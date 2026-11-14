// SharedCrew window: a standard X-Plane 12 floating window (X-Plane draws the frame and title bar),
// with the content drawn here. Plain, dark, one accent color, like X-Plane's own windows.
#include "ui.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <GL/gl.h>
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMProcessing.h"

namespace sc {

namespace {

constexpr int kWidth = 300;
constexpr int kPad = 14;
constexpr int kBtnH = 26;
constexpr int kFieldH = 26;
constexpr int kMinHeight = 140;

// Colors (RGB)
float kText[3] = {0.90f, 0.91f, 0.92f};
float kMuted[3] = {0.55f, 0.57f, 0.61f};
float kError[3] = {0.95f, 0.55f, 0.50f};
float kWhite[3] = {1.0f, 1.0f, 1.0f};
const float kFieldBg[3] = {0.09f, 0.10f, 0.11f};
const float kLine[3] = {0.27f, 0.29f, 0.33f};
const float kBtnBg[3] = {0.20f, 0.22f, 0.25f};
const float kBtnHover[3] = {0.25f, 0.27f, 0.31f};
const float kAccent[3] = {0.18f, 0.48f, 0.82f};
const float kAccentHover[3] = {0.22f, 0.54f, 0.88f};
const float kFocus[3] = {0.30f, 0.58f, 0.90f};

enum ButtonId { BtnHost = 1, BtnJoin, BtnPaste, BtnSettings, BtnCopy, BtnStop, BtnLetIn, BtnPasteReply,
                BtnTake, BtnDisconnect, FieldCode, FieldPort, FieldReply };

struct Hit {
    int l, t, r, b;
    int id;
};

struct Field {
    std::string text;
    size_t maxLen = 64;
    bool digitsOnly = false;
};

UiActions g_actions;
UiStatus g_status;
XPLMWindowID g_window = nullptr;
std::vector<Hit> g_hits;
Field g_codeField, g_portField, g_replyField;
int g_focus = 0;  // FieldCode / FieldPort / FieldReply or 0
bool g_showSettings = false;
float g_copiedAt = -100.0f;
std::string g_hint;  // local validation message
int g_fontH = 12;

// ---------------------------------------------------------------- clipboard

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
    for (auto& c : out)
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out.size() > 64) out.resize(64);
    return out;
}

// ---------------------------------------------------------------- drawing helpers

void fillRect(int l, int t, int r, int b, const float* c) {
    glColor3f(c[0], c[1], c[2]);
    glBegin(GL_QUADS);
    glVertex2i(l, b);
    glVertex2i(l, t);
    glVertex2i(r, t);
    glVertex2i(r, b);
    glEnd();
}

void frameRect(int l, int t, int r, int b, const float* c) {
    glColor3f(c[0], c[1], c[2]);
    glBegin(GL_LINE_LOOP);
    glVertex2f(l + 0.5f, b + 0.5f);
    glVertex2f(l + 0.5f, t - 0.5f);
    glVertex2f(r - 0.5f, t - 0.5f);
    glVertex2f(r - 0.5f, b + 0.5f);
    glEnd();
}

void hline(int l, int r, int y, const float* c) {
    glColor3f(c[0], c[1], c[2]);
    glBegin(GL_LINES);
    glVertex2f(static_cast<float>(l), y + 0.5f);
    glVertex2f(static_cast<float>(r), y + 0.5f);
    glEnd();
}

int textWidth(const std::string& s) {
    return static_cast<int>(XPLMMeasureString(xplmFont_Proportional, s.c_str(), static_cast<int>(s.size())));
}

void text(int x, int topY, const std::string& s, float* color) {
    // topY = top of the line; X-Plane draws from the baseline.
    XPLMDrawString(color, x, topY - g_fontH, const_cast<char*>(s.c_str()), nullptr, xplmFont_Proportional);
}

// Shortens text to fit a width, adding "..." when cut.
std::string fit(const std::string& s, int width) {
    if (textWidth(s) <= width) return s;
    std::string out = s;
    while (!out.empty() && textWidth(out + "...") > width) out.pop_back();
    return out + "...";
}

