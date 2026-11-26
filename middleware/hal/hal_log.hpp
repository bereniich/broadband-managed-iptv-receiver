// Log notifications of the HAL blocks. The HAL calls no PAL module (rule R1, Section 4.1.4); PAL-SDK
// passes a function that forwards the lines to PAL-LOG.
#pragma once

#include <functional>
#include <string>

namespace hal {

enum class LogLevel { Debug, Info, Warn, Error };

// `module` is the tag of the HAL block ("HAL-TUN", "HAL-DMX", "HAL-DSC").
using LogFn = std::function<void(LogLevel level, const char* module, const std::string& message)>;

}  // namespace hal
