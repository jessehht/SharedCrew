#pragma once
// Session codes: an internet address + a home-network address, written as
// 20 easy characters, e.g. "8F3K-QW2M-0ZP7-XR4D-11AB". Case and dashes don't matter.
#include <string>

#include "net.h"

namespace sc {

std::string encodeCode(const Endpoint& publicEp, const Endpoint& lanEp);
bool decodeCode(const std::string& text, Endpoint& publicEp, Endpoint& lanEp);

}  // namespace sc
