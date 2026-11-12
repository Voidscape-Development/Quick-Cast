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

#include "rtp/receive-stream.hpp"
#include "rtp/sync-clock.hpp"
#include "rtp/video-depacketizer.hpp"

#include <rtc/rtc.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace qc::vdo {

int64_t monotonicNowUs();

/**
 * libdatachannel media handler for one incoming track.
 *
 * Sits directly on the track's receive path: it consumes RTP/RTCP, produces the RTCP feedback the sender needs, and
 * emits complete video frames or audio packets stamped on the guest's shared SyncClock. Output callbacks run on
 * libdatachannel's network thread and must return quickly.
 */
class MediaReceiver final : public rtc::MediaHandler {
public:
	struct VideoOutput {
		rtp::EncodedFrame frame;
		uint64_t timestampNs = 0;
	};
	struct AudioOutput {
		std::vector<uint8_t> payload;
		uint64_t timestampNs = 0;
	};

	struct Config {
		bool video = false;
		int payloadType = -1;
		uint32_t clockRate = 90000;
		rtp::VideoCodec videoCodec = rtp::VideoCodec::H264;
		uint64_t rembBitrateBps = 0;
		std::shared_ptr<rtp::SyncClock> clock;
		std::function<void(VideoOutput &&)> onVideo;
		std::function<void(AudioOutput &&)> onAudio;
	};

	explicit MediaReceiver(Config config);

	void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override;
	bool requestKeyframe(const rtc::message_callback &send) override;
	bool requestBitrate(unsigned int bitrate, const rtc::message_callback &send) override;

	rtp::ReceiveStats stats() const;
	/** Time of the most recent media packet, or 0 when nothing has arrived yet. */
	int64_t lastPacketUs() const;

private:
	Config config_;
	mutable std::mutex mutex_;
	rtp::ReceiveStream stream_;
	std::unique_ptr<rtp::VideoDepacketizer> depacketizer_;
	int64_t lastPacketUs_ = 0;
	bool reportedFirstSenderReport_ = false;
};

} // namespace qc::vdo
