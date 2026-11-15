#pragma once
#include <cstdint>
typedef uint32_t UInt32;
typedef int32_t OSStatus;
typedef void *AudioUnit;
typedef uint32_t AudioUnitRenderActionFlags;
struct AudioBuffer { UInt32 mNumberChannels, mDataByteSize; void *mData; };
struct AudioBufferList { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; };
struct AudioTimeStamp { double mSampleTime; uint32_t mFlags; };
struct AudioStreamBasicDescription {
    double mSampleRate;
    UInt32 mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved;
};
enum : uint32_t {
    kAudioFormatLinearPCM = 0x6c70636d,
    kAudioFormatFlagIsFloat = 1, kAudioFormatFlagIsBigEndian = 2, kAudioFormatFlagIsSignedInteger = 4,
    kAudioFormatFlagIsPacked = 8, kAudioFormatFlagIsAlignedHigh = 16, kAudioFormatFlagIsNonInterleaved = 32,
    kLinearPCMFormatFlagsSampleFractionShift = 7, kLinearPCMFormatFlagsSampleFractionMask = 63 << 7,
    kAudioUnitRenderAction_PostRenderError = 256, kAudioUnitRenderAction_OutputIsSilence = 16,
    kAudioTimeStampSampleTimeValid = 1, kAudioUnitProperty_StreamFormat = 8, kAudioUnitScope_Output = 2,
    kAudioFormatUnsupportedDataFormatError = 0x666d743f,
};
constexpr OSStatus noErr = 0, kAudio_MemFullError = -108;
extern "C" OSStatus AudioUnitGetProperty(AudioUnit, UInt32, UInt32, UInt32, void *, UInt32 *);
