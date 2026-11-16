// Keeps the library folder's git repo (library.tsv, progress.tsv, playlists)
// in step with its remote. Both checks talk to the terminal directly, so
// they run before the UI starts and after it exits. If the folder isn't a
// git repo with a remote, or there's no terminal, they do nothing.
#pragma once

#include <filesystem>

// Fetches, and if the remote has new commits asks whether to pull them.
void sync_on_start(const std::filesystem::path& library);

// If there are uncommitted changes or unpushed commits, asks whether to
// commit and push them.
void sync_on_exit(const std::filesystem::path& library);
