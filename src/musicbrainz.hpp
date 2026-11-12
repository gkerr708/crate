// Looking up an album's real track order on MusicBrainz (:order).
//
// Same idea as Shrinker: a worker thread does the slow part (a few web
// requests through curl, a second apart as MusicBrainz asks), then calls
// `on_change` so the UI can pick the answer up with take_result().
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct AlbumOrder {
    bool ok = false;
    std::string error;          // why not, if not ok
    std::string release;        // the release it matched, e.g. "The Wall (1979, GB)"
    std::vector<int> numbers;   // numbers[i] is song i's track number, 0 if it isn't on the album
};

class AlbumLookup {
public:
    explicit AlbumLookup(std::function<void()> on_change) : on_change_(std::move(on_change)) {}
    ~AlbumLookup();

    AlbumLookup(const AlbumLookup&) = delete;
    AlbumLookup& operator=(const AlbumLookup&) = delete;

    // Finds `album` by `artist` and numbers `titles` (the songs you have)
    // by where they appear on it. False if a lookup is already running.
    bool start(std::string artist, std::string album, std::vector<std::string> titles);

    bool busy() const { return busy_; }

    // The answer, once, after the lookup finishes.
    std::optional<AlbumOrder> take_result();

private:
    void run(std::string artist, std::string album, std::vector<std::string> titles);

    std::function<void()> on_change_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::mutex mutex_;
    std::optional<AlbumOrder> result_;
};

// Matches song titles against an album's track titles (in order), loosely:
// "Another Brick in the Wall, Pt. 1" finds "Another Brick in the Wall, Part 1",
// "Time (2011 Remaster)" finds "Time". Each track is used once.
// Returns the 1-based track number for each song, 0 if none matched.
std::vector<int> match_tracks(const std::vector<std::string>& songs, const std::vector<std::string>& album);
