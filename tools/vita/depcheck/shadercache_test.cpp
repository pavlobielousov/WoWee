// Host test for include/platform/vita/shader_cache_logic.hpp (VITA-53). No Vita SDK needed:
//   c++ -std=c++20 -Wall -Wextra -Werror -Iinclude tools/vita/depcheck/shadercache_test.cpp -o /tmp/shadercache_test && /tmp/shadercache_test
#include "platform/vita/shader_cache_logic.hpp"

#include <cmath>
#include <cstdio>

using namespace wowee::platform::vita::shadercache;
static int g_fail = 0;
static void check(const char* name, bool ok) {
    if (!ok) ++g_fail;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

int main() {
    const std::string src = "void main() { gl_FragColor = vec4(1.0); }";
    // keys
    const std::string k = cacheKey(Stage::Fragment, src, "gl1");
    check("key is 24 hex characters", k.size() == 24 && k.find_first_not_of("0123456789abcdef") == std::string::npos);
    check("key is stable", k == cacheKey(Stage::Fragment, src, "gl1"));
    check("a changed source changes the key", k != cacheKey(Stage::Fragment, src + " ", "gl1"));
    check("a changed stage changes the key", k != cacheKey(Stage::Vertex, src, "gl1"));
    check("a changed build tag changes the key", k != cacheKey(Stage::Fragment, src, "gl2"));
    check("file name", fileNameForKey(k) == k + ".bin");

    // entries
    const std::string payload("\x01\x02\x00binary\xff", 10);
    std::string enc = encodeEntry(payload), out;
    check("entry round-trips", decodeEntry(enc, out) && out == payload);
    check("an empty payload round-trips", decodeEntry(encodeEntry(""), out) && out.empty());
    std::string torn = enc.substr(0, enc.size() - 3);
    check("a torn entry is rejected", !decodeEntry(torn, out));
    std::string flipped = enc;
    flipped.back() ^= 0x40;
    check("a corrupted entry is rejected", !decodeEntry(flipped, out));
    std::string wrongMagic = enc;
    wrongMagic[0] = 'X';
    check("a wrong magic is rejected", !decodeEntry(wrongMagic, out));
    std::string wrongVersion = enc;
    wrongVersion[4] = static_cast<char>(kFormatVersion + 1);
    check("another format version is rejected", !decodeEntry(wrongVersion, out));
    check("a short file is rejected", !decodeEntry("WSC1", out));
    std::string longer = enc + "x";
    check("trailing bytes are rejected", !decodeEntry(longer, out));

    // stale sweep
    const std::string keep = cacheKey(Stage::Vertex, "a", "t"), drop = cacheKey(Stage::Vertex, "b", "t");
    auto stale = staleFiles({fileNameForKey(keep), fileNameForKey(drop), "x.tmp", "notes.txt"}, {keep});
    check("stale: unreferenced, tmp and foreign files, not the referenced one",
          stale.size() == 3 && std::find(stale.begin(), stale.end(), fileNameForKey(keep)) == stale.end());
    check("stale: nothing to sweep when all are referenced", staleFiles({fileNameForKey(keep)}, {keep}).empty());

    // progress
    Progress p;
    for (int i = 0; i < 3; ++i) p.add(ProgramClass::Plain);
    p.add(ProgramClass::Skinned);
    check("progress counts programs", p.total() == 4 && p.done() == 0 && p.fraction() == 0.0);
    p.finished(800.0, true);  // twice as slow as guessed
    check("progress moves by the guessed weight", std::fabs(p.fraction() - 400.0 / 2300.0) < 1e-9);
    check("the remaining estimate scales by the measured slowdown",
          std::fabs(p.remainingMs() - (400 + 400 + 1100) * 2.0) < 1e-6);
    p.finished(0.0, false);  // a cache hit does not skew the scale
    check("a cache hit moves the bar and keeps the scale",
          std::fabs(p.remainingMs() - (400 + 1100) * 2.0) < 1e-6 && p.done() == 2);
    p.finished(800.0, true);
    p.finished(2200.0, true);
    check("progress ends at 100 %", p.fraction() == 1.0 && p.remainingMs() == 0.0);
    Progress empty;
    check("an empty manifest is complete", empty.fraction() == 1.0 && empty.remainingMs() == 0.0);

    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
