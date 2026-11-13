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

#include "collaborator-row.hpp"
#include "guest-row.hpp"
#include "invite-dialog.hpp"
#include "join-dialog.hpp"
#include "settings-dialog.hpp"
#include "ui-helpers.hpp"
#include "ui-style.hpp"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
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
	joinButton_ = new QPushButton(tr_("Dock.JoinShow"), statusCard_);
	joinButton_->setToolTip(tr_("Dock.JoinShow.Tooltip"));

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
	statusLayout->addWidget(joinButton_);

	noticeLabel_ = new QLabel(this);
	noticeLabel_->setWordWrap(true);
	noticeLabel_->setVisible(false);

	// --- Guest list ---
	listHeading_ = new QLabel(tr_("Dock.Guests"), this);

	// Layouts act on the guests in the scene, so the button sits with the list it arranges.
	layoutButton_ = new QToolButton(this);
	layoutButton_->setPopupMode(QToolButton::InstantPopup);
	auto *layoutMenu = new QMenu(layoutButton_);
	QAction *sideBySide = layoutMenu->addAction(tr_("Layout.SideBySide"));
	QAction *grid = layoutMenu->addAction(tr_("Layout.Grid"));
	QAction *pictureInPicture = layoutMenu->addAction(tr_("Layout.PictureInPicture"));
	layoutMenu->addSeparator();
	QAction *includeCamera = layoutMenu->addAction(tr_("Layout.IncludeCamera"));
	includeCamera->setCheckable(true);
	includeCamera->setChecked(controller_->settings().layoutIncludesCamera);
	includeCamera->setToolTip(tr_("Layout.IncludeCamera.Tooltip"));
	layoutMenu->setToolTipsVisible(true);
	layoutButton_->setMenu(layoutMenu);

	auto *listHeader = new QHBoxLayout();
	listHeader->setContentsMargins(0, 0, 0, 0);
	listHeader->addWidget(listHeading_, 1, Qt::AlignVCenter);
	listHeader->addWidget(layoutButton_, 0, Qt::AlignVCenter);

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
	// Shows joined before, below the guests of our own show. Rows are inserted after this heading.
	collaboratorHeading_ = new QLabel(tr_("Dock.Collaborators"), guestContainer_);
	collaboratorHeading_->setContentsMargins(0, 8, 0, 0);
	guestLayout_->addWidget(collaboratorHeading_);
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

	footerLayout_ = new QHBoxLayout();
	footerLayout_->setSpacing(4);
	footerLayout_->addWidget(inviteButton_, 1);
	// Keeps the settings button in its corner while the invite controls are hidden (in someone else's show).
	footerLayout_->addStretch(0);
	footerLayout_->addWidget(openLinkButton_, 0);
	footerLayout_->addWidget(settingsButton_, 0);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(10);
	layout->addWidget(statusCard_);
	layout->addWidget(noticeLabel_);
	layout->addLayout(listHeader);
	layout->addWidget(scroll, 1);
	layout->addLayout(footerLayout_);

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
	connect(joinButton_, &QPushButton::clicked, this, &QuickCastDock::joinShow);
	connect(sideBySide, &QAction::triggered, this, [this] { applyLayout(LayoutKind::SideBySide); });
	connect(grid, &QAction::triggered, this, [this] { applyLayout(LayoutKind::Grid); });
	connect(pictureInPicture, &QAction::triggered, this, [this] { applyLayout(LayoutKind::PictureInPicture); });
	connect(includeCamera, &QAction::toggled, controller_, &SessionController::setLayoutIncludesCamera);
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
	connect(controller_, &SessionController::collaboratorsChanged, this, &QuickCastDock::rebuildCollaborators);
	connect(controller_, &SessionController::guestStatusChanged, this, [this](const QString &) {
		refreshGuests();
		refreshState();
	});

	refreshTimer_->start();
	restyle();
	rebuildCollaborators();
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

	for (QLabel *heading : {listHeading_, collaboratorHeading_}) {
		ui::styleMuted(heading, 0.88);
		heading->setStyleSheet(heading->styleSheet() + QStringLiteral(" font-weight: 700;"));
		QFont headingFont = heading->font();
		headingFont.setBold(true);
		headingFont.setCapitalization(QFont::AllUppercase);
		headingFont.setLetterSpacing(QFont::PercentageSpacing, 108);
		heading->setFont(headingFont);
	}

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
	ui::styleIconButton(layoutButton_, ui::Icon::Layout, tr_("Layout.Tooltip"));
	ui::styleButton(joinButton_, ui::ButtonKind::Secondary);
	joinButton_->setIcon(ui::icon(ui::Icon::Enter, c.text));

	for (auto &[id, row] : rows_)
		row->restyle();
	for (CollaboratorRow *row : collaboratorRows_)
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
	// Whose show this is changes the wording, not the shape: the same card, list and button, seen from the
	// guest's chair.
	const bool joined = controller_->joined();
	const QString showName = controller_->joinedShowName();

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
		pill = joined ? tr_("State.Pill.Joining") : tr_("State.Pill.Starting");
		pillColor = c.warning;
		headline = joined ? tr_("State.Joining").arg(showName) : tr_("State.Connecting");
		break;
	case ServiceState::Live:
		pill = joined ? tr_("State.Pill.Joined") : tr_("State.Pill.Live");
		pillColor = c.danger;
		if (joined) {
			headline = tr_("State.Joined").arg(showName);
			subline = connected == 0 ? tr_("State.Joined.HintWaiting") : QString();
		} else if (guests.empty()) {
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
		subline = joined ? tr_("State.Reconnecting.HintJoined") : tr_("State.Reconnecting.Hint");
		break;
	case ServiceState::Failed:
		pill = tr_("State.Pill.Error");
		pillColor = c.danger;
		headline = controller_->lastShowWasJoined() ? tr_("State.Join.Failed") : tr_("State.Failed");
		break;
	}
	const QString detail = controller_->stateDetail();
	if (!detail.isEmpty() && (state == ServiceState::Failed || state == ServiceState::Live))
		subline = detail;

	statePill_->setText(pill);
	// Red is for our own show being open to guests. Sitting in someone else's is a calmer state.
	if (state == ServiceState::Live && joined)
		ui::stylePill(statePill_, c.good, QColor(Qt::white));
	else if (state == ServiceState::Live)
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
	const bool inviteIsNext = guests.empty() && !joined;
	if (joined) {
		showButton_->setText(tr_("Dock.LeaveShow"));
		showButton_->setToolTip(tr_("Dock.LeaveShow.Tooltip"));
	} else {
		showButton_->setText(running ? tr_("Dock.EndShow") : tr_("Dock.StartShow"));
		showButton_->setToolTip(running ? tr_("Dock.EndShow.Tooltip") : tr_("Dock.StartShow.Tooltip"));
	}
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

	if (joined)
		listHeading_->setText(tr_("Dock.InThisShow"));
	else
		listHeading_->setText(guests.empty() ? tr_("Dock.Guests") : tr_("Dock.GuestsCount").arg(guests.size()));

	// Joining is the alternative to starting, so it is offered exactly when starting is. Inviting only makes sense
	// for a show of our own.
	joinButton_->setVisible(!running);
	inviteButton_->setVisible(!joined);
	openLinkButton_->setVisible(!joined);
	footerLayout_->setStretch(1, joined ? 1 : 0);
	layoutButton_->setVisible(!guests.empty());
	const bool showCollaborators = !running && !collaboratorRows_.empty();
	collaboratorHeading_->setVisible(showCollaborators);
	for (CollaboratorRow *row : collaboratorRows_)
		row->setVisible(showCollaborators);
	// The "how to invite" steps would push remembered shows out of sight in a short dock; the status card above
	// already says how to get started.
	emptyState_->setVisible(guests.empty() && !showCollaborators);
	listHeading_->setVisible(!guests.empty() || !showCollaborators);
	emptyTitle_->setText(joined ? tr_("Dock.Empty.Joined.Title") : tr_("Dock.Empty.Title"));
	emptyText_->setText(joined ? tr_("Dock.Empty.Joined.Text") : tr_("Dock.Empty.Steps"));
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

void QuickCastDock::rebuildCollaborators()
{
	for (CollaboratorRow *row : collaboratorRows_) {
		guestLayout_->removeWidget(row);
		row->deleteLater();
	}
	collaboratorRows_.clear();

	restyling_ = true;
	int index = guestLayout_->indexOf(collaboratorHeading_) + 1;
	for (const Collaborator &collaborator : controller_->collaborators()) {
		auto *row = new CollaboratorRow(controller_, collaborator, guestContainer_);
		guestLayout_->insertWidget(index++, row);
		collaboratorRows_.push_back(row);
	}
	restyling_ = false;
	refreshState();
}

void QuickCastDock::joinShow()
{
	JoinDialog dialog(controller_, window());
	dialog.exec();
}

void QuickCastDock::applyLayout(LayoutKind kind)
{
	if (controller_->applyLayout(kind) == 0)
		showNotice(tr_("Notice.NothingToArrange"));
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
