#include "executor.hpp"

#include <future>

namespace qc {

Executor::Executor() : thread_([this] { run(); }) {}

Executor::~Executor()
{
	shutdown();
}

void Executor::shutdown()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_)
			return;
		stopping_ = true;
	}
	wake_.notify_all();
	if (thread_.joinable() && !onWorkerThread())
		thread_.join();
}

void Executor::post(Task task)
{
	postDelayed(std::chrono::milliseconds(0), std::move(task));
}

Executor::TimerId Executor::postDelayed(std::chrono::milliseconds delay, Task task)
{
	TimerId id;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_)
			return 0;
		id = nextId_++;
		queue_.emplace(Clock::now() + delay, Entry{id, std::move(task)});
	}
	wake_.notify_all();
	return id;
}

void Executor::cancel(TimerId id)
{
	if (id == 0)
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto it = queue_.begin(); it != queue_.end(); ++it) {
		if (it->second.id == id) {
			queue_.erase(it);
			return;
		}
	}
}

void Executor::runSync(const Task &task)
{
	if (onWorkerThread()) {
		task();
		return;
	}
	std::promise<void> done;
	auto finished = done.get_future();
	const TimerId id = postDelayed(std::chrono::milliseconds(0), [&] {
		task();
		done.set_value();
	});
	if (id == 0)
		return; // shutting down
	finished.wait();
}

void Executor::run()
{
	std::unique_lock<std::mutex> lock(mutex_);
	while (!stopping_) {
		if (queue_.empty()) {
			wake_.wait(lock);
			continue;
		}
		auto next = queue_.begin();
		if (next->first > Clock::now()) {
			wake_.wait_until(lock, next->first);
			continue;
		}
		Task task = std::move(next->second.task);
		queue_.erase(next);
		lock.unlock();
		task();
		lock.lock();
	}
	queue_.clear();
}

} // namespace qc
