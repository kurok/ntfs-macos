/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_CLI_JSON_H
#define NTFSMAC_CLI_JSON_H

/*
 * Small JSON reader for the daemon's replies (T-M4-04). It builds a tree in
 * one block of memory, limits depth and node count, and never recurses
 * deeper than JSON_DEPTH_MAX. It is for replies from our own daemon, but it
 * is written as if the input were hostile.
 */

#include <stdbool.h>
#include <stddef.h>

typedef enum json_type {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUMBER, /* kept as text */
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT
} json_type;

#define JSON_DEPTH_MAX 12u
#define JSON_NODES_MAX 4096u

typedef struct json_node {
	json_type type;
	const char *key;   /* member name inside an object, else NULL */
	const char *text;  /* string value or number text; NULL otherwise */
	bool boolean;
	struct json_node *first; /* first child of an array or object */
	struct json_node *next;  /* next sibling */
} json_node;

typedef struct json_doc json_doc;

/* NULL on any syntax error, trailing bytes, depth or size limit. */
json_doc *json_parse(const char *text, size_t len);
const json_node *json_root(const json_doc *d);
void json_free(json_doc *d);

/* Member of an object by name, or NULL. */
const json_node *json_member(const json_node *obj, const char *key);
/* String value of a member, or NULL if absent or not a string. */
const char *json_member_str(const json_node *obj, const char *key);

#endif
