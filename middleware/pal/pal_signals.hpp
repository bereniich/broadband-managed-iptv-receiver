// PAL helper: threads of the PAL and HAL are created with SIGINT and SIGTERM blocked.
//
// AL-MAIN handles both signals with a handler that only sets a flag (Section 4.2.5). A thread
// inherits the signal mask of the thread that creates it, so creating PAL and HAL threads inside
// this scope keeps the signals on the threads of the AL: a PAL or HAL thread is never interrupted
// in a blocking call (EINTR) and never runs the handler.
#pragma once

#include <pthread.h>
#include <signal.h>

namespace pal {

class BlockStopSignals {
public:
    BlockStopSignals() {
        sigset_t stop_signals;
        sigemptyset(&stop_signals);
        sigaddset(&stop_signals, SIGINT);
        sigaddset(&stop_signals, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &stop_signals, &previous_);
    }
    ~BlockStopSignals() { pthread_sigmask(SIG_SETMASK, &previous_, nullptr); }
    BlockStopSignals(const BlockStopSignals&) = delete;
    BlockStopSignals& operator=(const BlockStopSignals&) = delete;

private:
    sigset_t previous_;
};

}  // namespace pal
