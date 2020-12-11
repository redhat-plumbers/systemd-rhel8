/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

#include <stdbool.h>
#include <stdio.h>

#include "macro.h"

bool hostname_is_set(void);

char* gethostname_malloc(void);
int gethostname_strict(char **ret);

typedef enum ValidHostnameFlags {
        VALID_HOSTNAME_TRAILING_DOT = 1 << 0,   /* Accept trailing dot on multi-label names */
        VALID_HOSTNAME_DOT_HOST     = 1 << 1,   /* Accept ".host" as valid hostname */
} ValidHostnameFlags;

bool hostname_is_valid(const char *s, ValidHostnameFlags flags) _pure_;
char* hostname_cleanup(char *s);

bool is_localhost(const char *hostname);
bool is_gateway_hostname(const char *hostname);

int sethostname_idempotent(const char *s);

int shorten_overlong(const char *s, char **ret);

int read_etc_hostname_stream(FILE *f, char **ret);
int read_etc_hostname(const char *path, char **ret);
