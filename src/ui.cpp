#include "ui.h"

#include <cstdlib>

#include "XPLMDisplay.h"
#include "XPStandardWidgets.h"
#include "XPWidgets.h"

namespace sc {

namespace {

constexpr int kWidth = 400;
constexpr int kHeight = 310;
constexpr int kLabelW = 95;

UiActions g_actions;
XPWidgetID g_window = nullptr;
XPWidgetID g_status, g_role, g_controls, g_profile, g_link;
XPWidgetID g_hostField, g_portField;
XPWidgetID g_btnHost, g_btnJoin, g_btnStop, g_btnTake;
std::string g_lastHost;
int g_lastPort = 51000;

int handler(XPWidgetMessage msg, XPWidgetID, intptr_t p1, intptr_t) {
    if (msg == xpMessage_CloseButtonPushed) {
        XPHideWidget(g_window);
        return 1;
    }
    if (msg != xpMsg_PushButtonPressed) return 0;
    XPWidgetID btn = reinterpret_cast<XPWidgetID>(p1);
    if (btn == g_btnHost && g_actions.host) g_actions.host(uiPort());
    if (btn == g_btnJoin && g_actions.join) g_actions.join(uiHostText(), uiPort());
    if (btn == g_btnStop && g_actions.disconnect) g_actions.disconnect();
    if (btn == g_btnTake && g_actions.takeControls) g_actions.takeControls();
    return 1;
}

XPWidgetID caption(int l, int t, int r, const char* text) {
    return XPCreateWidget(l, t, r, t - 18, 1, text, 0, g_window, xpWidgetClass_Caption);
}

XPWidgetID row(int l, int r, int t, const char* label) {
    caption(l, t, l + kLabelW, label);
    return caption(l + kLabelW, t, r, "");
}

XPWidgetID button(int l, int t, int r, const char* text) {
    XPWidgetID b = XPCreateWidget(l, t, r, t - 22, 1, text, 0, g_window, xpWidgetClass_Button);
    XPSetWidgetProperty(b, xpProperty_ButtonType, xpPushButton);
    return b;
}

XPWidgetID field(int l, int t, int r, const std::string& text) {
    XPWidgetID f = XPCreateWidget(l, t, r, t - 20, 1, text.c_str(), 0, g_window, xpWidgetClass_TextField);
    XPSetWidgetProperty(f, xpProperty_TextFieldType, xpTextEntryField);
    XPSetWidgetProperty(f, xpProperty_Enabled, 1);
    return f;
}

void create() {
    int sl, st, sr, sb;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    const int L = sl + 80;
    const int T = st - 120;
    const int R = L + kWidth;
    const int B = T - kHeight;

    g_window = XPCreateWidget(L, T, R, B, 0, "SharedCrew " SHAREDCREW_VERSION, 1, nullptr, xpWidgetClass_MainWindow);
    XPSetWidgetProperty(g_window, xpProperty_MainWindowHasCloseBoxes, 1);
    XPAddWidgetCallback(g_window, handler);

    const int l = L + 15, r = R - 15;
    int y = T - 28;
    g_status = row(l, r, y, "Status");
    y -= 20;
    g_role = row(l, r, y, "Role");
    y -= 20;
    g_controls = row(l, r, y, "Controls");
    y -= 20;
    g_profile = row(l, r, y, "Aircraft");
    y -= 20;
    g_link = row(l, r, y, "Link");

    y -= 34;
    caption(l, y, l + kLabelW, "Host address");
    g_hostField = field(l + kLabelW, y, r, g_lastHost);
    y -= 26;
    caption(l, y, l + kLabelW, "Port (UDP)");
    g_portField = field(l + kLabelW, y, l + kLabelW + 80, std::to_string(g_lastPort));

    y -= 36;
    const int mid = (l + r) / 2;
    g_btnHost = button(l, y, mid - 5, "Host a session");
    g_btnJoin = button(mid + 5, y, r, "Join host");
    y -= 30;
    g_btnTake = button(l, y, mid - 5, "Take controls");
    g_btnStop = button(mid + 5, y, r, "Disconnect");

    y -= 34;
    caption(l, y, r, "The host needs this UDP port open in the router,");
    caption(l, y - 16, r, "or both pilots use the same VPN (ZeroTier, Radmin).");
}

void setText(XPWidgetID w, const std::string& s) {
    char cur[512] = {};
    XPGetWidgetDescriptor(w, cur, sizeof(cur) - 1);
    if (s != cur) XPSetWidgetDescriptor(w, s.c_str());
}

void enable(XPWidgetID w, bool on) { XPSetWidgetProperty(w, xpProperty_Enabled, on ? 1 : 0); }

}  // namespace

void uiInit(const UiActions& actions, const std::string& host, int port) {
    g_actions = actions;
    g_lastHost = host;
    g_lastPort = port;
}

void uiShow() {
    if (!g_window) create();
    XPShowWidget(g_window);
    XPBringRootWidgetToFront(g_window);
}

void uiToggle() {
    if (uiVisible())
        XPHideWidget(g_window);
    else
        uiShow();
}

bool uiVisible() { return g_window && XPIsWidgetVisible(g_window); }

void uiUpdate(const UiStatus& s) {
    if (!g_window) return;
    setText(g_status, s.status);
    setText(g_role, s.role.empty() ? "-" : s.role);
    setText(g_controls, s.controls.empty() ? "-" : s.controls);
    setText(g_profile, s.profile);
    setText(g_link, s.link.empty() ? "-" : s.link);
    enable(g_btnHost, !s.busy);
    enable(g_btnJoin, !s.busy);
    enable(g_hostField, !s.busy);
    enable(g_portField, !s.busy);
    enable(g_btnStop, s.busy);
    enable(g_btnTake, s.connected && !s.iAmFlying);
}

void uiDestroy() {
    if (g_window) XPDestroyWidget(g_window, 1);
    g_window = nullptr;
}

std::string uiHostText() {
    if (!g_window) return g_lastHost;
    char buf[256] = {};
    XPGetWidgetDescriptor(g_hostField, buf, sizeof(buf) - 1);
    std::string s = buf;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    g_lastHost = s;
    return s;
}

int uiPort() {
    if (!g_window) return g_lastPort;
    char buf[32] = {};
    XPGetWidgetDescriptor(g_portField, buf, sizeof(buf) - 1);
    int p = std::atoi(buf);
    if (p > 0 && p < 65536) g_lastPort = p;
    return g_lastPort;
}

}  // namespace sc
