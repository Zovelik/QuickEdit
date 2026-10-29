#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>


namespace core
{
    // Fixed set of worker threads running the tasks in submission order.
    // Long tasks (decoding a whole file) should check a cancellation flag of
    // their own: the destructor waits for the running tasks and drops the
    // queued ones.
    class ThreadPool
    {
    public:
        // 0 threads: one per hardware thread, minus one for the main thread.
        explicit ThreadPool(unsigned int threadCount = 0);
        ~ThreadPool();

        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        void submit(std::function<void()> task);

        // Runs body(i) for i in [0, count), split across the workers and the
        // calling thread, and returns once every call is done. Must not be
        // called from a task of this pool with all workers busy waiting on
        // it: the calling thread takes part, so it always progresses.
        void parallelFor(size_t count, const std::function<void(size_t)>& body);

        unsigned int threadCount() const { return (unsigned int)_threads.size(); }

    private:
        void workerLoop();

        std::vector<std::thread> _threads;
        std::deque<std::function<void()>> _tasks;
        std::mutex _mutex;
        std::condition_variable _condition;
        bool _stop = false;
    };
}
