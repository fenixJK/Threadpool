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

class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false), autoCreateThreads(false), timeLimit(std::chrono::milliseconds(100)) {
        newThreads(threads);
    }

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;
        auto task = std::make_shared<std::packaged_task<return_type()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop) throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task]() { (*task)(); });
        }
        condition.notify_one();
        return res;
    }

    void setAutoCreateThreads(bool enable, std::chrono::milliseconds limit = std::chrono::milliseconds(100), size_t Max_Threads = 0) {
        autoCreateThreads = enable;
        timeLimit = limit;
        MAX_THREAD_LIMIT = Max_Threads;
    }

    void removeThreads(size_t threads) {
        size_t threadsStopped = 0;
        auto it = workers.begin();
        while (it != workers.end() && threadsStopped < threads) {
            std::lock_guard<std::mutex> state_lock((*it)->state_mutex);
            if ((*it)->state == ThreadState::Waiting) {
                (*it)->state = ThreadState::Stopped;
                condition.notify_all();
                it = workers.erase(it);
                ++threadsStopped;
            }
            else {
                ++it;
            }
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (auto& worker : workers) {
            if (worker->thread.joinable()) worker->thread.join();
        }
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
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back(std::make_unique<Worker>(std::thread([this, i] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        if (this->condition.wait_for(lock, this->timeLimit, [this] { return this->stop || !this->tasks.empty(); })) {
                            if (this->stop && this->tasks.empty()) return; // Safely exit if stopping and no tasks
                            if (!this->tasks.empty()) {  // Check for empty queue
                                task = std::move(this->tasks.front());
                                this->tasks.pop();
                            }
                        }
                        if (this->autoCreateThreads && (workers.size() < MAX_THREAD_LIMIT || !MAX_THREAD_LIMIT)) {
                            this->newThreads(1);
                        }
                    }
                    if (task) {  // Ensure task is valid before calling
                        {
                            std::lock_guard<std::mutex> state_lock(this->workers[i]->state_mutex);
                            this->workers[i]->state = ThreadState::Running;
                        }
                        task(); // Safely execute the task
                        {
                            std::lock_guard<std::mutex> state_lock(this->workers[i]->state_mutex);
                            this->workers[i]->state = ThreadState::Waiting;
                        }
                    }
                }
                })));
        }
    }

    std::vector<std::unique_ptr<Worker>> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
    bool autoCreateThreads;
    std::chrono::milliseconds timeLimit;
    size_t MAX_THREAD_LIMIT = 0;
};
public:
    ThreadPool(size_t threads) : stop(false), autoCreateThreads(false), timeLimit(std::chrono::milliseconds(100)) {
        newThreads(threads);
    }

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;
        auto task = std::make_shared<std::packaged_task<return_type()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop) throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task]() { (*task)(); });
        }
        condition.notify_one();
        return res;
    }

    void setAutoCreateThreads(bool enable, std::chrono::milliseconds limit = std::chrono::milliseconds(100)) {
        autoCreateThreads = enable;
        timeLimit = limit;
    }

    void removeThreads(size_t threads) {
        size_t threadsStopped = 0;
        auto it = workers.begin();
        while (it != workers.end() && threadsStopped < threads) {
            std::lock_guard<std::mutex> state_lock((*it)->state_mutex);
            if ((*it)->state == ThreadState::Waiting) {
                (*it)->state = ThreadState::Stopped;
                condition.notify_all();
                it = workers.erase(it);
                ++threadsStopped;
            }
            else {
                ++it;
            }
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (auto& worker : workers) {
            if (worker->thread.joinable()) worker->thread.join();
        }
    }

private:
    void newThreads(size_t threads) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back(std::make_unique<Worker>(std::thread([this, i] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        if (this->condition.wait_for(lock, this->timeLimit, [this] { return this->stop || !this->tasks.empty(); })) {
                            if (this->stop && this->tasks.empty()) return; // Safely exit if stopping and no tasks
                            if (!this->tasks.empty()) {  // Check for empty queue
                                task = std::move(this->tasks.front());
                                this->tasks.pop();
                            }
                        }
                        if (this->autoCreateThreads && workers.size() < MAX_THREAD_LIMIT) {
                            this->newThreads(1);
                        }
                    }
                    if (task) {  // Ensure task is valid before calling
                        {
                            std::lock_guard<std::mutex> state_lock(this->workers[i]->state_mutex);
                            this->workers[i]->state = ThreadState::Running;
                        }
                        task(); // Safely execute the task
                        {
                            std::lock_guard<std::mutex> state_lock(this->workers[i]->state_mutex);
                            this->workers[i]->state = ThreadState::Waiting;
                        }
                    }
                }
                })));
        }
    }

    std::vector<std::unique_ptr<Worker>> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
    bool autoCreateThreads;
    std::chrono::milliseconds timeLimit;
    size_t MAX_THREAD_LIMIT = 20;
};