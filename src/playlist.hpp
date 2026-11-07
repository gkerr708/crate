// Playlists, stored as plain .m3u files in <library>/Playlists/.
//
// An .m3u file is just a list of song paths, one per line, so mpv, VLC and
// most other players can open them too. Lines starting with # are comments
// (we write "#EXTINF" lines with the song name, which players show).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "library.hpp"

struct Playlist {
    std::string name;                           // file name without .m3u
    std::filesystem::path file;                 // where it's saved
    std::vector<std::filesystem::path> songs;   // in order; may include missing files
};

// The folder playlists live in.
std::filesystem::path playlist_dir(const std::filesystem::path& library);

// Every .m3u in the playlist folder, sorted by name.
std::vector<Playlist> load_playlists(const std::filesystem::path& library);

// Writes the playlist to its file (creating the folder if needed). `tracks`
// is only used to write the song names into the #EXTINF comment lines.
bool save_playlist(const Playlist& pl, const std::vector<Track>& tracks);
