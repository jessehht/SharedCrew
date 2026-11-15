#pragma once
// Aircraft profiles: text files that list what to keep in sync for an aircraft.
#include <cstdint>
#include <string>
#include <vector>

#include "XPLMDataAccess.h"
#include "XPLMUtilities.h"

namespace sc {

enum class EntryKind {
    Sync,        // two-way: whoever changes it last wins (switches, knobs, radios)
    Stream,      // one-way from the pilot flying, every frame (yoke, throttle, fuel)
    MasterSync,  // one-way from the pilot flying, only when it changes (loading, weather)
    Back,        // one-way to the pilot flying, every frame (the other pilot's stick, tiller)
    Override,    // set to 1 on the other pilot's PC while connected
    ControlOverride,  // set to 1 on the PC of the pilot who does not have the controls
};

struct DrefEntry {
    EntryKind kind = EntryKind::Sync;
    std::string name;
    int index = -1;  // -1 = whole value/array, otherwise a single array element
    // Short-lived values (MCDU key presses, aircraft sync channels): checked every frame
    // and delivered reliably and in order, so no key press is ever lost.
    bool priority = false;
    // Receiver ignores changes smaller than this (0 = off). Used for clocks.
    double applyTolerance = 0;
    bool failed = false;  // exists but can't be used (wrong type): don't try again

    // Some aircraft files list a command among the datarefs (e.g. transponder IDENT).
    // If no dataref has this name but a command does, the press is mirrored instead.
    XPLMCommandRef cmd = nullptr;
    bool handlerRegistered = false;
    int suppressBegin = 0;
    int suppressEnd = 0;

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
    // Text / byte value (no number types at all)
    bool isData() const {
        return (type & xplmType_Data) &&
               !(type & (xplmType_Double | xplmType_Float | xplmType_Int | xplmType_FloatArray | xplmType_IntArray));
    }
    bool resolve();
    void read(std::vector<double>& out) const;
    void write(const std::vector<double>& v) const;
    std::string label() const;
};

// Command handler refcon flag: the command is listed among the datarefs (index into drefs).
constexpr int kDrefCommand = 0x10000;

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
    // Fixed roles: one PC always computes the aircraft and both pilots fly together
    // (like the real Airbus: two sidesticks). Used for SmartCopilot-style aircraft.
    bool fixedRoles = false;
    bool fromSmartCopilot = false;
    std::vector<DrefEntry> drefs;
    std::vector<CommandEntry> commands;
    uint32_t hash = 0;  // both pilots must have the same hash

    int unresolvedCount() const;
    // Try to find datarefs/commands that did not exist yet (aircraft plugins load late).
    void resolvePending(bool logMissing);
};

// Picks the profile whose "match" lines fit the .acf file name, plus default.txt.
// If none matches and the aircraft folder has a smartcopilot.cfg, that file is used.
bool loadProfile(const std::string& profilesDir, const std::string& acfFile, const std::string& acfPath,
                 Profile& out, std::string& err);

}  // namespace sc
