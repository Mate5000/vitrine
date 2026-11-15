#pragma once
#include <AudioToolbox/AudioToolbox.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
// Control side: initialize once before installing the existing AudioEffects
// notify, then prepare its output format at each start/route/format change.
// Prepare calls must be serialized, as SGPlayer's output watchers already are.
void SGQuickEQInitializeAudio(void);
void SGQuickEQSetReachable(bool reachable);
void SGQuickEQPrepare(AudioUnit unit);
void SGQuickEQRenderNotifyStatus(OSStatus status);
// Called from AudioEffects' existing notify AFTER its effects and BEFORE the
// visualizer. This never installs another hook or takes Sing's source-stage slot.
void SGQuickEQProcess(AudioUnit unit, uint64_t sourceEpoch, AudioUnitRenderActionFlags *flags,
                      const AudioTimeStamp *time, UInt32 frames, AudioBufferList *data);
#ifdef __cplusplus
}
#endif
