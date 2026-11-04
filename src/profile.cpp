#include "profile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "log.h"

namespace fs = std::filesystem;

namespace sc {

namespace {

constexpr int kMaxArray = 64;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

uint32_t fnv1a(uint32_t h, const std::string& s) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

struct ParsedFile {
    std::string title;
    std::vector<std::string> matches;
    bool noDefault = false;
    std::vector<std::pair<std::string, std::string>> lines;  // keyword, argument
};

bool parseFile(const fs::path& path, ParsedFile& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string raw;
    int lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        size_t hash = raw.find('#');
        if (hash != std::string::npos) raw = raw.substr(0, hash);
        std::string line = trim(raw);
        if (line.empty()) continue;

        std::istringstream ss(line);
        std::string key, arg;
        ss >> key;
        std::getline(ss, arg);
        key = lower(key);
        arg = trim(arg);

        if (key == "title") {
            out.title = arg;
        } else if (key == "match") {
            if (!arg.empty()) out.matches.push_back(lower(arg));
        } else if (key == "nodefault") {
            out.noDefault = true;
        } else if (key == "sync" || key == "stream" || key == "command") {
            if (arg.empty()) {
                log(path.filename().string() + " line " + std::to_string(lineNo) + ": missing name after " + key);
                continue;
            }
            out.lines.emplace_back(key, arg);
        } else {
            log(path.filename().string() + " line " + std::to_string(lineNo) + ": unknown keyword \"" + key + "\"");
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// DrefEntry

bool DrefEntry::resolve() {
    if (resolved()) return true;
    ref = XPLMFindDataRef(name.c_str());
    if (!ref) return false;
    type = XPLMGetDataRefTypes(ref);
    writable = XPLMCanWriteDataRef(ref) != 0;

    const bool isFloatArray = (type & xplmType_FloatArray) != 0;
    const bool isIntArray = (type & xplmType_IntArray) != 0;
    const bool isScalar = (type & (xplmType_Double | xplmType_Float | xplmType_Int)) != 0;

    if (index >= 0) {
        if (!isFloatArray && !isIntArray) {
            log("\"" + label() + "\" is not an array, ignoring it");
            ref = nullptr;
            return false;
        }
        count = 1;
    } else if (isScalar) {
        count = 1;
    } else if (isFloatArray) {
        count = std::min(XPLMGetDatavf(ref, nullptr, 0, 0), kMaxArray);
    } else if (isIntArray) {
        count = std::min(XPLMGetDatavi(ref, nullptr, 0, 0), kMaxArray);
    } else {
        log("\"" + name + "\" has a type SharedCrew can't sync yet, ignoring it");
        ref = nullptr;
        return false;
    }
    if (count <= 0) {
        ref = nullptr;
        return false;
    }
    read(lastKnown);
    return true;
}

void DrefEntry::read(std::vector<double>& out) const {
    out.assign(count, 0.0);
    if (!resolved()) return;

    if (index >= 0) {
        if (type & xplmType_FloatArray) {
            float f = 0;
            if (XPLMGetDatavf(ref, &f, index, 1) == 1) out[0] = f;
        } else {
            int i = 0;
            if (XPLMGetDatavi(ref, &i, index, 1) == 1) out[0] = i;
        }
        return;
    }
    if (type & xplmType_Double) {
        out[0] = XPLMGetDatad(ref);
    } else if (type & xplmType_Float) {
        out[0] = XPLMGetDataf(ref);
    } else if (type & xplmType_Int) {
        out[0] = XPLMGetDatai(ref);
    } else if (type & xplmType_FloatArray) {
        float tmp[kMaxArray];
        int n = XPLMGetDatavf(ref, tmp, 0, count);
        for (int i = 0; i < n && i < count; ++i) out[i] = tmp[i];
    } else if (type & xplmType_IntArray) {
        int tmp[kMaxArray];
        int n = XPLMGetDatavi(ref, tmp, 0, count);
        for (int i = 0; i < n && i < count; ++i) out[i] = tmp[i];
    }
}

void DrefEntry::write(const std::vector<double>& v) const {
    if (!resolved() || static_cast<int>(v.size()) != count) return;

    if (index >= 0) {
        if (type & xplmType_FloatArray) {
            float f = static_cast<float>(v[0]);
            XPLMSetDatavf(ref, &f, index, 1);
        } else {
            int i = static_cast<int>(std::lround(v[0]));
            XPLMSetDatavi(ref, &i, index, 1);
        }
        return;
    }
    if (type & xplmType_Double) {
        XPLMSetDatad(ref, v[0]);
    } else if (type & xplmType_Float) {
        XPLMSetDataf(ref, static_cast<float>(v[0]));
    } else if (type & xplmType_Int) {
        XPLMSetDatai(ref, static_cast<int>(std::lround(v[0])));
    } else if (type & xplmType_FloatArray) {
        float tmp[kMaxArray];
        for (int i = 0; i < count; ++i) tmp[i] = static_cast<float>(v[i]);
        XPLMSetDatavf(ref, tmp, 0, count);
    } else if (type & xplmType_IntArray) {
        int tmp[kMaxArray];
        for (int i = 0; i < count; ++i) tmp[i] = static_cast<int>(std::lround(v[i]));
        XPLMSetDatavi(ref, tmp, 0, count);
    }
}

std::string DrefEntry::label() const {
    return index >= 0 ? name + "[" + std::to_string(index) + "]" : name;
}

// ---------------------------------------------------------------------------
// Profile

int Profile::unresolvedCount() const {
    int n = 0;
    for (const auto& d : drefs)
        if (!d.resolved()) ++n;
    for (const auto& c : commands)
        if (!c.ref) ++n;
    return n;
}

void Profile::resolvePending(bool logMissing) {
    for (auto& d : drefs) {
        if (!d.resolved() && !d.resolve() && logMissing) log("dataref not found: " + d.label());
    }
    for (auto& c : commands) {
        if (!c.ref) {
            c.ref = XPLMFindCommand(c.name.c_str());
            if (!c.ref && logMissing) log("command not found: " + c.name);
        }
    }
}

bool loadProfile(const std::string& profilesDir, const std::string& acfFile, Profile& out, std::string& err) {
    out = Profile{};
    const fs::path dir = fs::u8path(profilesDir);
    if (!fs::is_directory(dir)) {
        err = "Profiles folder missing: " + profilesDir;
        return false;
    }

    // Find the aircraft profile: first file (alphabetical) with a matching "match" line.
    const std::string acf = lower(acfFile);
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file() && lower(e.path().extension().string()) == ".txt" &&
            lower(e.path().filename().string()) != "default.txt")
            files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());

    ParsedFile aircraft;
    bool found = false;
    for (const auto& f : files) {
        ParsedFile p;
        if (!parseFile(f, p)) continue;
        for (const auto& m : p.matches) {
            if (acf.find(m) != std::string::npos) {
                aircraft = std::move(p);
                out.file = f.filename().string();
                found = true;
                break;
            }
        }
        if (found) break;
    }

    ParsedFile def;
    out.useDefault = !(found && aircraft.noDefault);
    if (out.useDefault && !parseFile(dir / "default.txt", def)) {
        log("default.txt not found, continuing without it");
    }

    out.title = found ? (aircraft.title.empty() ? out.file : aircraft.title) : "Generic (default.txt)";

    // Build entries: default first, then the aircraft file. Skip duplicates.
    std::set<std::string> seen;
    uint32_t h = 2166136261u;
    auto addLines = [&](const ParsedFile& p) {
        for (const auto& [key, arg] : p.lines) {
            if (!seen.insert(key + " " + arg).second) continue;
            h = fnv1a(h, key + " " + arg + "\n");
            if (key == "command") {
                CommandEntry c;
                c.name = arg;
                out.commands.push_back(c);
                continue;
            }
            DrefEntry d;
            d.kind = key == "stream" ? EntryKind::Stream : EntryKind::Sync;
            d.name = arg;
            size_t br = arg.find('[');
            if (br != std::string::npos && arg.back() == ']') {
                d.name = trim(arg.substr(0, br));
                d.index = std::atoi(arg.substr(br + 1).c_str());
            }
            out.drefs.push_back(d);
        }
    };
    if (out.useDefault) addLines(def);
    if (found) addLines(aircraft);
    out.hash = h;

    out.resolvePending(false);
    return true;
}

}  // namespace sc
