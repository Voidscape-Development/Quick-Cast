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

#include "session/guest-service.hpp"

#include <obs.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

namespace qc {

/**
 * Decodes one guest's media and feeds it to every OBS source bound to that guest.
 *
 * Network threads only enqueue; a dedicated thread per guest decodes, so a slow decode can never stall packet
 * reception for anyone. Nothing is decoded while no source is attached.
 */
class GuestPlayer final : public GuestMediaSink {
public:
	/** `requestKeyframe` is invoked (from the decode thread) when the decoder cannot continue without one. */
	explicit GuestPlayer(std::function<void()> requestKeyframe);
	~GuestPlayer() override;

	void addSource(obs_source_t *source);
	void removeSource(obs_source_t *source);
	bool hasSources() const;
	/** Applies `apply` to every attached source (e.g. to mute them). */
	void forEachSource(const std::function<void(obs_source_t *)> &apply) const;

	// GuestMediaSink
	void onVideoFrame(EncodedVideoFrame &&frame) override;
	void onAudioPacket(EncodedAudioPacket &&packet) override;
	void onMediaStopped() override;

	/** Takes effect from the next keyframe; safe to call from any thread. */
	void setHardwareDecoding(bool enabled) { hardwareDecoding_ = enabled; }

	uint32_t width() const { return width_.load(); }
	uint32_t height() const { return height_.load(); }

private:
	struct Stop {};
	using Item = std::variant<EncodedVideoFrame, EncodedAudioPacket, Stop>;

	void run();
	void enqueue(Item &&item);
	void clearSources();

	std::function<void()> requestKeyframe_;

	mutable std::mutex sourcesMutex_;
	std::vector<obs_source_t *> sources_;
	std::atomic<bool> haveSources_{false};

	std::mutex queueMutex_;
	std::condition_variable wake_;
	std::deque<Item> queue_;
	size_t queuedVideo_ = 0;
	bool dropUntilKeyframe_ = false;
	bool stopping_ = false;

	std::atomic<bool> hardwareDecoding_{true};
	std::atomic<uint32_t> width_{0};
	std::atomic<uint32_t> height_{0};

	std::thread thread_;
};

} // namespace qc
