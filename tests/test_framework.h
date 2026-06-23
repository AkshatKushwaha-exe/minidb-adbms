// A tiny, dependency-free unit-test framework.
//
// Why hand-roll one instead of using GoogleTest / Catch2?  The lab toolchain
// has no package manager available, and a ~60 line framework keeps the project
// self contained and easy to explain.  It supports test registration, simple
// assertions and per-test failure reporting -- enough for our needs.
//
// Usage:
//   TEST(suite_name, what_it_checks) {
//       CHECK(condition);
//       CHECK_EQ(actual, expected);
//       CHECK_THROWS(some_expr);
//   }
#pragma once

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace minitest {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

// Returns the global list of registered tests (Meyers singleton so that
// static-initialisation order across translation units does not bite us).
std::vector<TestCase>& registry();

// Called by the TEST macro to register a test at static-init time.
int register_test(const std::string& name, std::function<void()> fn);

// Number of failed checks in the *current* test. Reset before each test runs.
extern int g_failures;

}  // namespace minitest

#define TEST(suite, name)                                                      \
    static void minitest_##suite##_##name();                                   \
    static int minitest_reg_##suite##_##name =                                 \
        minitest::register_test(#suite "." #name,                              \
                                minitest_##suite##_##name);                    \
    static void minitest_##suite##_##name()

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++minitest::g_failures;                                            \
            std::cerr << "  CHECK failed: " #cond " (" << __FILE__ << ":"      \
                      << __LINE__ << ")\n";                                    \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        auto _va = (a);                                                        \
        auto _vb = (b);                                                        \
        if (!(_va == _vb)) {                                                   \
            ++minitest::g_failures;                                            \
            std::cerr << "  CHECK_EQ failed: " #a " == " #b " (" << _va        \
                      << " vs " << _vb << ") (" << __FILE__ << ":" << __LINE__ \
                      << ")\n";                                                \
        }                                                                      \
    } while (0)

#define CHECK_THROWS(expr)                                                     \
    do {                                                                       \
        bool _threw = false;                                                   \
        try {                                                                  \
            expr;                                                              \
        } catch (...) {                                                        \
            _threw = true;                                                     \
        }                                                                      \
        if (!_threw) {                                                         \
            ++minitest::g_failures;                                            \
            std::cerr << "  CHECK_THROWS failed: " #expr " did not throw ("    \
                      << __FILE__ << ":" << __LINE__ << ")\n";                 \
        }                                                                      \
    } while (0)
