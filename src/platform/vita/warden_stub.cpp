// Warden on the Vita (VITA-47): there is none. The real src/game/warden_module.cpp needs OpenSSL 3
// (the VitaSDK has 1.1.1) and src/game/warden_emulator.cpp an x86 emulator (unicorn). A Vita build
// compiles THIS file instead of those two; nothing else about Warden changes.
//
// WardenModule::load() refuses, which WardenHandler already handles as "no module": it logs the
// failure, drops the module and answers the server's checks the way it does when no module could be
// loaded. A server that insists on a module will not let the client in, so this is for private
// servers that do not (the same as running the desktop client with an unloadable module).
#include "game/warden_emulator.hpp"
#include "game/warden_module.hpp"

#include "core/logger.hpp"

namespace wowee {
namespace game {

WardenEmulator::WardenEmulator() = default;
WardenEmulator::~WardenEmulator() = default;

WardenModule::WardenModule() = default;
WardenModule::~WardenModule() = default;

bool WardenModule::load(const std::vector<uint8_t>&, const std::vector<uint8_t>&,
                        const std::vector<uint8_t>&) {
    LOG_WARNING("Warden: modules are not supported on this platform");
    return false;
}

bool WardenModule::processCheckRequest(const std::vector<uint8_t>&, std::vector<uint8_t>&) { return false; }

uint32_t WardenModule::tick(uint32_t) { return 0; }

void WardenModule::generateRC4Keys(uint8_t*) {}

void WardenModule::unload() {}

void WardenModule::setRsaModulus(std::vector<uint8_t> modulus) { rsaModulus_ = std::move(modulus); }

void WardenModule::setCallbackDependencies(WardenCrypto* crypto, SendPacketFunc sendFunc) {
    callbackCrypto_ = crypto;
    callbackSendPacket_ = std::move(sendFunc);
}

} // namespace game
} // namespace wowee
