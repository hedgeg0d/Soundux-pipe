#include "queue.hpp"
#include <chrono>
#include <fancy.hpp>

namespace Soundux::Objects
{
    void Queue::handle()
    {
        std::unique_lock lock(queueMutex);
        while (!stop)
        {
            cv.wait(lock, [&]() { return !queue.empty() || stop; });
            while (!queue.empty())
            {
                auto front = std::move(*queue.begin());

                lock.unlock();
                const auto started = std::chrono::steady_clock::now();
                Fancy::fancy.logTime().message() << "Audio task " << front.first << " started" << std::endl;

                front.second();

                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
                Fancy::fancy.logTime().message() << "Audio task " << front.first << " finished in " << ms << " ms"
                                                 << std::endl;
                if (ms > 1000)
                {
                    Fancy::fancy.logTime().warning() << "Audio task " << front.first
                                                    << " blocked the audio queue for " << ms << " ms" << std::endl;
                }

                lock.lock();

                queue.erase(front.first);
            }
        }
    }

    void Queue::push_unique(std::uint64_t id, std::function<void()> function)
    {
        {
            std::lock_guard lock(queueMutex);
            if (queue.find(id) != queue.end())
            {
                return;
            }
        }

        std::unique_lock lock(queueMutex);
        queue.emplace(id, std::move(function));
        lock.unlock();

        cv.notify_one();
    }

    Queue::Queue()
    {
        handler = std::thread([this] { handle(); });
    }
    Queue::~Queue()
    {
        stop = true;
        cv.notify_all();
        handler.join();
    }
} // namespace Soundux::Objects