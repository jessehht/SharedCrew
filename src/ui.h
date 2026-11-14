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

enum class UiState { Idle, Hosting, Joining, Connected };

struct UiStatus {
    UiState state = UiState::Idle;
    std::string message;     // status or error line
    std::string aircraft;    // profile title
    std::string profileNote; // e.g. "SmartCopilot profile"
    std::string code;        // host: session code, guest: reply code (when needed)
    bool codeIsShort = true; // host: short server code (no reply-code step needed)
    // Connected
    int pingMs = 0;
    bool thisPcComputes = true;
    bool fixedRoles = false;
    bool iHaveControl = true;
    bool canTakeControls = false;
};

void uiInit(const UiActions& actions, const std::string& lastCode, int port);
void uiToggle();
void uiShow();
bool uiVisible();
void uiUpdate(const UiStatus& s);
void uiDestroy();
int uiPort();

}  // namespace sc
