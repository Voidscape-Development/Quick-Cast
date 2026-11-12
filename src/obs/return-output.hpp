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
#include "settings-store.hpp"

#include <obs.hpp>

namespace qc {

/** Registers the "quick_cast_return_output" output type with libobs. Call once from obs_module_load. */
void registerReturnOutput();

/**
 * The return feed: OBS' program video and one audio track, encoded with a dedicated low-latency encoder pair and
 * handed to the GuestService.
 *
 * It uses its own encoders so it never interferes with streaming or recording, and it only runs while at least one
 * guest is actually watching. All methods must be called on the UI thread.
 */
class ReturnFeed {
public:
	explicit ReturnFeed(GuestService *service);
	~ReturnFeed();

	/** Applies new settings; restarts the encoders if they are running. */
	void configure(const Settings &settings);
	void start();
	void stop();
	bool active() const;

	/** Releases every libobs object. Must be called before libobs shuts down. */
	void release();

private:
	bool createObjects();

	GuestService *service_;
	Settings settings_;
	bool wanted_ = false;
	OBSOutputAutoRelease output_;
	OBSEncoderAutoRelease videoEncoder_;
	OBSEncoderAutoRelease audioEncoder_;
};

} // namespace qc
