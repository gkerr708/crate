#include "progress.hpp"

#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

static fs::path progress_path(const fs::path& library) { return library / "progress.tsv"; }

ProgressMap read_progress(const fs::path& library) {
    ProgressMap out;
    std::ifstream in(progress_path(library));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string col;
        while (std::getline(ss, col, '\t')) cols.push_back(col);
        cols.resize(3);  // missing trailing columns become ""
        if (cols[0].empty()) continue;

        out[cols[0]] = {std::atoi(cols[1].c_str()), cols[2] == "1"};
    }
    return out;
}

bool write_progress(const fs::path& library, const ProgressMap& progress) {
    std::ostringstream out;
    out << "# crate listening progress for long tracks, tab-separated.\n"
        << "# path\tseconds\tdone\n";
    for (const auto& [path, p] : progress) {
        if (p.seconds <= 0 && !p.done) continue;  // nothing worth remembering
        out << path << '\t' << p.seconds << '\t' << (p.done ? 1 : 0) << '\n';
    }
    std::string text = out.str();
    if (!fs::exists(progress_path(library)) && progress.empty()) return true;  // don't create an empty file

    // Unchanged? Then leave the file alone.
    std::ifstream old(progress_path(library));
    std::string current((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
    if (current == text) return true;

    fs::path tmp = progress_path(library);
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!(f << text)) return false;
    }
    std::error_code ec;
    fs::rename(tmp, progress_path(library), ec);  // all-or-nothing replace
    return !ec;
}
