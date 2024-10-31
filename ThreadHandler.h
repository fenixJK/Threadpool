#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>
#include <algorithm>
#include <memory>
#include <map>

class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false), autoCreateThreads(false) {
        newThreads(threads);
    }

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<return_type> res = task->get_future();
        size_t Amount = 0;
        {
            std::lock_guard<std::mutex> state_lock(this->waiting_mutex);
            Amount = this->Waiting_Amount;
        }
        if (autoCreateThreads && Amount <= 0) {
            std::thread([this] {
                (*task)();
            });
            return res;
        }
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop) throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task]() { (*task)(); });
        }
        condition.notify_one();
        return res;
    }

    void setAutoCreateThreads(bool enable) {
        autoCreateThreads = enable;
    }

    void removeThreads(size_t threads) {
        if (workers.size() < threads) throw std::runtime_error("attempted to stop more then the amount of threads");
        size_t threadsStopped = 0;
        while (threadsStopped < threads) {
            auto it = workers.begin();
            while (it != workers.end() && threadsStopped < threads) {
                {
                    std::lock_guard<std::mutex> state_lock(it->second->state_mutex);
                    it->second->state = ThreadState::Stopped;
                }
                condition.notify_all();
                it = workers.erase(it);
                ++threadsStopped;
            }
        }
    }

    void addThreads(size_t threads) {
        newThreads(threads);
    }

    size_t gethreadCount() { return workers.size(); }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
    }

private:
    enum class ThreadState {
        Waiting,
        Running,
        Stopped
    };

    class Worker {
    public:
        Worker(std::thread&& thread) : thread(std::move(thread)), state(ThreadState::Waiting) {}
        Worker() : state(ThreadState::Stopped) {}
        ~Worker() { if (thread.joinable()) thread.join(); }
        std::thread thread;
        ThreadState state;
        std::mutex state_mutex;
    };

    void newThreads(size_t threads) {
        for (size_t i = 0; i < threads; ++i, threadID++) {
            workers.emplace(threadID, std::make_unique<Worker>());
            Worker* worker = workers[this->threadID].get();
            worker->thread = std::thread([this, worker] {
                {
                    std::lock_guard<std::mutex> state_lock(worker->state_mutex);
                    worker->state = ThreadState::Waiting;
                }
                {
                    std::lock_guard<std::mutex> waiting_lock(waiting_mutex);
                    ++Waiting_Amount;
                }
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this, worker] {
                            std::lock_guard<std::mutex> state_guard(worker->state_mutex);
                            return this->stop || worker->state == ThreadState::Stopped || !this->tasks.empty(); 
                        });
                        std::lock_guard<std::mutex> state_guard(worker->state_mutex);
                        if ((this->stop && this->tasks.empty()) || worker->state == ThreadState::Stopped) {
                            return;
                        }
                        else {
                            task = std::move(this->tasks.front());
                            this->tasks.pop();
                        }
                    }
                    if (task) {
                        {
                            std::lock_guard<std::mutex> state_lock(worker->state_mutex);
                            if (worker->state != ThreadState::Stopped) {
                                worker->state = ThreadState::Running;
                            }
                        }
                        {
                            std::lock_guard<std::mutex> waiting_lock(waiting_mutex);
                            --Waiting_Amount;
                        }
                        task();
                        {
                            std::unique_lock<std::mutex> state_lock(worker->state_mutex);
                            if (worker->state != ThreadState::Stopped) {
                                worker->state = ThreadState::Waiting;
                                state_lock.unlock();
                                std::lock_guard<std::mutex> waiting_lock(waiting_mutex);
                                ++Waiting_Amount;
                            }
                        }
                    }
                }
            });
        }
    }
    std::map<size_t, std::unique_ptr<Worker>> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::mutex waiting_mutex;
    std::condition_variable condition;
    bool stop;
    size_t threadID = 0;
    size_t Waiting_Amount = 0;
    bool autoCreateThreads;
};