#ifndef _SWINE_FILEUTIL_H
#define _SWINE_FILEUTIL_H

#include <stdio.h>

// fopen() that takes a UTF-8 path on every platform. On Windows the CRT's
// fopen() interprets paths in the ANSI code page, which breaks for user
// names with non-ASCII characters.
FILE *swine_fopen(const char *path, const char *mode);

#endif
