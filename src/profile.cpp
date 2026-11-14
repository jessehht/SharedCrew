#include "profile.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "log.h"
#include "profile_files.h"

namespace sc {

namespace {

constexpr int kMaxArray = 64;
constexpr int kMaxData = 128;  // text values (byte arrays), e.g. MCDU scratchpad

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

}  // namespace

// ---------------------------------------------------------------------------
// DrefEntry

bool DrefEntry::resolve() {
    if (resolved()) return true;
    if (failed) return false;
    ref = XPLMFindDataRef(name.c_str());
    if (!ref) return false;
    type = XPLMGetDataRefTypes(ref);
    writable = XPLMCanWriteDataRef(ref) != 0;

    const bool isFloatArray = (type & xplmType_FloatArray) != 0;
    const bool isIntArray = (type & xplmType_IntArray) != 0;
    const bool isScalar = (type & (xplmType_Double | xplmType_Float | xplmType_Int)) != 0;
    const bool isData = (type & xplmType_Data) != 0;

    if (isData && !isScalar && !isFloatArray && !isIntArray) {
        // Text / byte values (e.g. an MCDU scratchpad). Whole value only.
        count = std::min(XPLMGetDatab(ref, nullptr, 0, 0), kMaxData);
    } else if (index >= 0) {
        if (!isFloatArray && !isIntArray) {
            log("\"" + label() + "\" is not an array, ignoring it");
            ref = nullptr;
            failed = true;
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
        failed = true;
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

    if (isData()) {
        uint8_t tmp[kMaxData] = {};
        int n = XPLMGetDatab(ref, tmp, 0, count);
        for (int i = 0; i < n && i < count; ++i) out[i] = tmp[i];
        return;
    }
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

    if (isData()) {
        uint8_t tmp[kMaxData] = {};
        for (int i = 0; i < count; ++i) tmp[i] = static_cast<uint8_t>(std::lround(v[i]));
        XPLMSetDatab(ref, tmp, 0, count);
        return;
    }
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
        if (!d.resolved() && !d.resolve() && logMissing && !d.failed) log("dataref not found: " + d.label());
    }
    for (auto& c : commands) {
        if (!c.ref) {
            c.ref = XPLMFindCommand(c.name.c_str());
            if (!c.ref && logMissing) log("command not found: " + c.name);
        }
    }
}

bool loadProfile(const std::string& profilesDir, const std::string& acfFile, const std::string& acfPath,
                 Profile& out, std::string& err) {
    out = Profile{};
    ProfileSpec spec;
    if (!readProfileSpec(profilesDir, acfFile, acfPath, spec, err, [](const std::string& s) { log(s); }))
        return false;

    out.title = spec.title;
    out.file = spec.file;
    out.useDefault = spec.useDefault;
    out.fixedRoles = spec.fixedRoles;
    out.fromSmartCopilot = spec.fromSmartCopilot;
    out.hash = spec.hash;

    for (const auto& [key, arg] : spec.entries) {
        if (key == "command") {
            CommandEntry c;
            c.name = arg;
            out.commands.push_back(c);
            continue;
        }
        DrefEntry d;
        d.kind = key == "stream"     ? EntryKind::Stream
                 : key == "master"   ? EntryKind::MasterSync
                 : key == "back"     ? EntryKind::Back
                 : key == "override" ? EntryKind::Override
                                     : EntryKind::Sync;
        d.name = arg;
        size_t br = arg.find('[');
        if (br != std::string::npos && arg.back() == ']') {
            d.name = trim(arg.substr(0, br));
            d.index = std::atoi(arg.substr(br + 1).c_str());
        }
        d.priority = d.kind == EntryKind::Sync &&
                     (d.name.find("smartCopilotSync/") != std::string::npos || d.name.find("MCDU") != std::string::npos);
        if (d.name.rfind("sim/time/", 0) == 0) d.applyTolerance = 2.0;
        out.drefs.push_back(d);
    }
    out.resolvePending(false);
    return true;
}

}  // namespace sc