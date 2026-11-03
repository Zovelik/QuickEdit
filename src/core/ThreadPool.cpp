#include <core/ThreadPool.h>

#include <core/Guard.h>

#include <algorithm>
#include <atomic>
#include <memory>


namespace core
{

ThreadPool::ThreadPool(unsigned int threadCount)
{
    if (threadCount == 0) {
        threadCount = std::max(1u, std::thread::hardware_concurrency() - 1);
    }

    for (unsigned int i = 0; i < threadCount; i++) {
        _threads.emplace_back([this]() { workerLoop(); });
    }
}


ThreadPool::~ThreadPool()
{
    {
        std::scoped_lock lock(_mutex);
        _stop = true;
        _tasks.clear();
    }

    _condition.notify_all();

    for (std::thread& thread : _threads) {
        thread.join();
    }
}


void ThreadPool::submit(std::function<void()> task)
{
    {
        std::scoped_lock lock(_mutex);
        _tasks.push_back(std::move(task));
    }

    _condition.notify_one();
}


void ThreadPool::parallelFor(size_t count, const std::function<void(size_t)>& body)
{
    if (count == 0) {
        return;
    }

    // Shared by the helpers, which may still be queued when the loop is done.
    struct State
    {
        std::atomic<size_t> next = 0;
        std::atomic<size_t> done = 0;
        std::mutex mutex;
        std::condition_variable finished;
    };

    auto state = std::make_shared<State>();
    const size_t total = count;

    auto run = [state, total, &body]() {
        size_t i;

        // An exception counts the item done: the loop still ends.
        while ((i = state->next.fetch_add(1)) < total) {
            guarded("Parallel task", [&]() { body(i); });

            if (state->done.fetch_add(1) + 1 == total) {
                std::scoped_lock lock(state->mutex);
                state->finished.notify_all();
            }
        }
    };

    // The helpers capture body by reference: they only call it while the
    // loop is not finished, and this function does not return before that.
    const size_t helpers = std::min<size_t>(count - 1, _threads.size());

    for (size_t i = 0; i < helpers; i++) {
        submit(run);
    }

    run();

    std::unique_lock lock(state->mutex);
    state->finished.wait(lock, [&]() { return state->done.load() == total; });
}


void ThreadPool::workerLoop()
{
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock lock(_mutex);
            _condition.wait(lock, [this]() { return _stop || !_tasks.empty(); });

            if (_stop) {
                return;
            }

            task = std::move(_tasks.front());
            _tasks.pop_front();
        }

        guarded("Background task", task);
    }
}

}
