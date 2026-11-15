#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace sgquickeq {
constexpr std::size_t BandCount = 10;
constexpr std::array<double, BandCount> Frequencies = {{31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000}};
constexpr double OutputCeiling = 0.8912509381337456; // -1 dBFS, sample peak

struct Configuration {
    bool enabled = false;
    bool bassEnabled = false;
    bool eqEnabled = false;
    float bassFrequency = 80;
    float bassGain = 4;
    float bassMaxGain = 12;
    std::array<float, BandCount> bands{};
};
Configuration sanitize(Configuration config);

// One serialized producer (the settings controller), many readers. Every field is
// atomic; a bounded seqlock read never spins or races with a slider update.
class Mailbox {
public:
    Mailbox();
    void publish(Configuration config);
    bool read(Configuration &config, uint32_t &revision) const;
private:
    std::atomic<uint32_t> sequence_{0};
    std::array<std::atomic<uint32_t>, 16> words_{};
};

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    std::array<double, 2> z1{}, z2{};
    double process(double input, unsigned channel);
    double magnitude(double frequency, double rate) const;
};

// Per-stream state, fixed storage, no allocation or locks in any processing call.
// Changes crossfade two stable cascades over 20 ms instead of interpolating poles.
class Engine {
public:
    explicit Engine(double rate = 48000);
    void reset(double rate);
    void clearHistory();
    void setConfiguration(Configuration config, bool immediate = false);
    void processFrame(float &left, float &right, unsigned channels);
    bool isBypassed() const { return !fading_ && !current_.wet; }
    double headroomDB() const;
    static double responseDB(Configuration config, double rate, double frequency);
private:
    struct Bank {
        std::array<Biquad, BandCount + 1> filters{};
        double gain = 1;
        bool wet = false;
        double process(double input, unsigned channel);
    };
    static Bank prepare(Configuration config, double rate);
    void startTransition(Configuration config);
    double rate_ = 48000, limiterGain_ = 1, release_ = 0;
    Bank current_{}, next_{};
    Configuration target_{}, pending_{};
    unsigned fadeLength_ = 960, fadePosition_ = 0;
    bool fading_ = false, pendingChange_ = false;
};

enum class SampleType { Float32, Signed16, Signed32, Fixed824 };
struct PCMFormat {
    double rate = 0;
    unsigned channels = 0;
    SampleType type = SampleType::Float32;
    bool interleaved = true;
};
struct PCMBuffer { void *data; std::size_t bytes; unsigned channels; };
bool validPCM(const PCMFormat &format, const PCMBuffer *buffers, unsigned count, std::size_t frames);
bool processPCM(Engine &engine, const PCMFormat &format, PCMBuffer *buffers, unsigned count, std::size_t frames);
} // namespace sgquickeq
