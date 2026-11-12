#include "musicbrainz.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

// MusicBrainz asks every client to say who it is.
static const char* kUserAgent = "crate/1.0 ( terminal music player )";

// ---------------------------------------------------------------------------
// A small JSON reader: just enough for MusicBrainz's answers.

namespace {

struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    double number = 0;
    std::string string;
    std::vector<Json> array;
    std::vector<std::string> keys;  // an object's keys, and its values in `array`

    // Missing keys and out-of-range indices give null, so lookups can chain.
    const Json& operator[](const std::string& key) const {
        static const Json null;
        if (kind != Object) return null;
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return array[i];
        return null;
    }
    const std::string& str() const { return string; }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool parse(Json& out) {
        if (!value(out)) return false;
        space();
        return i_ == s_.size();
    }

private:
    void space() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    bool eat(char c) {
        space();
        if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; }
        return false;
    }
    bool literal(const char* word) {
        std::string w = word;
        if (s_.compare(i_, w.size(), w) != 0) return false;
        i_ += w.size();
        return true;
    }

    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& cp) {
        if (i_ + 4 > s_.size()) return false;
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s_[i_++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= c - '0';
            else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        if (!eat('"')) return false;
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) return false;
            char e = s_[i_++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    // A pair of \u escapes for characters outside the BMP.
                    if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(i_, 2, "\\u") == 0) {
                        i_ += 2;
                        unsigned lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: out += e;  // \" \\ \/
            }
        }
        return false;
    }

    bool value(Json& v) {
        space();
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '{') {
            ++i_;
            v.kind = Json::Object;
            if (eat('}')) return true;
            do {
                std::string key;
                Json item;
                if (!string(key) || !eat(':') || !value(item)) return false;
                v.keys.push_back(std::move(key));
                v.array.push_back(std::move(item));
            } while (eat(','));
            return eat('}');
        }
        if (c == '[') {
            ++i_;
            v.kind = Json::Array;
            if (eat(']')) return true;
            do {
                Json item;
                if (!value(item)) return false;
                v.array.push_back(std::move(item));
            } while (eat(','));
            return eat(']');
        }
        if (c == '"') { v.kind = Json::String; return string(v.string); }
        if (literal("true")) { v.kind = Json::Bool; v.number = 1; return true; }
        if (literal("false")) { v.kind = Json::Bool; return true; }
        if (literal("null")) return true;
        size_t start = i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || std::strchr("+-.eE", s_[i_]))) ++i_;
        if (i_ == start) return false;
        v.kind = Json::Number;
        v.number = std::atof(s_.substr(start, i_ - start).c_str());
        return true;
    }

    const std::string& s_;
    size_t i_ = 0;
};

// ---------------------------------------------------------------------------
// Fetching, through curl (it's on every desktop and in Termux).

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// GETs `url` once and parses it as JSON. On failure, `error` says why.
bool fetch_once(const std::string& url, Json& out, std::string& error) {
    int fds[2];
    if (pipe(fds) != 0) { error = "couldn't start curl"; return false; }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);

    std::vector<std::string> args = {"curl", "--silent", "--fail", "--location", "--max-time", "20",
                                     "--user-agent", kUserAgent, "--", url};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    pid_t pid;
    int rc = posix_spawnp(&pid, "curl", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        error = "curl isn't installed";
        return false;
    }
    std::string body;
    char buf[8192];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) body.append(buf, n);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "couldn't reach MusicBrainz";
        return false;
    }
    if (!Parser(body).parse(out)) {
        error = "MusicBrainz sent something unreadable";
        return false;
    }
    return true;
}

