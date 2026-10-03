#include "headless_client.hpp"

#include "core/data_paths.hpp"
#include "core/logger.hpp"
#include "core/memory_monitor.hpp"
#include "game/packet_parsers.hpp"

#include <cstdlib>
#include <functional>
#include <thread>

namespace wowee::headless {

namespace {

std::string envString(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// Where the world server's address in the realm list ends and its port begins: "host:port".
void splitRealmAddress(const std::string& address, std::string& host, uint16_t& port) {
    host = address;
    port = 8085;  // the default world server port, as the desktop client does
    const size_t colon = address.find(':');
    if (colon == std::string::npos) return;
    host = address.substr(0, colon);
    try {
        port = static_cast<uint16_t>(std::stoi(address.substr(colon + 1)));
    } catch (...) {
        LOG_WARNING("Invalid port in realm address: ", address);
    }
}

}  // namespace

std::string optionsFromEnv(Options& o) {
    o.host = envString("WOWEE_HEADLESS_HOST");
    o.account = envString("WOWEE_HEADLESS_ACCOUNT");
    o.password = envString("WOWEE_HEADLESS_PASSWORD");
    o.expansion = envString("WOWEE_HEADLESS_EXPANSION");
    o.realm = envString("WOWEE_HEADLESS_REALM");

    const std::string port = envString("WOWEE_HEADLESS_PORT");
    if (!port.empty()) {
        const long v = std::strtol(port.c_str(), nullptr, 10);
        if (v < 1 || v > 65535) return "WOWEE_HEADLESS_PORT is not a port number";
        o.port = static_cast<uint16_t>(v);
    }
    const std::string timeout = envString("WOWEE_HEADLESS_TIMEOUT");
    if (!timeout.empty()) {
        const long v = std::strtol(timeout.c_str(), nullptr, 10);
        if (v < 1 || v > 3600) return "WOWEE_HEADLESS_TIMEOUT must be 1..3600 seconds";
        o.phaseSeconds = static_cast<int>(v);
    }

    const std::string dataPath = envString("WOW_DATA_PATH");
    o.dataRoot = dataPath.empty() ? "./Data" : dataPath;
    o.installRoot = "Data";

    std::string missing;
    auto need = [&](const std::string& value, const char* name) {
        if (value.empty()) missing += std::string(missing.empty() ? "" : ", ") + name;
    };
    need(o.host, "WOWEE_HEADLESS_HOST");
    need(o.account, "WOWEE_HEADLESS_ACCOUNT");
    need(o.password, "WOWEE_HEADLESS_PASSWORD");
    need(o.expansion, "WOWEE_HEADLESS_EXPANSION");
    return missing.empty() ? std::string() : "missing: " + missing;
}

Client::Client(Options options) : options_(std::move(options)) {}

Client::~Client() {
    shutdown();
    if (tablesActive_) {
        game::setActiveOpcodeTable(nullptr);
        game::setActiveUpdateFieldTable(nullptr);
        pipeline::setActiveDBCLayout(nullptr);
        game::setActiveExpansionRegistry(nullptr);
    }
}

int Client::initialize() {
    core::MemoryMonitor::getInstance().initialize();

    // The client's own tables into the data root, then the registry scan, as Application does.
    std::vector<std::string> failures;
    const int copied = core::syncClientTables(options_.installRoot, options_.dataRoot, &failures);
    if (copied > 0) LOG_WARNING("Copied ", copied, " expansion tables into ", options_.dataRoot);
    for (const std::string& f : failures) LOG_WARNING("Could not write ", f);

    game::setActiveExpansionRegistry(&registry_);
    tablesActive_ = true;
    if (registry_.initialize(options_.dataRoot) == 0) {
        LOG_ERROR("No expansion tables under ", options_.dataRoot, "/expansions");
        return kTables;
    }
    // Application only logs these; a headless run cannot go on with the wrong profile.
    if (!registry_.setActive(options_.expansion)) {
        LOG_ERROR("Unknown expansion '", options_.expansion, "' in ", options_.dataRoot);
        return kTables;
    }
    const game::ExpansionProfile* profile = registry_.getActive();
    if (!profile) return kTables;

    services_.expansionRegistry = &registry_;
    gameHandler_ = std::make_unique<game::GameHandler>(services_);

    const std::string opcodes = profile->dataPath + "/opcodes.json";
    if (!gameHandler_->getOpcodeTable().loadFromJson(opcodes)) {
        LOG_ERROR("Failed to load opcodes from ", opcodes);
        return kTables;
    }
    game::setActiveOpcodeTable(&gameHandler_->getOpcodeTable());

    const std::string fields = profile->dataPath + "/update_fields.json";
    if (!gameHandler_->getUpdateFieldTable().loadFromJson(fields)) {
        LOG_ERROR("Failed to load update fields from ", fields);
        return kTables;
    }
    game::setActiveUpdateFieldTable(&gameHandler_->getUpdateFieldTable());

    auto parsers = game::createPacketParsers(profile->id);
    if (!parsers) {
        LOG_ERROR("No packet parsers for expansion '", profile->id, "'");
        return kTables;
    }
    gameHandler_->setPacketParsers(std::move(parsers));

    const std::string layouts = profile->dataPath + "/dbc_layouts.json";
    if (!dbcLayout_.loadFromJson(layouts)) {
        LOG_ERROR("Failed to load DBC layouts from ", layouts);
        return kTables;
    }
    pipeline::setActiveDBCLayout(&dbcLayout_);

    LOG_WARNING("Expansion ", profile->id, " (", profile->versionString(), ", build ",
                profile->build, ") tables loaded from ", profile->dataPath);
    lastTick_ = std::chrono::steady_clock::now();
    return kOk;
}

void Client::tick() {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - lastTick_).count();
    lastTick_ = now;
    authHandler_.update(dt);
    gameHandler_->update(dt);
}

namespace {

// Ticks `step` every 10 ms until `done` returns true or the limit passes. Returns true on done.
bool pump(int seconds, const std::function<void()>& step, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        step();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

int Client::connectAuth() {
    const game::ExpansionProfile* profile = registry_.getActive();

    authHandler_.setOnSuccess([this](const std::vector<uint8_t>& key) { sessionKey_ = key; });
    authHandler_.setOnFailure([this](const std::string& reason) { authFailure_ = reason; });

    // Exactly what AuthScreen::beginAuthAttempt sends. The vanilla family would retry with the
    // other protocol byte; add that here when a server needs it.
    auth::ClientInfo info;
    info.majorVersion = profile->majorVersion;
    info.minorVersion = profile->minorVersion;
    info.patchVersion = profile->patchVersion;
    info.build = profile->build;
    info.protocolVersion = profile->protocolVersion;
    info.game = profile->game;
    info.platform = profile->platform;
    info.os = profile->os;
    info.locale = profile->locale;
    info.timezone = profile->timezone;
    info.legacyVanillaRealmList = (profile->id == "classic" || profile->id == "turtle" ||
                                   profile->protocolVersion <= 3);
    authHandler_.setClientInfo(info);

    LOG_WARNING("Connecting to auth server ", options_.host, ":", options_.port, " (protocol ",
                static_cast<int>(info.protocolVersion), ")");
    if (!authHandler_.connect(options_.host, options_.port)) {
        LOG_ERROR("Could not connect to ", options_.host, ":", options_.port,
                  " - is the server up and the address right?");
        return kConnect;
    }
    authHandler_.authenticate(options_.account, options_.password);
    return kOk;
}

int Client::waitForRealmList() {
    bool needsMore = false;
    const bool done = pump(
        options_.phaseSeconds, [this] { tick(); },
        [&] {
            const auto state = authHandler_.getState();
            if (state == auth::AuthState::PIN_REQUIRED ||
                state == auth::AuthState::AUTHENTICATOR_REQUIRED) {
                needsMore = true;
                return true;
            }
            return state == auth::AuthState::REALM_LIST_RECEIVED ||
                   state == auth::AuthState::FAILED;
        });
    if (!done) {
        LOG_ERROR("No realm list from the auth server after ", options_.phaseSeconds, " s");
        return kTimeout;
    }
    if (needsMore) {
        LOG_ERROR("The account needs a PIN or an authenticator code; the headless client has none");
        return kAuth;
    }
    if (authHandler_.getState() == auth::AuthState::FAILED) {
        LOG_ERROR("Login failed: ", authFailure_.empty() ? "no reason given" : authFailure_);
        return kAuth;
    }
    if (authHandler_.getRealms().empty()) {
        LOG_ERROR("The auth server sent an empty realm list");
        return kAuth;
    }
    return kOk;
}

int Client::connectWorld() {
    const auto& realms = authHandler_.getRealms();
    const auth::Realm* realm = nullptr;
    for (const auto& r : realms) {
        LOG_WARNING("Realm ", static_cast<int>(r.id), ": ", r.name, " at ", r.address);
        if (!realm && (options_.realm.empty() || r.name == options_.realm)) realm = &r;
    }
    if (!realm) {
        LOG_ERROR("No realm named '", options_.realm, "' in the list");
        return kWorld;
    }

    std::string host;
    uint16_t port = 0;
    splitRealmAddress(realm->address, host, port);
    // MaNGOS/AzerothCore hand out an address the client may not be able to reach (127.0.0.1 from
    // inside a container, a public name on a LAN); the desktop client has the same override.
    if (const char* over = std::getenv("WOWEE_REALM_HOST_OVERRIDE"); over && *over) {
        LOG_WARNING("Overriding realm host '", host, "' with '", over, "'");
        host = over;
    }

    // The handler's key, or the one the success callback cached.
    std::vector<uint8_t> key = authHandler_.getSessionKey();
    if (key.empty()) key = sessionKey_;
    if (key.size() != 40) {
        LOG_ERROR("Auth session key has ", key.size(), " bytes; expected 40");
        return kAuth;
    }

    // The realm-reported build wins over the profile's world build (vanilla servers say 5875).
    uint32_t build = registry_.getActive()->worldBuild;
    if (realm->build != 0) build = realm->build;

    LOG_WARNING("Connecting to realm '", realm->name, "' world server ", host, ":", port,
                " (build ", build, ")");
    if (!gameHandler_->connect(host, port, key, authHandler_.getUsername(), build, realm->id)) {
        LOG_ERROR("Could not connect to the world server ", host, ":", port);
        return kConnect;
    }
    return kOk;
}

int Client::waitForCharacterList() {
    const bool done = pump(
        options_.phaseSeconds, [this] { tick(); },
        [this] {
            const auto state = gameHandler_->getState();
            return state == game::WorldState::CHAR_LIST_RECEIVED ||
                   state == game::WorldState::FAILED;
        });
    if (!done) {
        LOG_ERROR("No character list after ", options_.phaseSeconds, " s (world state ",
                  game::worldStateName(gameHandler_->getState()), ")");
        return kTimeout;
    }
    if (gameHandler_->getState() == game::WorldState::FAILED) {
        LOG_ERROR("World server login failed");
        return kWorld;
    }
    return kOk;
}

int Client::loginAndListCharacters() {
    if (int rc = connectAuth(); rc != kOk) return rc;
    if (int rc = waitForRealmList(); rc != kOk) return rc;
    if (int rc = connectWorld(); rc != kOk) return rc;
    if (int rc = waitForCharacterList(); rc != kOk) return rc;

    const auto& chars = gameHandler_->getCharacters();
    LOG_WARNING(chars.size(), " character(s) on this account");
    int index = 0;
    for (const auto& c : chars) {
        LOG_WARNING("  [", index++, "] ", c.name, ", level ", static_cast<int>(c.level),
                    ", guid ", c.guid);
    }
    return kOk;
}

void Client::shutdown() {
    if (gameHandler_) gameHandler_->disconnect();
    authHandler_.disconnect();
}

}  // namespace wowee::headless
