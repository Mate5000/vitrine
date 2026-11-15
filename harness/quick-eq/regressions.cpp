#include "../../tweak/Sources/Shared/QuickEQ/QuickEQAudio.h"
#include "../../tweak/Sources/Shared/QuickEQ/QuickEQInternal.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

using namespace sgquickeq;
static AudioStreamBasicDescription current;
extern "C" OSStatus AudioUnitGetProperty(AudioUnit, UInt32, UInt32, UInt32, void *out, UInt32 *) {
    std::memcpy(out, &current, sizeof current); return noErr;
}
Mailbox &SGQuickEQConfigurationMailbox() { static Mailbox box; return box; }
static Configuration bass() { Configuration c; c.enabled = c.bassEnabled = true; return c; }
static void format() {
    current = {}; current.mFormatID = kAudioFormatLinearPCM; current.mSampleRate = 48000;
    current.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    current.mBitsPerChannel = 32; current.mChannelsPerFrame = 1; current.mBytesPerFrame = 4;
}
static bool limiterRelease() {
    for (double rate : {44100., 48000.}) {
        Engine engine(rate); engine.setConfiguration(bass(), true);
        for (unsigned i = 0; i < unsigned(rate); ++i) {
            float l = 3, r = l; engine.processFrame(l, r, 2);
        }
        for (unsigned i = 1; i < 2000; ++i) {
            float l = 3 - i * .00001f, r = l; engine.processFrame(l, r, 2);
            if (std::abs(l) > OutputCeiling + 1e-6) {
                std::cerr << "limiter release peak " << l << " > " << OutputCeiling << '\n';
                return false;
            }
        }
    }
    return true;
}
static bool midBufferBypass() {
    Engine engine(48000); engine.setConfiguration(bass(), true); engine.setConfiguration({});
    std::array<int32_t, 2048> samples; samples.fill(123456789);
    PCMBuffer buffer{samples.data(), sizeof(samples), 1};
    processPCM(engine, {48000, 1, SampleType::Signed32, true}, &buffer, 1, samples.size());
    // Fade takes 960 frames; every remaining int32 sample must stay bit-exact.
    for (unsigned i = 960; i < samples.size(); ++i) if (samples[i] != 123456789) return false;
    return true;
}
static bool routeRecovery() {
    auto unit = reinterpret_cast<AudioUnit>(uintptr_t(1));
    format(); SGQuickEQPrepare(unit);
    std::array<float, 512> samples; samples.fill(.2f);
    AudioBufferList data{1, {{1, sizeof(samples), samples.data()}}};
    AudioUnitRenderActionFlags flags = 0;
    SGQuickEQConfigurationMailbox().publish({});
    SGQuickEQProcess(unit, 1, &flags, nullptr, samples.size(), &data);
    // The user changes EQ while an unsupported output is selected.
    current.mChannelsPerFrame = 6; current.mBytesPerFrame = 24; SGQuickEQPrepare(unit);
    SGQuickEQConfigurationMailbox().publish(bass());
    SGQuickEQProcess(unit, 2, &flags, nullptr, samples.size(), &data);
    const auto failure = SGQuickEQAudioStatistics().currentFailure.load();
    if (!failure) return false;
    // An unavailable route must keep its error visible until recovery, even
    // after callbacks stop or the panel polls its status several times.
    SGQuickEQProcess(unit, 2, &flags, nullptr, samples.size(), &data);
    if (SGQuickEQAudioStatistics().currentFailure.load() != failure) return false;
    format(); SGQuickEQPrepare(unit);
    const auto before = SGQuickEQAudioStatistics().processedFrames.load();
    SGQuickEQProcess(unit, 3, &flags, nullptr, samples.size(), &data);
    return SGQuickEQAudioStatistics().processedFrames.load() == before + samples.size() &&
           SGQuickEQAudioStatistics().currentFailure.load() == 0 && SGQuickEQAudioStatistics().lastFailure.load() == failure;
}
static bool outputChurn() {
    format(); SGQuickEQConfigurationMailbox().publish(bass());
    for (unsigned i = 0; i < 160; ++i) {
        auto unit = reinterpret_cast<AudioUnit>(uintptr_t(1000 + i));
        SGQuickEQPrepare(unit);
        std::array<float, 64> samples{};
        AudioBufferList data{1, {{1, sizeof(samples), samples.data()}}};
        AudioUnitRenderActionFlags flags = 0;
        const auto before = SGQuickEQAudioStatistics().processedFrames.load();
        SGQuickEQProcess(unit, i, &flags, nullptr, samples.size(), &data);
        if (SGQuickEQAudioStatistics().processedFrames.load() != before + samples.size()) {
            std::cerr << "EQ stopped at new output " << i + 1 << '\n'; return false;
        }
        for (float sample : samples) if (sample != 0) return false;
        // Give each output history which must not leak to the next one.
        samples.fill(.2f); SGQuickEQProcess(unit, i, &flags, nullptr, samples.size(), &data);
    }
    return true;
}
static bool concurrentReplacement() {
    format(); SGQuickEQConfigurationMailbox().publish(bass());
    auto unit = reinterpret_cast<AudioUnit>(uintptr_t(2000)); SGQuickEQPrepare(unit);
    std::atomic<uintptr_t> selected{2000};
    std::atomic<bool> running{true}, started{false}, valid{true};
    const auto before = SGQuickEQAudioStatistics().processedFrames.load();
    std::thread render([&] {
        std::array<float, 64> samples{};
        AudioBufferList data{1, {{1, sizeof(samples), samples.data()}}};
        AudioUnitRenderActionFlags flags = 0;
        while (running.load()) {
            samples.fill(.1f);
            auto output = reinterpret_cast<AudioUnit>(selected.load());
            SGQuickEQProcess(output, 1, &flags, nullptr, samples.size(), &data);
            for (float sample : samples) if (!std::isfinite(sample) || std::abs(sample) > OutputCeiling + 1e-6) valid.store(false);
            started.store(true);
        }
    });
    while (!started.load()) std::this_thread::yield();
    for (uintptr_t i = 2001; i < 4001; ++i) {
        SGQuickEQPrepare(reinterpret_cast<AudioUnit>(i));
        selected.store(i);
    }
    running.store(false); render.join();
    return valid.load() && SGQuickEQAudioStatistics().processedFrames.load() > before;
}
static bool silentBufferBounds() {
    format(); SGQuickEQConfigurationMailbox().publish(bass());
    auto unit = reinterpret_cast<AudioUnit>(uintptr_t(5000)); SGQuickEQPrepare(unit);
    std::array<float, 128> samples; samples.fill(.625f);
    AudioBufferList data{1, {{1, sizeof(samples), samples.data()}}};
    AudioUnitRenderActionFlags flags = kAudioUnitRenderAction_OutputIsSilence;
    SGQuickEQProcess(unit, 1, &flags, nullptr, 64, &data);
    for (unsigned i = 0; i < 64; ++i) if (samples[i] != 0) return false;
    for (unsigned i = 64; i < 128; ++i) if (samples[i] != .625f) return false;
    return true;
}
int main() {
    SGQuickEQInitializeAudio();
    unsigned failures = 0;
    const auto check = [&](bool passed, const char *name) {
        std::cout << (passed ? "PASS: " : "FAIL: ") << name << '\n'; failures += !passed;
    };
    check(limiterRelease(), "limiter ceiling during slow release");
    check(midBufferBypass(), "int32 bypass from the first sample after the fade");
    check(routeRecovery(), "settings changed on an unsupported route apply after recovery");
    check(outputChurn(), "160 output replacements without exhaustion or filter-history leakage");
    check(concurrentReplacement(), "rendering during 2000 output replacements");
    check(silentBufferBounds(), "silence processing preserves storage after the requested frames");
    return failures ? 1 : 0;
}
