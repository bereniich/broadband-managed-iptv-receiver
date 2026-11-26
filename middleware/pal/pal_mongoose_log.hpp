// Mongoose writes its own log lines character by character to stdout, at level DEBUG by default.
// The PAL lowers the level to errors and turns each Mongoose line into one ILogger line (NFR-13).
#pragma once

#include "pal/platform.hpp"

namespace pal {

// Mongoose logging is process-wide: the last routed logger receives the lines.
void route_mongoose_log(ILogger* logger);
// Stops routing to `logger` (if it is the routed one); later lines are dropped.
void unroute_mongoose_log(ILogger* logger);

}  // namespace pal
