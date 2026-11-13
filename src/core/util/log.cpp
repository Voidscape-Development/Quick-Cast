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

#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace qc::log {

namespace {
std::mutex g_mutex;
std::shared_ptr<Sink> g_sink;
} // namespace

void setSink(Sink sink)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_sink = sink ? std::make_shared<Sink>(std::move(sink)) : nullptr;
}

void write(Level level, const char *format, ...)
{
	std::shared_ptr<Sink> sink;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		sink = g_sink;
	}
	if (!sink)
		return;

	va_list args;
	va_start(args, format);
	va_list copy;
	va_copy(copy, args);
	const int needed = std::vsnprintf(nullptr, 0, format, copy);
	va_end(copy);
	std::string message;
	if (needed > 0) {
		std::vector<char> buffer(static_cast<size_t>(needed) + 1);
		std::vsnprintf(buffer.data(), buffer.size(), format, args);
		message.assign(buffer.data(), static_cast<size_t>(needed));
	}
	va_end(args);

	(*sink)(level, message);
}

} // namespace qc::log
