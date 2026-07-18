/**
 * @file test_harness.h
 * @brief ~40 lines of test scaffolding instead of a framework dependency.
 *
 * The whole point of tests/ is that it compiles the *same* source files the
 * firmware links, natively, so the lock-free ring and the wire codec are
 * exercised rather than a reimplementation of them.  Adding a unit-test
 * framework would add a dependency without adding coverage.
 */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        g_checks++;                                                          \
        const long long _a = (long long)(a);                                 \
        const long long _b = (long long)(b);                                 \
        if (_a != _b) {                                                      \
            g_failures++;                                                    \
            printf("  FAIL %s:%d  %s == %s  (%lld != %lld)\n",               \
                   __FILE__, __LINE__, #a, #b, _a, _b);                      \
        }                                                                    \
    } while (0)

#define TEST(name) printf("[test] %s\n", name)

#define TEST_SUMMARY()                                                       \
    do {                                                                     \
        printf("%d checks, %d failures\n", g_checks, g_failures);            \
        return (g_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;              \
    } while (0)

#endif /* TEST_HARNESS_H */
