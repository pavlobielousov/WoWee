#include "headless_client.hpp"

#include "core/data_paths.hpp"
#include "core/logger.hpp"
#include "core/memory_monitor.hpp"
#include "game/packet_parsers.hpp"

#include <cstdlib>
#include <unordered_map>
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

    const std::string character = envString("WOWEE_HEADLESS_CHARACTER");
    if (!character.empty()) {
        const long v = std::strtol(character.c_str(), nullptr, 10);
        if (v < 0 || v > 255) return "WOWEE_HEADLESS_CHARACTER must be a list index 0..255";
        o.character = static_cast<int>(v);
    }
    const std::string seconds = envString("WOWEE_HEADLESS_SECONDS");
    if (!seconds.empty()) {
        const long v = std::strtol(seconds.c_str(), nullptr, 10);
        if (v < 0 || v > 86400) return "WOWEE_HEADLESS_SECONDS must be 0..86400";
        o.runSeconds = static_cast<int>(v);
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
    bool requested = false;
    const bool done = pump(
        options_.phaseSeconds, [this] { tick(); },
        [&] {
            const auto state = authHandler_.getState();
            if (state == auth::AuthState::PIN_REQUIRED ||
                state == auth::AuthState::AUTHENTICATOR_REQUIRED) {
                needsMore = true;
                return true;
            }
            // The realm screen asks for the list once login has succeeded; nothing else does.
            if (state == auth::AuthState::AUTHENTICATED && !requested) {
                requested = true;
                authHandler_.requestRealmList();
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

int Client::enterWorldAndObserve() {
    const auto& chars = gameHandler_->getCharacters();
    if (options_.character >= static_cast<int>(chars.size())) {
        LOG_ERROR("Character index ", options_.character, " requested, the account has ",
                  chars.size());
        return kUsage;
    }
    const auto& chosen = chars[static_cast<size_t>(options_.character)];
    LOG_WARNING("Entering the world as ", chosen.name, " (guid ", chosen.guid, ")");
    gameHandler_->setActiveCharacterGuid(chosen.guid);
    gameHandler_->selectCharacter(chosen.guid);

    // IN_WORLD is set by SMSG_LOGIN_VERIFY_WORLD itself; no world-entry callback is needed.
    const bool done = pump(
        options_.phaseSeconds, [this] { tick(); },
        [this] {
            const auto state = gameHandler_->getState();
            return state == game::WorldState::IN_WORLD || state == game::WorldState::FAILED ||
                   state == game::WorldState::DISCONNECTED;
        });
    if (!done) {
        LOG_ERROR("Not in the world after ", options_.phaseSeconds, " s (world state ",
                  game::worldStateName(gameHandler_->getState()), ")");
        return kTimeout;
    }
    if (gameHandler_->getState() != game::WorldState::IN_WORLD) {
        LOG_ERROR("World entry failed (world state ",
                  game::worldStateName(gameHandler_->getState()), ")");
        return kWorld;
    }
    LOG_WARNING("In the world as ", chosen.name);
    observe();
    return kOk;
}

// Logs chat lines and entity arrivals and departures until the run limit, or until the server
// drops the connection (reported as a world failure by the caller's state check).
void Client::observe() {
    using Clock = std::chrono::steady_clock;
    const auto end = Clock::now() + std::chrono::seconds(options_.runSeconds);

    uint64_t lastChatUid = 0;
    for (const auto& m : gameHandler_->getChatHistory()) lastChatUid = std::max(lastChatUid, m.uid);

    struct Seen { game::ObjectType type; std::string name; };
    std::unordered_map<uint64_t, Seen> known;
    auto nextScan = Clock::now();

    while (Clock::now() < end && gameHandler_->getState() == game::WorldState::IN_WORLD) {
        tick();

        for (const auto& m : gameHandler_->getChatHistory()) {
            if (m.uid <= lastChatUid) continue;
            lastChatUid = m.uid;
            LOG_WARNING("[chat ", game::getChatTypeString(m.type), "] ",
                        m.senderName.empty() ? "-" : m.senderName, ": ", m.message);
        }

        if (Clock::now() >= nextScan) {
            nextScan = Clock::now() + std::chrono::milliseconds(500);
            std::unordered_map<uint64_t, bool> present;
            for (const auto& e : gameHandler_->getEntityManager().snapshotEntities()) {
                present[e->getGuid()] = true;
                auto it = known.find(e->getGuid());
                std::string name;
                if (auto* u = dynamic_cast<game::Unit*>(e.get())) name = u->getName();
                if (it == known.end()) {
                    known[e->getGuid()] = {e->getType(), name};
                    LOG_WARNING("[entity +] type ", static_cast<int>(e->getType()), " guid ",
                                e->getGuid(), name.empty() ? "" : " \"" + name + "\"", " at ",
                                e->getX(), ",", e->getY(), ",", e->getZ());
                } else if (it->second.name.empty() && !name.empty()) {
                    it->second.name = name;  // names arrive in a later query response
                    LOG_WARNING("[entity =] guid ", e->getGuid(), " is \"", name, "\"");
                }
            }
            for (auto it = known.begin(); it != known.end();) {
                if (!present.count(it->first)) {
                    LOG_WARNING("[entity -] guid ", it->first);
                    it = known.erase(it);
                } else {
                    ++it;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOG_WARNING("Run finished: ", known.size(), " entities known, world state ",
                game::worldStateName(gameHandler_->getState()));
}

void Client::shutdown() {
    if (gameHandler_) gameHandler_->disconnect();
    authHandler_.disconnect();
}

}  // namespace wowee::headless
