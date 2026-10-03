// DepCheck: runtime probe for docs/vita/DEPENDENCIES.md (VITA-3).
// Compiles AND runs the C++20 features and libraries WoWee relies on, so "the header exists" is
// not mistaken for "it works on newlib". Logs PASS/FAIL/INFO lines to
// ux0:data/wowee/depcheck.log and stdout, then exits by itself (no input needed).
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <shared_mutex>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/rc4.h>
#include <openssl/sha.h>
#include <zlib.h>

#include "devpath_cases.hpp"
#include "core/data_paths.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace fs = std::filesystem;

#define DIR_PATH "ux0:data/wowee"
#define LOG_PATH DIR_PATH "/depcheck.log"

static int g_fail = 0;

static void log_line(const char* fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof line) - 2) n = static_cast<int>(sizeof line) - 2;
    line[n++] = '\n';
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, static_cast<unsigned>(n));
        sceIoClose(fd);
    }
    fwrite(line, 1, static_cast<size_t>(n), stdout);
    fflush(stdout);
}

static void check(const char* name, bool ok, const char* detail = "") {
    if (!ok) ++g_fail;
    log_line("%s %s %s", ok ? "PASS" : "FAIL", name, detail);
}

// --- C++20 language features -------------------------------------------------------------
template <typename T>
concept Numeric = std::is_arithmetic_v<T>;

template <Numeric T>
static T twice(T v) { return v + v; }

struct Opts { int a; float b; bool c; };

static int sum_span(std::span<const int> s) {
    int t = 0;
    for (int v : s) t += v;
    return t;
}

static std::string where_name(std::source_location l = std::source_location::current()) {
    return l.function_name();
}

static void cxx20_language() {
    check("concept+requires", twice(21) == 42 && requires { twice(1.5f); });
    constexpr Opts o{.a = 1, .b = 2.0f, .c = true};  // designated initialisers
    check("designated-init", o.a == 1 && o.c);
    std::vector<int> v{1, 2, 3, 4};
    check("std::span", sum_span(v) == 10);
    auto rev = v | std::views::reverse;
    check("ranges/views", *rev.begin() == 4);
    float f = 1.5f;
    check("std::bit_cast", std::bit_cast<std::uint32_t>(f) == 0x3fc00000u);
    check("std::popcount", std::popcount(0xF0u) == 4 && std::has_single_bit(64u));
    check("source_location", !where_name().empty(), where_name().c_str());
    check("starts_with/contains", std::string("wowee").starts_with("wo") && v.size() == 4);
    std::variant<int, std::string> var = std::string("x");
    check("variant/optional", std::holds_alternative<std::string>(var) && std::optional<int>(3).has_value());
    // exceptions and thread_local
    bool caught = false;
    try { throw std::runtime_error("boom"); } catch (const std::exception&) { caught = true; }
    check("exceptions", caught);
}

static void* raw_thread_fn(void* a) { *static_cast<int*>(a) = 5; return nullptr; }

static void raw_pthread() {
    pthread_t th;
    int val = 0;
    int rc = pthread_create(&th, nullptr, raw_thread_fn, &val);
    int jr = rc == 0 ? pthread_join(th, nullptr) : -1;
    char b[96];
    snprintf(b, sizeof b, "pthread_create rc=%d join rc=%d val=%d", rc, jr, val);
    check("raw pthread_create/join", rc == 0 && jr == 0 && val == 5, b);
}

static void thread_basics() {
    static thread_local int tl = 7;
    int seen_in_thread = -1;
    std::thread t([&] { tl = 99; seen_in_thread = tl; });
    t.join();
    check("thread_local + std::thread", tl == 7 && seen_in_thread == 99);
}

