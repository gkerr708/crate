// Listening progress for podcasts (and audiobooks): <library>/progress.tsv
//
// Remembers where you stopped in anything marked as a podcast, so it
// picks up there next time, and which ones you've finished. Like
// library.tsv it lives in the library folder, so keeping the folder in git
// carries your place between computers (and your phone).
//
// Format: tab-separated columns, lines starting with # are comments.
//   path  seconds  done
// `path` is relative to the library folder; `done` is 1 once finished.
#pragma once

#include <filesystem>
#include <map>
#include <string>

struct Progress {
    int seconds = 0;    // where you stopped
    bool done = false;  // listened to the end
};

// Keyed by path relative to the library folder. A std::map keeps the file
// sorted, so it diffs cleanly in git.
using ProgressMap = std::map<std::string, Progress>;

ProgressMap read_progress(const std::filesystem::path& library);

// Writes progress.tsv, but only if it would change.
bool write_progress(const std::filesystem::path& library, const ProgressMap& progress);
