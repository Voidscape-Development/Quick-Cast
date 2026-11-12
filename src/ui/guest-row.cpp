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
	setObjectName(QStringLiteral("quickCastGuestRow"));
	setFrameShape(QFrame::StyledPanel);

	dot_ = new QLabel(this);
	dot_->setFixedSize(10, 10);

	nameLabel_ = new QLabel(this);
	QFont nameFont = nameLabel_->font();
	nameFont.setBold(true);
	nameLabel_->setFont(nameFont);
	nameLabel_->setTextInteractionFlags(Qt::NoTextInteraction);

	detailLabel_ = new QLabel(this);
	detailLabel_->setWordWrap(true);
	QFont detailFont = detailLabel_->font();
	detailFont.setPointSizeF(detailFont.pointSizeF() * 0.9);
	detailLabel_->setFont(detailFont);
	detailLabel_->setEnabled(false); // themed "secondary text" colour

	menuButton_ = new QToolButton(this);
	menuButton_->setText(QStringLiteral("⋯"));
	menuButton_->setToolTip(tr_("Guest.More"));
	menuButton_->setAccessibleName(tr_("Guest.More"));
	menuButton_->setPopupMode(QToolButton::InstantPopup);
	menuButton_->setAutoRaise(true);

	auto *menu = new QMenu(menuButton_);
	QAction *renameAction = menu->addAction(tr_("Guest.Rename"));
	muteAction_ = menu->addAction(tr_("Guest.Mute"));
	muteAction_->setCheckable(true);
	refreshAction_ = menu->addAction(tr_("Guest.RefreshVideo"));
	menu->addSeparator();
	disconnectAction_ = menu->addAction(tr_("Guest.Disconnect"));
	QAction *removeAction = menu->addAction(tr_("Guest.Remove"));
	menuButton_->setMenu(menu);

	linkButton_ = new QPushButton(tr_("Guest.CopyLink"), this);
	linkButton_->setToolTip(tr_("Guest.CopyLink.Tooltip"));
	sceneButton_ = new QPushButton(this);

	// Docks are often narrow: the name gives way first, and the one wide button gets a row to itself.
	nameLabel_->setMinimumWidth(40);
	nameLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	detailLabel_->setMinimumWidth(40);
	sceneButton_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

	auto *header = new QHBoxLayout();
	header->setSpacing(8);
	header->addWidget(dot_, 0, Qt::AlignVCenter);
	header->addWidget(nameLabel_, 1);
	header->addWidget(linkButton_, 0);
	header->addWidget(menuButton_, 0);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 8, 10, 10);
	layout->setSpacing(6);
	layout->addLayout(header);
	layout->addWidget(detailLabel_);
	layout->addWidget(sceneButton_);

	connect(linkButton_, &QPushButton::clicked, this, &GuestRow::copyLink);
	connect(sceneButton_, &QPushButton::clicked, this, &GuestRow::toggleScene);
	connect(renameAction, &QAction::triggered, this, &GuestRow::rename);
	connect(removeAction, &QAction::triggered, this, &GuestRow::remove);
	connect(muteAction_, &QAction::triggered, this,
		[this](bool checked) { controller_->setGuestMuted(guestId_, checked); });
	connect(refreshAction_, &QAction::triggered, this, [this] { controller_->refreshGuestVideo(guestId_); });
	connect(disconnectAction_, &QAction::triggered, this, [this] { controller_->disconnectGuest(guestId_); });
	// Mute state can be changed from OBS' own mixer, so read it fresh each time the menu opens.
	connect(menu, &QMenu::aboutToShow, this, [this] {
		muteAction_->setChecked(controller_->isGuestMuted(guestId_));
		muteAction_->setEnabled(inScene_ || controller_->isGuestMuted(guestId_));
	});

	refresh(guest);
}

void GuestRow::refresh(const GuestView &guest)
{
	name_ = QString::fromStdString(guest.seat.name);
	nameLabel_->setText(name_);
	nameLabel_->setToolTip(name_);

	const bool connected = guest.status.connection == GuestConnection::Connected;
	const bool connecting = guest.status.connection == GuestConnection::Connecting;

	// Colour is never the only signal: the detail line always spells the state out.
	const char *colour = connected ? "#2ecc71" : connecting ? "#f1c40f" : "#7f8c8d";
	dot_->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 5px;").arg(QLatin1String(colour)));

	QStringList parts;
	if (connected) {
		parts << tr_("Guest.State.Connected");
		GuestMediaStats stats;
		uint32_t width = 0;
		uint32_t height = 0;
		if (controller_->guestMedia(guestId_, stats, width, height)) {
			if (width > 0 && height > 0)
				parts << QStringLiteral("%1×%2").arg(width).arg(height);
			if (stats.videoBitrateKbps > 1.0)
				parts << formatBitrate(stats.videoBitrateKbps);
			if (stats.videoLoss >= 0.01)
				parts << tr_("Guest.Loss").arg(static_cast<int>(stats.videoLoss * 100.0 + 0.5));
		}
		if (guest.status.microphoneMuted)
			parts << tr_("Guest.MicMuted");
		if (guest.status.cameraMuted)
			parts << tr_("Guest.CameraOff");
	} else if (connecting) {
		parts << tr_("Guest.State.Connecting");
	} else if (controller_->showRunning()) {
		parts << tr_("Guest.State.Waiting");
	} else {
		parts << tr_("Guest.State.ShowOff");
	}
	detailLabel_->setText(parts.join(QStringLiteral(" · ")));

	inScene_ = controller_->isGuestInCurrentScene(guestId_);
	sceneButton_->setText(inScene_ ? tr_("Guest.RemoveFromScene") : tr_("Guest.AddToScene"));
	sceneButton_->setToolTip(inScene_ ? tr_("Guest.RemoveFromScene.Tooltip") : tr_("Guest.AddToScene.Tooltip"));

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
	inScene_ = controller_->isGuestInCurrentScene(guestId_);
	sceneButton_->setText(inScene_ ? tr_("Guest.RemoveFromScene") : tr_("Guest.AddToScene"));
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
