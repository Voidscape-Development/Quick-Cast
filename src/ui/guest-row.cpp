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

#include "guest-row.hpp"

#include "ui-helpers.hpp"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace qc {

namespace {

QString formatBitrate(double kbps)
{
	if (kbps >= 1000.0)
		return QStringLiteral("%1 Mbps").arg(kbps / 1000.0, 0, 'f', 1);
	return QStringLiteral("%1 kbps").arg(static_cast<int>(kbps + 0.5));
}

} // namespace

GuestRow::GuestRow(SessionController *controller, const GuestView &guest, QWidget *parent)
	: QFrame(parent),
	  controller_(controller),
	  guestId_(guest.seat.id)
{
	setObjectName(QStringLiteral("quickCastGuestCard"));

	avatar_ = new ui::Avatar(this);

	nameLabel_ = new ui::ElidedLabel(this);

	signal_ = new ui::SignalBars(this);
	detailLabel_ = new ui::ElidedLabel(this);
	flagsLabel_ = new QLabel(this);
	flagsLabel_->setWordWrap(true);

	linkButton_ = new QToolButton(this);
	muteButton_ = new QToolButton(this);
	menuButton_ = new QToolButton(this);
	menuButton_->setPopupMode(QToolButton::InstantPopup);

	auto *menu = new QMenu(menuButton_);
	QAction *renameAction = menu->addAction(tr_("Guest.Rename"));
	refreshAction_ = menu->addAction(tr_("Guest.RefreshVideo"));
	menu->addSeparator();
	disconnectAction_ = menu->addAction(tr_("Guest.Disconnect"));
	QAction *removeAction = menu->addAction(tr_("Guest.Remove"));
	menuButton_->setMenu(menu);

	sceneButton_ = new QPushButton(this);
	sceneButton_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

	// Name and quick actions share the first line; the status line runs the full width underneath so it has room
	// even in a narrow dock.
	auto *nameLine = new QHBoxLayout();
	nameLine->setSpacing(6);
	nameLine->addWidget(nameLabel_, 1);
	nameLine->addWidget(muteButton_);
	nameLine->addWidget(linkButton_);
	nameLine->addWidget(menuButton_);

	auto *statusLine = new QHBoxLayout();
	statusLine->setSpacing(6);
	statusLine->addWidget(signal_, 0, Qt::AlignVCenter);
	statusLine->addWidget(detailLabel_, 1);

	auto *identity = new QVBoxLayout();
	identity->setSpacing(0);
	identity->addLayout(nameLine);
	identity->addLayout(statusLine);

	auto *top = new QHBoxLayout();
	top->setSpacing(10);
	top->addWidget(avatar_, 0, Qt::AlignVCenter);
	top->addLayout(identity, 1);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(12, 10, 10, 12);
	layout->setSpacing(10);
	layout->addLayout(top);
	layout->addWidget(flagsLabel_);
	layout->addWidget(sceneButton_);

	connect(linkButton_, &QToolButton::clicked, this, &GuestRow::copyLink);
	connect(muteButton_, &QToolButton::clicked, this, &GuestRow::toggleMute);
	connect(sceneButton_, &QPushButton::clicked, this, &GuestRow::toggleScene);
	connect(renameAction, &QAction::triggered, this, &GuestRow::rename);
	connect(removeAction, &QAction::triggered, this, &GuestRow::remove);
	connect(refreshAction_, &QAction::triggered, this, [this] { controller_->refreshGuestVideo(guestId_); });
	connect(disconnectAction_, &QAction::triggered, this, [this] { controller_->disconnectGuest(guestId_); });

	restyle();
	refresh(guest);
}

void GuestRow::restyle()
{
	const ui::Colors c = ui::colors(this);
	ui::styleCard(this);
	ui::styleHeading(nameLabel_, 1.05);
	ui::styleMuted(detailLabel_);
	ui::styleMuted(flagsLabel_);
	flagsLabel_->setStyleSheet(flagsLabel_->styleSheet() + QStringLiteral(" color: %1;").arg(ui::css(c.warning)));
	ui::styleIconButton(linkButton_, ui::Icon::Link, tr_("Guest.CopyLink.Tooltip"));
	ui::styleIconButton(menuButton_, ui::Icon::More, tr_("Guest.More"));
	sceneStateKnown_ = false;
	applySceneState();
}

