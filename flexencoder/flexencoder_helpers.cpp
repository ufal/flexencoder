// flexencoder_helpers.cpp - Implementation of shared helpers

#include "flexencoder_helpers.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#else
static inline std::uint32_t htonl(std::uint32_t x) {
    return ((x & 0xffU) << 24) | ((x & 0xff00U) << 8)
         | ((x & 0xff0000U) >> 8) | ((x & 0xff000000U) >> 24);
}
static inline std::uint32_t ntohl(std::uint32_t x) { return htonl(x); }
#endif

namespace flexencoder {

void write_network_int(std::uint32_t val, FILE* f) {
    if (!f) return;
    std::uint32_t n = htonl(val);
    fwrite(&n, 4, 1, f);
}

std::uint32_t read_network_int(FILE* f) {
    if (!f) return 0;
    std::uint32_t n = 0;
    if (fread(&n, 4, 1, f) != 1) return 0;
    return ntohl(n);
}

} // namespace flexencoder
