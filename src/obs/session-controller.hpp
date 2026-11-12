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

#include "guest-player.hpp"
#include "return-output.hpp"
#include "session/guest-service.hpp"
#include "settings-store.hpp"

#include <QObject>
#include <QString>

#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace qc {

/** A guest as the UI sees them: their seat plus live status. */
struct GuestView {
	GuestSeat seat;
	GuestStatus status;
};

/**
 * The plugin's hub: owns the settings, the guest service, the per-guest players and the return feed, and exposes
 * all of it to the dock as a small UI-thread API with Qt signals.
 *
 * Except where noted, methods must be called on the UI thread.
 */
class SessionController final : public QObject, private GuestServiceListener {
	Q_OBJECT

public:
	/** The live controller, or nullptr outside obs_module_load..obs_module_unload. */
	static SessionController *instance();

	SessionController();
	~SessionController() override;

	/** Stops the show and lets go of every libobs object. Called when OBS is exiting. */
	void shutdown();

	// --- Show ---
	const Settings &settings() const { return settings_; }
	/** Saves new settings. A running show is restarted when the change affects the connection. */
	void applySettings(const Settings &settings);
	void startShow();
	void stopShow();
	/** Returns true exactly once per installation, the first time it is called. */
	bool takeFirstRun();
	ServiceState state() const { return state_; }
	QString stateDetail() const { return stateDetail_; }
	bool showRunning() const { return state_ != ServiceState::Stopped && state_ != ServiceState::Failed; }

	// --- Guests ---
	std::vector<GuestView> guests() const;
	GuestSeat addGuest(const QString &name);
	void renameGuest(const std::string &guestId, const QString &name);
	void removeGuest(const std::string &guestId);
	QString inviteLink(const std::string &guestId) const;
	QString openInviteLink() const;

	bool isGuestInCurrentScene(const std::string &guestId) const;
	bool addGuestToCurrentScene(const std::string &guestId);
	void removeGuestFromCurrentScene(const std::string &guestId);
	bool isGuestMuted(const std::string &guestId) const;
	void setGuestMuted(const std::string &guestId, bool muted);
	void refreshGuestVideo(const std::string &guestId);
	void disconnectGuest(const std::string &guestId);
	/** Live media statistics; returns false when the guest is not sending. */
	bool guestMedia(const std::string &guestId, GuestMediaStats &stats, uint32_t &width, uint32_t &height) const;

	// --- Called by guest sources; safe from any thread ---
	void attachSource(const std::string &guestId, obs_source_t *source);
	void detachSource(const std::string &guestId, obs_source_t *source);
	std::vector<GuestSeat> seatsSnapshot() const;

signals:
	void stateChanged();
	/** Guests were added, removed or renamed. */
	void guestListChanged();
	void guestStatusChanged(const QString &guestId);

private:
	// GuestServiceListener (called on the service thread)
	void onServiceState(ServiceState state, const std::string &detail) override;
	void onGuestStatus(const GuestStatus &status) override;
	void onSeatCreated(const GuestSeat &seat) override;
	void onReturnFeedDemand(bool wanted) override;

	std::shared_ptr<GuestPlayer> ensurePlayer(const std::string &guestId);
	std::shared_ptr<GuestPlayer> findPlayer(const std::string &guestId) const;
	void excludeFromReturnAudio(obs_source_t *source) const;
	void persist();
	void publishSeats();

	Settings settings_;
	std::unique_ptr<GuestService> service_;
	std::unique_ptr<ReturnFeed> returnFeed_;

	ServiceState state_ = ServiceState::Stopped;
	QString stateDetail_;
	std::map<std::string, GuestStatus> statuses_;
	bool returnDemand_ = false;
	bool shutDown_ = false;

	/** Guards state that guest sources touch from other threads. */
	mutable std::mutex sharedMutex_;
	std::map<std::string, std::shared_ptr<GuestPlayer>> players_;
	std::vector<GuestSeat> seatsSnapshot_;
	int returnAudioTrack_ = 6;
};

} // namespace qc
