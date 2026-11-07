// Downloading audio from a URL with yt-dlp, in the background.
//
// Same idea as Player: we start a child process, read what it prints on a
// background thread, and call `on_change` so the UI can redraw.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <sys/types.h>

struct DownloadResult {
    bool ok = false;
    std::filesystem::path file;  // the downloaded audio file, if ok
    std::string error;           // last thing yt-dlp complained about, if not
    std::string source;          // the page URL it came from (for library.tsv)
};

class Downloader {
public:
    explicit Downloader(std::function<void()> on_change) : on_change_(std::move(on_change)) {}
    ~Downloader();  // stops a running download

    Downloader(const Downloader&) = delete;
    Downloader& operator=(const Downloader&) = delete;

    // Starts downloading `url` into `out_dir`. False if one is already running.
    // `format` is a yt-dlp audio format: best (keep the original), opus, mp3, ...
    bool start(const std::string& url, const std::filesystem::path& out_dir,
               const std::string& format = "best");

    bool busy() const { return busy_; }
    int percent() const { return percent_; }

    // The result, once, after a download finishes.
    std::optional<DownloadResult> take_result();

private:
    void run(std::string url, std::filesystem::path out_dir, std::string format);

    std::function<void()> on_change_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::atomic<int> percent_{0};
    std::atomic<pid_t> pid_{-1};

    std::mutex result_mutex_;
    std::optional<DownloadResult> result_;
};

// Turns a video title like "Tool - Schism (Official Video) [HD]" into a
// title and, if it found one, an artist: {"Schism", "Tool"}.
struct TitleGuess {
    std::string title;
    std::string artist;
};
TitleGuess guess_from_title(const std::string& video_title);

// Cleans up a channel name: "ToolVEVO" -> "Tool", "Tool - Topic" -> "Tool".
std::string clean_channel(std::string channel);
