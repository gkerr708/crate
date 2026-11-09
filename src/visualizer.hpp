// Spectrum analyser for the song that's playing.
//
// mpv plays the audio in its own process, so we never see the samples.
// Instead a background thread records the "monitor" of the default output:
// PulseAudio / PipeWire's copy of whatever is going to the speakers. When we
// draw, an FFT turns the latest samples into loudness per frequency band.
//
// Because the monitor carries all sound, not just mpv's, we only listen while
// crate itself is playing. Other apps playing at the same time will show up too.
//
// One Visualizer does the listening; each panel on screen keeps its own
// SpectrumBars (they can have different numbers of bars).
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// The frequency range the bars cover, low to high, on a log scale.
constexpr float kSpectrumLowHz = 40;
constexpr float kSpectrumHighHz = 16000;

// Loudness in dB of `n_bars` log-spaced bands of `samples`, whose size must
// be a power of two. Highs are tilted up a little so a typical song looks
// level instead of sloping down to the right.
std::vector<float> spectrum_db(const std::vector<float>& samples, int rate, int n_bars);

class Visualizer {
public:
    // `on_frame` is called from the capture thread about 30 times a second
    // while the bars are moving, so the UI can redraw.
    explicit Visualizer(std::function<void()> on_frame);
    ~Visualizer();

    Visualizer(const Visualizer&) = delete;
    Visualizer& operator=(const Visualizer&) = delete;

    // Whether to listen: crate is playing and a spectrum is on screen.
    void set_live(bool live);
    // Whether any bars are still falling, so redraws should keep coming.
    void set_moving(bool moving) { moving_ = moving; }

    // Why capture isn't working, or "" if it is. Capture isn't retried
    // after an error until clear_error().
    std::string error() const;
    void clear_error();

    // Where each of `n_bars` bars should be, 0..1, right now. All zero
    // while not listening. Call once per panel per draw.
    std::vector<float> levels(int n_bars);

private:
    void capture(std::stop_token stop);
    std::vector<float> snapshot() const;

    std::function<void()> on_frame_;
    std::atomic<bool> live_{false};
    std::atomic<bool> moving_{false};
    std::atomic<bool> running_{false};  // the capture thread is alive

    mutable std::mutex mutex_;  // guards ring_, ring_pos_, error_
    std::vector<float> ring_;   // the latest samples, oldest at ring_pos_
    size_t ring_pos_ = 0;
    std::string error_;

    float level_ = -20;  // auto gain: recent loudest band, in dB (UI thread only)
    std::chrono::steady_clock::time_point last_levels_{};

    std::jthread thread_;  // last, so it stops before the rest is destroyed
};

// One panel's bars, smoothed over time: they jump up with the music and fall
// steadily, with peak caps that hang for a moment before dropping.
struct SpectrumBars {
    std::vector<float> bars, peaks;  // 0..1

    void step(const std::vector<float>& target);
    bool moving() const;  // anything still above the floor

private:
    std::vector<float> hold_;
    std::chrono::steady_clock::time_point last_{};
};
