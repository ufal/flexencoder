// flexencoder_helpers.hpp - Shared helpers for flexencoder (CWB binary I/O, etc.)

#pragma once

#include <cstdint>
#include <cstdio>

namespace flexencoder {

// Write a 32-bit integer in network byte order (big-endian) to a CWB binary stream.
void write_network_int(std::uint32_t val, FILE* f);

// Read a 32-bit integer in network byte order; returns 0 if read fails.
std::uint32_t read_network_int(FILE* f);

} // namespace flexencoder
