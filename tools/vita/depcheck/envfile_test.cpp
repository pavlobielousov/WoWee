// Host test for the env.txt parser and loader (VITA-6). No Vita SDK needed:
//   c++ -std=c++20 -Wall -Wextra -Werror -Iinclude tools/vita/depcheck/envfile_test.cpp \
//       src/platform/vita/vita_env.cpp -o /tmp/envfile_test && /tmp/envfile_test
#include "platform/vita/vita_platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using wowee::platform::vita::loadEnvFile;
using wowee::platform::vita::parseEnvLine;

static int g_fail = 0;
static void check(const char* name, bool ok) {
    if (!ok) ++g_fail;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

static bool parses(const char* line, const char* k, const char* v) {
    std::string key, value;
    return parseEnvLine(line, key, value) && key == k && value == v;
}
static bool rejects(const char* line) {
    std::string key, value;
    return !parseEnvLine(line, key, value);
}

int main() {
    check("plain", parses("A=1", "A", "1"));
    check("CRLF", parses("A=1\r\n", "A", "1"));
    check("blanks around key and value", parses("  A \t= 1 2 ", "A", "1 2"));
    check("export prefix", parses("export A=1", "A", "1"));
    check("double quotes", parses("A=\"x y\"", "A", "x y"));
    check("single quotes", parses("A='x y'", "A", "x y"));
    check("lone quote kept", parses("A=\"", "A", "\""));
    check("empty value", parses("A=", "A", ""));
    check("value keeps = and #", parses("A=b=c#d", "A", "b=c#d"));
    check("device path value", parses("WOW_DATA_PATH=uma0:data/wowee/Data", "WOW_DATA_PATH", "uma0:data/wowee/Data"));
    check("comment", rejects("# A=1"));
    check("indented comment", rejects("   # A=1"));
    check("blank", rejects("   \r\n"));
    check("no equals", rejects("badline"));
    check("empty key", rejects("=1"));

    namespace fs = std::filesystem;
    const fs::path p = fs::temp_directory_path() / "wowee_envfile_test.txt";
    {
        std::ofstream f(p, std::ios::binary);
        f << "# header\r\nVITA6_A=1\r\n\r\nVITA6_B = two words \r\nbad\r\nexport VITA6_C=3";  // no final newline
    }
    bool found = false;
    const auto keys = loadEnvFile(p.string().c_str(), found);
    check("file found", found);
    check("three keys, in order", keys.size() == 3 && keys[0] == "VITA6_A" && keys[1] == "VITA6_B" && keys[2] == "VITA6_C");
    check("setenv applied", std::getenv("VITA6_A") && std::string(std::getenv("VITA6_A")) == "1");
    check("value with spaces", std::getenv("VITA6_B") && std::string(std::getenv("VITA6_B")) == "two words");
    check("last line without newline", std::getenv("VITA6_C") && std::string(std::getenv("VITA6_C")) == "3");
    fs::remove(p);
    bool missing = true;
    check("missing file", loadEnvFile(p.string().c_str(), missing).empty() && !missing);
    return g_fail ? 1 : 0;
}
