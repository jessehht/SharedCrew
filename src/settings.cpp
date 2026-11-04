#include "settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace sc {

void Settings::load(const std::string& file) {
    std::ifstream in(std::filesystem::u8path(file));
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        while (!val.empty() && (val.back() == '\r' || val.back() == ' ')) val.pop_back();
        if (key == "host") host = val;
        if (key == "port") {
            int p = std::atoi(val.c_str());
            if (p > 0 && p < 65536) port = p;
        }
    }
}

void Settings::save(const std::string& file) const {
    std::ofstream out(std::filesystem::u8path(file));
    out << "host=" << host << "\n";
    out << "port=" << port << "\n";
}

}  // namespace sc
