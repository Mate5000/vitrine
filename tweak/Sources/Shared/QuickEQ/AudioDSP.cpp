#include "AudioDSP.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace sgquickeq {
static_assert(std::atomic<uint32_t>::is_always_lock_free, "The audio mailbox must be lock free");
static constexpr double Pi = 3.14159265358979323846;
static float bounded(float value, float low, float high, float fallback = 0) {
    return std::isfinite(value) ? std::max(low, std::min(high, value)) : fallback;
}
Configuration sanitize(Configuration c) {
    c.bassFrequency = bounded(c.bassFrequency, 30, 200, 80);
    c.bassMaxGain = bounded(c.bassMaxGain, 0, 18, 12);
    c.bassGain = bounded(c.bassGain, 0, c.bassMaxGain);
    for (float &gain : c.bands) gain = bounded(gain, -12, 12);
    return c;
}
static uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, sizeof u); return u; }
static float number(uint32_t u) { float f; std::memcpy(&f, &u, sizeof f); return f; }
Mailbox::Mailbox() { publish(Configuration{}); }
void Mailbox::publish(Configuration c) {
    c = sanitize(c);
    sequence_.fetch_add(1, std::memory_order_acq_rel);
    words_[0].store(c.enabled, std::memory_order_relaxed);
    words_[1].store(c.bassEnabled, std::memory_order_relaxed);
    words_[2].store(c.eqEnabled, std::memory_order_relaxed);
    words_[3].store(bits(c.bassFrequency), std::memory_order_relaxed);
    words_[4].store(bits(c.bassGain), std::memory_order_relaxed);
    words_[5].store(bits(c.bassMaxGain), std::memory_order_relaxed);
    for (std::size_t i = 0; i < BandCount; ++i) words_[i + 6].store(bits(c.bands[i]), std::memory_order_relaxed);
    sequence_.fetch_add(1, std::memory_order_release);
}
bool Mailbox::read(Configuration &c, uint32_t &revision) const {
    const uint32_t before = sequence_.load(std::memory_order_acquire);
    if ((before & 1) || before == revision) return false;
    Configuration candidate;
    candidate.enabled = words_[0].load(std::memory_order_relaxed);
    candidate.bassEnabled = words_[1].load(std::memory_order_relaxed);
    candidate.eqEnabled = words_[2].load(std::memory_order_relaxed);
    candidate.bassFrequency = number(words_[3].load(std::memory_order_relaxed));
    candidate.bassGain = number(words_[4].load(std::memory_order_relaxed));
    candidate.bassMaxGain = number(words_[5].load(std::memory_order_relaxed));
    for (std::size_t i = 0; i < BandCount; ++i) candidate.bands[i] = number(words_[i + 6].load(std::memory_order_relaxed));
    // This fence keeps the field reads before the second sequence read.
    std::atomic_thread_fence(std::memory_order_acq_rel);
    if (before != sequence_.load(std::memory_order_acquire)) return false;
    c = candidate;
    revision = before;
    return true;
}

