#include "pal_mongoose_log.hpp"

#include <mutex>
#include <string>

#include "mongoose.h"

namespace pal {

namespace {

std::mutex g_mutex;
ILogger* g_logger = nullptr;
std::string g_line;

void mongoose_log_char(char c, void*) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (c != '\n') {
        if (g_line.size() < 1024) g_line += c;
        return;
    }
    if (g_logger != nullptr && !g_line.empty()) g_logger->log(LogLevel::Warn, "PAL-MG", g_line);
    g_line.clear();
}

void drop_char(char, void*) {}

}  // namespace

void route_mongoose_log(ILogger* logger) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_logger = logger;
    g_line.clear();
    mg_log_set(MG_LL_ERROR);
    mg_log_set_fn(&mongoose_log_char, nullptr);
}

void unroute_mongoose_log(ILogger* logger) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logger != logger) return;
    g_logger = nullptr;
    g_line.clear();
    mg_log_set_fn(&drop_char, nullptr);
}

}  // namespace pal
