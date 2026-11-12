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

#include "quick-cast-dock.hpp"

#include "guest-row.hpp"
#include "settings-dialog.hpp"
#include "ui-helpers.hpp"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace qc {

QuickCastDock::QuickCastDock(SessionController *controller, QWidget *parent) : QWidget(parent), controller_(controller)
{
	setMinimumWidth(240);

	// --- Header: show state and the one button that matters most ---
	stateDot_ = new QLabel(this);
	stateDot_->setFixedSize(10, 10);
	stateLabel_ = new QLabel(this);
	stateLabel_->setWordWrap(true);
	showButton_ = new QPushButton(this);
	showButton_->setMinimumHeight(30);

	auto *stateLine = new QHBoxLayout();
	stateLine->setSpacing(8);
	stateLine->addWidget(stateDot_, 0, Qt::AlignVCenter);
	stateLine->addWidget(stateLabel_, 1);

	noticeLabel_ = new QLabel(this);
	noticeLabel_->setWordWrap(true);
	noticeLabel_->setVisible(false);
	noticeLabel_->setStyleSheet(
		QStringLiteral("padding: 6px 8px; border-radius: 4px; background-color: rgba(46, 204, 113, 40);"));

	// --- Guest list ---
	guestContainer_ = new QWidget(this);
	guestLayout_ = new QVBoxLayout(guestContainer_);
	guestLayout_->setContentsMargins(0, 0, 0, 0);
	guestLayout_->setSpacing(6);

	emptyLabel_ = new QLabel(tr_("Dock.Empty"), guestContainer_);
	emptyLabel_->setWordWrap(true);
	emptyLabel_->setAlignment(Qt::AlignCenter);
	emptyLabel_->setEnabled(false);
	emptyLabel_->setContentsMargins(12, 24, 12, 24);
	guestLayout_->addWidget(emptyLabel_);
	guestLayout_->addStretch(1);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setWidget(guestContainer_);

	// --- Footer ---
	auto *inviteButton = new QPushButton(tr_("Dock.InviteGuest"), this);
	inviteButton->setToolTip(tr_("Dock.InviteGuest.Tooltip"));

	auto *moreButton = new QToolButton(this);
	moreButton->setText(QStringLiteral("⋯"));
	moreButton->setToolTip(tr_("Dock.More"));
	moreButton->setAccessibleName(tr_("Dock.More"));
	moreButton->setPopupMode(QToolButton::InstantPopup);
	auto *moreMenu = new QMenu(moreButton);
	QAction *openLinkAction = moreMenu->addAction(tr_("Dock.CopyOpenLink"));
	openLinkAction->setToolTip(tr_("Dock.CopyOpenLink.Tooltip"));
	moreMenu->setToolTipsVisible(true);
	moreMenu->addSeparator();
	QAction *settingsAction = moreMenu->addAction(tr_("Dock.Settings"));
	moreButton->setMenu(moreMenu);

	auto *footer = new QHBoxLayout();
	footer->setSpacing(6);
	footer->addWidget(inviteButton, 1);
	footer->addWidget(moreButton, 0);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);
	layout->addLayout(stateLine);
	layout->addWidget(showButton_);
	layout->addWidget(noticeLabel_);
	layout->addWidget(scroll, 1);
	layout->addLayout(footer);

	refreshTimer_ = new QTimer(this);
	refreshTimer_->setInterval(1000);
	noticeTimer_ = new QTimer(this);
	noticeTimer_->setSingleShot(true);
	noticeTimer_->setInterval(6000);

	connect(showButton_, &QPushButton::clicked, this, [this] {
		if (controller_->showRunning())
			controller_->stopShow();
		else
			controller_->startShow();
	});
	connect(inviteButton, &QPushButton::clicked, this, &QuickCastDock::inviteGuest);
	connect(openLinkAction, &QAction::triggered, this, &QuickCastDock::copyOpenLink);
	connect(settingsAction, &QAction::triggered, this, &QuickCastDock::openSettings);
	connect(refreshTimer_, &QTimer::timeout, this, &QuickCastDock::refreshGuests);
	connect(noticeTimer_, &QTimer::timeout, this, [this] { noticeLabel_->setVisible(false); });

	connect(controller_, &SessionController::stateChanged, this, [this] {
		refreshState();
		refreshGuests();
	});
	connect(controller_, &SessionController::guestListChanged, this, &QuickCastDock::rebuildGuests);
	connect(controller_, &SessionController::guestStatusChanged, this, [this](const QString &) {
		refreshGuests();
		refreshState();
	});

	refreshTimer_->start();
	rebuildGuests();
	refreshState();
}

