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
#include "invite-dialog.hpp"
#include "settings-dialog.hpp"
#include "ui-helpers.hpp"
#include "ui-style.hpp"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace qc {

namespace {

QString formatElapsed(qint64 milliseconds)
{
	const qint64 seconds = milliseconds / 1000;
	const qint64 hours = seconds / 3600;
	const qint64 minutes = (seconds / 60) % 60;
	if (hours > 0)
		return QStringLiteral("%1:%2:%3")
			.arg(hours)
			.arg(minutes, 2, 10, QLatin1Char('0'))
			.arg(seconds % 60, 2, 10, QLatin1Char('0'));
	return QStringLiteral("%1:%2").arg(minutes, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

QuickCastDock::QuickCastDock(SessionController *controller, QWidget *parent) : QWidget(parent), controller_(controller)
{
	setMinimumWidth(280);

	// --- Status card: the state of the show and the button that changes it ---
	statusCard_ = new QWidget(this);
	statusCard_->setObjectName(QStringLiteral("quickCastStatusCard"));
	statePill_ = new QLabel(statusCard_);
	timerLabel_ = new QLabel(statusCard_);
	headline_ = new QLabel(statusCard_);
	headline_->setWordWrap(true);
	subline_ = new QLabel(statusCard_);
	subline_->setWordWrap(true);
	showButton_ = new QPushButton(statusCard_);

	auto *pillLine = new QHBoxLayout();
	pillLine->setSpacing(8);
	pillLine->addWidget(statePill_, 0, Qt::AlignVCenter);
	pillLine->addWidget(timerLabel_, 0, Qt::AlignVCenter);
	pillLine->addStretch(1);

	auto *statusLayout = new QVBoxLayout(statusCard_);
	statusLayout->setContentsMargins(12, 12, 12, 12);
	statusLayout->setSpacing(4);
	statusLayout->addLayout(pillLine);
	statusLayout->addSpacing(2);
	statusLayout->addWidget(headline_);
	statusLayout->addWidget(subline_);
	statusLayout->addSpacing(8);
	statusLayout->addWidget(showButton_);

	noticeLabel_ = new QLabel(this);
	noticeLabel_->setWordWrap(true);
	noticeLabel_->setVisible(false);

	// --- Guest list ---
	listHeading_ = new QLabel(tr_("Dock.Guests"), this);

	guestContainer_ = new QWidget(this);
	guestLayout_ = new QVBoxLayout(guestContainer_);
	guestLayout_->setContentsMargins(0, 0, 0, 0);
	guestLayout_->setSpacing(8);

	emptyState_ = new QWidget(guestContainer_);
	emptyState_->setObjectName(QStringLiteral("quickCastEmptyState"));
	emptyTitle_ = new QLabel(tr_("Dock.Empty.Title"), emptyState_);
	emptyTitle_->setAlignment(Qt::AlignCenter);
	emptyText_ = new QLabel(tr_("Dock.Empty.Steps"), emptyState_);
	emptyText_->setWordWrap(true);
	emptyText_->setTextFormat(Qt::PlainText);
	auto *emptyLayout = new QVBoxLayout(emptyState_);
	emptyLayout->setContentsMargins(16, 20, 16, 20);
	emptyLayout->setSpacing(10);
	emptyLayout->addWidget(emptyTitle_);
	emptyLayout->addWidget(emptyText_);
	guestLayout_->addWidget(emptyState_);
	guestLayout_->addStretch(1);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setWidget(guestContainer_);
	scroll->viewport()->setAutoFillBackground(false);
	guestContainer_->setAutoFillBackground(false);

	// --- Footer: bring someone in, plus the two occasional tools ---
	inviteButton_ = new QPushButton(tr_("Dock.InviteGuest"), this);
	inviteButton_->setToolTip(tr_("Dock.InviteGuest.Tooltip"));
	openLinkButton_ = new QToolButton(this);
	settingsButton_ = new QToolButton(this);

	auto *footer = new QHBoxLayout();
	footer->setSpacing(4);
	footer->addWidget(inviteButton_, 1);
	footer->addWidget(openLinkButton_, 0);
	footer->addWidget(settingsButton_, 0);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(10);
	layout->addWidget(statusCard_);
	layout->addWidget(noticeLabel_);
	layout->addWidget(listHeading_);
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
	connect(inviteButton_, &QPushButton::clicked, this, &QuickCastDock::inviteGuest);
	connect(openLinkButton_, &QToolButton::clicked, this, &QuickCastDock::copyOpenLink);
	connect(settingsButton_, &QToolButton::clicked, this, &QuickCastDock::openSettings);
	connect(refreshTimer_, &QTimer::timeout, this, [this] {
		refreshGuests();
		refreshState();
	});
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
	restyle();
	rebuildGuests();
}

void QuickCastDock::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	// Follow OBS theme switches. Restyling sets stylesheets, which itself raises StyleChange, hence the guard.
	if (!restyling_ && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)) {
		restyle();
		refreshState();
	}
}

void QuickCastDock::restyle()
{
	restyling_ = true;
	const ui::Colors c = ui::colors(this);

	ui::styleCard(statusCard_);
	ui::styleHeading(headline_, 1.1);
	ui::styleMuted(subline_);
	ui::styleMuted(timerLabel_, 1.0);
	timerLabel_->setFont([this] {
		QFont font = timerLabel_->font();
		font.setStyleHint(QFont::Monospace);
		return font;
	}());

	ui::styleMuted(listHeading_, 0.88);
	listHeading_->setStyleSheet(listHeading_->styleSheet() + QStringLiteral(" font-weight: 700;"));
	QFont headingFont = listHeading_->font();
	headingFont.setBold(true);
	headingFont.setCapitalization(QFont::AllUppercase);
	headingFont.setLetterSpacing(QFont::PercentageSpacing, 108);
	listHeading_->setFont(headingFont);

	emptyState_->setAttribute(Qt::WA_StyledBackground, true);
	emptyState_->setStyleSheet(QStringLiteral("#quickCastEmptyState { border: 1px dashed %1; border-radius: 8px; }")
					   .arg(ui::css(c.border)));
	ui::styleHeading(emptyTitle_, 1.05);
	ui::styleMuted(emptyText_, 1.0);

	noticeLabel_->setStyleSheet(
		QStringLiteral(
			"padding: 8px 10px; border-radius: 6px; background-color: %1; color: %2; border: 1px solid %3;")
			.arg(ui::css(ui::tint(c.good, 0.16)), ui::css(c.text), ui::css(ui::tint(c.good, 0.45))));

	ui::styleIconButton(openLinkButton_, ui::Icon::Link, tr_("Dock.CopyOpenLink.Tooltip"));
	ui::styleIconButton(settingsButton_, ui::Icon::Gear, tr_("Dock.Settings"));

	for (auto &[id, row] : rows_)
		row->restyle();
	restyling_ = false;
}

void QuickCastDock::refreshState()
{
	restyling_ = true;
	const ui::Colors c = ui::colors(this);
	const ServiceState state = controller_->state();
	const auto guests = controller_->guests();
	const auto connected = std::count_if(guests.begin(), guests.end(), [](const GuestView &guest) {
		return guest.status.connection == GuestConnection::Connected;
	});
	const bool running = controller_->showRunning();

	QString pill;
	QColor pillColor = c.text;
	QString headline;
	QString subline;
	switch (state) {
	case ServiceState::Stopped:
		pill = tr_("State.Pill.Off");
		headline = tr_("State.Off");
		subline = guests.empty() ? tr_("State.Off.HintNoGuests") : tr_("State.Off.Hint");
		break;
	case ServiceState::Connecting:
		pill = tr_("State.Pill.Starting");
		pillColor = c.warning;
		headline = tr_("State.Connecting");
		break;
	case ServiceState::Live:
		pill = tr_("State.Pill.Live");
		pillColor = c.danger;
		if (guests.empty()) {
			headline = tr_("State.Live");
			subline = tr_("State.Live.HintNoGuests");
		} else {
			headline = tr_("State.LiveGuests").arg(connected).arg(guests.size());
			subline = connected == 0 ? tr_("State.Live.HintWaiting") : QString();
		}
		break;
	case ServiceState::Reconnecting:
		pill = tr_("State.Pill.Reconnecting");
		pillColor = c.warning;
		headline = tr_("State.Reconnecting");
		subline = tr_("State.Reconnecting.Hint");
		break;
	case ServiceState::Failed:
		pill = tr_("State.Pill.Error");
		pillColor = c.danger;
		headline = tr_("State.Failed");
		break;
	}
	const QString detail = controller_->stateDetail();
	if (!detail.isEmpty() && (state == ServiceState::Failed || state == ServiceState::Live))
		subline = detail;

	statePill_->setText(pill);
	if (state == ServiceState::Live)
		ui::stylePill(statePill_, c.danger, QColor(Qt::white));
	else
		ui::stylePill(statePill_, ui::tint(pillColor, 0.22),
			      state == ServiceState::Stopped ? c.text : pillColor);

	// How long the show has been open: reassuring during a broadcast, and a quick sanity check after one.
	if (state == ServiceState::Live) {
		if (!liveClock_.isValid())
			liveClock_.start();
		timerLabel_->setText(formatElapsed(liveClock_.elapsed()));
	} else if (state == ServiceState::Stopped || state == ServiceState::Failed) {
		liveClock_.invalidate();
	}
	timerLabel_->setVisible(state == ServiceState::Live);

	headline_->setText(headline);
	subline_->setText(subline);
	subline_->setVisible(!subline.isEmpty());

	// One emphasised button at a time, always the next sensible step: invite when there is nobody to host, start
	// the show once there is. While live the emphasis moves into the list, to the guest waiting to go on screen.
	const bool inviteIsNext = guests.empty();
	showButton_->setText(running ? tr_("Dock.EndShow") : tr_("Dock.StartShow"));
	showButton_->setToolTip(running ? tr_("Dock.EndShow.Tooltip") : tr_("Dock.StartShow.Tooltip"));
	const bool startIsNext = !running && !inviteIsNext;
	ui::ButtonKind showKind = ui::ButtonKind::Secondary;
	if (running)
		showKind = ui::ButtonKind::Danger;
	else if (startIsNext)
		showKind = ui::ButtonKind::Primary;
	ui::styleButton(showButton_, showKind);
	showButton_->setIcon(running ? ui::icon(ui::Icon::Stop, c.danger)
				     : ui::icon(ui::Icon::Play, startIsNext ? c.accentText : c.text));
	ui::styleButton(inviteButton_, inviteIsNext ? ui::ButtonKind::Primary : ui::ButtonKind::Secondary);
	inviteButton_->setIcon(ui::icon(ui::Icon::Plus, inviteIsNext ? c.accentText : c.text));

	listHeading_->setText(guests.empty() ? tr_("Dock.Guests") : tr_("Dock.GuestsCount").arg(guests.size()));
	restyling_ = false;
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
	restyling_ = true;
	for (const GuestView &guest : guests) {
		auto *row = new GuestRow(controller_, guest, guestContainer_);
		connect(row, &GuestRow::notice, this, &QuickCastDock::showNotice);
		guestLayout_->insertWidget(index++, row);
		rows_[guest.seat.id] = row;
	}
	restyling_ = false;
	emptyState_->setVisible(guests.empty());
	refreshState();
}

void QuickCastDock::refreshGuests()
{
	if (!isVisible())
		return;
	restyling_ = true;
	for (const GuestView &guest : controller_->guests()) {
		auto it = rows_.find(guest.seat.id);
		if (it != rows_.end())
			it->second->refresh(guest);
	}
	restyling_ = false;
}

void QuickCastDock::showNotice(const QString &text)
{
	noticeLabel_->setText(text);
	noticeLabel_->setVisible(true);
	noticeTimer_->start();
}

void QuickCastDock::inviteGuest()
{
	InviteDialog dialog(controller_, window());
	dialog.exec();
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