void GuestRow::applySceneState()
{
	const bool inScene = controller_->isGuestInCurrentScene(guestId_);
	const bool muted = inScene && controller_->isGuestMuted(guestId_);
	if (sceneStateKnown_ && inScene == inScene_ && muted == muted_ && connected_ == styledConnected_)
		return;
	sceneStateKnown_ = true;
	styledConnected_ = connected_;
	inScene_ = inScene;
	muted_ = muted;

	// "Add" becomes the call to action the moment a guest is connected and not yet on screen; otherwise the
	// button stays quiet so the dock never shows a wall of emphasised buttons.
	// A guest who is on screen gets a green outline: visible at a glance down the list, and backed up in words
	// by the button, which now reads "Remove from scene".
	const ui::Colors c = ui::colors(this);
	ui::styleCard(this);
	if (inScene_) {
		setStyleSheet(
			QStringLiteral("#%1 { background-color: %2; border: 1px solid %3; border-radius: 8px; }")
				.arg(objectName(), ui::css(ui::tint(c.good, 0.07)), ui::css(ui::tint(c.good, 0.65))));
	}
	sceneButton_->setText(inScene_ ? tr_("Guest.RemoveFromScene") : tr_("Guest.AddToScene"));
	sceneButton_->setToolTip(inScene_ ? tr_("Guest.RemoveFromScene.Tooltip") : tr_("Guest.AddToScene.Tooltip"));
	const bool callToAction = !inScene_ && connected_;
	ui::styleButton(sceneButton_, callToAction ? ui::ButtonKind::Primary : ui::ButtonKind::Secondary);
	sceneButton_->setIcon(inScene_ ? QIcon() : ui::icon(ui::Icon::Plus, callToAction ? c.accentText : c.text));

	// Muting only means something once the guest has a source in OBS.
	muteButton_->setVisible(inScene_);
	ui::styleIconButton(muteButton_, muted_ ? ui::Icon::SpeakerMuted : ui::Icon::Speaker,
			    muted_ ? tr_("Guest.Unmute") : tr_("Guest.Mute"));
	if (muted_)
		muteButton_->setIcon(ui::icon(ui::Icon::SpeakerMuted, c.danger));
}

void GuestRow::refresh(const GuestView &guest)
{
	name_ = QString::fromStdString(guest.seat.name);
	nameLabel_->setText(name_);
	avatar_->setName(name_);

	const bool connected = guest.status.connection == GuestConnection::Connected;
	const bool connecting = guest.status.connection == GuestConnection::Connecting;
	connected_ = connected;
	avatar_->setPresence(connected    ? ui::Avatar::Presence::Connected
			     : connecting ? ui::Avatar::Presence::Connecting
					  : ui::Avatar::Presence::Offline);

	// The status line always says the state in words; the bars and the presence dot only reinforce it.
	QStringList parts;
	int signalLevel = 0;
	if (connected) {
		GuestMediaStats stats;
		uint32_t width = 0;
		uint32_t height = 0;
		const bool haveStats = controller_->guestMedia(guestId_, stats, width, height);
		const double loss = haveStats ? stats.videoLoss : 0.0;
		signalLevel = loss >= 0.05 ? 1 : loss >= 0.01 ? 2 : 3;
		parts << (signalLevel == 3 ? tr_("Guest.State.Connected")
					   : tr_("Guest.State.Unstable").arg(static_cast<int>(loss * 100.0 + 0.5)));
		if (height > 0)
			parts << QStringLiteral("%1p").arg(height);
		if (haveStats && stats.videoBitrateKbps > 1.0)
			parts << formatBitrate(stats.videoBitrateKbps);
	} else if (connecting) {
		parts << tr_("Guest.State.Connecting");
	} else if (controller_->showRunning()) {
		parts << tr_("Guest.State.Waiting");
	} else {
		parts << tr_("Guest.State.ShowOff");
	}
	signal_->setVisible(connected);
	signal_->setLevel(signalLevel);
	const QString detail = parts.join(QStringLiteral(" · "));
	detailLabel_->setText(detail);

	QStringList flags;
	if (connected && guest.status.microphoneMuted)
		flags << tr_("Guest.MicMuted");
	if (connected && guest.status.cameraMuted)
		flags << tr_("Guest.CameraOff");
	flagsLabel_->setText(flags.join(QStringLiteral(" · ")));
	flagsLabel_->setVisible(!flags.isEmpty());

	applySceneState();

	refreshAction_->setEnabled(connected);
	disconnectAction_->setEnabled(connected || connecting);
}

void GuestRow::copyLink()
{
	const QString link = controller_->inviteLink(guestId_);
	if (link.isEmpty())
		return;
	QApplication::clipboard()->setText(link);
	emit notice(controller_->showRunning() ? tr_("Notice.LinkCopied").arg(name_)
					       : tr_("Notice.LinkCopiedShowOff").arg(name_));
}

void GuestRow::toggleScene()
{
	if (inScene_) {
		controller_->removeGuestFromCurrentScene(guestId_);
	} else if (!controller_->addGuestToCurrentScene(guestId_)) {
		emit notice(tr_("Notice.NoScene"));
		return;
	}
	applySceneState();
}

void GuestRow::toggleMute()
{
	controller_->setGuestMuted(guestId_, !muted_);
	applySceneState();
}

void GuestRow::rename()
{
	bool accepted = false;
	const QString name = QInputDialog::getText(window(), tr_("Rename.Title"), tr_("Rename.Label"),
						   QLineEdit::Normal, name_, &accepted);
	if (accepted && !name.trimmed().isEmpty())
		controller_->renameGuest(guestId_, name);
}

void GuestRow::remove()
{
	const auto answer = QMessageBox::question(window(), tr_("Remove.Title"), tr_("Remove.Text").arg(name_),
						  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
	if (answer == QMessageBox::Yes) {
		controller_->removeGuestFromCurrentScene(guestId_);
		controller_->removeGuest(guestId_);
	}
}

} // namespace qc