bool mouseIn(int l, int t, int r, int b) {
    int x, y;
    XPLMGetMouseLocationGlobal(&x, &y);
    return x >= l && x < r && y <= t && y > b;
}

void button(int l, int t, int r, const std::string& label, int id, bool primary = false) {
    const int b = t - kBtnH;
    const bool hover = mouseIn(l, t, r, b);
    const float* bg = primary ? (hover ? kAccentHover : kAccent) : (hover ? kBtnHover : kBtnBg);
    fillRect(l, t, r, b, bg);
    if (!primary) frameRect(l, t, r, b, kLine);
    const int w = textWidth(label);
    text(l + (r - l - w) / 2, t - (kBtnH - g_fontH) / 2, label, primary ? kWhite : kText);
    g_hits.push_back({l, t, r, b, id});
}

void field(int l, int t, int r, Field& f, int id, const std::string& placeholder) {
    const int b = t - kFieldH;
    const bool focused = g_focus == id;
    fillRect(l, t, r, b, kFieldBg);
    frameRect(l, t, r, b, focused ? kFocus : kLine);
    const int tx = l + 8, ty = t - (kFieldH - g_fontH) / 2;
    if (f.text.empty() && !focused) {
        text(tx, ty, placeholder, kMuted);
    } else {
        text(tx, ty, f.text, kText);
        // Blinking caret
        if (focused && static_cast<int>(XPLMGetElapsedTime() * 2) % 2 == 0) {
            int cx = tx + textWidth(f.text) + 1;
            glColor3f(kText[0], kText[1], kText[2]);
            glBegin(GL_LINES);
            glVertex2f(cx + 0.5f, static_cast<float>(t - 6));
            glVertex2f(cx + 0.5f, static_cast<float>(b + 6));
            glEnd();
        }
    }
    g_hits.push_back({l, t, r, b, id});
}

// A label on the left and a value on the right, with a thin line above.
void row(int l, int r, int t, const std::string& label, const std::string& value) {
    hline(l, r, t, kLine);
    text(l, t - 7, label, kMuted);
    const std::string v = fit(value, (r - l) - textWidth(label) - 16);
    text(r - textWidth(v), t - 7, v, kText);
}

// ---------------------------------------------------------------- content

int lineH() { return g_fontH + 6; }

