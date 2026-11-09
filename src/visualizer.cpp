#include "visualizer.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

#ifdef CRATE_HAVE_PULSE
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

namespace {

constexpr int kRate = 48000;  // PipeWire's usual rate, so nothing gets resampled
constexpr int kFft = 2048;    // samples per FFT: ~43 ms, 23 Hz per bin
[[maybe_unused]] constexpr int kChunk = 512;  // samples per read: ~11 ms
constexpr float kRange = 45;  // dB shown from the bottom of a bar to the top

// In-place radix-2 FFT. a.size() must be a power of two.
void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {  // reorder into bit-reversed index order
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float angle = -2 * std::numbers::pi_v<float> / static_cast<float>(len);
        std::complex<float> step(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                auto even = a[i + k], odd = a[i + k + len / 2] * w;
                a[i + k] = even + odd;
                a[i + k + len / 2] = even - odd;
                w *= step;
            }
        }
    }
}

}  // namespace

std::vector<float> spectrum_db(const std::vector<float>& samples, int rate, int n_bars) {
    const int n = static_cast<int>(samples.size());
    std::vector<std::complex<float>> a(n);
    for (int i = 0; i < n; ++i) {  // Hann window: fades the ends so bins don't smear
        float w = 0.5f - 0.5f * std::cos(2 * std::numbers::pi_v<float> * i / (n - 1));
        a[i] = samples[i] * w;
    }
    fft(a);

    // Amplitude per bin, scaled so a full-volume sine wave reads 1 (0 dB).
    std::vector<float> amp(n / 2 + 1);
    for (int k = 0; k <= n / 2; ++k) amp[k] = std::abs(a[k]) * 4 / n;

    const float lo = kSpectrumLowHz, hi = std::min(kSpectrumHighHz, rate / 2.f);
    const float bin_hz = static_cast<float>(rate) / n;
    std::vector<float> out(n_bars);
    for (int i = 0; i < n_bars; ++i) {
        float fa = lo * std::pow(hi / lo, static_cast<float>(i) / n_bars);
        float fb = lo * std::pow(hi / lo, static_cast<float>(i + 1) / n_bars);
        float ba = fa / bin_hz, bb = fb / bin_hz;
        float v = 0;
        if (bb - ba < 1) {  // narrower than a bin (the bass end): interpolate
            float c = (ba + bb) / 2;
            int k = static_cast<int>(c);
            float t = c - k;
            v = amp[k] * (1 - t) + amp[std::min(k + 1, n / 2)] * t;
        } else {
            for (int k = static_cast<int>(ba); k <= std::min(static_cast<int>(bb), n / 2); ++k)
                v = std::max(v, amp[k]);
        }
        float tilt = 3 * std::log2(std::sqrt(fa * fb) / 1000);  // +3 dB per octave
        out[i] = 20 * std::log10(v + 1e-9f) + tilt;
    }
    return out;
}

Visualizer::Visualizer(std::function<void()> on_frame)
    : on_frame_(std::move(on_frame)), ring_(kFft, 0.f) {}

Visualizer::~Visualizer() = default;  // thread_ (a jthread) stops and joins itself

void Visualizer::set_live(bool live) {
    live_ = live;
    if (live && !running_ && error().empty()) {
        running_ = true;
        moving_ = true;
        thread_ = std::jthread([this](std::stop_token st) { capture(st); });  // joins the old, finished one
    }
}

std::string Visualizer::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void Visualizer::clear_error() {
    std::lock_guard lock(mutex_);
    error_.clear();
}

std::vector<float> Visualizer::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<float> s(kFft);
    for (int i = 0; i < kFft; ++i) s[i] = ring_[(ring_pos_ + i) % kFft];
    return s;
}

void Visualizer::capture(std::stop_token stop) {
    auto fail = [&](std::string why) {
        {
            std::lock_guard lock(mutex_);
            error_ = std::move(why);
        }
        running_ = false;
        on_frame_();
    };
#ifdef CRATE_HAVE_PULSE
    pa_sample_spec spec{PA_SAMPLE_FLOAT32NE, kRate, 1};  // mono: the server mixes the channels down
    pa_buffer_attr attr{};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.fragsize = kChunk * sizeof(float);  // small chunks, so the bars keep up with the music
    attr.tlength = attr.prebuf = attr.minreq = static_cast<uint32_t>(-1);
    int err = 0;
    pa_simple* s = pa_simple_new(nullptr, "crate", PA_STREAM_RECORD, "@DEFAULT_MONITOR@",
                                 "spectrum", &spec, nullptr, &attr, &err);
    if (!s) return fail(std::string("can't record the audio output: ") + pa_strerror(err));

    std::vector<float> buf(kChunk);
    auto last_frame = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
        if (pa_simple_read(s, buf.data(), buf.size() * sizeof(float), &err) < 0) {
            pa_simple_free(s);
            return fail(std::string("audio capture stopped: ") + pa_strerror(err));
        }
        {
            std::lock_guard lock(mutex_);
            for (float x : buf) {
                ring_[ring_pos_] = x;
                ring_pos_ = (ring_pos_ + 1) % kFft;
            }
        }
        // Paused and the bars have fallen: nothing left to animate.
        if (!live_ && !moving_) break;
        auto now = std::chrono::steady_clock::now();
        if (now - last_frame >= std::chrono::milliseconds(33)) {
            last_frame = now;
            on_frame_();
        }
    }
    pa_simple_free(s);
    running_ = false;
#else
    (void)stop;
    fail("crate was built without PulseAudio (install libpulse and rebuild)");
#endif
}

std::vector<float> Visualizer::levels(int n_bars) {
    auto now = std::chrono::steady_clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - last_levels_).count(), 0.f, 0.1f);
    last_levels_ = now;

    std::vector<float> out(n_bars, 0.f);
    if (!live_ || !running_ || n_bars <= 0) return out;
    std::vector<float> db = spectrum_db(snapshot(), kRate, n_bars);
    // Auto gain: follow the loudest band up instantly and back down slowly,
    // so quiet and loud songs both fill the space.
    float loudest = *std::max_element(db.begin(), db.end());
    level_ = std::max({loudest, level_ - dt * 6, -50.f});
    float top = level_ + 3;  // a little headroom above the loudest band
    for (int i = 0; i < n_bars; ++i) out[i] = std::clamp((db[i] - (top - kRange)) / kRange, 0.f, 1.f);
    return out;
}

void SpectrumBars::step(const std::vector<float>& target) {
    auto now = std::chrono::steady_clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - last_).count(), 0.f, 0.1f);
    last_ = now;

    const size_t n = target.size();
    if (bars.size() != n) {
        bars.assign(n, 0.f);
        peaks.assign(n, 0.f);
        hold_.assign(n, 0.f);
    }
    for (size_t i = 0; i < n; ++i) {
        float& b = bars[i];
        if (target[i] > b) b += (target[i] - b) * std::min(1.f, dt * 30);  // quick rise
        else b = std::max(target[i], b - dt * 1.6f);                       // steady fall

        // Peak caps: sit at the highest point for a moment, then drift down.
        if (b >= peaks[i]) {
            peaks[i] = b;
            hold_[i] = 0.4f;
        } else if (hold_[i] > 0) {
            hold_[i] -= dt;
        } else {
            peaks[i] = std::max(b, peaks[i] - dt * 0.7f);
        }
    }
}

bool SpectrumBars::moving() const {
    for (size_t i = 0; i < bars.size(); ++i)
        if (bars[i] > 0.002f || peaks[i] > 0.002f) return true;
    return false;
}
