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

#pragma once

#include <functional>
#include <string>

namespace qc::log {

enum class Level { Debug, Info, Warning, Error };

using Sink = std::function<void(Level, const std::string &)>;

/** Installs the process-wide log sink (the plugin routes this to OBS' log). Pass nullptr to silence. */
void setSink(Sink sink);

void write(Level level, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 2, 3)))
#endif
	;

} // namespace qc::log

#define QC_LOG_DEBUG(...) ::qc::log::write(::qc::log::Level::Debug, __VA_ARGS__)
#define QC_LOG_INFO(...) ::qc::log::write(::qc::log::Level::Info, __VA_ARGS__)
#define QC_LOG_WARN(...) ::qc::log::write(::qc::log::Level::Warning, __VA_ARGS__)
#define QC_LOG_ERROR(...) ::qc::log::write(::qc::log::Level::Error, __VA_ARGS__)
