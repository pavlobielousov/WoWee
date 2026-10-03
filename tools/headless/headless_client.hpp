#pragma once

// wowee_headless (VITA-48): the steps of a console client that uses only wowee_core. It does by
// hand what Application and the login/realm/character screens do for the desktop client: load
// the expansion tables, authenticate, pick a realm, connect to the world server, list the
// characters. main.cpp (desktop) and the Vita main (VITA-48 PR 3) only fill in Options and call
// these steps, so the Vita build reads the same settings from env.txt.

#include "auth/auth_handler.hpp"
#include "game/expansion_profile.hpp"
#include "game/game_handler.hpp"
#include "game/game_services.hpp"
#include "pipeline/dbc_layout.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wowee::headless {

// Process exit codes, one per phase, so a script can tell what failed without parsing the log.
enum ExitCode : int {
    kOk = 0,
    kUsage = 2,      // missing or malformed option
    kTables = 3,     // expansion tables not found or unreadable, unknown expansion id
    kConnect = 4,    // TCP connect to the auth or world server failed
    kAuth = 5,       // login refused or no realm list
    kWorld = 6,      // world authentication failed, or no usable realm
    kTimeout = 7,    // a phase took longer than its limit
};

struct Options {
    std::string host;          // auth server
    uint16_t port = 3724;
    std::string account;
    std::string password;      // from the environment only, never logged
    std::string expansion;     // profile id: classic, tbc, wotlk, turtle, ...
    std::string realm;         // realm name; empty = the first realm in the list
    std::string dataRoot;      // where Data/expansions/<id>/ lives
    std::string installRoot;   // where this client's own tables are shipped ("Data")
    int phaseSeconds = 30;     // limit for each phase
    int character = 0;         // index in the character list to enter the world as
    int statsSeconds = 30;     // how often the resource line is logged while in the world
    int runSeconds = 20;       // how long to stay in the world, logging, before disconnecting
};

// Reads WOWEE_HEADLESS_{HOST,PORT,ACCOUNT,PASSWORD,EXPANSION,REALM,TIMEOUT} and WOW_DATA_PATH
// from the environment. Returns "" on success, or what is missing or malformed.
std::string optionsFromEnv(Options& out);

class Client {
public:
    explicit Client(Options options);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Tables, registry, handlers. kOk or kTables.
    int initialize();
    // Connect, authenticate, receive the realm list, connect to the chosen realm's world server,
    // wait for the character list. kOk, kConnect, kAuth, kWorld or kTimeout.
    int loginAndListCharacters();
    // Enters the world as options.character, waits for IN_WORLD, then logs new chat lines and
    // entities appearing or leaving for options.runSeconds. kOk, kUsage (no such character),
    // kWorld or kTimeout.
    int enterWorldAndObserve();
    // Disconnects both sockets; safe to call twice.
    void shutdown();

    const game::GameHandler& gameHandler() const { return *gameHandler_; }

private:
    int connectAuth();
    int waitForRealmList();
    int connectWorld();
    int waitForCharacterList();
    void observe();
    // Calls both handlers' update() once, with the seconds since the previous call.
    void tick();

    Options options_;
    game::ExpansionRegistry registry_;
    pipeline::DBCLayout dbcLayout_;
    game::GameServices services_;
    auth::AuthHandler authHandler_;
    std::unique_ptr<game::GameHandler> gameHandler_;
    std::vector<uint8_t> sessionKey_;
    std::string authFailure_;
    std::chrono::steady_clock::time_point lastTick_{};
    bool tablesActive_ = false;
};

}  // namespace wowee::headless
