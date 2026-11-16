#include "sync.hpp"

#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <string>

namespace {

std::string quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

struct Result {
    int code = -1;
    std::string out;
};

// Runs `git -C <dir> <args>` with stderr merged into the output. Never
// waits on a password or host-key prompt, and gives up after `seconds`.
Result git(const std::filesystem::path& dir, const std::string& args, int seconds = 30) {
    std::string cmd = "GIT_TERMINAL_PROMPT=0 GIT_SSH_COMMAND='ssh -o BatchMode=yes -o ConnectTimeout=5' "
                      "timeout " + std::to_string(seconds) + " git -C " + quote(dir.string()) + " " + args + " 2>&1";
    Result r;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return r;
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof buf, p)) r.out.append(buf, n);
    int status = pclose(p);
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    while (!r.out.empty() && (r.out.back() == '\n' || r.out.back() == ' ')) r.out.pop_back();
    return r;
}

bool interactive() { return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO); }

// Y/n question; Enter means yes.
bool ask(const std::string& question) {
    std::cout << question << " [Y/n] " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    return line.empty() || line[0] == 'y' || line[0] == 'Y';
}

int count(const std::filesystem::path& dir, const std::string& range) {
    Result r = git(dir, "rev-list --count " + range);
    return r.code == 0 ? std::atoi(r.out.c_str()) : 0;
}

bool has_upstream(const std::filesystem::path& dir) {
    return git(dir, "rev-parse --abbrev-ref --symbolic-full-name @{u}").code == 0;
}

bool is_repo(const std::filesystem::path& dir) {
    // --show-toplevel must be the library folder itself, not a parent repo.
    Result r = git(dir, "rev-parse --show-toplevel");
    if (r.code != 0) return false;
    std::error_code ec;
    return std::filesystem::equivalent(r.out, dir, ec);
}

// Files left half-merged (git can exit 0 with these after an autostash).
bool has_conflicts(const std::filesystem::path& dir) {
    return !git(dir, "diff --name-only --diff-filter=U").out.empty();
}

// Pulls the remote's commits on top of ours, keeping uncommitted edits. If
// those edits clash with what came in, undo the clash and leave them in the
// stash rather than leaving conflict markers in the files.
bool pull(const std::filesystem::path& dir, std::string& error) {
    Result p = git(dir, "pull --rebase --autostash --quiet");
    if (p.code == 0 && !has_conflicts(dir)) return true;
    if (has_conflicts(dir)) {
        git(dir, "reset --hard --quiet HEAD");
        error = "edits clash; yours are in git stash";
    } else {
        error = p.out;
    }
    return false;
}

std::string plural(int n, const char* what) { return std::to_string(n) + " " + what + (n == 1 ? "" : "s"); }

}  // namespace

SyncResult sync_check(const std::filesystem::path& dir) {
    SyncResult r;
    if (!is_repo(dir) || !has_upstream(dir)) {
        r.repo = false;
        return r;
    }
    if (git(dir, "fetch --quiet", 8).code != 0) {
        r.ok = false;
        r.text = "Couldn't reach the music repo's remote";
        return r;
    }
    r.behind = count(dir, "HEAD..@{u}");
    if (r.behind > 0) r.text = "Music repo: " + plural(r.behind, "new commit") + " on the remote. :pull to get them";
    return r;
}

SyncResult sync_pull(const std::filesystem::path& dir) {
    SyncResult r;
    if (!is_repo(dir) || !has_upstream(dir)) {
        r.repo = false;
        r.ok = false;
        r.text = "The music folder isn't a git repo with a remote";
        return r;
    }
    if (git(dir, "fetch --quiet", 15).code != 0) {
        r.ok = false;
        r.text = "Couldn't reach the music repo's remote";
        return r;
    }
    int behind = count(dir, "HEAD..@{u}");
    if (behind == 0) {
        r.text = "Music repo is already up to date";
        return r;
    }
    std::string err;
    if (pull(dir, err)) {
        r.text = "Pulled " + plural(behind, "commit");
    } else {
        r.ok = false;
        r.text = "Pull failed: " + err;
    }
    return r;
}

SyncResult sync_push(const std::filesystem::path& dir) {
    SyncResult r;
    if (!is_repo(dir) || !has_upstream(dir)) {
        r.repo = false;
        r.ok = false;
        r.text = "The music folder isn't a git repo with a remote";
        return r;
    }
    if (has_conflicts(dir)) {
        r.ok = false;
        r.text = "The music repo has unresolved merge conflicts; fix them with git first";
        return r;
    }
    bool dirty = !git(dir, "status --porcelain").out.empty();
    if (!dirty && count(dir, "@{u}..HEAD") == 0) {
        r.text = "Nothing to push";
        return r;
    }
    if (dirty) {
        Result a = git(dir, "add -A");
        Result c = a.code == 0 ? git(dir, "commit --quiet -m 'Update library (crate)'") : a;
        if (c.code != 0) {
            r.ok = false;
            r.text = "Commit failed: " + c.out;
            return r;
        }
    }
    Result p = git(dir, "push --quiet");
    if (p.code == 0) {
        r.text = "Pushed";
        return r;
    }
    // Probably behind the remote: rebase on it and try once more.
    std::string err;
    if (pull(dir, err)) p = git(dir, "push --quiet");
    else p.out = err;
    if (p.code == 0) {
        r.text = "Pulled the remote's changes and pushed";
    } else {
        r.ok = false;
        r.text = "Push failed: " + p.out;
    }
    return r;
}

void sync_on_exit(const std::filesystem::path& dir) {
    if (!interactive() || !is_repo(dir) || !has_upstream(dir)) return;
    if (has_conflicts(dir)) {
        std::cout << "crate: the music repo has unresolved merge conflicts; fix them with git first.\n";
        return;
    }
    // Ignored files (the audio) don't show up here, only what the repo tracks.
    Result st = git(dir, "status --porcelain");
    int ahead = count(dir, "@{u}..HEAD");
    if (st.out.empty() && ahead == 0) return;

    if (!st.out.empty()) std::cout << "The music repo has uncommitted changes:\n" << st.out << "\n";
    if (ahead > 0) std::cout << "It also has " << plural(ahead, "commit") << " not pushed yet.\n";
    if (!ask(st.out.empty() ? "Push them now?" : "Commit and push them now?")) return;

    SyncResult r = sync_push(dir);
    std::cout << (r.ok ? "" : "crate: ") << r.text << (r.ok ? ".\n" : "\n");
}