// --- printf length modifiers (newlib-nano may drop %z / %ll) ----------------------------------------
static void printf_formats() {
    char b[96];
    struct Case { const char* name; const char* want; std::function<void(char*, size_t)> fn; };
    const Case cases[] = {
        {"%zu", "123456", [](char* o, size_t n) { snprintf(o, n, "%zu", static_cast<size_t>(123456)); }},
        {"%zd", "-5", [](char* o, size_t n) { snprintf(o, n, "%zd", static_cast<ssize_t>(-5)); }},
        {"%td", "7", [](char* o, size_t n) { snprintf(o, n, "%td", static_cast<ptrdiff_t>(7)); }},
        // does a dropped %z swallow its argument? (matters: a shifted argument before %s is a crash)
        {"%d|%zu|%d (args 1,2,3)", "1|zu|2", [](char* o, size_t n) { snprintf(o, n, "%d|%zu|%d", 1, static_cast<size_t>(2), 3); }},
        {"%lld", "-5000000000", [](char* o, size_t n) { snprintf(o, n, "%lld", -5000000000LL); }},
        {"%llu", "5000000000", [](char* o, size_t n) { snprintf(o, n, "%llu", 5000000000ULL); }},
        {"%llx", "12ab34cd56", [](char* o, size_t n) { snprintf(o, n, "%llx", 0x12ab34cd56ULL); }},
        {"%jd", "-9", [](char* o, size_t n) { snprintf(o, n, "%jd", static_cast<intmax_t>(-9)); }},
        {"%lu", "4000000000", [](char* o, size_t n) { snprintf(o, n, "%lu", 4000000000UL); }},
        {"PRIu64", "18446744073709551615", [](char* o, size_t n) { snprintf(o, n, "%" PRIu64, UINT64_MAX); }},
        {"%.2f", "3.14", [](char* o, size_t n) { snprintf(o, n, "%.2f", 3.14159); }},
        {"%g", "0.0001", [](char* o, size_t n) { snprintf(o, n, "%g", 0.0001); }},
    };
    for (const auto& c : cases) {
        char out[64] = {0};
        c.fn(out, sizeof out);
        snprintf(b, sizeof b, "got '%s' want '%s'", out, c.want);
        check((std::string("printf ") + c.name).c_str(), strcmp(out, c.want) == 0, b);
    }
}

// --- threads ----------------------------------------------------------------------------
static void threads() {
    unsigned hc = std::thread::hardware_concurrency();
    char buf[64];
    snprintf(buf, sizeof buf, "hardware_concurrency=%u", hc);
    log_line("INFO threads %s", buf);

    std::atomic<int> counter{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i) ts.emplace_back([&] { for (int k = 0; k < 1000; ++k) counter++; });
    for (auto& th : ts) th.join();
    check("std::thread x4", counter == 4000);

    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    std::thread waiter([&] {
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return ready; });
    });
    { std::lock_guard lk(m); ready = true; }
    cv.notify_all();
    waiter.join();
    check("mutex+condition_variable", true);

    std::shared_mutex sm;
    { std::shared_lock a(sm); std::shared_lock b(sm); }
    { std::unique_lock w(sm); }
    check("shared_mutex", true);

    auto fut = std::async(std::launch::async, [] { return 6 * 7; });
    check("std::async", fut.get() == 42);

    std::atomic<int> flag{0};
    std::thread setter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        flag.store(1);
        flag.notify_one();
    });
    flag.wait(0);
    setter.join();
    check("atomic::wait/notify (C++20)", flag.load() == 1);

    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    snprintf(buf, sizeof buf, "slept 50ms, measured %ld ms", static_cast<long>(ms));
    check("steady_clock", ms >= 45 && ms < 500, buf);

    std::random_device rd;
    bool rd_ok = true;
    unsigned r1 = 0, r2 = 0;
    try { r1 = rd(); r2 = rd(); } catch (...) { rd_ok = false; }
    snprintf(buf, sizeof buf, "%08x %08x (entropy=%.1f)", r1, r2, rd_ok ? rd.entropy() : -1.0);
    check("std::random_device", rd_ok && r1 != r2, buf);
}

