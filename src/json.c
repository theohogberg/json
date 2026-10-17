/* json - format JSON from stdin, optionally selecting a value; see CLAUDE.md */
#define _POSIX_C_SOURCE 200809L		/* read(2) and fstat(2): libc, not C99 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef MAXDEPTH
#define MAXDEPTH 1024
#endif

/* a regular file up to SLURP_MAX is read whole; anything else streams */
#ifndef SLURP_MAX
#define SLURP_MAX ((size_t)1 << 28)
#endif
#ifndef WINDOW
#define WINDOW (1u << 16)
#endif

/* hot-path helpers must not cost a call per token */
#ifdef __GNUC__
#define NORETURN __attribute__((noreturn))
#define INLINE   inline __attribute__((always_inline))
#define COLD     __attribute__((noinline))
#else
#define NORETURN
#define INLINE   inline
#define COLD
#endif

static int opt_raw;

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

/* bump allocator: kept chunks make arena_restore() O(1), and get reused */
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

	n = (n + 7) & ~(size_t)7;		/* nothing stored needs more than 8 */
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

static void *arena_zalloc(size_t n)
{
	return memset(arena_alloc(n), 0, n);
}

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
	jval *v = arena_zalloc(sizeof *v);

	v->t = t;
	return v;
}

/* growth abandons the old block: fine per query, too wasteful for the tree */
static void *arena_grow(const void *old, size_t oldn, size_t newn, size_t esz)
{
	void *p = arena_alloc(newn * esz);

	if (oldn)
		memcpy(p, old, oldn * esz);
	return p;
}

/* key is NULL for arrays */
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

/*
 * The two hot loops, and the only place intrinsics belong (scan_ws measured
 * faster without). Read only s[0..n),
 * return what the scalar loop would, and fall through to it for the tail.
 * Guard vector code with !defined(NOSIMD) so `make scalar` tests the fallback.
 *
 * x86: SSE2, which every x86-64 has; SSE4.2's string instructions are slower
 * than this. AVX2 is used if the build targets it (-mavx2) or, with GCC or
 * clang, if the CPU reports it at run time, so one binary fits every x86-64.
 * -DNOAVX2 stops at SSE2. ARM: NEON, which every arm64 has. Either way the
 * test is one compare per byte class and a mask of the first hit.
 */
#if !defined(NOSIMD)
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define SIMD_SSE2
#include <emmintrin.h>
#if defined(__AVX2__) && !defined(NOAVX2)
#define SIMD_AVX2
#define AVX2_FN
#define have_avx2 1
#include <immintrin.h>
#elif defined(__GNUC__) && !defined(NOAVX2)
#define SIMD_AVX2
#define SIMD_AVX2_DISPATCH
#define AVX2_FN __attribute__((target("avx2")))
#include <immintrin.h>
static int have_avx2;			/* set by simd_init() */
#endif
#elif (defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)) && \
    !defined(__ARM_BIG_ENDIAN)
#define SIMD_NEON
#include <arm_neon.h>
#endif
#endif

static void simd_init(void)
{
#ifdef SIMD_AVX2_DISPATCH
	__builtin_cpu_init();
	have_avx2 = __builtin_cpu_supports("avx2") != 0;
#endif
}

#if defined(SIMD_SSE2) || defined(SIMD_NEON)
static INLINE unsigned ctz64(uint64_t m)	/* m != 0 */
{
#if defined(__GNUC__)
	return (unsigned)__builtin_ctzll(m);
#else
	unsigned i = 0;

	while (!(m & 1)) {
		m >>= 1;
		i++;
	}
	return i;
#endif
}
#endif

#ifdef SIMD_NEON
/* one bit in four per byte of a 0x00/0xFF mask, as there is no movemask */
static INLINE uint64_t neon_mask(uint8x16_t m)
{
	return vget_lane_u64(vreinterpret_u64_u8(
		vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0);
}
#endif

#ifdef SIMD_AVX2
/* scan_str 32 bytes at a time from i; the hit, or (size_t)-1 if none */
static AVX2_FN size_t scan_str_avx2(const char *s, size_t n, size_t i)
{
	const __m256i q = _mm256_set1_epi8('"'), b = _mm256_set1_epi8('\\');
	const __m256i c = _mm256_set1_epi8(0x20);

	for (; i + 32 <= n; i += 32) {
		__m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(s + i));
		__m256i m = _mm256_or_si256(
			_mm256_or_si256(_mm256_cmpeq_epi8(v, q), _mm256_cmpeq_epi8(v, b)),
			_mm256_cmpgt_epi8(c, v));	/* signed: < 0x20 or >= 0x80 */
		uint32_t bits = (uint32_t)_mm256_movemask_epi8(m);

		if (bits)
			return i + ctz64(bits);
	}
	return (size_t)-1;
}
#endif

/* first quote, backslash, control or non-ASCII byte in s[0..n), else n */
static INLINE size_t scan_str(const char *s, size_t n)
{
	size_t i = 0;

#if defined(SIMD_SSE2) || defined(SIMD_NEON)
	/*
	 * Most strings are short keys, where setting up a vector costs more than
	 * it saves, so the first 8 bytes are one 64-bit word: per byte, high bit
	 * set if it is a quote or backslash (x - 1 & ~x of x ^ c), a control
	 * byte (x - 0x20 borrows) or non-ASCII. A borrow only marks bytes above
	 * a real hit, so the lowest mark is exact. Both targets are little-endian.
	 */
	if (n >= 8) {
		const uint64_t ones = 0x0101010101010101u;
		uint64_t x, m;

		memcpy(&x, s, 8);
		m = x ^ (ones * '"');
		m = (m - ones) & ~m;
		x ^= ones * '\\';
		m |= (x - ones) & ~x;
		x ^= ones * '\\';
		m |= (x - ones * 0x20) | x;
		m &= ones * 0x80;
		if (m)
			return ctz64(m) >> 3;
		i = 8;
	}
#endif
#if defined(SIMD_SSE2)
	const __m128i q = _mm_set1_epi8('"'), b = _mm_set1_epi8('\\');
	const __m128i c = _mm_set1_epi8(0x20);

	for (; i + 16 <= n; i += 16) {
		__m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
		__m128i m = _mm_or_si128(
			_mm_or_si128(_mm_cmpeq_epi8(v, q), _mm_cmpeq_epi8(v, b)),
			_mm_cmplt_epi8(v, c));		/* signed: < 0x20 or >= 0x80 */
		uint32_t bits = (uint32_t)_mm_movemask_epi8(m);

		if (bits)
			return i + ctz64(bits);
#ifdef SIMD_AVX2
		/* past one block the string is long: go wide */
		if (have_avx2 && i == 8 && n >= 56) {
			size_t r = scan_str_avx2(s, n, 24);

			if (r != (size_t)-1)
				return r;
			i = 8 + ((n - 24) & ~(size_t)31);	/* +16: past it */
		}
#endif
	}
#elif defined(SIMD_NEON)
	const uint8x16_t q = vdupq_n_u8('"'), b = vdupq_n_u8('\\');
	const int8x16_t c = vdupq_n_s8(0x20);

	for (; i + 16 <= n; i += 16) {
		uint8x16_t v = vld1q_u8((const uint8_t *)s + i);
		uint8x16_t m = vorrq_u8(vorrq_u8(vceqq_u8(v, q), vceqq_u8(v, b)),
					vcltq_s8(vreinterpretq_s8_u8(v), c));
		uint64_t bits = neon_mask(m);

		if (bits)
			return i + (ctz64(bits) >> 2);
	}
#endif
	for (; i < n; i++) {
		unsigned char ch = (unsigned char)s[i];
		if (ch == '"' || ch == '\\' || ch < 0x20 || ch >= 0x80)
			break;
	}
	return i;
}

/*
 * Scalar on purpose: whitespace runs are 0-1 bytes in compact input and
 * 5-13 indented, and a vector loop was slower on both (NEON, 15-25 MB).
 */
static size_t scan_ws(const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
			break;
	return i;
}

/*
 * Input is a window s[0..n) over stdin, unread from i. A slurped file is one
 * window that never moves. Otherwise more() slides it, keeping s[i..n), so
 * scanners address bytes relative to i and advance i only past whole tokens.
 */
typedef struct {
	char *s;
	size_t n, i, cap;
	size_t off;		/* input offset of s[0], for messages */
	int fd;			/* -1 once the rest of the input is in s */
	int windowed;		/* s moves, so nothing may point into it */
	int depth;
} P;

static NORETURN void perr_at(P *p, size_t k, const char *msg)
{
	die("parse error at byte %zu: %s", p->off + p->i + k, msg);
}

static NORETURN void perr(P *p, const char *msg)
{
	perr_at(p, 0, msg);
}

static void oflush(void);

/* read more input, keeping s[i..n); 0 at end of input */
static int more(P *p)
{
	ssize_t r;

	if (p->fd < 0)
		return 0;
	if (p->i) {
		memmove(p->s, p->s + p->i, p->n - p->i);
		p->off += p->i;
		p->n -= p->i;
		p->i = 0;
	}
	oflush();				/* show what we have before blocking */
	if (p->n == p->cap) {
		/*
		 * Full: a slurped file, sized exactly so ASan sees a read past
		 * its end, or one token filling the window. Grow only if a
		 * byte is really there.
		 */
		char c;

		do
			r = read(p->fd, &c, 1);
		while (r < 0 && errno == EINTR);
		if (r > 0) {
			p->s = xrealloc(p->s, p->cap *= 2);
			p->s[p->n] = c;
		}
	} else {
		do
			r = read(p->fd, p->s + p->n, p->cap - p->n);
		while (r < 0 && errno == EINTR);
	}
	if (r < 0)
		die("read: %s", strerror(errno));
	if (r == 0) {
		p->fd = -1;
		return 0;
	}
	p->n += (size_t)r;
	return 1;
}

/*
 * The heuristic: a regular file of known, moderate size is slurped in one
 * read, so the tree may point into it. Pipes, terminals, sockets and huge
 * files stream through a window, so memory is bounded by the largest token
 * plus whatever the query keeps, not by the input.
 */
static void input(P *p, int fd)
{
	struct stat st;

	memset(p, 0, sizeof *p);
	p->fd = fd;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
	    (uintmax_t)st.st_size < SLURP_MAX) {
		p->cap = st.st_size ? (size_t)st.st_size : 1;
		p->s = xrealloc(NULL, p->cap);
		while (more(p))
			;
	} else {
		p->windowed = 1;
		p->cap = WINDOW;
		p->s = xrealloc(NULL, p->cap);
	}
}

/* bytes unread, refilling until there are k or the input ends */
static size_t avail(P *p, size_t k)
{
	while (p->n - p->i < k && more(p))
		;
	return p->n - p->i;
}

/* past whitespace; the window then holds a byte unless the input is over */
static void skipws(P *p)
{
	do
		p->i += scan_ws(p->s + p->i, p->n - p->i);
	while (p->i == p->n && more(p));
}

