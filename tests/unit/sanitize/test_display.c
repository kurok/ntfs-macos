/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Path display sanitiser of ADR-023 decision 4 (the sanitiser half of U-VERDICT-05: "a path
 * with ESC or a bidirectional control is shown escaped"). Besides exact cases the test
 * proves the display is lossless: undoing the escapes of the output gives back the input
 * bytes, every escape stands for something the allow-list refuses, and every other output
 * byte belongs to an allowed code point or is a `/`.
 */
#include "sanitize.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "oracle.h"

static void show(const char *in, size_t len, char *out)
{
	size_t n = ntfsmac_path_display(in, len, out, NTFSMAC_PATH_DISPLAY_SIZE);

	CHECK_EQ_INT(strlen(out), n);
}

static void expect(const char *in, size_t len, const char *want)
{
	char out[NTFSMAC_PATH_DISPLAY_SIZE];

	show(in, len, out);
	if (strcmp(out, want) != 0) {
		fprintf(stderr, "FAIL: display of %zu bytes is '%s', want '%s'\n", len, out, want);
		g_failures++;
	}
}

#define EXPECT(in, want) expect((in), sizeof(in) - 1, (want))

/* Undo the escapes; false if the output is not a well-formed display text. */
static bool unescape(const char *out, const unsigned char *in, size_t inlen, unsigned char *back,
		     size_t *backlen)
{
	size_t i = 0, n = 0;
	size_t len = strlen(out);

	while (i < len) {
		unsigned char c = (unsigned char)out[i];

		if (c == '\\') {
			char kind;
			uint32_t v = 0;
			size_t digits = 0;

			if (i + 2 >= len || (out[i + 1] != 'u' && out[i + 1] != 'x') || out[i + 2] != '{')
				return false;
			kind = out[i + 1];
			i += 3;
			while (i < len && out[i] != '}') {
				char d = out[i];

				if (d >= '0' && d <= '9')
					v = v * 16 + (uint32_t)(d - '0');
				else if (d >= 'A' && d <= 'F')
					v = v * 16 + (uint32_t)(d - 'A' + 10);
				else
					return false; /* lower case or junk */
				digits++;
				i++;
			}
			if (i >= len || digits == 0 || digits > 6)
				return false;
			i++; /* } */
			if (digits > 4 && (v >> (4 * (digits - 1))) == 0)
				return false; /* leading zero beyond four digits */
			if (kind == 'x') {
				uint32_t other;

				/* `\x{..}` stands for a byte that really is invalid UTF-8 at this position */
				if (n >= inlen || oracle_decode(in + n, inlen - n, &other) != 0)
					return false;
				if (digits != 2 || v > 0xFF)
					return false;
				back[n++] = (unsigned char)v;
			} else {
				if (digits < 4 || oracle_cp_allowed(v) || v == '/' ||
				    (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF)
					return false; /* an escape only for what the allow-list refuses */
				n += oracle_encode(v, back + n);
			}
		} else {
			uint32_t cp;
			int k = oracle_decode((const unsigned char *)out + i, len - i, &cp);

			if (k == 0 || (cp != '/' && !oracle_cp_allowed(cp)))
				return false;
			memcpy(back + n, out + i, (size_t)k);
			n += (size_t)k;
			i += (size_t)k;
		}
	}
	*backlen = n;
	return true;
}

static void check_lossless(const char *in, size_t len)
{
	char out[NTFSMAC_PATH_DISPLAY_SIZE];
	unsigned char back[NTFSMAC_PATH_DISPLAY_SIZE];
	size_t blen = 0;

	show(in, len, out);
	if (!unescape(out, (const unsigned char *)in, len, back, &blen) || blen != len || memcmp(back, in, len) != 0) {
		fprintf(stderr, "FAIL: display of a %zu byte path is not lossless: '%s'\n", len, out);
		g_failures++;
	}
}

static void cases(void)
{
	EXPECT("/", "/");
	EXPECT("", "");
	EXPECT("/a/b c/d.txt", "/a/b c/d.txt");
	EXPECT("/caf\xC3\xA9/e\xCC\x81", "/caf\xC3\xA9/e\xCC\x81");  /* NFC and NFD both shown */
	EXPECT("/\xF0\x9F\x98\x80", "/\xF0\x9F\x98\x80");
	EXPECT("/.hidden/..", "/.hidden/..");                      /* the file is shown whole */
	EXPECT("/trail. /x  ", "/trail. /x  ");
	/* ESC [ 3 1 m : ESC is a control, `[` is not on the allow-list either */
	EXPECT("/x\x1B[31mred", "/x\\u{001B}\\u{005B}31mred");
	EXPECT("/a,b=c", "/a\\u{002C}b\\u{003D}c");
	EXPECT("/a\\b", "/a\\u{005C}b");                           /* the backslash is escaped */
	EXPECT("/a:b", "/a\\u{003A}b");
	EXPECT("/a\nb\tc\r", "/a\\u{000A}b\\u{0009}c\\u{000D}");
	EXPECT("/x\x7F", "/x\\u{007F}");
	EXPECT("/\xE2\x80\xAEgpj.exe", "/\\u{202E}gpj.exe");      /* RLO */
	EXPECT("/\xC2\x85", "/\\u{0085}");                         /* C1 */
	EXPECT("/\xEF\xBC\x8F", "/\\u{FF0F}");                     /* slash look-alike */
	EXPECT("/\xEE\x80\x80", "/\\u{E000}");
	EXPECT("/\xF3\xB0\x80\x80", "/\\u{F0000}");
	EXPECT("/\xF4\x8F\xBF\xBD", "/\\u{10FFFD}");
	EXPECT("/\xF4\x8F\xBF\xBF", "/\\u{10FFFF}");
	EXPECT("/a\0b", "/a\\u{0000}b");                            /* explicit length: NUL shown */
	EXPECT("/a\xFFz", "/a\\x{FF}z");                            /* invalid byte */
	EXPECT("/\xC0\xAF", "/\\x{C0}\\x{AF}");                     /* overlong slash is not a slash */
	EXPECT("/\xED\xA0\x80", "/\\x{ED}\\x{A0}\\x{80}");          /* surrogate */
	EXPECT("/\xE2\x82", "/\\x{E2}\\x{82}");                     /* truncated */
	EXPECT("//", "//");
}

static void bounds(void)
{
	char out[16];
	size_t n;

	/* cap 0: nothing written, length returned */
	memset(out, 'x', sizeof(out));
	CHECK_EQ_INT(ntfsmac_path_display("/abc", 4, out, 0), 4);
	CHECK(out[0] == 'x');
	CHECK_EQ_INT(ntfsmac_path_display("/abc", 4, NULL, 0), 4);
	CHECK_EQ_INT(ntfsmac_path_display("/a\x1B", 3, NULL, 0), 1 + 1 + 8);
	/* too small: whole elements only, NUL terminated, need reported */
	n = ntfsmac_path_display("/ab\x1B" "cd", 6, out, 6); /* room for 5 bytes */
	CHECK_EQ_INT(n, 3 + 8 + 2);
	CHECK(strcmp(out, "/ab") == 0);
	n = ntfsmac_path_display("/ab", 3, out, 3);
	CHECK_EQ_INT(n, 3);
	CHECK(strcmp(out, "/a") == 0);
	/* the documented size always suffices: 512 control bytes */
	{
		char in[512], big[NTFSMAC_PATH_DISPLAY_SIZE];

		memset(in, 0x01, sizeof(in));
		n = ntfsmac_path_display(in, sizeof(in), big, sizeof(big));
		CHECK_EQ_INT(n, 512 * 8);
		CHECK_EQ_INT(strlen(big), 512 * 8);
		memset(in, '\xFF', sizeof(in));
		n = ntfsmac_path_display(in, sizeof(in), big, sizeof(big));
		CHECK_EQ_INT(n, 512 * 6);
	}
}

/* xorshift: deterministic, no libc state */
static uint64_t rng(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static void lossless(void)
{
	uint64_t s = UINT64_C(0x9E3779B97F4A7C15);
	uint32_t cp;
	int round;

	/* every code point, alone and between ASCII */
	for (cp = 0; cp <= 0x10FFFF; cp++) {
		unsigned char in[6];
		size_t n;

		if (cp >= 0xD800 && cp <= 0xDFFF)
			continue;
		in[0] = '/';
		n = oracle_encode(cp, in + 1);
		check_lossless((const char *)in, n + 1);
	}
	/* random byte strings up to the 512 byte cap, plain and biased to UTF-8 lead bytes */
	for (round = 0; round < 20000; round++) {
		unsigned char in[512];
		size_t len = (size_t)(rng(&s) % 513), i;

		for (i = 0; i < len; i++) {
			uint64_t r = rng(&s);

			if (round % 2 == 0)
				in[i] = (unsigned char)r;
			else {
				static const unsigned char pick[] = {
					'/', 'a', ' ', '.', 0x1B, 0x00, '\\', 0xC2, 0xE2, 0xEF, 0xF0, 0xF4,
					0x80, 0xBF, 0x85, 0xAE, 0x8F, 0xBD, 0xED, 0xA0,
				};

				in[i] = pick[r % sizeof(pick)];
			}
		}
		check_lossless((const char *)in, len);
	}
}

int main(void)
{
	cases();
	bounds();
	lossless();
	if (g_failures != 0) {
		fprintf(stderr, "test_display: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_display: ok\n");
	return 0;
}
