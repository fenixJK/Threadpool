#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>

enum ThreadState {
    Waiting,
    Running,
    Stopped
};

class Worker {
public:
    Worker(std::thread* thread) {
        this->thread = thread;
        this->state = Waiting;
    }
    ~Worker() { if (this->thread->joinable()) this->thread->join(); }
    std::thread* thread;
    ThreadState state;
    std::mutex state_mutex;
private:
};

class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false) { newThreads(threads); this->threadCount = 0; threadPools.push_back(this); }
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type> {
        using return_type = typename std::result_of<F(Args...)>::type;
        auto task = std::make_shared< std::packaged_task<return_type()> >(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop) { perror("enqueue on stopped ThreadPool"); return std::future<return_type>(); }
            tasks.emplace([task]{ (*task)(); });
        }
        condition.notify_one();
        return res;
    }
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (Worker* worker : workers) { delete worker; }
        workers.clear();
        threadPools.erase(std::remove(threadPools.begin(), threadPools.end(), this), threadPools.end());
    }
    void addThreads(size_t threads) { newThreads(threads+this->threadCount); }
    void removeThreads(size_t threads) {
        // in progress
    }
    static std::vector<ThreadPool*> threadPools;
private:
    void newThreads(size_t threads) {
        for (; threadCount < threads; ++threadCount) {
            std::thread workerThread([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                        if (this->stop || this->tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    {
                        std::lock_guard<std::mutex> lock(this->workers[this->threadCount]->state_mutex);
                        this->workers[this->threadCount]->state = ThreadState::Running;
                    }
                    task();
                    {
                        std::lock_guard<std::mutex> lock(this->workers[this->threadCount]->state_mutex);
                        this->workers[this->threadCount]->state = ThreadState::Waiting;
                    }
                }
                });
            Worker* worker = new Worker(&workerThread);
            workers.emplace_back(worker);
        }
    }
    std::vector<Worker*> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
    size_t threadCount;
};
std::vector<ThreadPool*> ThreadPool::threadPools;