/* only after skipws(), so it never needs to refill */
static int peek(P *p)
{
	return p->i < p->n ? (unsigned char)p->s[p->i] : -1;
}

static void lit(P *p, const char *word)
{
	size_t l = strlen(word);

	if ((p->n - p->i < l && avail(p, l) < l) ||
	    memcmp(p->s + p->i, word, l) != 0)
		perr(p, "invalid literal");
	p->i += l;
}

/* length of the number at s[0..n), or where it goes wrong and *bad why */
static INLINE size_t num_len(const char *s, size_t n, const char **bad)
{
#define C(k) ((k) < n ? (unsigned char)s[k] : -1)
	size_t k = C(0) == '-' ? 1 : 0;

	*bad = NULL;
	if (C(k) == '0')
		k++;
	else if (isdig(C(k)))
		while (isdig(C(k))) k++;
	else
		*bad = "invalid number";

	if (!*bad && C(k) == '.') {
		k++;
		if (!isdig(C(k))) *bad = "expected digit after '.'";
		while (isdig(C(k))) k++;
	}
	if (!*bad && (C(k) == 'e' || C(k) == 'E')) {
		k++;
		if (C(k) == '+' || C(k) == '-') k++;
		if (!isdig(C(k))) *bad = "expected digit in exponent";
		while (isdig(C(k))) k++;
	}
	return k;
#undef C
}

/* length of the number at i; one that runs into the window's end rescans */
static INLINE size_t scan_number(P *p)
{
	const char *bad;
	size_t k;

	while ((k = num_len(p->s + p->i, p->n - p->i, &bad)) == p->n - p->i &&
	       more(p))
		;
	if (bad)
		perr_at(p, k, bad);
	return k;
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

/* length of the valid UTF-8 sequence at s[0..n), or 0 if invalid */
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

/* decode the code point at already-valid UTF-8 s[0..n) */
static size_t utf8_get(const char *s, size_t n, unsigned *cp)
{
	unsigned char a = (unsigned char)s[0];
	size_t w = a < 0x80 ? 1 : a < 0xE0 ? 2 : a < 0xF0 ? 3 : 4, k;
	unsigned v = w == 1 ? a : a & (0x3Fu >> (w - 1));

	if (w > n)
		w = n;
	for (k = 1; k < w; k++)
		v = v << 6 | ((unsigned char)s[k] & 0x3F);
	*cp = v;
	return w;
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

/* str_end's rare cases, out of line so str_end inlines; a refill re-bases end */
static COLD size_t str_slow(P *p, size_t end, int *esc)
{
	size_t k = end - p->i, m = avail(p, k + 4), w;	/* escape, UTF-8 */
	unsigned char c = k < m ? (unsigned char)p->s[p->i + k] : 0;

	end = p->i + k;
	if (k >= m || (c == '\\' && k + 1 >= m))
		perr(p, "unterminated string");
	if (c == '"')				/* the refill brought it */
		return end;
	if (c == '\\') {
		*esc = 1;
		return end + 2;
	}
	w = c < 0x20 ? 0 : utf8_check(p->s + end, m - k);
	if (w == 0)
		perr_at(p, k, c < 0x20 ? "control character in string"
				       : "invalid UTF-8 in string");
	return end + w;
}

/* index of the closing quote, *esc if escaped; i stays on the opening one */
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

/* decode the escape whose backslash precedes s[*j]; *j moves past it */
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

static char   obuf[1u << 16];
static size_t olen;

static void oflush(void)
{
	if (olen && fwrite(obuf, 1, olen, stdout) != olen)
		die("write error: %s", strerror(errno));
	olen = 0;
	if (fflush(stdout) != 0)
		die("write error: %s", strerror(errno));
}

/* om's rare case, out of line so om inlines */
static COLD void om_slow(const char *s, size_t n)
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

static INLINE void os_(const char *s) { om(s, strlen(s)); }

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

/* children of open containers; each closes into an exact-size arena copy */
static jstr  *stk_k;
static jval **stk_v;
static size_t stk_n, stk_cap;

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
	size_t len;

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
		if (peek(p) != '-' && !isdig(peek(p)))
			perr(p, "unexpected byte, input is not JSON");
		len = scan_number(p);
		v = newval(J_NUM);
		v->u.num.p = p->s + p->i;
		if (p->windowed)		/* the lexeme, not the window */
			v->u.num.p = arena_grow(p->s + p->i, len, len, 1);
		v->u.num.len = len;
		p->i += len;
	}
	p->depth--;
	return v;
}

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

/* streaming path: no allocation, output byte-identical to print_val */
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
	while (esc) {
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
	size_t len;

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
		if (peek(p) != '-' && !isdig(peek(p)))
			perr(p, "unexpected byte, input is not JSON");
		len = scan_number(p);
		om(p->s + p->i, len);
		p->i += len;
	}
	p->depth--;
}

/* after a member: 1 past a ',', 0 past the closing bracket */
static int next_member(P *p, char close)
{
	skipws(p);
	if (peek(p) == ',') { p->i++; skipws(p); return 1; }
	if (peek(p) == close) { p->i++; return 0; }
	perr(p, close == ']' ? "expected ',' or ']'" : "expected ',' or '}'");
}

/* validate and step over a value: what a query does not select */
static void skip_value(P *p);

static void skip_string(P *p)
{
	int esc;
	size_t end, j;
	const char *b;

	if (peek(p) != '"')
		perr(p, "expected string");
	end = str_end(p, &esc);
	for (j = p->i + 1; esc && (b = memchr(p->s + j, '\\', end - j)); ) {
		j = (size_t)(b - p->s) + 1;
		unescape(p, &j);
	}
	p->i = end + 1;
}

/* in an object, past a member's key and its ':' */
static void skip_key(P *p)
{
	skip_string(p);
	skipws(p);
	if (peek(p) != ':')
		perr(p, "expected ':'");
	p->i++;
}

static void skip_container(P *p)
{
	char open = p->s[p->i], close = open == '[' ? ']' : '}';

	p->i++;
	skipws(p);
	if (peek(p) == close) {
		p->i++;
		return;
	}
	do {
		if (open == '{')
			skip_key(p);
		skip_value(p);
	} while (next_member(p, close));
}

static void skip_value(P *p)
{
	size_t len;

	if (++p->depth > MAXDEPTH)
		perr(p, "nesting too deep");
	skipws(p);
	switch (peek(p)) {
	case '{': case '[': skip_container(p); break;
	case '"': skip_string(p); break;
	case 't': lit(p, "true");  break;
	case 'f': lit(p, "false"); break;
	case 'n': lit(p, "null");  break;
	case -1:  perr(p, "unexpected end of input");
	default:
		if (peek(p) != '-' && !isdig(peek(p)))
			perr(p, "unexpected byte, input is not JSON");
		len = scan_number(p);
		p->i += len;
	}
	p->depth--;
}

typedef void (*sink_fn)(const jval *v, void *ctx);

/* ctx counts the values printed */
static void print_sink(const jval *v, void *ctx)
{
	int *n = ctx;

	if ((*n)++)
		oc('\n');
	/* -r only for a string result: nested strings stay valid JSON */
	if (opt_raw && v->t == J_STR)
		om(v->u.str.p, v->u.str.len);
	else
		print_val(v, 0);
}

/* Python slice semantics; the result is one sub-array */
typedef struct {
	long long start, end, step;
	int has_start, has_end, has_step;
} jslice;

/* optional signed integer; returns whether one was present */
static int slice_num(const char **pp, long long *out)
{
	const char *s = *pp;
	long long v = 0;
	int neg = *s == '-';

	s += neg;
	if (!isdig((unsigned char)*s))
		return 0;
	for (; isdig((unsigned char)*s); s++) {
		long long d = *s - '0';

		if (v > (LLONG_MAX - d) / 10)
			die("number too large in path");
		v = v * 10 + d;
	}
	*pp = s;
	*out = neg ? -v : v;
	return 1;
}

/* how many of n elements a nonzero-step slice selects; the first in *first */
static long long slice_range(const jslice *sl, long long n, long long *first)
{
	long long step = sl->has_step ? sl->step : 1;
	long long lo = step > 0 ? 0 : -1, hi = step > 0 ? n : n - 1;
	long long start, end;

	/* defaults depend on direction: a[::-1] starts at the end */
	start = sl->has_start ? sl->start : (step > 0 ? 0 : n - 1);
	end   = sl->has_end   ? sl->end   : (step > 0 ? n : -1);
	if (sl->has_start && start < 0) start += n;
	if (sl->has_end   && end   < 0) end   += n;
	start = start < lo ? lo : start > hi ? hi : start;
	end   = end   < lo ? lo : end   > hi ? hi : end;
	*first = start;

	/* count first: stepping i past end could overflow for a huge step */
	if (step > 0)
		return start < end ? (end - start - 1) / step + 1 : 0;
	return start > end ? (start - end - 1) / -step + 1 : 0;
}

static jval *slice_array(const jval *a, const jslice *sl)
{
	long long step = sl->has_step ? sl->step : 1, start, cnt, k;
	jval *out = newval(J_ARR);

	if (step == 0)
		die("slice step cannot be zero");
	cnt = slice_range(sl, (long long)a->u.c.n, &start);
	out->u.c.v = arena_alloc((size_t)cnt * sizeof *out->u.c.v);
	for (k = 0; k < cnt; k++)
		out->u.c.v[k] = a->u.c.v[start + k * step];
	out->u.c.n = out->u.c.cap = (size_t)cnt;
	return out;
}

/* iterative, so many wildcards cannot exhaust the stack */
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

/* an array index spelled as a key: a.0 */
static size_t key_index(const char *q, size_t klen)
{
	size_t i, idx = 0;

	for (i = 0; i < klen; i++) {
		size_t d;

		if (!isdig((unsigned char)q[i]))
			die("cannot index array with '%.*s'", (int)klen, q);
		d = (size_t)(q[i] - '0');
		if (idx > (SIZE_MAX - d) / 10)
			die("index '%.*s' is too large", (int)klen, q);
		idx = idx * 10 + d;
	}
	return idx;
}

/* index of the first member named k, or the member count */
static size_t member(const jval *v, const char *k, size_t klen)
{
	size_t i;

	for (i = 0; i < v->u.c.n; i++)
		if (v->u.c.k[i].len == klen && memcmp(v->u.c.k[i].p, k, klen) == 0)
			break;
	return i;
}

static const jval *apply_key(const jval *v, const char *q, size_t klen)
{
	int glob = memchr(q, '+', klen) != NULL;
	size_t i, idx;

	if (v->t == J_OBJ && glob) {
		jval *out = newval(J_OBJ);

		for (i = 0; i < v->u.c.n; i++)
			if (glob_match(q, klen, v->u.c.k[i].p, v->u.c.k[i].len))
				push(out, &v->u.c.k[i], v->u.c.v[i]);
		return out;
	}
	if (v->t == J_OBJ) {
		if ((i = member(v, q, klen)) < v->u.c.n)
			return v->u.c.v[i];
		die("no such key: %.*s", (int)klen, q);
	}
	if (glob)
		die("cannot glob %s with '%.*s': globbing matches object keys",
		    typename(v->t), (int)klen, q);
	if (v->t != J_ARR)
		die("cannot index %s with '%.*s'", typename(v->t), (int)klen, q);

	idx = key_index(q, klen);
	if (idx >= v->u.c.n)
		die("index %zu out of range (total: %zu)", idx, v->u.c.n);
	return v->u.c.v[idx];
}