// --- std::filesystem on ux0: --------------------------------------------------------------
static void filesystem() {
    char buf[200];
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    snprintf(buf, sizeof buf, "current_path='%s' ec=%d", cwd.string().c_str(), ec.value());
    log_line("INFO fs %s", buf);

    fs::path p = "ux0:data/wowee/depcheck_fs";
    snprintf(buf, sizeof buf, "is_absolute=%d root_name='%s' root_dir='%s' has_root_path=%d",
             p.is_absolute(), p.root_name().string().c_str(), p.root_directory().string().c_str(), p.has_root_path());
    log_line("INFO fs ux0: path: %s", buf);
    fs::path ab = fs::absolute(p, ec);
    snprintf(buf, sizeof buf, "absolute()='%s' ec=%d", ab.string().c_str(), ec.value());
    log_line("INFO fs %s", buf);
    fs::path wc = fs::weakly_canonical(p, ec);
    snprintf(buf, sizeof buf, "weakly_canonical()='%s' ec=%d", wc.string().c_str(), ec.value());
    log_line("INFO fs %s", buf);

    bool made = fs::create_directories(p / "a" / "b", ec);
    check("fs::create_directories", made && !ec, ec.message().c_str());
    { std::ofstream o(p / "a" / "x.bin", std::ios::binary); o << "0123456789"; }
    check("ofstream+file_size", fs::file_size(p / "a" / "x.bin", ec) == 10 && !ec, ec.message().c_str());
    fs::rename(p / "a" / "x.bin", p / "a" / "b" / "y.bin", ec);
    check("fs::rename", !ec && fs::exists(p / "a" / "b" / "y.bin"), ec.message().c_str());
    int n = 0;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); ++it) ++n;
    snprintf(buf, sizeof buf, "entries=%d (expect 3: a, a/b, a/b/y.bin)", n);
    check("recursive_directory_iterator", n == 3, buf);
    fs::path rel = fs::relative(p / "a" / "b", p, ec);
    check("fs::relative", rel == fs::path("a/b"), rel.string().c_str());
    auto sp = fs::space(DIR_PATH, ec);
    snprintf(buf, sizeof buf, "free=%lu MB ec=%d", static_cast<unsigned long>(sp.available >> 20), ec.value());
    log_line("INFO fs space %s", buf);
    // chdir + relative paths: WoWee resolves ./Data and WOWEE_RESOURCE_ROOT against the working directory.
    fs::path before = fs::current_path(ec);
    fs::create_directories("ux0:data/wowee/depcheck_cwd", ec);
    fs::current_path("ux0:data/wowee/depcheck_cwd", ec);
    int cec = ec.value();
    fs::path after = fs::current_path(ec);
    { std::ofstream o("rel.txt"); o << "hi"; }
    bool rel_ok = fs::exists("ux0:data/wowee/depcheck_cwd/rel.txt", ec);
    snprintf(buf, sizeof buf, "before='%s' chdir ec=%d after='%s' relative-write-landed-in-cwd=%d", before.string().c_str(), cec,
             after.string().c_str(), rel_ok);
    check("fs::current_path(ux0:...) + relative open", cec == 0 && rel_ok, buf);
    fs::remove("ux0:data/wowee/depcheck_cwd/rel.txt", ec);
    fs::current_path(before, ec);
    fs::remove("ux0:data/wowee/depcheck_cwd", ec);

    // Manual cleanup first: fs::remove_all() spins forever on Vita3K (see remove_all_last()).
    fs::remove(p / "a" / "b" / "y.bin", ec);
    fs::remove(p / "a" / "b", ec);
    fs::remove(p / "a", ec);
    fs::remove(p, ec);
    check("fs::remove (file and dirs)", !fs::exists(p, ec));
}

// --- BSD sockets, the way src/network/net_platform.hpp uses them -------------------------------
// Needs a TCP listener on the dev machine: `python3 tools/vita/depcheck/echo_server.py` (port 9998).
// ux0:data/wowee/depcheck_host.txt may hold another "<ip>[:port]"; default is 127.0.0.1:9998.
static char g_net_memory[256 * 1024];

