// Audio playback by remote-controlling an mpv process.
//
// We start `mpv --idle` in the background with an "IPC socket": a special
// file we can connect to and send JSON commands through, like
//     {"command": ["loadfile", "/path/song.flac"]}
// mpv also sends messages back (position changes, track finished, ...),
// which a background thread reads so the UI always knows what's playing.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <sys/types.h>

class Player {
public:
    // `on_change` is called (from the background thread) whenever something
    // the UI shows changes, so it can redraw.
    explicit Player(std::function<void()> on_change);
    ~Player();

    // Player owns a process and a thread, so copying it makes no sense.
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool ok() const { return fd_ >= 0; }  // false if mpv failed to start

    void play(const std::string& path);
    void stop();
    void toggle_pause();
    void seek(double seconds);  // relative, e.g. -5 or +10
    void change_volume(int delta);

    double position() const { return position_; }
    double duration() const { return duration_; }
    bool paused() const { return paused_; }
    int volume() const { return static_cast<int>(volume_); }

    // True once each time a track reaches its end on its own (not when we
    // skip or stop it). The UI uses this to start the next song.
    bool take_finished() { return finished_.exchange(false); }

private:
    void send(const std::string& json);
    void read_loop();
    void handle_line(const std::string& line);

    std::function<void()> on_change_;
    std::string socket_path_;
    pid_t pid_ = -1;
    int fd_ = -1;
    std::mutex write_mutex_;
    std::thread reader_;

    // std::atomic: safe to write from the reader thread and read from the UI.
    std::atomic<double> position_{0};
    std::atomic<double> duration_{0};
    std::atomic<bool> paused_{false};
    std::atomic<double> volume_{70};
    std::atomic<bool> finished_{false};
};