static int isblank_(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static jstr path_quoted(const char **s);

/* one selector of a bracket: [0], [1:3], ['name'] */
typedef struct {
	char kind;		/* 'i'ndex (sl.start), 's'lice, 'n'ame */
	jslice sl;
	jstr name;
} bsel;

/* the selectors in [a,b,...]; *pp points past '[' and is left past ']' */
static size_t parse_bracket(const char **pp, const char *path, bsel **out)
{
	const char *b = *pp;
	bsel *sel = NULL;
	size_t n = 0;

	for (;;) {
		bsel x;

		memset(&x, 0, sizeof x);
		while (isblank_(*b))
			b++;
		x.kind = 'i';
		if (*b == '\'' || *b == '"') {
			x.kind = 'n';
			x.name = path_quoted(&b);
		} else {
			x.sl.has_start = slice_num(&b, &x.sl.start);
			if (*b == ':') {
				x.kind = 's';
				b++;
				x.sl.has_end = slice_num(&b, &x.sl.end);
				if (*b == ':') {
					b++;
					x.sl.has_step = slice_num(&b, &x.sl.step);
				}
			}
		}
		while (isblank_(*b))
			b++;
		if (*b != ']' && *b != ',')
			die("malformed bracket in path '%s'", path);
		if (x.kind == 'i' && !x.sl.has_start)
			die("empty bracket in path '%s'", path);
		sel = arena_grow(sel, n, n + 1, sizeof *sel);
		sel[n++] = x;
		if (*b++ == ']')
			break;
	}
	*pp = b;
	*out = sel;
	return n;
}

static void take(jval *out, const jval *v, char *taken, size_t j)
{
	if (!taken[j]) {
		taken[j] = 1;
		push(out, v->t == J_OBJ ? &v->u.c.k[j] : NULL, v->u.c.v[j]);
	}
}

/* [a,b,...]: one sub-array or sub-object, each member once, as asked */
static jval *apply_union(const jval *v, const bsel *sel, size_t n)
{
	size_t i, m;
	char *taken;
	jval *out;

	if (v->t != J_ARR && v->t != J_OBJ)
		die("cannot index %s with a bracket", typename(v->t));
	m = v->u.c.n;
	taken = arena_zalloc(m + 1);
	out = newval(v->t);
	for (i = 0; i < n; i++) {
		const bsel *x = &sel[i];
		long long step = x->sl.has_step ? x->sl.step : 1, first, cnt, k;
		size_t j;

		if ((x->kind == 'n') != (v->t == J_OBJ))
			die(v->t == J_OBJ ? "cannot index object with a number"
					  : "cannot index array with a name");
		if (x->kind == 'n') {
			if ((j = member(v, x->name.p, x->name.len)) == m)
				die("no such key: %.*s", (int)x->name.len, x->name.p);
			take(out, v, taken, j);
		} else if (x->kind == 'i') {
			k = x->sl.start < 0 ? x->sl.start + (long long)m : x->sl.start;
			if (k < 0 || k >= (long long)m)
				die("index %lld out of range (total: %zu)", x->sl.start, m);
			take(out, v, taken, (size_t)k);
		} else {
			if (step == 0)
				die("slice step cannot be zero");
			cnt = slice_range(&x->sl, (long long)m, &first);
			for (k = 0; k < cnt; k++)
				take(out, v, taken, (size_t)(first + k * step));
		}
	}
	return out;
}

/* *pp points past '['; leaves it past ']' */
static const jval *apply_bracket(const jval *v, const char **pp,
				 const char *path)
{
	bsel *sel;
	size_t n = parse_bracket(pp, path, &sel), j;
	long long idx;

	if (n > 1)
		return apply_union(v, sel, n);
	if (sel->kind == 'n') {
		if (v->t != J_OBJ)
			die("cannot index %s with a name", typename(v->t));
		if ((j = member(v, sel->name.p, sel->name.len)) == v->u.c.n)
			die("no such key: %.*s", (int)sel->name.len, sel->name.p);
		return v->u.c.v[j];
	}
	if (v->t != J_ARR)
		die("cannot index %s with a bracket", typename(v->t));
	if (sel->kind == 's')
		return slice_array(v, &sel->sl);

	idx = sel->sl.start < 0 ? sel->sl.start + (long long)v->u.c.n
				: sel->sl.start;
	if (idx < 0 || idx >= (long long)v->u.c.n)
		die("index %lld out of range (total: %zu)", sel->sl.start, v->u.c.n);
	return v->u.c.v[idx];
}

/*
 * [?expr] is RFC 9535's filter, compiled and evaluated by the --rfc9535 code
 * below, so it has RFC semantics: a missing member is false, not an error.
 * It streams the matching elements or member values, like [@]. doc_root is what $ means; a
 * path whose filters use $ is answered from a tree of the whole document.
 */
typedef struct rexp rexp;
static rexp *path_filter(const char *q, const char **end);
static int x_true(const rexp *e, const jval *cur, const jval *root);
static const jval *doc_root;

/* past the quoted name at s, escapes and all; at the NUL if unterminated */
static const char *skip_quoted(const char *s)
{
	char quote = *s++;

	while (*s && *s != quote)
		s += *s == '\\' && s[1] ? 2 : 1;
	return *s ? s + 1 : s;
}

/*
 * the ']' closing the '[' at s. Filters and quoted names are skipped whole,
 * so a ']' in either is safe; a quote starts a name only after '[' or ','.
 */
static const char *bracket_end(const char *s, const char *path)
{
	int depth = 0, name = 0;

	while (*s) {
		if (s[0] == '[' && s[1] == '?') {
			path_filter(s, &s);
			if (depth == 0)
				return s - 1;
			name = 0;
			continue;
		}
		if (name && (*s == '\'' || *s == '"')) {
			s = skip_quoted(s);
			name = 0;
			continue;
		}
		if (*s == '[')
			depth++;
		else if (*s == ']' && --depth == 0)
			return s;
		if (!isblank_(*s))
			name = *s == '[' || *s == ',';
		s++;
	}
	die("unclosed '[' in path '%s'", path);
}

/* past the '.' between components, but not a '..'; "a." and "a.." err */
static const char *next_comp(const char *q, const char *path)
{
	if (*q != '.')
		return q;
	if (q[1] == '\0' || (q[1] == '.' && q[2] == '\0'))
		die("empty component in path '%s'", path);
	return q[1] == '.' ? q : q + 1;
}

/* what [@inner]rest runs on each element: inner then rest */
static const char *stream_path(const char *inner, size_t ilen,
			       const char *rest, const char *path)
{
	size_t rlen = strlen(rest);
	char *sub = arena_alloc(ilen + rlen + 1);

	if (ilen)
		memcpy(sub, inner, ilen);
	memcpy(sub + ilen, rest, rlen + 1);
	/* a dangling '.' is an empty component */
	if (ilen + rlen > 0 && sub[ilen + rlen - 1] == '.')
		die("empty component in path '%s'", path);
	return sub;
}

static void walk(const jval *v, const char *q, const char *path,
		 sink_fn sink, void *ctx, int depth);

static void stream_array(const jval *v, const char *inner, size_t ilen,
			 const char *rest, const char *path,
			 sink_fn sink, void *ctx, int depth)
{
	const char *sub;
	size_t i;
	amark m;

	if (v->t != J_ARR && v->t != J_OBJ)
		die("cannot stream %s with [@]: it needs an array or object",
		    typename(v->t));
	sub = stream_path(inner, ilen, rest, path);
	m = arena_save();			/* after sub, which must survive */
	for (i = 0; i < v->u.c.n; i++) {
		walk(v->u.c.v[i], sub, path, sink, ctx, depth + 1);
		arena_restore(m);
	}
}

static void filter_array(const jval *v, const char *q, const char *path,
			 sink_fn sink, void *ctx, int depth)
{
	const char *end, *sub;
	rexp *f = path_filter(q, &end);
	size_t i;
	amark m;

	if (v->t != J_ARR && v->t != J_OBJ)
		die("cannot filter %s with [?]: it needs an array or object",
		    typename(v->t));
	sub = stream_path(NULL, 0, end, path);
	m = arena_save();
	for (i = 0; i < v->u.c.n; i++) {
		if (x_true(f, v->u.c.v[i], doc_root))
			walk(v->u.c.v[i], sub, path, sink, ctx, depth + 1);
		arena_restore(m);
	}
}

/* ..sel: what is selected under each node */
typedef struct {
	char kind;		/* 'n'ame, 'g'lob, 'i'ndex, '@', '?' */
	const char *k;
	size_t klen;
	long long idx;
	rexp *f;
	const char *rest;	/* the path each selected value goes on with */
} dsel;

/*
 * sel applied to v, then to each child, depth first: the order of RFC 9535
 * and of jq's .., where a node's own matches come before its children's.
 * It is a search, so a node without the key or index has none, not an error.
 */
static void desc_apply(const jval *v, const dsel *d, const char *path,
		       sink_fn sink, void *ctx, int depth)
{
	size_t i, n = v->t == J_ARR || v->t == J_OBJ ? v->u.c.n : 0, j = n;
	long long idx = d->idx < 0 ? d->idx + (long long)n : d->idx;
	amark m = arena_save();

	if (d->kind == 'n' && v->t == J_OBJ)
		j = member(v, d->k, d->klen);
	if (d->kind == 'i' && v->t == J_ARR && idx >= 0 && idx < (long long)n)
		j = (size_t)idx;
	for (i = 0; i < n; i++) {
		int hit = i == j;

		if (d->kind == 'g')
			hit = v->t == J_OBJ && glob_match(d->k, d->klen,
							  v->u.c.k[i].p, v->u.c.k[i].len);
		else if (d->kind == '@')
			hit = 1;
		else if (d->kind == '?')
			hit = x_true(d->f, v->u.c.v[i], doc_root);
		if (hit)
			walk(v->u.c.v[i], d->rest, path, sink, ctx, depth + 1);
		arena_restore(m);
	}
	for (i = 0; i < n; i++)
		desc_apply(v->u.c.v[i], d, path, sink, ctx, depth);
}

/* q is past the '..' */
static void descend(const jval *v, const char *q, const char *path,
		    sink_fn sink, void *ctx, int depth)
{
	const char *end = q + 1;
	bsel *sel;
	dsel d;

	memset(&d, 0, sizeof d);
	if (q[0] == '[' && q[1] == '@') {
		end = bracket_end(q, path);
		d.kind = '@';
		d.rest = stream_path(q + 2, (size_t)(end - (q + 2)), end + 1, path);
	} else if (q[0] == '[' && q[1] == '?') {
		d.kind = '?';
		d.f = path_filter(q, &end);
		d.rest = stream_path(NULL, 0, end, path);
	} else if (q[0] == '[') {
		if (parse_bracket(&end, path, &sel) != 1 || sel->kind == 's')
			die("after '..' a bracket holds one index or name: '%s'",
			    path);
		d.kind = sel->kind;
		d.k = sel->name.p;
		d.klen = sel->name.len;
		d.idx = sel->sl.start;
		d.rest = next_comp(end, path);
	} else {
		d.klen = strcspn(q, ".[");
		if (d.klen == 0)
			die("empty component in path '%s'", path);
		d.k = q;
		d.kind = memchr(q, '+', d.klen) ? 'g' : 'n';
		d.rest = next_comp(q + d.klen, path);
	}
	desc_apply(v, &d, path, sink, ctx, depth);
}

static void walk(const jval *v, const char *q, const char *path,
		 sink_fn sink, void *ctx, int depth)
{
	if (depth > MAXDEPTH)
		die("path nesting too deep");

	if (*q == '.' && q[1] != '.')		/* a.b and .a.b, not ..a */
		q++;
	while (*q) {
		if (q[0] == '.' && q[1] == '.') {
			descend(v, q + 2, path, sink, ctx, depth);
			return;
		}
		if (*q == '[' && q[1] == '?') {
			filter_array(v, q, path, sink, ctx, depth);
			return;
		}
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
		q = next_comp(q, path);
	}
	sink(v, ctx);
}

/*
 * walk() run on the input as it is parsed. A key or a non-negative index
 * steps into one member and skips the rest unbuilt, and [@] treats elements
 * one at a time, so a path costs a tree only for what it must see whole:
 * globs, slices, negative indexes and -r strings. Those, and every error,
 * go to walk() on a tree of just the current value, which keeps both paths
 * one set of semantics.
 */
static void sstep(P *p, const char *q, const char *path, int *nout, int depth);

static void swalk(P *p, const char *q, const char *path, int *nout, int depth)
{
	if (depth > MAXDEPTH)
		die("path nesting too deep");
	sstep(p, *q == '.' && q[1] != '.' ? q + 1 : q, path, nout, depth);
}

static void enter(P *p)
{
	if (++p->depth > MAXDEPTH)
		perr(p, "nesting too deep");
	p->i++;
	skipws(p);
}

static void skey(P *p, const char *k, size_t klen, const char *rest,
		 const char *path, int *nout, int depth)
{
	int found = 0;

	enter(p);
	if (peek(p) == '}')
		p->i++;
	else do {
		amark m = arena_save();
		jstr key = parse_string(p);
		int hit = !found && key.len == klen && memcmp(key.p, k, klen) == 0;

		arena_restore(m);
		skipws(p);
		if (peek(p) != ':')
			perr(p, "expected ':'");
		p->i++;
		if (hit)
			sstep(p, rest, path, nout, depth);
		else
			skip_value(p);
		found |= hit;
	} while (next_member(p, '}'));
	p->depth--;
	if (!found)
		die("no such key: %.*s", (int)klen, k);
}

static void sindex(P *p, size_t idx, const char *rest,
		   const char *path, int *nout, int depth)
{
	size_t n = 0;

	enter(p);
	if (peek(p) == ']')
		p->i++;
	else do {
		if (n++ == idx)
			sstep(p, rest, path, nout, depth);
		else
			skip_value(p);
	} while (next_member(p, ']'));
	p->depth--;
	if (idx >= n)
		die("index %zu out of range (total: %zu)", idx, n);
}

static void sstream(P *p, const char *q, const char *path, int *nout,
		    int depth)
{
	const char *end = bracket_end(q, path), *sub;
	char close = peek(p) == '{' ? '}' : ']';
	amark m;

	sub = stream_path(q + 2, (size_t)(end - (q + 2)), end + 1, path);
	m = arena_save();
	enter(p);
	if (peek(p) == close)
		p->i++;
	else do {
		if (close == '}')
			skip_key(p);
		swalk(p, sub, path, nout, depth + 1);
		arena_restore(m);
	} while (next_member(p, close));
	p->depth--;
}

/* each element needs a tree for the filter to look at, but only one at once */
static void sfilter(P *p, const char *q, const char *path, int *nout,
		    int depth)
{
	const char *end, *sub;
	rexp *f = path_filter(q, &end);
	char close = peek(p) == '{' ? '}' : ']';
	amark m;

	sub = stream_path(NULL, 0, end, path);
	m = arena_save();
	enter(p);
	if (peek(p) == close)
		p->i++;
	else do {
		jval *v;

		if (close == '}')
			skip_key(p);
		v = parse_value(p);
		if (x_true(f, v, doc_root))
			walk(v, sub, path, print_sink, nout, depth + 1);
		arena_restore(m);
	} while (next_member(p, close));
	p->depth--;
}

static void sstep(P *p, const char *q, const char *path, int *nout, int depth)
{
	const char *b = q + 1;
	long long idx;
	bsel *sel;
	int c;

	skipws(p);
	c = peek(p);
	if (!*q && !(opt_raw && c == '"')) {
		if ((*nout)++)
			oc('\n');
		emit_value(p, 0);
		return;
	}
	if ((c == '[' || c == '{') && *q == '[' && q[1] == '@') {
		sstream(p, q, path, nout, depth);
		return;
	}
	if ((c == '[' || c == '{') && *q == '[' && q[1] == '?') {
		if (depth + 1 > MAXDEPTH)
			die("path nesting too deep");
		sfilter(p, q, path, nout, depth);
		return;
	}
	if (c == '[' && *q == '[' && slice_num(&b, &idx) && *b == ']' &&
	    idx >= 0 && (unsigned long long)idx <= SIZE_MAX) {
		sindex(p, (size_t)idx, next_comp(b + 1, path), path, nout, depth);
		return;
	}
	if (c == '{' && *q == '[' && (q[1] == '\'' || q[1] == '"') &&
	    (b = q + 1, parse_bracket(&b, path, &sel) == 1)) {
		skey(p, sel->name.p, sel->name.len, next_comp(b, path), path, nout,
		     depth);
		return;
	}
	if (*q && *q != '[' && *q != '.') {	/* '..' goes to walk() */
		size_t klen = strcspn(q, ".[");

		if (!memchr(q, '+', klen) && (c == '{' || c == '[')) {
			const char *rest = next_comp(q + klen, path);

			if (c == '{')
				skey(p, q, klen, rest, path, nout, depth);
			else
				sindex(p, key_index(q, klen), rest, path, nout, depth);
			return;
		}
	}
	walk(parse_value(p), q, path, print_sink, nout, depth);
}

/*
 * RFC 9535 JSONPath, under --rfc9535. The query compiles first, so a bad
 * one is rejected before any input is read; evaluation never fails. Results
 * reach the sink in RFC order, depth first, which is the same order as
 * applying each segment to the whole nodelist before the next.
 */

typedef struct rre rre;

typedef struct {
	char kind;		/* 'n'ame, '*', 'i'ndex, ':' slice, '?' filter */
	jstr name;
	long long idx;
	jslice sl;
	rexp *f;
} rsel;

typedef struct {
	int desc;		/* .. */
	size_t n;
	rsel *sel;
} rseg;

typedef struct {
	int abs;		/* $, else @ */
	size_t n;
	rseg *seg;
} rquery;

enum { X_OR, X_AND, X_NOT, X_EQ, X_NE, X_LT, X_LE, X_GT, X_GE,
       X_LIT, X_QUERY, X_FN };
enum { T_VALUE, T_LOGICAL, T_NODES };
enum { F_LENGTH, F_COUNT, F_MATCH, F_SEARCH, F_VALUE };

struct rexp {
	int op, type, fn;
	rexp *a, *b;		/* operands, or a function's arguments */
	const jval *lit;
	rquery *q;
	const rre *re;		/* a literal pattern, compiled once */
	int re_lit;		/* 1: re is it, -1: it is not an I-Regexp */
};

typedef struct {
	const char *s;		/* argv, so NUL-terminated and long-lived */
	size_t i, n;
	int depth;
	int path;		/* in a standard path: no match/search */
} Q;

static NORETURN void qerr(Q *q, const char *msg)
{
	if (q->path)
		die("bad path at '%s': %s", q->s + q->i, msg);
	die("invalid query at offset %zu: %s", q->i, msg);
}

static int qc(Q *q, size_t k)
{
	return q->i + k < q->n ? (unsigned char)q->s[q->i + k] : 0;
}

static void qws(Q *q)
{
	while (qc(q, 0) == ' ' || qc(q, 0) == '\t' || qc(q, 0) == '\n' ||
	       qc(q, 0) == '\r')
		q->i++;
}

static int isalpha_(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

#define IJSON_MAX 9007199254740991LL	/* 2^53 - 1: exact in a double */

/* "0" / ["-"] DIGIT1 *DIGIT, in I-JSON range; 0 if there is none */
static int q_int(Q *q, long long *out)
{
	int neg = qc(q, 0) == '-';
	size_t k = neg ? 1 : 0;
	long long v = 0;

	if (!isdig(qc(q, k)))
		return 0;
	if (qc(q, k) == '0' && (k || isdig(qc(q, k + 1))))
		qerr(q, "integers have no leading zero, and no -0");
	for (; isdig(qc(q, k)); k++) {
		v = v * 10 + (qc(q, k) - '0');
		if (v > IJSON_MAX)
			qerr(q, "integer out of range");
	}
	q->i += k;
	*out = neg ? -v : v;
	return 1;
}

/* a quoted string, decoded */
static jstr q_string(Q *q)
{
	char quote = q->s[q->i++];
	jstr out;
	size_t o = 0;

	out.p = arena_alloc(q->n - q->i + 1);	/* never longer decoded */
	for (;;) {
		int c = qc(q, 0);
		unsigned cp, lo;
		size_t w;

		if (c == quote) {
			q->i++;
			break;
		}
		if (q->i >= q->n)
			qerr(q, "unterminated string");
		if (c < 0x20)
			qerr(q, "control character in string");
		if (c != '\\') {
			w = utf8_check(q->s + q->i, q->n - q->i);
			if (!w)
				qerr(q, "invalid UTF-8");
			memcpy(out.p + o, q->s + q->i, w);
			o += w;
			q->i += w;
			continue;
		}
		c = qc(q, 1);
		q->i += 2;
		switch (c) {
		case 'b': cp = '\b'; break;
		case 'f': cp = '\f'; break;
		case 'n': cp = '\n'; break;
		case 'r': cp = '\r'; break;
		case 't': cp = '\t'; break;
		case '/': case '\\': cp = (unsigned)c; break;
		case '\'': case '"':
			if (c != quote)
				qerr(q, "that quote needs no escape");
			cp = (unsigned)c;
			break;
		case 'u':
			if (!hex4(q->s + q->i, q->n - q->i, &cp))
				qerr(q, "bad \\u escape");
			q->i += 4;
			if (cp >= 0xDC00 && cp <= 0xDFFF)
				qerr(q, "lone low surrogate");
			if (cp >= 0xD800 && cp <= 0xDBFF) {
				if (qc(q, 0) != '\\' || qc(q, 1) != 'u' ||
				    !hex4(q->s + q->i + 2, q->n - q->i - 2, &lo) ||
				    lo < 0xDC00 || lo > 0xDFFF)
					qerr(q, "lone high surrogate");
				q->i += 6;
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
			}
			break;
		default:
			q->i -= 2;
			qerr(q, "unknown escape");
		}
		o += utf8_put(out.p + o, cp);
	}
	out.p[o] = '\0';
	out.len = o;
	return out;
}

/* member-name-shorthand: ALPHA, '_', non-ASCII, and digits after the first */
static jstr q_shorthand(Q *q)
{
	size_t start = q->i;
	jstr out;

	for (;;) {
		int c = qc(q, 0);
		size_t w;

		if (isalpha_(c) || c == '_' || (q->i > start && isdig(c))) {
			q->i++;
		} else if (c >= 0x80) {
			if (!(w = utf8_check(q->s + q->i, q->n - q->i)))
				qerr(q, "invalid UTF-8");
			q->i += w;
		} else {
			break;
		}
	}
	if (q->i == start)
		qerr(q, "expected a member name");
	out.len = q->i - start;
	out.p = arena_grow(q->s + start, out.len, out.len, 1);
	return out;
}

static rquery *q_query(Q *q, int abs);
static rexp *x_or(Q *q);

static rexp *xnew(int op, int type, rexp *a, rexp *b)
{
	rexp *e = arena_zalloc(sizeof *e);

	e->op = op;
	e->type = type;
	e->a = a;
	e->b = b;
	return e;
}

static rsel q_selector(Q *q)
{
	rsel s;
	int c = qc(q, 0);
	size_t save;

	memset(&s, 0, sizeof s);
	if (c == '\'' || c == '"') {
		s.kind = 'n';
		s.name = q_string(q);
	} else if (c == '*') {
		s.kind = '*';
		q->i++;
	} else if (c == '?') {
		s.kind = '?';
		q->i++;
		qws(q);
		s.f = x_or(q);
	} else {
		s.sl.has_start = q_int(q, &s.sl.start);
		save = q->i;
		qws(q);
		if (qc(q, 0) != ':') {
			if (!s.sl.has_start)
				qerr(q, "expected a selector");
			q->i = save;
			s.kind = 'i';
			s.idx = s.sl.start;
			return s;
		}
		s.kind = ':';
		q->i++;
		qws(q);
		s.sl.has_end = q_int(q, &s.sl.end);
		save = q->i;
		qws(q);
		if (qc(q, 0) == ':') {
			q->i++;
			qws(q);
			s.sl.has_step = q_int(q, &s.sl.step);
		} else {
			q->i = save;
		}
	}
	return s;
}

static rseg q_segment(Q *q)
{
	rseg g;
	size_t cap = 0;

	memset(&g, 0, sizeof g);
	if (qc(q, 0) == '.') {
		g.desc = qc(q, 1) == '.';
		q->i += g.desc ? 2 : 1;
		if (!g.desc || qc(q, 0) != '[') {
			g.n = 1;
			g.sel = arena_zalloc(sizeof *g.sel);
			if (qc(q, 0) == '*') {
				g.sel->kind = '*';
				q->i++;
			} else {
				g.sel->kind = 'n';
				g.sel->name = q_shorthand(q);
			}
			return g;
		}
	}
	q->i++;					/* '[' */
	qws(q);
	for (;;) {
		if (g.n == cap) {
			cap = cap ? cap * 2 : 4;
			g.sel = arena_grow(g.sel, g.n, cap, sizeof *g.sel);
		}
		g.sel[g.n++] = q_selector(q);
		qws(q);
		if (qc(q, 0) == ']')
			break;
		if (qc(q, 0) != ',')
			qerr(q, "expected ',' or ']'");
		q->i++;
		qws(q);
	}
	q->i++;
	return g;
}

static rquery *q_query(Q *q, int abs)
{
	rquery *r = arena_zalloc(sizeof *r);
	size_t cap = 0;

	if (++q->depth > MAXDEPTH)
		qerr(q, "query nesting too deep");
	r->abs = abs;
	q->i++;					/* '$' or '@' */
	for (;;) {
		size_t save = q->i;

		qws(q);
		if (qc(q, 0) != '.' && qc(q, 0) != '[') {
			q->i = save;
			break;
		}
		if (r->n == cap) {
			cap = cap ? cap * 2 : 4;
			r->seg = arena_grow(r->seg, r->n, cap, sizeof *r->seg);
		}
		r->seg[r->n++] = q_segment(q);
	}
	q->depth--;
	return r;
}

/* at most one node: no '..', and one name or index per segment */
static int singular(const rquery *r)
{
	size_t i;

	for (i = 0; i < r->n; i++)
		if (r->seg[i].desc || r->seg[i].n != 1 ||
		    (r->seg[i].sel->kind != 'n' && r->seg[i].sel->kind != 'i'))
			return 0;
	return 1;
}

static int comparable(const rexp *e)
{
	return e->op == X_LIT || (e->op == X_FN && e->type == T_VALUE) ||
	       (e->op == X_QUERY && singular(e->q));
}

static int testable(const rexp *e)
{
	return e->op == X_QUERY || (e->op == X_FN && e->type != T_VALUE);
}

static const rre *re_compile(const char *s, size_t n);

static rexp *x_atom(Q *q);

static rexp *x_func(Q *q, const char *name, size_t len)
{
	static const struct { const char *name; int fn, type, a, b; } fns[] = {
		{ "length", F_LENGTH, T_VALUE,   T_VALUE, -1 },
		{ "count",  F_COUNT,  T_VALUE,   T_NODES, -1 },
		{ "match",  F_MATCH,  T_LOGICAL, T_VALUE, T_VALUE },
		{ "search", F_SEARCH, T_LOGICAL, T_VALUE, T_VALUE },
		{ "value",  F_VALUE,  T_VALUE,   T_NODES, -1 },
	};
	size_t i, k;
	rexp *e, *arg[2];

	for (i = 0; i < sizeof fns / sizeof *fns; i++)
		if (strlen(fns[i].name) == len && !memcmp(fns[i].name, name, len))
			break;
	if (i == sizeof fns / sizeof *fns)
		qerr(q, "unknown function");
	if (q->path && fns[i].type == T_LOGICAL)
		qerr(q, "match() and search() need --rfc9535");
	q->i++;					/* '(' */
	for (k = 0; k < 2; k++) {
		int want = k ? fns[i].b : fns[i].a;

		if (want < 0)
			break;
		qws(q);
		if (k) {
			if (qc(q, 0) != ',')
				qerr(q, "too few arguments");
			q->i++;
			qws(q);
		}
		arg[k] = x_atom(q);
		if (want == T_VALUE ? !comparable(arg[k]) : arg[k]->op != X_QUERY)
			qerr(q, want == T_VALUE ? "argument must be a single value"
						: "argument must be a query");
	}
	qws(q);
	if (qc(q, 0) != ')')
		qerr(q, "expected ')'");
	q->i++;
	e = xnew(X_FN, fns[i].type, arg[0], k > 1 ? arg[1] : NULL);
	e->fn = fns[i].fn;
	if (k > 1 && arg[1]->op == X_LIT && arg[1]->lit->t == J_STR) {
		e->re = re_compile(arg[1]->lit->u.str.p, arg[1]->lit->u.str.len);
		e->re_lit = e->re ? 1 : -1;
	}
	return e;
}

/* number = (int / "-0") [ "." 1*DIGIT ] [ "e" [ "-" / "+" ] 1*DIGIT ] */
static jval *q_number(Q *q)
{
	size_t k = qc(q, 0) == '-' ? 1 : 0, start = q->i;
	jval *v;

	if (!isdig(qc(q, k)))
		qerr(q, "expected a number");
	if (qc(q, k) == '0' && isdig(qc(q, k + 1)))
		qerr(q, "numbers have no leading zero");
	while (isdig(qc(q, k)))
		k++;
	if (qc(q, k) == '.') {
		if (!isdig(qc(q, ++k)))
			qerr(q, "expected a digit after '.'");
		while (isdig(qc(q, k)))
			k++;
	}
	if (qc(q, k) == 'e' || qc(q, k) == 'E') {
		k++;
		if (qc(q, k) == '+' || qc(q, k) == '-')
			k++;
		if (!isdig(qc(q, k)))
			qerr(q, "expected a digit in the exponent");
		while (isdig(qc(q, k)))
			k++;
	}
	q->i += k;
	v = newval(J_NUM);
	v->u.num.p = q->s + start;
	v->u.num.len = k;
	return v;
}

/* a literal, a query or a function call */
static rexp *x_atom(Q *q)
{
	int c = qc(q, 0);
	rexp *e;

	if (c == '@' || c == '$') {
		e = xnew(X_QUERY, T_NODES, NULL, NULL);
		e->q = q_query(q, c == '$');
		return e;
	}
	e = xnew(X_LIT, T_VALUE, NULL, NULL);
	if (c == '\'' || c == '"') {
		jval *v = newval(J_STR);

		v->u.str = q_string(q);
		e->lit = v;
	} else if (c == '-' || isdig(c)) {
		e->lit = q_number(q);
	} else if (c >= 'a' && c <= 'z') {
		const char *name = q->s + q->i;
		size_t len = 0;

		while ((qc(q, len) >= 'a' && qc(q, len) <= 'z') ||
		       isdig(qc(q, len)) || qc(q, len) == '_')
			len++;
		q->i += len;
		if (qc(q, 0) == '(')
			return x_func(q, name, len);
		if (len == 4 && !memcmp(name, "true", 4))
			e->lit = newval(J_TRUE);
		else if (len == 5 && !memcmp(name, "false", 5))
			e->lit = newval(J_FALSE);
		else if (len == 4 && !memcmp(name, "null", 4))
			e->lit = newval(J_NULL);
		else
			qerr(q, "expected a literal or a function call");
	} else {
		qerr(q, "expected an expression");
	}
	return e;
}

static rexp *x_paren(Q *q)
{
	rexp *e;

	q->i++;
	qws(q);
	e = x_or(q);
	qws(q);
	if (qc(q, 0) != ')')
		qerr(q, "expected ')'");
	q->i++;
	return e;
}

static rexp *x_basic(Q *q)
{
	static const struct { const char *op; int x; } ops[] = {
		{ "==", X_EQ }, { "!=", X_NE }, { "<=", X_LE }, { ">=", X_GE },
		{ "<", X_LT }, { ">", X_GT },
	};
	size_t i, save;
	rexp *e, *r;

	if (qc(q, 0) == '!') {
		q->i++;
		qws(q);
		if (qc(q, 0) == '(')
			return xnew(X_NOT, T_LOGICAL, x_paren(q), NULL);
		e = x_atom(q);
		if (!testable(e))
			qerr(q, "'!' needs a test or a parenthesised expression");
		return xnew(X_NOT, T_LOGICAL, e, NULL);
	}
	if (qc(q, 0) == '(')
		return x_paren(q);
	e = x_atom(q);
	save = q->i;
	qws(q);
	for (i = 0; i < sizeof ops / sizeof *ops; i++)
		if (!strncmp(q->s + q->i, ops[i].op, strlen(ops[i].op)))
			break;
	if (i == sizeof ops / sizeof *ops) {
		q->i = save;
		if (!testable(e))
			qerr(q, "a literal or a value is not a test");
		return e;
	}
	q->i += strlen(ops[i].op);
	qws(q);
	r = x_atom(q);
	if (!comparable(e) || !comparable(r))
		qerr(q, "only single values compare");
	return xnew(ops[i].x, T_LOGICAL, e, r);
}

static rexp *x_and(Q *q)
{
	rexp *e = x_basic(q);

	for (;;) {
		size_t save = q->i;

		qws(q);
		if (qc(q, 0) != '&' || qc(q, 1) != '&') {
			q->i = save;
			return e;
		}
		q->i += 2;
		qws(q);
		e = xnew(X_AND, T_LOGICAL, e, x_basic(q));
	}
}

static rexp *x_or(Q *q)
{
	rexp *e;

	if (++q->depth > MAXDEPTH)
		qerr(q, "filter nesting too deep");
	e = x_and(q);
	for (;;) {
		size_t save = q->i;

		qws(q);
		if (qc(q, 0) != '|' || qc(q, 1) != '|') {
			q->i = save;
			break;
		}
		q->i += 2;
		qws(q);
		e = xnew(X_OR, T_LOGICAL, e, x_and(q));
	}
	q->depth--;
	return e;
}

static rquery *rfc_parse(const char *s)
{
	Q q;
	rquery *r;

	q.s = s;
	q.i = 0;
	q.n = strlen(s);
	q.depth = 0;
	q.path = 0;
	if (qc(&q, 0) != '$')
		qerr(&q, "a query starts with '$'");
	r = q_query(&q, 1);
	if (q.i != q.n)
		qerr(&q, "unexpected character");
	return r;
}

/* the filter in "[?expr]..." at s; *end is past its ']' */
static rexp *path_filter(const char *s, const char **end)
{
	Q q;
	rexp *e;

	q.s = s;
	q.i = 2;
	q.n = strlen(s);
	q.depth = 0;
	q.path = 1;
	qws(&q);
	e = x_or(&q);
	qws(&q);
	if (qc(&q, 0) != ']')
		qerr(&q, "expected ']' after the filter");
	*end = s + q.i + 1;
	return e;
}

/* a quoted name in a standard path's bracket, decoded; *s moves past it */
static jstr path_quoted(const char **s)
{
	Q q;
	jstr k;

	q.s = *s;
	q.i = 0;
	q.n = strlen(*s);
	q.depth = 0;
	q.path = 1;
	k = q_string(&q);
	*s += q.i;
	return k;
}

static int uses_root(const rexp *e);

static int query_uses_root(const rquery *r)
{
	size_t i, j;

	if (r->abs)
		return 1;
	for (i = 0; i < r->n; i++)
		for (j = 0; j < r->seg[i].n; j++)
			if (r->seg[i].sel[j].kind == '?' &&
			    uses_root(r->seg[i].sel[j].f))
				return 1;
	return 0;
}

static int uses_root(const rexp *e)
{
	return e && ((e->op == X_QUERY && query_uses_root(e->q)) ||
		     uses_root(e->a) || uses_root(e->b));
}

/* compile every filter in a path, before input: whether one uses $ */
static int path_uses_root(const char *q)
{
	int root = 0, name = 0;

	while (*q) {
		if (q[0] == '[' && q[1] == '?') {
			root |= uses_root(path_filter(q, &q));
			name = 0;
		} else if (name && (*q == '\'' || *q == '"')) {
			q = skip_quoted(q);
			name = 0;
		} else {
			if (!isblank_(*q))
				name = *q == '[' || *q == ',';
			q++;
		}
	}
	return root;
}

/* exact decimal comparison of two number lexemes: <0, 0 or >0 */
typedef struct {
	const char *d[2];	/* digits: the integer part, then the fraction */
	size_t n[2];
	long long e;		/* value is 0.digits * 10^e */
	int neg;
} rdec;

static void dec_split(const char *s, size_t len, rdec *x)
{
	const char *end = s + len, *t;
	long long ex = 0;
	int eneg = 0;

	x->neg = *s == '-';
	s += x->neg;
	for (t = s; t < end && isdig(*t); t++)
		;
	x->d[0] = s;
	x->n[0] = (size_t)(t - s);
	x->d[1] = t;
	x->n[1] = 0;
	if (t < end && *t == '.') {
		for (x->d[1] = s = ++t; t < end && isdig(*t); t++)
			;
		x->n[1] = (size_t)(t - s);
	}
	if (t < end) {				/* e or E */
		t++;
		if (*t == '+' || *t == '-')
			eneg = *t++ == '-';
		for (; t < end; t++)
			if (ex < (LLONG_MAX / 4 - 9) / 10)	/* saturate */
				ex = ex * 10 + (*t - '0');
	}
	x->e = (eneg ? -ex : ex) + (long long)x->n[0];
	while (x->n[0] && *x->d[0] == '0') {	/* leading zeros */
		x->d[0]++, x->n[0]--, x->e--;
	}
	if (!x->n[0])
		while (x->n[1] && *x->d[1] == '0')
			x->d[1]++, x->n[1]--, x->e--;
	while (x->n[1] && x->d[1][x->n[1] - 1] == '0')	/* trailing zeros */
		x->n[1]--;
	if (!x->n[1])
		while (x->n[0] && x->d[0][x->n[0] - 1] == '0')
			x->n[0]--;
}

static int dec_digit(const rdec *x, size_t k)
{
	return k < x->n[0] ? x->d[0][k] : x->d[1][k - x->n[0]];
}

static int numcmp(const jval *a, const jval *b)
{
	rdec x, y;
	size_t nx, ny, k;
	int sx, sy, r = 0;

	dec_split(a->u.num.p, a->u.num.len, &x);
	dec_split(b->u.num.p, b->u.num.len, &y);
	nx = x.n[0] + x.n[1];
	ny = y.n[0] + y.n[1];
	sx = !nx ? 0 : x.neg ? -1 : 1;		/* -0 is 0 */
	sy = !ny ? 0 : y.neg ? -1 : 1;
	if (sx != sy || !sx)
		return sx - sy;
	if (x.e != y.e)
		r = x.e < y.e ? -1 : 1;
	for (k = 0; !r && k < nx && k < ny; k++)
		r = dec_digit(&x, k) - dec_digit(&y, k);
	if (!r)
		r = nx < ny ? -1 : nx > ny;
	return sx * r;
}

static int jeq(const jval *a, const jval *b)
{
	size_t i, j;

	if (!a || !b)				/* Nothing equals only Nothing */
		return a == b;
	if (a->t != b->t)
		return 0;
	switch (a->t) {
	case J_NUM:
		return numcmp(a, b) == 0;
	case J_STR:
		return a->u.str.len == b->u.str.len &&
		       !memcmp(a->u.str.p, b->u.str.p, a->u.str.len);
	case J_ARR:
	case J_OBJ:
		if (a->u.c.n != b->u.c.n)
			return 0;
		for (i = 0; i < a->u.c.n; i++) {
			if (a->t == J_ARR) {
				if (!jeq(a->u.c.v[i], b->u.c.v[i]))
					return 0;
				continue;
			}
			for (j = 0; j < b->u.c.n; j++)
				if (b->u.c.k[j].len == a->u.c.k[i].len &&
				    !memcmp(b->u.c.k[j].p, a->u.c.k[i].p,
					    a->u.c.k[i].len))
					break;
			if (j == b->u.c.n || !jeq(a->u.c.v[i], b->u.c.v[j]))
				return 0;
		}
		return 1;
	default:
		return 1;
	}
}

/* numbers by value, strings by code point (UTF-8 byte order is the same) */
static int jlt(const jval *a, const jval *b)
{
	size_t n;
	int r;

	if (!a || !b || a->t != b->t)
		return 0;
	if (a->t == J_NUM)
		return numcmp(a, b) < 0;
	if (a->t != J_STR)
		return 0;
	n = a->u.str.len < b->u.str.len ? a->u.str.len : b->u.str.len;
	r = memcmp(a->u.str.p, b->u.str.p, n);
	return r < 0 || (r == 0 && a->u.str.len < b->u.str.len);
}

static void r_apply(const rquery *r, size_t si, const jval *v,
		    const jval *root, sink_fn sink, void *ctx);
static int x_true(const rexp *e, const jval *cur, const jval *root);

static void r_select(const rquery *r, size_t si, const rsel *s,
		     const jval *v, const jval *root, sink_fn sink, void *ctx)
{
	size_t i, n = v->t == J_ARR || v->t == J_OBJ ? v->u.c.n : 0;
	long long idx, first, cnt, k;

	switch (s->kind) {
	case 'n':
		if (v->t != J_OBJ)
			return;
		for (i = 0; i < n; i++)
			if (v->u.c.k[i].len == s->name.len &&
			    !memcmp(v->u.c.k[i].p, s->name.p, s->name.len)) {
				r_apply(r, si + 1, v->u.c.v[i], root, sink, ctx);
				return;
			}
		return;
	case '*':
		for (i = 0; i < n; i++)
			r_apply(r, si + 1, v->u.c.v[i], root, sink, ctx);
		return;
	case 'i':
		idx = s->idx < 0 ? s->idx + (long long)n : s->idx;
		if (v->t == J_ARR && idx >= 0 && idx < (long long)n)
			r_apply(r, si + 1, v->u.c.v[idx], root, sink, ctx);
		return;
	case ':':
		if (v->t != J_ARR || (s->sl.has_step && s->sl.step == 0))
			return;
		cnt = slice_range(&s->sl, (long long)n, &first);
		for (k = 0; k < cnt; k++)
			r_apply(r, si + 1,
				v->u.c.v[first + k * (s->sl.has_step ? s->sl.step : 1)],
				root, sink, ctx);
		return;
	default:
		for (i = 0; i < n; i++) {
			amark m = arena_save();
			int ok = x_true(s->f, v->u.c.v[i], root);

			arena_restore(m);
			if (ok)
				r_apply(r, si + 1, v->u.c.v[i], root, sink, ctx);
		}
	}
}

/* .. applies the segment to v, then to its descendants in document order */
static void r_desc(const rquery *r, size_t si, const jval *v,
		   const jval *root, sink_fn sink, void *ctx)
{
	size_t i;

	for (i = 0; i < r->seg[si].n; i++)
		r_select(r, si, &r->seg[si].sel[i], v, root, sink, ctx);
	if (v->t == J_ARR || v->t == J_OBJ)
		for (i = 0; i < v->u.c.n; i++)
			r_desc(r, si, v->u.c.v[i], root, sink, ctx);
}

static void r_apply(const rquery *r, size_t si, const jval *v,
		    const jval *root, sink_fn sink, void *ctx)
{
	size_t i;

	if (si == r->n) {
		sink(v, ctx);
		return;
	}
	if (r->seg[si].desc) {
		r_desc(r, si, v, root, sink, ctx);
		return;
	}
	for (i = 0; i < r->seg[si].n; i++)
		r_select(r, si, &r->seg[si].sel[i], v, root, sink, ctx);
}

typedef struct {
	size_t n;
	const jval *first;
} rcount;

static void count_sink(const jval *v, void *ctx)
{
	rcount *c = ctx;

	if (!c->n++)
		c->first = v;
}

static rcount x_nodes(const rquery *q, const jval *cur, const jval *root)
{
	rcount c = { 0, NULL };

	r_apply(q, 0, q->abs ? root : cur, root, count_sink, &c);
	return c;
}

static const jval *num_val(size_t n)
{
	char buf[24];
	jval *v = newval(J_NUM);
	int len = snprintf(buf, sizeof buf, "%zu", n);

	v->u.num.p = arena_grow(buf, (size_t)len, (size_t)len, 1);
	v->u.num.len = (size_t)len;
	return v;
}

/* a ValueType result, NULL for Nothing */
static const jval *x_value(const rexp *e, const jval *cur, const jval *root)
{
	const jval *v;
	rcount c;
	size_t i, n = 0;

	if (e->op == X_LIT)
		return e->lit;
	if (e->op == X_QUERY) {
		c = x_nodes(e->q, cur, root);
		return c.n == 1 ? c.first : NULL;
	}
	switch (e->fn) {
	case F_COUNT:
		return num_val(x_nodes(e->a->q, cur, root).n);
	case F_VALUE:
		c = x_nodes(e->a->q, cur, root);
		return c.n == 1 ? c.first : NULL;
	default:				/* F_LENGTH */
		v = x_value(e->a, cur, root);
		if (!v)
			return NULL;
		if (v->t == J_ARR || v->t == J_OBJ)
			return num_val(v->u.c.n);
		if (v->t != J_STR)
			return NULL;
		for (i = 0; i < v->u.str.len; i++)	/* code points */
			n += ((unsigned char)v->u.str.p[i] & 0xC0) != 0x80;
		return num_val(n);
	}
}

static int re_run(const rre *re, const char *s, size_t n, int full);

static int x_true(const rexp *e, const jval *cur, const jval *root)
{
	const jval *a, *b;
	const rre *re;

	switch (e->op) {
	case X_OR:  return x_true(e->a, cur, root) || x_true(e->b, cur, root);
	case X_AND: return x_true(e->a, cur, root) && x_true(e->b, cur, root);
	case X_NOT: return !x_true(e->a, cur, root);
	case X_QUERY: return x_nodes(e->q, cur, root).n > 0;
	case X_FN:				/* match or search */
		a = x_value(e->a, cur, root);
		if (!a || a->t != J_STR || e->re_lit < 0)
			return 0;
		re = e->re;
		if (!re) {
			b = x_value(e->b, cur, root);
			if (!b || b->t != J_STR ||
			    !(re = re_compile(b->u.str.p, b->u.str.len)))
				return 0;
		}
		return re_run(re, a->u.str.p, a->u.str.len, e->fn == F_MATCH);
	}
	a = x_value(e->a, cur, root);
	b = x_value(e->b, cur, root);
	switch (e->op) {
	case X_EQ: return jeq(a, b);
	case X_NE: return !jeq(a, b);
	case X_LT: return jlt(a, b);
	case X_LE: return jlt(a, b) || jeq(a, b);
	case X_GT: return jlt(b, a);
	default:   return jlt(b, a) || jeq(a, b);
	}
}

/*
 * I-Regexp (RFC 9485) for match() and search(): a parse to a tree, then a
 * Pike VM over code points, so time is linear in the input and no pattern
 * can make it backtrack. An invalid pattern is not an error: the function
 * is false. RFC 9485's grammar makes ^ and $ plain characters, but the
 * compliance suite, and every engine it is usually mapped onto, anchors.
 */
#include "unicat.h"

enum ucat { U_Lu, U_Ll, U_Lt, U_Lm, U_Lo, U_Mn, U_Mc, U_Me, U_Nd, U_Nl,
	    U_No, U_Pc, U_Pd, U_Ps, U_Pe, U_Pi, U_Pf, U_Po, U_Sm, U_Sc, U_Sk,
	    U_So, U_Zs, U_Zl, U_Zp, U_Cc, U_Cf, U_Cs, U_Co, U_Cn };

#define RE_MAX 100000			/* instructions, after {n,m} expands */

static int ucat_of(unsigned cp)
{
	size_t lo = 0, hi = sizeof ucat_runs / sizeof *ucat_runs;

	while (hi - lo > 1) {
		size_t mid = lo + (hi - lo) / 2;

		if (ucat_runs[mid] >> 5 <= cp)
			lo = mid;
		else
			hi = mid;
	}
	return (int)(ucat_runs[lo] & 31);
}

typedef struct {
	unsigned lo, hi;
} rrange;

typedef struct {
	int neg;
	size_t n;
	rrange *r;
	uint32_t cats;		/* \p{..}: any of these */
	uint32_t ncats;		/* \P{..}: not all of these (ALL if none) */
} rclass;

typedef struct rnode rnode;
struct rnode {
	char t;			/* 'c'har, 'k' class, '&' sequence, '|', '*' repeat,
				   '^' or '$' */
	unsigned c;
	rclass *k;
	rnode **kid;
	size_t n;
	long long min, max;	/* repeat; max < 0 is unbounded */
};

typedef struct {
	char op;		/* 'c'har, 'k' class, 's'plit, 'j'ump, 'm'atch,
				   '^' or '$' */
	unsigned c;
	const rclass *k;
	int x, y;
} rinst;

struct rre {
	rinst *pc;
	int n;
};

typedef struct {
	const char *s;
	size_t i, n;
	int depth;
} RP;

#define ALLCATS 0x3FFFFFFFu

static int rc(RP *r, size_t k)
{
	return r->i + k < r->n ? (unsigned char)r->s[r->i + k] : -1;
}

static int re_class(const rclass *k, unsigned cp)
{
	size_t i;
	int in = 0, cat;

	for (i = 0; i < k->n && !in; i++)
		in = cp >= k->r[i].lo && cp <= k->r[i].hi;
	if (!in && (k->cats || k->ncats != ALLCATS)) {
		cat = ucat_of(cp);
		in = (k->cats >> cat & 1) || !(k->ncats >> cat & 1);
	}
	return in != k->neg;
}

static void rx_range(rclass *k, unsigned lo, unsigned hi)
{
	k->r = arena_grow(k->r, k->n, k->n + 1, sizeof *k->r);
	k->r[k->n].lo = lo;
	k->r[k->n++].hi = hi;
}

/* \p{..} or \P{..} at the 'p'; 0 if malformed */
static int rx_prop(RP *r, rclass *k)
{
	static const char names[] = "LuLlLtLmLoMnMcMeNdNlNoPcPdPsPePiPfPo"
				    "SmScSkSoZsZlZpCcCfCsCoCn";
	int neg = rc(r, 0) == 'P', major = rc(r, 2), minor = rc(r, 3), i;
	uint32_t mask = 0;

	if (rc(r, 1) != '{')
		return 0;
	r->i += 3;
	if (minor != '}')
		r->i++;
	if (rc(r, 0) != '}')
		return 0;
	r->i++;
	for (i = 0; i < 30; i++)
		if (names[2 * i] == major &&
		    (minor == '}' || (names[2 * i + 1] == minor && i != U_Cs)))
			mask |= (uint32_t)1 << i;
	if (!mask)
		return 0;
	if (neg)
		k->ncats &= mask;
	else
		k->cats |= mask;
	return 1;
}

/* SingleCharEsc after the backslash */
static int rx_single(int c, unsigned *cp)
{
	if (c == 'n' || c == 'r' || c == 't')
		*cp = c == 'n' ? '\n' : c == 'r' ? '\r' : '\t';
	else if (c > 0 && strchr("()*+-.?[\\]^{|}", c))
		*cp = (unsigned)c;
	else
		return 0;
	return 1;
}

static void rx_cp(RP *r, unsigned *cp)
{
	r->i += utf8_get(r->s + r->i, r->n - r->i, cp);
}

/* CCchar: anything but - [ \ ], or a SingleCharEsc */
static int rx_ccchar(RP *r, unsigned *cp)
{
	int c = rc(r, 0);

	if (c < 0 || c == '-' || c == '[' || c == ']')
		return 0;
	if (c == '\\') {
		if (!rx_single(rc(r, 1), cp))
			return 0;
		r->i += 2;
		return 1;
	}
	rx_cp(r, cp);
	return 1;
}

static rnode *rx_new(char t)
{
	rnode *a = arena_zalloc(sizeof *a);

	a->t = t;
	return a;
}

static rnode *rx_klass(void)
{
	rnode *a = rx_new('k');

	a->k = arena_zalloc(sizeof *a->k);
	a->k->ncats = ALLCATS;
	return a;
}

/* [...] after the '[' */
static rnode *rx_bracket(RP *r)
{
	rnode *a = rx_klass();
	unsigned lo, hi;
	int first = 1;

	if (rc(r, 0) == '^') {
		a->k->neg = 1;
		r->i++;
	}
	for (;; first = 0) {
		int c = rc(r, 0);

		if (c == ']' && !first) {
			r->i++;
			return a;
		}
		if (c == '-') {			/* only first or last */
			if (!first && rc(r, 1) != ']')
				return NULL;
			rx_range(a->k, '-', '-');
			r->i++;
			continue;
		}
		if (c == '\\' && (rc(r, 1) == 'p' || rc(r, 1) == 'P')) {
			r->i++;
			if (!rx_prop(r, a->k))
				return NULL;
			continue;
		}
		if (!rx_ccchar(r, &lo))
			return NULL;
		hi = lo;
		if (rc(r, 0) == '-' && rc(r, 1) != ']' && rc(r, 1) >= 0) {
			r->i++;
			if (!rx_ccchar(r, &hi) || hi < lo)
				return NULL;
		}
		rx_range(a->k, lo, hi);
	}
}

static rnode *rx_alt(RP *r);

static rnode *rx_atom(RP *r)
{
	int c = rc(r, 0);
	rnode *a;

	r->i++;
	switch (c) {
	case '(':
		if (++r->depth > MAXDEPTH)
			return NULL;
		a = rx_alt(r);
		r->depth--;
		if (!a || rc(r, 0) != ')')
			return NULL;
		r->i++;
		return a;
	case '[':
		return rx_bracket(r);
	case '.':
		a = rx_klass();
		a->k->neg = 1;
		rx_range(a->k, '\n', '\n');
		rx_range(a->k, '\r', '\r');
		return a;
	case '\\':
		if (rc(r, 0) == 'p' || rc(r, 0) == 'P') {
			a = rx_klass();
			return rx_prop(r, a->k) ? a : NULL;
		}
		a = rx_new('c');
		if (!rx_single(rc(r, 0), &a->c))
			return NULL;
		r->i++;
		return a;
	case '^': case '$':
		return rx_new((char)c);
	case ')': case '*': case '+': case '?': case ']': case '{': case '|':
	case '}':
		return NULL;
	}
	r->i--;
	a = rx_new('c');
	rx_cp(r, &a->c);
	return a;
}

static int rx_count(RP *r, long long *v)
{
	if (!isdig(rc(r, 0)))
		return 0;
	for (*v = 0; isdig(rc(r, 0)); r->i++)
		if (*v <= RE_MAX)
			*v = *v * 10 + (rc(r, 0) - '0');
	return 1;
}

static rnode *rx_branch(RP *r)
{
	rnode *seq = rx_new('&');
	size_t cap = 0;

	for (;;) {
		int c = rc(r, 0);
		rnode *a, *rep;

		if (c < 0 || c == '|' || c == ')')
			return seq;
		if (!(a = rx_atom(r)))
			return NULL;
		c = rc(r, 0);
		if (c == '*' || c == '+' || c == '?' || c == '{') {
			rep = rx_new('*');
			rep->n = 1;
			rep->kid = arena_alloc(sizeof *rep->kid);
			rep->kid[0] = a;
			rep->min = c == '+';
			rep->max = c == '?' ? 1 : -1;
			r->i++;
			if (c == '{') {
				if (!rx_count(r, &rep->min))
					return NULL;
				rep->max = rep->min;
				if (rc(r, 0) == ',') {
					r->i++;
					rep->max = -1;
					if (isdig(rc(r, 0)) && (!rx_count(r, &rep->max) ||
					    rep->max < rep->min))
						return NULL;
				}
				if (rc(r, 0) != '}')
					return NULL;
				r->i++;
			}
			a = rep;
		}
		if (seq->n == cap) {
			cap = cap ? cap * 2 : 8;
			seq->kid = arena_grow(seq->kid, seq->n, cap, sizeof *seq->kid);
		}
		seq->kid[seq->n++] = a;
	}
}

static rnode *rx_alt(RP *r)
{
	rnode *alt = rx_new('|'), *b;
	size_t cap = 0;

	for (;;) {
		if (!(b = rx_branch(r)))
			return NULL;
		if (alt->n == cap) {
			cap = cap ? cap * 2 : 4;
			alt->kid = arena_grow(alt->kid, alt->n, cap, sizeof *alt->kid);
		}
		alt->kid[alt->n++] = b;
		if (rc(r, 0) != '|')
			return alt->n == 1 ? b : alt;
		r->i++;
	}
}

/* instructions a node compiles to, saturating past RE_MAX */
static long long rx_size(const rnode *a)
{
	long long s = 0, k;
	size_t i;

	switch (a->t) {
	case 'c': case 'k': case '^': case '$':
		return 1;
	case '*':
		k = rx_size(a->kid[0]);
		s = a->min * k + (a->max < 0 ? k + 2 : (a->max - a->min) * (k + 1));
		break;
	default:
		for (i = 0; i < a->n && s <= RE_MAX; i++)
			s += rx_size(a->kid[i]);
		if (a->t == '|')
			s += 2 * (long long)(a->n - 1);
	}
	return s > RE_MAX ? RE_MAX + 1 : s;
}

static int rx_emit(rinst *pc, int at, const rnode *a)
{
	size_t i;
	long long k;
	int s, j;

	switch (a->t) {
	case 'c':
		pc[at].op = 'c';
		pc[at].c = a->c;
		return at + 1;
	case 'k':
		pc[at].op = 'k';
		pc[at].k = a->k;
		return at + 1;
	case '^': case '$':
		pc[at].op = a->t;
		return at + 1;
	case '&':
		for (i = 0; i < a->n; i++)
			at = rx_emit(pc, at, a->kid[i]);
		return at;
	case '|':				/* split, x, jump end; split, y, ... */
		for (i = 0, j = -1; i < a->n; i++) {
			s = at;
			if (i + 1 < a->n) {
				pc[s].op = 's';
				pc[s].x = s + 1;
				at++;
			}
			at = rx_emit(pc, at, a->kid[i]);
			if (i + 1 < a->n) {
				pc[at].op = 'j';
				pc[at].x = j;		/* chain, patched below */
				j = at++;
				pc[s].y = at;
			}
		}
		while (j >= 0) {
			s = pc[j].x;
			pc[j].x = at;
			j = s;
		}
		return at;
	}
	for (k = 0; k < a->min; k++)
		at = rx_emit(pc, at, a->kid[0]);
	if (a->max < 0) {			/* L: split M, E; M: x; jump L; E: */
		s = at;
		pc[s].op = 's';
		pc[s].x = s + 1;
		at = rx_emit(pc, s + 1, a->kid[0]);
		pc[at].op = 'j';
		pc[at].x = s;
		pc[s].y = ++at;
		return at;
	}
	for (; k < a->max; k++) {		/* split M, E; M: x; E: */
		s = at;
		pc[s].op = 's';
		pc[s].x = s + 1;
		at = rx_emit(pc, s + 1, a->kid[0]);
		pc[s].y = at;
	}
	return at;
}

static const rre *re_compile(const char *s, size_t n)
{
	RP r;
	rnode *a;
	rre *re;
	long long size;

	r.s = s;
	r.n = n;
	r.i = 0;
	r.depth = 0;
	a = rx_alt(&r);
	if (!a || r.i != n)
		return NULL;
	size = rx_size(a) + 1;
	if (size > RE_MAX)
		die("regular expression too large");
	re = arena_alloc(sizeof *re);
	re->pc = arena_zalloc((size_t)size * sizeof *re->pc);
	re->n = rx_emit(re->pc, 0, a) + 1;
	re->pc[re->n - 1].op = 'm';
	return re;
}

/*
 * add pc and every state it reaches without input, at the start of s and/or
 * the end as given; 1 if one is the match
 */
static int re_add(const rre *re, int *list, int *n, unsigned *mark,
		  unsigned gen, int *stk, int pc, int start, int end)
{
	int sp = 0, hit = 0;

	stk[sp++] = pc;
	while (sp) {
		pc = stk[--sp];
		if (mark[pc] == gen)
			continue;
		mark[pc] = gen;
		switch (re->pc[pc].op) {
		case 'j': stk[sp++] = re->pc[pc].x; break;
		case 's': stk[sp++] = re->pc[pc].y; stk[sp++] = re->pc[pc].x; break;
		case 'm': hit = 1; break;
		case '^': if (start) stk[sp++] = pc + 1; break;
		case '$': if (end) stk[sp++] = pc + 1; break;
		default:  list[(*n)++] = pc;
		}
	}
	return hit;
}

/* full: match() on all of s; else search() for a match anywhere */
static int re_run(const rre *re, const char *s, size_t n, int full)
{
	size_t sz = (size_t)re->n, i = 0;
	int *cur = arena_alloc(sz * sizeof *cur), *nxt = arena_alloc(sz * sizeof *nxt);
	int *stk = arena_alloc((2 * sz + 1) * sizeof *stk), *t, nc = 0, nn, k;
	unsigned *mark = arena_zalloc(sz * sizeof *mark), gen = 1, cp;
	int hit = re_add(re, cur, &nc, mark, gen, stk, 0, 1, n == 0);

	for (;;) {
		if (hit && (!full || i == n))
			return 1;
		if (i == n || (full && !nc))
			return 0;
		i += utf8_get(s + i, n - i, &cp);
		gen++;
		nn = hit = 0;
		for (k = 0; k < nc; k++) {
			const rinst *in = &re->pc[cur[k]];

			if (in->op == 'c' ? in->c == cp : re_class(in->k, cp))
				hit |= re_add(re, nxt, &nn, mark, gen, stk,
					      cur[k] + 1, 0, i == n);
		}
		if (!full)			/* a match may start anywhere */
			hit |= re_add(re, nxt, &nn, mark, gen, stk, 0, 0, i == n);
		t = cur, cur = nxt, nxt = t;
		nc = nn;
	}
}

static void usage(void)
{
	puts("usage: json [options] [path]\n"
"\n"
"Reads JSON on stdin and prints it formatted, or the selected value.\n"
"\n"
"  a.b.0  items[2]  items[-1]  items[1:3]   keys, indexes, Python slices\n"
"  na+                                      '+' globs object keys\n"
"  items[@.name]                            [@] streams elements or values\n"
"  items[?@.n > 1].name                     [?] streams those that match\n"
"  ..id  a..name  ..[?@.x]                  .. searches every depth\n"
"  ['a.b']  items[0,2]  o['x','y']          quoted names; unions, each once\n"
"\n"
"  -r, --raw-output   print a string result without quotes or escapes\n"
"      --raw          alias for --raw-output\n"
"      --rfc9535      the path is RFC 9535 JSONPath: '$.items[*].name';\n"
"                     prints each selected node, and nothing for none\n"
"  -h, --help         this message\n"
"  --                 end of options");
}

int main(int argc, char **argv)
{
	const char *filter = NULL;
	int i, endopts = 0, rfc = 0, nout = 0, whole = 0;
	rquery *rq = NULL;
	jval *root;
	P p;

	simd_init();
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (endopts || a[0] != '-' || a[1] == '\0') {
			if (filter)
				die("only one path may be given");
			filter = a;
		} else if (a[1] != '-') {
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
	if (rfc && filter)
		rq = rfc_parse(filter);
	else if (filter)
		whole = path_uses_root(filter);

	input(&p, 0);
	skipws(&p);
	if (rq) {			/* any node may be compared with any other */
		root = parse_value(&p);
		skipws(&p);
		if (p.i != p.n)
			perr(&p, "trailing data after JSON value");
		r_apply(rq, 0, root, root, print_sink, &nout);
		if (nout)
			oc('\n');
		oflush();
		return 0;
	}

	if (whole) {			/* $ in a filter: everything, as a tree */
		doc_root = parse_value(&p);
		skipws(&p);
		if (p.i != p.n)
			perr(&p, "trailing data after JSON value");
		walk(doc_root, filter, filter, print_sink, &nout, 0);
	} else if (filter)
		swalk(&p, filter, filter, &nout, 0);
	else if (opt_raw && peek(&p) == '"')	/* decoded, so through a tree */
		print_sink(parse_value(&p), &nout);
	else
		emit_value(&p, 0);
	skipws(&p);
	if (p.i != p.n)
		perr(&p, "trailing data after JSON value");
	oc('\n');
	oflush();
	return 0;
}
