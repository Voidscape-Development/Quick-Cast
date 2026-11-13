/*
Quick Cast - remote guests for OBS Studio
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "executor.hpp"

#include <future>
#include <memory>

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
	// The promise lives only inside the task: if the executor shuts down and drops the task unrun, the promise is
	// destroyed with it and the wait below ends instead of hanging forever.
	auto done = std::make_shared<std::promise<void>>();
	auto finished = done->get_future();
	const TimerId id = postDelayed(std::chrono::milliseconds(0), [&task, done = std::move(done)] {
		task();
		done->set_value();
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
