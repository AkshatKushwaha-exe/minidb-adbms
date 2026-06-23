// Implementation + entry point for the tiny test framework (see header).
#include "test_framework.h"

namespace minitest {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

int register_test(const std::string& name, std::function<void()> fn) {
    registry().push_back({name, std::move(fn)});
    return 0;
}

int g_failures = 0;

}  // namespace minitest

int main(int argc, char** argv) {
    // Optional substring filter: `./test_runner storage` runs only tests whose
    // name contains "storage".
    std::string filter = (argc > 1) ? argv[1] : "";

    int passed = 0;
    int failed = 0;
    for (auto& test : minitest::registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) {
            continue;
        }
        minitest::g_failures = 0;
        try {
            test.fn();
        } catch (const std::exception& e) {
            ++minitest::g_failures;
            std::cerr << "  uncaught exception: " << e.what() << "\n";
        } catch (...) {
            ++minitest::g_failures;
            std::cerr << "  uncaught non-standard exception\n";
        }
        if (minitest::g_failures == 0) {
            std::cout << "[PASS] " << test.name << "\n";
            ++passed;
        } else {
            std::cout << "[FAIL] " << test.name << "\n";
            ++failed;
        }
    }

    std::cout << "\n----------------------------------------------------------\n";
    std::cout << passed << " passed, " << failed << " failed, "
              << (passed + failed) << " total\n";
    return failed == 0 ? 0 : 1;
}