// Draws the content and returns the height it used.
int drawContent(int L, int T, int R) {
    g_hits.clear();
    const int l = L + kPad, r = R - kPad;
    const int mid = (l + r) / 2;
    int y = T - kPad;
    const UiStatus& s = g_status;
    const bool copied = XPLMGetElapsedTime() - g_copiedAt < 2.0f;

    switch (s.state) {
        case UiState::Idle: {
            text(l, y, "Aircraft", kMuted);
            y -= lineH();
            text(l, y, fit(s.aircraft, r - l), kText);
            y -= lineH();
            if (!s.profileNote.empty()) {
                text(l, y, fit(s.profileNote, r - l), kMuted);
                y -= lineH();
            }
            y -= 8;
            button(l, y, r, "Host a flight", BtnHost, true);
            y -= kBtnH + 16;
            text(l, y, "Session code", kMuted);
            y -= lineH();
            field(l, y, r - 64, g_codeField, FieldCode, "Paste the code");
            button(r - 56, y, r, "Paste", BtnPaste);
            y -= kFieldH + 8;
            button(l, y, r, "Join", BtnJoin);
            y -= kBtnH + 10;
            const std::string msg = !g_hint.empty() ? g_hint : s.message;
            if (!msg.empty()) {
                // Wrap long messages over two lines
                int width = r - l;
                std::string first = msg, rest;
                if (textWidth(msg) > width) {
                    size_t cut = msg.size();
                    while (cut > 0 && (textWidth(msg.substr(0, cut)) > width || msg[cut - 1] != ' ')) --cut;
                    if (cut == 0) cut = msg.size();
                    first = msg.substr(0, cut);
                    rest = msg.substr(cut);
                }
                text(l, y, first, kError);
                y -= lineH();
                if (!rest.empty()) {
                    text(l, y, fit(rest, width), kError);
                    y -= lineH();
                }
                y -= 4;
            }
            text(l, y, g_showSettings ? "Hide settings" : "Settings", kMuted);
            g_hits.push_back({l, y, l + textWidth("Hide settings"), y - lineH(), BtnSettings});
            y -= lineH();
            if (g_showSettings) {
                y -= 4;
                text(l, y - 7, "UDP port", kMuted);
                field(r - 90, y, r, g_portField, FieldPort, "51000");
                y -= kFieldH + 4;
            }
            break;
        }
        case UiState::Hosting: {
            if (s.code.empty()) {
                text(l, y, "Making your session code...", kText);
                y -= lineH();
            } else {
                text(l, y, "Send this code to the other pilot", kMuted);
                y -= lineH() + 6;
                // Code, drawn twice for weight and spaced out so it reads well
                int x = l;
                for (char c : s.code) {
                    char one[2] = {c, 0};
                    XPLMDrawString(kWhite, x, y - g_fontH, one, nullptr, xplmFont_Proportional);
                    XPLMDrawString(kWhite, x + 1, y - g_fontH, one, nullptr, xplmFont_Proportional);
                    x += textWidth(one) + 4;
                }
                y -= lineH() + 10;
                button(l, y, r, copied ? "Copied" : "Copy code", BtnCopy);
                y -= kBtnH + 12;
            }
            text(l, y, fit(s.message, r - l), kText);
            y -= lineH() + 4;
            if (!s.code.empty() && !s.codeIsShort) {
                // Long code without the server: the guest may send a reply code back.
                text(l, y, "Got a reply code back?", kMuted);
                y -= lineH();
                field(l, y, r - 64, g_replyField, FieldReply, "Paste reply code");
                button(r - 56, y, r, "Paste", BtnPasteReply);
                y -= kFieldH + 8;
                button(l, y, r, "Let guest in", BtnLetIn);
                y -= kBtnH + 8;
            }
            button(l, y, r, "Stop", BtnStop);
            y -= kBtnH;
            break;
        }
        case UiState::Joining: {
            text(l, y, fit(s.message, r - l), kText);
            y -= lineH() + 6;
            if (!s.code.empty()) {
                text(l, y, "Send this reply code to the host", kMuted);
                y -= lineH() + 4;
                text(l, y, s.code, kWhite);
                y -= lineH() + 6;
                button(l, y, r, copied ? "Copied" : "Copy reply code", BtnCopy);
                y -= kBtnH + 8;
            }
            button(l, y, r, "Cancel", BtnDisconnect);
            y -= kBtnH;
            break;
        }
        case UiState::Connected: {
            text(l, y, "Connected  " + std::to_string(s.pingMs) + " ms", kText);
            y -= lineH() + 6;
            row(l, r, y, "Aircraft", s.aircraft);
            y -= lineH() + 8;
            row(l, r, y, "This PC", s.thisPcComputes ? "Computes the aircraft" : "Follows the host");
            y -= lineH() + 8;
            if (s.fixedRoles) {
                row(l, r, y, "Thrust and tiller", s.iHaveControl ? "You" : "Other pilot");
                y -= lineH() + 8;
                row(l, r, y, "Sidesticks", "Both");
            } else {
                row(l, r, y, "Flying", s.iHaveControl ? "You" : "Other pilot");
            }
            y -= lineH() + 8;
            hline(l, r, y, kLine);
            y -= 10;
            if (!s.message.empty() && s.message.rfind("Connected", 0) != 0) {
                text(l, y, fit(s.message, r - l), kMuted);
                y -= lineH() + 4;
            }
            if (s.canTakeControls) {
                button(l, y, r, "Take controls", BtnTake, true);
                y -= kBtnH + 8;
            }
            button(l, y, r, "Disconnect", BtnDisconnect);
            y -= kBtnH;
            break;
        }
    }
    (void)mid;
    return T - y + kPad;
}

