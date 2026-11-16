/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-CTL-PARSE-01..04: the strict request parser (ctl_parse.c).
 *   01 every command parses to the fields of section 3.4
 *   02 every kind of malformed JSON is BAD_JSON or BAD_FIELD, never OK
 *   03 oversize lines, unknown commands, misplaced and malformed fields
 *   04 escapes, UTF-8 and the wire-name tables
 *   05 control code points (C0, DEL, C1) in any string value
 */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "ctl_priv.h"

#define GOOD_AUTH_P "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
#define P(s) ntfsmac_ctl_parse_request((s), strlen(s), &r)

static void test_valid(void)
{
	ntfsmac_ctl_request r;

	CHECK_EQ_INT(P("{\"cmd\":\"list\"}"), NTFSMAC_CTL_PARSE_OK);
	CHECK_EQ_INT(r.cmd, NTFSMAC_CTL_CMD_LIST);
	CHECK_EQ_INT(P("  {  \"cmd\" : \"doctor\" }  \t"), NTFSMAC_CTL_PARSE_OK);
	CHECK_EQ_INT(r.cmd, NTFSMAC_CTL_CMD_DOCTOR);

	CHECK_EQ_INT(P("{\"cmd\":\"status\",\"bsd\":\"disk4s1\"}"), NTFSMAC_CTL_PARSE_OK);
	CHECK_EQ_INT(r.cmd, NTFSMAC_CTL_CMD_STATUS);
	CHECK(strcmp(r.bsd, "disk4s1") == 0);

	/* key order does not matter */
	CHECK_EQ_INT(P("{\"bsd\":\"disk12\",\"cmd\":\"mount\",\"mode\":\"ro\"}"), NTFSMAC_CTL_PARSE_OK);
	CHECK_EQ_INT(r.cmd, NTFSMAC_CTL_CMD_MOUNT);
	CHECK(r.read_only);
	CHECK_EQ_INT(P("{\"cmd\":\"mount\",\"bsd\":\"disk12\",\"mode\":\"auto\"}"), NTFSMAC_CTL_PARSE_OK);
	CHECK(!r.read_only);
	CHECK_EQ_INT(P("{\"cmd\":\"mount\",\"bsd\":\"disk12\"}"), NTFSMAC_CTL_PARSE_OK);
	CHECK(!r.read_only);

	CHECK_EQ_INT(P("{\"cmd\":\"unmount\",\"bsd\":\"disk3s2\",\"force\":true}"), NTFSMAC_CTL_PARSE_OK);
	CHECK(r.force);
	CHECK_EQ_INT(P("{\"cmd\":\"unmount\",\"bsd\":\"disk3s2\"}"), NTFSMAC_CTL_PARSE_OK);
	CHECK(!r.force);

	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk3s2\",\"confirm_label\":\"My Disk\","
		       "\"confirm_serial\":\"0a1B2c3D\",\"auth\":\"" GOOD_AUTH_P "\"}"),
		     NTFSMAC_CTL_PARSE_OK);
	CHECK_EQ_INT(r.cmd, NTFSMAC_CTL_CMD_JOURNAL_CLEAR);
	CHECK(strcmp(r.confirm_label, "My Disk") == 0);
	CHECK(strcmp(r.confirm_serial, "0A1B2C3D") == 0); /* normalised to upper case */
	CHECK(r.has_auth && r.auth[0] == 0x00 && r.auth[1] == 0x11 && r.auth[31] == 0xff);

	/* `auth` may be absent: check 4 refuses it, the parser does not */
	CHECK_EQ_INT(P("{\"cmd\":\"repair\",\"bsd\":\"disk3s2\",\"action\":\"sync_mirror\","
		       "\"confirm_label\":\"X\",\"confirm_serial\":\"00000000\"}"),
		     NTFSMAC_CTL_PARSE_OK);
	CHECK(!r.has_auth);
	CHECK_EQ_INT(r.action, NTFSMAC_CTL_REPAIR_SYNC_MIRROR);
	CHECK_EQ_INT(P("{\"cmd\":\"repair\",\"bsd\":\"disk3s2\",\"action\":\"clear_dirty\","
		       "\"confirm_label\":\"X\",\"confirm_serial\":\"00000000\"}"),
		     NTFSMAC_CTL_PARSE_OK);
	CHECK_EQ_INT(r.action, NTFSMAC_CTL_REPAIR_CLEAR_DIRTY);
}

