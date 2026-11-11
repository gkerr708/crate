#include "shrink.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
extern char** environ;

Shrinker::~Shrinker() {
    stopping_ = true;
    if (pid_ > 0) kill(pid_, SIGTERM);
    if (worker_.joinable()) worker_.join();
}

bool Shrinker::start(std::vector<ShrinkJob> jobs, const std::string& bitrate) {
    if (busy_ || jobs.empty()) return false;
    if (worker_.joinable()) worker_.join();  // the previous, finished batch
    busy_ = true;
    item_ = 0;
    count_ = static_cast<int>(jobs.size());
    percent_ = 0;
    worker_ = std::thread(&Shrinker::run, this, std::move(jobs), bitrate);
    return true;
}

std::vector<ShrinkResult> Shrinker::take_results() {
    std::lock_guard lock(results_mutex_);
    return std::exchange(results_, {});
}

void Shrinker::run(std::vector<ShrinkJob> jobs, std::string bitrate) {
    for (size_t i = 0; i < jobs.size() && !stopping_; ++i) {
        item_ = static_cast<int>(i) + 1;
        percent_ = 0;
        on_change_();
        ShrinkResult r = encode(jobs[i], bitrate);
        {
            std::lock_guard lock(results_mutex_);
            results_.push_back(std::move(r));
        }
        on_change_();
    }
    busy_ = false;
    on_change_();
}

ShrinkResult Shrinker::encode(const ShrinkJob& job, const std::string& bitrate) {
    ShrinkResult result;
    result.job = job;

    // -progress pipe:1 prints "key=value" lines; out_time_us is how far it's got.
    // -vn drops cover art (Opus files can't hold it as a video stream);
    // the tags come along with -map_metadata.
    std::vector<std::string> args = {
        "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
        "-i", job.in.string(), "-vn", "-map_metadata", "0",
        "-c:a", "libopus", "-b:a", bitrate,
        "-progress", "pipe:1", "-nostats", job.out.string(),
    };
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        result.error = "couldn't create a pipe";
        return result;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", 0, 0);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 1);  // stdout -> pipe
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 2);  // stderr -> pipe
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    pid_t pid;
    int err = posix_spawnp(&pid, "ffmpeg", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);  // only the child writes
    if (err != 0) {
        close(pipe_fds[0]);
        result.error = "couldn't run ffmpeg (is it installed?)";
        return result;
    }
    pid_ = pid;

    std::string buffer, last_error;
    char chunk[1024];
    ssize_t n;
    while ((n = read(pipe_fds[0], chunk, sizeof(chunk))) > 0) {
        buffer.append(chunk, static_cast<size_t>(n));
        size_t nl;
        while ((nl = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            if (line.starts_with("out_time_us=")) {
                double secs = std::atof(line.c_str() + 12) / 1e6;
                int p = job.seconds > 0 ? static_cast<int>(secs * 100 / job.seconds) : 0;
                p = std::max(0, std::min(p, 99));
                if (p != percent_) {
                    percent_ = p;
                    on_change_();
                }
            } else if (!line.empty() && line.find('=') == std::string::npos) {
                last_error = line;  // with -loglevel error, anything else is a complaint
            }
        }
    }
    close(pipe_fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    pid_ = -1;

    result.ok = !stopping_ && WIFEXITED(status) && WEXITSTATUS(status) == 0 && fs::exists(job.out);
    if (!result.ok) {
        std::error_code ec;
        fs::remove(job.out, ec);  // don't leave half a file behind
        result.error = stopping_ ? "stopped" : last_error.empty() ? "ffmpeg failed" : last_error;
    }
    return result;
}
