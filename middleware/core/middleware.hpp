// AL-MAIN composition root (Sections 2.2.6 and 4.2.5): creates the AL modules over the interfaces of a
// pal::Platform, connects them and runs the AL worker thread. The process entry point
// (middleware/main/main.cpp) only reads the configuration, creates the platform, handles the signals
// and calls these functions in the order of Section 4.2.5.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "headend_client.hpp"
#include "hls_packager.hpp"
#include "key_manager.hpp"
#include "middleware_config.hpp"
#include "pal/platform.hpp"
#include "psi_parser.hpp"
#include "service_manager.hpp"
#include "session_controller.hpp"
#include "task_queue.hpp"
#include "ui_api.hpp"

namespace al {

class Middleware {
public:
    // Creates the AL objects; the UI and tuner handlers are registered here, so this must happen
    // before Platform::start() (Section 3.9.2).
    Middleware(pal::Platform& platform, const MiddlewareConfig& config);
    ~Middleware();

    Middleware(const Middleware&) = delete;
    Middleware& operator=(const Middleware&) = delete;

    void start_worker();     // the AL worker thread (Section 4.2.1)
    void load_services();    // posts the task "load services" (REQ-01)

    // Posts the task "shutdown" (T-31); `done` is called on the worker when AL-SES has finished.
    void request_shutdown(std::function<void()> done);
    // Orderly stop of Section 4.2.5 on the calling (main) thread: request_shutdown and wait for its
    // completion at most `timeout`. Returns true if it completed in time.
    bool shutdown(std::chrono::milliseconds timeout);
    // After Platform::stop(): closes the task queue, joins the worker and zeroes the keys.
    void finish();

    // For the tests (test mode of the queue, rule T5) and the entry point.
    TaskQueue& queue() { return queue_; }
    KeyManager& keys() { return keys_; }
    SessionController& session() { return session_; }

private:
    pal::Platform& platform_;
    TaskQueue queue_;
    ServiceManager services_;
    HeadendClient headend_;
    PsiParser psi_;
    KeyManager keys_;
    HlsPackager output_;
    UiApi api_;
    SessionController session_;
    std::thread worker_;
    bool finished_ = false;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_done_ = false;
};

}  // namespace al
