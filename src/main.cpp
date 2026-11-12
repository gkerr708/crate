// crate: a vim-flavoured music library and player for the terminal.
//
// Layout:  [ Artists ][ Albums ][ Songs ............................ ]
//          [ big spectrum with frequency axis (b, off by default)     ]
//          [ now playing (i) ][ mini spectrum (v) ................... ]
//          NORMAL  status / search / command line
//
// "♫ Playlists" is listed with the artists. Select it and the middle pane
// shows your playlists instead of albums.
//
// How it fits together:
//   library.cpp  - finds your songs, reads and writes their tags (TagLib)
//   player.cpp   - plays audio by remote-controlling an mpv process
//   download.cpp - downloads audio from a URL with yt-dlp
//   playlist.cpp - reads and writes .m3u playlist files
//   visualizer.cpp - records what's playing and turns it into spectrum bars
//   main.cpp     - this file: all the state, key handling and drawing
//
// FTXUI works like the todo app: we keep plain C++ state in `App`, one
// function draws the screen from that state, and one function changes the
// state in response to keys. FTXUI redraws after every event.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stop_token>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "download.hpp"
#include "library.hpp"
#include "manifest.hpp"
#include "musicbrainz.hpp"
#include "player.hpp"
#include "progress.hpp"
#include "playlist.hpp"
#include "shrink.hpp"
#include "visualizer.hpp"

using namespace ftxui;
namespace fs = std::filesystem;

// A scrollable list's cursor: which row is selected, which row is at the top.
struct Cursor {
    int sel = 0;
    int top = 0;

    // Keep `sel` inside the list and scroll so it stays visible.
    void fit(int size, int rows) {
        sel = std::clamp(sel, 0, std::max(0, size - 1));
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
        top = std::clamp(top, 0, std::max(0, size - rows));
    }
};

enum class Pane { Artists, Albums, Songs };
constexpr int kArtistsWidth = 30;  // the songs pane gets whatever is left
constexpr int kAlbumsWidth = 36;
enum class Mode { Normal, Search, Command, Url, Tag, Pick, Confirm, Rename };
// What the URL prompt adds: as = one video as a song, aa = a playlist as
// an album, ap = one video as a podcast (low bitrate, keeps your place).
enum class AddKind { Song, Album, Podcast };
// Podcasts are speech, which sounds fine at a fraction of music's bitrate.
constexpr const char* kPodcastFormat = "opus";
constexpr const char* kPodcastQuality = "48K";
// :shrink re-encodes to Opus at this bitrate; files already below
// kShrinkSkipBps (bits per second) aren't worth it.
constexpr const char* kShrinkBitrate = "48k";
constexpr std::uintmax_t kShrinkSkipBps = 80000;

// The first two rows of the artists pane are special.
constexpr int kAllArtists = 0;
constexpr int kPlaylists = 1;

// A row in the middle pane: an album, "All albums", or a playlist.
struct Group {
    enum Kind { AllAlbums, Album, List } kind = AllAlbums;
    std::string label;
    std::string artist;  // for Album: whose album (matters under "All")
    std::string album;
    int playlist = -1;   // for List: index into App::playlists
};

// The Title / Artist / Album form, used for new downloads (`a`) and for
// editing an existing song's tags (`e`).
struct TagForm {
    enum Field { Title, Artist, Album, Podcast };  // Podcast is a yes/no toggle
    fs::path file;
    bool is_new = false;     // a fresh download, to be moved into the library on save
    std::string fields[4];   // what's typed in each field (Podcast's is unused)
    int field = Title;       // the field being edited
    int pick = -1;           // highlighted suggestion; -1 = use what's typed
    std::string source;      // URL it was downloaded from
    bool podcast = false;    // the Podcast toggle; ap starts it on
    // aS / aA / aP: nothing was downloaded. Saving only lists the song(s) in
    // library.tsv, as not downloaded here (⇣); `file` is empty.
    bool listing = false;
    int seconds = 0;         // a listed song's length, 0 if unknown

    // A downloaded playlist, saved as one album: Title isn't asked (each
    // song's comes from its video), only Artist and Album.
    std::vector<PlaylistItem> batch;
    std::vector<std::string> video_titles;  // batch[i]'s video title, from its tags
    int failed = 0;                         // videos that didn't download
};

// Where downloads wait until they're tagged: ~/.cache/crate/incoming
static fs::path incoming_dir() {
    const char* cache = std::getenv("XDG_CACHE_HOME");
    fs::path base = cache ? fs::path(cache) : fs::path(std::getenv("HOME")) / ".cache";
    return base / "crate" / "incoming";
}

// Settings kept per computer: ~/.local/state/crate/<name>
static fs::path state_file(const std::string& name) {
    const char* state = std::getenv("XDG_STATE_HOME");
    fs::path base = state ? fs::path(state) : fs::path(std::getenv("HOME")) / ".local" / "state";
    return base / "crate" / name;
}

// On/off settings kept in state files, written as 1 or 0.
static bool read_flag(const std::string& name, bool fallback) {
    bool value = fallback;
    if (FILE* f = std::fopen(state_file(name).c_str(), "r")) {
        int v = 0;
        if (std::fscanf(f, "%d", &v) == 1) value = v != 0;
        std::fclose(f);
    }
    return value;
}

static void write_flag(const std::string& name, bool value) {
    std::error_code ec;
    fs::create_directories(state_file(name).parent_path(), ec);
    if (FILE* f = std::fopen(state_file(name).c_str(), "w")) {
        std::fprintf(f, "%d\n", value ? 1 : 0);
        std::fclose(f);
    }
}

// "1.5×" style speed.
static std::string format_speed(double speed) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%g×", speed);
    return buf;
}

// The playlist a URL points at (YouTube's list=...), or "" if none. Mixes
// (list=RD...) are endless auto-generated radio, not a playlist to keep.
static std::string playlist_id(const std::string& url) {
    auto pos = url.find("list=");
    if (pos == std::string::npos || (pos > 0 && url[pos - 1] != '?' && url[pos - 1] != '&')) return "";
    std::string id = url.substr(pos + 5, url.find('&', pos) - pos - 5);
    return id.starts_with("RD") ? "" : id;
}

// Whether a URL also names one video (watch?v=..., youtu.be/...), as when
// it's copied while a playlist is playing.
static bool names_video(const std::string& url) {
    return url.find("v=") != std::string::npos || url.find("youtu.be/") != std::string::npos;
}

// What's on the Wayland clipboard, if it looks like a URL.
static std::string clipboard_url() {
    std::string out;
    if (FILE* p = popen("wl-paste --no-newline 2>/dev/null", "r")) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf), p);
        out.assign(buf, n);
        pclose(p);
    }
    bool looks_like_url = out.starts_with("http") && out.find_first_of(" \n\t") == std::string::npos;
    return looks_like_url ? out : "";
}

static std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

// Options matching what's typed, ones that start with it first. Used for
// every "type or pick" box (artist, album, playlist).
static std::vector<std::string> complete(const std::vector<std::string>& options, const std::string& typed) {
    std::string t = lower(trim(typed));
    std::vector<std::string> starts, contains;
    for (const std::string& o : options) {
        auto pos = lower(o).find(t);
        if (pos == 0) starts.push_back(o);
        else if (pos != std::string::npos) contains.push_back(o);
    }
    starts.insert(starts.end(), contains.begin(), contains.end());
    if (starts.size() > 8) starts.resize(8);
    return starts;
}

