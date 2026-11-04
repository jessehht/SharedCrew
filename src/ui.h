#pragma once
#include <functional>
#include <string>

namespace sc {

struct UiActions {
    std::function<void(int port)> host;
    std::function<void(const std::string& codeOrAddress, int port)> join;
    std::function<void(const std::string& replyCode)> letIn;
    std::function<void()> disconnect;
    std::function<void()> takeControls;
};

struct UiStatus {
    std::string status;
    std::string role;       // "Host", "Guest" or empty
    std::string controls;   // who flies
    std::string profile;    // profile title + match info
    std::string link;       // peer address + ping
    std::string code;       // session code (host) or reply code (guest)
    std::string codeLabel;  // "Session code" / "Reply code"
    bool canLetIn = false;  // host is waiting: the Join button becomes "Let guest in"
    bool connected = false;
    bool busy = false;      // hosting/connecting/connected
    bool iAmFlying = true;
};

void uiInit(const UiActions& actions, const std::string& host, int port);
void uiToggle();
void uiShow();
bool uiVisible();
void uiUpdate(const UiStatus& s);
void uiDestroy();
std::string uiHostText();
int uiPort();

}  // namespace sc
