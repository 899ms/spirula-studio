#include "app/gui/ReconStamp.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace gui {

namespace {

// The flag `args[i]` belongs to, which is the flag itself or the nearest one
// before it. `args` is a command line, so a bare token is a flag's value.
std::string flag_at(const std::vector<std::string>& args, size_t i) {
    for (size_t j = i + 1; j-- > 0;)
        if (args[j].rfind("--", 0) == 0) return args[j];
    return i < args.size() ? args[i] : std::string();
}

// The file is one token per line and a value can hold newlines -- the manifest
// travels as its own text. Dropping such a line would shift every token after
// it, so the stamp could never match the run that wrote it again.
std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\')      out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else                out += c;
    }
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
        const char c = s[++i];
        out += c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    }
    return out;
}

}  // namespace

ReconStamp read_recon_stamp(const std::string& workspace, const char* file) {
    ReconStamp st;
    if (workspace.empty()) return st;
    std::ifstream f(fs::path(workspace) / file, std::ios::binary);
    if (!f) return st;
    std::string s;
    bool first = true;
    while (std::getline(f, s)) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (first) {
            st.engine = unescape(s);
            first = false;
            continue;
        }
        st.args.push_back(unescape(s));
    }
    st.present = !first;
    return st;
}

void write_recon_stamp(const std::string& workspace, const ReconStamp& st,
                       const char* file) {
    if (workspace.empty()) return;
    std::ofstream f(fs::path(workspace) / file,
                    std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << escape(st.engine) << '\n';
    for (const std::string& a : st.args) f << escape(a) << '\n';
}

std::string recon_stamp_change(const ReconStamp& prior, const ReconStamp& now) {
    if (!prior.present) return "";
    if (prior.engine != now.engine) return now.engine;
    const size_t n = std::min(prior.args.size(), now.args.size());
    for (size_t i = 0; i < n; i++)
        if (prior.args[i] != now.args[i]) return flag_at(now.args, i);
    if (prior.args.size() == now.args.size()) return "";
    const std::vector<std::string>& longer =
        prior.args.size() > now.args.size() ? prior.args : now.args;
    return flag_at(longer, n);
}

}  // namespace gui
