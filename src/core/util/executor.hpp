#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace qc {

/**
 * A single worker thread that runs posted tasks in order, plus delayed tasks.
 *
 * Network callbacks arrive on libdatachannel's threads, where it is not safe to tear down the object that is calling
 * back. Everything that mutates session state is therefore funnelled through one Executor, which also removes the
 * need for fine-grained locking inside the session.
 */
class Executor {
public:
	using Task = std::function<void()>;
	using TimerId = uint64_t;

	Executor();
	~Executor();

	Executor(const Executor &) = delete;
	Executor &operator=(const Executor &) = delete;

	void post(Task task);
	TimerId postDelayed(std::chrono::milliseconds delay, Task task);
	void cancel(TimerId id);

	/** Runs `task` on the worker and waits for it. Runs inline when already on the worker thread. */
	void runSync(const Task &task);

	bool onWorkerThread() const { return std::this_thread::get_id() == thread_.get_id(); }

	/** Stops accepting work, drops pending tasks and joins the worker. Idempotent. */
	void shutdown();

private:
	using Clock = std::chrono::steady_clock;
	struct Entry {
		TimerId id;
		Task task;
	};

	void run();

	std::mutex mutex_;
	std::condition_variable wake_;
	std::multimap<Clock::time_point, Entry> queue_;
	TimerId nextId_ = 1;
	bool stopping_ = false;
	std::thread thread_;
};

} // namespace qc