// ---------------------------------------------------------------- callbacks

int g_requestedHeight = 0;

void drawWindow(XPLMWindowID win, void*) {
    XPLMSetGraphicsState(0, 0, 0, 0, 1, 0, 0);
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);
    int used = drawContent(l, t, r);
    if (used < kMinHeight) used = kMinHeight;
    // Fit the window to its content, keeping the top edge where it is. Only when the content
    // height changes: chasing X-Plane's own size adjustments every frame makes the window drift.
    if (!XPLMWindowIsPoppedOut(win) && used != g_requestedHeight) {
        g_requestedHeight = used;
        XPLMSetWindowGeometry(win, l, t, r, t - used);
    }
}

void activateField(int id) {
    g_focus = id;
    XPLMTakeKeyboardFocus(g_window);
}

void releaseField() {
    if (g_focus && XPLMHasKeyboardFocus(g_window)) XPLMTakeKeyboardFocus(nullptr);
    g_focus = 0;
}

void doJoin() {
    if (g_codeField.text.empty()) {
        g_hint = "Enter the session code first";
        return;
    }
    g_hint.clear();
    releaseField();
    if (g_actions.join) g_actions.join(g_codeField.text, uiPort());
}

void doLetIn() {
    if (g_replyField.text.empty()) return;
    releaseField();
    if (g_actions.letIn) g_actions.letIn(g_replyField.text);
}

void onClick(int id) {
    switch (id) {
        case BtnHost:
            g_hint.clear();
            releaseField();
            if (g_actions.host) g_actions.host(uiPort());
            break;
        case BtnJoin: doJoin(); break;
        case BtnPaste:
            g_codeField.text = getClipboard();
            g_hint.clear();
            break;
        case BtnPasteReply: g_replyField.text = getClipboard(); break;
        case BtnSettings: g_showSettings = !g_showSettings; break;
        case BtnCopy:
            if (!g_status.code.empty()) {
                setClipboard(g_status.code);
                g_copiedAt = XPLMGetElapsedTime();
            }
            break;
        case BtnStop:
        case BtnDisconnect:
            if (g_actions.disconnect) g_actions.disconnect();
            break;
        case BtnLetIn: doLetIn(); break;
        case BtnTake:
            if (g_actions.takeControls) g_actions.takeControls();
            break;
        case FieldCode:
        case FieldPort:
        case FieldReply: activateField(id); break;
        default: break;
    }
}

int mouseClick(XPLMWindowID, int x, int y, XPLMMouseStatus status, void*) {
    if (status != xplm_MouseDown) return 1;
    int hitId = 0;
    for (const auto& h : g_hits)
        if (x >= h.l && x < h.r && y <= h.t && y > h.b) hitId = h.id;
    if (hitId != FieldCode && hitId != FieldPort && hitId != FieldReply) releaseField();
    if (hitId) onClick(hitId);
    return 1;
}

Field* focusedField() {
    if (g_focus == FieldCode) return &g_codeField;
    if (g_focus == FieldPort) return &g_portField;
    if (g_focus == FieldReply) return &g_replyField;
    return nullptr;
}

