/*
 * json - format JSON from stdin, optionally selecting a value
 *
 *   curl -s URL | json
 *   curl -s URL | json user.name
 *   curl -s URL | json items.0.id
 *
 * Two execution paths. With no filter the document is validated and
 * reformatted in a single pass with no allocation at all, because the
 * tree would be built, printed once and thrown away. With a filter the
 * document is parsed into a tree, since selecting needs random access.
 *
 * Results reach the output through a sink callback rather than a return
 * value, so a filter can yield any number of values.
 *
 * build: make
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

static const char *progname = "json";

static int opt_raw;			/* -r: print strings without quotes */
static int opt_rfc;			/* --rfc9535: full JSONPath semantics */

static void die(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", progname);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n);
	if (!p)
		die("out of memory");
	return p;
}

static void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n);
	if (!q)
		die("out of memory");
	return q;
}


/* ---------- arena ----------
 * Values and decoded strings are bump-allocated out of large chunks
 * rather than malloc'd one at a time.
 *
 * Chunks are kept and reused rather than freed, so arena_restore() is
 * O(1) and costs no allocator traffic. That is what makes per-element
 * processing possible: parse one element, use it, restore the mark, and
 * the next element reuses the same memory instead of growing a tree for
 * the whole document.
 */
#define ARENA_CHUNK (1u << 20)

static char  **arena_chunk;			/* every chunk ever allocated */
static size_t *arena_size;			/* its capacity */
static size_t  arena_n, arena_cap;		/* chunks in use / array capacity */
static size_t  arena_cur, arena_off;		/* current chunk and bump offset */

typedef struct {
	size_t chunk, off;
} amark;

static amark arena_save(void)
{
	amark m;

	m.chunk = arena_cur;
	m.off = arena_off;
	return m;
}

/*
 * Everything allocated since the mark becomes free for reuse. Any value
 * that must outlive the mark has to be copied out first -- there is no
 * way to detect a dangling reference, so keep the lifetime obvious.
 */
static void arena_restore(amark m)
{
	arena_cur = m.chunk;
	arena_off = m.off;
}

static void arena_push(size_t want)
{
	size_t sz = want > ARENA_CHUNK ? want : ARENA_CHUNK;

	if (arena_n == arena_cap) {
		arena_cap = arena_cap ? arena_cap * 2 : 8;
		arena_chunk = xrealloc(arena_chunk, arena_cap * sizeof *arena_chunk);
		arena_size  = xrealloc(arena_size,  arena_cap * sizeof *arena_size);
	}
	arena_chunk[arena_n] = xmalloc(sz);
	arena_size[arena_n] = sz;
	arena_cur = arena_n;
	arena_off = 0;
	arena_n++;
}

static void *arena_alloc(size_t n)
{
	void *p;

	n = (n + 15) & ~(size_t)15;		/* keep 16-byte alignment */
	for (;;) {
		if (arena_n && arena_size[arena_cur] - arena_off >= n)
			break;
		if (arena_cur + 1 < arena_n) {	/* reuse an existing chunk */
			arena_cur++;
			arena_off = 0;
			continue;
		}
		arena_push(n);
	}
	p = arena_chunk[arena_cur] + arena_off;
	arena_off += n;
	return p;
}

static int isdig(int c) { return c >= '0' && c <= '9'; }

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
		struct { jval **v; size_t n, cap; } arr;
		struct { jstr *k; jval **v; size_t n, cap; } obj;
	} u;
};

static const char *typename(jtype t)
{
	switch (t) {
	case J_NULL:  return "null";
	case J_TRUE:
	case J_FALSE: return "boolean";
	case J_NUM:   return "number";
	case J_STR:   return "string";
	case J_ARR:   return "array";
	case J_OBJ:   return "object";
	}
	return "?";
}

/*
 * Values built while answering a query -- slice results, glob results --
 * grow out of the arena rather than the heap, so a per-element reset
 * reclaims them. Growth copies into a fresh block and abandons the old
 * one; the arena makes that cheap and the reset makes it bounded.
 */
static void *arena_grow(void *old, size_t oldn, size_t newn, size_t esz)
{
	void *p = arena_alloc(newn * esz);

	if (oldn)
		memcpy(p, old, oldn * esz);
	return p;
}

static void arr_push(jval *a, jval *item)
{
	if (a->u.arr.n == a->u.arr.cap) {
		size_t cap = a->u.arr.cap ? a->u.arr.cap * 2 : 8;

		a->u.arr.v = arena_grow(a->u.arr.v, a->u.arr.n, cap,
					sizeof *a->u.arr.v);
		a->u.arr.cap = cap;
	}
	a->u.arr.v[a->u.arr.n++] = item;
}

static void obj_push(jval *o, jstr key, jval *item)
{
	if (o->u.obj.n == o->u.obj.cap) {
		size_t cap = o->u.obj.cap ? o->u.obj.cap * 2 : 8;

		o->u.obj.k = arena_grow(o->u.obj.k, o->u.obj.n, cap,
					sizeof *o->u.obj.k);
		o->u.obj.v = arena_grow(o->u.obj.v, o->u.obj.n, cap,
					sizeof *o->u.obj.v);
		o->u.obj.cap = cap;
	}
	o->u.obj.k[o->u.obj.n] = key;
	o->u.obj.v[o->u.obj.n] = item;
	o->u.obj.n++;
}

