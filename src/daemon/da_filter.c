/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pure helpers of the Disk Arbitration module. See da_filter.h. */
#include "da_filter.h"

#include <string.h>

#define GPT_BASIC_DATA "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
#define MBR_NTFS "Windows_NTFS"
#define NUM_MAX 6

static bool equal_nocase(const char *a, const char *b)
{
	for (; *a != '\0' && *b != '\0'; a++, b++) {
		char x = *a, y = *b;

		if (x >= 'a' && x <= 'z')
			x = (char)(x - 'a' + 'A');
		if (y >= 'a' && y <= 'z')
			y = (char)(y - 'a' + 'A');
		if (x != y)
			return false;
	}
	return *a == *b;
}

static size_t digits(const char *s)
{
	size_t n = 0;

	while (s[n] >= '0' && s[n] <= '9')
		n++;
	return n;
}

bool ntfsmac_da_bsd_name_valid(const char *name)
{
	size_t n;

	if (name == NULL || strncmp(name, "disk", 4) != 0)
		return false;
	name += 4;
	n = digits(name);
	if (n == 0 || n > NUM_MAX)
		return false;
	name += n;
	if (*name == '\0')
		return true;
	if (*name != 's')
		return false;
	name++;
	n = digits(name);
	return n != 0 && n <= NUM_MAX && name[n] == '\0';
}

bool ntfsmac_da_content_is_candidate(const char *content, bool whole)
{
	if (content == NULL || content[0] == '\0')
		return whole;
	return equal_nocase(content, MBR_NTFS) || equal_nocase(content, GPT_BASIC_DATA);
}

void ntfsmac_da_table_init(ntfsmac_da_table *t)
{
	memset(t, 0, sizeof *t);
}

static int find(const ntfsmac_da_table *t, const char *name)
{
	unsigned i;

	if (name == NULL)
		return -1;
	for (i = 0; i < t->count; i++)
		if (strcmp(t->names[i], name) == 0)
			return (int)i;
	return -1;
}

bool ntfsmac_da_table_add(ntfsmac_da_table *t, const char *name)
{
	if (!ntfsmac_da_bsd_name_valid(name) || strlen(name) >= NTFSMAC_DA_NAME_MAX)
		return false;
	if (find(t, name) >= 0)
		return true;
	if (t->count >= NTFSMAC_DA_TABLE_MAX)
		return false;
	strcpy(t->names[t->count], name);
	t->flags[t->count] = false;
	t->count++;
	return true;
}

bool ntfsmac_da_table_remove(ntfsmac_da_table *t, const char *name, bool *flag)
{
	int i = find(t, name);

	if (i < 0)
		return false;
	if (flag != NULL)
		*flag = t->flags[i];
	t->count--;
	if ((unsigned)i != t->count) {
		memcpy(t->names[i], t->names[t->count], NTFSMAC_DA_NAME_MAX);
		t->flags[i] = t->flags[t->count];
	}
	t->names[t->count][0] = '\0';
	t->flags[t->count] = false;
	return true;
}

bool ntfsmac_da_table_contains(const ntfsmac_da_table *t, const char *name)
{
	return find(t, name) >= 0;
}

bool ntfsmac_da_table_set_flag(ntfsmac_da_table *t, const char *name, bool flag)
{
	int i = find(t, name);

	if (i < 0)
		return false;
	t->flags[i] = flag;
	return true;
}

bool ntfsmac_da_table_get_flag(const ntfsmac_da_table *t, const char *name, bool *flag)
{
	int i = find(t, name);

	if (i < 0)
		return false;
	*flag = t->flags[i];
	return true;
}
