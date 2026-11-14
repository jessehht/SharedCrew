#include "profile_files.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace sc {

namespace {

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
    bool fixedRoles = false;
    bool importSmartCopilot = false;
    std::vector<std::pair<std::string, std::string>> lines;  // keyword, argument
};

bool isEntryKeyword(const std::string& k) {
    return k == "sync" || k == "stream" || k == "master" || k == "back" || k == "override" || k == "command";
}

// Reads a SmartCopilot config (shipped with many aircraft) into SharedCrew entries.
// The file stays in the aircraft folder; we only read it.
bool parseSmartCopilot(const fs::path& path, ParsedFile& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string raw, section;
    while (std::getline(in, raw)) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            size_t end = line.find(']');
            section = end == std::string::npos ? "" : line.substr(1, end - 1);
            continue;
        }
        size_t eq = line.find('=');
        std::string name = trim(eq == std::string::npos ? line : line.substr(0, eq));
        if (name.empty()) continue;
        if (section == "INFO") {
            if (name == "ID" && eq != std::string::npos) out.title = trim(line.substr(eq + 1));
            continue;
        }
        if (section == "SETUP") continue;
        // "name[0]_FIXED_INDEX_" means element 0 only
        size_t fixed = name.find("_FIXED_INDEX_");
        if (fixed != std::string::npos) name = name.substr(0, fixed);
        if (name.find(' ') != std::string::npos) continue;

        std::string key;
        if (section == "TRIGGERS" || section == "TRANSPONDER" || section == "RADIO")
            key = "sync";
        else if (section == "CONTINUED")
            key = "stream";
        else if (section == "SEND_BACK")
            key = "back";
        else if (section == "SLOW")
            key = "master";
        // [WEATHER] is skipped: those are X-Plane 11 weather values. In X-Plane 12 both pilots use
        // real weather, and rewriting the weather every few seconds makes the sim stutter.
        else if (section == "COMMANDS")
            key = "command";
        else if (section == "OVERRIDE")
            key = name.find("/override/") != std::string::npos ? "override" : "stream";
        if (!key.empty()) out.lines.emplace_back(key, name);
    }
    // Aircraft with their own sync channel (ToLiss: smartCopilotSync/.../MCDU) already pass
    // MCDU keys to the other PC themselves. Mirroring an MCDU key command as well makes it
    // arrive twice (one CLR press cleared the message and a letter).
    bool ownMcduSync = false;
    for (const auto& [key, name] : out.lines)
        if (name.find("smartCopilotSync/") != std::string::npos && name.find("/MCDU/") != std::string::npos)
            ownMcduSync = true;
    if (ownMcduSync) {
        out.lines.erase(std::remove_if(out.lines.begin(), out.lines.end(),
                                       [](const std::pair<std::string, std::string>& l) {
                                           return l.first == "command" && l.second.find("MCDU") != std::string::npos;
                                       }),
                        out.lines.end());
    }
    out.noDefault = true;
    out.fixedRoles = true;
    if (out.title.empty()) out.title = "SmartCopilot config";
    return true;
}

bool parseFile(const fs::path& path, ParsedFile& out, const ProfileLog& log) {
    std::ifstream in(path);
    if (!in) return false;
    std::string raw;
    int lineNo = 0;
    const std::string fname = path.filename().string();
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
        } else if (key == "fixedroles") {
            out.fixedRoles = true;
        } else if (key == "import" && lower(arg) == "smartcopilot") {
            out.importSmartCopilot = true;
        } else if (isEntryKeyword(key)) {
            if (arg.empty()) {
                if (log) log(fname + " line " + std::to_string(lineNo) + ": missing name after " + key);
                continue;
            }
            out.lines.emplace_back(key, arg);
        } else if (log) {
            log(fname + " line " + std::to_string(lineNo) + ": unknown keyword \"" + key + "\"");
        }
    }
    return true;
}

}  // namespace

bool readProfileSpec(const std::string& profilesDir, const std::string& acfFile, const std::string& acfPath,
                     ProfileSpec& out, std::string& err, const ProfileLog& log) {
    out = ProfileSpec{};
    const fs::path dir = fs::u8path(profilesDir);
    if (!fs::is_directory(dir)) {
        err = "Profiles folder missing: " + profilesDir;
        return false;
    }

    // The aircraft profile: first file (alphabetical) with a matching "match" line.
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
        if (!parseFile(f, p, log)) continue;
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

    // The aircraft's own SmartCopilot config.
    ParsedFile scp;
    bool haveScp = false;
    if (!acfPath.empty() && (!found || aircraft.importSmartCopilot)) {
        fs::path cfg = fs::u8path(acfPath).parent_path() / "smartcopilot.cfg";
        std::error_code ec;
        if (fs::is_regular_file(cfg, ec) && parseSmartCopilot(cfg, scp)) {
            haveScp = true;
            out.fromSmartCopilot = true;
            if (log) log("using the aircraft's smartcopilot.cfg (" + std::to_string(scp.lines.size()) + " entries)");
        }
    }

    ParsedFile def;
    out.useDefault = found ? !aircraft.noDefault : !haveScp;
    if (out.useDefault && !parseFile(dir / "default.txt", def, log) && log)
        log("default.txt not found, continuing without it");
    out.fixedRoles = (found && aircraft.fixedRoles) || haveScp;

    if (found)
        out.title = aircraft.title.empty() ? out.file : aircraft.title;
    else if (haveScp)
        out.title = scp.title + " (SmartCopilot config)";
    else
        out.title = "Generic (default.txt)";

    // default, then the aircraft file, then SmartCopilot. Each dataref / command only once.
    std::set<std::string> seen;
    uint32_t h = 2166136261u;
    if (out.fixedRoles) h = fnv1a(h, "fixedroles\n");
    auto add = [&](const ParsedFile& p) {
        for (const auto& [key, arg] : p.lines) {
            if (!seen.insert((key == "command" ? "c " : "d ") + arg).second) continue;
            h = fnv1a(h, key + " " + arg + "\n");
            out.entries.emplace_back(key, arg);
        }
    };
    if (out.useDefault) add(def);
    if (found) add(aircraft);
    if (haveScp) add(scp);
    out.hash = h;
    return true;
}

}  // namespace sc
