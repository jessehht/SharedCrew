#include "ui.h"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

#include "XPLMDisplay.h"
#include "XPStandardWidgets.h"
#include "XPWidgets.h"

namespace sc {

namespace {

constexpr int kWidth = 420;
constexpr int kHeight = 360;
constexpr int kLabelW = 100;
constexpr int kSmallBtn = 70;

UiActions g_actions;
XPWidgetID g_window = nullptr;
XPWidgetID g_status, g_role, g_controls, g_profile, g_link;
XPWidgetID g_codeLabel, g_code, g_btnCopy;
XPWidgetID g_inputField, g_btnPaste, g_portField;
XPWidgetID g_btnHost, g_btnJoin, g_btnStop, g_btnTake;
XPWidgetID g_hint1, g_hint2;
std::string g_lastHost;
std::string g_currentCode;
bool g_letInMode = false;
int g_lastPort = 51000;

// --- clipboard (session codes are long, so copy/paste matters)

void setClipboard(const std::string& text) {
#ifdef _WIN32
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (mem) {
        std::memcpy(GlobalLock(mem), text.c_str(), text.size() + 1);
        GlobalUnlock(mem);
        SetClipboardData(CF_TEXT, mem);
    }
    CloseClipboard();
#else
    (void)text;
#endif
}

std::string getClipboard() {
    std::string out;
#ifdef _WIN32
    if (!OpenClipboard(nullptr)) return out;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h) {
        const char* p = static_cast<const char*>(GlobalLock(h));
        if (p) out = p;
        GlobalUnlock(h);
    }
    CloseClipboard();
#endif
    // Keep it on one line and trim
    for (auto& c : out)
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out.size() > 200) out.resize(200);
    return out;
}

// --- widgets

int handler(XPWidgetMessage msg, XPWidgetID, intptr_t p1, intptr_t) {
    if (msg == xpMessage_CloseButtonPushed) {
        XPHideWidget(g_window);
        return 1;
    }
    if (msg != xpMsg_PushButtonPressed) return 0;
    XPWidgetID btn = reinterpret_cast<XPWidgetID>(p1);
    if (btn == g_btnHost && g_actions.host) g_actions.host(uiPort());
    if (btn == g_btnJoin) {
        if (g_letInMode) {
            if (g_actions.letIn) g_actions.letIn(uiHostText());
        } else if (g_actions.join) {
            g_actions.join(uiHostText(), uiPort());
        }
    }
    if (btn == g_btnStop && g_actions.disconnect) g_actions.disconnect();
    if (btn == g_btnTake && g_actions.takeControls) g_actions.takeControls();
    if (btn == g_btnCopy && !g_currentCode.empty()) setClipboard(g_currentCode);
    if (btn == g_btnPaste) XPSetWidgetDescriptor(g_inputField, getClipboard().c_str());
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
    g_codeLabel = caption(l, y, l + kLabelW, "Your code");
    g_code = caption(l + kLabelW, y, r - kSmallBtn - 8, "-");
    g_btnCopy = button(r - kSmallBtn, y + 2, r, "Copy");

    y -= 28;
    caption(l, y, l + kLabelW, "Code or IP");
    g_inputField = field(l + kLabelW, y, r - kSmallBtn - 8, g_lastHost);
    g_btnPaste = button(r - kSmallBtn, y + 1, r, "Paste");

    y -= 26;
    caption(l, y, l + kLabelW, "Port (UDP)");
    g_portField = field(l + kLabelW, y, l + kLabelW + 80, std::to_string(g_lastPort));

    y -= 36;
    const int mid = (l + r) / 2;
    g_btnHost = button(l, y, mid - 5, "Host a session");
    g_btnJoin = button(mid + 5, y, r, "Join");
    y -= 30;
    g_btnTake = button(l, y, mid - 5, "Take controls");
    g_btnStop = button(mid + 5, y, r, "Disconnect");

    y -= 34;
    g_hint1 = caption(l, y, r, "");
    g_hint2 = caption(l, y - 16, r, "");
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

    g_currentCode = s.code;
    setText(g_codeLabel, s.codeLabel.empty() ? "Your code" : s.codeLabel);
    setText(g_code, s.code.empty() ? (s.busy && !s.connected ? "working..." : "-") : s.code);
    enable(g_btnCopy, !s.code.empty());

    g_letInMode = s.canLetIn;
    setText(g_btnJoin, s.canLetIn ? "Let guest in" : "Join");

    const bool editable = !s.busy || s.canLetIn;
    enable(g_btnHost, !s.busy);
    enable(g_btnJoin, editable);
    enable(g_inputField, editable);
    enable(g_btnPaste, editable);
    enable(g_portField, !s.busy);
    enable(g_btnStop, s.busy);
    enable(g_btnTake, s.canTakeControls);

    // Short instructions for the current step
    std::string h1, h2;
    if (!s.busy) {
        h1 = "Host: click Host a session and send your code to the other pilot.";
        h2 = "Other pilot: paste the code and click Join.";
    } else if (s.canLetIn) {
        h1 = "Send the session code to the other pilot.";
        h2 = "Got a reply code back? Paste it and click Let guest in.";
    } else if (!s.connected && !s.code.empty()) {
        h1 = "Send this reply code to the host.";
        h2 = "The host pastes it and clicks Let guest in.";
    }
    setText(g_hint1, h1);
    setText(g_hint2, h2);
}

void uiDestroy() {
    if (g_window) XPDestroyWidget(g_window, 1);
    g_window = nullptr;
}

std::string uiHostText() {
    if (!g_window) return g_lastHost;
    char buf[256] = {};
    XPGetWidgetDescriptor(g_inputField, buf, sizeof(buf) - 1);
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