static void test_bad_json(void)
{
	static const char *const bad[] = {
		"", " ", "{", "}", "[]", "null", "true", "\"cmd\"", "12", "{\"cmd\"", "{\"cmd\":",
		"{\"cmd\":\"list\"", "{\"cmd\":\"list\",}", "{,\"cmd\":\"list\"}",
		"{\"cmd\":\"list\"}}", "{\"cmd\":\"list\"} x", "{\"cmd\":\"list\"}{\"cmd\":\"list\"}",
		"{cmd:\"list\"}", "{'cmd':'list'}", "{\"cmd\" \"list\"}", "{\"cmd\":\"list\" \"bsd\":\"x\"}",
		"{\"cmd\":\"li", "{\"cmd\":\"list\\", "{\"cmd\":\"l\\x\"}", "{\"cmd\":\"l\\u12\"}",
		"{\"cmd\":\"l\\uZZZZ\"}", "{\"cmd\":\"\\ud800\"}", "{\"cmd\":\"\\udc00\"}",
		"{\"cmd\":\"\\ud800\\u0041\"}", "{\"cmd\":\"\\u0000\"}",
		"{\"cmd\":\"lis\x01t\"}", "{\"cmd\":\"list\n\"}", "{\"cmd\":\"\xc3\"}", "{\"cmd\":\"\xff\"}",
		"{\"cmd\":\"\xc0\x80\"}", "{\"cmd\":\"\xed\xa0\x80\"}", "{\"cmd\":\"\xf4\x90\x80\x80\"}",
		"{\"cmd\":\"\xe0\x80\x80\"}", "\xef\xbb\xbf{\"cmd\":\"list\"}",
		"{\"force\":tru}", "{\"force\":fals}", "{\"force\":True}", "{\"cmd\":\"list\"} //",
	};
	size_t i;

	for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		ntfsmac_ctl_request r;
		ntfsmac_ctl_parse_result res = ntfsmac_ctl_parse_request(bad[i], strlen(bad[i]), &r);

		if (res == NTFSMAC_CTL_PARSE_OK)
			fprintf(stderr, "accepted bad input #%zu: %s\n", i, bad[i]);
		CHECK(res != NTFSMAC_CTL_PARSE_OK);
	}
	/* those that are plain syntax errors */
	{
		ntfsmac_ctl_request r;

		CHECK_EQ_INT(P(""), NTFSMAC_CTL_PARSE_BAD_JSON);
		CHECK_EQ_INT(P("[]"), NTFSMAC_CTL_PARSE_BAD_JSON);
		CHECK_EQ_INT(P("{\"cmd\":\"list\"} x"), NTFSMAC_CTL_PARSE_BAD_JSON);
		CHECK_EQ_INT(P("{\"cmd\":\"list\",}"), NTFSMAC_CTL_PARSE_BAD_JSON);
		CHECK_EQ_INT(P("{\"cmd\":\"\xff\"}"), NTFSMAC_CTL_PARSE_BAD_JSON);
	}
	/* an embedded NUL ends no string and is not accepted anywhere */
	{
		ntfsmac_ctl_request r;
		const char nul1[] = "{\"cmd\":\"list\"}\0";
		const char nul2[] = "{\"cmd\":\"li\0st\"}";

		CHECK_EQ_INT(ntfsmac_ctl_parse_request(nul1, sizeof nul1 - 1, &r), NTFSMAC_CTL_PARSE_BAD_JSON);
		CHECK_EQ_INT(ntfsmac_ctl_parse_request(nul2, sizeof nul2 - 1, &r), NTFSMAC_CTL_PARSE_BAD_JSON);
	}
}