void QuickCastDock::refreshState()
{
	const ServiceState state = controller_->state();
	const auto guests = controller_->guests();
	const auto connected = std::count_if(guests.begin(), guests.end(), [](const GuestView &guest) {
		return guest.status.connection == GuestConnection::Connected;
	});

	QString text;
	const char *colour = "#7f8c8d";
	switch (state) {
	case ServiceState::Stopped:
		text = tr_("State.Off");
		break;
	case ServiceState::Connecting:
		text = tr_("State.Connecting");
		colour = "#f1c40f";
		break;
	case ServiceState::Live:
		colour = "#e74c3c";
		if (guests.empty())
			text = tr_("State.Live");
		else
			text = tr_("State.LiveGuests").arg(connected).arg(guests.size());
		break;
	case ServiceState::Reconnecting:
		text = tr_("State.Reconnecting");
		colour = "#f1c40f";
		break;
	case ServiceState::Failed:
		text = tr_("State.Failed");
		colour = "#e67e22";
		break;
	}
	const QString detail = controller_->stateDetail();
	if (!detail.isEmpty() && (state == ServiceState::Failed || state == ServiceState::Live))
		text += QStringLiteral("\n") + detail;

	stateLabel_->setText(text);
	stateDot_->setStyleSheet(
		QStringLiteral("background-color: %1; border-radius: 5px;").arg(QLatin1String(colour)));
	showButton_->setText(controller_->showRunning() ? tr_("Dock.EndShow") : tr_("Dock.StartShow"));
	showButton_->setToolTip(controller_->showRunning() ? tr_("Dock.EndShow.Tooltip")
							   : tr_("Dock.StartShow.Tooltip"));
}

void QuickCastDock::rebuildGuests()
{
	for (auto &[id, row] : rows_) {
		guestLayout_->removeWidget(row);
		row->deleteLater();
	}
	rows_.clear();

	const auto guests = controller_->guests();
	int index = 0;
	for (const GuestView &guest : guests) {
		auto *row = new GuestRow(controller_, guest, guestContainer_);
		connect(row, &GuestRow::notice, this, &QuickCastDock::showNotice);
		guestLayout_->insertWidget(index++, row);
		rows_[guest.seat.id] = row;
	}
	emptyLabel_->setVisible(guests.empty());
	refreshState();
}

void QuickCastDock::refreshGuests()
{
	if (!isVisible())
		return;
	for (const GuestView &guest : controller_->guests()) {
		auto it = rows_.find(guest.seat.id);
		if (it != rows_.end())
			it->second->refresh(guest);
	}
}

void QuickCastDock::showNotice(const QString &text)
{
	noticeLabel_->setText(text);
	noticeLabel_->setVisible(true);
	noticeTimer_->start();
}

void QuickCastDock::inviteGuest()
{
	bool accepted = false;
	const QString name = QInputDialog::getText(window(), tr_("Invite.Title"), tr_("Invite.Label"),
						   QLineEdit::Normal, QString(), &accepted);
	if (!accepted || name.trimmed().isEmpty())
		return;

	const GuestSeat seat = controller_->addGuest(name);
	QApplication::clipboard()->setText(controller_->inviteLink(seat.id));
	const QString guestName = QString::fromStdString(seat.name);
	showNotice(controller_->showRunning() ? tr_("Notice.LinkCopied").arg(guestName)
					      : tr_("Notice.LinkCopiedShowOff").arg(guestName));
}

void QuickCastDock::copyOpenLink()
{
	QApplication::clipboard()->setText(controller_->openInviteLink());
	showNotice(tr_("Notice.OpenLinkCopied"));
}

void QuickCastDock::openSettings()
{
	SettingsDialog dialog(controller_, window());
	dialog.exec();
}

} // namespace qc
