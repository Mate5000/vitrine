#pragma once
#include "AudioDSP.hpp"
#include "AudioDiagnostics.hpp"

struct SGQuickEQStatistics {
    std::atomic<uint64_t> callbacks{0}, processedBlocks{0}, processedFrames{0};
    std::atomic<uint64_t> unsupportedBlocks{0}, concurrentBlocks{0}, lastFailure{0}, currentFailure{0};
    std::atomic<uint32_t> sampleRate{0};
    std::atomic<bool> reachable{false};
};
sgquickeq::Mailbox &SGQuickEQConfigurationMailbox();
SGQuickEQStatistics &SGQuickEQAudioStatistics();
sgquickeq::Configuration SGQuickEQCurrentConfiguration();
void SGQuickEQSaveConfiguration(sgquickeq::Configuration value);
