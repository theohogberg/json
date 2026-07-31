/*
 * json - pretty-print JSON from stdin, optionally selecting a path
 *
 *   curl -s URL | json
 *   curl -s URL | json user.name
 *   curl -s URL | json items.0.id
 *
 * build: cc -std=c99 -O2 -Wall -Wextra -o json json.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#define MAXDEPTH 256

static const char *progname = "json";

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
 * The tool never frees, so per-value malloc buys nothing but overhead.
 * Bump-allocate from big chunks instead; exit reclaims.
 */
#define ARENA_CHUNK (1u << 20)
static char  *arena_ptr;
static size_t arena_left;

static void *arena_alloc(size_t n)
{
	void *p;

	n = (n + 15) & ~(size_t)15;		/* keep 16-byte alignment */
	if (n > arena_left) {
		size_t sz = n > ARENA_CHUNK ? n : ARENA_CHUNK;
		arena_ptr = xmalloc(sz);
		arena_left = sz;
	}
	p = arena_ptr;
	arena_ptr += n;
	arena_left -= n;
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
 * surrogate half, or a code point above U+10FFFF. RFC 8259 s8.1 requires
 * JSON text to be UTF-8, so anything else is rejected rather than passed
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

/* ---------- path selection ---------- */

static const jval *select_path(const jval *v, const char *path)
{
	const char *q = path;

	if (*q == '.')				/* allow both a.b and .a.b */
		q++;
	while (*q) {
		const char *dot = strchr(q, '.');
		size_t klen = dot ? (size_t)(dot - q) : strlen(q);

		if (klen == 0 || (dot && dot[1] == '\0'))
			die("empty component in path '%s'", path);

		if (v->t == J_OBJ) {
			const jval *next = NULL;
			size_t i;
			for (i = 0; i < v->u.obj.n; i++) {
				if (v->u.obj.k[i].len == klen &&
				    memcmp(v->u.obj.k[i].p, q, klen) == 0) {
					next = v->u.obj.v[i];
					break;
				}
			}
			if (!next)
				die("no such key: %.*s", (int)klen, q);
			v = next;
		} else if (v->t == J_ARR) {
			size_t idx = 0, i;
			for (i = 0; i < klen; i++) {
				size_t d;
				if (!isdig((unsigned char)q[i]))
					die("cannot index array with '%.*s'", (int)klen, q);
				d = (size_t)(q[i] - '0');
				if (idx > (SIZE_MAX - d) / 10)
					die("index '%.*s' is too large", (int)klen, q);
				idx = idx * 10 + d;
			}
			if (idx >= v->u.arr.n)
				die("index %zu out of range (total: %zu)",
				    idx, v->u.arr.n);
			v = v->u.arr.v[idx];
		} else {
			die("cannot index %s with '%.*s'", typename(v->t), (int)klen, q);
		}
		q = dot ? dot + 1 : q + klen;
	}
	return v;
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

int main(int argc, char **argv)
{
	char *buf;
	size_t len;
	P p;
	jval *root;
	const jval *sel;

	if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
		fprintf(stderr, "usage: %s [path]\n"
		                "  reads JSON on stdin, prints it formatted\n"
		                "  path selects a value: user.name, items.0.id\n", progname);
		return 0;
	}
	if (argc > 2)
		die("usage: %s [path]", progname);

	setvbuf(stdout, NULL, _IOFBF, 1 << 16);
	buf = slurp(stdin, &len);
	p.s = buf; p.n = len; p.i = 0; p.depth = 0;

	skipws(&p);
	if (p.i == p.n)
		die("empty input");
	root = parse_value(&p);
	skipws(&p);
	if (p.i != p.n)
		perr(&p, "trailing data after JSON value");

	sel = (argc == 2) ? select_path(root, argv[1]) : root;
	print_val(sel, 0);
	oc('\n');
	oflush();
	return 0;
}
