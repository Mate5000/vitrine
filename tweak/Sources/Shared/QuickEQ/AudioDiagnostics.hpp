#pragma once
#include <atomic>
#include <cstdint>

namespace sgquickeq {
enum class FailureStage : uint32_t {
    None, PCMBufferLayout, OutOfMemory, RemoteIOFormat, RenderNotify
};
constexpr uint64_t failureValue(FailureStage stage, int32_t status) {
    return (uint64_t(stage) << 32) | uint32_t(status);
}
constexpr FailureStage failureStage(uint64_t value) { return FailureStage(value >> 32); }
constexpr int32_t failureStatus(uint64_t value) { return int32_t(uint32_t(value)); }
inline const char *failureStageName(FailureStage stage) {
    switch (stage) {
        case FailureStage::None: return "No failure recorded";
        case FailureStage::PCMBufferLayout: return "Validate PCM buffer layout";
        case FailureStage::OutOfMemory: return "Reserve a Quick EQ output slot";
        case FailureStage::RemoteIOFormat: return "Read RemoteIO PCM stream format";
        case FailureStage::RenderNotify: return "AudioUnitAddRenderNotify";
    }
    return "Unknown stage";
}
} // namespace sgquickeq
