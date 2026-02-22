#ifndef REDSEA_PORT_IO_INPUT_HH_
#define REDSEA_PORT_IO_INPUT_HH_

#include <array>
#include <chrono>
#include <cstddef>

#include "redsea_port/constants.hh"

namespace redsea {

constexpr std::size_t kInputChunkSize = 8192;
constexpr auto kBufferSize = static_cast<std::size_t>(kInputChunkSize * kMaxResampleRatio) + 1;

class MPXBuffer {
 public:
  std::array<float, kBufferSize> data{};
  std::size_t used_size{};
  std::chrono::time_point<std::chrono::system_clock> time_received;
};

}  // namespace redsea

#endif  // REDSEA_PORT_IO_INPUT_HH_
