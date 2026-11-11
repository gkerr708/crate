// The music library: finding audio files on disk and reading their tags.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct Track {
    std::filesystem::path path;
    std::string title;   // falls back to the file name if there is no tag
    std::string artist;  // "Unknown Artist" if there is no tag
    std::string album;
    int number = 0;      // track number on the album, 0 if unknown
    int seconds = 0;     // length, 0 if unknown
    bool missing = false;  // listed in library.tsv but not downloaded on this computer
    std::string source;    // URL it can be downloaded from, "" if unknown
};

// Sorts by artist, album, track number, then title.
void sort_tracks(std::vector<Track>& tracks);

// Recursively finds every audio file under `dir`, reads its tags, and
// returns them sorted by artist, album, track number, then title.
std::vector<Track> scan_library(const std::filesystem::path& dir);

// Reads one file's tags.
Track read_track(const std::filesystem::path& path);

// Writes title/artist/album into the file's tags, and the track number if
// `number` is above 0 (otherwise the existing one is kept). False if that failed.
bool write_tags(const std::filesystem::path& path, const std::string& title,
                const std::string& artist, const std::string& album, int number = 0);

// Moves a file to the desktop trash (~/.local/share/Trash), so it can be
// restored from a file manager. On failure returns false and sets `error`.
bool move_to_trash(const std::filesystem::path& path, std::string& error);

// Makes a string safe to use as a file or folder name.
std::string safe_filename(std::string s);

// "3:07" style time, or "1:05:09" past an hour. Negative or zero gives "-:--".
std::string format_time(int seconds);

// Lowercase copy, for case-insensitive comparisons and searching.
std::string lower(std::string s);
