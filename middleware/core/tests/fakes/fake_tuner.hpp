// Fake tuner of the AL unit tests (Section 5.3.1, "AL fakes"): status events on request of the test.
#pragma once

#include <string>
#include <utility>

#include "fakes/call_log.hpp"
#include "pal/platform.hpp"

namespace fakes {

class FakeTuner : public pal::ITuner {
public:
    explicit FakeTuner(CallLog* log = nullptr) : log_(log) {}

    void set_status_handler(StatusCallback on_status) override { handler_ = std::move(on_status); }

    bool tune(const pal::TuneParams& params) override {
        record(log_, "tune " + std::to_string(params.udp_port) + " " + std::to_string(params.ssrc));
        ++tune_calls_;
        last_params_ = params;
        tuned_ = tune_result_;
        return tune_result_;
    }

    void untune() override {
        record(log_, "untune");
        tuned_ = false;
    }

    Stats stats() const override { return stats_; }

    // ---- test side
    void set_tune_result(bool ok) { tune_result_ = ok; }
    void set_stats(const Stats& stats) { stats_ = stats; }

    // Delivers a status to the handler, as the tuner thread would. Returns false (and delivers
    // nothing) when the tuner is not tuned: no status callback after untune() (Section 3.9.1, rule 4).
    bool emit(pal::TunerStatus status) {
        if (!tuned_ || !handler_) return false;
        handler_(status);
        return true;
    }

    bool tuned() const { return tuned_; }
    int tune_calls() const { return tune_calls_; }
    const pal::TuneParams& last_params() const { return last_params_; }

private:
    CallLog* log_;
    StatusCallback handler_;
    bool tune_result_ = true;
    bool tuned_ = false;
    int tune_calls_ = 0;
    pal::TuneParams last_params_{0, 0};
    Stats stats_{0, 0, 0};
};

}  // namespace fakes