static std::string plural(int n, const std::string& word) {
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

// Remove the last UTF-8 character (not just the last byte).
static void pop_char(std::string& s) {
    while (!s.empty() && (s.back() & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
}

struct App {
    fs::path dir;
    Player& player;
    Downloader& downloader;
    ScreenInteractive& screen;

    App(fs::path d, Player& p, Downloader& dl, ScreenInteractive& s)
        : dir(std::move(d)), player(p), downloader(dl), screen(s) {}

    // --- library ---
    std::vector<Track> tracks;                         // every song, sorted
    std::unordered_map<std::string, int> track_index;  // path -> index into tracks
    std::vector<Playlist> playlists;
    std::vector<std::string> artists;  // [0] "All", [1] "♫ Playlists", then artists
    std::vector<Group> groups;         // middle pane
    std::vector<int> songs;            // right pane: indices into `tracks`
    std::vector<int> slots;            // in a playlist: songs[i] is entry slots[i] of it
    Cursor artist_cur, group_cur, song_cur;
    std::string filter;                // active search text (songs pane)
    std::uintmax_t library_bytes = 0;  // everything in the music folder, shown bottom right
    std::string jump;                  // last / search in the artists or albums pane
    int jump_origin = 0;               // where the cursor was when that search began

    // --- UI ---
    Pane pane = Pane::Artists;  // start in the artists pane
    Mode mode = Mode::Normal;
    std::string input;    // what's being typed after /, :, the URL prompt or the playlist picker
    std::string message;  // one-off feedback, cleared on the next key
    bool show_help = false;
    int rows = 10;        // visible list rows; updated every draw
    int cols = 120;       // screen width; updated every draw
    // Panes that fit side by side: 3 when wide, 2 on a small window,
    // 1 on a phone. The visible ones slide along to follow the active pane.
    int shown = 3;        // how many panes are visible
    int first_shown = 0;  // leftmost visible pane
    bool visible(Pane p) const {
        int i = static_cast<int>(p);
        return i >= first_shown && i < first_shown + shown;
    }
    // Where each pane and the now-playing box landed on screen last draw,
    // so a tap can be matched to what's under it.
    Box pane_box[3];
    Box now_box;
    // Counting taps, to tell a single tap from a double tap.
    std::chrono::steady_clock::time_point last_tap{};
    int last_tap_y = -100;
    int taps = 0;
    int tap_id = 0;          // bumped to cancel a pending single-tap action
    std::jthread tap_timer;  // waits briefly after a tap for a second one
    // The rightmost visible pane stretches; the others keep fixed widths.
    bool stretches(Pane p) const { return static_cast<int>(p) == first_shown + shown - 1; }
    int count = 0;        // vim count prefix, e.g. the 5 in 5j
    char pending = 0;     // first key of gg / yy / dd
    int pending_count = 0;
    int visual = -1;      // V mode: the row where the selection started, -1 = off

    // --- playlist editing ---
    std::vector<fs::path> yanked;   // songs copied with yy / dd, pasted with p
    std::vector<fs::path> cut;      // songs cut with x in an album, put back with p / P
    std::vector<fs::path> picking;  // songs being added with A
    int pick = -1;                  // highlighted playlist in the A picker
    std::string confirm_text;       // question asked in Confirm mode (y/n)
    std::function<void()> confirm_action;  // y
    std::function<void()> confirm_all;     // a, if the question offers it
    bool confirm_enter = false;            // Enter means y

    TagForm form;

    // --- library.tsv / downloading missing songs ---
    std::unordered_map<std::string, std::string> pending_sources;  // new file -> its URL, until the next load()
    std::vector<std::string> restore_queue;  // missing songs (relative paths) waiting to download
    std::string restoring;                   // the one downloading now, "" if none
    std::string last_url;                    // what was typed in the `a` prompt
    AddKind add_kind = AddKind::Song;        // which of as / aa / ap opened it
    bool add_listing = false;                // aS / aA / aP: list it without downloading
    std::set<std::string> pending_podcasts;  // new files to mark as podcasts, until the next load()

    // --- playback ---
    std::vector<int> queue;  // what we're playing through (indices into tracks)
    int queue_pos = -1;
    int playing = -1;        // index into tracks, -1 if nothing
    bool shuffle = false;
    std::mt19937 rng{std::random_device{}()};

    // --- progress.tsv: where you stopped in long tracks ---
    ProgressMap progress;
    bool progress_dirty = false;  // changed since it was last written
    std::chrono::steady_clock::time_point progress_written{};
    bool was_paused = false;

    // Playback speed for long tracks; music always plays at normal speed.
    // Each artist remembers its own (lowercased name -> speed), so a new
    // episode plays at the speed you used for that show last time; artists
    // you haven't set use long_speed. Remembered per computer in
    // ~/.local/state/crate/speed and speeds.tsv.
    double long_speed = 1.0;
    std::map<std::string, double> artist_speed;

    // --- spectrum panels and now playing ---
    //   v  mini spectrum, in the bottom row beside now playing
    //   b  big spectrum, full width above the bottom row, with a frequency axis
    //   i  now playing
    // On/off remembered in ~/.local/state/crate/{visualizer,big-spectrum,now-playing}.
    Visualizer vis{[this] { screen.PostEvent(Event::Custom); }};

    // --- :shrink ---
    Shrinker shrinker{[this] { screen.PostEvent(Event::Custom); }};
    bool shrinking = false;          // a batch was started and hasn't been summed up yet
    int shrink_done = 0, shrink_failed = 0;
    std::uintmax_t shrink_saved = 0;  // bytes
    std::string shrink_error;

    // --- :order ---
    AlbumLookup album_lookup{[this] { screen.PostEvent(Event::Custom); }};
    std::vector<std::string> order_paths;  // the album's songs, as sent to the lookup
    std::string order_album;
    SpectrumBars mini_bars, big_bars;
    bool show_vis = true, show_big = false, show_now = true;
    int vis_rows = 0;  // mini spectrum rows shown last draw, 0 = hidden
    int big_rows = 0;  // big spectrum bar rows shown last draw, 0 = hidden

    // =================================================================
    // Library: building the three panes
    // =================================================================
    void load() {
        std::string playing_path = playing >= 0 ? tracks[playing].path.string() : "";
        tracks = scan_library(dir);
        library_bytes = folder_size(dir);
        merge_manifest();
        track_index.clear();
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i) track_index[tracks[i].path.string()] = i;
        playlists = load_playlists(dir);
        progress = read_progress(dir);
        if (FILE* f = std::fopen(state_file("speed").c_str(), "r")) {
            double s = 0;
            if (std::fscanf(f, "%lf", &s) == 1 && s >= 0.5 && s <= 3) long_speed = s;
            std::fclose(f);
        }
        artist_speed.clear();
        {
            std::ifstream in(state_file("speeds.tsv"));
            std::string line;
            while (std::getline(in, line)) {
                auto tab = line.rfind('\t');
                if (tab == std::string::npos) continue;
                double sp = std::atof(line.c_str() + tab + 1);
                if (sp >= 0.5 && sp <= 3) artist_speed[line.substr(0, tab)] = sp;
            }
        }
        show_vis = read_flag("visualizer", true);
        show_big = read_flag("big-spectrum", false);
        show_now = read_flag("now-playing", true);

        build_artists();
        build_groups();
        refresh_songs();

        // Indices changed, so find the playing song again by its path.
        auto it = track_index.find(playing_path);
        playing = it == track_index.end() ? -1 : it->second;
        queue = songs;
        queue_pos = index_of(queue, playing);

        if (tracks.empty()) message = "No audio files found in " + dir.string();
    }

    // Total size of every file under `folder`, skipping anything unreadable.
    static std::uintmax_t folder_size(const fs::path& folder) {
        std::uintmax_t total = 0;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code size_ec;
            if (it->is_regular_file(size_ec)) {
                auto n = it->file_size(size_ec);
                if (!size_ec) total += n;
            }
        }
        return total;
    }

    // 4.2 GB, or 350 MB under a gigabyte.
    static std::string human_size(std::uintmax_t bytes) {
        char buf[32];
        double gb = bytes / 1e9;
        if (gb >= 1) std::snprintf(buf, sizeof buf, "%.1f GB", gb);
        else std::snprintf(buf, sizeof buf, "%.0f MB", bytes / 1e6);
        return buf;
    }

    // Path relative to the library folder, as stored in library.tsv.
    std::string rel(const fs::path& p) const { return p.lexically_relative(dir).string(); }

    // Combine what's on disk with library.tsv: songs listed there but not
    // on this computer become `missing` tracks, and source URLs are attached.
    // Then write library.tsv back, so newly found songs get recorded.
    void merge_manifest() {
        std::unordered_map<std::string, ManifestEntry> listed;
        for (ManifestEntry& e : read_manifest(dir)) listed[e.path] = std::move(e);

        for (Track& t : tracks) {
            std::string r = rel(t.path);
            if (auto it = listed.find(r); it != listed.end()) {
                t.source = it->second.source;
                t.podcast = it->second.podcast;
                // A copy whose file has no number (e.g. on another computer
                // before :order ran there) still sorts by the listed one.
                if (t.number == 0) t.number = it->second.number;
                listed.erase(it);
            }
            if (auto it = pending_sources.find(r); it != pending_sources.end()) t.source = it->second;
            if (pending_podcasts.count(r)) t.podcast = true;
        }
        pending_sources.clear();
        pending_podcasts.clear();

        for (auto& [path, e] : listed) {  // what's left isn't on this computer
            Track t;
            t.path = dir / path;
            t.title = e.title.empty() ? fs::path(path).stem().string() : e.title;
            t.artist = e.artist.empty() ? "Unknown Artist" : e.artist;
            t.album = e.album;
            t.seconds = e.seconds;
            t.source = e.source;
            t.number = e.number;
            t.podcast = e.podcast;
            t.missing = true;
            tracks.push_back(std::move(t));
        }
        sort_tracks(tracks);
        save_manifest();
    }

    void save_manifest() {
        std::vector<ManifestEntry> entries;
        for (const Track& t : tracks) {
            entries.push_back({rel(t.path), t.artist == "Unknown Artist" ? "" : t.artist, t.album, t.title,
                               t.seconds, t.source, t.number, t.podcast});
        }
        std::sort(entries.begin(), entries.end(),
                  [](const ManifestEntry& a, const ManifestEntry& b) { return a.path < b.path; });
        if (!write_manifest(dir, entries)) message = "Couldn't write " + manifest_path(dir).string();
    }

    void build_artists() {
        std::string keep = artist_cur.sel < static_cast<int>(artists.size()) ? artists[artist_cur.sel] : "";
        artists = {"All", "♫ Playlists"};
        for (const Track& t : tracks) {
            // Tracks are sorted by artist, so equal names (ignoring case) are next to each other.
            if (artists.size() == 2 || lower(artists.back()) != lower(t.artist)) artists.push_back(t.artist);
        }
        artist_cur.sel = std::max(0, index_of_name(artists, keep));
    }

    void build_groups() {
        Group keep = group_cur.sel < static_cast<int>(groups.size()) ? groups[group_cur.sel] : Group{};
        groups.clear();

        if (artist_cur.sel == kPlaylists) {
            for (int i = 0; i < static_cast<int>(playlists.size()); ++i) {
                groups.push_back({Group::List, playlists[i].name, "", "", i});
            }
        } else {
            std::string artist = artist_cur.sel == kAllArtists ? "" : artists[artist_cur.sel];
            groups.push_back({Group::AllAlbums, artist.empty() ? "All songs" : "All albums", "", "", -1});
            for (const Track& t : tracks) {
                if (!artist.empty() && lower(t.artist) != lower(artist)) continue;
                bool seen = std::any_of(groups.begin(), groups.end(), [&](const Group& g) {
                    return g.kind == Group::Album && lower(g.album) == lower(t.album) &&
                           lower(g.artist) == lower(t.artist);
                });
                if (!seen) {
                    groups.push_back({Group::Album, t.album.empty() ? "(no album)" : t.album, t.artist, t.album});
                }
            }
        }

        // Stay on the same row if it still exists (e.g. after a rescan).
        group_cur.sel = 0;
        for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
            if (groups[i].kind == keep.kind && groups[i].label == keep.label && groups[i].artist == keep.artist)
                group_cur.sel = i;
        }
    }

    // Rebuild the song list from the selected album/playlist and the search filter.
    void refresh_songs() {
        int keep = songs.empty() ? -1 : songs[std::clamp(song_cur.sel, 0, (int)songs.size() - 1)];
        std::string needle = lower(filter);
        auto matches = [&](const Track& t) {
            return needle.empty() ||
                   lower(t.title + " " + t.artist + " " + t.album).find(needle) != std::string::npos;
        };

        songs.clear();
        slots.clear();
        visual = -1;
        if (groups.empty()) return;
        const Group& g = groups[group_cur.sel];

        if (g.kind == Group::List) {
            const auto& entries = playlists[g.playlist].songs;
            for (int j = 0; j < static_cast<int>(entries.size()); ++j) {
                auto it = track_index.find(entries[j].string());
                if (it == track_index.end() || !matches(tracks[it->second])) continue;  // missing file
                songs.push_back(it->second);
                slots.push_back(j);
            }
        } else {
            std::string artist = artist_cur.sel == kAllArtists ? "" : lower(artists[artist_cur.sel]);
            for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
                const Track& t = tracks[i];
                if (g.kind == Group::Album &&
                    (lower(t.artist) != lower(g.artist) || lower(t.album) != lower(g.album))) continue;
                if (g.kind == Group::AllAlbums && !artist.empty() && lower(t.artist) != artist) continue;
                if (!matches(t)) continue;
                songs.push_back(i);
            }
        }
        int pos = index_of(songs, keep);
        song_cur.sel = pos >= 0 ? pos : 0;
    }

    static int index_of(const std::vector<int>& v, int value) {
        auto it = std::find(v.begin(), v.end(), value);
        return it == v.end() ? -1 : static_cast<int>(it - v.begin());
    }

    static int index_of_name(const std::vector<std::string>& v, const std::string& name) {
        for (int i = 0; i < static_cast<int>(v.size()); ++i) {
            if (lower(v[i]) == lower(name)) return i;
        }
        return -1;
    }

    // Put the cursor on the song at `path`, moving as little as possible:
    //   1. if it's already listed where you are, just select it there;
    //   2. otherwise open its artist and album;
    //   3. otherwise fall back to All / All songs.
    void select_path(const fs::path& path) {
        auto it = track_index.find(path.string());
        if (it == track_index.end()) return;
        int track = it->second;
        pane = Pane::Songs;

        if (int row = index_of(songs, track); row >= 0) {  // 1. already in view
            song_cur.sel = row;
            return;
        }

        filter.clear();
        const Track& t = tracks[track];
        int artist = index_of_name(artists, t.artist);
        artist_cur.sel = artist >= 0 ? artist : kAllArtists;  // 2. its artist...
        build_groups();
        group_cur.sel = 0;
        for (int i = 0; i < static_cast<int>(groups.size()); ++i) {  // ...and album
            if (groups[i].kind == Group::Album && lower(groups[i].album) == lower(t.album)) group_cur.sel = i;
        }
        refresh_songs();

        if (index_of(songs, track) < 0) {  // 3. fall back to everything
            artist_cur.sel = kAllArtists;
            build_groups();
            group_cur.sel = 0;
            refresh_songs();
        }
        song_cur.sel = std::max(0, index_of(songs, track));
    }

    // =================================================================
    // Playback
    // =================================================================
    // Podcasts remember where you stopped and play at their own speed.
    bool is_podcast(int track) const { return tracks[track].podcast; }

    const Progress* progress_of(int track) const {
        auto it = progress.find(rel(tracks[track].path));
        return it == progress.end() ? nullptr : &it->second;
    }

    // Record where we are in the playing track, if it's a long one.
    void note_progress() {
        if (playing < 0 || !is_podcast(playing) || player.loading()) return;
        int pos = static_cast<int>(player.position());
        if (pos <= 0) return;
        Progress& p = progress[rel(tracks[playing].path)];
        // Stopping in the last 30 seconds counts as finished.
        Progress now = tracks[playing].seconds - pos < 30 ? Progress{0, true} : Progress{pos, false};
        if (now.seconds == p.seconds && now.done == p.done) return;
        p = now;
        progress_dirty = true;
    }

    // Write progress.tsv: right away if `now`, else at most every 15 seconds.
    void save_progress(bool now) {
        auto t = std::chrono::steady_clock::now();
        if (!progress_dirty || (!now && t - progress_written < std::chrono::seconds(15))) return;
        if (!write_progress(dir, progress)) message = "Couldn't write progress.tsv";
        progress_dirty = false;
        progress_written = t;
    }

    void start(int track) {
        note_progress();  // remember where we left the previous one
        save_progress(true);
        playing = track;
        // Resume a podcast where you stopped, backing up a few seconds
        // to catch the thread again. Finished ones start over.
        double from = 0;
        if (const Progress* p = progress_of(track); is_podcast(track) && p && !p->done && p->seconds > 10) {
            from = p->seconds - 5;
        }
        player.play(tracks[track].path.string(), from);
        player.set_speed(speed_of(track));
    }

    // The speed a track plays at: music always 1×, podcasts their artist's speed.
    double speed_of(int track) const {
        if (!is_podcast(track)) return 1.0;
        auto it = artist_speed.find(lower(tracks[track].artist));
        return it != artist_speed.end() ? it->second : long_speed;
    }

    // What < / > adjust: the playing podcast's artist, else the default.
    double current_speed() const {
        return playing >= 0 && is_podcast(playing) ? speed_of(playing) : long_speed;
    }

    // < / > and :speed. Applies to podcasts only.
    // While one is playing, sets the speed for its artist; otherwise the
    // default for artists that don't have one yet.
    void set_long_speed(double speed) {
        speed = std::clamp(std::round(speed * 100) / 100, 0.5, 3.0);
        std::error_code ec;
        fs::create_directories(state_file("speed").parent_path(), ec);
        if (playing >= 0 && is_podcast(playing)) {
            const std::string& artist = tracks[playing].artist;
            artist_speed[lower(artist)] = speed;
            player.set_speed(speed);
            message = "Speed " + format_speed(speed) + " for " + artist;
            std::ofstream out(state_file("speeds.tsv"));
            for (const auto& [name, sp] : artist_speed) out << name << '\t' << sp << '\n';
            return;
        }
        long_speed = speed;
        message = "Default speed " + format_speed(speed) + " for podcasts (ones you've set keep theirs)";
        if (FILE* f = std::fopen(state_file("speed").c_str(), "w")) {
            std::fprintf(f, "%g\n", long_speed);
            std::fclose(f);
        }
    }

    // v / :vis, b / :big and i / :now, each with an optional on|off.
    // Returns the new setting and saves it.
    bool set_panel(bool& flag, const std::string& file, const std::string& arg) {
        flag = arg == "on" ? true : arg == "off" ? false : !flag;
        write_flag(file, flag);
        if (flag) vis.clear_error();  // turning a spectrum on retries capture
        return flag;
    }
    void set_visualizer(const std::string& arg) {
        message = set_panel(show_vis, "visualizer", arg) ? "Mini spectrum on (v hides it)" : "Mini spectrum off (v shows it)";
    }
    void set_big(const std::string& arg) {
        message = set_panel(show_big, "big-spectrum", arg) ? "Big spectrum on (b hides it)" : "Big spectrum off (b shows it)";
    }
    void set_now_playing(const std::string& arg) {
        message = set_panel(show_now, "now-playing", arg) ? "Now playing on (i hides it)" : "Now playing off (i shows it)";
    }

    // A track played to its end on its own.
    void finished() {
        if (playing >= 0 && is_podcast(playing)) {
            progress[rel(tracks[playing].path)] = {0, true};
            progress_dirty = true;
        }
        next();
    }

    // Called once on quit.
    void shutdown() {
        note_progress();
        save_progress(true);
    }

    // Enter on a song: play through the current list starting there.
    void play_from_list(int list_pos) {
        if (songs.empty()) return;
        if (tracks[songs[list_pos]].missing) {  // not downloaded here yet: fetch it instead
            restore({rel(tracks[songs[list_pos]].path)});
            return;
        }
        queue = songs;
        queue_pos = list_pos;
        if (shuffle) shuffle_rest();
        start(queue[queue_pos]);
    }

    // Shuffle everything after the current song.
    void shuffle_rest() {
        if (queue_pos + 1 < static_cast<int>(queue.size())) {
            std::shuffle(queue.begin() + queue_pos + 1, queue.end(), rng);
        }
    }

    void next() {
        // Skip songs that aren't downloaded on this computer.
        while (queue_pos + 1 < static_cast<int>(queue.size()) && tracks[queue[queue_pos + 1]].missing) ++queue_pos;
        if (queue_pos + 1 < static_cast<int>(queue.size())) {
            start(queue[++queue_pos]);
        } else {
            player.stop();
            playing = -1;
            message = "End of queue";
        }
    }

    void prev() {
        // Like most players: more than 3s in, "previous" restarts the song.
        if (playing < 0) return;
        if (player.position() > 3 || queue_pos <= 0) {
            player.seek(-player.position());
        } else {
            int i = queue_pos - 1;
            while (i > 0 && tracks[queue[i]].missing) --i;
            if (!tracks[queue[i]].missing) start(queue[queue_pos = i]);
        }
    }

    void toggle_shuffle() {
        shuffle = !shuffle;
        if (shuffle) {
            shuffle_rest();
        } else if (playing >= 0) {
            // Back to list order, carrying on from the current song.
            queue = songs;
            queue_pos = index_of(queue, playing);
            if (queue_pos < 0) { queue = {playing}; queue_pos = 0; }
        }
        message = shuffle ? "Shuffle on" : "Shuffle off";
    }

    // Move the cursor to the playing song, clearing filters if needed.
    void jump_to_playing() {
        if (playing < 0) return;
        if (index_of(songs, playing) < 0) select_path(tracks[playing].path);
        song_cur.sel = std::max(0, index_of(songs, playing));
        pane = Pane::Songs;
    }

    // =================================================================
    // Playlists
    // =================================================================
    Playlist* current_playlist() {
        if (groups.empty() || groups[group_cur.sel].kind != Group::List) return nullptr;
        return &playlists[groups[group_cur.sel].playlist];
    }

    std::vector<std::string> playlist_names() const {
        std::vector<std::string> names;
        for (const Playlist& p : playlists) names.push_back(p.name);
        return names;
    }

    void save(const Playlist& pl) {
        if (!save_playlist(pl, tracks)) message = "Couldn't save playlist " + pl.name;
    }

    // Index of the playlist called `name` (any case), creating it if asked.
    int playlist_index(const std::string& name, bool create) {
        int i = index_of_name(playlist_names(), name);
        if (i >= 0 || !create) return i;
        playlists.push_back({name, playlist_dir(dir) / (safe_filename(name) + ".m3u"), {}});
        sort_playlists();
        return index_of_name(playlist_names(), name);
    }

    void sort_playlists() {
        std::sort(playlists.begin(), playlists.end(),
                  [](const Playlist& a, const Playlist& b) { return lower(a.name) < lower(b.name); });
    }

    // Playlists were added/renamed/removed: rebuild the panes that show them.
    void playlists_changed(const std::string& select_name = "") {
        build_groups();
        if (!select_name.empty() && artist_cur.sel == kPlaylists) {
            group_cur.sel = std::max(0, index_of_name(playlist_names(), select_name));
        }
        refresh_songs();
    }

    // Open a playlist: select "♫ Playlists" and that playlist.
    void show_playlist(const std::string& name) {
        artist_cur.sel = kPlaylists;
        filter.clear();
        playlists_changed(name);
        pane = Pane::Albums;
    }

    // Rows the next yank/delete/add applies to: the V selection, or `n`
    // rows from the cursor (so 3yy yanks three songs).
    std::pair<int, int> row_range(int n) const {
        int last = static_cast<int>(songs.size()) - 1;
        if (visual >= 0) return {std::min(visual, song_cur.sel), std::min(std::max(visual, song_cur.sel), last)};
        return {song_cur.sel, std::min(song_cur.sel + n - 1, last)};
    }

    std::vector<fs::path> paths_in(std::pair<int, int> range) const {
        std::vector<fs::path> out;
        for (int i = range.first; i <= range.second; ++i) out.push_back(tracks[songs[i]].path);
        return out;
    }

    void yank(int n) {
        if (songs.empty()) return;
        yanked = paths_in(row_range(n));
        visual = -1;
        message = "Yanked " + plural(yanked.size(), "song") + " (p pastes into a playlist)";
    }

    void delete_rows(int n) {
        if (songs.empty()) return;
        Playlist* pl = current_playlist();
        if (!pl) {  // not in a playlist: delete the songs themselves
            auto [first, last] = row_range(n);
            visual = -1;
            delete_songs(std::vector<int>(songs.begin() + first, songs.begin() + last + 1), "");
            return;
        }
        auto range = row_range(n);
        yanked = paths_in(range);  // like vim, deleting also yanks, so dd + p moves a song
        std::vector<int> doomed(slots.begin() + range.first, slots.begin() + range.second + 1);
        std::sort(doomed.rbegin(), doomed.rend());  // erase from the back so indices stay valid
        for (int slot : doomed) pl->songs.erase(pl->songs.begin() + slot);
        save(*pl);
        refresh_songs();
        song_cur.sel = range.first;
        message = "Removed " + plural(doomed.size(), "song") + " from " + pl->name;
    }

    void put(bool before) {
        Playlist* pl = current_playlist();
        if (!pl && !cut.empty()) return put_cut(before);
        if (!pl) {
            message = "Open a playlist to paste into it (or press A to add to one)";
            return;
        }
        if (yanked.empty()) {
            message = "Nothing yanked yet (yy copies a song)";
            return;
        }
        int at = songs.empty() ? static_cast<int>(pl->songs.size())
                               : slots[song_cur.sel] + (before ? 0 : 1);
        pl->songs.insert(pl->songs.begin() + at, yanked.begin(), yanked.end());
        save(*pl);
        refresh_songs();
        for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
            if (slots[i] >= at) { song_cur.sel = i; break; }
        }
        message = "Put " + plural(yanked.size(), "song") + " into " + pl->name;
    }

    // J / K: swap the song with its neighbour, in a playlist or an album.
    void move_song(int delta) {
        Playlist* pl = current_playlist();
        if (!pl) return move_in_album(delta);
        int other = song_cur.sel + delta;
        if (songs.empty() || other < 0 || other >= static_cast<int>(songs.size())) return;
        std::swap(pl->songs[slots[song_cur.sel]], pl->songs[slots[other]]);
        save(*pl);
        refresh_songs();
        song_cur.sel = other;
    }

    // J / K in an album: swap the song with its neighbour and renumber the
    // whole album 1, 2, 3... in the new order (tags and library.tsv).
    void move_in_album(int delta) {
        if (groups.empty() || groups[group_cur.sel].kind != Group::Album) {
            message = "J / K reorder songs inside an album or a playlist";
            return;
        }
        if (!filter.empty()) { message = "Clear the search first (Esc), then J / K"; return; }
        int other = song_cur.sel + delta;
        if (songs.empty() || other < 0 || other >= static_cast<int>(songs.size())) return;

        std::vector<int> order = songs;
        std::swap(order[song_cur.sel], order[other]);
        renumber_album(order, tracks[order[other]].path.string());
    }

    // True (with a message if not) when the songs pane shows a whole album
    // that can be reordered.
    bool album_reorderable() {
        if (groups.empty() || groups[group_cur.sel].kind != Group::Album) {
            message = "Open an album or a playlist to reorder it";
            return false;
        }
        if (!filter.empty()) { message = "Clear the search first (Esc)"; return false; }
        return !songs.empty();
    }

    // x in an album: remember the selected songs, to put back elsewhere with p / P.
    void cut_rows(int n) {
        if (songs.empty()) return;
        if (current_playlist()) return delete_rows(n);  // a playlist already does this with dd
        if (!album_reorderable()) return;
        cut = paths_in(row_range(n));
        visual = -1;
        message = "Cut " + plural(cut.size(), "song") + " — p / P puts it after / before the selected song";
    }

    // p / P in an album: move the cut songs after / before the selected one.
    void put_cut(bool before) {
        if (!album_reorderable()) return;
        std::set<std::string> moving;
        for (const fs::path& p : cut) moving.insert(p.string());
        std::vector<int> rest, cut_rows;
        int at = -1;
        for (int i = 0; i < static_cast<int>(songs.size()); ++i) {
            bool is_cut = moving.count(tracks[songs[i]].path.string());
            if (i == song_cur.sel) at = static_cast<int>(rest.size()) + (before || is_cut ? 0 : 1);
            (is_cut ? cut_rows : rest).push_back(songs[i]);
        }
        if (cut_rows.empty()) { message = "The cut songs aren't in this album"; return; }
        if (cut_rows.size() != cut.size()) { message = "x / p move songs within one album"; return; }
        std::vector<int> order = rest;
        order.insert(order.begin() + at, cut_rows.begin(), cut_rows.end());
        renumber_album(order, tracks[cut_rows[0]].path.string());
        cut.clear();
        message = "Moved " + plural(cut_rows.size(), "song");
    }

    // Number an album's songs 1, 2, 3... in `order` (indices into tracks),
    // in their tags and library.tsv, and keep the cursor on `keep`.
    void renumber_album(const std::vector<int>& order, const std::string& keep) {
        std::string moved = keep;
        int failed = 0;
        for (int n = 1; n <= static_cast<int>(order.size()); ++n) {
            Track& t = tracks[order[n - 1]];
            if (t.number == n) continue;
            t.number = n;
            if (!t.missing && !write_tags(t.path, t.title, t.artist == "Unknown Artist" ? "" : t.artist, t.album, n))
                ++failed;
        }
        resort_tracks();
        save_manifest();
        refresh_songs();
        if (auto it = track_index.find(moved); it != track_index.end()) song_cur.sel = std::max(0, index_of(songs, it->second));
        if (failed) message = std::to_string(failed) + " couldn't be retagged";
    }

    // Sort `tracks` again after numbers changed, keeping everything that
    // points into it (what's playing, the queue) on the same songs.
    void resort_tracks() {
        std::string playing_path = playing >= 0 ? tracks[playing].path.string() : "";
        std::vector<std::string> queue_paths;
        for (int i : queue) queue_paths.push_back(tracks[i].path.string());
        sort_tracks(tracks);
        track_index.clear();
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i) track_index[tracks[i].path.string()] = i;
        auto find = [&](const std::string& path) {
            auto it = track_index.find(path);
            return it == track_index.end() ? -1 : it->second;
        };
        playing = playing_path.empty() ? -1 : find(playing_path);
        for (size_t i = 0; i < queue.size(); ++i) queue[i] = find(queue_paths[i]);
    }

    // A: choose a playlist to add the selected songs to.
    void begin_pick(int n) {
        if (songs.empty()) return;
        picking = paths_in(row_range(n));
        visual = -1;
        mode = Mode::Pick;
        input.clear();
        pick = -1;
    }

    std::string pick_value() const {
        auto sugg = complete(playlist_names(), input);
        return pick >= 0 && pick < static_cast<int>(sugg.size()) ? sugg[pick] : input;
    }

    bool on_pick_key(const Event& e) {
        int n_sugg = static_cast<int>(complete(playlist_names(), input).size());
        if (e == Event::Escape) {
            mode = Mode::Normal;
        } else if (e == Event::Return) {
            std::string name = trim(pick_value());
            if (name.empty()) {
                message = "Type a playlist name, or pick one with Ctrl-n";
                return true;
            }
            int i = playlist_index(name, true);
            Playlist& pl = playlists[i];
            pl.songs.insert(pl.songs.end(), picking.begin(), picking.end());
            save(pl);
            mode = Mode::Normal;
            playlists_changed(artist_cur.sel == kPlaylists ? groups[group_cur.sel].label : "");
            message = "Added " + plural(picking.size(), "song") + " to " + pl.name;
        } else if (e == Event::CtrlN || e == Event::ArrowDown) {
            if (n_sugg) pick = pick + 1 < n_sugg ? pick + 1 : -1;
        } else if (e == Event::CtrlP || e == Event::ArrowUp) {
            if (n_sugg) pick = pick < 0 ? n_sugg - 1 : pick - 1;
        } else if (e == Event::Backspace) {
            input = pick_value();
            pop_char(input);
            pick = -1;
        } else if (e == Event::CtrlU) {
            input.clear();
            pick = -1;
        } else if (e.is_character()) {
            input = pick_value() + e.character();
            pick = -1;
        }
        return true;
    }

    void new_playlist(const std::string& name) {
        if (name.empty()) { message = "Usage: :new <playlist name>"; return; }
        bool existed = playlist_index(name, false) >= 0;
        int i = playlist_index(name, true);
        if (!existed) save(playlists[i]);
        show_playlist(playlists[i].name);
        message = existed ? "Playlist " + playlists[i].name + " already exists"
                          : "Created playlist " + playlists[i].name + " (A adds songs to it)";
    }

    void rename_playlist(const std::string& name) {
        Playlist* pl = current_playlist();
        if (!pl) { message = "Select a playlist first (♫ Playlists in the artists pane)"; return; }
        if (name.empty()) { message = "Usage: :rename <new name>"; return; }
        int other = playlist_index(name, false);
        if (other >= 0 && &playlists[other] != pl) { message = "There's already a playlist called " + name; return; }

        fs::path new_file = playlist_dir(dir) / (safe_filename(name) + ".m3u");
        std::error_code ec;
        if (new_file != pl->file) fs::rename(pl->file, new_file, ec);
        if (ec) { message = "Couldn't rename: " + ec.message(); return; }
        std::string old = pl->name;
        pl->name = name;
        pl->file = new_file;
        sort_playlists();
        playlists_changed(name);
        message = "Renamed " + old + " to " + name;
    }

    void delete_playlist() {
        Playlist* pl = current_playlist();
        if (!pl) { message = "Select a playlist first (♫ Playlists in the artists pane)"; return; }
        std::string name = pl->name;
        confirm_text = "Delete playlist \"" + name + "\"? Your songs are kept. (y/n)";
        confirm_action = [this, name] {
            int i = playlist_index(name, false);
            if (i < 0) return;
            std::error_code ec;
            fs::remove(playlists[i].file, ec);
            playlists.erase(playlists.begin() + i);
            playlists_changed();
            message = "Deleted playlist " + name;
        };
        mode = Mode::Confirm;
    }

    // =================================================================
    // Missing songs: download them from library.tsv's source URLs
    // =================================================================
    const Track* track_at(const std::string& relative) const {
        auto it = track_index.find((dir / relative).string());
        return it == track_index.end() ? nullptr : &tracks[it->second];
    }

    // Queue missing songs for download. Ones without a URL are skipped.
    void restore(const std::vector<std::string>& paths) {
        int queued = 0, no_source = 0;
        for (const std::string& r : paths) {
            const Track* t = track_at(r);
            if (!t || !t->missing) continue;
            if (t->source.empty()) { ++no_source; continue; }
            if (r == restoring || std::find(restore_queue.begin(), restore_queue.end(), r) != restore_queue.end())
                continue;
            restore_queue.push_back(r);
            ++queued;
        }
        if (queued) message = "Downloading " + plural(queued, "missing song");
        if (no_source) {
            message += (queued ? " · " : "") + plural(no_source, "song") +
                       " with no source URL (:source <url> sets one)";
        }
        pump_restores();
    }

    // Start the next queued download if nothing else is downloading.
    void pump_restores() {
        if (!restoring.empty() || downloader.busy() || restore_queue.empty()) return;
        restoring = restore_queue.front();
        restore_queue.erase(restore_queue.begin());
        const Track* t = track_at(restoring);
        if (!t) { restoring.clear(); return pump_restores(); }

        // Ask for the same format as before, so the file name (and any
        // playlist pointing at it) stays exactly the same.
        static const std::unordered_map<std::string, std::string> formats = {
            {".opus", "opus"}, {".m4a", "m4a"}, {".mp3", "mp3"}, {".flac", "flac"},
            {".ogg", "vorbis"}, {".wav", "wav"}, {".aac", "aac"},
        };
        if (t->podcast) return (void)downloader.start(t->source, incoming_dir(), kPodcastFormat, false, kPodcastQuality);
        auto f = formats.find(lower(t->path.extension().string()));
        downloader.start(t->source, incoming_dir(), f == formats.end() ? "best" : f->second);
    }

    // A restore download finished: tag it from library.tsv and put it where it belongs.
    void finish_restore(const DownloadResult& result) {
        std::string r = std::exchange(restoring, "");
        const Track* found = track_at(r);
        if (!found) return;
        const Track copy = *found;  // a copy: `tracks` may change below
        const Track* t = &copy;
        if (!result.ok) {
            message = "Couldn't download " + t->title + ": " + result.error;
            return;
        }
        fs::path dest = t->path;
        if (dest.extension() != result.file.extension()) {
            // Got a different format after all: keep the new extension.
            dest.replace_extension(result.file.extension());
            pending_sources[rel(dest)] = t->source;
            if (t->podcast) pending_podcasts.insert(rel(dest));
            tracks.erase(tracks.begin() + track_index[copy.path.string()]);  // drop the old entry
            save_manifest();
        }
        write_tags(result.file, t->title, t->artist == "Unknown Artist" ? "" : t->artist, t->album, t->number);
        std::string title = t->title;
        if (!move_file(result.file, dest)) {
            message = "Downloaded " + title + ", but couldn't move it to " + dest.string();
            return;
        }
        load();
        int left = static_cast<int>(restore_queue.size());
        message = "Downloaded " + title + (left ? " (" + std::to_string(left) + " to go)" : "");
    }

    // :sync [artist|album]: download the missing songs of the whole
    // library, the selected artist, or the selected album / playlist.
    void sync(const std::string& scope) {
        std::vector<int> picked;  // indices into `tracks`
        std::string where;
        if (scope.empty() || scope == "all") {
            for (int i = 0; i < static_cast<int>(tracks.size()); ++i) picked.push_back(i);
            where = "library.tsv";
        } else if (scope == "artist") {
            if (artist_cur.sel == kAllArtists || artist_cur.sel == kPlaylists) {
                message = "Select an artist in the artists pane first";
                return;
            }
            std::string artist = lower(artists[artist_cur.sel]);
            for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
                if (lower(tracks[i].artist) == artist) picked.push_back(i);
            where = artists[artist_cur.sel];
        } else if (scope == "album") {
            if (groups.empty()) { message = "Select an album first"; return; }
            const Group& g = groups[group_cur.sel];
            if (g.kind == Group::List) {
                for (const fs::path& p : playlists[g.playlist].songs)
                    if (auto it = track_index.find(p.string()); it != track_index.end()) picked.push_back(it->second);
            } else {
                std::string artist = artist_cur.sel == kAllArtists ? "" : lower(artists[artist_cur.sel]);
                for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
                    const Track& t = tracks[i];
                    if (g.kind == Group::Album ? lower(t.artist) == lower(g.artist) && lower(t.album) == lower(g.album)
                                               : artist.empty() || lower(t.artist) == artist)
                        picked.push_back(i);
                }
            }
            where = g.label;
        } else {
            message = "Usage: :sync [artist|album]  (no argument syncs everything)";
            return;
        }
        std::vector<std::string> missing;
        for (int i : picked) if (tracks[i].missing) missing.push_back(rel(tracks[i].path));
        if (missing.empty()) message = "Everything in " + where + " is already downloaded";
        else restore(missing);
    }

    // :podcast [on|off]: mark (or unmark) the selected song, album or artist
    // as a podcast. Without on/off it flips, going by the first song.
    // What a command like :podcast or :shrink acts on: the selected song(s)
    // in the songs pane, else the selected album or artist. Indices into
    // `tracks`; `what` names it for messages. Empty (with a message) if
    // nothing suitable is selected.
    std::vector<int> selected_tracks(std::string& what) {
        std::vector<int> picked;
        if (pane == Pane::Songs) {
            if (songs.empty()) return {};
            auto [first, last] = row_range(1);
            picked.assign(songs.begin() + first, songs.begin() + last + 1);
            what = picked.size() == 1 ? tracks[picked[0]].title : plural(picked.size(), "song");
        } else if (pane == Pane::Artists) {
            if (artist_cur.sel == kAllArtists || artist_cur.sel == kPlaylists) { message = "Select an artist first"; return {}; }
            for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
                if (lower(tracks[i].artist) == lower(artists[artist_cur.sel])) picked.push_back(i);
            what = artists[artist_cur.sel];
        } else {
            if (groups.empty() || groups[group_cur.sel].kind != Group::Album) { message = "Select an album first"; return {}; }
            const Group& g = groups[group_cur.sel];
            for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
                if (lower(tracks[i].artist) == lower(g.artist) && lower(tracks[i].album) == lower(g.album)) picked.push_back(i);
            what = g.label;
        }
        return picked;
    }

    void mark_podcast(const std::string& arg) {
        if (arg != "" && arg != "on" && arg != "off") { message = "Usage: :podcast [on|off]"; return; }
        std::string what;
        std::vector<int> picked = selected_tracks(what);
        if (picked.empty()) return;
        bool on = arg == "on" ? true : arg == "off" ? false : !tracks[picked[0]].podcast;
        for (int i : picked) tracks[i].podcast = on;
        visual = -1;
        save_manifest();
        message = what + (on ? " is now a podcast (keeps your place, < / > speed)" : " is no longer a podcast");
    }

    // :shrink: re-encode the selected song(s), album or artist to low-bitrate
    // Opus, in the background. Works without a link, unlike :sync.
    void shrink() {
        if (shrinker.busy()) { message = "Already shrinking (see the bottom right)"; return; }
        std::string what;
        std::vector<int> picked = selected_tracks(what);
        if (picked.empty()) return;
        std::vector<ShrinkJob> jobs;
        int small = 0, absent = 0, now_playing = 0;
        for (int i : picked) {
            const Track& t = tracks[i];
            if (t.missing) { ++absent; continue; }
            if (i == playing) { ++now_playing; continue; }  // mpv has it open
            std::error_code ec;
            std::uintmax_t bytes = fs::file_size(t.path, ec);
            if (ec) continue;
            if (t.seconds > 0 && bytes * 8 / t.seconds < kShrinkSkipBps) { ++small; continue; }
            // Encode into the cache, so a rescan never finds half a file in the library.
            fs::path out = incoming_dir() / ("shrinking-" + std::to_string(jobs.size()) + ".opus");
            jobs.push_back({t.path, out, t.seconds});
        }
        std::string notes;
        if (small) notes += " · " + std::to_string(small) + " already small";
        if (absent) notes += " · " + std::to_string(absent) + " not downloaded";
        if (now_playing) notes += " · skipped the playing song";
        if (jobs.empty()) { message = "Nothing to shrink in " + what + notes; return; }

        std::error_code ec;
        fs::create_directories(incoming_dir(), ec);
        shrinking = true;
        shrink_done = shrink_failed = 0;
        shrink_saved = 0;
        shrink_error.clear();
        shrinker.start(std::move(jobs), kShrinkBitrate);
        message = "Shrinking " + what + " (" + plural(shrinker.count(), "file") + ") to 48 kbps" + notes;
    }

    // :order: look the selected album up on MusicBrainz and number its songs
    // the way the album has them (otherwise unnumbered songs sort by title).
    void order_album_tracks() {
        if (album_lookup.busy()) { message = "Already looking up " + order_album; return; }
        std::string artist, album;
        if (pane == Pane::Songs && !songs.empty()) {
            const Track& t = tracks[songs[song_cur.sel]];
            artist = t.artist;
            album = t.album;
        } else if (pane == Pane::Albums && !groups.empty() && groups[group_cur.sel].kind == Group::Album) {
            artist = groups[group_cur.sel].artist;
            album = groups[group_cur.sel].album;
        } else {
            message = "Select an album first";
            return;
        }
        if (album.empty()) { message = "That song isn't on an album"; return; }
        if (artist == "Unknown Artist") { message = "Set the album's artist first (e on the artist)"; return; }

        std::vector<std::string> titles;
        order_paths.clear();
        for (const Track& t : tracks) {
            if (lower(t.artist) != lower(artist) || lower(t.album) != lower(album)) continue;
            titles.push_back(t.title);
            order_paths.push_back(t.path.string());
        }
        order_album = album;
        album_lookup.start(artist, album, std::move(titles));
        message = "Looking up " + album + " — " + artist + " on MusicBrainz…";
    }

    // Called on every event: numbers the album once the lookup is back.
    void check_order() {
        if (mode != Mode::Normal) return;  // wait until the user isn't typing
        std::optional<AlbumOrder> result = album_lookup.take_result();
        if (!result) return;
        if (!result->ok) { message = "Couldn't order " + order_album + ": " + result->error; return; }

        // Songs that aren't on the album go after the ones that are.
        int next = *std::max_element(result->numbers.begin(), result->numbers.end()) + 1;
        int matched = 0, failed = 0;
        std::string selected = pane == Pane::Songs && !songs.empty() ? tracks[songs[song_cur.sel]].path.string() : "";
        for (size_t i = 0; i < order_paths.size(); ++i) {
            auto it = track_index.find(order_paths[i]);
            if (it == track_index.end()) continue;  // gone since the lookup started
            Track& t = tracks[it->second];
            int n = result->numbers[i];
            if (n > 0) ++matched;
            else n = next++;
            if (n == t.number) continue;
            t.number = n;
            if (!t.missing && !write_tags(t.path, t.title, t.artist == "Unknown Artist" ? "" : t.artist, t.album, n))
                ++failed;
        }
        save_manifest();  // numbers for songs not on this computer live here
        load();
        if (!selected.empty()) select_path(selected);
        int total = static_cast<int>(order_paths.size());
        message = "Ordered " + order_album + " like " + result->release;
        if (matched < total) message += " · " + std::to_string(total - matched) + " not on it, put at the end";
        if (failed) message += " · " + std::to_string(failed) + " couldn't be tagged";
    }

    // Called on every event: swaps finished files into the library.
    void check_shrink() {
        if (mode != Mode::Normal) return;  // wait until the user isn't typing
        bool finished = !shrinker.busy();  // checked first: then every result is already in
        std::vector<ShrinkResult> results = shrinker.take_results();
        for (const ShrinkResult& r : results) finish_shrink(r);
        if (!results.empty()) load();
        if (finished && shrinking) {
            shrinking = false;
            message = "Shrunk " + plural(shrink_done, "file") + ", saved " + human_size(shrink_saved) +
                      " (originals in the trash)";
            if (shrink_failed) message += " · " + std::to_string(shrink_failed) + " failed: " + shrink_error;
        }
    }

    // Replace one file with its shrunk copy: the original goes to the trash,
    // and if the name changed (Money.mp3 -> Money.opus), library.tsv,
    // playlists and the saved position follow it.
    void finish_shrink(const ShrinkResult& r) {
        const fs::path& old = r.job.in;
        const fs::path& small = r.job.out;
        std::error_code ec;
        auto fail = [&](const std::string& why) {
            fs::remove(small, ec);
            ++shrink_failed;
            shrink_error = why;
        };
        if (!r.ok) return fail(r.error);
        auto it = track_index.find(old.string());
        if (it == track_index.end() || tracks[it->second].missing) return fail("deleted while shrinking");
        if (it->second == playing) return fail("it started playing");
        Track& t = tracks[it->second];

        std::uintmax_t before = fs::file_size(old, ec), after = fs::file_size(small, ec);
        if (ec || after >= before) { fs::remove(small, ec); return; }  // no smaller: keep the original
        write_tags(small, t.title, t.artist == "Unknown Artist" ? "" : t.artist, t.album, t.number);

        fs::path dest = old;
        dest.replace_extension(".opus");
        if (dest != old && fs::exists(dest)) dest = free_path(old.parent_path(), old.stem().string(), ".opus");
        std::string error;
        if (!move_to_trash(old, error)) return fail(error);
        if (!move_file(small, dest)) {
            ++shrink_failed;
            shrink_error = "couldn't move the new file into place (the original is in the trash)";
            return;
        }
        if (dest != old) {
            t.path = dest;
            for (Playlist& pl : playlists) {
                bool changed = false;
                for (fs::path& p : pl.songs)
                    if (p == old) { p = dest; changed = true; }
                if (changed) save(pl);
            }
            if (auto p = progress.find(rel(old)); p != progress.end()) {
                progress[rel(dest)] = p->second;
                progress.erase(rel(old));
                progress_dirty = true;
                save_progress(true);
            }
            save_manifest();
        }
        ++shrink_done;
        shrink_saved += before - after;
    }

    void set_source(const std::string& url) {
        if (pane != Pane::Songs || songs.empty()) { message = "Select a song first"; return; }
        if (url.empty()) { message = "Usage: :source <url>  (where this song can be downloaded)"; return; }
        Track& t = tracks[songs[song_cur.sel]];
        t.source = url;
        save_manifest();
        message = "Saved source for " + t.title;
    }

    void forget_song() {
        if (pane != Pane::Songs || songs.empty()) return;
        const Track& t = tracks[songs[song_cur.sel]];
        if (!t.missing) {
            message = ":forget only removes songs that aren't downloaded here (delete the file to remove one)";
            return;
        }
        std::string title = t.title;
        tracks.erase(tracks.begin() + songs[song_cur.sel]);
        save_manifest();
        load();
        message = "Removed " + title + " from library.tsv";
    }

    // =================================================================
    // Deleting
    // =================================================================

    // dd in the artists or albums pane: delete everything it contains.
    void delete_group() {
        if (pane == Pane::Artists) {
            delete_artist_here();
        } else if (pane == Pane::Albums && !groups.empty()) {
            const Group& g = groups[group_cur.sel];
            if (g.kind == Group::List) return delete_playlist();
            if (g.kind == Group::AllAlbums) {
                message = "Pick an album to delete, or go to the artists pane to delete the whole artist";
                return;
            }
            delete_album(g.artist, g.album);
        }
    }

    // dr: the selected album. Only in the albums pane, so a stray dr while
    // browsing songs can't take out a whole album.
    void delete_album_here() {
        if (pane == Pane::Albums && !groups.empty() && groups[group_cur.sel].kind == Group::Album) {
            delete_album(groups[group_cur.sel].artist, groups[group_cur.sel].album);
        } else {
            message = "dr deletes an album: select one in the albums pane";
        }
    }

    // da: the selected artist. Only in the artists pane, for the same reason.
    void delete_artist_here() {
        std::string artist;
        if (pane == Pane::Artists && artist_cur.sel != kAllArtists && artist_cur.sel != kPlaylists) {
            artist = artists[artist_cur.sel];
        }
        if (artist.empty()) { message = "da deletes an artist: select one in the artists pane"; return; }
        std::vector<int> doomed;
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
            if (lower(tracks[i].artist) == lower(artist)) doomed.push_back(i);
        }
        delete_songs(doomed, "everything by " + artist);
    }

    void delete_album(const std::string& artist, const std::string& album) {
        std::vector<int> doomed;
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
            if (lower(tracks[i].artist) == lower(artist) && lower(tracks[i].album) == lower(album))
                doomed.push_back(i);
        }
        delete_songs(doomed, "the album " + (album.empty() ? "(no album) by " + artist : album));
    }

    // Ask what to delete:
    //   y (or Enter): just the downloaded files. The songs stay in library.tsv
    //                 and in playlists as missing (⇣), so :sync can get them back.
    //   a:            everything, files and library.tsv entries.
    // `what` describes them in the question ("the album X"); "" = list titles.
    void delete_songs(const std::vector<int>& doomed, const std::string& what) {
        if (doomed.empty()) return;
        std::vector<fs::path> paths;
        int on_disk = 0, no_link = 0;  // no_link: downloaded here, but with no URL to get it back
        for (int i : doomed) {
            paths.push_back(tracks[i].path);
            if (!tracks[i].missing) ++on_disk;
            if (!tracks[i].missing && tracks[i].source.empty()) ++no_link;
        }
        std::string subject = !what.empty() ? what + " (" + plural(paths.size(), "song") + ")"
                            : paths.size() == 1 ? "\"" + tracks[doomed[0]].title + "\""
                                                : plural(paths.size(), "song");
        // Remember paths, not indices: indices change whenever the library reloads.
        if (on_disk == 0) {  // nothing downloaded here, so only forgetting is left
            confirm_text = "Remove " + subject + " from library.tsv? It isn't downloaded here. (y/n)";
            confirm_action = [this, paths] { really_delete(paths, true); };
        } else {
            confirm_text = "Delete " + subject + ": y = downloads only (keep in library) · a = everything · n = cancel [Y/a/n]";
            confirm_action = [this, paths] { really_delete(paths, false); };
            if (no_link) {
                // Keeping the details is only useful if it can be downloaded
                // again, so say when it can't before trashing it.
                std::string these = paths.size() == 1 ? "No link" : std::to_string(no_link) + " with no link";
                confirm_action = [this, paths, these] {
                    confirm_text = these + ": details kept, but can't redownload. Delete? (y/n)";
                    confirm_action = [this, paths] { really_delete(paths, false); };
                    mode = Mode::Confirm;
                };
            }
            confirm_all = [this, paths] { really_delete(paths, true); };
            confirm_enter = true;
        }
        mode = Mode::Confirm;
    }

    // Trash the downloaded files. With `forget`, also drop the songs from
    // library.tsv, playlists and the download queue.
    void really_delete(const std::vector<fs::path>& paths, bool forget) {
        std::vector<std::string> gone;  // as strings, for quick comparisons
        int trashed = 0;
        std::string error;
        for (const fs::path& p : paths) {
            auto it = track_index.find(p.string());
            if (it == track_index.end()) continue;
            const Track& t = tracks[it->second];
            if (!t.missing) {
                if (it->second == playing) {  // don't keep playing a deleted song
                    player.stop();
                    playing = -1;
                }
                if (!move_to_trash(t.path, error)) continue;  // couldn't: keep it everywhere
                ++trashed;
                remove_empty_folders(t.path.parent_path());
            }
            gone.push_back(p.string());
        }
        std::string undo = trashed ? " (" + std::to_string(trashed) + " in the trash, restore from your file manager)" : "";

        if (!forget) {
            // library.tsv already lists them, so the reload turns them into missing songs.
            load();
            message = "Removed " + plural(trashed, "download") + undo + " · :sync gets them back";
            if (!error.empty()) message += " · some failed: " + error;
            return;
        }

        auto is_gone = [&](const fs::path& p) {
            return std::find(gone.begin(), gone.end(), p.string()) != gone.end();
        };
        // Out of every playlist...
        for (Playlist& pl : playlists) {
            auto before = pl.songs.size();
            std::erase_if(pl.songs, is_gone);
            if (pl.songs.size() != before) save(pl);
        }
        // ...out of the download queue...
        std::erase_if(restore_queue, [&](const std::string& r) { return is_gone(dir / r); });
        // ...and out of library.tsv, or it would come back as a missing (⇣) song.
        std::erase_if(tracks, [&](const Track& t) { return is_gone(t.path); });
        save_manifest();
        load();

        message = "Deleted " + plural(gone.size(), "song") + undo;
        if (!error.empty()) message += " · some failed: " + error;
    }

    // Remove `folder` and its parents while they're empty, stopping at the library.
    void remove_empty_folders(fs::path folder) {
        std::error_code ec;
        while (folder != dir && folder.string().starts_with(dir.string()) && fs::is_empty(folder, ec) && !ec) {
            fs::remove(folder, ec);
            folder = folder.parent_path();
        }
    }

    // =================================================================
    // Adding songs and editing tags
    // =================================================================

    // as / aa / ap: ask for a URL, pre-filled from the clipboard.
    // aS / aA / aP (`listing`): the same, but only list it, without downloading.
    void begin_add(AddKind kind, bool listing = false) {
        if (downloader.busy() || !restoring.empty()) {
            message = "A download is already running";
            return;
        }
        add_kind = kind;
        add_listing = listing;
        mode = Mode::Url;
        input = clipboard_url();
    }

    // Download what was typed in the `a` prompt. A playlist URL becomes an
    // album; a video that's in a playlist asks which one is meant.
    void start_add(const std::string& url) {
        last_url = url;
        if (add_kind == AddKind::Album) {
            if (playlist_id(url).empty()) { message = "That isn't a playlist link (as adds a single song)"; return; }
            if (add_listing) {
                downloader.lookup(url, true);
                message = "Reading the playlist…";
                return;
            }
            downloader.start(url, incoming_dir(), "best", true);
            message = "Downloading playlist…";
            return;
        }
        // A song or podcast is one video: a link to just a playlist has none.
        if (!playlist_id(url).empty() && !names_video(url)) {
            message = "That's a playlist link: aa adds it as an album";
            return;
        }
        if (add_listing) {
            downloader.lookup(url);
            message = "Reading the link…";
            return;
        }
        if (add_kind == AddKind::Podcast) {
            downloader.start(url, incoming_dir(), kPodcastFormat, false, kPodcastQuality);
            message = "Downloading podcast…";
        } else {
            downloader.start(url, incoming_dir());
            message = "Downloading…";
        }
    }

    // Called on every event: opens the form when a download has finished.
    void check_download() {
        if (mode != Mode::Normal) return;  // wait until the user isn't typing
        auto result = downloader.take_result();
        if (result && !restoring.empty()) {
            finish_restore(*result);
            result.reset();
        }
        pump_restores();
        if (!result) return;
        if (!result->ok) {
            message = (result->info_only ? "Couldn't read the link: " : "Download failed: ") + result->error;
            return;
        }
        if (result->playlist) return begin_album(*result);

        // Pre-fill from the tags yt-dlp wrote, e.g. title "Tool - Schism (Official Video)".
        // A listing has no file: the same details come straight from yt-dlp.
        Track t;
        if (result->info_only) {
            t.title = result->title;
            t.artist = result->artist.empty() ? "Unknown Artist" : result->artist;
            t.album = result->album;
            t.seconds = result->seconds;
        } else {
            t = read_track(result->file);
        }
        bool podcast = add_kind == AddKind::Podcast;
        std::string channel = t.artist != "Unknown Artist" ? clean_channel(t.artist) : "";
        // A podcast's title isn't "Artist - Song" ("AMA - Nov 2026"), so don't
        // split it; its artist is the channel.
        TitleGuess guess = podcast ? TitleGuess{clean_title(t.title), channel} : guess_from_title(t.title);
        std::string artist = guess.artist.empty() ? channel : guess.artist;

        form = {};
        form.file = result->file;
        form.is_new = true;
        form.listing = result->info_only;
        form.seconds = t.seconds;
        form.source = result->source.empty() ? last_url : result->source;
        form.podcast = podcast;
        form.fields[TagForm::Title] = guess.title;
        form.fields[TagForm::Artist] = match_known(TagForm::Artist, artist);
        form.fields[TagForm::Album] = match_known(TagForm::Album, t.album);
        form.field = TagForm::Title;
        mode = Mode::Tag;
        message.clear();
    }

    // A playlist finished downloading: ask for its artist and album once.
    void begin_album(const DownloadResult& result) {
        form = {};
        form.is_new = true;
        form.source = last_url;
        form.batch = result.items;
        form.failed = result.failed;
        form.listing = result.info_only;

        // The artist: what most video titles start with ("Tool - Schism"),
        // else the playlist's channel ("Tool - Topic"), else the first song's tag.
        std::unordered_map<std::string, int> votes;
        std::string artist, first_tag;
        int best = 0;
        for (const PlaylistItem& item : form.batch) {
            Track t;
            if (form.listing) t.title = item.title;  // no file to read; the channel comes from `uploader`
            else t = read_track(item.file);
            form.video_titles.push_back(t.title);
            if (first_tag.empty() && t.artist != "Unknown Artist") first_tag = clean_channel(t.artist);
            std::string guessed = guess_from_title(t.title).artist;
            if (guessed.empty()) continue;
            int v = ++votes[lower(guessed)];
            if (v > best) { best = v; artist = guessed; }
        }
        if (best * 2 <= static_cast<int>(form.batch.size())) artist = clean_channel(result.uploader);
        if (artist.empty()) artist = first_tag;

        // YouTube Music names album playlists "Album - <name>".
        std::string album = result.playlist_title;
        if (album.starts_with("Album - ")) album.erase(0, 8);

        form.fields[TagForm::Title] = plural(form.batch.size(), "song");
        form.fields[TagForm::Artist] = match_known(TagForm::Artist, artist);
        form.fields[TagForm::Album] = match_known(TagForm::Album, trim(album));
        form.field = TagForm::Artist;
        mode = Mode::Tag;
        message.clear();
    }

    // A playlist song's title: its video title, minus the artist in front
    // when that's the album's artist ("Tool - Schism" -> "Schism"). Other
    // " - " parts are kept, so "Schism - Live" stays whole.
    static std::string album_song_title(const std::string& video_title, const std::string& artist) {
        TitleGuess g = guess_from_title(video_title);
        if (!g.artist.empty() && lower(g.artist) == lower(artist) && !g.title.empty()) return g.title;
        std::string t = clean_title(video_title);
        return t.empty() ? video_title : t;
    }

    // `e`: edit the selected song's tags.
    void begin_edit() {
        if (pane != Pane::Songs) return begin_rename();
        if (songs.empty()) return;
        const Track& t = tracks[songs[song_cur.sel]];
        form = {};
        form.file = t.path;
        form.fields[TagForm::Title] = t.title;
        form.fields[TagForm::Artist] = t.artist == "Unknown Artist" ? "" : t.artist;
        form.fields[TagForm::Album] = t.album;
        form.podcast = t.podcast;
        mode = Mode::Tag;
    }

    // `e` in the artists / albums pane: type a new name for the selected
    // artist, album or playlist on the status line.
    void begin_rename() {
        if (pane == Pane::Artists) {
            if (artist_cur.sel == kAllArtists || artist_cur.sel == kPlaylists) {
                message = "Select an artist to rename";
                return;
            }
            input = artists[artist_cur.sel];
        } else {
            if (groups.empty()) return;
            const Group& g = groups[group_cur.sel];
            if (g.kind == Group::AllAlbums) { message = "Select an album to rename"; return; }
            input = g.kind == Group::List ? playlists[g.playlist].name : g.album;
        }
        mode = Mode::Rename;
    }

    // What the rename prompt is renaming, for the status line.
    std::string rename_subject() const {
        if (pane == Pane::Artists) return "artist";
        return !groups.empty() && groups[group_cur.sel].kind == Group::List ? "playlist" : "album";
    }

    // Enter in the rename prompt: retag every song of the artist / album.
    // Files stay where they are (as with editing a song), so playlists
    // that point at them keep working.
    void apply_rename(std::string name) {
        name = trim(name);
        bool renaming_artist = pane == Pane::Artists;
        Group g = renaming_artist || groups.empty() ? Group{} : groups[group_cur.sel];
        if (g.kind == Group::List) return rename_playlist(name);
        if (renaming_artist && name.empty()) { message = "The artist name can't be empty"; return; }

        std::string old_artist = renaming_artist ? artists[artist_cur.sel] : g.artist;
        if (renaming_artist) name = match_known(TagForm::Artist, name);  // join "tool" onto "Tool"
        auto selected = [&](const Track& t) {
            if (lower(t.artist) != lower(old_artist)) return false;
            return renaming_artist || lower(t.album) == lower(g.album);
        };

        int changed = 0, failed = 0;
        fs::path first;
        for (Track& t : tracks) {
            if (!selected(t)) continue;
            if (renaming_artist) t.artist = name;
            else t.album = name;
            std::string artist = t.artist == "Unknown Artist" ? "" : t.artist;
            // A song not downloaded here only lives in library.tsv, saved below.
            if (!t.missing && !write_tags(t.path, t.title, artist, t.album, t.number)) { ++failed; continue; }
            if (first.empty()) first = t.path;
            ++changed;
        }
        save_manifest();
        load();
        if (!first.empty()) {
            select_path(first);
            set_pane(renaming_artist ? static_cast<int>(Pane::Artists) : static_cast<int>(Pane::Albums));
        }
        message = "Renamed " + plural(changed, "song") + " to " + (name.empty() ? "no album" : name);
        if (failed) message += " · " + std::to_string(failed) + " couldn't be retagged";
    }

    // Every distinct artist in the library, or every album by the form's artist.
    std::vector<std::string> known_values(int field) const {
        std::vector<std::string> out;
        std::string artist = lower(form.fields[TagForm::Artist]);
        for (const Track& t : tracks) {
            std::string v;
            if (field == TagForm::Artist && t.artist != "Unknown Artist") v = t.artist;
            if (field == TagForm::Album && lower(t.artist) == artist) v = t.album;
            if (!v.empty() && index_of_name(out, v) < 0) out.push_back(v);
        }
        return out;
    }

    // If `value` matches a known artist/album apart from case, use the
    // library's spelling, so "tool" doesn't become a second artist next to "Tool".
    std::string match_known(int field, const std::string& value) const {
        std::string v = trim(value);
        for (const std::string& known : known_values(field)) {
            if (lower(known) == lower(v)) return known;
        }
        return v;
    }

    std::vector<std::string> suggestions() const {
        if (form.field == TagForm::Title || form.field == TagForm::Podcast) return {};
        return complete(known_values(form.field), form.fields[form.field]);
    }

    // What the current field shows: the highlighted suggestion, or the typed text.
    std::string form_value() const {
        if (form.pick >= 0) {
            auto s = suggestions();
            if (form.pick < static_cast<int>(s.size())) return s[form.pick];
        }
        return form.fields[form.field];
    }

    // Lock in the current field's value before moving to another field.
    void commit_field() {
        if (form.field == TagForm::Podcast) return;
        form.fields[form.field] = form.field == TagForm::Title ? trim(form_value())
                                                               : match_known(form.field, form_value());
        form.pick = -1;
    }

    // Move a file, falling back to copy + delete across drives.
    static bool move_file(const fs::path& from, const fs::path& to) {
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        fs::rename(from, to, ec);
        if (!ec) return true;
        ec.clear();
        fs::copy_file(from, to, ec);
        if (ec) return false;
        fs::remove(from, ec);
        return true;
    }

    // folder/name.ext, or folder/name (2).ext if that's taken, and so on.
    static fs::path free_path(const fs::path& folder, const std::string& name, const std::string& ext) {
        fs::path p = folder / (name + ext);
        for (int i = 2; fs::exists(p); ++i) p = folder / (name + " (" + std::to_string(i) + ")" + ext);
        return p;
    }

    void save_form() {
        commit_field();
        if (!form.batch.empty()) return save_album();
        const std::string& title = form.fields[TagForm::Title];
        const std::string& artist = form.fields[TagForm::Artist];
        const std::string& album = form.fields[TagForm::Album];
        if (title.empty()) {
            form.field = TagForm::Title;
            message = "The title can't be empty";
            return;
        }
        mode = Mode::Normal;
        if (form.listing) {
            Track t;
            t.path = listing_path(folder_for(artist, album), title, {});
            t.title = title;
            t.artist = artist.empty() ? "Unknown Artist" : artist;
            t.album = album;
            t.seconds = form.seconds;
            t.source = form.source;
            t.podcast = form.podcast;
            t.missing = true;
            fs::path where = t.path;
            tracks.push_back(std::move(t));
            save_manifest();
            load();
            select_path(where);
            message = "Listed (not downloaded): " + title + (artist.empty() ? "" : " — " + artist) +
                      " · Enter or :sync downloads it";
            return;
        }
        if (!write_tags(form.file, title, artist, album)) {
            message = "Couldn't write tags to " + form.file.filename().string();
            return;
        }

        fs::path final_path = form.file;
        if (!form.is_new) {  // the Podcast toggle lives in library.tsv, not the file's tags
            if (auto it = track_index.find(form.file.string()); it != track_index.end()) {
                tracks[it->second].podcast = form.podcast;
                save_manifest();
            }
        }
        if (form.is_new) {
            // File it as ~/Music/Artist/Album/Title.ext
            fs::path folder = dir / safe_filename(artist.empty() ? "Unknown Artist" : artist);
            if (!album.empty()) folder /= safe_filename(album);
            final_path = free_path(folder, safe_filename(title), form.file.extension().string());
            if (!move_file(form.file, final_path)) {
                message = "Tagged, but couldn't move it into " + folder.string();
                return;
            }
            pending_sources[rel(final_path)] = form.source;  // recorded in library.tsv by load()
            if (form.podcast) pending_podcasts.insert(rel(final_path));
        }
        load();
        select_path(final_path);
        message = "Saved: " + title + (artist.empty() ? "" : " — " + artist);
    }

    // ~/Music/Artist/Album, where a new song is filed.
    fs::path folder_for(const std::string& artist, const std::string& album) const {
        fs::path folder = dir / safe_filename(artist.empty() ? "Unknown Artist" : artist);
        if (!album.empty()) folder /= safe_filename(album);
        return folder;
    }

    // Where a listed (not downloaded) song would go once downloaded: like
    // free_path, but also skipping paths library.tsv already lists, and
    // those in `taken` (others of the same batch). Opus is what YouTube
    // usually has, and a different format just changes the extension later.
    fs::path listing_path(const fs::path& folder, const std::string& title, const std::set<fs::path>& taken) const {
        std::string name = safe_filename(title), ext = ".opus";
        auto used = [&](const fs::path& p) { return fs::exists(p) || track_index.count(p.string()) || taken.count(p); };
        fs::path p = folder / (name + ext);
        for (int i = 2; used(p); ++i) p = folder / (name + " (" + std::to_string(i) + ")" + ext);
        return p;
    }

    // aA: list every song of the playlist as not downloaded (⇣).
    void save_album_listing() {
        const std::string& artist = form.fields[TagForm::Artist];
        const std::string& album = form.fields[TagForm::Album];
        fs::path folder = folder_for(artist, album);
        std::set<fs::path> taken;
        for (size_t i = 0; i < form.batch.size(); ++i) {
            const PlaylistItem& item = form.batch[i];
            Track t;
            std::string title = album_song_title(form.video_titles[i], artist);
            t.path = listing_path(folder, title, taken);
            taken.insert(t.path);
            t.title = title;
            t.artist = artist.empty() ? "Unknown Artist" : artist;
            t.album = album;
            t.number = item.index;
            t.seconds = item.seconds;
            t.source = item.source;
            t.missing = true;
            tracks.push_back(std::move(t));
        }
        save_manifest();
        load();
        // Select the album's first song (tracks are sorted by track number).
        for (const Track& t : tracks) {
            if (taken.count(t.path)) { select_path(t.path); break; }
        }
        message = "Listed " + plural(form.batch.size(), "song") + " in " + (album.empty() ? "no album" : album) +
                  (artist.empty() ? "" : " — " + artist) + " (not downloaded) · :sync album downloads them";
        if (form.failed) message += " · " + std::to_string(form.failed) + " unavailable on YouTube";
    }

    void save_album() {
        const std::string& artist = form.fields[TagForm::Artist];
        const std::string& album = form.fields[TagForm::Album];
        mode = Mode::Normal;
        if (form.listing) return save_album_listing();
        fs::path folder = folder_for(artist, album);

        int saved = 0, problems = 0;
        fs::path first;
        for (size_t i = 0; i < form.batch.size(); ++i) {
            const PlaylistItem& item = form.batch[i];
            std::string title = album_song_title(form.video_titles[i], artist);
            fs::path to = free_path(folder, safe_filename(title), item.file.extension().string());
            if (!write_tags(item.file, title, artist, album, item.index) || !move_file(item.file, to)) {
                // Keep it where cancel would have put it rather than losing it.
                fs::path unsorted = free_path(dir / "Unsorted", item.file.stem().string(), item.file.extension().string());
                if (move_file(item.file, unsorted)) pending_sources[rel(unsorted)] = item.source;
                ++problems;
                continue;
            }
            pending_sources[rel(to)] = item.source;  // recorded in library.tsv by load()
            if (first.empty()) first = to;
            ++saved;
        }
        load();
        if (!first.empty()) select_path(first);
        message = "Saved " + plural(saved, "song") + " to " + (album.empty() ? "no album" : album) +
                  (artist.empty() ? "" : " — " + artist);
        if (problems) message += " · " + std::to_string(problems) + " couldn't be tagged (left in Unsorted/)";
        if (form.failed) message += " · " + std::to_string(form.failed) + " unavailable on YouTube";
    }

    void cancel_form() {
        mode = Mode::Normal;
        if (!form.is_new) {
            message = "Edit cancelled";
            return;
        }
        if (form.listing) {  // nothing was downloaded, so nothing to keep
            message = "Not added";
            return;
        }
        if (!form.batch.empty()) {  // keep the whole playlist, untagged, in Unsorted/
            fs::path first;
            for (const PlaylistItem& item : form.batch) {
                fs::path to = free_path(dir / "Unsorted", item.file.stem().string(), item.file.extension().string());
                if (!move_file(item.file, to)) continue;
                pending_sources[rel(to)] = item.source;
                if (first.empty()) first = to;
            }
            load();
            if (!first.empty()) select_path(first);
            message = "Kept " + plural(form.batch.size(), "song") + " untagged in Unsorted/ (press e to tag them later)";
            return;
        }
        // Don't throw a download away: keep it, untagged, in ~/Music/Unsorted.
        fs::path to = free_path(dir / "Unsorted", form.file.stem().string(), form.file.extension().string());
        if (move_file(form.file, to)) {
            pending_sources[rel(to)] = form.source;
            load();
            select_path(to);
            message = "Kept it untagged in Unsorted/ (press e to tag it later)";
        }
    }

    bool on_form_key(const Event& e) {
        int n_sugg = static_cast<int>(suggestions().size());
        int first = form.batch.empty() ? TagForm::Title : TagForm::Artist;  // a playlist has no Title to ask
        int last = form.batch.empty() ? TagForm::Podcast : TagForm::Album;
        auto go_to_field = [&](int f) {
            commit_field();
            form.field = std::clamp(f, first, last);
        };

        if (e == Event::Escape) cancel_form();
        else if (e == Event::Return) {
            // Enter on Album saves too: the Podcast toggle is a Tab away when wanted.
            if (form.field >= TagForm::Album) save_form();
            else go_to_field(form.field + 1);
        }
        else if (e == Event::Tab) go_to_field(form.field == last ? first : form.field + 1);
        else if (e == Event::TabReverse) go_to_field(form.field == first ? last : form.field - 1);
        else if (form.field == TagForm::Podcast) {  // a yes/no toggle, not text
            std::string c = e.is_character() ? e.character() : "";
            if (c == " ") form.podcast = !form.podcast;
            else if (c == "y") form.podcast = true;
            else if (c == "n") form.podcast = false;
        }
        // Ctrl-n / Ctrl-p walk the suggestions like vim's completion menu;
        // stepping past either end goes back to what you typed.
        else if (e == Event::CtrlN || e == Event::ArrowDown) {
            if (n_sugg) form.pick = form.pick + 1 < n_sugg ? form.pick + 1 : -1;
        }
        else if (e == Event::CtrlP || e == Event::ArrowUp) {
            if (n_sugg) form.pick = form.pick < 0 ? n_sugg - 1 : form.pick - 1;
        }
        else if (e == Event::Backspace) {
            std::string v = form_value();
            pop_char(v);
            form.fields[form.field] = v;
            form.pick = -1;
        }
        else if (e == Event::CtrlU) {  // clear the field, as in a shell
            form.fields[form.field].clear();
            form.pick = -1;
        }
        else if (e.is_character()) {
            form.fields[form.field] = form_value() + e.character();
            form.pick = -1;
        }
        return true;
    }

    // =================================================================
    // Keys
    // =================================================================
    Cursor& cur() {
        if (pane == Pane::Artists) return artist_cur;
        if (pane == Pane::Albums) return group_cur;
        return song_cur;
    }

    int list_size() const {
        if (pane == Pane::Artists) return static_cast<int>(artists.size());
        if (pane == Pane::Albums) return static_cast<int>(groups.size());
        return static_cast<int>(songs.size());
    }

    // Move the cursor in the current pane; panes to the right follow along.
    void move_to(int index) {
        cur().sel = std::clamp(index, 0, std::max(0, list_size() - 1));
        if (pane == Pane::Artists) {
            group_cur = {};
            build_groups();
        }
        if (pane != Pane::Songs) {
            song_cur = {};
            refresh_songs();
        }
    }

    void set_pane(int p) {
        pane = static_cast<Pane>(std::clamp(p, 0, 2));
        visual = -1;
    }

    void run_command(const std::string& line) {
        std::string cmd = trim(line);
        auto space = cmd.find(' ');
        std::string verb = cmd.substr(0, space);
        std::string arg = space == std::string::npos ? "" : trim(cmd.substr(space + 1));

        if (verb == "q" || verb == "quit") screen.Exit();
        else if (verb == "rescan") {
            load();
            message = "Found " + plural(tracks.size(), "song");
        }
        else if (verb == "shuffle") toggle_shuffle();
        else if (verb == "speed") {
            double s = std::atof(arg.c_str());
            if (s > 0) set_long_speed(s);
            else message = "Usage: :speed 1.5  (0.5 to 3)";
        }
        else if (verb == "help") show_help = true;
        else if (verb == "vis" || verb == "big" || verb == "now") {
            if (arg != "" && arg != "on" && arg != "off") message = "Usage: :" + verb + " [on|off]";
            else if (verb == "vis") set_visualizer(arg);
            else if (verb == "big") set_big(arg);
            else set_now_playing(arg);
        }
        else if (verb == "new") new_playlist(arg);
        else if (verb == "rename") rename_playlist(arg);
        else if (verb == "delete") delete_playlist();
        else if (verb == "sync") sync(arg);
        else if (verb == "podcast") mark_podcast(arg);
        else if (verb == "shrink") shrink();
        else if (verb == "order") order_album_tracks();
        else if (verb == "source") set_source(arg);
        else if (verb == "forget") forget_song();
        else if (!verb.empty() && std::all_of(verb.begin(), verb.end(), ::isdigit)) {
            move_to(std::stoi(verb) - 1);  // :42 goes to line 42, like vim
        }
        else if (!verb.empty()) message = "Unknown command: " + verb;
    }

    // Name of row i in the artists or albums pane, for / search.
    std::string row_name(int i) const {
        return pane == Pane::Artists ? artists[i] : groups[i].label;
    }

    // In the artists / albums pane, / jumps to a matching row rather than
    // filtering. Look for `needle` starting at row `from`, stepping by `dir`
    // and wrapping round; returns -1 if nothing matches.
    int find_row(const std::string& needle, int from, int dir) const {
        int size = list_size();
        if (needle.empty() || size == 0) return -1;
        std::string want = lower(needle);
        for (int k = 0; k < size; ++k) {
            int i = ((from + dir * k) % size + size) % size;
            if (lower(row_name(i)).find(want) != std::string::npos) return i;
        }
        return -1;
    }

    // n / N: the next / previous match of the last search in this pane.
    void jump_again(int dir) {
        if (pane == Pane::Songs) { message = "n / N work in the artists and albums panes"; return; }
        if (jump.empty()) { message = "No search yet (press / to search)"; return; }
        int i = find_row(jump, cur().sel + dir, dir);
        if (i < 0) message = "Not found: " + jump;
        else move_to(i);
    }

    // Typing after /, : or the URL prompt
    bool on_input_key(const Event& e) {
        bool jumping = mode == Mode::Search && pane != Pane::Songs;
        if (e == Event::Escape) {
            if (jumping) move_to(jump_origin);  // back to where the search began
            else if (mode == Mode::Search) { filter.clear(); refresh_songs(); }
            mode = Mode::Normal;
        } else if (e == Event::Return) {
            Mode was = mode;
            mode = Mode::Normal;
            if (jumping) {
                jump = input;
                if (!input.empty() && find_row(input, jump_origin, 1) < 0) message = "Not found: " + input;
            }
            if (was == Mode::Command) run_command(input);
            if (was == Mode::Rename) apply_rename(input);
            if (was == Mode::Url && !trim(input).empty()) start_add(trim(input));
        } else if (e == Event::CtrlU) {
            input.clear();
        } else if (e == Event::Backspace) {
            if (input.empty()) { mode = Mode::Normal; return true; }
            pop_char(input);
        } else if (e.is_character()) {
            input += e.character();
        } else {
            return true;
        }
        if (jumping) {  // jump to the first match as you type
            int i = find_row(input, jump_origin, 1);
            move_to(i >= 0 ? i : jump_origin);
        } else if (mode == Mode::Search) {  // search filters live as you type
            filter = input;
            set_pane(static_cast<int>(Pane::Songs));
            refresh_songs();
        }
        return true;
    }

    bool on_event(const Event& e) {
        bool handled = handle_event(e);
        check_download();  // after the key too, in case it just closed a prompt
        check_shrink();
        check_order();
        return handled;
    }

    // Touch / mouse. Taps are hard to aim on a phone, so they act on the
    // selected row wherever they land. Swipe to move the selection, then:
    //   tap:        play the selected song (or pause it if it's the one
    //               playing), or open the selected artist / album;
    //               on the now-playing box, pause
    //   double tap: go back one pane
    // A tap waits a moment before acting, in case it becomes a double tap.
    static constexpr auto kTapGap = std::chrono::milliseconds(350);

    int pane_at(int x, int y) const {
        for (int p = 0; p < 3; ++p)
            if (visible(static_cast<Pane>(p)) && pane_box[p].Contain(x, y)) return p;
        return -1;
    }

    void on_tap(int x, int y) {
        if (show_help) {
            show_help = false;
            return;
        }
        if (mode != Mode::Normal) return;
        message.clear();
        if (now_box.Contain(x, y)) {
            player.toggle_pause();
            return;
        }
        int p = pane_at(x, y);
        if (p < 0) return;
        set_pane(p);
        if (pane != Pane::Songs) set_pane(p + 1);  // open the selected artist / album
        else if (!songs.empty() && songs[song_cur.sel] == playing) player.toggle_pause();
        else if (!songs.empty()) play_from_list(song_cur.sel);
    }

    bool on_mouse(Event e) {
        Mouse& m = e.mouse();
        bool tap = m.button == Mouse::Left && m.motion == Mouse::Pressed;
        bool wheel = m.button == Mouse::WheelUp || m.button == Mouse::WheelDown;
        if (!tap && !wheel) return false;  // ignore moves and releases
        if (mode != Mode::Normal) return false;

        if (wheel) {
            int p = pane_at(m.x, m.y);
            if (show_help || p < 0) return false;
            set_pane(p);
            move_to(cur().sel + (m.button == Mouse::WheelDown ? 1 : -1));
            return true;
        }

        // Taps count as one gesture when they come quickly, a few rows
        // apart at most (fingers wander).
        auto now = std::chrono::steady_clock::now();
        bool follows = now - last_tap < kTapGap && std::abs(m.y - last_tap_y) <= 3;
        taps = follows ? taps + 1 : 1;
        last_tap = now;
        last_tap_y = m.y;

        if (taps == 1) {
            // Act after a pause, unless a second tap cancels it first.
            int id = ++tap_id, x = m.x, y = m.y;
            tap_timer = std::jthread([this, id, x, y](std::stop_token stop) {
                std::mutex mu;
                std::condition_variable_any cv;
                std::unique_lock lock(mu);
                cv.wait_for(lock, stop, kTapGap, [] { return false; });
                if (stop.stop_requested()) return;
                screen.Post([this, id, x, y] {
                    if (id == tap_id) on_tap(x, y);
                });
                screen.PostEvent(Event::Custom);  // redraw
            });
            return false;
        }
        // Double tap: cancel the pending tap and go back a pane.
        ++tap_id;
        tap_timer.request_stop();
        taps = 0;
        if (show_help) show_help = false;
        else set_pane(static_cast<int>(pane) - 1);
        return true;
    }

    bool handle_event(const Event& e) {
        if (player.take_finished()) finished();
        note_progress();
        // Save when you pause, so closing the app (or the phone killing it) loses nothing.
        bool paused_now = player.paused();
        save_progress(paused_now != was_paused);
        was_paused = paused_now;
        check_download();
        if (e == Event::Custom) return true;  // just a redraw request from a background thread
        if (e.is_mouse()) return on_mouse(e);

        if (mode == Mode::Tag) return on_form_key(e);
        if (mode == Mode::Pick) return on_pick_key(e);
        if (mode == Mode::Confirm) {
            mode = Mode::Normal;
            auto yes = std::move(confirm_action);
            auto all = std::move(confirm_all);
            bool enter = confirm_enter;
            confirm_action = nullptr;
            confirm_all = nullptr;
            confirm_enter = false;
            if (e == Event::Character('y') || (enter && e == Event::Return)) yes();
            else if (all && e == Event::Character('a')) all();
            else message = "Cancelled";
            return true;
        }
        if (mode != Mode::Normal) return on_input_key(e);
        message.clear();

        if (show_help) {  // any key closes help
            show_help = false;
            return true;
        }

        std::string c = e.is_character() ? e.character() : "";

        // Count prefix: digits before a command, as in 5j, 10G or 3yy.
        if (c.size() == 1 && std::isdigit(static_cast<unsigned char>(c[0])) && (c != "0" || count > 0)) {
            count = count * 10 + (c[0] - '0');
            return true;
        }
        bool has_count = count > 0;
        int n = has_count ? count : 1;
        count = 0;

        // Second key of gg / yy / dd.
        if (pending) {
            char first = pending;
            int first_count = pending_count;
            pending = 0;
            if (first == 'g' && c == "g") move_to(first_count ? first_count - 1 : 0);
            if (first == 'y' && c == "y") yank(std::max(1, first_count));
            if (first == 'd' && c == "d") {
                if (pane == Pane::Songs) delete_rows(std::max(1, first_count));
                else delete_group();
            }
            if (first == 'd' && c == "r") delete_album_here();
            if (first == 'a') {
                if (c == "s") begin_add(AddKind::Song);
                else if (c == "a") begin_add(AddKind::Album);
                else if (c == "p") begin_add(AddKind::Podcast);
                else if (c == "S") begin_add(AddKind::Song, true);
                else if (c == "A") begin_add(AddKind::Album, true);
                else if (c == "P") begin_add(AddKind::Podcast, true);
                else message = "Add: as = song · aa = album · ap = podcast · capital (aS aA aP) = list without downloading";
            }
            if (first == 'd' && c == "a") delete_artist_here();
            return true;
        }
        auto start_pending = [&](char key) {
            pending = key;
            pending_count = has_count ? n : 0;
        };

        bool in_songs = pane == Pane::Songs;
        auto songs_only = [&](auto action) {
            if (in_songs) action();
            else message = "That works in the songs pane (press l to go there)";
        };

        // --- movement ---
        if (c == "j" || e == Event::ArrowDown) move_to(cur().sel + n);
        else if (c == "k" || e == Event::ArrowUp) move_to(cur().sel - n);
        else if (c == "g") start_pending('g');
        else if (c == "G") move_to(has_count ? n - 1 : list_size() - 1);
        else if (e == Event::CtrlD) move_to(cur().sel + rows / 2);
        else if (e == Event::CtrlU) move_to(cur().sel - rows / 2);
        else if (e == Event::CtrlF) move_to(cur().sel + rows);
        else if (e == Event::CtrlB) move_to(cur().sel - rows);
        else if (c == "h") set_pane(static_cast<int>(pane) - 1);
        else if (c == "l") set_pane(static_cast<int>(pane) + 1);
        else if (e == Event::Tab) set_pane((static_cast<int>(pane) + 1) % 3);
        else if (e == Event::TabReverse) set_pane((static_cast<int>(pane) + 2) % 3);
        else if (c == "o") jump_to_playing();

        // --- playback ---
        else if (e == Event::Return) {
            if (pane == Pane::Songs) play_from_list(song_cur.sel);
            else set_pane(static_cast<int>(pane) + 1);
        }
        else if (c == " ") player.toggle_pause();
        else if (c == "]") next();
        else if (c == "[") prev();
        else if (e == Event::ArrowRight) player.seek(5 * n);
        else if (e == Event::ArrowLeft) player.seek(-5 * n);
        else if (c == "}") player.seek(30 * n);
        else if (c == "{") player.seek(-30 * n);
        else if (c == "+" || c == "=") player.change_volume(5 * n);
        else if (c == "-") player.change_volume(-5 * n);
        else if (c == "s") toggle_shuffle();
        else if (c == "v") set_visualizer("");
        else if (c == "i") set_now_playing("");
        else if (c == "b") set_big("");
        else if (c == ">") set_long_speed(current_speed() + 0.1 * n);
        else if (c == "<") set_long_speed(current_speed() - 0.1 * n);

        // --- playlists (vim-style: yank, delete, put) ---
        else if (c == "V") songs_only([&] { visual = (visual >= 0 || songs.empty()) ? -1 : song_cur.sel; });
        else if (c == "y") songs_only([&] { visual >= 0 ? yank(1) : start_pending('y'); });
        else if (c == "d") { visual >= 0 ? delete_rows(1) : start_pending('d'); }
        else if (c == "x") songs_only([&] { cut_rows(1); });
        else if (c == "p") songs_only([&] { put(false); });
        else if (c == "P") songs_only([&] { put(true); });
        else if (c == "A") songs_only([&] { begin_pick(n); });
        else if (c == "J") songs_only([&] { move_song(+1); });
        else if (c == "K") songs_only([&] { move_song(-1); });

        // --- library ---
        else if (c == "a") start_pending('a');
        else if (c == "e") begin_edit();

        // --- modes ---
        else if (c == "/") {
            mode = Mode::Search;
            if (in_songs) input = filter;
            else { input.clear(); jump_origin = cur().sel; }
        }
        else if (c == "n") jump_again(+1);
        else if (c == "N") jump_again(-1);
        else if (c == ":") { mode = Mode::Command; input.clear(); }
        else if (e == Event::Escape) {
            if (visual >= 0) visual = -1;
            else { filter.clear(); refresh_songs(); }
        }
        else if (c == "?") show_help = true;
        else if (c == "q") screen.Exit();
        else return false;

        return true;
    }

    // =================================================================
    // Drawing
    // =================================================================
    Element highlight(Element row, bool selected, bool focused) const {
        if (!selected) return row;
        return focused ? row | inverted : row | bgcolor(Color::GrayDark);
    }

    // A bordered, scrolling list. `row(i)` draws row i.
    Element draw_list(const std::string& title, Pane which, Cursor& c, int size, Element header,
                      const std::function<Element(int)>& row, std::function<bool(int)> marked = {}) {
        c.fit(size, rows);
        bool active = pane == which;
        Elements lines;
        lines.push_back(header | dim);
        for (int i = c.top; i < std::min(size, c.top + rows); ++i) {
            bool selected = i == c.sel || (marked && marked(i));
            lines.push_back(highlight(row(i), selected, active));
        }
        return window(text(title) | (active ? color(Color::Green) : dim) | bold, vbox(std::move(lines)) | flex);
    }

    // The playing song's row, and its artist's and album's, are green.
    // Green text clashes with the unfocused highlight's background, so on
    // that row it's bold instead.
    Element mark_playing(Element row, Pane which, int i, int sel) const {
        bool on_dim_highlight = i == sel && pane != which;
        return on_dim_highlight ? row | bold : row | color(Color::Green);
    }

    Element draw_artists() {
        int n = static_cast<int>(artists.size()) - 2;
        std::string playing_artist = playing >= 0 ? lower(tracks[playing].artist) : "";
        return draw_list(" Artists ", Pane::Artists, artist_cur, artists.size(),
                         text(" " + plural(n, "artist")), [&](int i) {
            if (i == kPlaylists) return text(" " + artists[i] + " ") | color(Color::Cyan);
            Element row = text(" " + artists[i] + " ");
            if (i == kAllArtists || playing < 0 || lower(artists[i]) != playing_artist) return row;
            return mark_playing(row, Pane::Artists, i, artist_cur.sel);
        }) | (stretches(Pane::Artists) ? flex : size(WIDTH, EQUAL, kArtistsWidth));
    }

    Element draw_groups() {
        bool lists = artist_cur.sel == kPlaylists;
        int n = lists ? static_cast<int>(groups.size()) : static_cast<int>(groups.size()) - 1;
        Element header = text(" " + (lists ? plural(n, "playlist") : plural(n, "album")));
        Element body = draw_list(lists ? " Playlists " : " Albums ", Pane::Albums, group_cur, groups.size(),
                                 header, [&](int i) {
            const Group& g = groups[i];
            if (g.kind == Group::List) {
                return hbox({text(" ♫ " + g.label) | color(Color::Cyan) | flex,
                             text(" " + std::to_string(playlists[g.playlist].songs.size()) + " ") | dim});
            }
            Element row = g.kind == Group::Album && artist_cur.sel == kAllArtists
                ? hbox({text(" " + g.label), text("  " + g.artist) | dim})
                : text(" " + g.label + " ");
            bool has_playing = g.kind == Group::Album && playing >= 0 &&
                               lower(tracks[playing].artist) == lower(g.artist) &&
                               lower(tracks[playing].album) == lower(g.album);
            return has_playing ? mark_playing(row, Pane::Albums, i, group_cur.sel) : row;
        });
        if (lists && groups.empty()) {
            body = window(text(" Playlists ") | bold | (pane == Pane::Albums ? color(Color::Green) : dim),
                          vbox({text(" No playlists yet.") | dim, text(""),
                                text(" Press A on a song to add") | dim, text(" it to a new playlist, or") | dim,
                                text(" type :new Name") | dim}) | flex);
        }
        return body | (stretches(Pane::Albums) ? flex : size(WIDTH, EQUAL, kAlbumsWidth));
    }

    Element draw_songs() {
        // Drop the album, then the artist column when there isn't room,
        // so the title always gets space.
        int inner = cols - 2 - (visible(Pane::Artists) ? kArtistsWidth : 0) - (visible(Pane::Albums) ? kAlbumsWidth : 0);
        bool show_album = inner >= 70, show_artist = inner >= 45;
        auto columns = [=](Element mark, Element title, Element artist, Element album, Element time) {
            Elements cells = {mark | size(WIDTH, EQUAL, 2), title | flex};
            if (show_artist) cells.insert(cells.end(), {text(" "), artist | size(WIDTH, EQUAL, 18)});
            if (show_album) cells.insert(cells.end(), {text(" "), album | size(WIDTH, EQUAL, 18)});
            cells.push_back(time | align_right | size(WIDTH, EQUAL, 9));  // fits "-12:34:56"
            return hbox(std::move(cells));
        };
        Element header = columns(text(""), text("Title"), text("Artist"), text("Album"), text("Time"));

        std::string title = " Songs ";
        if (!groups.empty()) {
            const Group& g = groups[group_cur.sel];
            if (g.kind == Group::List) title = " ♫ " + g.label + " ";
            else if (g.kind == Group::Album) title = " " + g.label + " ";
        }
        if (!filter.empty()) title += "/" + filter + " ";

        auto [lo, hi] = visual >= 0 ? row_range(1) : std::pair{-1, -1};
        return draw_list(title, Pane::Songs, song_cur, songs.size(), header, [&](int i) {
            const Track& t = tracks[songs[i]];
            bool is_playing = songs[i] == playing;
            std::string mark = is_playing ? "▶" : "";
            if (t.missing) {
                std::string r = rel(t.path);
                bool queued = r == restoring ||
                              std::find(restore_queue.begin(), restore_queue.end(), r) != restore_queue.end();
                mark = queued ? "…" : "⇣";
            }
            // Long tracks: ◐ part-way (showing time left), ✓ finished.
            std::string time = format_time(t.seconds);
            bool done = false;
            if (const Progress* p = progress_of(songs[i]); p && is_podcast(songs[i])) {
                done = p->done;
                if (mark.empty()) mark = p->done ? "✓" : p->seconds > 0 ? "◐" : "";
                if (!p->done && p->seconds > 0) time = "-" + format_time(t.seconds - p->seconds);
            }
            Element row = columns(text(mark), text(t.title), text(t.artist), text(t.album), text(time));
            if (t.missing || (done && !is_playing)) return row | dim;
            if (!is_playing) return row;
            return mark_playing(row, Pane::Songs, i, song_cur.sel);  // ▶ marks it too
        }, [&](int i) { return i >= lo && i <= hi; }) | flex;
    }

    // Compact now-playing box, `height` rows inside: title and artist at the
    // top, a short progress bar at the bottom. Volume and the like go in
    // the title of the box.
    Element draw_now_playing(int height) const {
        Elements lines;
        Element progress = text("");
        if (playing < 0) {
            lines = {text(" Nothing playing") | dim, text(" Enter plays the selected song") | dim};
        } else {
            const Track& t = tracks[playing];
            double pos = player.position(), len = player.duration();
            float frac = len > 0 ? static_cast<float>(pos / len) : 0.f;
            lines = {hbox({text(player.paused() ? " ⏸ " : " ▶ ") | color(Color::Green), text(t.title) | bold}),
                     text("   " + t.artist + (t.album.empty() ? "" : " · " + t.album)) | dim};
            progress = hbox({text(" " + (pos < 1 ? std::string("0:00") : format_time((int)pos)) + " "),
                             gauge(frac) | color(Color::Green) | size(WIDTH, LESS_THAN, 40) | flex,
                             text(" " + format_time((int)len) + " "), filler()});
        }
        while (static_cast<int>(lines.size()) < height - 1) lines.push_back(text(""));
        lines.push_back(progress);

        bool sped = playing >= 0 && speed_of(playing) != 1.0;
        std::string flags = " vol " + std::to_string(player.volume()) + "%" +
                            (sped ? " · " + format_speed(speed_of(playing)) : "") + (shuffle ? " · shuffle" : "") + " ";
        return window(hbox({text(" Now playing ") | dim, filler(), text(flags) | dim}), vbox(std::move(lines)));
    }

    // A spectrum panel `width` columns wide (border included) with `height`
    // rows of bars. Each cell is one of 8 block heights, so a bar moves in
    // steps of 1/8 of a row. `axis` adds frequency ticks along the bottom.
    Element draw_spectrum(SpectrumBars& sb, int width, int height, bool axis, const std::string& key) {
        int inner = std::max(1, width - 2);
        int bar_w = inner >= 100 ? 2 : 1;  // wide: 2-column bars
        int n = std::max(1, (inner + 1) / (bar_w + 1));
        sb.step(vis.levels(n));
        Element title = text(" Spectrum ") | dim;

        if (std::string err = vis.error(); !err.empty()) {
            Elements lines{paragraph(" " + err) | dim, text(" " + key + " hides this") | dim};
            return window(title, vbox(std::move(lines)) | size(HEIGHT, EQUAL, height + (axis ? 2 : 0)));
        }

        static const char* blocks[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
        int pad = (inner - (n * (bar_w + 1) - 1)) / 2;  // centre the bars
        Elements lines;
        for (int r = 0; r < height; ++r) {
            int from_bottom = height - 1 - r;
            std::string line(pad, ' ');
            for (int i = 0; i < n; ++i) {
                int fill = std::clamp(static_cast<int>(sb.bars[i] * height * 8) - from_bottom * 8, 0, 8);
                float peak = sb.peaks[i] * height - from_bottom;  // where the cap sits in this cell, 0..1
                const char* cell = blocks[fill];
                if (fill == 0 && peak > 0 && peak <= 1 && sb.peaks[i] > 0.02f) cell = peak < 0.5f ? "▁" : "▔";
                for (int k = 0; k < bar_w; ++k) line += cell;
                if (i + 1 < n) line += ' ';
            }
            // One colour: the app's accent, as on the active pane title and
            // the playing song. A named colour, so it follows the terminal theme.
            lines.push_back(text(line) | color(Color::Green));
        }

        if (axis) {
            // A ruler with a ┬ under each frequency, and its label below.
            // Labels that would overlap the previous one are left out.
            static const std::pair<float, const char*> ticks[] = {
                {50, "50"}, {100, "100"}, {200, "200"}, {500, "500"}, {1000, "1k"},
                {2000, "2k"}, {5000, "5k"}, {10000, "10k"}, {16000, "16k"}};
            std::vector<std::string> ruler(inner, "─");
            std::string labels(inner, ' ');
            int free_from = 0;  // first column a new label may use
            for (auto [hz, label] : ticks) {
                float x = std::log(hz / kSpectrumLowHz) / std::log(kSpectrumHighHz / kSpectrumLowHz);  // 0..1
                int col = std::clamp(pad + static_cast<int>(x * n * (bar_w + 1)), 0, inner - 1);
                int len = static_cast<int>(std::strlen(label));
                int start = std::clamp(col - len / 2, 0, inner - len);
                if (start < free_from) continue;
                ruler[col] = "┬";
                labels.replace(start, len, label);
                free_from = start + len + 1;
            }
            std::string ruler_line;
            for (const auto& cell : ruler) ruler_line += cell;
            lines.push_back(text(ruler_line) | dim);
            lines.push_back(text(labels) | dim);
        }
        return window(title, vbox(std::move(lines)));
    }

    Element draw_status() const {
        auto tag = [](const std::string& name, Color c) {
            return text(" " + name + " ") | bgcolor(c) | color(Color::Black);
        };
        Element mode_tag, rest;
        if (mode == Mode::Search) {
            mode_tag = tag("SEARCH", Color::Yellow);
            rest = text(" /" + input + "▏");
        } else if (mode == Mode::Command) {
            mode_tag = tag("COMMAND", Color::Magenta);
            rest = text(" :" + input + "▏");
        } else if (mode == Mode::Rename) {
            mode_tag = tag("RENAME", Color::Blue);
            rest = hbox({text(" New " + rename_subject() + " name: "), text(input + "▏"),
                         text("   Enter save · Esc cancel") | dim});
        } else if (mode == Mode::Url) {
            std::string what = add_kind == AddKind::Album ? "ALBUM" : add_kind == AddKind::Podcast ? "PODCAST" : "SONG";
            mode_tag = tag((add_listing ? "LIST " : "ADD ") + what, Color::Cyan);
            rest = hbox({text(" URL: "), text(input + "▏"),
                         text(add_listing ? "   Enter list it (no download) · Esc cancel" : "   Enter download · Esc cancel") | dim});
        } else if (mode == Mode::Tag || mode == Mode::Pick) {
            mode_tag = tag(mode == Mode::Tag ? "TAG" : "PLAYLIST", Color::Blue);
            rest = text(" " + message);
        } else if (mode == Mode::Confirm) {
            mode_tag = tag("CONFIRM", Color::Red);
            rest = text(" " + confirm_text) | bold;
        } else {
            mode_tag = visual >= 0 ? tag("VISUAL", Color::RedLight) : tag("NORMAL", Color::Green);
            std::string keys = (count > 0 ? std::to_string(count) : "") +
                               (pending ? std::string(1, pending) : "");
            rest = text(" " + (message.empty() ? keys : message));
        }
        std::string right = plural(songs.size(), "song") + "  " + human_size(library_bytes) + "  ? help ";
        std::string queued = restore_queue.empty() ? "" : " +" + std::to_string(restore_queue.size());
        std::string of;  // "3/12 " in a playlist
        if (downloader.item() > 0) {
            of = std::to_string(downloader.item()) +
                 (downloader.item_count() > 0 ? "/" + std::to_string(downloader.item_count()) : "") + " ";
        }
        Element download = downloader.busy()
            ? text(" ⇣ downloading " + of + std::to_string(downloader.percent()) + "%" + queued + " ") | color(Color::Cyan)
            : text("");
        Element shrink = shrinker.busy()
            ? text(" ⇲ shrinking " + std::to_string(shrinker.item()) + "/" + std::to_string(shrinker.count()) + " " +
                   std::to_string(shrinker.percent()) + "% ") | color(Color::Cyan)
            : text("");
        return hbox({mode_tag | bold, rest | flex, download, shrink, text(right) | dim});
    }

    // A "type or pick" box's suggestion lines, plus whether the value is new.
    static void add_suggestions(Elements& lines, const std::vector<std::string>& sugg, int picked,
                                const std::string& typed, const std::string& noun) {
        for (int j = 0; j < static_cast<int>(sugg.size()); ++j) {
            Element row = text((j == picked ? " ▸ " : "   ") + sugg[j]);
            lines.push_back(j == picked ? row | inverted : row);
        }
        std::string t = trim(typed);
        if (picked < 0 && !t.empty()) {
            bool exact = index_of_name(sugg, t) >= 0;
            lines.push_back(exact ? text(" ✓ already exists") | color(Color::Green)
                                  : text(" + new " + noun + " \"" + t + "\"") | color(Color::Yellow));
        }
    }

    Element draw_form() const {
        static const char* labels[] = {"Title", "Artist", "Album", "Podcast"};
        static const char* nouns[] = {"title", "artist", "album"};
        static const char* album_labels[] = {"Songs", "Artist", "Album"};
        bool batch = !form.batch.empty();
        Elements lines;
        if (batch) {
            std::string note = "Every song gets this artist and album, numbered in playlist order";
            if (form.failed) note += " · " + std::to_string(form.failed) + " unavailable, skipped";
            lines.push_back(text(note) | dim);
        } else {
            std::string note = form.file.filename().string();
            if (form.podcast) note = "Podcast · " + note;
            lines.push_back(text(note) | dim);
        }
        lines.push_back(text(""));

        for (int i = 0; i < (batch ? 3 : 4); ++i) {
            if (i == TagForm::Podcast) {
                bool active = form.field == i;
                Element label = text(std::string(active ? "› " : "  ") + labels[i]) | size(WIDTH, EQUAL, 10);
                Element value = text(form.podcast ? "yes" : "no");
                Element hint = text(active ? "   Space toggles · keeps your place, plays at its own speed" : "") | dim;
                lines.push_back(active ? hbox({label | color(Color::Green) | bold, value | bold, hint})
                                       : hbox({label | dim, value}));
                continue;
            }
            if (batch && i == TagForm::Title) {  // not editable: just says how many
                lines.push_back(hbox({text(std::string("  ") + album_labels[i]) | size(WIDTH, EQUAL, 10) | dim,
                                      text(form.fields[i]) | dim}));
                continue;
            }
            bool active = i == form.field;
            std::string value = active ? form_value() : form.fields[i];
            Element label = text(std::string(active ? "› " : "  ") + labels[i]) | size(WIDTH, EQUAL, 10);
            Element field = text(value + (active ? "▏" : ""));
            if (value.empty() && !active) field = text("—") | dim;
            lines.push_back(active ? hbox({label | color(Color::Green) | bold, field | bold})
                                   : hbox({label | dim, field}));
        }
        if (batch) {  // what the songs will be called, with the artist set now
            lines.push_back(separator());
            const std::string& artist = form.field == TagForm::Artist ? form_value() : form.fields[TagForm::Artist];
            int shown = std::min<int>(form.batch.size(), 8);
            for (int i = 0; i < shown; ++i) {
                std::string n = std::to_string(form.batch[i].index);
                lines.push_back(hbox({text(std::string(n.size() < 2 ? " " : "") + n + "  ") | dim,
                                      text(album_song_title(form.video_titles[i], artist))}));
            }
            if (static_cast<int>(form.batch.size()) > shown)
                lines.push_back(text("    … and " + std::to_string(form.batch.size() - shown) + " more") | dim);
        }

        lines.push_back(separator());

        if (form.field == TagForm::Title) {
            lines.push_back(text("The song's name") | dim);
        } else if (form.field == TagForm::Podcast) {
            lines.push_back(text("Podcasts remember where you stopped and play at their own speed (< / >)") | dim);
        } else if (form.field == TagForm::Album && trim(form.fields[TagForm::Artist]).empty()) {
            lines.push_back(text("Albums are suggested once the artist is set") | dim);
        } else {
            auto sugg = suggestions();
            if (sugg.empty() && trim(form.fields[form.field]).empty()) {
                lines.push_back(text(form.field == TagForm::Artist ? "No artists in your library yet"
                                                                   : "No albums by this artist yet") | dim);
            }
            add_suggestions(lines, sugg, form.pick, form.fields[form.field], nouns[form.field]);
        }

        lines.push_back(separator());
        lines.push_back(text("Enter next/save · Tab fields · Ctrl-n/p pick · Ctrl-u clear · Esc cancel") | dim);
        std::string title = batch ? " Add playlist as album " : !form.is_new ? " Edit tags "
                          : form.podcast ? " Add podcast " : " Add song ";
        return window(text(title) | bold | color(Color::Green), vbox(std::move(lines))) |
               size(WIDTH, EQUAL, 76) | clear_under | center;
    }

    Element draw_picker() const {
        Elements lines;
        lines.push_back(text("Add " + plural(picking.size(), "song") + " to:") | dim);
        lines.push_back(text(""));
        lines.push_back(hbox({text("› Playlist  ") | color(Color::Green) | bold, text(pick_value() + "▏") | bold}));
        lines.push_back(separator());
        auto sugg = complete(playlist_names(), input);
        if (playlists.empty() && trim(input).empty()) {
            lines.push_back(text("No playlists yet: type a name to create one") | dim);
        }
        add_suggestions(lines, sugg, pick, input, "playlist");
        lines.push_back(separator());
        lines.push_back(text("Enter add · Ctrl-n/p pick · Esc cancel") | dim);
        return window(text(" Add to playlist ") | bold | color(Color::Green), vbox(std::move(lines))) |
               size(WIDTH, EQUAL, 56) | clear_under | center;
    }

    Element draw_help() const {
        auto line = [](std::string keys, std::string what) {
            return hbox({text(keys) | color(Color::Green) | size(WIDTH, EQUAL, 18), text(what)});
        };
        auto heading = [](std::string s) { return text(s) | bold; };
        Element left = vbox({
            heading("Moving"),
            line("j / k", "down / up (counts work: 5j)"),
            line("gg / G", "top / bottom (10G = line 10)"),
            line("Ctrl-d / Ctrl-u", "half page down / up"),
            line("h / l / Tab", "pane left / right"),
            line("o", "jump to the playing song"),
            line("/", "search: filters songs; jumps to a match in artists / albums (Esc clears)"),
            line("n / N", "next / previous match (artists / albums)"),
            heading("Playing"),
            line("Enter", "play song (or open the pane to the right)"),
            line("Space", "pause / resume"),
            line("] / [", "next / previous song"),
            line("← / →  { / }", "seek 5s / 30s"),
            line("+ / -   s", "volume   shuffle"),
            line("v / b / i", "mini / big spectrum, now playing"),
            line("> / <  :speed 1.5", "faster / slower (podcasts only, saved per artist)"),
            heading("Library"),
            line("as / aa / ap", "add a song / an album (playlist) / a podcast from a URL"),
            line("e", "edit tags (or rename the artist / album)"),
            line("", "a playlist URL adds the whole playlist as an album"),
            line("aS / aA / aP", "same, but only list it (⇣), without downloading"),
            line("dd", "delete song (or the album / artist pane's selection)"),
            line("dr / da", "delete the album (albums pane) / artist (artists pane)"),
            line("", "asks: y/Enter = downloads only, a = also forget it, n = cancel"),
            line(":vis :big :now", "same, add on|off to choose"),
            line(":rescan  :q", "rescan the folder / quit"),
        });
        Element right = vbox({
            heading("Playlists  (♫ Playlists in the artists pane)"),
            line("A", "add song(s) to a playlist"),
            line("yy / dd / p / P", "yank / remove from playlist / paste"),
            line("V", "select lines, then y, d or A"),
            line("J / K", "move song down / up (in an album, renumbers it)"),
            line("x then p / P", "cut a song, put it after / before another (album)"),
            line(":new :rename", ":new Name  :rename Name  :delete"),
            heading("Other computers  (⇣ = not downloaded here)"),
            line("Enter on ⇣", "download that song"),
            line(":sync", "download every missing song"),
            line(":sync album", "download the selected album's missing songs"),
            line(":sync artist", "download the selected artist's missing songs"),
            line(":podcast [on|off]", "mark the selected song / album / artist as a podcast"),
            line(":shrink", "re-encode the selected song / album / artist to 48 kbps"),
            line(":order", "number the selected album's songs in album order (MusicBrainz)"),
            line(":source URL", "set where the song can be downloaded"),
            line(":forget", "remove a missing song from the list"),
        });
        return window(text(" Keys ") | bold, vbox({
            hbox({left, text("    "), right}),
            separator(),
            text("Any key closes this") | dim | center,
        })) | clear_under | center;
    }

    Element draw() {
        auto term = Terminal::Size();
        cols = term.dimx;
        int list_rows_base = term.dimy - 4;  // minus status, pane borders and header

        // Bottom row: [ now playing ][ mini spectrum ......... ], 3 rows
        // inside. The mini spectrum hides where it would be too cramped
        // (a phone). Either panel can be turned off.
        int now_w = show_now ? std::clamp(cols / 3, 34, 56) : 0;
        bool fits = cols - now_w >= 30 && term.dimy >= 20;
        vis_rows = show_vis && fits ? 3 : 0;
        int bottom = vis_rows ? vis_rows : show_now ? 3 : 0;  // rows inside the bottom boxes

        // The big spectrum goes full width above that: about a quarter of
        // the height, plus its border (2) and frequency axis (2). Skipped
        // when it would leave the lists fewer than 6 rows.
        big_rows = show_big ? std::clamp(term.dimy / 4, 4, 14) : 0;

        // Rows left for list items: screen minus status (1), the panes'
        // borders (2) and header row (1), and the spectrum and bottom rows.
        rows = list_rows_base - (bottom ? bottom + 2 : 0);
        if (big_rows && rows - (big_rows + 4) < 6) big_rows = 0;
        rows = std::max(1, rows - (big_rows ? big_rows + 4 : 0));

        bool playing_now = playing >= 0 && !player.paused() && !player.loading();
        vis.set_live((vis_rows > 0 || big_rows > 0) && playing_now);

        // Fit as many panes as the width allows, then slide the window of
        // visible panes just far enough to keep the active one on screen.
        shown = cols >= 100 ? 3 : cols >= 65 ? 2 : 1;
        int active = static_cast<int>(pane);
        first_shown = std::clamp(first_shown, active - shown + 1, active);
        first_shown = std::clamp(first_shown, 0, 3 - shown);

        Elements visible_panes;
        if (visible(Pane::Artists)) visible_panes.push_back(draw_artists() | reflect(pane_box[0]));
        if (visible(Pane::Albums)) visible_panes.push_back(draw_groups() | reflect(pane_box[1]));
        if (visible(Pane::Songs)) visible_panes.push_back(draw_songs() | reflect(pane_box[2]));
        Element panes = hbox(std::move(visible_panes));

        Elements bottom_row;
        if (show_now) {
            Element now = draw_now_playing(bottom) | reflect(now_box);
            bottom_row.push_back(vis_rows ? now | size(WIDTH, EQUAL, now_w) : now | flex);
        } else {
            now_box = Box{};  // nothing there to tap
        }
        if (vis_rows) bottom_row.push_back(draw_spectrum(mini_bars, cols - (show_now ? now_w : 0), vis_rows, false, "v") | flex);
        vis.set_moving(playing_now || mini_bars.moving() || big_bars.moving());

        Element main = vbox({
            panes | flex,
            big_rows ? draw_spectrum(big_bars, cols, big_rows, true, "b") : text("") | size(HEIGHT, EQUAL, 0),
            hbox(std::move(bottom_row)),
            draw_status(),
        });
        if (mode == Mode::Tag) return dbox({main, draw_form()});
        if (mode == Mode::Pick) return dbox({main, draw_picker()});
        if (show_help) return dbox({main, draw_help()});
        return main;
    }
};

int main(int argc, char** argv) {
    // Library folder: first argument, else $CRATE_DIR, else ~/Music.
    fs::path dir;
    if (argc > 1) dir = argv[1];
    else if (const char* env = std::getenv("CRATE_DIR")) dir = env;
    else dir = fs::path(std::getenv("HOME")) / "Music";

    if (!fs::is_directory(dir)) {
        std::cerr << "crate: not a folder: " << dir << "\n";
        return 1;
    }
    // A full, clean path, so song paths always look the same (playlists match them).
    dir = fs::weakly_canonical(fs::absolute(dir));

    auto screen = ScreenInteractive::Fullscreen();
    // The player and downloader call this from their own threads; PostEvent
    // is thread-safe and makes FTXUI wake up and redraw.
    Player player([&] { screen.PostEvent(Event::Custom); });
    if (!player.ok()) {
        std::cerr << "crate: couldn't start mpv (is it installed?)\n";
        return 1;
    }
    Downloader downloader([&] { screen.PostEvent(Event::Custom); });

    App app(dir, player, downloader, screen);
    app.load();

    auto ui = Renderer([&] { return app.draw(); });
    ui |= CatchEvent([&](Event e) { return app.on_event(e); });
    screen.Loop(ui);
    app.shutdown();
}
