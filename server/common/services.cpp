#include "services.hpp"

#include <algorithm>
#include <set>

#include "config.hpp"

namespace srv {

int64_t Service::cycle_duration() const {
    int64_t total = 0;
    for (const Program& p : programs) total += p.duration;
    return total;
}

const Service* ServiceCatalog::find(std::string_view service_id) const {
    for (const Service& s : services) {
        if (s.service_id == service_id) return &s;
    }
    return nullptr;
}

namespace {

constexpr int64_t kMaxDuration = 86400;

class Reader {
public:
    explicit Reader(const std::string& path) : path_(path) {}

    [[noreturn]] void fail(const std::string& where, const std::string& what) const {
        throw ConfigError(path_ + ": " + where + ": " + what);
    }

    std::string string(const nlohmann::json& obj, const char* key, const std::string& where,
                       bool allow_empty = false) const {
        const auto it = obj.find(key);
        if (it == obj.end() || !it->is_string()) fail(where, std::string("\"") + key + "\" missing or not a string");
        std::string value = it->get<std::string>();
        if (value.empty() && !allow_empty) fail(where, std::string("\"") + key + "\" is empty");
        return value;
    }

    int64_t integer(const nlohmann::json& obj, const char* key, const std::string& where,
                    int64_t min, int64_t max) const {
        const auto it = obj.find(key);
        const auto value = it == obj.end() ? std::nullopt : json_int64(*it);
        if (!value || *value < min || *value > max) {
            fail(where, std::string("\"") + key + "\" missing or not an integer " +
                            std::to_string(min) + " - " + std::to_string(max));
        }
        return *value;
    }

    bool boolean(const nlohmann::json& obj, const char* key, const std::string& where) const {
        const auto it = obj.find(key);
        if (it == obj.end() || !it->is_boolean()) fail(where, std::string("\"") + key + "\" missing or not a boolean");
        return it->get<bool>();
    }

private:
    const std::string& path_;
};

bool is_service_id(const std::string& id) {
    if (id.size() < 5 || id.compare(0, 4, "svc-") != 0) return false;
    return std::all_of(id.begin() + 4, id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

ServiceCatalog load_services(const std::string& path) {
    const nlohmann::json root = read_json_file(path);
    const Reader r(path);
    if (!root.is_object()) r.fail("top level", "not a JSON object");

    ServiceCatalog catalog;
    catalog.archive_window_s = r.integer(root, "archive_window_s", "top level", 1, kMaxDuration);

    const auto list = root.find("services");
    if (list == root.end() || !list->is_array() || list->empty()) {
        r.fail("top level", "\"services\" missing or not a non-empty array");
    }
    std::set<std::string> ids;
    std::set<int> numbers;
    for (size_t i = 0; i < list->size(); ++i) {
        const nlohmann::json& entry = (*list)[i];
        const std::string where = "services[" + std::to_string(i) + "]";
        if (!entry.is_object()) r.fail(where, "not a JSON object");

        Service s;
        s.service_id = r.string(entry, "service_id", where);
        if (!is_service_id(s.service_id)) r.fail(where, "\"service_id\" is not of the form svc-<n>");
        if (!ids.insert(s.service_id).second) r.fail(where, "duplicate \"service_id\" " + s.service_id);
        s.number = static_cast<int>(r.integer(entry, "number", where, 1, 999));
        if (!numbers.insert(s.number).second) r.fail(where, "duplicate \"number\" " + std::to_string(s.number));
        s.name = r.string(entry, "name", where);
        s.description = r.string(entry, "description", where);
        s.is_protected = r.boolean(entry, "protected", where);
        s.file = r.string(entry, "file", where);

        const auto programs = entry.find("programs");
        if (programs == entry.end() || !programs->is_array() || programs->empty()) {
            r.fail(where, "\"programs\" missing or not a non-empty array");
        }
        for (size_t j = 0; j < programs->size(); ++j) {
            const nlohmann::json& p = (*programs)[j];
            const std::string pwhere = where + ".programs[" + std::to_string(j) + "]";
            if (!p.is_object()) r.fail(pwhere, "not a JSON object");
            Program program;
            program.title = r.string(p, "title", pwhere);
            program.description = r.string(p, "description", pwhere, /*allow_empty=*/true);
            program.duration = r.integer(p, "duration", pwhere, 1, kMaxDuration);
            s.programs.push_back(std::move(program));
        }
        catalog.services.push_back(std::move(s));
    }
    std::sort(catalog.services.begin(), catalog.services.end(),
              [](const Service& a, const Service& b) { return a.number < b.number; });
    return catalog;
}

}  // namespace srv
