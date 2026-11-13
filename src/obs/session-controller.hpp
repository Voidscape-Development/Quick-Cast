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
#include "layout/tile-layout.hpp"
#include "return-output.hpp"
#include "session/guest-service.hpp"
#include "settings-store.hpp"

#include <QObject>
#include <QString>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace qc {

/** Outcome of asking to join the show an invite link leads to. */
enum class JoinResult {
	Joining,
	/** The text is not an invite link. */
	NotAnInvite,
	/** The link leads to the user's own show. */
	OwnShow,
};

/** A guest as the UI sees them: their seat plus live status. */
struct GuestView {
	GuestSeat seat;
	GuestStatus status;
};

/**
 * The plugin's hub: owns the settings, the guest service, the per-guest players and the return feed, and exposes
 * all of it to the dock as a small UI-thread API with Qt signals.
 *
 * At any moment the plugin is in at most one show: the user's own, which they host, or a collaborator's, which they
 * joined as a guest. The "guests" the UI lists are the other people in whichever show that is.
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

	// --- Someone else's show ---
	/** Whether the text is an invite link Quick Cast can join. */
	bool isInviteLink(const QString &link) const;
	/**
	 * Takes our seat in the show an invite link leads to, ending our own show first if it is running. The show is
	 * remembered as a collaborator; `name` labels it and may be empty.
	 */
	JoinResult joinShow(const QString &link, const QString &name);
	/** Joins a remembered show again. */
	bool joinCollaborator(const std::string &collaboratorId);
	/** True while we are a guest in someone else's show (stopShow leaves it). */
	bool joined() const { return !activeCollaborator_.empty(); }
	/** Whether the most recent show was someone else's; tells the UI how to word an error after it ended. */
	bool lastShowWasJoined() const { return lastShowWasJoined_; }
	/** What to call the show we are in, or the given collaborator's. */
	QString collaboratorName(const std::string &collaboratorId) const;
	QString joinedShowName() const { return collaboratorName(activeCollaborator_); }
	/** Remembered shows, most recently joined first. */
	std::vector<Collaborator> collaborators() const;
	void renameCollaborator(const std::string &collaboratorId, const QString &name);
	/** Forgets a show and the people met in it. Leaves it first if we are in it. */
	void removeCollaborator(const std::string &collaboratorId);

	// --- Layouts ---
	/**
	 * Arranges the guests in the current scene (and, for the layouts that fill the canvas, the user's own camera
	 * if they asked for that and it is in the scene). When no guest is in the scene yet, everyone who is connected
	 * is added first. Returns how many items were placed; 0 means there was nothing to arrange.
	 */
	size_t applyLayout(LayoutKind kind);
	void setLayoutIncludesCamera(bool include);

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
	void setSourceOfflineAppearance(const std::string &guestId, obs_source_t *source, OfflineMode mode,
					const std::string &imagePath);
	/** Video bitrate to request from one guest instead of the show default; 0 removes the override. */
	void setGuestBitrateOverride(const std::string &guestId, uint32_t kbps);
	std::vector<GuestSeat> seatsSnapshot() const;

signals:
	void stateChanged();
	/** Guests were added, removed or renamed, or the show they belong to changed. */
	void guestListChanged();
	/** Remembered shows were added, removed or renamed. */
	void collaboratorsChanged();
	void guestStatusChanged(const QString &guestId);

private:
	// GuestServiceListener (called on the service thread)
	void onServiceState(ServiceState state, const std::string &detail) override;
	void onGuestStatus(const GuestStatus &status) override;
	void onSeatCreated(const GuestSeat &seat) override;
	void onReturnFeedDemand(bool wanted) override;

	/** Seats of the show we are in: our own show's unless we have joined someone else's. */
	const std::vector<GuestSeat> &activeSeats() const;
	/** The list a seat lives in (ours, or one collaborator's), whichever show is active. */
	std::vector<GuestSeat> *seatListFor(const std::string &guestId);
	GuestSeat *findSeat(const std::string &guestId);
	const GuestSeat *findSeat(const std::string &guestId) const;
	bool isCollaboratorSeat(const std::string &guestId) const;
	void connectAs(const ShowConfig &config);
	void configureFeeds();
	std::shared_ptr<GuestPlayer> ensurePlayer(const std::string &guestId);
	std::shared_ptr<GuestPlayer> findPlayer(const std::string &guestId) const;
	void excludeFromReturnAudio(obs_source_t *source) const;
	void persist();
	void publishSeats();
	void applyBitrateOverrides();

	Settings settings_;
	std::unique_ptr<GuestService> service_;
	std::unique_ptr<ReturnFeed> returnFeed_;
	std::unique_ptr<ReturnFeed> secondFeed_;

	ServiceState state_ = ServiceState::Stopped;
	QString stateDetail_;
	/** The collaborator whose show we are in; empty while hosting or idle. */
	std::string activeCollaborator_;
	bool lastShowWasJoined_ = false;
	std::map<std::string, GuestStatus> statuses_;
	bool returnDemand_ = false;
	/** Read by guest sources on other threads. */
	std::atomic<bool> shutDown_{false};

	/** Guards state that guest sources touch from other threads. */
	mutable std::mutex sharedMutex_;
	std::map<std::string, std::shared_ptr<GuestPlayer>> players_;
	std::vector<GuestSeat> seatsSnapshot_;
	int returnAudioTrack_ = 6;
	GuestQuality defaultQuality_;
	std::map<std::string, uint32_t> bitrateOverrides_;
	std::atomic<bool> hardwareDecoding_{true};
};

} // namespace qc
