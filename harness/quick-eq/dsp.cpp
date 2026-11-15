#include "../../tweak/Sources/Shared/QuickEQ/AudioDSP.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <thread>
#include <vector>

static std::atomic<unsigned long> allocations{0};
void *operator new(std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

using namespace sgquickeq;
constexpr double Pi = 3.14159265358979323846;
static unsigned checks = 0;
static void check(bool condition, const char *message) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static void near(double actual, double expected, double tolerance, const char *message) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "actual=" << actual << " expected=" << expected << '\n'; check(false, message);
    }
    ++checks;
}
static Configuration bass() {
    Configuration c; c.enabled = c.bassEnabled = true; return c;
}
static double sineResponse(Configuration config, double rate, double frequency) {
    Engine engine(rate); engine.setConfiguration(config, true);
    double inEnergy = 0, outEnergy = 0;
    for (unsigned i = 0; i < unsigned(rate * 2); ++i) {
        float input = float(0.01 * std::sin(2 * Pi * frequency * i / rate));
        float left = input, right = 0;
        engine.processFrame(left, right, 1);
        if (i >= unsigned(rate)) { inEnergy += input * input; outEnergy += left * left; }
    }
    return 10 * std::log10(outEnergy / inEnergy) - engine.headroomDB();
}
static void responses() {
    for (double rate : {44100.0, 48000.0}) {
        for (std::size_t band = 0; band < BandCount; ++band) {
            Configuration c; c.enabled = c.eqEnabled = true; c.bands[band] = 6;
            near(Engine::responseDB(c, rate, Frequencies[band]), 6, 0.001, "EQ band center is +6 dB");
            near(sineResponse(c, rate, Frequencies[band]), 6, 0.025, "rendered EQ sine matches its response");
            c.bands[band] = -12;
            near(Engine::responseDB(c, rate, Frequencies[band]), -12, 0.001, "EQ cut at its center");
        }
        auto c = bass(); c.bassGain = 12;
        near(Engine::responseDB(c, rate, 2), 12, 0.001, "bass shelf DC gain");
        near(Engine::responseDB(c, rate, 80), 6, 0.001, "bass frequency is the shelf midpoint");
        near(sineResponse(c, rate, 40), Engine::responseDB(c, rate, 40), 0.03, "rendered bass boost");
        double last = 13;
        for (double f : {5.0, 20.0, 40.0, 80.0, 160.0, 400.0, 2000.0}) {
            double response = Engine::responseDB(c, rate, f);
            check(response <= last + 0.001 && response >= -0.001, "bass shelf is monotonic with no resonant bump"); last = response;
        }
        c.bassFrequency = 160;
        near(Engine::responseDB(c, rate, 160), 6, 0.001, "bass frequency adjustment");
        c.bassMaxGain = 3;
        near(Engine::responseDB(c, rate, 2), 3, 0.001, "bass gain obeys its ceiling");
    }
}
static void limitsAndTransitions() {
    for (double rate : {44100.0, 48000.0}) {
        auto c = bass(); c.eqEnabled = true; c.bassMaxGain = c.bassGain = 18; c.bands.fill(12);
        Engine engine(rate); engine.setConfiguration(c, true);
        check(engine.headroomDB() < -25, "overlapping EQ and bass get combined headroom");
        uint32_t random = 1;
        for (unsigned i = 0; i < unsigned(rate); ++i) {
            random = random * 1664525 + 1013904223;
            float left = float(int32_t(random) / 2147483648.0 * 4), right = -left;
            engine.processFrame(left, right, 2);
            check(std::isfinite(left) && std::isfinite(right), "extreme input remains finite");
            check(std::abs(left) <= OutputCeiling + 1e-6 && std::abs(right) <= OutputCeiling + 1e-6, "limiter enforces -1 dBFS");
            near(left, -right, 1e-6, "stereo-linked limiter preserves balance");
        }
        Engine transition(rate);
        float previous = 0.2f;
        transition.setConfiguration(c);
        for (unsigned i = 0; i < unsigned(rate / 2); ++i) {
            if (i % 137 == 0) { auto next = bass(); next.bassGain = float(i % 13); next.enabled = (i % 3) != 0; transition.setConfiguration(next); }
            float left = 0.2f, right = left;
            transition.processFrame(left, right, 2);
            check(std::abs(left - previous) < 0.05, "rapid parameter changes do not make a hard step");
            check(std::isfinite(left), "rapid changes are stable"); previous = left;
        }
        transition.setConfiguration({});
        for (unsigned i = 0; i < unsigned(rate / 10); ++i) { float l = 0.7f, r = 0.1f; transition.processFrame(l, r, 2); }
        check(transition.isBypassed(), "disabled engine settles to exact bypass");
        float left = 0.999999f, right = -0.812345f;
        transition.processFrame(left, right, 2);
        check(left == 0.999999f && right == -0.812345f, "disabled output is unchanged");
        const auto before = allocations.load();
        transition.setConfiguration(c);
        for (unsigned i = 0; i < 10000; ++i) { float l = 0.1f, r = 0.2f; transition.processFrame(l, r, 2); }
        check(allocations.load() == before, "configuration and processing allocate no memory");
    }
}
static void pcmAdapters() {
    for (double rate : {44100.0, 48000.0}) {
        Engine dry(rate);
        int32_t integers[] = {INT32_MAX, INT32_MIN, 123456789, -111111111};
        int32_t original[4]; std::memcpy(original, integers, sizeof integers);
        PCMFormat format{rate, 2, SampleType::Signed32, true};
        PCMBuffer buffer{integers, sizeof integers, 2};
        check(processPCM(dry, format, &buffer, 1, 2), "int32 stereo PCM accepted");
        check(std::memcmp(integers, original, sizeof integers) == 0, "int32 bypass is bit exact");
        Configuration flat; flat.enabled = flat.eqEnabled = true;
        dry.setConfiguration(flat);
        check(dry.isBypassed(), "flat configuration does not start a needless transition");
        check(processPCM(dry, format, &buffer, 1, 2), "flat int32 PCM accepted");
        check(std::memcmp(integers, original, sizeof integers) == 0, "flat int32 settings update remains bit exact");
        format.channels = 1; format.type = SampleType::Signed16;
        int16_t samples[] = {INT16_MIN, INT16_MAX, 0, 12345, -12345};
        buffer = {samples, sizeof samples, 1};
        check(processPCM(dry, format, &buffer, 1, 5), "mono int16 accepted");
        check(samples[0] == INT16_MIN && samples[4] == -12345, "int16 bypass is exact");
        auto c = bass(); c.eqEnabled = true; c.bands[4] = 6;
        Engine mono(rate), stereo(rate); mono.setConfiguration(c, true); stereo.setConfiguration(c, true);
        float left[2048], right[2048], one[2048];
        for (unsigned i = 0; i < 2048; ++i) left[i] = one[i] = float(std::sin(2 * Pi * 500 * i / rate) * 0.1);
        std::fill(std::begin(right), std::end(right), 0);
        PCMFormat planar{rate, 2, SampleType::Float32, false};
        PCMBuffer planes[] = {{left, sizeof left, 1}, {right, sizeof right, 1}};
        check(processPCM(stereo, planar, planes, 2, 2048), "planar stereo PCM accepted");
        PCMFormat monoFormat{rate, 1, SampleType::Float32, true};
        PCMBuffer monoBuffer{one, sizeof one, 1};
        check(processPCM(mono, monoFormat, &monoBuffer, 1, 2048), "mono float PCM accepted");
        for (unsigned i = 0; i < 2048; ++i) {
            near(left[i], one[i], 1e-7, "mono matches stereo left channel");
            check(right[i] == 0, "filter state does not leak across channels");
        }
        float invalid[] = {0.25f, -0.5f};
        PCMBuffer shortBuffer{invalid, sizeof(float), 2};
        PCMFormat packed{rate, 2, SampleType::Float32, true};
        check(!processPCM(stereo, packed, &shortBuffer, 1, 1), "short buffer rejected");
        check(invalid[0] == 0.25f && invalid[1] == -0.5f, "unsupported layout stays unchanged");
        packed.channels = 6;
        check(!processPCM(stereo, packed, &shortBuffer, 1, 1), "unsupported channel layout rejected");
        stereo.reset(44100); stereo.setConfiguration(c, true);
        float l = 0, r = 0; stereo.processFrame(l, r, 2);
        check(l == 0 && r == 0, "sample rate change clears old filter state");
    }
}
static void settingsAndMailbox() {
    auto c = bass(); c.bassMaxGain = -4; c.bassGain = 999; c.bassFrequency = NAN; c.bands[0] = INFINITY;
    c = sanitize(c);
    check(c.bassMaxGain == 0 && c.bassGain == 0 && c.bassFrequency == 80 && c.bands[0] == 0, "invalid stored values are sanitized");
    Mailbox mailbox;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (unsigned i = 0; i < 100000; ++i) {
            Configuration value; value.enabled = true; value.bassFrequency = i % 2 ? 80 : 120;
            value.bands.fill(i % 2 ? 4 : -3); mailbox.publish(value);
        }
        done.store(true, std::memory_order_release);
    });
    uint32_t revision = 0; Configuration current;
    do {
        if (mailbox.read(current, revision) && current.enabled)
            for (float gain : current.bands) check(gain == (current.bassFrequency == 80 ? 4 : -3), "mailbox snapshot is consistent during concurrent writes");
    } while (!done.load(std::memory_order_acquire));
    writer.join();
}
int main() {
    responses(); limitsAndTransitions(); pcmAdapters(); settingsAndMailbox();
    std::cout << "PASS: " << checks << " DSP, PCM, transition, headroom and concurrency checks\n";
}
