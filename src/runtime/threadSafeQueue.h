#pragma once
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <optional>

template <typename T>
class ThreadSafeQueue
{
private:
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<T> queue;
    std::atomic<bool> running{true};

public:
    void enqueue(T item)
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            queue.push_back(std::move(item));
        }

        cv.notify_one();
    }

    std::optional<T> waitAndPop()
    {
        std::unique_lock<std::mutex> lock(mtx);

        cv.wait(lock, [&]
                { return !running.load() || !queue.empty(); });

        if (!running.load() && queue.empty())
            return std::nullopt;

        T item = std::move(queue.front());
        queue.pop_front();

        return item;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            running.store(false);
        }

        cv.notify_all();
    }
};
