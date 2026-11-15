#include "QuickEQAudio.h"
#include "QuickEQInternal.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
using namespace sgquickeq;
struct Output {
    std::atomic<uintptr_t> unit{0};
    // Format and its generation are published as one consistent snapshot.
    std::atomic<uint64_t> format{0};
    uint32_t generation = 0; // control thread only
    uint64_t lastPrepared = 0; // control thread only
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    Engine engine;
    Configuration configuration;
    uint32_t revision = 0;
    uint64_t observedFormat = 0, observedEpoch = 0, sourceEpoch = 0;
    double expectedTime = 0;
    bool hasTime = false;
};
// Bounded storage outlives callbacks even if Core Audio disposes a unit while a
// render completes. Least-recently-prepared idle slots can be reused indefinitely.
// No map, allocation, Objective-C message or blocking lock on the render thread.
std::array<Output, 32> &outputs() { static std::array<Output, 32> slots; return slots; }
static_assert(std::atomic<uint64_t>::is_always_lock_free, "Audio atomics must not lock");
void fail(FailureStage stage, int32_t status) {
    auto &stats = SGQuickEQAudioStatistics();
    stats.lastFailure.store(failureValue(stage, status), std::memory_order_relaxed);
    stats.currentFailure.store(failureValue(stage, status), std::memory_order_relaxed);
    stats.unsupportedBlocks.fetch_add(1, std::memory_order_relaxed);
}
uint64_t packedFormat(const AudioStreamBasicDescription &f) {
    if (f.mFormatID != kAudioFormatLinearPCM || !std::isfinite(f.mSampleRate) || f.mSampleRate < 8000 ||
        f.mSampleRate > 192000 || f.mChannelsPerFrame < 1 || f.mChannelsPerFrame > 2 ||
        (f.mFormatFlags & (kAudioFormatFlagIsBigEndian | kAudioFormatFlagIsAlignedHigh))) return 0;
    SampleType type;
    const bool interleaved = !(f.mFormatFlags & kAudioFormatFlagIsNonInterleaved);
    if (f.mFormatFlags & kAudioFormatFlagIsFloat) {
        if (f.mBitsPerChannel != 32) return 0;
        type = SampleType::Float32;
    } else if (f.mFormatFlags & kAudioFormatFlagIsSignedInteger) {
        unsigned fraction = (f.mFormatFlags & kLinearPCMFormatFlagsSampleFractionMask) >> kLinearPCMFormatFlagsSampleFractionShift;
        if (f.mBitsPerChannel == 16 && !fraction) type = SampleType::Signed16;
        else if (f.mBitsPerChannel == 32 && !fraction) type = SampleType::Signed32;
        else if (f.mBitsPerChannel == 32 && fraction == 24) type = SampleType::Fixed824;
        else return 0;
    } else return 0;
    if (f.mBytesPerFrame != f.mBitsPerChannel / 8 * (interleaved ? f.mChannelsPerFrame : 1)) return 0;
    return uint64_t(std::llround(f.mSampleRate)) | uint64_t(f.mChannelsPerFrame) << 20 |
           uint64_t(type) << 24 | uint64_t(interleaved) << 28;
}
Output *findOutput(AudioUnit unit) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(unit);
    if (!key) return nullptr;
    for (auto &slot : outputs()) {
        const uintptr_t seen = slot.unit.load(std::memory_order_acquire);
        if (seen == key) return &slot;
    }
    return nullptr;
}
}
SGQuickEQStatistics &SGQuickEQAudioStatistics() { static SGQuickEQStatistics stats; return stats; }
void SGQuickEQInitializeAudio() {
    (void)outputs();
    (void)SGQuickEQAudioStatistics();
    (void)SGQuickEQConfigurationMailbox();
}
void SGQuickEQSetReachable(bool reachable) { SGQuickEQAudioStatistics().reachable.store(reachable); }
void SGQuickEQRenderNotifyStatus(OSStatus status) {
    if (status != noErr) fail(FailureStage::RenderNotify, status);
}
void SGQuickEQPrepare(AudioUnit unit) {
    if (!unit) return;
    // SGPlayer serializes its output watchers under sg_outputsLock. Only this
    // control-side function writes cache metadata; render never waits on it.
    static uint64_t serial = 0;
    AudioStreamBasicDescription format{};
    UInt32 size = sizeof format;
    OSStatus status = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, &size);
    const uint64_t word = status == noErr ? packedFormat(format) : 0;
    Output *output = findOutput(unit);
    if (output) {
        output->lastPrepared = ++serial;
        output->format.store(uint64_t(++output->generation) << 32 | word, std::memory_order_release);
    } else {
        std::array<Output *, 32> order{};
        for (size_t i = 0; i < order.size(); ++i) order[i] = &outputs()[i];
        std::sort(order.begin(), order.end(), [](const Output *a, const Output *b) { return a->lastPrepared < b->lastPrepared; });
        for (Output *slot : order) {
            if (slot->busy.test_and_set(std::memory_order_acquire)) continue;
            // A render may already have found this slot but not acquired it.
            // It rechecks the unit after acquiring busy and leaves old buffers
            // untouched if this slot now belongs to another output.
            slot->unit.store(0, std::memory_order_release);
            slot->engine = Engine();
            slot->configuration = {};
            slot->revision = 0;
            slot->observedFormat = slot->observedEpoch = slot->sourceEpoch = 0;
            slot->hasTime = false;
            slot->lastPrepared = ++serial;
            slot->format.store(uint64_t(++slot->generation) << 32 | word, std::memory_order_release);
            slot->unit.store(reinterpret_cast<uintptr_t>(unit), std::memory_order_release);
            slot->busy.clear(std::memory_order_release);
            output = slot;
            break;
        }
        if (!output) { fail(FailureStage::OutOfMemory, kAudio_MemFullError); return; }
    }
    if (!word) fail(FailureStage::RemoteIOFormat, status ? status : OSStatus(kAudioFormatUnsupportedDataFormatError));
    else SGQuickEQAudioStatistics().currentFailure.store(0, std::memory_order_relaxed);
}
void SGQuickEQProcess(AudioUnit unit, uint64_t sourceEpoch, AudioUnitRenderActionFlags *flags,
                      const AudioTimeStamp *time, UInt32 frames, AudioBufferList *data) {
    auto &stats = SGQuickEQAudioStatistics();
    stats.callbacks.fetch_add(1, std::memory_order_relaxed);
    if (!flags || (*flags & kAudioUnitRenderAction_PostRenderError) || !frames || !data) return;
    Output *output = findOutput(unit);
    if (!output) return;
    if (output->busy.test_and_set(std::memory_order_acquire)) {
        stats.concurrentBlocks.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    struct Release { Output *p; ~Release() { p->busy.clear(std::memory_order_release); } } release{output};
    if (output->unit.load(std::memory_order_acquire) != reinterpret_cast<uintptr_t>(unit)) return;
    const uint64_t snapshot = output->format.load(std::memory_order_acquire);
    const uint64_t word = snapshot & 0xffffffff, epoch = snapshot >> 32;
    if (!word) return;
    // Do not consume a settings revision until an engine can apply it. Otherwise
    // changes made on an unsupported route are lost when the old format returns.
    const bool changed = SGQuickEQConfigurationMailbox().read(output->configuration, output->revision);
    PCMFormat format{double(word & 0xfffff), unsigned((word >> 20) & 3), SampleType((word >> 24) & 7), bool((word >> 28) & 1)};
    if (word != output->observedFormat) {
        output->engine.reset(format.rate);
        output->engine.setConfiguration(output->configuration, true);
        output->observedFormat = word;
        output->hasTime = false;
    } else if (changed) output->engine.setConfiguration(output->configuration);
    if (epoch != output->observedEpoch || sourceEpoch != output->sourceEpoch) {
        output->engine.clearHistory(); output->hasTime = false;
        output->observedEpoch = epoch; output->sourceEpoch = sourceEpoch;
    }
    if (time && (time->mFlags & kAudioTimeStampSampleTimeValid)) {
        if (output->hasTime && std::abs(time->mSampleTime - output->expectedTime) > 1) output->engine.clearHistory();
        output->expectedTime = time->mSampleTime + frames; output->hasTime = true;
    }
    if (output->engine.isBypassed()) return;
    if (data->mNumberBuffers > 2) { fail(FailureStage::PCMBufferLayout, 0); return; }
    PCMBuffer buffers[2]{};
    for (UInt32 b = 0; b < data->mNumberBuffers; ++b)
        buffers[b] = {data->mBuffers[b].mData, data->mBuffers[b].mDataByteSize, data->mBuffers[b].mNumberChannels};
    if (!validPCM(format, buffers, data->mNumberBuffers, frames)) { fail(FailureStage::PCMBufferLayout, 0); return; }
    if (*flags & kAudioUnitRenderAction_OutputIsSilence)
        for (UInt32 b = 0; b < data->mNumberBuffers; ++b)
            std::memset(buffers[b].data, 0, size_t(frames) * buffers[b].channels * (format.type == SampleType::Signed16 ? 2 : 4));
    if (!processPCM(output->engine, format, buffers, data->mNumberBuffers, frames)) { fail(FailureStage::PCMBufferLayout, 0); return; }
    *flags &= ~kAudioUnitRenderAction_OutputIsSilence;
    stats.sampleRate.store(uint32_t(format.rate), std::memory_order_relaxed);
    stats.processedBlocks.fetch_add(1, std::memory_order_relaxed);
    stats.processedFrames.fetch_add(frames, std::memory_order_relaxed);
    stats.currentFailure.store(0, std::memory_order_relaxed);
}
