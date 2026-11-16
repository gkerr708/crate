// Keeps the library folder's git repo (library.tsv, progress.tsv, playlists)
// in step with its remote. The functions that return a SyncResult are safe
// to call from a background thread. If the folder isn't its own git repo
// with a remote, they report that and do nothing.
#pragma once

#include <filesystem>
#include <string>

struct SyncResult {
    bool ok = true;     // false: something went wrong, `text` says what
    bool repo = true;   // false: not a git repo with a remote, nothing was done
    int behind = 0;     // commits on the remote we don't have (from sync_check)
    std::string text;   // a message fit for the status bar
};

// Fetches and reports how many commits the remote has that we don't.
SyncResult sync_check(const std::filesystem::path& library);

// Pulls the remote's commits (keeping uncommitted edits).
SyncResult sync_pull(const std::filesystem::path& library);

// Commits whatever changed in the repo and pushes it.
SyncResult sync_push(const std::filesystem::path& library);

// After the UI has closed: if there are uncommitted changes or unpushed
// commits, asks in the terminal whether to commit and push them.
void sync_on_exit(const std::filesystem::path& library);
