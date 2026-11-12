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
