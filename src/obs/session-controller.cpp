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

#include <QMetaObject>

#include <algorithm>

namespace qc {

namespace {
SessionController *g_instance = nullptr;
constexpr const char *kPlaceholderName = "Guest";
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
	for (const GuestSeat &seat : settings_.seats)
		ensurePlayer(seat.id);

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
	seatsSnapshot_ = settings_.seats;
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
	state_ = ServiceState::Connecting;
	stateDetail_.clear();
	emit stateChanged();
	service_->start(settings_.toShowConfig());
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
	emit stateChanged();
	emit guestListChanged();
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
				   settings.secondFeedEnabled != settings_.secondFeedEnabled ||
				   settings.secondVideoSource != settings_.secondVideoSource ||
				   settings.secondHeight != settings_.secondHeight ||
				   settings.secondVideoBitrateKbps != settings_.secondVideoBitrateKbps ||
				   settings.returnVideoBitrateKbps != settings_.returnVideoBitrateKbps ||
				   settings.returnAudioBitrateKbps != settings_.returnAudioBitrateKbps ||
				   settings.returnFeedEnabled != settings_.returnFeedEnabled;
	const int previousTrack = settings_.returnAudioTrack;

	// Seats are managed through the dock, not the settings dialog.
	const std::vector<GuestSeat> seats = settings_.seats;
	settings_ = settings;
	settings_.seats = seats;
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

	if (returnChanged) {
		returnFeed_->configure(settings_);
		secondFeed_->configure(settings_);
	}

	if (connectionChanged && showRunning()) {
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
	result.reserve(settings_.seats.size());
	for (const GuestSeat &seat : settings_.seats) {
		GuestView view;
		view.seat = seat;
		if (auto it = statuses_.find(seat.id); it != statuses_.end())
			view.status = it->second;
		view.status.id = seat.id;
		result.push_back(std::move(view));
	}
	return result;
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
	GuestSeat *seat = settings_.findSeat(guestId);
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
	settings_.seats.erase(std::remove_if(settings_.seats.begin(), settings_.seats.end(),
					     [&](const GuestSeat &seat) { return seat.id == guestId; }),
			      settings_.seats.end());
	statuses_.erase(guestId);
	persist();
	// The player stays alive while OBS sources are still bound to it; they simply show nothing.
	emit guestListChanged();
}

QString SessionController::inviteLink(const std::string &guestId) const
{
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
	const GuestSeat *seat = settings_.findSeat(guestId);
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
			GuestSeat *seat = settings_.findSeat(status.id);
			if (!seat)
				return;
			statuses_[status.id] = status;

			// Someone who joined through the open link tells us their name once connected.
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
			if (shutDown_ || settings_.findSeat(seat.id))
				return;
			settings_.seats.push_back(seat);
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