void handleKey(XPLMWindowID, char key, XPLMKeyFlags flags, char vkey, void*, int losingFocus) {
    if (losingFocus) {
        g_focus = 0;
        return;
    }
    Field* f = focusedField();
    if (!f || !(flags & xplm_DownFlag)) return;
    const unsigned char vk = static_cast<unsigned char>(vkey);
    if ((flags & xplm_ControlFlag) && vk == XPLM_VK_V) {
        f->text = getClipboard();
        if (f->text.size() > f->maxLen) f->text.resize(f->maxLen);
        return;
    }
    if (vk == XPLM_VK_BACK) {
        if (!f->text.empty()) f->text.pop_back();
        return;
    }
    if (vk == XPLM_VK_RETURN || vk == XPLM_VK_ENTER) {
        if (g_focus == FieldCode) doJoin();
        else if (g_focus == FieldReply) doLetIn();
        else releaseField();
        return;
    }
    if (vk == XPLM_VK_ESCAPE || vk == XPLM_VK_TAB) {
        releaseField();
        return;
    }
    const unsigned char c = static_cast<unsigned char>(key);
    if (c < 32 || c > 126 || f->text.size() >= f->maxLen) return;
    if (f->digitsOnly && (c < '0' || c > '9')) return;
    f->text += static_cast<char>(c);
    if (g_focus == FieldCode) g_hint.clear();
}

XPLMCursorStatus cursor(XPLMWindowID, int, int, void*) { return xplm_CursorDefault; }
int wheel(XPLMWindowID, int, int, int, int, void*) { return 1; }

void create() {
    int sl, st, sr, sb;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    XPLMCreateWindow_t p{};
    p.structSize = sizeof(p);
    p.left = sl + 80;
    p.top = st - 140;
    p.right = p.left + kWidth;
    p.bottom = p.top - 300;
    p.visible = 1;
    p.drawWindowFunc = drawWindow;
    p.handleMouseClickFunc = mouseClick;
    p.handleRightClickFunc = mouseClick;
    p.handleKeyFunc = handleKey;
    p.handleCursorFunc = cursor;
    p.handleMouseWheelFunc = wheel;
    p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
    p.layer = xplm_WindowLayerFloatingWindows;
    g_window = XPLMCreateWindowEx(&p);
    XPLMSetWindowTitle(g_window, "SharedCrew");
    XPLMSetWindowResizingLimits(g_window, kWidth, kMinHeight, kWidth + 200, 700);
    XPLMSetWindowPositioningMode(g_window, xplm_WindowPositionFree, -1);
}

}  // namespace

void uiInit(const UiActions& actions, const std::string& lastCode, int port) {
    g_actions = actions;
    g_codeField.text = lastCode;
    g_codeField.maxLen = 64;
    g_portField.text = std::to_string(port);
    g_portField.maxLen = 5;
    g_portField.digitsOnly = true;
    g_replyField.maxLen = 64;
    int w = 0, h = 0, digits = 0;
    XPLMGetFontDimensions(xplmFont_Proportional, &w, &h, &digits);
    if (h > 0) g_fontH = h;
}

void uiShow() {
    if (!g_window) create();
    // Put it back on screen if it ended up (partly) outside it.
    if (!XPLMWindowIsPoppedOut(g_window)) {
        int sl, st, sr, sb, l, t, r, b;
        XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
        XPLMGetWindowGeometry(g_window, &l, &t, &r, &b);
        if (t > st || b < sb || l < sl || r > sr) {
            const int h = t - b;
            XPLMSetWindowGeometry(g_window, sl + 80, st - 140, sl + 80 + (r - l), st - 140 - h);
        }
    }
    g_requestedHeight = 0;
    XPLMSetWindowIsVisible(g_window, 1);
    XPLMBringWindowToFront(g_window);
}

void uiToggle() {
    if (uiVisible()) {
        releaseField();
        XPLMSetWindowIsVisible(g_window, 0);
    } else {
        uiShow();
    }
}

bool uiVisible() { return g_window && XPLMGetWindowIsVisible(g_window); }

void uiUpdate(const UiStatus& s) {
    // A new state clears the local hint and the reply field.
    if (s.state != g_status.state) {
        g_hint.clear();
        if (s.state != UiState::Hosting) g_replyField.text.clear();
    }
    g_status = s;
}

void uiDestroy() {
    if (g_window) {
        releaseField();
        XPLMDestroyWindow(g_window);
    }
    g_window = nullptr;
}

int uiPort() {
    int p = std::atoi(g_portField.text.c_str());
    return (p > 0 && p < 65536) ? p : 51000;
}

}  // namespace sc
