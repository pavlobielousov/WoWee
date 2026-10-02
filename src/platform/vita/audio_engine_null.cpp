// A silent AudioEngine (VITA-46): the same class as src/audio/audio_engine.cpp with no miniaudio,
// for builds that have no audio backend (the headless core and, until VITA-29, the Vita).
//
// Build this file INSTEAD of src/audio/audio_engine.cpp, never both. Every other audio source
// (the managers, AudioCoordinator) compiles unchanged: audio_engine.hpp only forward-declares the
// miniaudio types, so nothing else in the layer includes miniaudio.
//
// initialize() fails on purpose. AudioCoordinator already handles that (it logs "audio will be
// disabled", creates no managers, and every getter returns nullptr), and all of game/ already
// checks those pointers, because a desktop with no sound device takes the same path. So no game
// code needs a seam for audio.
#include "audio/audio_engine.hpp"

namespace wowee {
namespace audio {

AudioEngine& AudioEngine::instance() {
    static AudioEngine inst;
    return inst;
}

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() = default;

bool AudioEngine::initialize() { return false; }
void AudioEngine::shutdown() {}
std::string AudioEngine::getOutputDeviceName() const { return {}; }

// Volumes are remembered so the getters answer what was set, as on a real device.
void AudioEngine::setMasterVolume(float volume) { masterVolume_ = volume; }
void AudioEngine::setSuspended(bool suspended) { suspended_ = suspended; }

void AudioEngine::setListenerPosition(const glm::vec3& position) { listenerPosition_ = position; }
void AudioEngine::setListenerOrientation(const glm::vec3& forward, const glm::vec3& up) {
    listenerForward_ = forward;
    listenerUp_ = up;
}

bool AudioEngine::playSound2D(const std::vector<uint8_t>&, float, float) { return false; }
bool AudioEngine::playSound2D(const std::string&, float, float) { return false; }
uint32_t AudioEngine::playSound2DStoppable(const std::vector<uint8_t>&, float) { return 0; }
void AudioEngine::stopSound(uint32_t) {}
bool AudioEngine::playSound3D(const std::vector<uint8_t>&, const glm::vec3&, float, float, float) {
    return false;
}
bool AudioEngine::playSound3D(const std::string&, const glm::vec3&, float, float, float) {
    return false;
}

bool AudioEngine::playMusic(std::shared_ptr<const std::vector<uint8_t>>, float, bool) { return false; }
void AudioEngine::stopMusic() {}
bool AudioEngine::isMusicPlaying() const { return false; }
void AudioEngine::setMusicVolume(float volume) { musicVolume_ = volume; }

void AudioEngine::update(float) {}

bool AudioEngine::beginOutputCapture() { return false; }
void AudioEngine::endOutputCapture() {}
uint32_t AudioEngine::readCapturedOutput(float*, uint32_t) { return 0; }
uint32_t AudioEngine::capturedOutputAvailable() const { return 0; }
uint32_t AudioEngine::getOutputChannels() const { return 0; }
uint32_t AudioEngine::getOutputSampleRate() const { return 0; }

} // namespace audio
} // namespace wowee