// fetch_once, at most one request a second (MusicBrainz turns away faster
// clients), trying again a couple of times if it was turned away anyway.
bool fetch_json(const std::string& url, Json& out, std::string& error) {
    static std::mutex mutex;
    static std::chrono::steady_clock::time_point last;
    std::lock_guard lock(mutex);
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto wait = std::chrono::milliseconds(attempt == 0 ? 1100 : 3000);
        std::this_thread::sleep_until(last + wait);
        out = Json{};
        bool ok = fetch_once(url, out, error);
        last = std::chrono::steady_clock::now();
        if (ok || error != "couldn't reach MusicBrainz") return ok;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Matching titles.

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int roman_or_word(const std::string& w);

// "Another Brick in the Wall, Pt. 1" -> "another brick in the wall part 1".
// With `strip`, also drops (…) / […] and a " - 2011 Remaster" style tail.
std::string normalize(const std::string& title, bool strip) {
    std::string s = title;
    if (strip) {
        std::string kept;
        int depth = 0;
        for (char c : s) {
            if (c == '(' || c == '[') ++depth;
            else if ((c == ')' || c == ']') && depth > 0) --depth;
            else if (depth == 0) kept += c;
        }
        if (auto dash = kept.find(" - "); dash != std::string::npos && dash > 0) kept.erase(dash);
        s = kept;
    }
    std::vector<std::string> words;
    std::string word;
    // Numbers after "part" are written every way: Pts. 1-5, Parts I–V, Part One.
    bool numbering = false;
    auto flush = [&] {
        if (word.empty()) return;
        if (word == "pt" || word == "pts" || word == "parts") word = "part";
        if (numbering) {
            if (int n = roman_or_word(word)) word = std::to_string(n);
        }
        numbering = word == "part" || (numbering && std::isdigit(static_cast<unsigned char>(word[0])));
        words.push_back(std::move(word));
        word.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = s[i];
        if (c == '&') { flush(); words.push_back("and"); }
        else if (c == 0xC3 && i + 1 < s.size() && (s[i + 1] == '\xA6' || s[i + 1] == '\x86')) { word += "ae"; ++i; }
        else if (c == 0xE2 && i + 2 < s.size() && s[i + 1] == '\x80' && (s[i + 2] == '\x98' || s[i + 2] == '\x99')) i += 2;  // ’
        else if (c == 0xE2 && i + 2 < s.size() && s[i + 1] == '\x80') { flush(); i += 2; }  // – — • and friends
        else if (c == '\'') {}  // "Don't" and "Dont" are the same word
        else if (std::isalnum(c) || c >= 0x80) word += static_cast<char>(std::tolower(c));
        else flush();
    }
    flush();
    std::string out;
    for (const std::string& w : words) out += (out.empty() ? "" : " ") + w;
    return out;
}

// "iv" -> 4, "two" -> 2; 0 if it's neither.
int roman_or_word(const std::string& w) {
    static const char* names[] = {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"};
    for (int i = 0; i < 10; ++i)
        if (w == names[i]) return i + 1;
    if (w.empty() || w.find_first_not_of("ivx") != std::string::npos) return 0;
    int total = 0;
    for (size_t i = 0; i < w.size(); ++i) {
        int v = w[i] == 'i' ? 1 : w[i] == 'v' ? 5 : 10;
        int next = i + 1 < w.size() ? (w[i + 1] == 'i' ? 1 : w[i + 1] == 'v' ? 5 : 10) : 0;
        total += v < next ? -v : v;
    }
    return total;
}

// How many words two normalized titles share.
int shared_words(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::vector<std::string> out;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i)
            if (i == s.size() || s[i] == ' ') {
                if (i > start) out.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        return out;
    };
    std::vector<std::string> x = split(a), y = split(b);
    int n = 0;
    for (const std::string& w : x)
        if (std::find(y.begin(), y.end(), w) != y.end()) ++n;
    return n;
}

// Whether `small` appears in `big` as whole words.
bool contains_words(const std::string& big, const std::string& small) {
    if (small.size() < 4 || small.size() >= big.size()) return false;
    for (size_t at = big.find(small); at != std::string::npos; at = big.find(small, at + 1)) {
        bool left = at == 0 || big[at - 1] == ' ';
        bool right = at + small.size() == big.size() || big[at + small.size()] == ' ';
        if (left && right) return true;
    }
    return false;
}

}  // namespace

std::vector<int> match_tracks(const std::vector<std::string>& songs, const std::vector<std::string>& album) {
    std::vector<int> numbers(songs.size(), 0);
    std::vector<bool> used(album.size(), false);

    // Go through the songs in title order, numbers compared as numbers, so
    // when two could be either track ("Pts. 1-5" and "Pts. 6-9" against
    // "Part One" and "Part Two") they take them in that order.
    std::vector<size_t> by_title(songs.size());
    for (size_t i = 0; i < songs.size(); ++i) by_title[i] = i;
    auto natural_less = [](const std::string& x, const std::string& y) {
        size_t i = 0, j = 0;
        while (i < x.size() && j < y.size()) {
            if (std::isdigit(static_cast<unsigned char>(x[i])) && std::isdigit(static_cast<unsigned char>(y[j]))) {
                size_t i2 = i, j2 = j;
                while (i2 < x.size() && std::isdigit(static_cast<unsigned char>(x[i2]))) ++i2;
                while (j2 < y.size() && std::isdigit(static_cast<unsigned char>(y[j2]))) ++j2;
                long a = std::atol(x.substr(i, i2 - i).c_str()), b = std::atol(y.substr(j, j2 - j).c_str());
                if (a != b) return a < b;
                i = i2;
                j = j2;
            } else {
                if (x[i] != y[j]) return x[i] < y[j];
                ++i;
                ++j;
            }
        }
        return x.size() - i < y.size() - j;
    };
    std::vector<std::string> keys;
    for (const std::string& t : songs) keys.push_back(normalize(t, false));
    std::stable_sort(by_title.begin(), by_title.end(),
                     [&](size_t a, size_t b) { return natural_less(keys[a], keys[b]); });

    // Strictest first, so "In the Flesh?" and "In the Flesh" each find their own.
    auto pass = [&](auto key) {
        for (size_t s : by_title) {
            if (numbers[s]) continue;
            std::string want = key(songs[s]);
            if (want.empty()) continue;
            for (size_t t = 0; t < album.size(); ++t) {
                if (!used[t] && key(album[t]) == want) {
                    used[t] = true;
                    numbers[s] = static_cast<int>(t) + 1;
                    break;
                }
            }
        }
    };
    pass([](const std::string& x) { return lower_ascii(x); });
    pass([](const std::string& x) { return normalize(x, false); });
    pass([](const std::string& x) { return normalize(x, true); });

    // Last, one inside the other: "Schism (Official Video)" -> "Schism".
    // If several fit ("Shine On..., Parts I–V" and "..., Parts VI–IX"), the
    // one sharing the most words wins, then the earlier one.
    for (size_t s : by_title) {
        if (numbers[s]) continue;
        std::string a = normalize(songs[s], false), a_strip = normalize(songs[s], true);
        size_t best = album.size();
        int best_shared = -1;
        for (size_t t = 0; t < album.size(); ++t) {
            if (used[t]) continue;
            std::string b = normalize(album[t], true);
            if (!contains_words(a, b) && !contains_words(b, a_strip)) continue;
            int shared = shared_words(a, normalize(album[t], false));
            if (shared > best_shared) {
                best = t;
                best_shared = shared;
            }
        }
        if (best < album.size()) {
            used[best] = true;
            numbers[s] = static_cast<int>(best) + 1;
        }
    }
    return numbers;
}

