// The library index: <library>/library.tsv
//
// A plain-text list of every song crate knows about, one per line, with
// where it can be downloaded from. It's meant to be kept in git: clone it
// on another computer and crate shows every song, downloading missing ones
// from their source URL (:sync).
//
// Format: tab-separated columns, lines starting with # are comments.
//   path  artist  album  title  seconds  source  [track]
// `track` (the track number) is only written when the song has one.
// `path` is relative to the library folder, e.g. "Tool/Lateralus/Schism.opus".
#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct ManifestEntry {
    std::string path;  // relative to the library folder
    std::string artist;
    std::string album;
    std::string title;
    int seconds = 0;
    std::string source;  // URL it was downloaded from, "" if unknown
    int number = 0;      // track number on the album, 0 if unknown
};

std::filesystem::path manifest_path(const std::filesystem::path& library);

std::vector<ManifestEntry> read_manifest(const std::filesystem::path& library);

// Writes the index, but only if it would change (so git sees no noise).
bool write_manifest(const std::filesystem::path& library, const std::vector<ManifestEntry>& entries);
