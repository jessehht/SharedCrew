#pragma once
// Reading profile files (no X-Plane code here, so the test peer uses the exact same rules).
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace sc {

struct ProfileSpec {
    std::string title;
    std::string file;  // matched SharedCrew profile file, empty if none
    bool useDefault = true;
    bool fixedRoles = false;
    bool fromSmartCopilot = false;
    // keyword (sync, stream, master, back, override, command) + name, in order, no duplicates
    std::vector<std::pair<std::string, std::string>> entries;
    uint32_t hash = 0;  // both pilots must have the same hash
};

using ProfileLog = std::function<void(const std::string&)>;

// profiles/default.txt + the profile whose "match" fits the .acf file name.
// If no profile matches (or it says "import smartcopilot"), the aircraft's own
// smartcopilot.cfg next to the .acf is read.
bool readProfileSpec(const std::string& profilesDir, const std::string& acfFile, const std::string& acfPath,
                     ProfileSpec& out, std::string& err, const ProfileLog& log);

}  // namespace sc
