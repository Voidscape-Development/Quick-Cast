#pragma once

#include "session/guest-service.hpp"

#include <memory>
#include <string>

namespace qc::vdo {

struct VdoServiceOptions {
	std::string signalingUrl = "wss://wss.vdo.ninja:443";
	/** Web app guests open; invite links are built against it. */
	std::string guestAppUrl = "https://vdo.ninja/";
};

/**
 * GuestService backed by the public VDO.Ninja signaling network.
 *
 * The host joins a VDO.Ninja room as its director, views every guest's stream over a dedicated peer connection, and
 * (optionally) seeds one stream of its own, the return feed, that guests' browsers pick up automatically.
 */
class VdoService final : public GuestService {
public:
	explicit VdoService(VdoServiceOptions options = {});
	~VdoService() override;

	void setListener(GuestServiceListener *listener) override;
	void start(const ShowConfig &config) override;
	void stop() override;

	void addSeat(const GuestSeat &seat) override;
	void renameSeat(const std::string &guestId, const std::string &name) override;
	void removeSeat(const std::string &guestId) override;

	std::string inviteLink(const ShowConfig &config, const GuestSeat &seat) const override;
	std::string openInviteLink(const ShowConfig &config) const override;

	void setMediaSink(const std::string &guestId, std::shared_ptr<GuestMediaSink> sink) override;
	void setGuestQuality(const std::string &guestId, const GuestQuality &quality) override;
	void requestKeyframe(const std::string &guestId) override;
	void disconnectGuest(const std::string &guestId) override;
	bool guestStats(const std::string &guestId, GuestMediaStats &stats) const override;

	void setReturnFeedFormat(const ReturnFeedFormat &format) override;
	void sendReturnVideo(const uint8_t *data, size_t size, int64_t timestampUs, bool keyframe) override;
	void sendReturnAudio(const uint8_t *data, size_t size, int64_t timestampUs) override;

private:
	class Impl;
	std::shared_ptr<Impl> impl_;
};

} // namespace qc::vdo
