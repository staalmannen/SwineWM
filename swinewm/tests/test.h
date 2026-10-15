#ifndef _SWINE_TEST_H
#define _SWINE_TEST_H

#include <stdio.h>
#include <string.h>

// Tiny assertion harness: no dependencies, works with MSVC and GCC.
extern int test_failures;
extern int test_checks;

#define CHECK(cond) do { \
	test_checks++; \
	if (!(cond)) { \
		test_failures++; \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

#define CHECK_STR(a, b) do { \
	const char *_a = (a), *_b = (b); \
	test_checks++; \
	if (!_a || !_b || strcmp(_a, _b) != 0) { \
		test_failures++; \
		fprintf(stderr, "%s:%d: CHECK_STR failed: \"%s\" != \"%s\"\n", \
			__FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); \
	} \
} while (0)

#define CHECK_INT(a, b) do { \
	long long _a = (long long)(a), _b = (long long)(b); \
	test_checks++; \
	if (_a != _b) { \
		test_failures++; \
		fprintf(stderr, "%s:%d: CHECK_INT failed: %s == %lld, expected %lld\n", \
			__FILE__, __LINE__, #a, _a, _b); \
	} \
} while (0)

typedef void (*test_fn)(void);

#endif
