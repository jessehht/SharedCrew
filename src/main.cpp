// SharedCrew - shared cockpit for X-Plane 12.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "XPLMDataAccess.h"
#include "XPLMMenus.h"
#include "XPLMPlanes.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"
#include "log.h"
#include "net.h"
#include "profile.h"
#include "session.h"
#include "settings.h"
#include "ui.h"

using namespace sc;

namespace {

std::string g_pluginDir;  // .../Resources/plugins/SharedCrew
std::string g_acfFile;
Profile g_profile;
Session g_session(g_profile);
Settings g_settings;
XPLMFlightLoopID g_loop = nullptr;
XPLMMenuID g_menu = nullptr;
XPLMCommandRef g_cmdWindow = nullptr;
XPLMCommandRef g_cmdTake = nullptr;
double g_profileLoadedAt = 0;
float g_resolveTimer = 0;
float g_uiTimer = 0;
bool g_missingLogged = false;

std::string settingsFile() { return g_pluginDir + "/settings.txt"; }

// SmartCopilot-compatible role datarefs. Aircraft like the ToLiss Airbus read these to know
// which PC computes the systems. 0 = not connected, 1 = other PC is master, 2 = this PC is master.
std::vector<XPLMDataRef> g_scpRefs;

int scpRole(void*) {
    if (!g_session.connected()) return 0;
    return g_session.iAmFlying() ? 2 : 1;
}

int scpControl(void*) {
    if (!g_session.connected()) return 0;
    return g_session.iHaveControl() ? 2 : 1;
}

void registerScpDatarefs() {
    for (const char* name : {"scp/api/ismaster", "scp/api/hascontrol_1"}) {
        if (XPLMFindDataRef(name)) {
            log(std::string(name) + " already exists (SmartCopilot installed?), not adding it");
            continue;
        }
        auto reader = std::string(name) == "scp/api/ismaster" ? scpRole : scpControl;
        g_scpRefs.push_back(XPLMRegisterDataAccessor(name, xplmType_Int, 0, reader, nullptr, nullptr, nullptr,
                                                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                     nullptr, nullptr, nullptr));
    }
}

int onProfileCommand(XPLMCommandRef, XPLMCommandPhase phase, void* refcon) {
    g_session.onLocalCommand(static_cast<int>(reinterpret_cast<intptr_t>(refcon)), phase);
    return 1;  // always let X-Plane / the aircraft handle the press as normal
}

void registerCommandHandlers() {
    for (size_t i = 0; i < g_profile.commands.size(); ++i) {
        auto& c = g_profile.commands[i];
        if (c.ref && !c.handlerRegistered) {
            XPLMRegisterCommandHandler(c.ref, onProfileCommand, 1, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            c.handlerRegistered = true;
        }
    }
}

void unregisterCommandHandlers() {
    for (size_t i = 0; i < g_profile.commands.size(); ++i) {
        auto& c = g_profile.commands[i];
        if (c.handlerRegistered) {
            XPLMUnregisterCommandHandler(c.ref, onProfileCommand, 1,
                                         reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            c.handlerRegistered = false;
        }
    }
}

void reloadProfile() {
    if (g_session.phase() != Phase::Idle) g_session.stop("Aircraft changed");
    unregisterCommandHandlers();

    char file[512] = {}, path[1024] = {};
    XPLMGetNthAircraftModel(0, file, path);
    g_acfFile = file;
    std::string full = path, rel;
    for (const char* marker : {"\\Aircraft\\", "/Aircraft/"}) {
        size_t at = full.find(marker);
        if (at != std::string::npos) {
            rel = full.substr(at + 10);
            break;
        }
    }
    g_session.setAircraft(g_acfFile, rel);

    std::string err;
    if (!loadProfile(g_pluginDir + "/profiles", g_acfFile, path, g_profile, err)) {
        log(err);
    }
    registerCommandHandlers();
    g_profileLoadedAt = XPLMGetElapsedTime();
    g_missingLogged = false;
    log("aircraft " + g_acfFile + " -> profile \"" + g_profile.title + "\" (" +
        std::to_string(g_profile.drefs.size()) + " datarefs, " + std::to_string(g_profile.commands.size()) +
        " commands)");
}

UiStatus buildStatus() {
    UiStatus s;
    s.status = g_session.status();
    s.busy = g_session.phase() != Phase::Idle;
    s.connected = g_session.connected();
    s.iAmFlying = g_session.iAmFlying();
    if (g_session.role() == Role::Host) s.role = "Host (this PC computes the aircraft)";
    if (g_session.role() == Role::Guest) s.role = "Guest";
    if (s.connected) s.controls = s.iAmFlying ? "You are flying" : "Other pilot is flying";
    if (s.connected && g_session.fixedRoles())
        s.controls = g_session.iHaveControl() ? "You: thrust + tiller (2 sticks)"
                                              : "Other: thrust + tiller (2 sticks)";
    s.canTakeControls = s.connected && !g_session.iHaveControl();

    s.profile = g_profile.title;
    int missing = g_profile.unresolvedCount();
    if (missing > 0) s.profile += " (" + std::to_string(missing) + " items not found)";

    s.code = g_session.myCode();
    s.canLetIn = g_session.waitingForGuest();
    if (g_session.role() == Role::Host) s.codeLabel = "Session code";
    if (g_session.role() == Role::Guest) {
        s.codeLabel = "Reply code";
        if (!g_session.needsReplyCode()) s.code.clear();  // only needed if the host can't be reached
    }

    if (s.connected) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s, ping %.0f ms", g_session.peerText().c_str(), g_session.pingMs());
        s.link = buf;
    }
    return s;
}

float flightLoop(float elapsed, float, int, void*) {
    // Aircraft plugins create their datarefs a little after the aircraft loads, so keep looking.
    g_resolveTimer += elapsed;
    if (g_resolveTimer >= 1.0f && g_profile.unresolvedCount() > 0) {
        g_resolveTimer = 0;
        bool logNow = !g_missingLogged && XPLMGetElapsedTime() - g_profileLoadedAt > 20.0;
        g_profile.resolvePending(logNow);
        if (logNow) g_missingLogged = true;
        registerCommandHandlers();
    }

    g_session.update();

    g_uiTimer += elapsed;
    if (g_uiTimer >= 0.25f && uiVisible()) {
        g_uiTimer = 0;
        uiUpdate(buildStatus());
    }
    return -1.0f;  // every frame
}

void onHost(int port) {
    g_settings.port = port;
    g_settings.save(settingsFile());
    g_session.startHost(static_cast<uint16_t>(port));
}

void onJoin(const std::string& host, int port) {
    if (host.empty()) {
        uiUpdate(buildStatus());
        return;
    }
    g_settings.host = host;
    g_settings.port = port;
    g_settings.save(settingsFile());
    g_session.join(host, static_cast<uint16_t>(port));
}

void menuHandler(void*, void* item) {
    intptr_t id = reinterpret_cast<intptr_t>(item);
    if (id == 1) uiToggle();
    if (id == 2) g_session.takeControls();
}

int commandHandler(XPLMCommandRef cmd, XPLMCommandPhase phase, void*) {
    if (phase != xplm_CommandBegin) return 0;
    if (cmd == g_cmdWindow) uiToggle();
    if (cmd == g_cmdTake) g_session.takeControls();
    return 0;
}

}  // namespace

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    std::strcpy(outName, "SharedCrew");
    std::strcpy(outSig, "org.sharedcrew.plugin");
    std::strcpy(outDesc, "Shared cockpit: fly one aircraft together from two PCs.");

    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);

    // Plugin folder = two levels above .../SharedCrew/win_x64/SharedCrew.xpl
    char path[1024] = {};
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, path, nullptr, nullptr);
    std::string p = path;
    for (int i = 0; i < 2; ++i) {
        size_t cut = p.find_last_of("/\\");
        if (cut != std::string::npos) p = p.substr(0, cut);
    }
    g_pluginDir = p;

    if (!netInit()) log("network start failed");
    g_settings.load(settingsFile());
    if (g_settings.server == "off")
        g_session.setServer("");
    else
        g_session.setServer(g_settings.server.empty() ? kDefaultServer : g_settings.server);

    UiActions actions;
    actions.host = onHost;
    actions.join = onJoin;
    actions.letIn = [](const std::string& code) { g_session.letIn(code); };
    actions.disconnect = [] { g_session.stop("Disconnected"); };
    actions.takeControls = [] { g_session.takeControls(); };
    uiInit(actions, g_settings.host, g_settings.port);

    int item = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "SharedCrew", nullptr, 0);
    g_menu = XPLMCreateMenu("SharedCrew", XPLMFindPluginsMenu(), item, menuHandler, nullptr);
    XPLMAppendMenuItem(g_menu, "Show / hide window", reinterpret_cast<void*>(1), 0);
    XPLMAppendMenuItem(g_menu, "Take controls", reinterpret_cast<void*>(2), 0);

    g_cmdWindow = XPLMCreateCommand("sharedcrew/toggle_window", "SharedCrew: show or hide the window");
    g_cmdTake = XPLMCreateCommand("sharedcrew/take_controls", "SharedCrew: take the controls");
    XPLMRegisterCommandHandler(g_cmdWindow, commandHandler, 1, nullptr);
    XPLMRegisterCommandHandler(g_cmdTake, commandHandler, 1, nullptr);

    registerScpDatarefs();

    XPLMCreateFlightLoop_t loop{};
    loop.structSize = sizeof(loop);
    loop.phase = xplm_FlightLoop_Phase_BeforeFlightModel;
    loop.callbackFunc = flightLoop;
    g_loop = XPLMCreateFlightLoop(&loop);

    log("started, version " SHAREDCREW_VERSION ", folder " + g_pluginDir);
    return 1;
}

PLUGIN_API void XPluginStop() {
    g_session.stop("", false);
    unregisterCommandHandlers();
    XPLMUnregisterCommandHandler(g_cmdWindow, commandHandler, 1, nullptr);
    XPLMUnregisterCommandHandler(g_cmdTake, commandHandler, 1, nullptr);
    if (g_loop) XPLMDestroyFlightLoop(g_loop);
    for (XPLMDataRef r : g_scpRefs) XPLMUnregisterDataAccessor(r);
    g_scpRefs.clear();
    uiDestroy();
    if (g_menu) XPLMDestroyMenu(g_menu);
    netShutdown();
}

PLUGIN_API int XPluginEnable() {
    reloadProfile();
    XPLMScheduleFlightLoop(g_loop, -1.0f, 1);
    return 1;
}

PLUGIN_API void XPluginDisable() {
    g_session.stop("Plugin disabled");
    XPLMScheduleFlightLoop(g_loop, 0, 1);
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int msg, void* param) {
    if (msg == XPLM_MSG_PLANE_LOADED && reinterpret_cast<intptr_t>(param) == 0) {
        reloadProfile();
    }
}