/* ---------- parser ---------- */

typedef struct {
	const char *s;
	size_t n, i;
	int depth;
} P;

static void perr(P *p, const char *msg)
{
	die("parse error at byte %zu: %s", p->i, msg);
}

static int peek(P *p)
{
	return p->i < p->n ? (unsigned char)p->s[p->i] : -1;
}


#if (defined(__SSE2__) || defined(_M_X64)) && !defined(NOSIMD)
#include <emmintrin.h>
#define HAVE_SIMD 1
#endif

/*
 * First byte in s[0..n) that needs attention inside a string literal:
 * a closing quote, a backslash, a control character, or a non-ASCII byte
 * that must go through UTF-8 validation. Returns n if there is none.
 * The SSE2 path checks 16 bytes per iteration; the scalar loop below is
 * both the tail and the fallback on non-x86.
 */
static size_t scan_str(const char *s, size_t n)
{
	size_t i = 0;
#ifdef HAVE_SIMD
	const __m128i q  = _mm_set1_epi8('"');
	const __m128i bs = _mm_set1_epi8('\\');
	const __m128i ctl = _mm_set1_epi8(0x1F);

	for (; i + 16 <= n; i += 16) {
		__m128i v = _mm_loadu_si128((const __m128i *)(s + i));
		__m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, q),
					 _mm_cmpeq_epi8(v, bs));
		int bits;

		/* unsigned v <= 0x1F, via min: SSE2 has no unsigned compare */
		m = _mm_or_si128(m, _mm_cmpeq_epi8(_mm_min_epu8(v, ctl), v));
		/* v >= 0x80 is just the sign bit, so OR v in and take movemask */
		bits = _mm_movemask_epi8(_mm_or_si128(m, v));
		if (bits)
			return i + (size_t)__builtin_ctz((unsigned)bits);
	}
#endif
	for (; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\' || c < 0x20 || c >= 0x80)
			return i;
	}
	return n;
}

/* Length of the whitespace run at s[0..n). */
static size_t scan_ws(const char *s, size_t n)
{
	size_t i = 0;
#ifdef HAVE_SIMD
	const __m128i sp = _mm_set1_epi8(' ');
	const __m128i tb = _mm_set1_epi8('\t');
	const __m128i nl = _mm_set1_epi8('\n');
	const __m128i cr = _mm_set1_epi8('\r');

	for (; i + 16 <= n; i += 16) {
		__m128i v = _mm_loadu_si128((const __m128i *)(s + i));
		__m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, sp),
					 _mm_cmpeq_epi8(v, tb));
		int bits;

		m = _mm_or_si128(m, _mm_cmpeq_epi8(v, nl));
		m = _mm_or_si128(m, _mm_cmpeq_epi8(v, cr));
		bits = _mm_movemask_epi8(m);
		if (bits != 0xFFFF)
			return i + (size_t)__builtin_ctz(~(unsigned)bits);
	}
#endif
	for (; i < n; i++) {
		char c = s[i];
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
			break;
	}
	return i;
}

static void skipws(P *p)
{
	p->i += scan_ws(p->s + p->i, p->n - p->i);
}

static jval *newval(jtype t)
{
	jval *v = arena_alloc(sizeof *v);
	memset(v, 0, sizeof *v);
	v->t = t;
	return v;
}

static int hex4(P *p, size_t at, unsigned *out)
{
	unsigned v = 0;
	int k;

	for (k = 0; k < 4; k++) {
		int c, d;
		if (at + (size_t)k >= p->n)
			return 0;
		c = (unsigned char)p->s[at + k];
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else return 0;
		v = v * 16 + (unsigned)d;
	}
	*out = v;
	return 1;
}

/*
 * Validate one UTF-8 sequence at s[0..n). Returns its length, or 0 if the
 * sequence is malformed: bad continuation byte, overlong encoding,
 * surrogate half, or a code point above U+10FFFF. The JSON spec requires
 * text to be UTF-8, so anything else is rejected rather than passed
 * through -- otherwise this tool would emit invalid JSON.
 */
