// SRV-MW: OTT MW REST API IF-EXT-03 (Section 3.2) and EPG generation (Section 4.7.7).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "clock.hpp"
#include "http.hpp"
#include "log.hpp"
#include "services.hpp"

namespace srv {

struct EpgEvent {
    int64_t start = 0;
    int64_t duration = 0;
    const Program* program = nullptr;
};

// Events of the EPG cycle of `service` that overlap [from, to), sorted by start (Section 4.7.7):
// for every cycle number k from floor(from / D) - 1 to floor(to / D), the events
// start = k * D + o_i with o_i = d_0 + ... + d_(i-1), kept if start < to and start + d_i > from.
std::vector<EpgEvent> epg_events(const Service& service, int64_t from, int64_t to);

// Handler of the REST API under /api/v1. Stateless after construction; handle() may be called
// from any thread. Every request is logged with module tag SRV-MW (INFO, or WARN for an error).
class MwApi {
public:
    static constexpr int64_t kMaxEpgRange = 86400;  // Section 3.2.3: to - from <= 86 400

    MwApi(ServiceCatalog catalog, std::string stream_control_url, WallClock clock = system_wall_clock(),
          Logger* logger = nullptr);

    HttpResponse handle(const HttpRequest& request) const;

private:
    HttpResponse route(const HttpRequest& request) const;
    HttpResponse services() const;
    HttpResponse service(const std::string& service_id) const;
    HttpResponse epg(const std::string& query) const;

    ServiceCatalog catalog_;
    std::string stream_control_url_;
    WallClock clock_;
    Logger* logger_;
};

}  // namespace srv
