#pragma once
#include <string>

namespace sc {

// Writes a line to X-Plane's Log.txt, prefixed with "SharedCrew: ".
void log(const std::string& msg);

}  // namespace sc
