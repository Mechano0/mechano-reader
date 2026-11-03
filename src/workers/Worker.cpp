#include "workers/Worker.h"

#include <windows.h>
#include <objbase.h>

namespace mzr::workers {

Worker::Worker() : t_([this] { Run(); }) {}

Worker::~Worker() {
    {
        std::lock_guard lock(m_);
        stop_ = true;
        q_.clear();
    }
    cv_.notify_all();
    if (t_.joinable()) t_.join();
}

void Worker::Post(std::function<void()> job) {
    {
        std::lock_guard lock(m_);
        q_.push_back(std::move(job));
    }
    cv_.notify_one();
}

bool Worker::Busy() const {
    std::lock_guard lock(m_);
    return running_ || !q_.empty();
}

void Worker::Run() {
    // COM is needed by shell APIs some jobs use (e.g. file type names).
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadDescription(GetCurrentThread(), L"MechanoZip worker");
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(m_);
            cv_.wait(lock, [this] { return stop_ || !q_.empty(); });
            if (stop_) break;
            job = std::move(q_.front());
            q_.pop_front();
            running_ = true;
        }
        try {
            job();
        } catch (...) {
            // A job must never take the app down; jobs report their own errors.
        }
        std::lock_guard lock(m_);
        running_ = false;
    }
    CoUninitialize();
}

} // namespace mzr::workers
