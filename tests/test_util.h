// Small helpers shared by the test files.
#pragma once

#include <cstdio>
#include <string>

namespace minitest {

// Returns a unique file path under build/test_data and removes any existing
// file at that path so each test starts clean.
inline std::string temp_path(const std::string& name) {
    std::string dir = "build/test_data";
    std::string cmd = "mkdir -p " + dir;
    (void)std::system(cmd.c_str());
    std::string path = dir + "/" + name;
    std::remove(path.c_str());
    return path;
}

// Remove a file, ignoring errors (used for cleanup).
inline void remove_file(const std::string& path) { std::remove(path.c_str()); }

// Returns a fresh, empty directory under build/test_data for engine tests.
inline std::string temp_dir(const std::string& name) {
    std::string dir = "build/test_data/" + name;
    (void)std::system(("rm -rf " + dir).c_str());
    (void)std::system(("mkdir -p " + dir).c_str());
    return dir;
}

}  // namespace minitest
