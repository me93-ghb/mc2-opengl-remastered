#pragma once
#include <cstddef>
#include <vector>

namespace MpWire {
// Increment when a field, message meaning, or packed chunk layout changes.
constexpr unsigned Version = 1;
constexpr std::size_t MaxPacket = 65536;
bool encode(const void* native, std::size_t size, std::vector<unsigned char>& wire);
bool decode(const void* wire, std::size_t size, std::vector<unsigned char>& native);
}
