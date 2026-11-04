#include "log.h"

#include "XPLMUtilities.h"

namespace sc {

void log(const std::string& msg) {
    std::string line = "SharedCrew: " + msg + "\n";
    XPLMDebugString(line.c_str());
}

}  // namespace sc
