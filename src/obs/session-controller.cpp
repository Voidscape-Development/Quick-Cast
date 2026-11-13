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

#include "session-controller.hpp"

#include "scene-integration.hpp"
#include "vdo/identity.hpp"
#include "vdo/vdo-service.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QDateTime>
#include <QMetaObject>

#include <algorithm>

namespace qc {

namespace {
SessionController *g_instance = nullptr;
constexpr const char *kPlaceholderName = "Guest";
/** Seat (after the collaborator's prefix) under which the host of a joined show appears. */
constexpr const char *kHostSeatSuffix = "host";
} // namespace

SessionController *SessionController::instance()
{
	return g_instance;
}

SessionController::SessionController()
{
	settings_ = loadSettings();
	service_ = std::make_unique<vdo::VdoService>();
	service_->setListener(this);
	returnFeed_ = std::make_unique<ReturnFeed>(service_.get(), ReturnFeedId::Main);
	returnFeed_->configure(settings_);
	secondFeed_ = std::make_unique<ReturnFeed>(service_.get(), ReturnFeedId::Second);
	secondFeed_->configure(settings_);

	publishSeats();
	// Every seat we know gets a player, whichever show it belongs to: OBS sources bind to seats, and a source for
	// a collaborator's host has to work the moment we join their show.
	for (const GuestSeat &seat : settings_.seats)
		ensurePlayer(seat.id);
	for (const Collaborator &collaborator : settings_.collaborators) {
		for (const GuestSeat &seat : collaborator.seats)
			ensurePlayer(seat.id);
	}

	g_instance = this;
}

SessionController::~SessionController()
{
	shutdown();
	g_instance = nullptr;
}

void SessionController::shutdown()
{
	if (shutDown_.exchange(true))
		return;

	if (returnFeed_)
		returnFeed_->release();
	if (secondFeed_)
		secondFeed_->release();
	if (service_) {
		service_->stop();
		service_->setListener(nullptr);
	}
	state_ = ServiceState::Stopped;
}

void SessionController::persist()
{
	saveSettings(settings_);
	publishSeats();
}

void SessionController::publishSeats()
{
	hardwareDecoding_ = settings_.hardwareDecoding;
	std::lock_guard<std::mutex> lock(sharedMutex_);
	defaultQuality_ = settings_.toShowConfig().defaultQuality;
	// Guest sources can be bound to anyone we know, so their picker lists the people in remembered shows too.
	seatsSnapshot_ = settings_.seats;
	for (const Collaborator &collaborator : settings_.collaborators)
		seatsSnapshot_.insert(seatsSnapshot_.end(), collaborator.seats.begin(), collaborator.seats.end());
	for (auto &[id, player] : players_)
		player->setHardwareDecoding(settings_.hardwareDecoding);
	returnAudioTrack_ = settings_.returnAudioTrack;
}

std::vector<GuestSeat> SessionController::seatsSnapshot() const
{
	std::lock_guard<std::mutex> lock(sharedMutex_);
	return seatsSnapshot_;
}

// ---------------------------------------------------------------------------------------------------------------------
// Show
// ---------------------------------------------------------------------------------------------------------------------

void SessionController::startShow()
{
	if (shutDown_ || showRunning())
		return;
	lastShowWasJoined_ = false;
	connectAs(settings_.toShowConfig());
}

void SessionController::connectAs(const ShowConfig &config)
{
	state_ = ServiceState::Connecting;
	stateDetail_.clear();
	emit stateChanged();
	service_->start(config);
	// Starting a show resets every guest to the default quality; put the per-guest choices back.
	applyBitrateOverrides();
}

void SessionController::configureFeeds()
{
	// What we send depends on whose show it is: guests of our own show get the return feed as configured, while
	// in someone else's show we send our camera and microphone.
	const Settings effective = joined() ? settings_.forJoinedShow() : settings_;
	returnFeed_->configure(effective);
	secondFeed_->configure(effective);
}

bool SessionController::takeFirstRun()
{
	if (settings_.dockIntroduced)
		return false;
	settings_.dockIntroduced = true;
	persist();
	return true;
}

void SessionController::stopShow()
{
	if (shutDown_)
		return;
	returnFeed_->stop();
	secondFeed_->stop();
	returnDemand_ = false;
	service_->stop();
	// stop() is synchronous, but its final notifications are still queued; settle the UI state now.
	state_ = ServiceState::Stopped;
	stateDetail_.clear();
	for (auto &[id, status] : statuses_) {
		status.connection = GuestConnection::Offline;
		status.receivingReturnFeed = false;
	}
	if (joined()) {
		// Leaving someone else's show puts us back in front of our own.
		activeCollaborator_.clear();
		configureFeeds();
	}
	emit stateChanged();
	emit guestListChanged();
}

// ---------------------------------------------------------------------------------------------------------------------
// Someone else's show
// ---------------------------------------------------------------------------------------------------------------------

bool SessionController::isInviteLink(const QString &link) const
{
	return service_->parseInviteLink(link.trimmed().toStdString()).has_value();
}

JoinResult SessionController::joinShow(const QString &link, const QString &name)
{
	auto target = service_->parseInviteLink(link.trimmed().toStdString());
	if (!target)
		return JoinResult::NotAnInvite;
	// Joining our own show would have us talking to ourselves.
	const ShowConfig own = settings_.toShowConfig();
	if (target->room == vdo::sanitizeId(own.room, vdo::kMaxRoomIdLength) && target->password == own.password)
		return JoinResult::OwnShow;

	// The same show again, perhaps through a newer link: keep the collaborator and the people already met there.
	Collaborator *collaborator = nullptr;
	for (Collaborator &existing : settings_.collaborators) {
		if (existing.target.room == target->room && existing.target.password == target->password)
			collaborator = &existing;
	}
	if (!collaborator) {
		Collaborator created;
		do {
			created.id = vdo::generateStreamId(6);
		} while (settings_.findCollaborator(created.id));
		settings_.collaborators.push_back(std::move(created));
		collaborator = &settings_.collaborators.back();
	}

	// An open link reserves no seat. Choose one now and keep it, so that the host sees the same person, in the
	// same place in their scenes, every time we come back.
	if (target->seatId.empty())
		target->seatId = collaborator->target.seatId.empty() ? vdo::generateStreamId(10)
								     : collaborator->target.seatId;
	collaborator->target = *target;
	if (!name.trimmed().isEmpty())
		collaborator->name = name.trimmed().toStdString();

	const std::string id = collaborator->id;
	persist();
	emit collaboratorsChanged();
	return joinCollaborator(id) ? JoinResult::Joining : JoinResult::NotAnInvite;
}

bool SessionController::joinCollaborator(const std::string &collaboratorId)
{
	if (shutDown_ || !settings_.findCollaborator(collaboratorId))
		return false;
	if (showRunning())
		stopShow();

	Collaborator *collaborator = settings_.findCollaborator(collaboratorId);
	collaborator->lastJoined = QDateTime::currentSecsSinceEpoch();
	persist();

	activeCollaborator_ = collaboratorId;
	lastShowWasJoined_ = true;
	configureFeeds();
	connectAs(settings_.toJoinConfig(*collaborator));
	emit collaboratorsChanged();
	emit guestListChanged();
	return true;
}

QString SessionController::collaboratorName(const std::string &collaboratorId) const
{
	const Collaborator *collaborator = settings_.findCollaborator(collaboratorId);
	if (!collaborator)
		return {};
	// Until the user names the show or its host introduces themselves, the room name is all we have.
	return QString::fromStdString(collaborator->name.empty() ? collaborator->target.room : collaborator->name);
}

std::vector<Collaborator> SessionController::collaborators() const
{
	std::vector<Collaborator> result = settings_.collaborators;
	std::stable_sort(result.begin(), result.end(),
			 [](const Collaborator &a, const Collaborator &b) { return a.lastJoined > b.lastJoined; });
	return result;
}

void SessionController::renameCollaborator(const std::string &collaboratorId, const QString &name)
{
	Collaborator *collaborator = settings_.findCollaborator(collaboratorId);
	const std::string newName = name.trimmed().toStdString();
	if (!collaborator || newName.empty() || collaborator->name == newName)
		return;
	collaborator->name = newName;
	persist();
	emit collaboratorsChanged();
	emit stateChanged();
}

void SessionController::removeCollaborator(const std::string &collaboratorId)
{
	const Collaborator *collaborator = settings_.findCollaborator(collaboratorId);
	if (!collaborator)
		return;
	if (activeCollaborator_ == collaboratorId)
		stopShow();
	for (const GuestSeat &seat : collaborator->seats) {
		service_->removeSeat(seat.id);
		statuses_.erase(seat.id);
	}
	settings_.collaborators.erase(
		std::remove_if(settings_.collaborators.begin(), settings_.collaborators.end(),
			       [&](const Collaborator &entry) { return entry.id == collaboratorId; }),
		settings_.collaborators.end());
	persist();
	emit collaboratorsChanged();
	emit guestListChanged();
}

// ---------------------------------------------------------------------------------------------------------------------
// Layouts
// ---------------------------------------------------------------------------------------------------------------------

size_t SessionController::applyLayout(LayoutKind kind)
{
	const std::vector<GuestSeat> seats = activeSeats();
	const auto inScene = [&] {
		std::vector<OBSSourceAutoRelease> found;
		for (const GuestSeat &seat : seats) {
			OBSSourceAutoRelease source = findGuestSource(seat.id);
			if (source && isInCurrentScene(source))
				found.push_back(std::move(source));
		}
		return found;
	};

	std::vector<OBSSourceAutoRelease> guestSources = inScene();
	if (guestSources.empty()) {
		// Nobody on screen yet: "side by side" then means "put everyone who is here on screen, side by side".
		for (const GuestSeat &seat : seats) {
			const auto status = statuses_.find(seat.id);
			if (status != statuses_.end() && status->second.connection == GuestConnection::Connected)
				addGuestToCurrentScene(seat.id);
		}
		guestSources = inScene();
	}
	if (guestSources.empty())
		return 0;

	std::vector<obs_source_t *> sources;
	// The layouts that fill the canvas are about us and them together, so our camera takes the first tile.
	// Picture-in-picture only tucks the guests into a corner of whatever the scene already shows.
	OBSSourceAutoRelease camera;
	if (settings_.layoutIncludesCamera && kind != LayoutKind::PictureInPicture) {
		const std::string cameraName = settings_.collabVideoSource.empty() ? detectCameraSourceName()
										   : settings_.collabVideoSource;
		if (!cameraName.empty())
			camera = obs_get_source_by_name(cameraName.c_str());
		if (camera && isInCurrentScene(camera))
			sources.push_back(camera);
	}
	for (const OBSSourceAutoRelease &source : guestSources)
		sources.push_back(source);
	return arrangeInCurrentScene(sources, kind);
}

void SessionController::setLayoutIncludesCamera(bool include)
{
	if (settings_.layoutIncludesCamera == include)
		return;
	settings_.layoutIncludesCamera = include;
	persist();
}

void SessionController::applySettings(const Settings &settings)
{
	const bool connectionChanged =
		settings.room != settings_.room || settings.password != settings_.password ||
		settings.hostName != settings_.hostName || settings.returnFeedEnabled != settings_.returnFeedEnabled ||
		settings.secondFeedEnabled != settings_.secondFeedEnabled ||
		settings.guestBitrateKbps != settings_.guestBitrateKbps ||
		settings.preferredCodec != settings_.preferredCodec || settings.turnUrl != settings_.turnUrl ||
		settings.turnUsername != settings_.turnUsername || settings.turnPassword != settings_.turnPassword;
	const bool returnChanged = settings.returnAudioTrack != settings_.returnAudioTrack ||
				   settings.returnHeight != settings_.returnHeight ||
				   settings.returnVideoSource != settings_.returnVideoSource ||
				   settings.returnAudioSource != settings_.returnAudioSource ||
				   settings.secondFeedEnabled != settings_.secondFeedEnabled ||
				   settings.secondVideoSource != settings_.secondVideoSource ||
				   settings.secondHeight != settings_.secondHeight ||
				   settings.secondVideoBitrateKbps != settings_.secondVideoBitrateKbps ||
				   settings.returnVideoBitrateKbps != settings_.returnVideoBitrateKbps ||
				   settings.returnAudioBitrateKbps != settings_.returnAudioBitrateKbps ||
				   settings.returnFeedEnabled != settings_.returnFeedEnabled ||
				   settings.collabVideoSource != settings_.collabVideoSource ||
				   settings.collabAudioSource != settings_.collabAudioSource ||
				   settings.collabSecondEnabled != settings_.collabSecondEnabled ||
				   settings.collabSecondSource != settings_.collabSecondSource ||
				   settings.collabHeight != settings_.collabHeight ||
				   settings.collabVideoBitrateKbps != settings_.collabVideoBitrateKbps;
	// Whether a guest publishes a second feed is part of how they join.
	const bool joinChanged =
		settings.collabSecondEnabled != settings_.collabSecondEnabled ||
		settings.collabSecondSource.empty() != settings_.collabSecondSource.empty() ||
		settings.hostName != settings_.hostName || settings.guestBitrateKbps != settings_.guestBitrateKbps ||
		settings.preferredCodec != settings_.preferredCodec || settings.turnUrl != settings_.turnUrl ||
		settings.turnUsername != settings_.turnUsername || settings.turnPassword != settings_.turnPassword;
	const int previousTrack = settings_.returnAudioTrack;

	// Seats and collaborators are managed through the dock, not the settings dialog.
	const std::vector<GuestSeat> seats = settings_.seats;
	const std::vector<Collaborator> collaborators = settings_.collaborators;
	settings_ = settings;
	settings_.seats = seats;
	settings_.collaborators = collaborators;
	persist();

	if (previousTrack != settings_.returnAudioTrack) {
		// Give guest sources their old track back and take them out of the new one.
		std::vector<std::shared_ptr<GuestPlayer>> players;
		{
			std::lock_guard<std::mutex> lock(sharedMutex_);
			for (auto &[id, player] : players_)
				players.push_back(player);
		}
		for (auto &player : players) {
			player->forEachSource([&](obs_source_t *source) {
				uint32_t mixers = obs_source_get_audio_mixers(source);
				mixers |= 1u << (previousTrack - 1);
				mixers &= ~(1u << (settings_.returnAudioTrack - 1));
				obs_source_set_audio_mixers(source, mixers);
			});
		}
	}

	if (returnChanged)
		configureFeeds();

	if (joined()) {
		// Our own show's details do not matter to the one we are in; rejoin only for what does.
		if (joinChanged && showRunning())
			joinCollaborator(std::string(activeCollaborator_));
	} else if (connectionChanged && showRunning()) {
		stopShow();
		startShow();
	}
	emit guestListChanged();
}

// ---------------------------------------------------------------------------------------------------------------------
// Guests
// ---------------------------------------------------------------------------------------------------------------------

std::vector<GuestView> SessionController::guests() const
{
	std::vector<GuestView> result;
	const std::vector<GuestSeat> &seats = activeSeats();
	result.reserve(seats.size());
	for (const GuestSeat &seat : seats) {
		GuestView view;
		view.seat = seat;
		if (auto it = statuses_.find(seat.id); it != statuses_.end())
			view.status = it->second;
		view.status.id = seat.id;
		result.push_back(std::move(view));
	}
	return result;
}

const std::vector<GuestSeat> &SessionController::activeSeats() const
{
	if (const Collaborator *collaborator = settings_.findCollaborator(activeCollaborator_))
		return collaborator->seats;
	return settings_.seats;
}

std::vector<GuestSeat> *SessionController::seatListFor(const std::string &guestId)
{
	// Seats of a joined show carry their collaborator's prefix, which tells the lists apart even for an event
	// that arrives after we have moved on to another show.
	for (Collaborator &collaborator : settings_.collaborators) {
		if (guestId.rfind(collaborator.seatPrefix(), 0) == 0)
			return &collaborator.seats;
	}
	// Our own seats are plain stream IDs and never contain the separator.
	return guestId.find(':') == std::string::npos ? &settings_.seats : nullptr;
}

GuestSeat *SessionController::findSeat(const std::string &guestId)
{
	std::vector<GuestSeat> *seats = seatListFor(guestId);
	if (!seats)
		return nullptr;
	auto it = std::find_if(seats->begin(), seats->end(), [&](const GuestSeat &seat) { return seat.id == guestId; });
	return it != seats->end() ? &*it : nullptr;
}

const GuestSeat *SessionController::findSeat(const std::string &guestId) const
{
	return const_cast<SessionController *>(this)->findSeat(guestId);
}

bool SessionController::isCollaboratorSeat(const std::string &guestId) const
{
	return guestId.find(':') != std::string::npos;
}

GuestSeat SessionController::addGuest(const QString &name)
{
	GuestSeat seat;
	do {
		seat.id = vdo::generateStreamId(10);
	} while (settings_.findSeat(seat.id));
	seat.name = name.trimmed().isEmpty() ? std::string(kPlaceholderName) : name.trimmed().toStdString();

	settings_.seats.push_back(seat);
	persist();
	ensurePlayer(seat.id);
	service_->addSeat(seat);
	emit guestListChanged();
	return seat;
}

void SessionController::renameGuest(const std::string &guestId, const QString &name)
{
	GuestSeat *seat = findSeat(guestId);
	const std::string newName = name.trimmed().toStdString();
	if (!seat || newName.empty() || seat->name == newName)
		return;

	// Keep auto-named OBS sources in step with the guest's name.
	const std::string oldSourceName = guestSourceName(seat->name);
	if (OBSSourceAutoRelease source = findGuestSource(guestId)) {
		const char *current = obs_source_get_name(source);
		if (current && oldSourceName == current) {
			const std::string wanted = guestSourceName(newName);
			OBSSourceAutoRelease clash = obs_get_source_by_name(wanted.c_str());
			if (!clash)
				obs_source_set_name(source, wanted.c_str());
		}
	}

	seat->name = newName;
	persist();
	service_->renameSeat(guestId, newName);
	emit guestListChanged();
}

void SessionController::removeGuest(const std::string &guestId)
{
	service_->removeSeat(guestId);
	if (std::vector<GuestSeat> *seats = seatListFor(guestId)) {
		seats->erase(std::remove_if(seats->begin(), seats->end(),
					    [&](const GuestSeat &seat) { return seat.id == guestId; }),
			     seats->end());
	}
	statuses_.erase(guestId);
	persist();
	// The player stays alive while OBS sources are still bound to it; they simply show nothing.
	emit guestListChanged();
}

QString SessionController::inviteLink(const std::string &guestId) const
{
	// Only seats of our own show have a link; the people in someone else's show were invited by its host.
	const GuestSeat *seat = settings_.findSeat(guestId);
	if (!seat)
		return {};
	return QString::fromStdString(service_->inviteLink(settings_.toShowConfig(), *seat));
}

QString SessionController::openInviteLink() const
{
	return QString::fromStdString(service_->openInviteLink(settings_.toShowConfig()));
}

bool SessionController::isGuestInCurrentScene(const std::string &guestId) const
{
	OBSSourceAutoRelease source = findGuestSource(guestId);
	return source && isInCurrentScene(source);
}

bool SessionController::addGuestToCurrentScene(const std::string &guestId)
{
	const GuestSeat *seat = findSeat(guestId);
	if (!seat)
		return false;
	OBSSourceAutoRelease source = getOrCreateGuestSource(guestId, guestSourceName(seat->name));
	if (!source)
		return false;
	if (isInCurrentScene(source))
		return true;
	return addToCurrentScene(source, settings_.fitToCanvas);
}

void SessionController::removeGuestFromCurrentScene(const std::string &guestId)
{
	if (OBSSourceAutoRelease source = findGuestSource(guestId))
		removeFromCurrentScene(source);
}

bool SessionController::isGuestMuted(const std::string &guestId) const
{
	auto player = findPlayer(guestId);
	if (!player)
		return false;
	bool any = false;
	bool allMuted = true;
	player->forEachSource([&](obs_source_t *source) {
		any = true;
		allMuted = allMuted && obs_source_muted(source);
	});
	return any && allMuted;
}

void SessionController::setGuestMuted(const std::string &guestId, bool muted)
{
	if (auto player = findPlayer(guestId))
		player->forEachSource([muted](obs_source_t *source) { obs_source_set_muted(source, muted); });
}

void SessionController::refreshGuestVideo(const std::string &guestId)
{
	service_->requestKeyframe(guestId);
}

void SessionController::disconnectGuest(const std::string &guestId)
{
	service_->disconnectGuest(guestId);
}

bool SessionController::guestMedia(const std::string &guestId, GuestMediaStats &stats, uint32_t &width,
				   uint32_t &height) const
{
	width = 0;
	height = 0;
	if (auto player = findPlayer(guestId)) {
		width = player->width();
		height = player->height();
	}
	return service_->guestStats(guestId, stats);
}

// ---------------------------------------------------------------------------------------------------------------------
// Players and sources
// ---------------------------------------------------------------------------------------------------------------------

std::shared_ptr<GuestPlayer> SessionController::findPlayer(const std::string &guestId) const
{
	std::lock_guard<std::mutex> lock(sharedMutex_);
	auto it = players_.find(guestId);
	return it != players_.end() ? it->second : nullptr;
}

std::shared_ptr<GuestPlayer> SessionController::ensurePlayer(const std::string &guestId)
{
	std::shared_ptr<GuestPlayer> player;
	{
		std::lock_guard<std::mutex> lock(sharedMutex_);
		auto it = players_.find(guestId);
		if (it != players_.end())
			return it->second;

		GuestService *service = service_.get();
		player = std::make_shared<GuestPlayer>([service, guestId] { service->requestKeyframe(guestId); });
		player->setHardwareDecoding(hardwareDecoding_.load());
		players_.emplace(guestId, player);
	}
	service_->setMediaSink(guestId, player);
	return player;
}

void SessionController::excludeFromReturnAudio(obs_source_t *source) const
{
	int track;
	{
		std::lock_guard<std::mutex> lock(sharedMutex_);
		track = returnAudioTrack_;
	}
	// Guests already hear each other directly; routing them into the return feed would give them an echo of
	// themselves.
	const uint32_t mixers = obs_source_get_audio_mixers(source);
	const uint32_t bit = 1u << (track - 1);
	if (mixers & bit)
		obs_source_set_audio_mixers(source, mixers & ~bit);
}

void SessionController::attachSource(const std::string &guestId, obs_source_t *source)
{
	if (guestId.empty() || !source || shutDown_)
		return;
	excludeFromReturnAudio(source);
	ensurePlayer(guestId)->addSource(source);
}

void SessionController::setSourceOfflineAppearance(const std::string &guestId, obs_source_t *source, OfflineMode mode,
						   const std::string &imagePath)
{
	if (guestId.empty() || !source || shutDown_)
		return;
	if (auto player = findPlayer(guestId))
		player->setOfflineAppearance(source, mode, imagePath);
}

void SessionController::setGuestBitrateOverride(const std::string &guestId, uint32_t kbps)
{
	if (guestId.empty() || shutDown_)
		return;
	GuestQuality quality;
	{
		std::lock_guard<std::mutex> lock(sharedMutex_);
		const auto existing = bitrateOverrides_.find(guestId);
		const uint32_t previous = existing != bitrateOverrides_.end() ? existing->second : 0;
		if (previous == kbps)
			return;
		if (kbps == 0)
			bitrateOverrides_.erase(guestId);
		else
			bitrateOverrides_[guestId] = kbps;
		quality = defaultQuality_;
	}
	if (kbps != 0)
		quality.maxVideoBitrateKbps = kbps;
	service_->setGuestQuality(guestId, quality);
}

void SessionController::applyBitrateOverrides()
{
	std::map<std::string, uint32_t> overrides;
	GuestQuality quality;
	{
		std::lock_guard<std::mutex> lock(sharedMutex_);
		overrides = bitrateOverrides_;
		quality = defaultQuality_;
	}
	for (const auto &[guestId, kbps] : overrides) {
		GuestQuality guestQuality = quality;
		guestQuality.maxVideoBitrateKbps = kbps;
		service_->setGuestQuality(guestId, guestQuality);
	}
}

void SessionController::detachSource(const std::string &guestId, obs_source_t *source)
{
	if (guestId.empty() || !source)
		return;
	if (auto player = findPlayer(guestId))
		player->removeSource(source);
}

// ---------------------------------------------------------------------------------------------------------------------
// Service events (service thread -> UI thread)
// ---------------------------------------------------------------------------------------------------------------------

void SessionController::onServiceState(ServiceState state, const std::string &detail)
{
	QMetaObject::invokeMethod(
		this,
		[this, state, detail = QString::fromStdString(detail)] {
			if (shutDown_)
				return;
			state_ = state;
			stateDetail_ = detail;
			if (state == ServiceState::Failed && joined()) {
				// The show ended on its own (the host removed us, or our seat was taken). Unlike after
				// stopShow nobody has tidied up yet.
				returnFeed_->stop();
				secondFeed_->stop();
				returnDemand_ = false;
				for (auto &[id, status] : statuses_) {
					status.connection = GuestConnection::Offline;
					status.receivingReturnFeed = false;
				}
				activeCollaborator_.clear();
				configureFeeds();
				emit guestListChanged();
			}
			emit stateChanged();
		},
		Qt::QueuedConnection);
}

void SessionController::onGuestStatus(const GuestStatus &status)
{
	QMetaObject::invokeMethod(
		this,
		[this, status] {
			if (shutDown_)
				return;
			GuestSeat *seat = findSeat(status.id);
			if (!seat)
				return;
			statuses_[status.id] = status;

			// A show we joined without naming it takes the name its host goes by.
			if (!status.reportedName.empty() && isCollaboratorSeat(status.id)) {
				for (Collaborator &collaborator : settings_.collaborators) {
					if (collaborator.name.empty() &&
					    status.id == collaborator.seatPrefix() + kHostSeatSuffix) {
						collaborator.name = status.reportedName;
						persist();
						emit collaboratorsChanged();
						emit stateChanged();
					}
				}
			}

			// Someone who joined through the open link tells us their name once connected, as does everyone
			// in a show we joined ourselves.
			if (!status.reportedName.empty() && seat->name == kPlaceholderName) {
				renameGuest(status.id, QString::fromStdString(status.reportedName));
				return; // renameGuest refreshes the list
			}
			emit guestStatusChanged(QString::fromStdString(status.id));
		},
		Qt::QueuedConnection);
}

void SessionController::onSeatCreated(const GuestSeat &seat)
{
	QMetaObject::invokeMethod(
		this,
		[this, seat] {
			std::vector<GuestSeat> *seats = seatListFor(seat.id);
			if (shutDown_ || !seats || findSeat(seat.id))
				return;
			seats->push_back(seat);
			persist();
			ensurePlayer(seat.id);
			emit guestListChanged();
		},
		Qt::QueuedConnection);
}

void SessionController::onReturnFeedDemand(bool wanted)
{
	QMetaObject::invokeMethod(
		this,
		[this, wanted] {
			if (shutDown_)
				return;
			returnDemand_ = wanted;
			if (wanted) {
				returnFeed_->start();
				secondFeed_->start();
			} else {
				returnFeed_->stop();
				secondFeed_->stop();
			}
		},
		Qt::QueuedConnection);
}

} // namespace qc
