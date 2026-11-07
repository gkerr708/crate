#include "download.hpp"

#include <cstdlib>
#include <vector>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "library.hpp"  // lower()

namespace fs = std::filesystem;
extern char** environ;

Downloader::~Downloader() {
    if (pid_ > 0) kill(pid_, SIGTERM);  // the worker's read() then ends
    if (worker_.joinable()) worker_.join();
}

bool Downloader::start(const std::string& url, const fs::path& out_dir, const std::string& format) {
    if (busy_) return false;
    if (worker_.joinable()) worker_.join();  // the previous, finished download
    busy_ = true;
    percent_ = 0;
    worker_ = std::thread(&Downloader::run, this, url, out_dir, format);
    return true;
}

std::optional<DownloadResult> Downloader::take_result() {
    std::lock_guard lock(result_mutex_);
    auto r = std::move(result_);
    result_.reset();
    return r;
}

void Downloader::run(std::string url, fs::path out_dir, std::string format) {
    fs::create_directories(out_dir);
    std::string out = (out_dir / "%(title)s [%(id)s].%(ext)s").string();

    // --print makes yt-dlp quiet and print only what we ask for:
    //   "PROGRESS  45.3%" lines while downloading, then "FILE /path/x.opus"
    //   and "URL https://..." (the page it came from).
    std::vector<std::string> args = {
        "yt-dlp", "--no-playlist", "--extract-audio", "--audio-format", format,
        "--audio-quality", "0", "--embed-metadata", "--color", "never",
        "--newline", "--progress", "--progress-template", "download:PROGRESS %(progress._percent_str)s",
        "--print", "after_move:FILE %(filepath)s", "--print", "after_move:URL %(webpage_url)s",
        "--output", out, "--", url,
    };
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    // A pipe: yt-dlp writes into one end, we read from the other.
    int pipe_fds[2];
    DownloadResult result;
    if (pipe(pipe_fds) != 0) {
        result.error = "couldn't create a pipe";
    } else {
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", 0, 0);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 1);  // stdout -> pipe
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 2);  // stderr -> pipe
        posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
        pid_t pid;
        int err = posix_spawnp(&pid, "yt-dlp", &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(pipe_fds[1]);  // only the child writes

        if (err != 0) {
            result.error = "couldn't run yt-dlp (is it installed?)";
        } else {
            pid_ = pid;
            std::string buffer, last_line;
            char chunk[1024];
            ssize_t n;
            while ((n = read(pipe_fds[0], chunk, sizeof(chunk))) > 0) {
                buffer.append(chunk, static_cast<size_t>(n));
                size_t nl;
                while ((nl = buffer.find_first_of("\r\n")) != std::string::npos) {
                    std::string line = buffer.substr(0, nl);
                    buffer.erase(0, nl + 1);
                    if (line.starts_with("PROGRESS")) {
                        int p = static_cast<int>(std::atof(line.c_str() + 8));
                        if (p != percent_) { percent_ = p; on_change_(); }
                    } else if (line.starts_with("FILE ")) {
                        result.file = line.substr(5);
                    } else if (line.starts_with("URL ")) {
                        result.source = line.substr(4);
                    } else if (!line.empty()) {
                        last_line = line;
                    }
                }
            }
            int status = 0;
            waitpid(pid, &status, 0);
            pid_ = -1;
            result.ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                        !result.file.empty() && fs::exists(result.file);
            if (!result.ok) result.error = last_line.empty() ? "download failed" : last_line;
        }
        close(pipe_fds[0]);
    }

    {
        std::lock_guard lock(result_mutex_);
        result_ = std::move(result);
    }
    busy_ = false;
    on_change_();
}

// ---------------------------------------------------------------------------
// Title guessing
// ---------------------------------------------------------------------------

static std::string trim(std::string s) {
    auto start = s.find_first_not_of(" \t-|");
    auto end = s.find_last_not_of(" \t-|");
    return start == std::string::npos ? "" : s.substr(start, end - start + 1);
}

// Removes "(Official Video)", "[HD]", "(Lyrics)" and similar chunks.
static std::string strip_junk(std::string s) {
    static const std::vector<std::string> junk = {
        "official", "video", "audio", "lyric", "visualizer", "hd", "4k", "hq", "remaster", "explicit",
    };
    for (auto [open, close] : {std::pair{'(', ')'}, std::pair{'[', ']'}}) {
        size_t from = 0;
        while ((from = s.find(open, from)) != std::string::npos) {
            size_t to = s.find(close, from);
            if (to == std::string::npos) break;
            std::string inside = lower(s.substr(from, to - from + 1));
            bool is_junk = false;
            for (const auto& word : junk) is_junk |= inside.find(word) != std::string::npos;
            if (is_junk) s.erase(from, to - from + 1);
            else from = to + 1;
        }
    }
    return trim(s);
}

TitleGuess guess_from_title(const std::string& video_title) {
    std::string t = strip_junk(video_title);
    for (const std::string sep : {" - ", " – ", " — ", " | "}) {
        auto pos = t.find(sep);
        if (pos != std::string::npos) {
            return {trim(t.substr(pos + sep.size())), trim(t.substr(0, pos))};
        }
    }
    return {t, ""};
}

std::string clean_channel(std::string channel) {
    for (const std::string suffix : {" - Topic", "VEVO", "Official"}) {
        if (channel.size() > suffix.size() && channel.ends_with(suffix)) {
            channel.erase(channel.size() - suffix.size());
        }
    }
    return trim(channel);
}