static size_t utf8_check(const char *s, size_t n)
{
	unsigned char a = (unsigned char)s[0], b, c, d;

	if (a < 0x80)
		return 1;
	if (a < 0xC2 || a > 0xF4)		/* C0/C1 overlong, >F4 too big */
		return 0;

	b = n > 1 ? (unsigned char)s[1] : 0;
	if (b < 0x80 || b > 0xBF)
		return 0;

	if (a < 0xE0)
		return 2;

	if (a < 0xF0) {				/* 3 bytes */
		if (a == 0xE0 && b < 0xA0) return 0;	/* overlong */
		if (a == 0xED && b > 0x9F) return 0;	/* surrogate */
		c = n > 2 ? (unsigned char)s[2] : 0;
		if (c < 0x80 || c > 0xBF) return 0;
		return 3;
	}

	if (a == 0xF0 && b < 0x90) return 0;		/* overlong */
	if (a == 0xF4 && b > 0x8F) return 0;		/* > U+10FFFF */
	c = n > 2 ? (unsigned char)s[2] : 0;
	d = n > 3 ? (unsigned char)s[3] : 0;
	if (c < 0x80 || c > 0xBF) return 0;
	if (d < 0x80 || d > 0xBF) return 0;
	return 4;
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

/* decoded length is never longer than the raw span, so one alloc is enough */
static jstr parse_string(P *p)
{
	size_t start, end, j, o;
	int esc;
	jstr out;

	if (peek(p) != '"')
		perr(p, "expected string");
	p->i++;
	start = p->i;

	esc = 0;
	for (end = start; end < p->n; ) {
		unsigned char c;

		end += scan_str(p->s + end, p->n - end);
		if (end >= p->n)
			break;
		c = (unsigned char)p->s[end];
		if (c == '"')
			break;
		if (c == '\\') { esc = 1; end += 2; continue; }
		if (c < 0x20) { p->i = end; perr(p, "control character in string"); }
		{
			size_t w = utf8_check(p->s + end, p->n - end);
			if (w == 0) { p->i = end; perr(p, "invalid UTF-8 in string"); }
			end += w;
		}
	}
	if (end >= p->n) { p->i = start; perr(p, "unterminated string"); }

	out.p = arena_alloc(end - start + 1);
	if (!esc) {				/* common case: nothing to decode */
		memcpy(out.p, p->s + start, end - start);
		out.p[end - start] = '\0';
		out.len = end - start;
		p->i = end + 1;
		return out;
	}
	o = 0;
	for (j = start; j < end; ) {
		unsigned char c = (unsigned char)p->s[j];
		if (c != '\\') { out.p[o++] = (char)c; j++; continue; }
		if (++j >= end) { p->i = j; perr(p, "truncated escape"); }
		switch (p->s[j]) {
		case '"':  out.p[o++] = '"';  j++; break;
		case '\\': out.p[o++] = '\\'; j++; break;
		case '/':  out.p[o++] = '/';  j++; break;
		case 'b':  out.p[o++] = '\b'; j++; break;
		case 'f':  out.p[o++] = '\f'; j++; break;
		case 'n':  out.p[o++] = '\n'; j++; break;
		case 'r':  out.p[o++] = '\r'; j++; break;
		case 't':  out.p[o++] = '\t'; j++; break;
		case 'u': {
			unsigned cp, lo;
			if (!hex4(p, j + 1, &cp)) { p->i = j; perr(p, "bad \\u escape"); }
			j += 5;
			if (cp >= 0xD800 && cp <= 0xDBFF) {	/* high surrogate */
				if (j + 1 < end && p->s[j] == '\\' && p->s[j + 1] == 'u' &&
				    hex4(p, j + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					j += 6;
				} else {
					cp = 0xFFFD;
				}
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				cp = 0xFFFD;			/* stray low surrogate */
			}
			o += utf8_put(out.p + o, cp);
			break;
		}
		default:
			p->i = j;
			perr(p, "unknown escape");
		}
	}
	out.p[o] = '\0';
	out.len = o;
	p->i = end + 1;
	return out;
}

static jval *parse_number(P *p)
{
	size_t start = p->i;
	jval *v;

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

	v = newval(J_NUM);
	v->u.num.p = p->s + start;		/* raw text: exact round-trip */
	v->u.num.len = p->i - start;
	return v;
}

static void lit(P *p, const char *word)
{
	size_t l = strlen(word);
	if (p->n - p->i < l || memcmp(p->s + p->i, word, l) != 0)
		perr(p, "invalid literal");
	p->i += l;
}

static jval *parse_value(P *p);

static jval *parse_array(P *p)
{
	jval *v = newval(J_ARR);

	p->i++;					/* '[' */
	skipws(p);
	if (peek(p) == ']') { p->i++; return v; }
	for (;;) {
		if (v->u.arr.n == v->u.arr.cap) {
			v->u.arr.cap = v->u.arr.cap ? v->u.arr.cap * 2 : 8;
			v->u.arr.v = xrealloc(v->u.arr.v, v->u.arr.cap * sizeof *v->u.arr.v);
		}
		v->u.arr.v[v->u.arr.n++] = parse_value(p);
		skipws(p);
		if (peek(p) == ',') { p->i++; skipws(p); continue; }
		if (peek(p) == ']') { p->i++; return v; }
		perr(p, "expected ',' or ']'");
	}
}

static jval *parse_object(P *p)
{
	jval *v = newval(J_OBJ);

	p->i++;					/* '{' */
	skipws(p);
	if (peek(p) == '}') { p->i++; return v; }
	for (;;) {
		jstr k;
		if (v->u.obj.n == v->u.obj.cap) {
			v->u.obj.cap = v->u.obj.cap ? v->u.obj.cap * 2 : 8;
			v->u.obj.k = xrealloc(v->u.obj.k, v->u.obj.cap * sizeof *v->u.obj.k);
			v->u.obj.v = xrealloc(v->u.obj.v, v->u.obj.cap * sizeof *v->u.obj.v);
		}
		k = parse_string(p);
		skipws(p);
		if (peek(p) != ':')
			perr(p, "expected ':'");
		p->i++;
		skipws(p);
		v->u.obj.k[v->u.obj.n] = k;
		v->u.obj.v[v->u.obj.n] = parse_value(p);
		v->u.obj.n++;
		skipws(p);
		if (peek(p) == ',') { p->i++; skipws(p); continue; }
		if (peek(p) == '}') { p->i++; return v; }
		perr(p, "expected ',' or '}'");
	}
}

static jval *parse_value(P *p)
{
	jval *v;

	if (++p->depth > MAXDEPTH)
		perr(p, "nesting too deep");
	skipws(p);
	switch (peek(p)) {
	case '{': v = parse_object(p); break;
	case '[': v = parse_array(p); break;
	case '"': v = newval(J_STR); v->u.str = parse_string(p); break;
	case 't': lit(p, "true");  v = newval(J_TRUE);  break;
	case 'f': lit(p, "false"); v = newval(J_FALSE); break;
	case 'n': lit(p, "null");  v = newval(J_NULL);  break;
	case -1:  perr(p, "unexpected end of input"); return NULL;
	default:
		if (peek(p) == '-' || isdig(peek(p))) {
			v = parse_number(p);
			break;
		}
		{
			int c = peek(p);
			if (c >= 0x20 && c < 0x7f)
				die("parse error at byte %zu: unexpected '%c', "
				    "input is not JSON", p->i, c);
			die("parse error at byte %zu: unexpected byte 0x%02x, "
			    "input is not JSON", p->i, (unsigned)c);
		}
		return NULL;
	}
	p->depth--;
	return v;
}

/* ---------- printer ---------- */


/* ---------- output buffer ----------
 * stdio costs a call (and a lock) per token. Batch into one buffer and
 * hand it to the kernel in big writes.
 */
static char   obuf[1u << 16];
static size_t olen;

static void oflush(void)
{
	if (olen && fwrite(obuf, 1, olen, stdout) != olen)
		die("write error: %s", strerror(errno));
	olen = 0;
}

static void om(const char *s, size_t n)
{
	if (n >= sizeof obuf) {
		oflush();
		if (fwrite(s, 1, n, stdout) != n)
			die("write error: %s", strerror(errno));
		return;
	}
	if (olen + n > sizeof obuf)
		oflush();
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

static void print_str(const jstr *s)
{
	size_t i;

	size_t run = 0;			/* pending bytes needing no escape */

	oc('"');
	for (i = 0; i < s->len; i++) {
		unsigned char c = (unsigned char)s->p[i];
		const char *esc;
		char ubuf[7];

		switch (c) {
		case '"':  esc = "\\\""; break;
		case '\\': esc = "\\\\"; break;
		case '\b': esc = "\\b";  break;
		case '\f': esc = "\\f";  break;
		case '\n': esc = "\\n";  break;
		case '\r': esc = "\\r";  break;
		case '\t': esc = "\\t";  break;
		default:
			if (c >= 0x20) { run++; continue; }
			sprintf(ubuf, "\\u%04x", c);
			esc = ubuf;
		}
		if (run) {
			om(s->p + i - run, run);
			run = 0;
		}
		os_(esc);
	}
	if (run)
		om(s->p + s->len - run, run);
	oc('"');
}

static void pad(int lvl)
{
	static char sp[64];
	int n = lvl * 2;

	if (sp[0] != ' ')
		memset(sp, ' ', sizeof sp);
	while (n > (int)sizeof sp) {
		om(sp, sizeof sp);
		n -= (int)sizeof sp;
	}
	if (n > 0)
		om(sp, (size_t)n);
}

static void print_val(const jval *v, int lvl)
{
	size_t i;

	switch (v->t) {
	case J_NULL:  os_("null");  break;
	case J_TRUE:  os_("true");  break;
	case J_FALSE: os_("false"); break;
	case J_NUM:   om(v->u.num.p, v->u.num.len); break;
	case J_STR:   print_str(&v->u.str); break;
	case J_ARR:
		if (v->u.arr.n == 0) { os_("[]"); break; }
		os_("[\n");
		for (i = 0; i < v->u.arr.n; i++) {
			pad(lvl + 1);
			print_val(v->u.arr.v[i], lvl + 1);
			os_(i + 1 < v->u.arr.n ? ",\n" : "\n");
		}
		pad(lvl);
		oc(']');
		break;
	case J_OBJ:
		if (v->u.obj.n == 0) { os_("{}"); break; }
		os_("{\n");
		for (i = 0; i < v->u.obj.n; i++) {
			pad(lvl + 1);
			print_str(&v->u.obj.k[i]);
			os_(": ");
			print_val(v->u.obj.v[i], lvl + 1);
			os_(i + 1 < v->u.obj.n ? ",\n" : "\n");
		}
		pad(lvl);
		oc('}');
		break;
	}
}


/* ---------- streaming reformatter ----------
 * When no path is given, the document is printed once and discarded, so
 * building a tree is wasted work: every value costs a node and every
 * string costs a copy, all to be freed at exit. These functions validate
 * and emit in a single pass with zero allocation. Strings are echoed as
 * they appeared in the input rather than decoded and re-escaped, so
 * \u0041 stays \u0041 instead of becoming A.
 */
static void emit_value(P *p, int lvl);

static void emit_indent(int lvl)
{
	static char sp[64];
	int n = lvl * 2;

	if (sp[0] != ' ')
		memset(sp, ' ', sizeof sp);
	while (n > (int)sizeof sp) { om(sp, sizeof sp); n -= (int)sizeof sp; }
	if (n > 0)
		om(sp, (size_t)n);
}

/*
 * Emit one decoded byte with the same escaping print_str would apply, so
 * streamed output matches tree output byte for byte.
 */
static void emit_esc_byte(unsigned char c)
{
	char b[8];

	switch (c) {
	case '"':  os_("\\\""); return;
	case '\\': os_("\\\\"); return;
	case '\b': os_("\\b");  return;
	case '\f': os_("\\f");  return;
	case '\n': os_("\\n");  return;
	case '\r': os_("\\r");  return;
	case '\t': os_("\\t");  return;
	default:
		if (c < 0x20) { sprintf(b, "\\u%04x", c); os_(b); return; }
		oc((char)c);
	}
}

/*
 * Validate the string at p->i and emit it. Unescaped runs go straight
 * from the input buffer to the output buffer; escapes are decoded and
 * re-emitted inline. Nothing is allocated.
 */
static void emit_string(P *p)
{
	size_t run;

	if (peek(p) != '"')
		perr(p, "expected string");
	p->i++;
	oc('"');
	run = p->i;
	for (;;) {
		unsigned char c;

		p->i += scan_str(p->s + p->i, p->n - p->i);
		if (p->i >= p->n) { p->i = run; perr(p, "unterminated string"); }
		c = (unsigned char)p->s[p->i];

		if (c == '"') {
			om(p->s + run, p->i - run);
			p->i++;
			oc('"');
			return;
		}
		if (c < 0x20)
			perr(p, "control character in string");
		if (c != '\\') {			/* non-ASCII: validate, keep */
			size_t w = utf8_check(p->s + p->i, p->n - p->i);
			if (w == 0) perr(p, "invalid UTF-8 in string");
			p->i += w;
			continue;
		}

		om(p->s + run, p->i - run);	/* flush the pending clean run */
		p->i++;
		if (p->i >= p->n) perr(p, "truncated escape");
		switch (p->s[p->i]) {
		case '"':  p->i++; os_("\\\""); break;
		case '\\': p->i++; os_("\\\\"); break;
		case '/':  p->i++; oc('/');     break;
		case 'b':  p->i++; os_("\\b");  break;
		case 'f':  p->i++; os_("\\f");  break;
		case 'n':  p->i++; os_("\\n");  break;
		case 'r':  p->i++; os_("\\r");  break;
		case 't':  p->i++; os_("\\t");  break;
		case 'u': {
			unsigned cp, lo;
			char ubuf[4];

			if (!hex4(p, p->i + 1, &cp)) perr(p, "bad \\u escape");
			p->i += 5;
			if (cp >= 0xD800 && cp <= 0xDBFF) {
				if (p->i + 1 < p->n && p->s[p->i] == '\\' &&
				    p->s[p->i + 1] == 'u' &&
				    hex4(p, p->i + 2, &lo) &&
				    lo >= 0xDC00 && lo <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					p->i += 6;
				} else {
					cp = 0xFFFD;
				}
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				cp = 0xFFFD;
			}
			if (cp < 0x80)
				emit_esc_byte((unsigned char)cp);
			else
				om(ubuf, utf8_put(ubuf, cp));
			break;
		}
		default:
			perr(p, "unknown escape");
		}
		run = p->i;
	}
}

static void emit_number(P *p)
{
	size_t start = p->i;

	if (peek(p) == '-') p->i++;
	if (peek(p) == '0') p->i++;
	else if (isdig(peek(p))) { while (isdig(peek(p))) p->i++; }
	else perr(p, "invalid number");
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
	om(p->s + start, p->i - start);
}

static void emit_array(P *p, int lvl)
{
	int first = 1;

	p->i++;					/* '[' */
	skipws(p);
	if (peek(p) == ']') { p->i++; os_("[]"); return; }
	os_("[\n");
	for (;;) {
		if (!first) os_(",\n");
		first = 0;
		emit_indent(lvl + 1);
		emit_value(p, lvl + 1);
		skipws(p);
		if (peek(p) == ',') { p->i++; skipws(p); continue; }
		if (peek(p) == ']') { p->i++; break; }
		perr(p, "expected ',' or ']'");
	}
	oc('\n');
	emit_indent(lvl);
	oc(']');
}

static void emit_object(P *p, int lvl)
{
	int first = 1;

	p->i++;					/* '{' */
	skipws(p);
	if (peek(p) == '}') { p->i++; os_("{}"); return; }
	os_("{\n");
	for (;;) {
		if (!first) os_(",\n");
		first = 0;
		emit_indent(lvl + 1);
		emit_string(p);
		skipws(p);
		if (peek(p) != ':') perr(p, "expected ':'");
		p->i++;
		os_(": ");
		skipws(p);
		emit_value(p, lvl + 1);
		skipws(p);
		if (peek(p) == ',') { p->i++; skipws(p); continue; }
		if (peek(p) == '}') { p->i++; break; }
		perr(p, "expected ',' or '}'");
	}
	oc('\n');
	emit_indent(lvl);
	oc('}');
}

static void emit_value(P *p, int lvl)
{
	if (++p->depth > MAXDEPTH)
		perr(p, "nesting too deep");
	skipws(p);
	switch (peek(p)) {
	case '{': emit_object(p, lvl); break;
	case '[': emit_array(p, lvl); break;
	case '"': emit_string(p); break;
	case 't': lit(p, "true");  os_("true");  break;
	case 'f': lit(p, "false"); os_("false"); break;
	case 'n': lit(p, "null");  os_("null");  break;
	case -1:  perr(p, "unexpected end of input"); return;
	default:
		if (peek(p) == '-' || isdig(peek(p))) { emit_number(p); break; }
		perr(p, "unexpected byte, input is not JSON");
	}
	p->depth--;
}

/* ---------- path selection ---------- */

/* ---------- result sink ----------
 * A filter produces a stream of values, not a single value. Path
 * selection happens to produce exactly one, but iteration and
 * predicates will produce any number, so results are delivered through
 * a callback rather than returned. Retrofitting this later would mean
 * rewriting every filter, so it is here from the start.
 */
typedef void (*sink_fn)(const jval *v, void *ctx);

/* Prints each value on its own line, the way a stream of results reads. */
struct print_ctx {
	int n;
};

static void print_sink(const jval *v, void *ctx)
{
	struct print_ctx *c = ctx;

	if (c->n++)
		oc('\n');
	/*
	 * -r prints a string result as its bytes, with no quotes and no
	 * escaping, so it can feed a shell pipeline. It applies only when
	 * the result itself is a string; strings nested inside a printed
	 * object or array are still valid JSON and stay quoted.
	 */
	if (opt_raw && v->t == J_STR)
		om(v->u.str.p, v->u.str.len);
	else
		print_val(v, 0);
}

/*
 * Array slice, Python semantics -- which RFC 9535 adopted, so the two
 * agree here. Negative indices count from the end, out-of-range bounds
 * are clamped rather than rejected, and a zero step is an error.
 *
 * The result is a new array rather than a stream of elements, matching
 * Python and jq. RFC 9535 would instead yield each element as its own
 * result; that difference is reserved for --rfc9535.
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
	int neg = 0;

	if (*s == '-') { neg = 1; s++; }
	if (!isdig((unsigned char)*s))
		return 0;
	while (isdig((unsigned char)*s)) {
		long d = *s - '0';
		if (v > (LONG_MAX - d) / 10)
			die("number too large in path");
		v = v * 10 + d;
		s++;
	}
	*pp = s;
	*out = neg ? -v : v;
	return 1;
}

static jval *slice_array(const jval *a, const jslice *sl)
{
	long n = (long)a->u.arr.n;
	long step = sl->has_step ? sl->step : 1;
	long start, end, i;
	jval *out;

	if (step == 0)
		die("slice step cannot be zero");

	/* defaults differ by direction: a[::-1] runs from the end down */
	start = sl->has_start ? sl->start : (step > 0 ? 0 : n - 1);
	end   = sl->has_end   ? sl->end   : (step > 0 ? n : -1);

	if (sl->has_start && start < 0) start += n;
	if (sl->has_end   && end   < 0) end   += n;

	if (step > 0) {				/* valid positions are [0, n] */
		if (start < 0) start = 0;
		if (start > n) start = n;
		if (end < 0) end = 0;
		if (end > n) end = n;
	} else {				/* and [-1, n-1] going down */
		if (start < -1) start = -1;
		if (start > n - 1) start = n - 1;
		if (end < -1) end = -1;
		if (end > n - 1) end = n - 1;
	}

	out = newval(J_ARR);
	if (step > 0)
		for (i = start; i < end; i += step)
			arr_push(out, a->u.arr.v[i]);
	else
		for (i = start; i > end; i += step)
			arr_push(out, a->u.arr.v[i]);
	return out;
}

/* Apply a bracket component. *pp points just past '['; leaves it past ']'. */
static const jval *apply_bracket(const jval *v, const char **pp, const char *path)
{
	const char *b = *pp;
	jslice sl;
	int is_slice = 0;

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

	{					/* plain index, may be negative */
		long idx = sl.start;

		if (idx < 0)
			idx += (long)v->u.arr.n;
		if (idx < 0 || idx >= (long)v->u.arr.n)
			die("index %ld out of range (total: %zu)",
			    sl.start, v->u.arr.n);
		return v->u.arr.v[idx];
	}
}

/* ---------- globbing ----------
 * '+' is the wildcard rather than '*' so a pattern survives the shell
 * unquoted: '*' would be expanded against the working directory before
 * the program ever runs, silently turning the query into something else.
 *
 * Only '+' is supported. Character classes are deliberately absent
 * because '[' already introduces an index or slice, and a pattern is
 * matched against one key at a time, so it never spans a '.'.
 */
static int is_glob(const char *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (p[i] == '+')
			return 1;
	return 0;
}

/*
 * Iterative backtracking match, so a pattern with many wildcards cannot
 * blow the stack the way a recursive matcher on hostile input would.
 */
static int glob_match(const char *pat, size_t pn, const char *str, size_t sn)
{
	size_t p = 0, s = 0, star = (size_t)-1, mark = 0;

	while (s < sn) {
		if (p < pn && pat[p] == '+') {
			star = p++;		/* remember where to resume */
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
 * Select every pair whose key matches, preserving the container type:
 * an object in, an object out. Zero matches yields an empty object
 * rather than an error, because a pattern is a query and querying for
 * something absent is a legitimate empty answer -- unlike naming a key
 * that is not there, which stays an error.
 */
static const jval *glob_object(const jval *v, const char *pat, size_t pn)
{
	jval *out = newval(J_OBJ);
	size_t i;

	for (i = 0; i < v->u.obj.n; i++)
		if (glob_match(pat, pn, v->u.obj.k[i].p, v->u.obj.k[i].len))
			obj_push(out, v->u.obj.k[i], v->u.obj.v[i]);
	return out;
}

/*
 * Apply one key component to a value. Returns the selected value, or
 * dies -- a named key that is absent is an error, while a pattern that
 * matches nothing yields an empty object.
 */
static const jval *apply_key(const jval *v, const char *q, size_t klen)
{
	if (v->t == J_OBJ && is_glob(q, klen))
		return glob_object(v, q, klen);

	if (v->t == J_OBJ) {
		size_t i;

		for (i = 0; i < v->u.obj.n; i++)
			if (v->u.obj.k[i].len == klen &&
			    memcmp(v->u.obj.k[i].p, q, klen) == 0)
				return v->u.obj.v[i];
		die("no such key: %.*s", (int)klen, q);
	}

	if (v->t == J_ARR) {
		size_t idx = 0, i;

		if (is_glob(q, klen))
			die("cannot glob an array with '%.*s': globbing "
			    "matches object keys (use [@] for a stream)",
			    (int)klen, q);
		for (i = 0; i < klen; i++) {
			size_t d;

			if (!isdig((unsigned char)q[i]))
				die("cannot index array with '%.*s'",
				    (int)klen, q);
			d = (size_t)(q[i] - '0');
			if (idx > (SIZE_MAX - d) / 10)
				die("index '%.*s' is too large", (int)klen, q);
			idx = idx * 10 + d;
		}
		if (idx >= v->u.arr.n)
			die("index %zu out of range (total: %zu)",
			    idx, v->u.arr.n);
		return v->u.arr.v[idx];
	}

	if (is_glob(q, klen))
		die("cannot glob %s with '%.*s': globbing matches object keys",
		    typename(v->t), (int)klen, q);
	die("cannot index %s with '%.*s'", typename(v->t), (int)klen, q);
	return NULL;				/* not reached */
}

/*
 * Find the ']' closing the bracket that starts at s. Nested brackets are
 * counted, so [@[0]] matches the outer one.
 */
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
	return NULL;				/* not reached */
}

static void walk(const jval *v, const char *q, const char *path,
		 sink_fn sink, void *ctx, int depth);

/*
 * [@...] converts an array into a stream: the remainder of the path is
 * applied to each element in turn and every result is delivered to the
 * sink separately, rather than being collected into one value.
 *
 * Anything written inside the brackets after '@' is simply the start of
 * that remainder, so array[@.na+] and array[@].na+ mean the same thing.
 * The two are spliced into one sub-path here, once per stream rather
 * than once per element.
 */
static void stream_array(const jval *v, const char *inner, size_t ilen,
			 const char *rest, const char *path,
			 sink_fn sink, void *ctx, int depth)
{
	size_t rlen = strlen(rest);
	char *sub;
	size_t i;
	amark m;

	if (v->t != J_ARR)
		die("cannot stream %s with [@]: [@] converts an array into a "
		    "stream of its elements", typename(v->t));

	sub = arena_alloc(ilen + rlen + 1);
	memcpy(sub, inner, ilen);
	memcpy(sub + ilen, rest, rlen);
	sub[ilen + rlen] = '\0';

	/*
	 * A lone '.' means identity at the top level, but inside brackets
	 * it is a trailing separator with nothing after it, which is an
	 * error everywhere else in a path.
	 */
	if (ilen + rlen > 0 && sub[ilen + rlen - 1] == '.')
		die("empty component in path '%s'", path);

	/*
	 * Taken after sub is allocated so the sub-path survives the loop.
	 * Anything a single element's filter allocates -- a glob result,
	 * a slice -- is dead once the sink has consumed it, so the next
	 * element reuses that memory instead of the arena growing with
	 * the length of the stream.
	 */
	m = arena_save();
	for (i = 0; i < v->u.arr.n; i++) {
		walk(v->u.arr.v[i], sub, path, sink, ctx, depth + 1);
		arena_restore(m);
	}
}

/*
 * Walk a path from position q, delivering every result to the sink.
 * Most paths yield exactly one value; [@] is what makes the count vary,
 * which is why nothing here returns a value.
 */
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
			return;			/* the recursion finishes it */
		}
		if (*q == '[') {
			q++;
			v = apply_bracket(v, &q, path);
		} else {
			size_t klen = 0;

			while (q[klen] && q[klen] != '.' && q[klen] != '[')
				klen++;
			if (klen == 0)
				die("empty component in path '%s'", path);
			v = apply_key(v, q, klen);
			q += klen;
		}
		if (*q == '.') {
			q++;
			if (*q == '\0' || *q == '.')
				die("empty component in path '%s'", path);
		}
	}
	sink(v, ctx);
}

/*
 * Apply a filter to a document and deliver every result to the sink.
 *
 * Today the only filter is a dotted path, which yields exactly one
 * value or exits with an error. Iteration and predicates will yield
 * zero or many, which is why nothing here returns a value.
 *
 * Execution modes, in increasing cost. Filters should be routed to the
 * cheapest mode that can express them:
 *
 *   1. no filter          stream straight to output, no tree      (main)
 *   2. forward-only       skip-scan, tree only the target subtree
 *   3. per-element        stream the outer array, tree one element
 *                         at a time, arena_restore() between them
 *   4. whole document     full tree; needed only by filters whose
 *                         first output depends on the last input
 *                         (sorting, grouping, deduplication)
 *
 * Only modes 1 and 4 exist so far. Mode 3 is the one that matters: it
 * bounds memory by the largest element rather than the document, so
 * input stops needing to fit in RAM.
 */
static void eval(const jval *root, const char *filter, sink_fn sink, void *ctx)
{
	amark m = arena_save();

	walk(root, filter, filter, sink, ctx, 0);

	/*
	 * Anything a filter allocated while producing results is dead once
	 * the sink has consumed them. A sink that needs to keep a value
	 * must copy it out. Per-element mode will run this loop once per
	 * element, which is what bounds memory.
	 */
	arena_restore(m);
}

/* ---------- main ---------- */

static char *slurp(FILE *f, size_t *outlen)
{
	size_t cap = 1 << 16, len = 0;
	char *buf = xmalloc(cap);

	for (;;) {
		size_t n;
		if (len + 1 == cap)
			buf = xrealloc(buf, cap *= 2);
		n = fread(buf + len, 1, cap - len - 1, f);
		len += n;
		if (n == 0) {
			if (ferror(f))
				die("read: %s", strerror(errno));
			break;
		}
	}
	buf[len] = '\0';
	*outlen = len;
	return buf;
}

static void usage(FILE *f)
{
	fprintf(f,
"usage: %s [options] [path]\n"
"\n"
"Reads JSON on stdin and prints it formatted. With a path, prints the\n"
"selected value instead.\n"
"\n"
"  path            user.name, items.0.id, items[2], items[1:3]\n"
"                  slices follow Python rules: negative indices count\n"
"                  from the end, out-of-range bounds are clamped, and\n"
"                  the result is a sub-array\n"
"\n"
"                  '+' globs object keys: items[0].na+ returns an object\n"
"                  of every matching key and its value. '+' is used\n"
"                  instead of '*' so patterns survive the shell without\n"
"                  quoting. Globbing an array or a scalar is an error.\n"
"\n"
"                  [@] turns an array into a stream, printing each result\n"
"                  separately. The rest of the path applies to every\n"
"                  element, so items[@].name and items[@.name] are the\n"
"                  same, and items[-100:][@.+ie] streams the last hundred\n"
"                  elements filtered to keys ending in 'ie'.\n"
"\n"
"  -r, --raw-output   print a string result without quotes or escapes\n"
"      --raw          alias for --raw-output\n"
"      --rfc9535      use RFC 9535 JSONPath semantics (not implemented)\n"
"  -h, --help         this message\n"
"  --                 end of options\n", progname);
}

int main(int argc, char **argv)
{
	char *buf;
	size_t len;
	P p;
	jval *root;
	struct print_ctx out;
	const char *filter = NULL;
	int i, endopts = 0;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (endopts || a[0] != '-' || a[1] == '\0') {
			if (filter)
				die("only one path may be given");
			filter = a;
		} else if (a[1] == '-') {		/* long option */
			const char *o = a + 2;
			if (*o == '\0') { endopts = 1; continue; }
			if (strcmp(o, "help") == 0) { usage(stdout); return 0; }
			else if (strcmp(o, "raw-output") == 0 ||
				 strcmp(o, "raw") == 0) opt_raw = 1;
			else if (strcmp(o, "rfc9535") == 0) opt_rfc = 1;
			else die("unknown option '%s' (try --help)", a);
		} else {				/* short option cluster */
			const char *o;
			for (o = a + 1; *o; o++) {
				switch (*o) {
				case 'r': opt_raw = 1; break;
				case 'h': usage(stdout); return 0;
				default:
					die("unknown option '-%c' (try --help)", *o);
				}
			}
		}
	}

	if (opt_rfc)
		die("--rfc9535 is not implemented yet: slices currently yield a\n"
		    "sub-array (Python and jq behaviour) rather than one result\n"
		    "per element, and a missing key is an error rather than an\n"
		    "empty result");

	setvbuf(stdout, NULL, _IOFBF, 1 << 16);
	buf = slurp(stdin, &len);
	p.s = buf; p.n = len; p.i = 0; p.depth = 0;

	skipws(&p);
	if (p.i == p.n)
		die("empty input");

	if (!filter) {
		/*
		 * No path: stream, never build a tree. The one exception is
		 * -r on a document that is itself a string, which has to be
		 * decoded before it can be printed unquoted.
		 */
		if (opt_raw && peek(&p) == '"') {
			jstr s = parse_string(&p);
			om(s.p, s.len);
		} else {
			emit_value(&p, 0);
		}
		skipws(&p);
		if (p.i != p.n)
			perr(&p, "trailing data after JSON value");
		oc('\n');
		oflush();
		return 0;
	}

	root = parse_value(&p);
	skipws(&p);
	if (p.i != p.n)
		perr(&p, "trailing data after JSON value");

	out.n = 0;
	eval(root, filter, print_sink, &out);
	oc('\n');
	oflush();
	return 0;
}
