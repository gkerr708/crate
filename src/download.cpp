#include "download.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
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

bool Downloader::start(const std::string& url, const fs::path& out_dir, const std::string& format,
                       bool playlist, const std::string& quality) {
    if (busy_) return false;
    if (worker_.joinable()) worker_.join();  // the previous, finished download
    busy_ = true;
    percent_ = 0;
    item_ = 0;
    item_count_ = 0;
    worker_ = std::thread(&Downloader::run, this, url, out_dir, format, playlist, quality);
    return true;
}

std::optional<DownloadResult> Downloader::take_result() {
    std::lock_guard lock(result_mutex_);
    auto r = std::move(result_);
    result_.reset();
    return r;
}

void Downloader::run(std::string url, fs::path out_dir, std::string format, bool playlist, std::string quality) {
    fs::create_directories(out_dir);
    std::string out = (out_dir / "%(title)s [%(id)s].%(ext)s").string();

    // --print makes yt-dlp quiet and print only what we ask for:
    //   "PROGRESS  45.3%" lines while downloading, then "FILE /path/x.opus"
    //   and "URL https://..." (the page it came from).
    //
    // A playlist download instead prints, for every video that made it,
    //   "ITEM 3 https://... /path/x.opus" (position, page, file),
    //   "TITLE <playlist title>" and "UPLOADER <channel>",
    // and progress lines get "3/12" (which video, out of how many) added.
    std::vector<std::string> args = {
        "yt-dlp", playlist ? "--yes-playlist" : "--no-playlist", "--extract-audio", "--audio-format", format,
        "--audio-quality", quality, "--embed-metadata", "--color", "never", "--newline", "--progress",
    };
    if (quality != "0") {
        // A low bitrate was asked for. yt-dlp only applies --audio-quality
        // when it converts, and YouTube's audio is usually Opus already, so
        // it would just copy the full-size stream. Instead pick YouTube's own
        // low-bitrate stream (about 50-64 kbps); elsewhere, the smallest one.
        args.insert(args.end(), {"--format", "bestaudio[abr<=64]/worstaudio/bestaudio"});
    }
    if (playlist) {
        args.insert(args.end(), {
            // Keep going past unavailable / private videos.
            "--ignore-errors",
            "--progress-template",
            "download:PROGRESS %(progress._percent_str)s %(info.playlist_index)s/%(info.n_entries)s",
            "--print", "after_move:ITEM %(playlist_index)s %(webpage_url)s %(filepath)s",
            "--print", "after_move:TITLE %(playlist_title)s",
            "--print", "after_move:UPLOADER %(playlist_uploader,channel,uploader)s",
        });
    } else {
        args.insert(args.end(), {
            "--progress-template", "download:PROGRESS %(progress._percent_str)s",
            "--print", "after_move:FILE %(filepath)s", "--print", "after_move:URL %(webpage_url)s",
        });
    }
    args.insert(args.end(), {"--output", out, "--", url});
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    // A pipe: yt-dlp writes into one end, we read from the other.
    int pipe_fds[2];
    DownloadResult result;
    result.playlist = playlist;
    int errors = 0;
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
                        int i = 0, count = 0;
                        // "PROGRESS   1.0% 3/12": the percentage is space-padded, so take the last word.
                        if (auto sp = line.rfind(' '); playlist && sp != std::string::npos)
                            std::sscanf(line.c_str() + sp + 1, "%d/%d", &i, &count);
                        if (p != percent_ || i != item_ || count != item_count_) {
                            percent_ = p;
                            item_ = i;
                            item_count_ = count;
                            on_change_();
                        }
                    } else if (line.starts_with("ITEM ")) {
                        // "ITEM <index> <url> <path>": the path may contain spaces, the others can't.
                        auto a = line.find(' ', 5);
                        auto b = a == std::string::npos ? a : line.find(' ', a + 1);
                        if (b != std::string::npos) {
                            PlaylistItem item;
                            item.index = std::atoi(line.substr(5, a - 5).c_str());
                            item.source = line.substr(a + 1, b - a - 1);
                            item.file = line.substr(b + 1);
                            if (fs::exists(item.file)) result.items.push_back(std::move(item));
                        }
                    } else if (line.starts_with("TITLE ")) {
                        if (result.playlist_title.empty()) result.playlist_title = line.substr(6);
                    } else if (line.starts_with("UPLOADER ")) {
                        if (result.uploader.empty()) result.uploader = line.substr(9);
                    } else if (line.starts_with("FILE ")) {
                        result.file = line.substr(5);
                    } else if (line.starts_with("URL ")) {
                        result.source = line.substr(4);
                    } else if (!line.empty()) {
                        if (line.starts_with("ERROR")) ++errors;
                        last_line = line;
                    }
                }
            }
            int status = 0;
            waitpid(pid, &status, 0);
            pid_ = -1;
            if (playlist) {
                // Some videos failing doesn't fail the playlist; none making it does.
                for (std::string* s : {&result.playlist_title, &result.uploader})
                    if (*s == "NA") s->clear();
                std::stable_sort(result.items.begin(), result.items.end(),
                                 [](const PlaylistItem& a, const PlaylistItem& b) { return a.index < b.index; });
                int n = static_cast<int>(result.items.size());
                result.failed = std::max(errors, item_count_ > n ? item_count_ - n : 0);
                result.ok = n > 0;
            } else {
                result.ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                            !result.file.empty() && fs::exists(result.file);
            }
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

// Removes junk left loose at the end, outside brackets:
// "Money | Lyrics", "Money - Lyric Video", "Money Lyrics" -> "Money".
// Without this, "Money | Lyrics" would split into artist "Money", title "Lyrics".
static std::string strip_trailing_junk(std::string s) {
    static const std::vector<std::string> tails = {
        "with lyrics", "lyrics video", "lyric video", "lyrics", "lyric",
        "official music video", "official video", "official audio", "music video", "visualizer",
    };
    for (bool changed = true; changed;) {
        changed = false;
        s = trim(s);
        std::string low = lower(s);
        for (const auto& tail : tails) {
            if (low.size() <= tail.size() || !low.ends_with(tail)) continue;
            // Only a whole word: "Lyrics" yes, the end of "Lyricsmith" no.
            char before = low[low.size() - tail.size() - 1];
            if (std::isalnum(static_cast<unsigned char>(before))) continue;
            std::string rest = trim(s.substr(0, s.size() - tail.size()));
            while (!rest.empty() && (rest.back() == ':' || rest.back() == '/' || rest.back() == ',')) rest.pop_back();
            rest = trim(rest);
            if (rest.empty()) continue;  // the whole title is "Lyrics": leave it
            s = rest;
            changed = true;
            break;
        }
    }
    return s;
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
    return strip_trailing_junk(s);
}

std::string clean_title(const std::string& video_title) { return strip_junk(video_title); }

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
