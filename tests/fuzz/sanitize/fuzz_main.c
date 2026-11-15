/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Replay driver for the F-SANITIZE-* harnesses when libFuzzer is not there (host build,
 * ctest): runs LLVMFuzzerTestOneInput() once for every file named on the command line
 * and for every regular file in every directory named. Exit 0 if no oracle aborted.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define REPLAY_MAX_INPUT (1u << 20)

static unsigned long g_count;

static int run_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	size_t n;

	if (f == NULL) {
		fprintf(stderr, "fuzz_main: cannot open %s\n", path);
		return 1;
	}
	buf = malloc(REPLAY_MAX_INPUT + 1);
	if (buf == NULL)
		abort();
	n = fread(buf, 1, REPLAY_MAX_INPUT + 1, f);
	fclose(f);
	if (n > REPLAY_MAX_INPUT) {
		fprintf(stderr, "fuzz_main: %s over 1 MiB\n", path);
		free(buf);
		return 1;
	}
	(void)LLVMFuzzerTestOneInput(buf, n);
	g_count++;
	free(buf);
	return 0;
}

static int run_path(const char *path)
{
	struct stat st;
	int rc = 0;

	if (stat(path, &st) != 0) {
		fprintf(stderr, "fuzz_main: cannot stat %s\n", path);
		return 1;
	}
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		struct dirent *de;

		if (d == NULL)
			return 1;
		while ((de = readdir(d)) != NULL) {
			char sub[4096];
			struct stat ss;

			if (de->d_name[0] == '.')
				continue;
			if (snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name) >= (int)sizeof(sub))
				continue;
			if (stat(sub, &ss) == 0 && S_ISREG(ss.st_mode))
				rc |= run_file(sub);
		}
		closedir(d);
		return rc;
	}
	return run_file(path);
}

int main(int argc, char **argv)
{
	int i, rc = 0;

	for (i = 1; i < argc; i++)
		rc |= run_path(argv[i]);
	printf("fuzz_main: %lu inputs replayed\n", g_count);
	return rc;
}
