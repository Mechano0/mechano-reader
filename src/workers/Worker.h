#pragma once
// One background thread that runs heavy jobs (open / extract / compress /
// test) in order, so the UI thread never does archive I/O (brief section 17).
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace mzr::workers {

class Worker {
public:
    Worker();
    ~Worker();                       // finishes the running job, drops the queue, joins
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void Post(std::function<void()> job);
    bool Busy() const;

private:
    void Run();
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false;
    bool running_ = false;
    std::thread t_;
};

} // namespace mzr::workers
