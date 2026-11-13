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

#include "collaborator-row.hpp"

#include "ui-helpers.hpp"

#include <QDateTime>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace qc {

namespace {

/** "Today", "Yesterday", or the date: enough to tell regular collaborators from one-offs. */
QString describeLastJoined(int64_t secondsSinceEpoch)
{
	if (secondsSinceEpoch <= 0)
		return tr_("Collaborator.NeverJoined");
	const QDate date = QDateTime::fromSecsSinceEpoch(secondsSinceEpoch).date();
	const qint64 days = date.daysTo(QDate::currentDate());
	if (days <= 0)
		return tr_("Collaborator.JoinedToday");
	if (days == 1)
		return tr_("Collaborator.JoinedYesterday");
	return tr_("Collaborator.JoinedOn").arg(QLocale().toString(date, QLocale::ShortFormat));
}

} // namespace

CollaboratorRow::CollaboratorRow(SessionController *controller, const Collaborator &collaborator, QWidget *parent)
	: QFrame(parent),
	  controller_(controller),
	  collaboratorId_(collaborator.id),
	  name_(controller->collaboratorName(collaborator.id))
{
	setObjectName(QStringLiteral("quickCastCollaboratorCard"));

	nameLabel_ = new ui::ElidedLabel(this);
	nameLabel_->setText(name_);
	detailLabel_ = new ui::ElidedLabel(this);
	detailLabel_->setText(describeLastJoined(collaborator.lastJoined));

	joinButton_ = new QPushButton(tr_("Collaborator.Join"), this);
	joinButton_->setToolTip(tr_("Collaborator.Join.Tooltip").arg(name_));

	menuButton_ = new QToolButton(this);
	menuButton_->setPopupMode(QToolButton::InstantPopup);
	auto *menu = new QMenu(menuButton_);
	QAction *renameAction = menu->addAction(tr_("Guest.Rename"));
	QAction *forgetAction = menu->addAction(tr_("Collaborator.Forget"));
	menuButton_->setMenu(menu);

	auto *identity = new QVBoxLayout();
	identity->setSpacing(0);
	identity->addWidget(nameLabel_);
	identity->addWidget(detailLabel_);

	auto *layout = new QHBoxLayout(this);
	// No avatar here, unlike a guest card: in a narrow dock the name needs the room more.
	layout->setContentsMargins(12, 8, 10, 8);
	layout->setSpacing(10);
	layout->addLayout(identity, 1);
	layout->addWidget(joinButton_, 0, Qt::AlignVCenter);
	layout->addWidget(menuButton_, 0, Qt::AlignVCenter);

	connect(joinButton_, &QPushButton::clicked, this, [this] { controller_->joinCollaborator(collaboratorId_); });
	connect(renameAction, &QAction::triggered, this, &CollaboratorRow::rename);
	connect(forgetAction, &QAction::triggered, this, &CollaboratorRow::forget);

	restyle();
}

void CollaboratorRow::restyle()
{
	ui::styleCard(this);
	ui::styleHeading(nameLabel_, 1.0);
	ui::styleMuted(detailLabel_);
	ui::styleButton(joinButton_, ui::ButtonKind::Secondary);
	joinButton_->setIcon(ui::icon(ui::Icon::Enter, ui::colors(this).text));
	ui::styleIconButton(menuButton_, ui::Icon::More, tr_("Collaborator.More"));
}

void CollaboratorRow::rename()
{
	bool accepted = false;
	const QString name = QInputDialog::getText(window(), tr_("Collaborator.Rename.Title"), tr_("Rename.Label"),
						   QLineEdit::Normal, name_, &accepted);
	if (accepted && !name.trimmed().isEmpty())
		controller_->renameCollaborator(collaboratorId_, name);
}

void CollaboratorRow::forget()
{
	const auto answer = QMessageBox::question(window(), tr_("Collaborator.Forget.Title"),
						  tr_("Collaborator.Forget.Text").arg(name_),
						  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
	if (answer == QMessageBox::Yes)
		controller_->removeCollaborator(collaboratorId_);
}

} // namespace qc
