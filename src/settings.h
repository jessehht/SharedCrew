#pragma once
#include <string>

namespace sc {

struct Settings {
    std::string host;
    int port = 51000;

    void load(const std::string& file);
    void save(const std::string& file) const;
};

}  // namespace sc
