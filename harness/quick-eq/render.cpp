#include "../../tweak/Sources/Shared/QuickEQ/QuickEQAudio.h"
#include "../../tweak/Sources/Shared/QuickEQ/QuickEQInternal.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

static AudioStreamBasicDescription current;
extern "C" OSStatus AudioUnitGetProperty(AudioUnit, UInt32, UInt32, UInt32, void *out, UInt32 *size) {
    assert(*size == sizeof current); std::memcpy(out, &current, sizeof current); return noErr;
}
sgquickeq::Mailbox &SGQuickEQConfigurationMailbox() { static sgquickeq::Mailbox box; return box; }
static void format(double rate, unsigned channels, bool planar, bool integer = false) {
    current = {}; current.mFormatID = kAudioFormatLinearPCM; current.mSampleRate = rate;
    current.mFormatFlags = (integer ? kAudioFormatFlagIsSignedInteger : kAudioFormatFlagIsFloat) | kAudioFormatFlagIsPacked |
                          (planar ? UInt32(kAudioFormatFlagIsNonInterleaved) : 0U);
    current.mBitsPerChannel = integer ? 16 : 32; current.mChannelsPerFrame = channels;
    current.mBytesPerFrame = (integer ? 2 : 4) * (planar ? 1 : channels);
}
int main() {
    SGQuickEQInitializeAudio(); SGQuickEQSetReachable(true);
    auto first = reinterpret_cast<AudioUnit>(uintptr_t(1)), second = reinterpret_cast<AudioUnit>(uintptr_t(2));
    std::array<float, 512> left, right, original;
    for (size_t i = 0; i < left.size(); ++i) left[i] = right[i] = original[i] = 0.2f * std::sin(double(i) * .01);
    struct { UInt32 count; AudioBuffer buffers[2]; } storage{2, {{1, sizeof(left), left.data()}, {1, sizeof(right), right.data()}}};
    auto *list = reinterpret_cast<AudioBufferList *>(&storage);
    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp time{}; time.mFlags = kAudioTimeStampSampleTimeValid;
    format(48000, 2, true); SGQuickEQPrepare(first);
    SGQuickEQProcess(first, 1, &flags, &time, left.size(), list);
    assert(left == original && right == original && SGQuickEQAudioStatistics().processedFrames.load() == 0);
    sgquickeq::Configuration config; config.enabled = config.bassEnabled = true; config.bassGain = 18; config.bassMaxGain = 18;
    config.eqEnabled = true; config.bands.fill(12);
    SGQuickEQConfigurationMailbox().publish(config);
    for (unsigned block = 0; block < 50; ++block) {
        for (size_t i = 0; i < left.size(); ++i) left[i] = right[i] = float(std::sin((block * 512 + i) * .01));
        time.mSampleTime = block * 512;
        SGQuickEQProcess(first, 1, &flags, &time, left.size(), list);
        for (size_t i = 0; i < left.size(); ++i) assert(std::isfinite(left[i]) && std::abs(left[i]) <= sgquickeq::OutputCeiling + 1e-6 && left[i] == right[i]);
    }
    assert(SGQuickEQAudioStatistics().processedFrames.load() == 50 * 512);
    // Different units have their own filter history. A new one, fed silence,
    // must not play the old one's bass tail; source epochs clear that tail too.
    SGQuickEQPrepare(second);
    left.fill(0); right.fill(0);
    SGQuickEQProcess(second, 1, &flags, &time, 512, list);
    for (float value : left) assert(value == 0);
    SGQuickEQProcess(first, 2, &flags, &time, 512, list);
    for (float value : left) assert(value == 0);
    // Unsupported layout is preserved, even when marked silent/uninitialized.
    left.fill(.7f); original = left; storage.buffers[0].mDataByteSize = 4;
    flags = kAudioUnitRenderAction_OutputIsSilence;
    auto failed = SGQuickEQAudioStatistics().unsupportedBlocks.load();
    SGQuickEQProcess(first, 2, &flags, &time, 512, list);
    assert(left == original && SGQuickEQAudioStatistics().unsupportedBlocks.load() == failed + 1);
    storage.buffers[0].mDataByteSize = sizeof(left);
    // Mono 16-bit and sample-rate/format changes use the same production adapter.
    format(44100, 1, false, true); SGQuickEQPrepare(first);
    std::array<int16_t, 512> mono{};
    AudioBufferList integer{1, {{1, sizeof(mono), mono.data()}}}; flags = 0;
    SGQuickEQProcess(first, 3, &flags, &time, 512, &integer);
    assert(SGQuickEQAudioStatistics().sampleRate.load() == 44100);
    // After the fade out, bypass is bit-exact and never clears a silence buffer.
    config.enabled = false; SGQuickEQConfigurationMailbox().publish(config);
    for (unsigned i = 0; i < 4; ++i) { time.mSampleTime += 512; SGQuickEQProcess(first, 3, &flags, &time, 512, &integer); }
    mono.fill(1234); auto saved = mono; flags = kAudioUnitRenderAction_OutputIsSilence;
    SGQuickEQProcess(first, 3, &flags, &time, 512, &integer); assert(mono == saved);
    // Measure the production callback's bass effect, not only its counters.
    config = {}; config.enabled = config.bassEnabled = true;
    SGQuickEQConfigurationMailbox().publish(config);
    format(48000, 2, true); SGQuickEQPrepare(first); flags = 0;
    double lowEnergy = 0, highEnergy = 0;
    constexpr double pi = 3.14159265358979323846;
    for (unsigned block = 0; block < 100; ++block) {
        for (size_t i = 0; i < left.size(); ++i) {
            const double t = double(block * left.size() + i) / 48000;
            left[i] = .1f * std::sin(2 * pi * 40 * t);
            right[i] = .1f * std::sin(2 * pi * 1000 * t);
        }
        time.mSampleTime = block * left.size();
        SGQuickEQProcess(first, 4, &flags, &time, 512, list);
        if (block > 20) for (size_t i = 0; i < left.size(); ++i) {
            lowEnergy += left[i] * left[i]; highEnergy += right[i] * right[i];
        }
    }
    assert(std::sqrt(lowEnergy / highEnergy) > 1.3);
    // An unknown output codec is left byte-exact, with a precise failure stage.
    current.mFormatID = 0x6F707573; SGQuickEQPrepare(first);
    original = left;
    SGQuickEQProcess(first, 4, &flags, &time, 512, list);
    assert(left == original);
    assert(sgquickeq::failureStage(SGQuickEQAudioStatistics().lastFailure.load()) == sgquickeq::FailureStage::RemoteIOFormat);
    SGQuickEQRenderNotifyStatus(-50);
    assert(sgquickeq::failureStage(SGQuickEQAudioStatistics().lastFailure.load()) == sgquickeq::FailureStage::RenderNotify);
    std::cout << "PASS: Spotify PCM adapter, isolated outputs, epochs, limiter, 44.1/48 kHz, mono/stereo, invalid format, exact bypass\n";
}
