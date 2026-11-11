// Re-encoding files to a low bitrate with ffmpeg, in the background (:shrink).
//
// Same idea as Downloader: a worker thread runs one ffmpeg at a time,
// reads its progress, and calls `on_change` so the UI can redraw. Each
// finished file is handed back with take_results(); swapping it into the
// library (trash, playlists, library.tsv) is left to the caller.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

struct ShrinkJob {
    std::filesystem::path in;   // the file to shrink
    std::filesystem::path out;  // where the smaller copy goes (a temporary name)
    int seconds = 0;            // its length, for the progress percentage
};

struct ShrinkResult {
    ShrinkJob job;
    bool ok = false;
    std::string error;  // what ffmpeg complained about, if not ok
};

class Shrinker {
public:
    explicit Shrinker(std::function<void()> on_change) : on_change_(std::move(on_change)) {}
    ~Shrinker();  // stops a running encode

    Shrinker(const Shrinker&) = delete;
    Shrinker& operator=(const Shrinker&) = delete;

    // Encodes each job's `in` to Opus at `bitrate` (e.g. "48k") into its `out`.
    // False if a batch is already running.
    bool start(std::vector<ShrinkJob> jobs, const std::string& bitrate);

    bool busy() const { return busy_; }
    int item() const { return item_; }    // which file is encoding, from 1
    int count() const { return count_; }  // how many in this batch
    int percent() const { return percent_; }

    // Files finished since the last call.
    std::vector<ShrinkResult> take_results();

private:
    void run(std::vector<ShrinkJob> jobs, std::string bitrate);
    ShrinkResult encode(const ShrinkJob& job, const std::string& bitrate);

    std::function<void()> on_change_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<int> item_{0};
    std::atomic<int> count_{0};
    std::atomic<int> percent_{0};
    std::atomic<pid_t> pid_{-1};

    std::mutex results_mutex_;
    std::vector<ShrinkResult> results_;
};
