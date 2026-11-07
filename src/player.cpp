#include "player.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;  // the current environment, passed on to mpv

// Put a string inside JSON quotes, escaping the characters JSON cares about.
static std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) continue;  // drop other control chars
                out += c;
        }
    }
    return out + "\"";
}

// Tiny JSON helpers. mpv's messages are simple one-line objects, so finding
// `"key":` and reading what follows is enough; no need for a JSON library.
static std::string json_field(const std::string& line, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = line.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    if (pos < line.size() && line[pos] == '"') {  // string value
        auto end = line.find('"', pos + 1);
        return line.substr(pos + 1, end - pos - 1);
    }
    auto end = line.find_first_of(",}", pos);  // number / true / false / null
    return line.substr(pos, end - pos);
}

Player::Player(std::function<void()> on_change) : on_change_(std::move(on_change)) {
    // Where to put the socket: XDG_RUNTIME_DIR on a desktop, else TMPDIR
    // (set by Termux on Android, which has no /tmp), else /tmp.
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime) runtime = std::getenv("TMPDIR");
    socket_path_ = std::string(runtime ? runtime : "/tmp") + "/crate-mpv-" +
                   std::to_string(getpid()) + ".sock";

    // --- Start mpv, with its terminal output thrown away (/dev/null) ---
    std::string ipc = "--input-ipc-server=" + socket_path_;
    const char* argv[] = {"mpv",          "--idle=yes",     "--no-video",
                          "--no-terminal", "--volume=70",   ipc.c_str(),
                          nullptr};

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd : {0, 1, 2}) {
        posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_RDWR, 0);
    }
    int err = posix_spawnp(&pid_, "mpv", &actions, nullptr, const_cast<char**>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (err != 0) {
        pid_ = -1;
        return;
    }

    // --- Connect to its socket. mpv needs a moment to create it, so retry ---
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
    for (int attempt = 0; attempt < 150 && fd_ < 0; ++attempt) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            fd_ = fd;
        } else {
            close(fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    if (fd_ < 0) return;

    // Ask mpv to message us whenever these properties change.
    send(R"({"command":["observe_property",1,"time-pos"]})");
    send(R"({"command":["observe_property",2,"duration"]})");
    send(R"({"command":["observe_property",3,"pause"]})");
    send(R"({"command":["observe_property",4,"volume"]})");

    reader_ = std::thread(&Player::read_loop, this);
}

Player::~Player() {
    if (fd_ >= 0) {
        send(R"({"command":["quit"]})");
        shutdown(fd_, SHUT_RDWR);  // wakes up the reader thread's read()
    }
    if (reader_.joinable()) reader_.join();
    if (fd_ >= 0) close(fd_);

    if (pid_ > 0) {
        // Give mpv up to a second to quit, then insist.
        for (int i = 0; i < 50 && waitpid(pid_, nullptr, WNOHANG) == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (kill(pid_, 0) == 0) {
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
        }
    }
    unlink(socket_path_.c_str());
}

void Player::send(const std::string& json) {
    if (fd_ < 0) return;
    std::string line = json + "\n";
    std::lock_guard lock(write_mutex_);
    // MSG_NOSIGNAL: if mpv died, return an error instead of killing us with SIGPIPE.
    ::send(fd_, line.data(), line.size(), MSG_NOSIGNAL);
}

void Player::play(const std::string& path, double start) {
    finished_ = false;
    // Until mpv reports the new file loaded, ignore position updates: late
    // ones from the previous file would otherwise look like this file's.
    loading_ = true;
    position_ = start;
    std::string cmd = R"({"command":["loadfile",)" + json_string(path) + R"(,"replace")";
    // loadfile <path> <flags> <index> <options>: -1 = no playlist index (mpv 0.38+).
    if (start > 0) cmd += R"(,-1,"start=)" + std::to_string(static_cast<int>(start)) + R"(")";
    send(cmd + "]}");
    send(R"({"command":["set_property","pause",false]})");
}

void Player::stop() { send(R"({"command":["stop"]})"); }
void Player::toggle_pause() { send(R"({"command":["cycle","pause"]})"); }

void Player::seek(double seconds) {
    send(R"({"command":["seek",)" + std::to_string(seconds) + R"(,"relative"]})");
}

void Player::change_volume(int delta) {
    send(R"({"command":["add","volume",)" + std::to_string(delta) + "]}");
}

void Player::read_loop() {
    std::string buffer;
    char chunk[4096];
    while (true) {
        ssize_t n = read(fd_, chunk, sizeof(chunk));
        if (n <= 0) return;  // mpv closed the socket, or we're shutting down
        buffer.append(chunk, static_cast<size_t>(n));

        // Messages are separated by newlines; handle every complete one.
        size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            handle_line(buffer.substr(0, newline));
            buffer.erase(0, newline + 1);
        }
    }
}

void Player::handle_line(const std::string& line) {
    std::string event = json_field(line, "event");

    if (event == "property-change") {
        std::string name = json_field(line, "name");
        std::string data = json_field(line, "data");
        double value = (data.empty() || data == "null") ? 0 : std::atof(data.c_str());

        if (name == "time-pos") {
            if (loading_) return;
            // time-pos changes many times a second; only redraw when the
            // displayed whole second actually changes.
            bool new_second = std::floor(value) != std::floor(position_.load());
            position_ = value;
            if (!new_second) return;
        } else if (name == "duration") {
            duration_ = value;
        } else if (name == "pause") {
            paused_ = (data == "true");
        } else if (name == "volume") {
            volume_ = value;
        }
        on_change_();
    } else if (event == "file-loaded") {
        loading_ = false;
    } else if (event == "end-file" && json_field(line, "reason") == "eof") {
        finished_ = true;
        position_ = 0;
        on_change_();
    }
}
