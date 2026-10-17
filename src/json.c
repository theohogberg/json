/*
 * json - format JSON from stdin, optionally selecting a value
 *
 *   curl -s URL | json
 *   curl -s URL | json user.name
 *
 * No filter: validate and reformat in one pass, no allocation, no tree.
 * Filter: parse into a tree, walk the path, deliver results to a sink.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>

#ifndef MAXDEPTH				/* override with -DMAXDEPTH=N */
#define MAXDEPTH 1024
#endif

/* Shared helpers on the hot path must not cost a call per token. */
#ifdef __GNUC__
#define NORETURN __attribute__((noreturn))
#define INLINE   inline __attribute__((always_inline))
#else
#define NORETURN
#define INLINE   inline
#endif

static int opt_raw;			/* -r: print strings without quotes */

static NORETURN void die(const char *fmt, ...)
{
	va_list ap;

	fputs("json: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n);
	if (!p)
		die("out of memory");
	return p;
}

static int isdig(int c) { return c >= '0' && c <= '9'; }

/* ---------- arena ----------
 * Bump allocator. Chunks are kept and reused, so arena_restore() is O(1):
 * everything allocated since the mark is reclaimed, which bounds memory per
 * stream element. Anything built while answering a query must come from here.
 */
#define ARENA_CHUNK (1u << 20)

typedef struct chunk {
	struct chunk *next;
	char *buf;
	size_t cap;
} chunk;

typedef struct {
	chunk *c;
	size_t off;
} amark;

static chunk *arena_head, *arena_cur;
static size_t arena_off;

static amark arena_save(void)
{
	amark m = { arena_cur, arena_off };
	return m;
}

static void arena_restore(amark m)
{
	arena_cur = m.c;
	arena_off = m.off;
}

static void *arena_alloc(size_t n)
{
	void *p;

	n = (n + 7) & ~(size_t)7;		/* pointers and size_t; nothing needs more */
	while (!arena_cur || arena_cur->cap - arena_off < n) {
		chunk *nx = arena_cur ? arena_cur->next : arena_head;

		if (!nx) {
			nx = xrealloc(NULL, sizeof *nx);
			nx->next = NULL;
			nx->cap = n > ARENA_CHUNK ? n : ARENA_CHUNK;
			nx->buf = xrealloc(NULL, nx->cap);
			if (arena_cur)
				arena_cur->next = nx;
			else
				arena_head = nx;
		}
		arena_cur = nx;
		arena_off = 0;
	}
	p = arena_cur->buf + arena_off;
	arena_off += n;
	return p;
}

/* ---------- value model ---------- */

typedef enum { J_NULL, J_TRUE, J_FALSE, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct {			/* decoded string, may contain NUL */
	char *p;
	size_t len;
} jstr;

typedef struct jval jval;
struct jval {
	jtype t;
	union {
		struct { const char *p; size_t len; } num;	/* raw lexeme */
		jstr str;
		struct { jstr *k; jval **v; size_t n, cap; } c;	/* k: objects only */
	} u;
};

static const char *typename(jtype t)
{
	static const char *const names[] = {
		"null", "boolean", "boolean", "number", "string", "array", "object"
	};
	return names[t];
}

static jval *newval(jtype t)
{
	jval *v = arena_alloc(sizeof *v);

	memset(v, 0, sizeof *v);
	v->t = t;
	return v;
}

/* Growth copies into a fresh block and abandons the old one. */
static void *arena_grow(void *old, size_t oldn, size_t newn, size_t esz)
{
	void *p = arena_alloc(newn * esz);

	if (oldn)
		memcpy(p, old, oldn * esz);
	return p;
}

/* Append to an array, or to an object when key is non-NULL. */
static void push(jval *c, const jstr *key, jval *item)
{
	if (c->u.c.n == c->u.c.cap) {
		size_t cap = c->u.c.cap ? c->u.c.cap * 2 : 8;

		c->u.c.v = arena_grow(c->u.c.v, c->u.c.n, cap, sizeof *c->u.c.v);
		if (key)
			c->u.c.k = arena_grow(c->u.c.k, c->u.c.n, cap, sizeof *c->u.c.k);
		c->u.c.cap = cap;
	}
	if (key)
		c->u.c.k[c->u.c.n] = *key;
	c->u.c.v[c->u.c.n++] = item;
}

/* ---------- scanners ----------
 * The two hot loops, and the only place intrinsics belong. Contract: read
 * only s[0..n), return the same answer as the scalar loop. A vector version
 * handles whole 16-byte blocks and falls through to the scalar loop for the
 * tail. Guard it so -DNOSIMD forces scalar; `make scalar` relies on that:
 *
 *   #if !defined(NOSIMD) && defined(__SSE2__)  ... #elif ... __ARM_NEON ...
 */

/*
 * First byte in s[0..n) needing attention inside a string literal: a quote,
 * a backslash, a control character, or a non-ASCII byte (UTF-8 validation).
 * Returns n if there is none.
 */
static size_t scan_str(const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\' || c < 0x20 || c >= 0x80)
			break;
	}
	return i;
}

/* Length of the whitespace run at s[0..n). */
static size_t scan_ws(const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
			break;
	return i;
}

/* ---------- lexer ---------- */

typedef struct {
	const char *s;
	size_t n, i;
	int depth;
} P;

static NORETURN void perr(P *p, const char *msg)
{
	die("parse error at byte %zu: %s", p->i, msg);
}

static int peek(P *p)
{
	return p->i < p->n ? (unsigned char)p->s[p->i] : -1;
}

static void skipws(P *p)
{
	p->i += scan_ws(p->s + p->i, p->n - p->i);
}

static void lit(P *p, const char *word)
{
	size_t l = strlen(word);

	if (p->n - p->i < l || memcmp(p->s + p->i, word, l) != 0)
		perr(p, "invalid literal");
	p->i += l;
}

/* Advance past one number, validating the grammar. The lexeme stays raw. */
static INLINE void scan_number(P *p)
{
	if (peek(p) == '-')
		p->i++;
	if (peek(p) == '0')
		p->i++;
	else if (isdig(peek(p)))
		while (isdig(peek(p))) p->i++;
	else
		perr(p, "invalid number");

	if (peek(p) == '.') {
		p->i++;
		if (!isdig(peek(p))) perr(p, "expected digit after '.'");
		while (isdig(peek(p))) p->i++;
	}
	if (peek(p) == 'e' || peek(p) == 'E') {
		p->i++;
		if (peek(p) == '+' || peek(p) == '-') p->i++;
		if (!isdig(peek(p))) perr(p, "expected digit in exponent");
		while (isdig(peek(p))) p->i++;
	}
}

static int hex4(const char *s, size_t n, unsigned *out)
{
	unsigned v = 0;
	size_t k;

	if (n < 4)
		return 0;
	for (k = 0; k < 4; k++) {
		int c = (unsigned char)s[k], d;

		if (isdig(c)) d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else return 0;
		v = v * 16 + (unsigned)d;
	}
	*out = v;
	return 1;
}

/*
 * Validate one UTF-8 sequence at s[0..n). Returns its length, or 0 if
 * malformed: bad continuation, overlong, surrogate half, or above U+10FFFF.
 * Invalid input is rejected so the tool never emits invalid JSON.
 */
static size_t utf8_check(const char *s, size_t n)
{
	unsigned char a = (unsigned char)s[0], b, c, d;

	if (a < 0x80)
		return 1;
	if (a < 0xC2 || a > 0xF4)
		return 0;
	b = n > 1 ? (unsigned char)s[1] : 0;
	if (b < 0x80 || b > 0xBF)
		return 0;
	if (a < 0xE0)
		return 2;
	if (a < 0xF0) {
		if (a == 0xE0 && b < 0xA0) return 0;	/* overlong */
		if (a == 0xED && b > 0x9F) return 0;	/* surrogate */
		c = n > 2 ? (unsigned char)s[2] : 0;
		return c >= 0x80 && c <= 0xBF ? 3 : 0;
	}
	if (a == 0xF0 && b < 0x90) return 0;		/* overlong */
	if (a == 0xF4 && b > 0x8F) return 0;		/* > U+10FFFF */
	c = n > 2 ? (unsigned char)s[2] : 0;
	d = n > 3 ? (unsigned char)s[3] : 0;
	return c >= 0x80 && c <= 0xBF && d >= 0x80 && d <= 0xBF ? 4 : 0;
}

static size_t utf8_put(char *d, unsigned cp)
{
	if (cp < 0x80) {
		d[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		d[0] = (char)(0xC0 | (cp >> 6));
		d[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		d[0] = (char)(0xE0 | (cp >> 12));
		d[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		d[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	d[0] = (char)(0xF0 | (cp >> 18));
	d[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	d[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	d[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

/* str_end's out-of-line half: everything but a clean run ending in '"'. */
static size_t str_slow(P *p, size_t end, int *esc)
{
	unsigned char c = end < p->n ? (unsigned char)p->s[end] : 0;
	size_t w;

	if (end >= p->n || (c == '\\' && end + 1 >= p->n))
		perr(p, "unterminated string");
	if (c == '\\') {
		*esc = 1;
		return end + 2;
	}
	w = c < 0x20 ? 0 : utf8_check(p->s + end, p->n - end);
	if (w == 0) {
		p->i = end;
		perr(p, c < 0x20 ? "control character in string"
				 : "invalid UTF-8 in string");
	}
	return end + w;
}

/*
 * Validate the string literal whose opening quote is at p->i. Returns the
 * index of the closing quote and sets *esc if any escape was seen. p->i is
 * left at the opening quote.
 */
static INLINE size_t str_end(P *p, int *esc)
{
	size_t end = p->i + 1;

	*esc = 0;
	for (;;) {
		end += scan_str(p->s + end, p->n - end);
		if (end < p->n && p->s[end] == '"')
			return end;
		end = str_slow(p, end, esc);
	}
}

/*
 * Decode the escape whose backslash precedes s[*j], advancing *j past it.
 * A \u surrogate pair becomes one code point; a lone surrogate, U+FFFD.
 */
static unsigned unescape(P *p, size_t *j)
{
	const char *s = p->s + *j;
	size_t n = p->n - *j;
	unsigned cp, lo;

	p->i = *j;
	++*j;
	switch (*s) {
	case '"': case '\\': case '/': return (unsigned char)*s;
	case 'b': return '\b';
	case 'f': return '\f';
	case 'n': return '\n';
	case 'r': return '\r';
	case 't': return '\t';
	case 'u': break;
	default: perr(p, "unknown escape");
	}
	if (!hex4(s + 1, n - 1, &cp))
		perr(p, "bad \\u escape");
	*j += 4;
	if (cp >= 0xD800 && cp <= 0xDBFF) {
		if (n > 6 && s[5] == '\\' && s[6] == 'u' && hex4(s + 7, n - 7, &lo) &&
		    lo >= 0xDC00 && lo <= 0xDFFF) {
			cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
			*j += 6;
		} else {
			cp = 0xFFFD;
		}
	} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
		cp = 0xFFFD;
	}
	return cp;
}

/* ---------- output buffer ----------
 * Batch into one buffer and hand it to the kernel in big writes.
 */
static char   obuf[1u << 16];
static size_t olen;

static void oflush(void)
{
	if (olen && fwrite(obuf, 1, olen, stdout) != olen)
		die("write error: %s", strerror(errno));
	olen = 0;
}

/* om's out-of-line half, so the common case inlines: the buffer is full. */
static void om_slow(const char *s, size_t n)
{
	oflush();
	if (n >= sizeof obuf) {
		if (fwrite(s, 1, n, stdout) != n)
			die("write error: %s", strerror(errno));
		return;
	}
	memcpy(obuf, s, n);
	olen = n;
}

static INLINE void om(const char *s, size_t n)
{
	if (olen + n > sizeof obuf) {
		om_slow(s, n);
		return;
	}
	memcpy(obuf + olen, s, n);
	olen += n;
}

static void oc(char c)
{
	if (olen == sizeof obuf)
		oflush();
	obuf[olen++] = c;
}

static void os_(const char *s) { om(s, strlen(s)); }

static void indent(int lvl)
{
	static char sp[64];
	int n = lvl * 2;

	if (!sp[0])
		memset(sp, ' ', sizeof sp);
	for (; n > (int)sizeof sp; n -= (int)sizeof sp)
		om(sp, sizeof sp);
	om(sp, (size_t)n);
}

/* One byte of string content, escaped as JSON requires. */
static void oesc(unsigned char c)
{
	static const char hex[] = "0123456789abcdef";

	switch (c) {
	case '"': case '\\': break;
	case '\b': c = 'b'; break;
	case '\f': c = 'f'; break;
	case '\n': c = 'n'; break;
	case '\r': c = 'r'; break;
	case '\t': c = 't'; break;
	default:
		if (c >= 0x20) { oc((char)c); return; }
		os_("\\u00");
		oc(hex[c >> 4]);
		oc(hex[c & 15]);
		return;
	}
	oc('\\');
	oc((char)c);
}

static void print_str(const char *s, size_t len)
{
	size_t i = 0, run;

	oc('"');
	while (i < len) {
		for (run = i; i < len; i++)
			if ((unsigned char)s[i] < 0x20 || s[i] == '"' || s[i] == '\\')
				break;
		om(s + run, i - run);
		if (i < len)
			oesc((unsigned char)s[i++]);
	}
	oc('"');
}

/* ---------- parser (tree) ---------- */

static jstr parse_string(P *p)
{
	int esc;
	size_t end, j, o = 0;
	jstr out;

	if (peek(p) != '"')
		perr(p, "expected string");
	end = str_end(p, &esc);
	j = p->i + 1;
	out.p = arena_alloc(end - j + 1);	/* decoded is never longer than raw */
	if (!esc) {
		memcpy(out.p, p->s + j, end - j);
		o = end - j;
	}
	while (esc && j < end) {
		if (p->s[j] != '\\') {
			out.p[o++] = p->s[j++];
		} else {
			j++;
			o += utf8_put(out.p + o, unescape(p, &j));
		}
	}
	out.p[o] = '\0';
	out.len = o;
	p->i = end + 1;
	return out;
}

static jval *parse_value(P *p);

/*
 * Children of every open container, innermost last. Each container copies
 * its own run into the arena at exact size when it closes, so the tree has
 * no growth slack and no abandoned blocks. Reused for the whole parse.
 */
static jstr  *stk_k;
static jval **stk_v;
static size_t stk_n, stk_cap;

/* Array or object, chosen by the opening byte. */
static jval *parse_container(P *p)
{
	char open = p->s[p->i], close = open == '[' ? ']' : '}';
	jval *v = newval(open == '[' ? J_ARR : J_OBJ);
	size_t base = stk_n, n;

	p->i++;
	skipws(p);
	if (peek(p) == close) { p->i++; return v; }
	for (;;) {
		jstr k = { NULL, 0 };
		jval *item;

		if (open == '{') {
			k = parse_string(p);
			skipws(p);
			if (peek(p) != ':')
				perr(p, "expected ':'");
			p->i++;
		}
		item = parse_value(p);
		if (stk_n == stk_cap) {
			stk_cap = stk_cap ? stk_cap * 2 : 256;
			stk_k = xrealloc(stk_k, stk_cap * sizeof *stk_k);
			stk_v = xrealloc(stk_v, stk_cap * sizeof *stk_v);
		}
		stk_k[stk_n] = k;
		stk_v[stk_n++] = item;
		skipws(p);
		if (peek(p) == ',') { p->i++; skipws(p); continue; }
		if (peek(p) == close) { p->i++; break; }
		perr(p, close == ']' ? "expected ',' or ']'" : "expected ',' or '}'");
	}
	n = stk_n - base;
	stk_n = base;
	v->u.c.v = arena_grow(stk_v + base, n, n, sizeof *stk_v);
	if (open == '{')
		v->u.c.k = arena_grow(stk_k + base, n, n, sizeof *stk_k);
	v->u.c.n = v->u.c.cap = n;
	return v;
}

static jval *parse_value(P *p)
{
	jval *v;
	size_t start;

	if (++p->depth > MAXDEPTH)
		perr(p, "nesting too deep");
	skipws(p);
	switch (peek(p)) {
	case '{': case '[': v = parse_container(p); break;
	case '"': v = newval(J_STR); v->u.str = parse_string(p); break;
	case 't': lit(p, "true");  v = newval(J_TRUE);  break;
	case 'f': lit(p, "false"); v = newval(J_FALSE); break;
	case 'n': lit(p, "null");  v = newval(J_NULL);  break;
	case -1:  perr(p, "unexpected end of input");
	default:
		start = p->i;
		if (peek(p) != '-' && !isdig(peek(p)))
			perr(p, "unexpected byte, input is not JSON");
		scan_number(p);
		v = newval(J_NUM);
		v->u.num.p = p->s + start;		/* raw text: exact round-trip */
		v->u.num.len = p->i - start;
	}
	p->depth--;
	return v;
}

/* ---------- printer (tree) ---------- */

static void print_val(const jval *v, int lvl)
{
	size_t i, n;

	switch (v->t) {
	case J_NULL:  os_("null");  return;
	case J_TRUE:  os_("true");  return;
	case J_FALSE: os_("false"); return;
	case J_NUM:   om(v->u.num.p, v->u.num.len); return;
	case J_STR:   print_str(v->u.str.p, v->u.str.len); return;
	case J_ARR:
	case J_OBJ:   break;
	}
	n = v->u.c.n;
	oc(v->t == J_ARR ? '[' : '{');
	for (i = 0; i < n; i++) {
		if (i)
			oc(',');
		oc('\n');
		indent(lvl + 1);
		if (v->t == J_OBJ) {
			print_str(v->u.c.k[i].p, v->u.c.k[i].len);
			os_(": ");
		}
		print_val(v->u.c.v[i], lvl + 1);
	}
	if (n) {
		oc('\n');
		indent(lvl);
	}
	oc(v->t == J_ARR ? ']' : '}');
}

/* ---------- streaming reformatter ----------
 * Validate and emit in one pass, allocating nothing. Output must match
 * print_val byte for byte, so escapes are decoded and re-escaped alike.
 */
static void emit_value(P *p, int lvl);

static void emit_string(P *p)
{
	int esc;
	size_t end, j;

	if (peek(p) != '"')
		perr(p, "expected string");
	end = str_end(p, &esc);
	j = p->i + 1;
	oc('"');
	if (!esc)
		om(p->s + j, end - j);
	while (esc) {				/* clean runs copy straight through */
		const char *b = memchr(p->s + j, '\\', end - j);
		size_t run = b ? (size_t)(b - (p->s + j)) : end - j;
		unsigned cp;

		om(p->s + j, run);
		j += run;
		if (!b)
			break;
		j++;
		cp = unescape(p, &j);
		if (cp < 0x80) {
			oesc((unsigned char)cp);
		} else {
			char u[4];

			om(u, utf8_put(u, cp));
		}
	}
	oc('"');
	p->i = end + 1;
}

/* Array or object, chosen by the opening byte. */
static void emit_container(P *p, int lvl)
{
	char open = p->s[p->i], close = open == '[' ? ']' : '}';
	int first = 1;

	p->i++;
	skipws(p);
	oc(open);
	if (peek(p) == close) {
		p->i++;
		oc(close);
		return;
	}
	for (;;) {
		if (!first)
			oc(',');
		oc('\n');
		first = 0;
		indent(lvl + 1);
		if (open == '{') {
			emit_string(p);
			skipws(p);
			if (peek(p) != ':')
				perr(p, "expected ':'");
			p->i++;
			os_(": ");
		}
		emit_value(p, lvl + 1);
		skipws(p);
		if (peek(p) == ',') { p->i++; skipws(p); continue; }
		if (peek(p) == close) { p->i++; break; }
		perr(p, close == ']' ? "expected ',' or ']'" : "expected ',' or '}'");
	}
	oc('\n');
	indent(lvl);
	oc(close);
}

static void emit_value(P *p, int lvl)
{
	size_t start;

	if (++p->depth > MAXDEPTH)
		perr(p, "nesting too deep");
	skipws(p);
	switch (peek(p)) {
	case '{': case '[': emit_container(p, lvl); break;
	case '"': emit_string(p); break;
	case 't': lit(p, "true");  os_("true");  break;
	case 'f': lit(p, "false"); os_("false"); break;
	case 'n': lit(p, "null");  os_("null");  break;
	case -1:  perr(p, "unexpected end of input");
	default:
		start = p->i;
		if (peek(p) != '-' && !isdig(peek(p)))
			perr(p, "unexpected byte, input is not JSON");
		scan_number(p);
		om(p->s + start, p->i - start);	/* the lexeme as written */
	}
	p->depth--;
}

/* ---------- result sink ----------
 * A filter yields a stream, not a value: [@] makes the count a runtime
 * property, so results are delivered through a callback.
 */
typedef void (*sink_fn)(const jval *v, void *ctx);

/* Prints each value on its own line. ctx is a count of values printed. */
static void print_sink(const jval *v, void *ctx)
{
	int *n = ctx;

	if ((*n)++)
		oc('\n');
	/* -r applies only when the result itself is a string; nested strings
	 * stay quoted so the output remains valid JSON. */
	if (opt_raw && v->t == J_STR)
		om(v->u.str.p, v->u.str.len);
	else
		print_val(v, 0);
}

/* ---------- path selection ---------- */

/*
 * Array slice, Python semantics: negative indices count from the end,
 * out-of-range bounds clamp, a zero step is an error. The result is one
 * sub-array, not a stream.
 */
typedef struct {
	long start, end, step;
	int has_start, has_end, has_step;
} jslice;

/* Parse an optional signed integer. Returns 1 if one was present. */
static int slice_num(const char **pp, long *out)
{
	const char *s = *pp;
	long v = 0;
	int neg = *s == '-';

	s += neg;
	if (!isdig((unsigned char)*s))
		return 0;
	for (; isdig((unsigned char)*s); s++) {
		long d = *s - '0';

		if (v > (LONG_MAX - d) / 10)
			die("number too large in path");
		v = v * 10 + d;
	}
	*pp = s;
	*out = neg ? -v : v;
	return 1;
}

static jval *slice_array(const jval *a, const jslice *sl)
{
	long n = (long)a->u.c.n;
	long step = sl->has_step ? sl->step : 1;
	long lo = step > 0 ? 0 : -1, hi = step > 0 ? n : n - 1;	/* valid range */
	long start, end, cnt, k;
	jval *out = newval(J_ARR);

	if (step == 0)
		die("slice step cannot be zero");

	/* defaults differ by direction: a[::-1] runs from the end down */
	start = sl->has_start ? sl->start : (step > 0 ? 0 : n - 1);
	end   = sl->has_end   ? sl->end   : (step > 0 ? n : -1);
	if (sl->has_start && start < 0) start += n;
	if (sl->has_end   && end   < 0) end   += n;
	start = start < lo ? lo : start > hi ? hi : start;
	end   = end   < lo ? lo : end   > hi ? hi : end;

	/* count first: stepping i past end could overflow for a huge step */
	if (step > 0)
		cnt = start < end ? (end - start - 1) / step + 1 : 0;
	else
		cnt = start > end ? (start - end - 1) / -step + 1 : 0;
	out->u.c.v = arena_alloc((size_t)cnt * sizeof *out->u.c.v);
	for (k = 0; k < cnt; k++)
		out->u.c.v[k] = a->u.c.v[start + k * step];
	out->u.c.n = out->u.c.cap = (size_t)cnt;
	return out;
}

/* Apply a bracket component. *pp points just past '['; leaves it past ']'. */
static const jval *apply_bracket(const jval *v, const char **pp, const char *path)
{
	const char *b = *pp;
	jslice sl;
	int is_slice = 0;
	long idx;

	memset(&sl, 0, sizeof sl);
	sl.has_start = slice_num(&b, &sl.start);
	if (*b == ':') {
		is_slice = 1;
		b++;
		sl.has_end = slice_num(&b, &sl.end);
		if (*b == ':') {
			b++;
			sl.has_step = slice_num(&b, &sl.step);
		}
	}
	if (*b != ']')
		die("malformed bracket in path '%s'", path);
	if (!is_slice && !sl.has_start)
		die("empty bracket in path '%s'", path);
	*pp = b + 1;

	if (v->t != J_ARR)
		die("cannot index %s with a bracket", typename(v->t));
	if (is_slice)
		return slice_array(v, &sl);

	idx = sl.start < 0 ? sl.start + (long)v->u.c.n : sl.start;
	if (idx < 0 || idx >= (long)v->u.c.n)
		die("index %ld out of range (total: %zu)", sl.start, v->u.c.n);
	return v->u.c.v[idx];
}

/*
 * '+' is the glob wildcard, not '*', so a pattern survives the shell
 * unquoted. Iterative backtracking, so many wildcards cannot blow the stack.
 */
static int glob_match(const char *pat, size_t pn, const char *str, size_t sn)
{
	size_t p = 0, s = 0, star = (size_t)-1, mark = 0;

	while (s < sn) {
		if (p < pn && pat[p] == '+') {
			star = p++;
			mark = s;
		} else if (p < pn && pat[p] == str[s]) {
			p++;
			s++;
		} else if (star != (size_t)-1) {
			p = star + 1;		/* let the last '+' eat one more */
			s = ++mark;
		} else {
			return 0;
		}
	}
	while (p < pn && pat[p] == '+')
		p++;
	return p == pn;
}

/*
 * Apply one key component. A named key that is absent is an error; a glob
 * that matches nothing is an empty object.
 */
static const jval *apply_key(const jval *v, const char *q, size_t klen)
{
	int glob = memchr(q, '+', klen) != NULL;
	size_t i, idx = 0;

	if (v->t == J_OBJ && glob) {
		jval *out = newval(J_OBJ);

		for (i = 0; i < v->u.c.n; i++)
			if (glob_match(q, klen, v->u.c.k[i].p, v->u.c.k[i].len))
				push(out, &v->u.c.k[i], v->u.c.v[i]);
		return out;
	}
	if (v->t == J_OBJ) {
		for (i = 0; i < v->u.c.n; i++)
			if (v->u.c.k[i].len == klen &&
			    memcmp(v->u.c.k[i].p, q, klen) == 0)
				return v->u.c.v[i];
		die("no such key: %.*s", (int)klen, q);
	}
	if (glob)
		die("cannot glob %s with '%.*s': globbing matches object keys",
		    typename(v->t), (int)klen, q);
	if (v->t != J_ARR)
		die("cannot index %s with '%.*s'", typename(v->t), (int)klen, q);

	for (i = 0; i < klen; i++) {
		size_t d;

		if (!isdig((unsigned char)q[i]))
			die("cannot index array with '%.*s'", (int)klen, q);
		d = (size_t)(q[i] - '0');
		if (idx > (SIZE_MAX - d) / 10)
			die("index '%.*s' is too large", (int)klen, q);
		idx = idx * 10 + d;
	}
	if (idx >= v->u.c.n)
		die("index %zu out of range (total: %zu)", idx, v->u.c.n);
	return v->u.c.v[idx];
}

/* Find the ']' closing the bracket at s; nested brackets are counted. */
static const char *bracket_end(const char *s, const char *path)
{
	int depth = 0;

	for (; *s; s++) {
		if (*s == '[')
			depth++;
		else if (*s == ']' && --depth == 0)
			return s;
	}
	die("unclosed '[' in path '%s'", path);
}

static void walk(const jval *v, const char *q, const char *path,
		 sink_fn sink, void *ctx, int depth);

/*
 * [@...] turns an array into a stream: the text inside the brackets plus
 * whatever follows is the path applied to each element, so a[@.x] and
 * a[@].x are the same path. Each element's allocations are reclaimed
 * before the next.
 */
static void stream_array(const jval *v, const char *inner, size_t ilen,
			 const char *rest, const char *path,
			 sink_fn sink, void *ctx, int depth)
{
	size_t rlen = strlen(rest), i;
	char *sub;
	amark m;

	if (v->t != J_ARR)
		die("cannot stream %s with [@]: it needs an array", typename(v->t));

	sub = arena_alloc(ilen + rlen + 1);
	memcpy(sub, inner, ilen);
	memcpy(sub + ilen, rest, rlen + 1);
	/* a lone '.' is identity at top level, but a dangling one here */
	if (ilen + rlen > 0 && sub[ilen + rlen - 1] == '.')
		die("empty component in path '%s'", path);

	m = arena_save();			/* after sub, which must survive */
	for (i = 0; i < v->u.c.n; i++) {
		walk(v->u.c.v[i], sub, path, sink, ctx, depth + 1);
		arena_restore(m);
	}
}

/* Walk a path from q, delivering every result to the sink. */
static void walk(const jval *v, const char *q, const char *path,
		 sink_fn sink, void *ctx, int depth)
{
	if (depth > MAXDEPTH)
		die("path nesting too deep");

	if (*q == '.')				/* allow both a.b and .a.b */
		q++;
	while (*q) {
		if (*q == '[' && q[1] == '@') {
			const char *end = bracket_end(q, path);

			stream_array(v, q + 2, (size_t)(end - (q + 2)),
				     end + 1, path, sink, ctx, depth);
			return;
		}
		if (*q == '[') {
			q++;
			v = apply_bracket(v, &q, path);
		} else {
			size_t klen = strcspn(q, ".[");

			if (klen == 0)
				die("empty component in path '%s'", path);
			v = apply_key(v, q, klen);
			q += klen;
		}
		if (*q == '.' && (*++q == '\0' || *q == '.'))
			die("empty component in path '%s'", path);
	}
	sink(v, ctx);
}

/* ---------- main ---------- */

static char *slurp(FILE *f, size_t *outlen)
{
	size_t cap = 1 << 16, len = 0, n;
	char *buf = xrealloc(NULL, cap);

	while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
		len += n;
		if (len + 1 == cap)
			buf = xrealloc(buf, cap *= 2);
	}
	if (ferror(f))
		die("read: %s", strerror(errno));
	buf[len] = '\0';
	*outlen = len;
	return buf;
}

static void usage(void)
{
	puts("usage: json [options] [path]\n"
"\n"
"Reads JSON on stdin and prints it formatted, or the selected value.\n"
"\n"
"  a.b.0  items[2]  items[-1]  items[1:3]   keys, indexes, Python slices\n"
"  na+                                      '+' globs object keys\n"
"  items[@.name]                            [@] streams an array's elements\n"
"\n"
"  -r, --raw-output   print a string result without quotes or escapes\n"
"      --raw          alias for --raw-output\n"
"      --rfc9535      RFC 9535 semantics (not implemented)\n"
"  -h, --help         this message\n"
"  --                 end of options");
}

int main(int argc, char **argv)
{
	const char *filter = NULL;
	int i, endopts = 0, rfc = 0, nout = 0;
	jval *root = NULL;
	size_t len;
	char *buf;
	P p;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (endopts || a[0] != '-' || a[1] == '\0') {
			if (filter)
				die("only one path may be given");
			filter = a;
		} else if (a[1] != '-') {		/* short cluster: -rh */
			for (a++; *a; a++) {
				if (*a == 'r')
					opt_raw = 1;
				else if (*a == 'h') {
					usage();
					return 0;
				}
				else
					die("unknown option '-%c' (try --help)", *a);
			}
		} else if (strcmp(a, "--") == 0) {
			endopts = 1;
		} else if (strcmp(a, "--help") == 0) {
			usage();
			return 0;
		} else if (strcmp(a, "--raw") == 0 || strcmp(a, "--raw-output") == 0) {
			opt_raw = 1;
		} else if (strcmp(a, "--rfc9535") == 0) {
			rfc = 1;
		} else {
			die("unknown option '%s' (try --help)", a);
		}
	}
	if (rfc)
		die("--rfc9535 is not implemented yet: slices would yield one result\n"
		    "per element instead of a sub-array, and a missing key would\n"
		    "yield empty instead of an error");

	buf = slurp(stdin, &len);
	p.s = buf; p.n = len; p.i = 0; p.depth = 0;
	skipws(&p);
	if (p.i == p.n)
		die("empty input");

	/* No filter streams, except -r on a top-level string, which must be
	 * decoded before it can be printed bare. */
	if (filter || (opt_raw && peek(&p) == '"'))
		root = parse_value(&p);
	else
		emit_value(&p, 0);
	skipws(&p);
	if (p.i != p.n)
		perr(&p, "trailing data after JSON value");

	if (root)
		walk(root, filter ? filter : "", filter ? filter : "", print_sink, &nout, 0);
	oc('\n');
	oflush();
	return 0;
}