static Biquad normalized(double b0, double b1, double b2, double a0, double a1, double a2) {
    Biquad filter;
    filter.b0 = b0 / a0; filter.b1 = b1 / a0; filter.b2 = b2 / a0;
    filter.a1 = a1 / a0; filter.a2 = a2 / a0;
    return filter;
}
static Biquad peak(double rate, double frequency, double gain) {
    if (gain == 0 || frequency >= rate * 0.49) return {};
    const double w = 2 * Pi * frequency / rate, s = std::sin(w), c = std::cos(w);
    const double a = std::pow(10.0, gain / 40.0);
    const double alpha = s * std::sinh(std::log(2.0) * 0.5 * w / s); // one octave
    return normalized(1 + alpha * a, -2 * c, 1 - alpha * a, 1 + alpha / a, -2 * c, 1 - alpha / a);
}
static Biquad shelf(double rate, double frequency, double gain) {
    if (gain == 0) return {};
    const double w = 2 * Pi * frequency / rate, c = std::cos(w);
    const double a = std::pow(10.0, gain / 40.0);
    const double beta = std::sqrt(2 * a) * std::sin(w); // shelf slope 1: monotonic, no resonant bump
    return normalized(a * ((a + 1) - (a - 1) * c + beta),
                      2 * a * ((a - 1) - (a + 1) * c),
                      a * ((a + 1) - (a - 1) * c - beta),
                      (a + 1) + (a - 1) * c + beta,
                      -2 * ((a - 1) + (a + 1) * c),
                      (a + 1) + (a - 1) * c - beta);
}
double Biquad::process(double input, unsigned channel) {
    const double out = b0 * input + z1[channel];
    z1[channel] = b1 * input - a1 * out + z2[channel];
    z2[channel] = b2 * input - a2 * out;
    // Clear subnormals and invalid state without changing the hardware FP mode.
    if (std::abs(z1[channel]) < 1e-24) z1[channel] = 0;
    if (std::abs(z2[channel]) < 1e-24) z2[channel] = 0;
    if (!std::isfinite(out)) { z1[channel] = z2[channel] = 0; return 0; }
    return out;
}
double Biquad::magnitude(double frequency, double rate) const {
    const double c = std::cos(2 * Pi * frequency / rate), c2 = 2 * c * c - 1;
    const double numerator = b0 * b0 + b1 * b1 + b2 * b2 + 2 * b1 * (b0 + b2) * c + 2 * b0 * b2 * c2;
    const double denominator = 1 + a1 * a1 + a2 * a2 + 2 * a1 * (1 + a2) * c + 2 * a2 * c2;
    return std::sqrt(std::max(numerator, 1e-24) / std::max(denominator, 1e-24));
}
double Engine::Bank::process(double input, unsigned channel) {
    if (!wet) return input;
    double value = input * gain;
    for (Biquad &filter : filters) value = filter.process(value, channel);
    return value;
}
Engine::Bank Engine::prepare(Configuration c, double rate) {
    c = sanitize(c);
    Bank bank;
    if (!c.enabled) return bank;
    if (c.bassEnabled && c.bassGain > 0) {
        bank.filters[0] = shelf(rate, c.bassFrequency, c.bassGain);
        bank.wet = true;
    }
    for (std::size_t i = 0; i < BandCount; ++i) {
        if (c.eqEnabled && c.bands[i] != 0 && Frequencies[i] < rate * 0.49) {
            bank.filters[i + 1] = peak(rate, Frequencies[i], c.bands[i]);
            bank.wet = true;
        }
    }
    if (!bank.wet) return bank;
    // Account for overlapping filters, including the bass shelf, rather than
    // treating the largest individual slider as the combined response.
    double maximum = 1;
    const auto measure = [&](double f) {
        double magnitude = 1;
        for (const auto &filter : bank.filters) magnitude *= filter.magnitude(f, rate);
        maximum = std::max(maximum, magnitude);
    };
    for (unsigned i = 0; i <= 512; ++i) measure(5 * std::pow(rate * 0.098, i / 512.0));
    for (double f : Frequencies) if (f < rate * 0.49) measure(f);
    bank.gain = OutputCeiling / maximum;
    return bank;
}
Engine::Engine(double rate) { reset(rate); }
void Engine::reset(double rate) {
    rate_ = std::isfinite(rate) && rate >= 8000 && rate <= 192000 ? rate : 48000;
    fadeLength_ = std::max(1u, static_cast<unsigned>(rate_ * 0.020));
    release_ = std::exp(-1.0 / (rate_ * 0.050));
    current_ = prepare(target_, rate_);
    next_ = current_;
    fading_ = pendingChange_ = false;
    fadePosition_ = 0; limiterGain_ = 1;
}
void Engine::clearHistory() {
    for (Bank *bank : {&current_, &next_}) for (auto &filter : bank->filters) { filter.z1 = {}; filter.z2 = {}; }
    limiterGain_ = 1;
}
void Engine::startTransition(Configuration c) {
    next_ = prepare(c, rate_);
    target_ = c;
    if (!current_.wet && !next_.wet) {
        current_ = next_; fading_ = false; fadePosition_ = 0;
        return;
    }
    fading_ = true;
    fadePosition_ = 0;
}
void Engine::setConfiguration(Configuration c, bool immediate) {
    c = sanitize(c);
    if (immediate) {
        target_ = c; current_ = prepare(c, rate_); next_ = current_;
        fading_ = pendingChange_ = false; limiterGain_ = 1;
    } else if (fading_) {
        pending_ = c; pendingChange_ = true;
    } else startTransition(c);
}
void Engine::processFrame(float &left, float &right, unsigned channels) {
    if (channels < 1 || channels > 2) return;
    const double in[2] = {std::isfinite(left) ? left : 0, std::isfinite(right) ? right : 0};
    double out[2] = {};
    const double mix = fading_ ? static_cast<double>(fadePosition_ + 1) / fadeLength_ : 0;
    for (unsigned ch = 0; ch < channels; ++ch) {
        out[ch] = current_.process(in[ch], ch);
        if (fading_) out[ch] += mix * (next_.process(in[ch], ch) - out[ch]);
    }
    const double wet = fading_ ? (current_.wet ? 1 - mix : 0) + (next_.wet ? mix : 0) : (current_.wet ? 1 : 0);
    if (wet > 0) {
        const double ceiling = 1 + wet * (OutputCeiling - 1);
        const double amplitude = std::max(std::abs(out[0]), std::abs(out[1]));
        const double required = amplitude > ceiling ? ceiling / amplitude : 1;
        // Release toward unity, but never past the gain this sample permits.
        limiterGain_ = std::min(required, 1 - (1 - limiterGain_) * release_);
        out[0] *= limiterGain_; out[1] *= limiterGain_;
    } else limiterGain_ = 1;
    left = static_cast<float>(out[0]);
    if (channels == 2) right = static_cast<float>(out[1]);
    if (fading_ && ++fadePosition_ == fadeLength_) {
        current_ = next_; fading_ = false;
        if (pendingChange_) { pendingChange_ = false; startTransition(pending_); }
    }
}
double Engine::headroomDB() const { return 20 * std::log10((fading_ ? next_ : current_).gain); }
double Engine::responseDB(Configuration c, double rate, double frequency) {
    const Bank bank = prepare(c, rate);
    double magnitude = 1;
    for (const auto &filter : bank.filters) magnitude *= filter.magnitude(frequency, rate);
    return 20 * std::log10(magnitude);
}

