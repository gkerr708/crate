// crate: a vim-flavoured music library and player for the terminal.
//
// Layout:  [ Artists ][ Albums ][ Songs ............................ ]
//          [ now playing + progress bar                               ]
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
//   main.cpp     - this file: all the state, key handling and drawing
//
// FTXUI works like the todo app: we keep plain C++ state in `App`, one
// function draws the screen from that state, and one function changes the
// state in response to keys. FTXUI redraws after every event.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <string>
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
#include "player.hpp"
#include "playlist.hpp"

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
enum class Mode { Normal, Search, Command, Url, Tag, Pick, Confirm };

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
    enum Field { Title, Artist, Album };
    fs::path file;
    bool is_new = false;     // a fresh download, to be moved into the library on save
    std::string fields[3];   // what's typed in each field
    int field = Title;       // the field being edited
    int pick = -1;           // highlighted suggestion; -1 = use what's typed
    std::string source;      // URL it was downloaded from
};

// Where downloads wait until they're tagged: ~/.cache/crate/incoming
static fs::path incoming_dir() {
    const char* cache = std::getenv("XDG_CACHE_HOME");
    fs::path base = cache ? fs::path(cache) : fs::path(std::getenv("HOME")) / ".cache";
    return base / "crate" / "incoming";
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
    std::string filter;                // active search text

    // --- UI ---
    Pane pane = Pane::Songs;
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
    // The rightmost visible pane stretches; the others keep fixed widths.
    bool stretches(Pane p) const { return static_cast<int>(p) == first_shown + shown - 1; }
    int count = 0;        // vim count prefix, e.g. the 5 in 5j
    char pending = 0;     // first key of gg / yy / dd
    int pending_count = 0;
    int visual = -1;      // V mode: the row where the selection started, -1 = off

    // --- playlist editing ---
    std::vector<fs::path> yanked;   // songs copied with yy / dd, pasted with p
    std::vector<fs::path> picking;  // songs being added with A
    int pick = -1;                  // highlighted playlist in the A picker
    std::string confirm_text;       // question asked in Confirm mode (y/n)
    std::function<void()> confirm_action;

    TagForm form;

    // --- library.tsv / downloading missing songs ---
    std::unordered_map<std::string, std::string> pending_sources;  // new file -> its URL, until the next load()
    std::vector<std::string> restore_queue;  // missing songs (relative paths) waiting to download
    std::string restoring;                   // the one downloading now, "" if none
    std::string last_url;                    // what was typed in the `a` prompt

    // --- playback ---
    std::vector<int> queue;  // what we're playing through (indices into tracks)
    int queue_pos = -1;
    int playing = -1;        // index into tracks, -1 if nothing
    bool shuffle = false;
    std::mt19937 rng{std::random_device{}()};

    // =================================================================
    // Library: building the three panes
    // =================================================================
    void load() {
        std::string playing_path = playing >= 0 ? tracks[playing].path.string() : "";
        tracks = scan_library(dir);
        merge_manifest();
        track_index.clear();
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i) track_index[tracks[i].path.string()] = i;
        playlists = load_playlists(dir);

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
                listed.erase(it);
            }
            if (auto it = pending_sources.find(r); it != pending_sources.end()) t.source = it->second;
        }
        pending_sources.clear();

        for (auto& [path, e] : listed) {  // what's left isn't on this computer
            Track t;
            t.path = dir / path;
            t.title = e.title.empty() ? fs::path(path).stem().string() : e.title;
            t.artist = e.artist.empty() ? "Unknown Artist" : e.artist;
            t.album = e.album;
            t.seconds = e.seconds;
            t.source = e.source;
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
                               t.seconds, t.source});
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
    void start(int track) {
        playing = track;
        player.play(tracks[track].path.string());
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

    // J / K: swap the song with its neighbour.
    void move_song(int delta) {
        Playlist* pl = current_playlist();
        if (!pl) {
            message = "J / K reorder songs inside a playlist";
            return;
        }
        int other = song_cur.sel + delta;
        if (songs.empty() || other < 0 || other >= static_cast<int>(songs.size())) return;
        std::swap(pl->songs[slots[song_cur.sel]], pl->songs[slots[other]]);
        save(*pl);
        refresh_songs();
        song_cur.sel = other;
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
            tracks.erase(tracks.begin() + track_index[copy.path.string()]);  // drop the old entry
            save_manifest();
        }
        write_tags(result.file, t->title, t->artist == "Unknown Artist" ? "" : t->artist, t->album);
        std::string title = t->title;
        if (!move_file(result.file, dest)) {
            message = "Downloaded " + title + ", but couldn't move it to " + dest.string();
            return;
        }
        load();
        int left = static_cast<int>(restore_queue.size());
        message = "Downloaded " + title + (left ? " (" + std::to_string(left) + " to go)" : "");
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
            if (artist_cur.sel == kAllArtists || artist_cur.sel == kPlaylists) {
                message = "Pick an artist to delete (this one is a whole category)";
                return;
            }
            std::string artist = artists[artist_cur.sel];
            std::vector<int> doomed;
            for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
                if (lower(tracks[i].artist) == lower(artist)) doomed.push_back(i);
            }
            delete_songs(doomed, "everything by " + artist);
        } else if (pane == Pane::Albums && !groups.empty()) {
            const Group& g = groups[group_cur.sel];
            if (g.kind == Group::List) return delete_playlist();
            if (g.kind == Group::AllAlbums) {
                message = "Pick an album to delete, or press dd on the artist to delete all of it";
                return;
            }
            std::vector<int> doomed;
            for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
                if (lower(tracks[i].artist) == lower(g.artist) && lower(tracks[i].album) == lower(g.album))
                    doomed.push_back(i);
            }
            delete_songs(doomed, "the album " + g.label);
        }
    }

    // Ask, then move these songs to the trash and forget them everywhere.
    // `what` describes them in the question ("the album X"); "" = list titles.
    void delete_songs(const std::vector<int>& doomed, const std::string& what) {
        if (doomed.empty()) return;
        std::vector<fs::path> paths;
        int on_disk = 0;
        for (int i : doomed) {
            paths.push_back(tracks[i].path);
            if (!tracks[i].missing) ++on_disk;
        }
        std::string subject = !what.empty() ? what + " (" + plural(paths.size(), "song") + ")"
                            : paths.size() == 1 ? "\"" + tracks[doomed[0]].title + "\""
                                                : plural(paths.size(), "song");
        confirm_text = on_disk ? "Move " + subject + " to the trash? (y/n)"
                               : "Remove " + subject + " from library.tsv? It isn't downloaded here. (y/n)";
        // Remember paths, not indices: indices change whenever the library reloads.
        confirm_action = [this, paths] { really_delete(paths); };
        mode = Mode::Confirm;
    }

    void really_delete(const std::vector<fs::path>& paths) {
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

        message = "Deleted " + plural(gone.size(), "song") +
                  (trashed ? " (" + std::to_string(trashed) + " in the trash, restore from your file manager)" : "");
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

    // `a`: ask for a URL, pre-filled from the clipboard.
    void begin_add() {
        if (downloader.busy() || !restoring.empty()) {
            message = "A download is already running";
            return;
        }
        mode = Mode::Url;
        input = clipboard_url();
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
            message = "Download failed: " + result->error;
            return;
        }

        // Pre-fill from the tags yt-dlp wrote, e.g. title "Tool - Schism (Official Video)".
        Track t = read_track(result->file);
        TitleGuess guess = guess_from_title(t.title);
        std::string artist = guess.artist;
        if (artist.empty() && t.artist != "Unknown Artist") artist = clean_channel(t.artist);

        form = {};
        form.file = result->file;
        form.is_new = true;
        form.source = result->source.empty() ? last_url : result->source;
        form.fields[TagForm::Title] = guess.title;
        form.fields[TagForm::Artist] = match_known(TagForm::Artist, artist);
        form.fields[TagForm::Album] = match_known(TagForm::Album, t.album);
        form.field = TagForm::Title;
        mode = Mode::Tag;
        message.clear();
    }

    // `e`: edit the selected song's tags.
    void begin_edit() {
        if (pane != Pane::Songs || songs.empty()) return;
        const Track& t = tracks[songs[song_cur.sel]];
        form = {};
        form.file = t.path;
        form.fields[TagForm::Title] = t.title;
        form.fields[TagForm::Artist] = t.artist == "Unknown Artist" ? "" : t.artist;
        form.fields[TagForm::Album] = t.album;
        mode = Mode::Tag;
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
        if (form.field == TagForm::Title) return {};
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
        const std::string& title = form.fields[TagForm::Title];
        const std::string& artist = form.fields[TagForm::Artist];
        const std::string& album = form.fields[TagForm::Album];
        if (title.empty()) {
            form.field = TagForm::Title;
            message = "The title can't be empty";
            return;
        }
        mode = Mode::Normal;
        if (!write_tags(form.file, title, artist, album)) {
            message = "Couldn't write tags to " + form.file.filename().string();
            return;
        }

        fs::path final_path = form.file;
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
        }
        load();
        select_path(final_path);
        message = "Saved: " + title + (artist.empty() ? "" : " — " + artist);
    }

    void cancel_form() {
        mode = Mode::Normal;
        if (!form.is_new) {
            message = "Edit cancelled";
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
        auto go_to_field = [&](int f) {
            commit_field();
            form.field = std::clamp(f, 0, 2);
        };

        if (e == Event::Escape) cancel_form();
        else if (e == Event::Return) {
            if (form.field == TagForm::Album) save_form();
            else go_to_field(form.field + 1);
        }
        else if (e == Event::Tab) go_to_field((form.field + 1) % 3);
        else if (e == Event::TabReverse) go_to_field((form.field + 2) % 3);
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
        else if (verb == "help") show_help = true;
        else if (verb == "new") new_playlist(arg);
        else if (verb == "rename") rename_playlist(arg);
        else if (verb == "delete") delete_playlist();
        else if (verb == "sync") {
            std::vector<std::string> missing;
            for (const Track& t : tracks) if (t.missing) missing.push_back(rel(t.path));
            if (missing.empty()) message = "Everything in library.tsv is already downloaded";
            else restore(missing);
        }
        else if (verb == "source") set_source(arg);
        else if (verb == "forget") forget_song();
        else if (!verb.empty() && std::all_of(verb.begin(), verb.end(), ::isdigit)) {
            move_to(std::stoi(verb) - 1);  // :42 goes to line 42, like vim
        }
        else if (!verb.empty()) message = "Unknown command: " + verb;
    }

    // Typing after /, : or the URL prompt
    bool on_input_key(const Event& e) {
        if (e == Event::Escape) {
            if (mode == Mode::Search) { filter.clear(); refresh_songs(); }
            mode = Mode::Normal;
        } else if (e == Event::Return) {
            Mode was = mode;
            mode = Mode::Normal;
            if (was == Mode::Command) run_command(input);
            if (was == Mode::Url && !trim(input).empty()) {
                last_url = trim(input);
                downloader.start(last_url, incoming_dir());
                message = "Downloading…";
            }
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
        if (mode == Mode::Search) {  // search filters live as you type
            filter = input;
            set_pane(static_cast<int>(Pane::Songs));
            refresh_songs();
        }
        return true;
    }

    bool on_event(const Event& e) {
        bool handled = handle_event(e);
        check_download();  // after the key too, in case it just closed a prompt
        return handled;
    }

    bool handle_event(const Event& e) {
        if (player.take_finished()) next();
        check_download();
        if (e == Event::Custom) return true;  // just a redraw request from a background thread

        if (mode == Mode::Tag) return on_form_key(e);
        if (mode == Mode::Pick) return on_pick_key(e);
        if (mode == Mode::Confirm) {
            mode = Mode::Normal;
            if (e == Event::Character('y')) confirm_action();
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

        // --- playlists (vim-style: yank, delete, put) ---
        else if (c == "V") songs_only([&] { visual = (visual >= 0 || songs.empty()) ? -1 : song_cur.sel; });
        else if (c == "y") songs_only([&] { visual >= 0 ? yank(1) : start_pending('y'); });
        else if (c == "d") { visual >= 0 ? delete_rows(1) : start_pending('d'); }
        else if (c == "p") songs_only([&] { put(false); });
        else if (c == "P") songs_only([&] { put(true); });
        else if (c == "A") songs_only([&] { begin_pick(n); });
        else if (c == "J") songs_only([&] { move_song(+1); });
        else if (c == "K") songs_only([&] { move_song(-1); });

        // --- library ---
        else if (c == "a") begin_add();
        else if (c == "e") begin_edit();

        // --- modes ---
        else if (c == "/") { mode = Mode::Search; input = filter; }
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

    Element draw_artists() {
        int n = static_cast<int>(artists.size()) - 2;
        return draw_list(" Artists ", Pane::Artists, artist_cur, artists.size(),
                         text(" " + plural(n, "artist")), [&](int i) {
            if (i == kPlaylists) return text(" " + artists[i] + " ") | color(Color::Cyan);
            return text(" " + artists[i] + " ");
        }) | (stretches(Pane::Artists) ? flex : size(WIDTH, EQUAL, 24));
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
            if (g.kind == Group::Album && artist_cur.sel == kAllArtists) {
                return hbox({text(" " + g.label), text("  " + g.artist) | dim});
            }
            return text(" " + g.label + " ");
        });
        if (lists && groups.empty()) {
            body = window(text(" Playlists ") | bold | (pane == Pane::Albums ? color(Color::Green) : dim),
                          vbox({text(" No playlists yet.") | dim, text(""),
                                text(" Press A on a song to add") | dim, text(" it to a new playlist, or") | dim,
                                text(" type :new Name") | dim}) | flex);
        }
        return body | (stretches(Pane::Albums) ? flex : size(WIDTH, EQUAL, 30));
    }

    Element draw_songs() {
        // Drop the album, then the artist column when there isn't room,
        // so the title always gets space.
        int inner = cols - 2 - (visible(Pane::Artists) ? 24 : 0) - (visible(Pane::Albums) ? 30 : 0);
        bool show_album = inner >= 70, show_artist = inner >= 45;
        auto columns = [=](Element mark, Element title, Element artist, Element album, Element time) {
            Elements cells = {mark | size(WIDTH, EQUAL, 2), title | flex};
            if (show_artist) cells.insert(cells.end(), {text(" "), artist | size(WIDTH, EQUAL, 18)});
            if (show_album) cells.insert(cells.end(), {text(" "), album | size(WIDTH, EQUAL, 18)});
            cells.push_back(time | align_right | size(WIDTH, EQUAL, 6));
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
            Element row = columns(text(mark), text(t.title), text(t.artist),
                                  text(t.album), text(format_time(t.seconds)));
            if (t.missing) return row | dim;
            return is_playing ? row | color(Color::Green) : row;
        }, [&](int i) { return i >= lo && i <= hi; }) | flex;
    }

    Element draw_now_playing() const {
        Element top, bottom;
        if (playing < 0) {
            top = text("  Nothing playing. Enter plays the selected song.") | dim;
            bottom = text("");
        } else {
            const Track& t = tracks[playing];
            double pos = player.position(), len = player.duration();
            float progress = len > 0 ? static_cast<float>(pos / len) : 0.f;
            top = hbox({text(player.paused() ? " ⏸ " : " ▶ ") | color(Color::Green),
                        text(t.title) | bold, text("  " + t.artist) | dim,
                        text(t.album.empty() ? "" : "  ·  " + t.album) | dim});
            bottom = hbox({text(" " + (pos < 1 ? std::string("0:00") : format_time((int)pos)) + " "),
                           gauge(progress) | color(Color::Green) | flex,
                           text(" " + format_time((int)len) + " ")});
        }
        std::string flags = " vol " + std::to_string(player.volume()) + "%" +
                            (shuffle ? "  shuffle" : "") + " ";
        return window(text(" Now playing ") | dim,
                      vbox({hbox({top | flex, text(flags) | dim}), bottom}));
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
        } else if (mode == Mode::Url) {
            mode_tag = tag("ADD", Color::Cyan);
            rest = hbox({text(" URL: "), text(input + "▏"), text("   Enter download · Esc cancel") | dim});
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
        std::string right = plural(songs.size(), "song") + "  ? help ";
        std::string queued = restore_queue.empty() ? "" : " +" + std::to_string(restore_queue.size());
        Element download = downloader.busy()
            ? text(" ⇣ downloading " + std::to_string(downloader.percent()) + "%" + queued + " ") | color(Color::Cyan)
            : text("");
        return hbox({mode_tag | bold, rest | flex, download, text(right) | dim});
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
        static const char* labels[] = {"Title", "Artist", "Album"};
        static const char* nouns[] = {"title", "artist", "album"};
        Elements lines;
        lines.push_back(text(form.file.filename().string()) | dim);
        lines.push_back(text(""));

        for (int i = 0; i < 3; ++i) {
            bool active = i == form.field;
            std::string value = active ? form_value() : form.fields[i];
            Element label = text(std::string(active ? "› " : "  ") + labels[i]) | size(WIDTH, EQUAL, 10);
            Element field = text(value + (active ? "▏" : ""));
            if (value.empty() && !active) field = text("—") | dim;
            lines.push_back(active ? hbox({label | color(Color::Green) | bold, field | bold})
                                   : hbox({label | dim, field}));
        }
        lines.push_back(separator());

        if (form.field == TagForm::Title) {
            lines.push_back(text("The song's name") | dim);
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
        std::string title = form.is_new ? " Add song " : " Edit tags ";
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
            line("/", "search (Esc clears)"),
            heading("Playing"),
            line("Enter", "play song (or open the pane to the right)"),
            line("Space", "pause / resume"),
            line("] / [", "next / previous song"),
            line("← / →  { / }", "seek 5s / 30s"),
            line("+ / -   s", "volume   shuffle"),
            heading("Library"),
            line("a / e", "add song from URL / edit tags"),
            line("dd", "delete song / album / artist (asks; goes to trash)"),
            line(":rescan  :q", "rescan the folder / quit"),
        });
        Element right = vbox({
            heading("Playlists  (♫ Playlists in the artists pane)"),
            line("A", "add song(s) to a playlist"),
            line("yy / dd / p / P", "yank / remove from playlist / paste"),
            line("V", "select lines, then y, d or A"),
            line("J / K", "move song down / up"),
            line(":new :rename", ":new Name  :rename Name  :delete"),
            heading("Other computers  (⇣ = not downloaded here)"),
            line("Enter on ⇣", "download that song"),
            line(":sync", "download every missing song"),
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
        // Rows left for list items: screen minus now-playing (4), status (1),
        // the panes' borders (2) and their header row (1).
        auto term = Terminal::Size();
        rows = std::max(1, term.dimy - 8);
        cols = term.dimx;

        // Fit as many panes as the width allows, then slide the window of
        // visible panes just far enough to keep the active one on screen.
        shown = cols >= 100 ? 3 : cols >= 65 ? 2 : 1;
        int active = static_cast<int>(pane);
        first_shown = std::clamp(first_shown, active - shown + 1, active);
        first_shown = std::clamp(first_shown, 0, 3 - shown);

        Elements visible_panes;
        if (visible(Pane::Artists)) visible_panes.push_back(draw_artists());
        if (visible(Pane::Albums)) visible_panes.push_back(draw_groups());
        if (visible(Pane::Songs)) visible_panes.push_back(draw_songs());
        Element panes = hbox(std::move(visible_panes));

        Element main = vbox({
            panes | flex,
            draw_now_playing(),
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
}
