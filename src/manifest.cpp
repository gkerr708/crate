#include "manifest.hpp"

#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

fs::path manifest_path(const fs::path& library) { return library / "library.tsv"; }

// Tabs and newlines would break the columns, so swap them for spaces.
static std::string clean(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

std::vector<ManifestEntry> read_manifest(const fs::path& library) {
    std::vector<ManifestEntry> out;
    std::ifstream in(manifest_path(library));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string col;
        while (std::getline(ss, col, '\t')) cols.push_back(col);
        cols.resize(8);  // missing trailing columns become ""

        ManifestEntry e;
        e.path = cols[0];
        e.artist = cols[1];
        e.album = cols[2];
        e.title = cols[3];
        e.seconds = std::atoi(cols[4].c_str());
        e.source = cols[5];
        e.number = std::atoi(cols[6].c_str());
        e.podcast = cols[7] == "podcast";
        if (!e.path.empty()) out.push_back(std::move(e));
    }
    return out;
}

bool write_manifest(const fs::path& library, const std::vector<ManifestEntry>& entries) {
    std::ostringstream out;
    out << "# crate library index: one song per line, tab-separated.\n"
        << "# path\tartist\talbum\ttitle\tseconds\tsource\n";
    for (const ManifestEntry& e : entries) {
        out << clean(e.path) << '\t' << clean(e.artist) << '\t' << clean(e.album) << '\t'
            << clean(e.title) << '\t' << e.seconds << '\t' << clean(e.source);
        // Left off when unset, so older lines don't change.
        if (e.number > 0 || e.podcast) out << '\t' << (e.number > 0 ? std::to_string(e.number) : "");
        if (e.podcast) out << "\tpodcast";
        out << '\n';
    }
    std::string text = out.str();

    // Unchanged? Then leave the file alone.
    std::ifstream old(manifest_path(library));
    std::string current((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
    if (current == text) return true;

    fs::path tmp = manifest_path(library);
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!(f << text)) return false;
    }
    std::error_code ec;
    fs::rename(tmp, manifest_path(library), ec);  // all-or-nothing replace
    return !ec;
}
