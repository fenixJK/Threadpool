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
        auto task = std::make_shared<std::packaged_task<return_type()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<return_type> res = task->get_future();
        size_t Task_Amount = 0;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop) throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task]() { (*task)(); });
            Task_Amount = tasks.size();
        }
        condition.notify_one();
        if (autoCreateThreads && MAX_AUTO_THREAD_LIMIT > 0) {
            std::thread([this, Task_Amount] {
                size_t Amount = 0;
                {
                    std::lock_guard<std::mutex> state_lock(this->Waiting_Mutex);
                    Amount = this->Waiting_Amount;
                }
                if (!(this->Waiting_Amount > Task_Amount)) {
                    size_t amount = Task_Amount - this->Waiting_Amount;
                    std::clamp(amount, size_t(0), this->MAX_AUTO_THREAD_LIMIT - this->AUTO_THREAD_AMOUNT);
                    this->newThreads(amount);
                    this->AUTO_THREAD_AMOUNT += amount;
                }
            });
        }
        return res;
    }

    void setAutoCreateThreads(bool enable, size_t Max_Threads = 0) {
        autoCreateThreads = enable;
        MAX_AUTO_THREAD_LIMIT = Max_Threads;
    }

    size_t getActiveThreadCount() {
        std::lock_guard<std::mutex> state_lock(Waiting_Mutex);
        return workers.size() - Waiting_Amount;
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
        ~Worker() { if (thread.joinable()) thread.join(); }
        std::thread thread;
        ThreadState state;
        std::mutex state_mutex;
    };

    void newThreads(size_t threads) {
        for (size_t i = 0; i < threads; ++i, this->threadID++) {
            workers.emplace(this->threadID, std::make_unique<Worker>(std::thread([this] {
                Worker* worker = this->workers[this->threadID].get();
                {
                    std::lock_guard<std::mutex> state_lock(worker->state_mutex);
                    worker->state = ThreadState::Waiting;
                    std::lock_guard<std::mutex> state_lock2(Waiting_Mutex);
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
                        if ((this->stop) || worker->state == ThreadState::Stopped) {
                            {
                                std::lock_guard<std::mutex> state_lock(Waiting_Mutex);
                                --Waiting_Amount;
                            }
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
                        task();
                        {
                            std::lock_guard<std::mutex> state_lock(worker->state_mutex);
                            if (worker->state != ThreadState::Stopped) {
                                worker->state = ThreadState::Waiting;
                                {
                                    std::lock_guard<std::mutex> state_lock(Waiting_Mutex);
                                    ++Waiting_Amount;
                                }
                            }
                        }
                    }
                }
            })));
        }
    }

    #if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
    void forceTerminateThread(std::thread& th) {
        TerminateThread(th.native_handle(), 0);
        th.detach();
    }
    #elif defined(__linux__) || defined(__APPLE__)
    #include <pthread.h>
    void forceTerminateThread(std::thread& th) {
        pthread_cancel(th.native_handle());
        th.detach();
    }
    #endif

    std::map<size_t, std::unique_ptr<Worker>> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
    bool autoCreateThreads;
    size_t MAX_AUTO_THREAD_LIMIT = 0;
    size_t AUTO_THREAD_AMOUNT = 0;
    size_t threadID = 0;
    std::mutex Waiting_Mutex;
    size_t Waiting_Amount = 0;
};