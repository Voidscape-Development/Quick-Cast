#pragma once

#include "reorder-buffer.hpp"
#include "rtcp.hpp"
#include "rtp-packet.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace qc::rtp {

/** Snapshot of receive-side statistics for one RTP stream. */
struct ReceiveStats {
	uint64_t packetsReceived = 0;
	uint64_t bytesReceived = 0;
	int64_t packetsLost = 0;
	uint64_t nacksSent = 0;
	uint64_t plisSent = 0;
	/** Interarrival jitter in milliseconds (RFC 3550). */
	double jitterMs = 0.0;
	/** Loss over the most recent reporting interval, 0..1. */
	double recentLossFraction = 0.0;
};

/**
 * Receive-side state for a single RTP stream: sequence tracking, loss/jitter statistics, reordering, and the RTCP
 * feedback (receiver reports, NACK, PLI, REMB) a WebRTC sender needs in order to keep its bitrate up.
 *
 * Transport agnostic: feed it raw RTP/RTCP and give it a callback for RTCP it wants to send. Not thread safe; the
 * owner serialises access.
 */
class ReceiveStream {
public:
	struct Config {
		uint32_t clockRate = 90000;
		/** Retransmission requests; pointless for audio where late data is useless. */
		bool enableNack = true;
		ReorderBuffer::Config reorder;
		/** Bitrate advertised to the sender via REMB; 0 disables REMB. */
		uint64_t rembBitrateBps = 0;
		/** SSRC we identify ourselves with in RTCP. */
		uint32_t localSsrc = 1;
		int64_t reportIntervalUs = 1'000'000;
	};

	using RtcpSender = std::function<void(std::vector<uint8_t> &&)>;
	using PacketSink = std::function<void(Packet &&)>;
	using LossSink = std::function<void(int64_t)>;

	explicit ReceiveStream(const Config &config);

	/** Handles one incoming RTP packet; in-order packets come out through `onPacket`. */
	void receiveRtp(const uint8_t *data, size_t size, int64_t nowUs, const PacketSink &onPacket,
			const LossSink &onLoss, const RtcpSender &sendRtcp);

	/** Handles one incoming (compound) RTCP packet. */
	void receiveRtcp(const uint8_t *data, size_t size, int64_t nowUs);

	/** Asks the sender for a keyframe (rate limited). Returns false when suppressed or the SSRC is unknown. */
	bool requestKeyframe(int64_t nowUs, const RtcpSender &sendRtcp);

	void setRembBitrate(uint64_t bitrateBps) { config_.rembBitrateBps = bitrateBps; }

	ReceiveStats stats() const { return stats_; }
	std::optional<uint32_t> remoteSsrc() const { return haveSsrc_ ? std::optional<uint32_t>(ssrc_) : std::nullopt; }

	/** Most recent sender report and the local time it arrived, used for A/V sync. */
	struct SenderReportInfo {
		SenderReport report;
		int64_t arrivalUs = 0;
	};
	std::optional<SenderReportInfo> lastSenderReport() const { return lastSenderReport_; }

private:
	void sendReports(int64_t nowUs, const RtcpSender &sendRtcp);
	void resetForNewSsrc(uint32_t ssrc);

	Config config_;
	ReorderBuffer reorder_;
	Unwrapper<uint16_t> sequenceUnwrapper_;
	ReceiveStats stats_;

	bool haveSsrc_ = false;
	uint32_t ssrc_ = 0;
	int64_t baseSequence_ = 0;
	int64_t highestSequence_ = 0;

	// Jitter estimation
	bool haveTransit_ = false;
	int64_t lastTransit_ = 0;
	double jitter_ = 0.0; // in RTP clock units

	// Interval accounting for the receiver report's fraction-lost field
	int64_t expectedPrior_ = 0;
	int64_t receivedPrior_ = 0;
	uint64_t receivedInOrder_ = 0;

	int64_t lastReportUs_ = 0;
	int64_t lastKeyframeRequestUs_ = 0;
	std::optional<SenderReportInfo> lastSenderReport_;
};

} // namespace qc::rtp