// ---------------------------------------------------------------------------

AlbumLookup::~AlbumLookup() {
    if (worker_.joinable()) worker_.join();
}

bool AlbumLookup::start(std::string artist, std::string album, std::vector<std::string> titles) {
    if (busy_) return false;
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    worker_ = std::thread(&AlbumLookup::run, this, std::move(artist), std::move(album), std::move(titles));
    return true;
}

std::optional<AlbumOrder> AlbumLookup::take_result() {
    std::lock_guard lock(mutex_);
    return std::exchange(result_, std::nullopt);
}

void AlbumLookup::run(std::string artist, std::string album, std::vector<std::string> titles) {
    AlbumOrder order;
    auto finish = [&] {
        {
            std::lock_guard lock(mutex_);
            result_ = std::move(order);
        }
        busy_ = false;
        on_change_();
    };
    // Quotes would end the search phrase early.
    auto phrase = [](std::string s) {
        std::erase_if(s, [](char c) { return c == '"' || c == '\\'; });
        return "\"" + s + "\"";
    };
    std::string query = "release:" + phrase(album) + " AND artist:" + phrase(artist);
    Json found;
    if (!fetch_json("https://musicbrainz.org/ws/2/release/?fmt=json&limit=15&query=" + url_encode(query), found,
                    order.error))
        return finish();

    // Search results are best first. Try a few, preferring official releases
    // and skipping ones with the same number of tracks as one already tried
    // (usually the same album pressed in another country).
    std::vector<const Json*> candidates;
    for (const Json& r : found["releases"].array)
        if (r["score"].number >= 80) candidates.push_back(&r);
    std::stable_partition(candidates.begin(), candidates.end(),
                          [](const Json* r) { return (*r)["status"].str() == "Official"; });
    std::vector<int> tried_counts;

    size_t best_matched = 0, best_total = 0;
    int tried = 0;
    for (const Json* r : candidates) {
        if (tried == 4) break;
        int count = static_cast<int>((*r)["track-count"].number);
        if (std::find(tried_counts.begin(), tried_counts.end(), count) != tried_counts.end()) continue;
        tried_counts.push_back(count);
        ++tried;

        Json release;
        std::string error;
        if (!fetch_json("https://musicbrainz.org/ws/2/release/" + (*r)["id"].str() + "?fmt=json&inc=recordings",
                        release, error)) {
            order.error = error;
            continue;
        }
        // Every disc, one after another: disc 2 track 1 comes after disc 1's last.
        std::vector<std::string> list;
        for (const Json& medium : release["media"].array) {
            const std::string& format = medium["format"].str();
            if (format.find("DVD") != std::string::npos || format.find("Blu-ray") != std::string::npos) continue;
            for (const Json& track : medium["tracks"].array) list.push_back(track["title"].str());
        }
        std::vector<int> numbers = match_tracks(titles, list);
        size_t matched = std::count_if(numbers.begin(), numbers.end(), [](int n) { return n > 0; });
        // More matches wins; on a tie, the shorter release (not the deluxe edition).
        if (matched > best_matched || (matched == best_matched && matched > 0 && list.size() < best_total)) {
            best_matched = matched;
            best_total = list.size();
            order.ok = true;
            order.numbers = std::move(numbers);
            order.release = release["title"].str();
            std::string year = release["date"].str().substr(0, 4);
            if (!year.empty()) order.release += " (" + year + ")";
        }
        if (matched == titles.size() && list.size() == titles.size()) break;  // can't do better
    }
    // A release with only a few of the songs (one episode of a series,
    // a single) is the wrong thing: leave the order alone.
    if (order.ok && best_matched * 2 < titles.size()) {
        order.ok = false;
        order.error = "only " + std::to_string(best_matched) + " of the " + std::to_string(titles.size()) +
                      " songs are on " + order.release;
    }
    if (!order.ok && order.error.empty())
        order.error = candidates.empty() ? "couldn't find it on MusicBrainz" : "none of the songs are on it";
    if (order.ok) order.error.clear();
    finish();
}
