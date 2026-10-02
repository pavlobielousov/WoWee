#pragma once
// Pure (no I/O) test cases for platform/vita/device_path.hpp. Shared by the host test
// (devpath_test.cpp) and the DepCheck probe, so both run the same assertions.
#include "platform/vita/device_path.hpp"

#include <string>

namespace devpath_cases {

// `check(name, ok, detail)` is the caller's reporter.
template <typename Check>
void run(Check&& check) {
    using namespace wowee::platform::vita;
    auto chk = [&](const char* name, bool ok) { check(name, ok, ""); };
    auto eq = [&](const char* name, const std::string& got, const char* want) {
        check(name, got == want, got.c_str());
    };

    chk("hasDeviceRoot ux0:data", hasDeviceRoot("ux0:data/x"));
    chk("hasDeviceRoot uma0:/", hasDeviceRoot("uma0:/data"));
    chk("hasDeviceRoot app0:", hasDeviceRoot("app0:"));
    chk("!hasDeviceRoot relative", !hasDeviceRoot("assets/x"));
    chk("!hasDeviceRoot dos drive", !hasDeviceRoot("C:\\x"));
    chk("!hasDeviceRoot empty", !hasDeviceRoot(""));
    chk("!hasDeviceRoot colon inside later segment", !hasDeviceRoot("data/a:b"));

    eq("normalize ux0:data/x", normalizeDevicePath("ux0:data/x"), "ux0:/data/x");
    eq("normalize ux0:/data/x", normalizeDevicePath("ux0:/data/x"), "ux0:/data/x");
    eq("normalize dup slashes and dots", normalizeDevicePath("ux0://data/./x/"), "ux0:/data/x");
    eq("normalize .. stays on device", normalizeDevicePath("ux0:data/../x"), "ux0:/x");
    eq("normalize .. clamps at root", normalizeDevicePath("ux0:/../../x"), "ux0:/x");
    eq("normalize root", normalizeDevicePath("app0:"), "app0:/");
    eq("normalize leaves relative alone", normalizeDevicePath("../a"), "../a");

    // opcode_table.cpp keys its inheritance-cycle check on this string: every spelling of one file
    // has to give one key.
    chk("one key per file",
          normalizeDevicePath("ux0:data/x/../y.json") == normalizeDevicePath("ux0:/data/y.json") &&
          normalizeDevicePath("ux0:/data//y.json") == normalizeDevicePath("ux0:data/./y.json"));

    eq("resolve device path ignores cwd", resolveDevicePath("uma0:data/x", "ux0:/data/wowee"), "uma0:/data/x");
    eq("resolve relative against ux0 cwd", resolveDevicePath("addons", "ux0:/data/wowee"), "ux0:/data/wowee/addons");
    eq("resolve ../ against ux0 cwd", resolveDevicePath("../addons", "ux0:/data/wowee"), "ux0:/data/addons");
    eq("resolve relative against bare app0:", resolveDevicePath("assets/a.png", "app0:"), "app0:/assets/a.png");
    eq("resolve ../ against app0: clamps", resolveDevicePath("../addons", "app0:"), "app0:/addons");
    eq("resolve against uma0 cwd", resolveDevicePath("Data", "uma0:/wowee"), "uma0:/wowee/Data");
    eq("resolve leading slash uses cwd device", resolveDevicePath("/data/x", "ux0:/data/wowee"), "ux0:/data/x");
    eq("resolve without any device is unchanged", resolveDevicePath("addons", ""), "addons");
    eq("resolve without cwd device is unchanged", resolveDevicePath("addons", "/home/x"), "addons");
}

}  // namespace devpath_cases
