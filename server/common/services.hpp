// Content configuration content/services.json (IF-INT-05, Section 3.5), read by SRV-MW and SRV-ARC.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace srv {

struct Program {
    std::string title;
    std::string description;
    int64_t duration = 0;  // seconds
};

struct Service {
    std::string service_id;  // "svc-<n>"
    int number = 0;          // channel number shown to the viewer
    std::string name;
    std::string description;
    bool is_protected = false;
    std::string file;        // content file, relative to content_dir
    std::vector<Program> programs;

    // Cycle duration D = sum of the program durations (Section 4.7.7).
    int64_t cycle_duration() const;
};

struct ServiceCatalog {
    int64_t archive_window_s = 0;
    std::vector<Service> services;  // sorted by number

    const Service* find(std::string_view service_id) const;
};

// Reads and validates services.json. Throws ConfigError naming the file, the entry and the field
// for a missing file, invalid JSON or an invalid entry (ER-37). The rules are the ones the
// receiver applies to the service list (REQ-02: `service_id` of the form `svc-<n>`, `number`
// 1 – 999, both unique; `name` non-empty; `protected` boolean), so that the server never serves an
// entry the receiver would discard, plus: `description` and `file` non-empty strings, at least one
// program with non-empty `title`, a `description` string and an integer `duration` 1 – 86 400 s,
// and an integer `archive_window_s` 1 – 86 400 s. Unknown fields are ignored (Section 3.1).
// Comparing the program durations with the content file is SRV-ARC's check (ER-38).
ServiceCatalog load_services(const std::string& path);

}  // namespace srv
