// Host test for src/platform/vita/audio_engine_null.cpp (VITA-46). Needs glm only:
//   c++ -std=c++20 -Wall -Wextra -Werror -Iinclude tools/vita/depcheck/audio_null_test.cpp
//       src/platform/vita/audio_engine_null.cpp -o /tmp/audio_null_test && /tmp/audio_null_test
// (one command line; the Mac host has no glm, so run it in the desktop container, see DEV_SETUP.md)
#include "audio/audio_engine.hpp"

#include <cstdio>

using wowee::audio::AudioEngine;
static int g_fail = 0;

static void check(const char* name, bool ok) {
    if (!ok) ++g_fail;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
}

int main() {
    auto& a = AudioEngine::instance();
    check("singleton", &a == &AudioEngine::instance());
    check("initialize fails (AudioCoordinator then disables audio)", !a.initialize());
    check("not initialized", !a.isInitialized());
    a.shutdown();
    check("device name empty", a.getOutputDeviceName().empty());

    a.setMasterVolume(0.25f);
    check("master volume remembered", a.getMasterVolume() == 0.25f);
    a.setSuspended(true);
    check("suspended remembered", a.isSuspended());
    a.setListenerPosition({1.f, 2.f, 3.f});
    check("listener position remembered", a.getListenerPosition().y == 2.f);
    a.setListenerOrientation({0.f, 0.f, -1.f}, {0.f, 1.f, 0.f});
    a.setAssetManager(nullptr);

    const std::vector<uint8_t> wav(16, 0);
    check("playSound2D(data) false", !a.playSound2D(wav));
    check("playSound2D(path) false", !a.playSound2D(std::string("x.wav")));
    check("stoppable handle is 0", a.playSound2DStoppable(wav) == 0);
    a.stopSound(0);
    check("playSound3D(data) false", !a.playSound3D(wav, {0.f, 0.f, 0.f}));
    check("playSound3D(path) false", !a.playSound3D(std::string("x.wav"), {0.f, 0.f, 0.f}));
    check("playMusic false", !a.playMusic(std::make_shared<const std::vector<uint8_t>>(wav)));
    a.stopMusic();
    a.setMusicVolume(0.5f);
    check("music not playing", !a.isMusicPlaying());
    a.update(0.016f);

    float buf[8];
    check("capture refused", !a.beginOutputCapture());
    a.endOutputCapture();
    check("nothing captured", a.readCapturedOutput(buf, 8) == 0 && a.capturedOutputAvailable() == 0);
    check("no channels / rate", a.getOutputChannels() == 0 && a.getOutputSampleRate() == 0);

    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