static void sockets() {
    char b[160];
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam param = {g_net_memory, sizeof g_net_memory, 0};
    int ni = sceNetInit(&param);
    int nc = sceNetCtlInit();
    snprintf(b, sizeof b, "sceNetInit=0x%x sceNetCtlInit=0x%x", ni, nc);
    log_line("INFO net %s", b);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    snprintf(b, sizeof b, "fd=%d errno=%d", fd, errno);
    check("socket(AF_INET, SOCK_STREAM)", fd >= 0, b);
    if (fd < 0) return;
    int one = 1;
    int nd = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    snprintf(b, sizeof b, "rc=%d errno=%d", nd, errno);
    check("setsockopt(TCP_NODELAY)", nd == 0, b);
    int flags = fcntl(fd, F_GETFL, 0);
    int nb = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    snprintf(b, sizeof b, "F_GETFL=%d F_SETFL rc=%d errno=%d", flags, nb, errno);
    check("fcntl(O_NONBLOCK)", nb != -1, b);

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int gr = getaddrinfo("127.0.0.1", "9998", &hints, &res);
    snprintf(b, sizeof b, "rc=%d (%s)", gr, gr ? gai_strerror(gr) : "ok");
    check("getaddrinfo(numeric)", gr == 0 && res, b);
    if (res) freeaddrinfo(res);
    struct addrinfo* res2 = nullptr;
    int g2 = getaddrinfo("localhost", nullptr, &hints, &res2);
    snprintf(b, sizeof b, "rc=%d (%s)", g2, g2 ? gai_strerror(g2) : "ok");
    log_line("INFO net getaddrinfo(\"localhost\") %s", b);
    if (res2) freeaddrinfo(res2);

    char host[64] = "127.0.0.1";
    int port = 9998;
    SceUID hf = sceIoOpen(DIR_PATH "/depcheck_host.txt", SCE_O_RDONLY, 0);
    if (hf >= 0) {
        char t[64] = {0};
        sceIoRead(hf, t, sizeof t - 1);
        sceIoClose(hf);
        t[strcspn(t, " \r\n\t")] = 0;
        char* colon = strchr(t, ':');
        if (colon) { *colon = 0; port = atoi(colon + 1); }
        snprintf(host, sizeof host, "%s", t);
    }
    sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host, &sa.sin_addr);
    int cr = connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    int ce = errno;
    snprintf(b, sizeof b, "%s:%d rc=%d errno=%d (EINPROGRESS=%d)", host, port, cr, ce, EINPROGRESS);
    log_line("INFO net non-blocking connect %s", b);
    fd_set wf, ef;
    FD_ZERO(&wf); FD_ZERO(&ef);
    FD_SET(fd, &wf); FD_SET(fd, &ef);
    timeval tv = {3, 0};
    int sel = select(fd + 1, nullptr, &wf, &ef, &tv);
    int soerr = -1;
    socklen_t sl = sizeof soerr;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    snprintf(b, sizeof b, "select=%d writable=%d SO_ERROR=%d", sel, FD_ISSET(fd, &wf) ? 1 : 0, soerr);
    bool connected = (cr == 0 || ce == EINPROGRESS) && sel > 0 && soerr == 0;
    check("connect+select(write)+SO_ERROR (needs echo_server.py)", connected, b);
    if (connected) {
        const char msg[] = "ping";
        ssize_t sn = send(fd, msg, 4, 0);
        char rb[16] = {0};
        timeval tv2 = {3, 0};
        fd_set rf;
        FD_ZERO(&rf); FD_SET(fd, &rf);
        int rs = select(fd + 1, &rf, nullptr, nullptr, &tv2);
        ssize_t rn = rs > 0 ? recv(fd, rb, sizeof rb - 1, 0) : -1;
        snprintf(b, sizeof b, "sent=%ld select=%d recv=%ld '%s'", static_cast<long>(sn), rs, static_cast<long>(rn), rb);
        check("send/select/recv echo", sn == 4 && rn == 4 && strcmp(rb, "ping") == 0, b);
    }
    // a refused connection must be reported, not hang (port 1 is closed)
    int fd2 = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(fd2, F_SETFL, fcntl(fd2, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in dead = sa;
    dead.sin_port = htons(1);
    connect(fd2, reinterpret_cast<sockaddr*>(&dead), sizeof dead);
    fd_set wf2, ef2;
    FD_ZERO(&wf2); FD_ZERO(&ef2);
    FD_SET(fd2, &wf2); FD_SET(fd2, &ef2);
    timeval tv3 = {3, 0};
    int sel2 = select(fd2 + 1, nullptr, &wf2, &ef2, &tv3);
    int so2 = 0;
    socklen_t sl2 = sizeof so2;
    getsockopt(fd2, SOL_SOCKET, SO_ERROR, &so2, &sl2);
    snprintf(b, sizeof b, "select=%d SO_ERROR=%d (ECONNREFUSED=%d)", sel2, so2, ECONNREFUSED);
    log_line("INFO net refused-connect %s", b);
    close(fd2);
    close(fd);
}

// --- VITA-35: the eight std::filesystem path call sites, with and without the helper ------------
// Each probe line copies the expression a shared source file uses. "raw" is what libstdc++ gives,
// "helper" is platform/vita/device_path.hpp. Run under the two working directories the app can
// have: "app0:" (at launch) and a chdir'd "ux0:/data/...".
static void probe_call_sites(const char* tag) {
    namespace pv = wowee::platform::vita;
    std::error_code ec;
    char buf[300];
    auto line = [&](const char* site, const char* what, const std::string& v) {
        snprintf(buf, sizeof buf, "%s %s %s='%s' ec=%d", tag, site, what, v.c_str(), ec.value());
        log_line("INFO devpath %s", buf);
        ec.clear();
    };
    log_line("INFO devpath [%s] cwd='%s'", tag, fs::current_path(ec).string().c_str());
    ec.clear();
    // auth_screen.cpp:1008,1093  current_path() / relative, then exists()
    line("auth_screen", "current_path()/rel", (fs::current_path(ec) / "assets/krayonsignin.png").string());
    line("auth_screen", "current_path()/rel(2)", (fs::current_path(ec) / fs::path("assets") / "Original Music" / "T.mp3").string());
    // config_paths.cpp:26-29  current == fs::path(root)
    {
        fs::path cur = fs::current_path(ec);
        fs::path root = cur;  // the env root spelled exactly like the OS spells it
        snprintf(buf, sizeof buf, "%s config_paths same-spelling equal=%d", tag, cur == root);
        log_line("INFO devpath %s", buf);
        std::string spelled = cur.string();
        if (spelled.size() > 2 && spelled.back() != '/') spelled += "/";
        else if (spelled.size() > 2) spelled.pop_back();
        snprintf(buf, sizeof buf, "%s config_paths other-spelling '%s' raw-equal=%d helper-equal=%d", tag, spelled.c_str(),
                 cur == fs::path(spelled), pv::resolveDevicePath(cur) == pv::resolveDevicePath(fs::path(spelled)));
        log_line("INFO devpath %s", buf);
    }
    // addon_manager.cpp:145-146
    for (const char* local : {"addons", "../addons", "../../addons"}) {
        line("addon_manager", (std::string("raw absolute ") + local).c_str(), fs::absolute(local, ec).string());
        line("addon_manager", (std::string("raw weakly_canonical(absolute) ") + local).c_str(),
             fs::weakly_canonical(fs::absolute(local, ec), ec).string());
        line("addon_manager", (std::string("helper ") + local).c_str(), pv::resolveDevicePath(fs::path(local)).string());
    }
    // opcode_table.cpp:106  weakly_canonical(path) as the cycle-detection key (the raw call takes no ec there)
    for (const char* in : {"ux0:data/wowee/depcheck_dp/ops.json", "ux0:data/wowee/depcheck_dp/x/../ops.json", "ops.json"}) {
        line("opcode_table", (std::string("raw weakly_canonical ") + in).c_str(), fs::weakly_canonical(in, ec).string());
        line("opcode_table", (std::string("helper ") + in).c_str(), pv::resolveDevicePath(fs::path(in)).string());
    }
    // zone_manager.cpp:23  exists(rel) then canonical(rel)
    {
        fs::path rel = fs::path("assets") / "Original Music" / "T.mp3";
        fs::path abs = fs::canonical(rel, ec);
        line("zone_manager", "raw canonical (missing file)", abs.string());
        if (fs::exists("eboot.bin")) line("zone_manager", "raw canonical (existing eboot.bin)", fs::canonical("eboot.bin", ec).string());
        fs::create_directories("ux0:data/wowee/depcheck_dp/assets/Original Music", ec);
        ec.clear();
        { std::ofstream o("ux0:data/wowee/depcheck_dp/assets/Original Music/T.mp3"); o << "x"; }
        fs::path cur = fs::current_path(ec);
        fs::current_path("ux0:data/wowee/depcheck_dp", ec);
        ec.clear();
        line("zone_manager", "cwd now", fs::current_path(ec).string());
        bool ex = fs::exists(rel, ec);
        ec.clear();
        fs::path abs2 = fs::canonical(rel, ec);
        line("zone_manager", ex ? "raw canonical (file exists, cwd=depcheck_dp)" : "exists=0", abs2.string());
        line("zone_manager", "helper", pv::resolveDevicePath(rel).string());
        fs::current_path(cur, ec);
        ec.clear();
    }
}

static void devpath() {
    namespace pv = wowee::platform::vita;
    std::error_code ec;
    devpath_cases::run([](const char* name, bool ok, const char* detail) {
        check((std::string("devpath ") + name).c_str(), ok, detail);
    });
    // uma0: strings are lexical, no device needed.
    check("devpath uma0: lexical", pv::resolveDevicePath("Data/../addons", "uma0:/wowee") == "uma0:/wowee/addons");

    fs::create_directories("ux0:data/wowee/depcheck_dp", ec);
    ec.clear();
    fs::path start = fs::current_path(ec);
    ec.clear();
    probe_call_sites("cwd=app0");
    fs::current_path("ux0:data/wowee/depcheck_dp", ec);
    ec.clear();
    probe_call_sites("cwd=ux0-chdir");
    fs::current_path(start, ec);
    ec.clear();

    // helper vs the real thing: the helper's answer must be openable.
    {
        fs::path want = "ux0:data/wowee/depcheck_dp/ops.json";
        { std::ofstream o(want); o << "{}"; }
        fs::path got = pv::resolveDevicePath(fs::path("ux0:data/wowee/depcheck_dp/x/../ops.json"));
        check("devpath helper result opens", std::ifstream(got).is_open(), got.string().c_str());
        // spelling variants must collapse to one key (opcode_table cycle check)
        check("devpath one key per file (cwd=app0)",
              pv::resolveDevicePath(fs::path("ux0:data/wowee/depcheck_dp/x/../ops.json")) ==
              pv::resolveDevicePath(fs::path("ux0:/data/wowee/depcheck_dp/ops.json")));
    }

    // addon_manager.cpp:131  fs::equivalent(entry, asked) on two DIFFERENT existing files/dirs.
    {
        fs::create_directories("ux0:data/wowee/depcheck_dp/d1", ec);
        fs::create_directories("ux0:data/wowee/depcheck_dp/d2", ec);
        ec.clear();
        bool diff = fs::equivalent("ux0:data/wowee/depcheck_dp/d1", "ux0:data/wowee/depcheck_dp/d2", ec);
        int e1 = ec.value();
        ec.clear();
        bool same = fs::equivalent("ux0:data/wowee/depcheck_dp/d1", "ux0:data/wowee/depcheck_dp/d1", ec);
        int e2 = ec.value();
        ec.clear();
        bool same2 = fs::equivalent("ux0:data/wowee/depcheck_dp/d1", "ux0:/data/wowee/depcheck_dp/d1/", ec);
        char b[200];
        snprintf(b, sizeof b, "different=%d(ec=%d) same=%d(ec=%d) same-other-spelling=%d", diff, e1, same, e2, same2);
        // Known bug (VITA-41): fs::equivalent is true for two different directories. This passes
        // while it is, and fails the day it is fixed: then the workaround (core::samePath) can go.
        check("devpath fs::equivalent still wrong, workaround needed (VITA-41)", diff, b);
        // The helper the call sites use instead (VITA-41).
        std::error_code pec;
        const bool pDiff = pv::pathsEquivalent("ux0:data/wowee/depcheck_dp/d1", "ux0:data/wowee/depcheck_dp/d2", pec);
        const bool pSame = pv::pathsEquivalent("ux0:data/wowee/depcheck_dp/d1", "ux0:data/wowee/depcheck_dp/d1", pec);
        const bool pSame2 = pv::pathsEquivalent("ux0:data/wowee/depcheck_dp/d1", "ux0:/data/wowee/depcheck_dp/d1/", pec);
        const bool pCase = pv::pathsEquivalent("ux0:data/wowee/depcheck_dp/d1", "ux0:data/WOWEE/depcheck_dp/D1", pec);
        const bool pMissing = pv::pathsEquivalent("ux0:data/wowee/depcheck_dp/d1", "ux0:data/wowee/depcheck_dp/nope", pec);
        snprintf(b, sizeof b, "different=%d same=%d other-spelling=%d other-case=%d missing=%d", pDiff, pSame, pSame2, pCase, pMissing);
        check("devpath pathsEquivalent (two dirs differ, spellings agree)", !pDiff && pSame && pSame2 && pCase && !pMissing, b);

        // The call site that mattered: syncClientTables must copy into a separate data root.
        fs::create_directories("ux0:data/wowee/depcheck_dp/install/expansions/x", ec);
        fs::create_directories("ux0:data/wowee/depcheck_dp/dataroot/expansions/x", ec);
        { std::ofstream("ux0:data/wowee/depcheck_dp/install/expansions/x/expansion.json") << "{\"id\":\"x\"}"; }
        { std::ofstream("ux0:data/wowee/depcheck_dp/dataroot/expansions/x/manifest.json") << "{}"; }
        const int copied = wowee::core::syncClientTables("ux0:data/wowee/depcheck_dp/install", "ux0:data/wowee/depcheck_dp/dataroot");
        std::string got;
        { std::ifstream in("ux0:data/wowee/depcheck_dp/dataroot/expansions/x/expansion.json"); std::getline(in, got); }
        const int again = wowee::core::syncClientTables("ux0:data/wowee/depcheck_dp/install", "ux0:data/wowee/depcheck_dp/dataroot");
        const int sameRoot = wowee::core::syncClientTables("ux0:data/wowee/depcheck_dp/install", "ux0:/data/wowee/depcheck_dp/install/");
        snprintf(b, sizeof b, "copied=%d content='%s' again=%d same-root=%d", copied, got.c_str(), again, sameRoot);
        check("devpath syncClientTables copies into a separate data root", copied == 1 && got == "{\"id\":\"x\"}" && again == 0 && sameRoot == 0, b);
        ec.clear();
    }

    // removeTree: non-empty tree, the case where fs::remove_all hangs.
    {
        fs::path t = "ux0:data/wowee/depcheck_dp/tree";
        fs::create_directories(t / "a" / "b", ec);
        { std::ofstream o(t / "x.bin"); o << "x"; }
        { std::ofstream o(t / "a" / "y.bin"); o << "y"; }
        { std::ofstream o(t / "a" / "b" / "z.bin"); o << "z"; }
        ec.clear();
        auto n = pv::removeTree(t, ec);
        char b[100];
        snprintf(b, sizeof b, "removed=%lu ec=%d", static_cast<unsigned long>(n), ec.value());
        check("devpath removeTree (non-empty tree)", !ec && n == 6 && !fs::exists(t), b);
        ec.clear();
    }

    // uma0: I/O, only if the device is mounted (Vita3K usually has no uma0:).
    {
        fs::create_directories("uma0:data/wowee/depcheck_dp", ec);
        if (ec || !fs::exists("uma0:data/wowee/depcheck_dp")) {
            log_line("SKIP devpath uma0: I/O (device not mounted: %s)", ec.message().c_str());
        } else {
            { std::ofstream o("uma0:data/wowee/depcheck_dp/a.txt"); o << "x"; }
            fs::path got = pv::resolveDevicePath(fs::path("uma0:data/wowee/depcheck_dp/../depcheck_dp/a.txt"));
            check("devpath uma0: helper result opens", std::ifstream(got).is_open(), got.string().c_str());
            pv::removeTree("uma0:data/wowee/depcheck_dp", ec);
        }
        ec.clear();
    }

    pv::removeTree("ux0:data/wowee/depcheck_dp", ec);
    check("devpath cleanup", !fs::exists("ux0:data/wowee/depcheck_dp"));
}

// --- libraries ---------------------------------------------------------------------------
// Runs last: libstdc++'s fs::remove_all() loops forever on Vita3K for a non-empty tree (it keeps
// calling sceIoRemove on the directory, which answers "Directory not empty"). If the app never logs
// "depcheck done", this is where it hung.
static void remove_all_last() {
    std::error_code ec;
    fs::path p = "ux0:data/wowee/depcheck_fs2";
    fs::create_directories(p / "a", ec);
    { std::ofstream o(p / "a" / "x.bin"); o << "x"; }
    log_line("INFO fs about to call fs::remove_all on a non-empty tree");
    auto n = fs::remove_all(p, ec);
    char b[64];
    snprintf(b, sizeof b, "removed=%lu ec=%d", static_cast<unsigned long>(n), ec.value());
    check("fs::remove_all (non-empty tree)", !fs::exists(p, ec), b);
}

static void libraries() {
    char buf[200];
    // OpenSSL (what auth/network use): SHA1, HMAC, BN, RC4, RAND
    unsigned char md[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>("abc"), 3, md);
    check("openssl SHA1(abc)", md[0] == 0xa9 && md[1] == 0x99 && md[19] == 0x9d);
    unsigned int hl = 0;
    unsigned char hm[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha1(), "key", 3, reinterpret_cast<const unsigned char*>("data"), 4, hm, &hl);
    check("openssl HMAC-SHA1", hl == 20);
    BIGNUM* b = BN_new();
    BIGNUM* e = BN_new();
    BIGNUM* m = BN_new();
    BIGNUM* r = BN_new();
    BN_CTX* ctx = BN_CTX_new();
    BN_set_word(b, 7);
    BN_set_word(e, 560);
    BN_set_word(m, 561);
    BN_mod_exp(r, b, e, m, ctx);
    snprintf(buf, sizeof buf, "7^560 mod 561 = %lu (expect 1)", static_cast<unsigned long>(BN_get_word(r)));
    check("openssl BN_mod_exp", BN_get_word(r) == 1, buf);
    BN_free(b); BN_free(e); BN_free(m); BN_free(r); BN_CTX_free(ctx);
    RC4_KEY k;
    unsigned char key[4] = {1, 2, 3, 4}, data[8] = {0}, out[8];
    RC4_set_key(&k, 4, key);
    RC4(&k, 8, data, out);
    check("openssl RC4", std::any_of(out, out + 8, [](unsigned char c) { return c != 0; }));
    unsigned char rnd[16] = {0};
    int rr = RAND_bytes(rnd, sizeof rnd);
    check("openssl RAND_bytes", rr == 1 && std::any_of(rnd, rnd + 16, [](unsigned char c) { return c != 0; }));
    log_line("INFO openssl version %s", OpenSSL_version(OPENSSL_VERSION));

    // zlib
    std::string src(4096, 'a');
    uLongf dl = compressBound(static_cast<uLong>(src.size()));
    std::vector<Bytef> comp(dl);
    int zr = compress(comp.data(), &dl, reinterpret_cast<const Bytef*>(src.data()), static_cast<uLong>(src.size()));
    std::vector<Bytef> back(src.size());
    uLongf bl = static_cast<uLongf>(back.size());
    int zu = uncompress(back.data(), &bl, comp.data(), dl);
    check("zlib round trip", zr == Z_OK && zu == Z_OK && bl == src.size());
    log_line("INFO zlib version %s", zlibVersion());

    // glm (float math on NEON-less-by-default build; no GLM_FORCE_* here)
    glm::mat4 pr = glm::perspective(glm::radians(60.0f), 960.0f / 544.0f, 0.1f, 100.0f);
    glm::vec4 c = pr * glm::vec4(0, 0, -10.0f, 1);
    snprintf(buf, sizeof buf, "z_ndc=%.3f (GL [-1,1] convention, WoWee defines GLM_FORCE_DEPTH_ZERO_TO_ONE)", c.z / c.w);
    check("glm perspective", c.w > 0, buf);

    // Lua 5.1.5 (vendored, built as plain C with no LUA_USE_* defines)
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    int lr = luaL_dostring(L, "local t={} for i=1,100 do t[i]=i*i end return t[10]+#string.format('%5.2f', 3.14159)");
    double v = lr == 0 ? lua_tonumber(L, -1) : -1;
    snprintf(buf, sizeof buf, "result=%g sizeof(lua_Number)=%lu sizeof(ptrdiff_t)=%lu", v, (unsigned long)sizeof(lua_Number), (unsigned long)sizeof(ptrdiff_t));
    check("lua 5.1.5 run", lr == 0 && v == 105.0, buf);
    lua_close(L);
}

template <typename F>
static void guarded(const char* name, F fn) {
    try {
        fn();
    } catch (const std::system_error& e) {
        ++g_fail;
        log_line("FAIL %s: std::system_error code=%d '%s'", name, e.code().value(), e.what());
    } catch (const std::exception& e) {
        ++g_fail;
        log_line("FAIL %s: exception '%s'", name, e.what());
    } catch (...) {
        ++g_fail;
        log_line("FAIL %s: unknown exception", name);
    }
}

int main() {
    sceIoMkdir(DIR_PATH, 0777);
    log_line("depcheck started");
    char buf[96];
    snprintf(buf, sizeof buf, "__cplusplus=%ld gcc=%s sizeof(size_t)=%lu long=%lu void*=%lu wchar_t=%lu", __cplusplus,
             __VERSION__, (unsigned long)sizeof(size_t), (unsigned long)sizeof(long), (unsigned long)sizeof(void*),
             (unsigned long)sizeof(wchar_t));
    log_line("INFO build %s", buf);
    guarded("cxx20_language", cxx20_language);
    guarded("raw_pthread", raw_pthread);
    guarded("thread_basics", thread_basics);
    guarded("printf_formats", printf_formats);
    guarded("threads", threads);
    guarded("filesystem", filesystem);
    guarded("devpath", devpath);
    guarded("libraries", libraries);
    guarded("sockets", sockets);
    log_line("depcheck done: %d failure(s) before the remove_all test", g_fail);
    guarded("remove_all_last", remove_all_last);  // hangs on Vita3K, so keep it last
    log_line("depcheck finished (remove_all returned)");
    sceKernelExitProcess(g_fail);
    return g_fail;
}
