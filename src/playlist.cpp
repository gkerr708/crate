#include "playlist.hpp"

#include <algorithm>
#include <fstream>
#include <unordered_map>

namespace fs = std::filesystem;

fs::path playlist_dir(const fs::path& library) { return library / "Playlists"; }

std::vector<Playlist> load_playlists(const fs::path& library) {
    std::vector<Playlist> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(playlist_dir(library), ec)) {
        if (lower(entry.path().extension().string()) != ".m3u" &&
            lower(entry.path().extension().string()) != ".m3u8") continue;

        Playlist pl;
        pl.name = entry.path().stem().string();
        pl.file = entry.path();
        std::ifstream in(entry.path());
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();  // Windows line endings
            if (line.empty() || line[0] == '#') continue;
            fs::path p = line;
            // Relative paths are relative to the playlist file's folder.
            if (p.is_relative()) p = entry.path().parent_path() / p;
            pl.songs.push_back(p.lexically_normal());
        }
        out.push_back(std::move(pl));
    }
    std::sort(out.begin(), out.end(),
              [](const Playlist& a, const Playlist& b) { return lower(a.name) < lower(b.name); });
    return out;
}

bool save_playlist(const Playlist& pl, const std::vector<Track>& tracks) {
    std::error_code ec;
    fs::create_directories(pl.file.parent_path(), ec);

    std::unordered_map<std::string, const Track*> by_path;
    for (const Track& t : tracks) by_path[t.path.string()] = &t;

    // Write to a temporary file, then rename it over the old one. A rename
    // is all-or-nothing, so a crash halfway can't leave a half-written playlist.
    fs::path tmp = pl.file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        out << "#EXTM3U\n";
        for (const fs::path& song : pl.songs) {
            if (auto it = by_path.find(song.string()); it != by_path.end()) {
                const Track& t = *it->second;
                out << "#EXTINF:" << t.seconds << "," << t.artist << " - " << t.title << "\n";
            }
            // Relative paths (like ../Tool/Lateralus/Schism.opus) keep working
            // if you move or sync the whole Music folder somewhere else.
            fs::path rel = song.lexically_relative(pl.file.parent_path());
            out << (rel.empty() ? song : rel).string() << "\n";
        }
        if (!out) return false;
    }
    fs::rename(tmp, pl.file, ec);
    return !ec;
}
