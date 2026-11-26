// Fake UI channel of the AL unit tests (Section 5.3.1, "AL fakes"): the test plays the UI clients,
// the fake records every frame the AL sends.
#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

#include "pal/platform.hpp"

namespace fakes {

class FakeUiChannel : public pal::IUiChannel {
public:
    struct Frame {
        bool broadcast;
        ClientId client;     // 0 for a broadcast
        std::string text;
        bool delivered;      // false: send() to a client that is not connected
    };

    void set_handlers(MessageCallback on_message, ConnectionCallback on_connection) override {
        on_message_ = std::move(on_message);
        on_connection_ = std::move(on_connection);
    }

    void send(ClientId client, const std::string& text) override {
        frames_.push_back(Frame{false, client, text, connected_.count(client) != 0});
    }

    void broadcast(const std::string& text) override {
        frames_.push_back(Frame{true, 0, text, !connected_.empty()});
    }

    // ---- test side
    void connect(ClientId client) {
        connected_.insert(client);
        if (on_connection_) on_connection_(client, true);
    }

    void disconnect(ClientId client) {
        connected_.erase(client);
        if (on_connection_) on_connection_(client, false);
    }

    void receive(ClientId client, const std::string& text) {
        if (on_message_) on_message_(client, text);
    }

    const std::vector<Frame>& frames() const { return frames_; }
    void clear() { frames_.clear(); }

    // All frames parsed as JSON (a frame that is not JSON becomes a discarded value).
    std::vector<nlohmann::json> json_frames() const {
        std::vector<nlohmann::json> result;
        for (const Frame& f : frames_) result.push_back(nlohmann::json::parse(f.text, nullptr, false));
        return result;
    }

    // Data of every event with this name, in the order sent.
    std::vector<nlohmann::json> events(const std::string& name) const {
        std::vector<nlohmann::json> result;
        for (const nlohmann::json& j : json_frames()) {
            if (j.is_object() && j.value("type", "") == "event" && j.value("name", "") == name) {
                result.push_back(j.value("data", nlohmann::json()));
            }
        }
        return result;
    }

    // Responses with this id (normally exactly one, Section 3.6.1).
    std::vector<nlohmann::json> responses(long long id) const {
        std::vector<nlohmann::json> result;
        for (const nlohmann::json& j : json_frames()) {
            if (j.is_object() && j.value("type", "") == "response" && j.contains("id") &&
                j["id"].is_number_integer() && j["id"].get<long long>() == id) {
                result.push_back(j);
            }
        }
        return result;
    }

private:
    MessageCallback on_message_;
    ConnectionCallback on_connection_;
    std::set<ClientId> connected_;
    std::vector<Frame> frames_;
};

}  // namespace fakes
