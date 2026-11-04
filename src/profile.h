#pragma once
// Aircraft profiles: text files that list what to keep in sync for an aircraft.
#include <cstdint>
#include <string>
#include <vector>

#include "XPLMDataAccess.h"
#include "XPLMUtilities.h"

namespace sc {

enum class EntryKind {
    Sync,    // two-way: whoever changes it last wins (switches, knobs, radios)
    Stream,  // one-way from the pilot flying, every frame (yoke, throttle, fuel)
};

struct DrefEntry {
    EntryKind kind = EntryKind::Sync;
    std::string name;
    int index = -1;  // -1 = whole value/array, otherwise a single array element

    XPLMDataRef ref = nullptr;
    XPLMDataTypeID type = 0;
    bool writable = false;
    int count = 0;  // number of values (1 for scalars)

    // Sync state
    std::vector<double> lastKnown;
    uint32_t version = 0;
    uint8_t origin = 0;
    bool dirty = false;
    bool warnedReadOnly = false;

    bool resolved() const { return ref != nullptr && count > 0; }
    bool resolve();
    void read(std::vector<double>& out) const;
    void write(const std::vector<double>& v) const;
    std::string label() const;
};

struct CommandEntry {
    std::string name;
    XPLMCommandRef ref = nullptr;
    bool handlerRegistered = false;
    int suppressBegin = 0;  // remote presses we are replaying, so we don't send them back
    int suppressEnd = 0;
};

struct Profile {
    std::string title;       // e.g. "Cessna 172 (Laminar)"
    std::string file;        // matched profile file, empty if only default
    bool useDefault = true;
    std::vector<DrefEntry> drefs;
    std::vector<CommandEntry> commands;
    uint32_t hash = 0;  // both pilots must have the same hash

    int unresolvedCount() const;
    // Try to find datarefs/commands that did not exist yet (aircraft plugins load late).
    void resolvePending(bool logMissing);
};

// Picks the profile whose "match" lines fit the .acf file name, plus default.txt.
bool loadProfile(const std::string& profilesDir, const std::string& acfFile, Profile& out, std::string& err);

}  // namespace sc
