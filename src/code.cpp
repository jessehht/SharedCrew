#include "code.h"

#include <cctype>
#include <cstdint>

namespace sc {

namespace {

// Crockford base32: no I, L, O or U, so it is hard to misread.
const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int charValue(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    for (int i = 0; i < 32; ++i)
        if (kAlphabet[i] == c) return i;
    return -1;
}

void putEndpoint(uint8_t* b, const Endpoint& e) {
    b[0] = e.ip >> 24;
    b[1] = (e.ip >> 16) & 255;
    b[2] = (e.ip >> 8) & 255;
    b[3] = e.ip & 255;
    b[4] = e.port >> 8;
    b[5] = e.port & 255;
}

Endpoint getEndpoint(const uint8_t* b) {
    Endpoint e;
    e.ip = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
    e.port = static_cast<uint16_t>((b[4] << 8) | b[5]);
    return e;
}

}  // namespace

std::string encodeCode(const Endpoint& publicEp, const Endpoint& lanEp) {
    uint8_t bytes[12];
    putEndpoint(bytes, publicEp);
    putEndpoint(bytes + 6, lanEp);

    // 96 bits -> 20 characters of 5 bits (last one padded)
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (uint8_t b : bytes) {
        acc = (acc << 8) | b;
        bits += 8;
        while (bits >= 5) {
            out += kAlphabet[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) out += kAlphabet[(acc << (5 - bits)) & 31];

    std::string grouped;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i > 0 && i % 4 == 0) grouped += '-';
        grouped += out[i];
    }
    return grouped;
}

bool decodeCode(const std::string& text, Endpoint& publicEp, Endpoint& lanEp) {
    std::string clean;
    for (char c : text) {
        if (c == '-' || c == ' ' || c == '\t') continue;
        clean += c;
    }
    if (clean.size() != 20) return false;

    uint8_t bytes[12] = {};
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (char c : clean) {
        int v = charValue(c);
        if (v < 0) return false;
        acc = (acc << 5) | static_cast<uint32_t>(v);
        bits += 5;
        if (bits >= 8) {
            if (n < sizeof(bytes)) bytes[n++] = static_cast<uint8_t>((acc >> (bits - 8)) & 255);
            bits -= 8;
        }
    }
    if (n != sizeof(bytes)) return false;
    publicEp = getEndpoint(bytes);
    lanEp = getEndpoint(bytes + 6);
    return publicEp.valid() || lanEp.valid();
}

}  // namespace sc
