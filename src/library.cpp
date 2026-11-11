#include "library.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <tuple>

#include <taglib/fileref.h>
#include <taglib/tag.h>

namespace fs = std::filesystem;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string format_time(int seconds) {
    if (seconds <= 0) return "-:--";
    auto two = [](int n) { return (n < 10 ? "0" : "") + std::to_string(n); };
    int h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;
    if (h > 0) return std::to_string(h) + ":" + two(m) + ":" + two(s);  // 1:05:09
    return std::to_string(m) + ":" + two(s);                            // 3:07
}

static bool is_audio(const fs::path& p) {
    static const std::set<std::string> exts = {
        ".mp3", ".flac", ".opus", ".ogg", ".m4a", ".aac", ".wav", ".wma", ".aiff",
    };
    return exts.contains(lower(p.extension().string()));
}

Track read_track(const fs::path& path) {
    Track t;
    t.path = path;

    // FileRef figures out the format (mp3, flac, ...) from the file itself.
    TagLib::FileRef file(path.c_str());
    if (!file.isNull() && file.tag()) {
        TagLib::Tag* tag = file.tag();
        t.title = tag->title().to8Bit(true);  // true = give us UTF-8
        t.artist = tag->artist().to8Bit(true);
        t.album = tag->album().to8Bit(true);
        t.number = static_cast<int>(tag->track());
    }
    if (!file.isNull() && file.audioProperties()) {
        t.seconds = file.audioProperties()->lengthInSeconds();
    }

    if (t.title.empty()) t.title = path.stem().string();
    if (t.artist.empty()) t.artist = "Unknown Artist";
    return t;
}

bool write_tags(const fs::path& path, const std::string& title, const std::string& artist,
                const std::string& album, int number) {
    TagLib::FileRef file(path.c_str());
    if (file.isNull() || !file.tag()) return false;
    // TagLib stores text in its own String type; tell it ours is UTF-8.
    file.tag()->setTitle(TagLib::String(title, TagLib::String::UTF8));
    file.tag()->setArtist(TagLib::String(artist, TagLib::String::UTF8));
    file.tag()->setAlbum(TagLib::String(album, TagLib::String::UTF8));
    if (number > 0) file.tag()->setTrack(static_cast<unsigned int>(number));
    return file.save();
}

// The trash follows the freedesktop.org spec, which file managers (and
// `gio trash --list`) understand: the file goes in Trash/files/, and a small
// Trash/info/<name>.trashinfo records where it came from and when.
static std::string url_encode_path(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool move_to_trash(const fs::path& path, std::string& error) {
    const char* data = std::getenv("XDG_DATA_HOME");
    fs::path trash = data ? fs::path(data) / "Trash" : fs::path(std::getenv("HOME")) / ".local/share/Trash";
    std::error_code ec;
    fs::create_directories(trash / "files", ec);
    fs::create_directories(trash / "info", ec);

    // Pick a name not already in the trash: song.mp3, song.2.mp3, ...
    std::string stem = path.stem().string(), ext = path.extension().string();
    std::string name = path.filename().string();
    for (int i = 2; fs::exists(trash / "files" / name) || fs::exists(trash / "info" / (name + ".trashinfo")); ++i) {
        name = stem + "." + std::to_string(i) + ext;
    }

    std::time_t now = std::time(nullptr);
    char date[32];
    std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    fs::path info = trash / "info" / (name + ".trashinfo");
    {
        std::ofstream out(info);
        out << "[Trash Info]\nPath=" << url_encode_path(fs::absolute(path).string())
            << "\nDeletionDate=" << date << "\n";
        if (!out) { error = "couldn't write to the trash"; return false; }
    }

    fs::path dest = trash / "files" / name;
    fs::rename(path, dest, ec);
    if (ec) {  // different drive: copy, then delete
        ec.clear();
        fs::copy_file(path, dest, ec);
        if (!ec) fs::remove(path, ec);
    }
    if (ec) {
        fs::remove(info);
        error = ec.message();
        return false;
    }
    return true;
}

std::string safe_filename(std::string s) {
    for (char& c : s) {
        if (c == '/' || c == '\\' || static_cast<unsigned char>(c) < 0x20) c = '_';
    }
    while (!s.empty() && (s.front() == '.' || s.front() == ' ')) s.erase(0, 1);  // no hidden files
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.size() > 150) {
        s.resize(150);
        while (!s.empty() && (s.back() & 0xC0) == 0x80) s.pop_back();  // don't cut a UTF-8 character
        if (!s.empty() && (s.back() & 0x80)) s.pop_back();
    }
    return s.empty() ? "Untitled" : s;
}

std::vector<Track> scan_library(const fs::path& dir) {
    std::vector<Track> tracks;
    std::error_code ec;  // passing this makes the iterator report errors instead of throwing
    auto options = fs::directory_options::skip_permission_denied |
                   fs::directory_options::follow_directory_symlink;

    for (auto it = fs::recursive_directory_iterator(dir, options, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && is_audio(it->path())) {
            tracks.push_back(read_track(it->path()));
        }
    }

    sort_tracks(tracks);
    return tracks;
}

void sort_tracks(std::vector<Track>& tracks) {
    // Sort with a tuple: compares artist first, then album, and so on.
    std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) {
        return std::tuple(lower(a.artist), lower(a.album), a.number, lower(a.title)) <
               std::tuple(lower(b.artist), lower(b.album), b.number, lower(b.title));
    });
}