static void test_fields(void)
{
	ntfsmac_ctl_request r;
	char big[NTFSMAC_CTL_LINE_MAX + 64];
	size_t n;

	/* too long */
	memset(big, 'a', sizeof big);
	CHECK_EQ_INT(ntfsmac_ctl_parse_request(big, NTFSMAC_CTL_LINE_MAX + 1, &r), NTFSMAC_CTL_PARSE_TOO_LONG);
	CHECK_EQ_INT(ntfsmac_ctl_parse_request(big, sizeof big, &r), NTFSMAC_CTL_PARSE_TOO_LONG);
	/* exactly the limit is judged by its content, not by its length */
	CHECK_EQ_INT(ntfsmac_ctl_parse_request(big, NTFSMAC_CTL_LINE_MAX, &r), NTFSMAC_CTL_PARSE_BAD_JSON);
	CHECK_EQ_INT(ntfsmac_ctl_parse_request(NULL, 0, &r), NTFSMAC_CTL_PARSE_BAD_JSON);
	CHECK_EQ_INT(ntfsmac_ctl_parse_request("{}", 2, NULL), NTFSMAC_CTL_PARSE_BAD_JSON);

	/* a very long value for a field with a limit */
	n = (size_t)snprintf(big, sizeof big, "{\"cmd\":\"status\",\"bsd\":\"disk");
	memset(big + n, '1', 3000);
	strcpy(big + n + 3000, "\"}");
	CHECK_EQ_INT(ntfsmac_ctl_parse_request(big, strlen(big), &r), NTFSMAC_CTL_PARSE_BAD_FIELD);

	/* commands */
	CHECK_EQ_INT(P("{}"), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
	CHECK_EQ_INT(P("{\"bsd\":\"disk1\"}"), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
	CHECK_EQ_INT(P("{\"cmd\":\"reboot\"}"), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
	CHECK_EQ_INT(P("{\"cmd\":\"LIST\"}"), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
	CHECK_EQ_INT(P("{\"cmd\":\"journal-clear\"}"), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
	CHECK_EQ_INT(P("{\"cmd\":\"\"}"), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
	CHECK_EQ_INT(P("{\"cmd\":\"this_command_name_is_far_too_long\"}"), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);

	/* unknown, duplicate, misplaced and missing fields */
	CHECK_EQ_INT(P("{\"cmd\":\"list\",\"x\":\"y\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"list\",\"bsd\":\"disk1\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"list\",\"cmd\":\"list\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"status\",\"bsd\":\"disk1\",\"bsd\":\"disk2\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"status\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"status\",\"bsd\":\"disk1\",\"force\":true}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"unmount\",\"bsd\":\"disk1\",\"mode\":\"ro\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"mount\",\"bsd\":\"disk1\",\"auth\":\"" GOOD_AUTH_P "\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"repair\",\"bsd\":\"disk1\",\"confirm_label\":\"x\","
		       "\"confirm_serial\":\"00000000\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD); /* no action */
	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\",\"confirm_label\":\"x\","
		       "\"confirm_serial\":\"00000000\",\"action\":\"clear_dirty\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);

	/* wrong value types */
	CHECK_EQ_INT(P("{\"cmd\":\"status\",\"bsd\":5}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"status\",\"bsd\":null}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"status\",\"bsd\":[\"disk1\"]}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"status\",\"bsd\":{}}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":5}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"unmount\",\"bsd\":\"disk1\",\"force\":\"true\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"unmount\",\"bsd\":\"disk1\",\"force\":1}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"unmount\",\"bsd\":\"disk1\",\"force\":null}"), NTFSMAC_CTL_PARSE_BAD_FIELD);

	/* field values */
	CHECK_EQ_INT(P("{\"cmd\":\"mount\",\"bsd\":\"disk1\",\"mode\":\"rw\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"mount\",\"bsd\":\"disk1\",\"mode\":\"RO\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"repair\",\"bsd\":\"disk1\",\"action\":\"format\",\"confirm_label\":\"x\","
		       "\"confirm_serial\":\"00000000\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\",\"confirm_label\":\"\","
		       "\"confirm_serial\":\"00000000\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\",\"confirm_label\":\"x\","
		       "\"confirm_serial\":\"0000000\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\",\"confirm_label\":\"x\","
		       "\"confirm_serial\":\"000000000\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\",\"confirm_label\":\"x\","
		       "\"confirm_serial\":\"0000000G\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\",\"confirm_label\":\"x\","
		       "\"confirm_serial\":\"00000000\",\"auth\":\"abcd\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\",\"confirm_label\":\"x\","
		       "\"confirm_serial\":\"00000000\",\"auth\":\"" GOOD_AUTH_P "00\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\",\"confirm_label\":\"x\","
		       "\"confirm_serial\":\"00000000\",\"auth\":\"g011223344556677889900aabbccddeeff"
		       "00112233445566778899aabbccddee\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	/* a label of 255 bytes is the longest */
	{
		char line[600];

		n = (size_t)snprintf(line, sizeof line, "{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\","
				     "\"confirm_serial\":\"00000000\",\"confirm_label\":\"");
		memset(line + n, 'a', 255);
		strcpy(line + n + 255, "\"}");
		CHECK_EQ_INT(ntfsmac_ctl_parse_request(line, strlen(line), &r), NTFSMAC_CTL_PARSE_OK);
		CHECK_EQ_INT(strlen(r.confirm_label), 255);
		strcpy(line + n + 256 - 1, "a\"}");
		CHECK_EQ_INT(ntfsmac_ctl_parse_request(line, strlen(line), &r), NTFSMAC_CTL_PARSE_BAD_FIELD);
	}
}

static void test_bsd(void)
{
	static const char *const good[] = {"disk0", "disk4", "disk4s1", "disk123s456", "disk999999999"};
	static const char *const bad[] = {"", "disk", "disks1", "disk4s", "disk4s1s", "disk4s1s2", "Disk4",
					  "/dev/disk4", "../disk4", "disk4 ", " disk4", "disk4\n", "disk-1",
					  "rdisk4", "disk4s1/", "disk4/../x", "disk1234567890", "disk4s1234567890",
					  "disk٣", "disk4;ls", "disk4\"", "diskx"};
	size_t i;

	for (i = 0; i < sizeof good / sizeof good[0]; i++)
		CHECK(ntfsmac_ctl_bsd_valid(good[i]));
	for (i = 0; i < sizeof bad / sizeof bad[0]; i++)
		CHECK(!ntfsmac_ctl_bsd_valid(bad[i]));
	CHECK(!ntfsmac_ctl_bsd_valid(NULL));
}

static void test_escapes(void)
{
	ntfsmac_ctl_request r;

	/* escapes decode; UTF-8 passes; the key may use an escape too */
	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\",\"confirm_serial\":\"00000000\","
		       "\"confirm_label\":\"a\\\"b\\\\c\\/d\\u00e9\\ud83d\\ude00 \xc3\xa9\"}"),
		     NTFSMAC_CTL_PARSE_OK);
	CHECK(strcmp(r.confirm_label, "a\"b\\c/d\xc3\xa9\xf0\x9f\x98\x80 \xc3\xa9") == 0);
	CHECK_EQ_INT(P("{\"c\\u006dd\":\"li\\u0073t\"}"), NTFSMAC_CTL_PARSE_OK);
	CHECK_EQ_INT(r.cmd, NTFSMAC_CTL_CMD_LIST);
	/* a label can hold no control code point, not even through an escape (see test_control) */
	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\",\"confirm_serial\":\"00000000\","
		       "\"confirm_label\":\"a\\nb\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
}

/* U-CTL-PARSE-05: control code points are refused in every string value. */
static void test_control(void)
{
	ntfsmac_ctl_request r;
	static const char *const bad[] = {
		"a\\u0001b", "a\\u001fb", "a\\tb", "a\\rb", "a\\nb", "a\\bb", "a\\fb",
		"a\\u007fb",                    /* DEL */
		"a\\u0080b", "a\\u009bb", "a\\u009fb", /* C1, escaped */
		("a\x7f" "b"),                   /* DEL, raw */
		("a\xc2\x80" "b"), ("a\xc2\x9b" "b"), ("a\xc2\x9f" "b"), /* C1, raw */
		"\\u001b[31m",                   /* escape sequence */
	};
	static const char *const good[] = {
		"a b", "caf\xc3\xa9",
		"a\\u00a0b",   /* U+00A0, first code point after C1 */
		("a\xc2\xa0" "b"),
		"a\\u0020b",   /* a space is not a control */
		"a\\u007eb",   /* tilde */
	};
	char line[300];
	unsigned i;

	for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		snprintf(line, sizeof line, "{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\","
			 "\"confirm_serial\":\"00000000\",\"confirm_label\":\"%s\"}", bad[i]);
		if (ntfsmac_ctl_parse_request(line, strlen(line), &r) != NTFSMAC_CTL_PARSE_BAD_FIELD)
			fprintf(stderr, "control value %u accepted\n", i);
		CHECK_EQ_INT(ntfsmac_ctl_parse_request(line, strlen(line), &r), NTFSMAC_CTL_PARSE_BAD_FIELD);
		CHECK_EQ_INT(r.cmd, 0);
		CHECK_EQ_INT(r.confirm_label[0], 0); /* nothing is handed on */
	}
	for (i = 0; i < sizeof good / sizeof good[0]; i++) {
		snprintf(line, sizeof line, "{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\","
			 "\"confirm_serial\":\"00000000\",\"confirm_label\":\"%s\"}", good[i]);
		CHECK_EQ_INT(ntfsmac_ctl_parse_request(line, strlen(line), &r), NTFSMAC_CTL_PARSE_OK);
	}
	/* the other string fields too: a control code point is a bad field, not a mismatch */
	CHECK_EQ_INT(P("{\"cmd\":\"status\",\"bsd\":\"disk1\\u0001\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"mount\",\"bsd\":\"disk1\",\"mode\":\"ro\\n\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"repair\",\"bsd\":\"disk1\",\"action\":\"clear_dirty\\u009b\","
		       "\"confirm_label\":\"x\",\"confirm_serial\":\"00000000\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\",\"confirm_label\":\"x\","
		       "\"confirm_serial\":\"0000000\\u007f\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(P("{\"cmd\":\"journal_clear\",\"bsd\":\"disk1\",\"confirm_label\":\"x\","
		       "\"confirm_serial\":\"00000000\",\"auth\":\"\\u0001\"}"), NTFSMAC_CTL_PARSE_BAD_FIELD);
	/* a command name with a control code point is still just an unknown command */
	CHECK_EQ_INT(P("{\"cmd\":\"list\\n\"}"), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
}

static void test_names(void)
{
	CHECK(strcmp(ntfsmac_ctl_cmd_name(NTFSMAC_CTL_CMD_JOURNAL_CLEAR), "journal_clear") == 0);
	CHECK(strcmp(ntfsmac_ctl_cmd_name(NTFSMAC_CTL_CMD_COUNT_), "?") == 0);
	CHECK(strcmp(ntfsmac_ctl_error_name(NTFSMAC_CTL_ERR_BAD_JSON), "bad_json") == 0);
	CHECK(strcmp(ntfsmac_ctl_error_name((ntfsmac_ctl_error)99), "?") == 0);
	CHECK(strcmp(ntfsmac_ctl_refusal_name(NTFSMAC_CTL_REFUSE_SPAWNED_PROCESS), "spawned_process") == 0);
	CHECK(strcmp(ntfsmac_ctl_refusal_name((ntfsmac_ctl_refusal)-1), "?") == 0);
}

int main(void)
{
	test_valid();
	test_bad_json();
	test_fields();
	test_bsd();
	test_escapes();
	test_control();
	test_names();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