static std::size_t bytesPerSample(SampleType type) { return type == SampleType::Signed16 ? 2 : 4; }
bool validPCM(const PCMFormat &f, const PCMBuffer *buffers, unsigned count, std::size_t frames) {
    if (!buffers || f.channels < 1 || f.channels > 2 || !std::isfinite(f.rate) || f.rate < 8000 || f.rate > 192000) return false;
    const unsigned expected = f.interleaved ? 1 : f.channels;
    if (count != expected || frames > std::numeric_limits<std::size_t>::max() / (8 * f.channels)) return false;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned channels = f.interleaved ? f.channels : 1;
        if (!buffers[i].data || buffers[i].channels != channels || buffers[i].bytes < frames * channels * bytesPerSample(f.type)) return false;
    }
    return true;
}
template<typename T> static T loadSample(const void *base, std::size_t index) {
    T value; std::memcpy(&value, static_cast<const char *>(base) + index * sizeof(T), sizeof(T)); return value;
}
template<typename T> static void storeSample(void *base, std::size_t index, T value) {
    std::memcpy(static_cast<char *>(base) + index * sizeof(T), &value, sizeof(T));
}
static float readSample(const void *data, std::size_t at, SampleType type) {
    switch (type) {
        case SampleType::Float32: return loadSample<float>(data, at);
        case SampleType::Signed16: return loadSample<int16_t>(data, at) / 32768.0f;
        case SampleType::Signed32: return static_cast<float>(loadSample<int32_t>(data, at) / 2147483648.0);
        case SampleType::Fixed824: return static_cast<float>(loadSample<int32_t>(data, at) / 16777216.0);
    }
    return 0;
}
static void writeSample(void *data, std::size_t at, SampleType type, float value) {
    if (type == SampleType::Float32) { storeSample(data, at, value); return; }
    const double scale = type == SampleType::Signed16 ? 32768.0 : type == SampleType::Fixed824 ? 16777216.0 : 2147483648.0;
    const double low = type == SampleType::Signed16 ? -32768.0 : -2147483648.0;
    const double high = type == SampleType::Signed16 ? 32767.0 : 2147483647.0;
    const double rounded = std::nearbyint(std::max(low, std::min(high, value * scale)));
    if (type == SampleType::Signed16) storeSample(data, at, static_cast<int16_t>(rounded));
    else storeSample(data, at, static_cast<int32_t>(rounded));
}
bool processPCM(Engine &engine, const PCMFormat &f, PCMBuffer *buffers, unsigned count, std::size_t frames) {
    if (!validPCM(f, buffers, count, frames)) return false;
    if (engine.isBypassed()) return true; // Preserve integer PCM exactly, including all 32 bits.
    for (std::size_t i = 0; i < frames; ++i) {
        // A fade can end inside this buffer. Do not round the dry tail through
        // float32 (which cannot preserve every signed32 PCM sample).
        if (engine.isBypassed()) break;
        const std::size_t at = f.interleaved ? i * f.channels : i;
        void *leftData = buffers[0].data;
        void *rightData = f.interleaved || f.channels == 1 ? leftData : buffers[1].data;
        const std::size_t rightAt = at + (f.interleaved && f.channels == 2 ? 1 : 0);
        float left = readSample(leftData, at, f.type);
        float right = f.channels == 2 ? readSample(rightData, rightAt, f.type) : left;
        engine.processFrame(left, right, f.channels);
        writeSample(leftData, at, f.type, left);
        if (f.channels == 2) writeSample(rightData, rightAt, f.type, right);
    }
    return true;
}
} // namespace sgquickeq
