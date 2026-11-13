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

#include "settings-dialog.hpp"

#include "obs/scene-integration.hpp"
#include "ui-helpers.hpp"
#include "ui-style.hpp"
#include "vdo/identity.hpp"

#include <obs.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <cstring>

namespace qc {

namespace {

QLabel *hint(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text, parent);
	label->setWordWrap(true);
	ui::styleMuted(label);
	return label;
}

/** Sources first, then scenes: everything that produces video, excluding the guests themselves. */
QStringList videoSourceNames()
{
	QStringList scenes;
	QStringList sources;
	struct Lists {
		QStringList *scenes;
		QStringList *sources;
	} lists{&scenes, &sources};

	const auto collect = [](void *param, obs_source_t *source) {
		auto *out = static_cast<Lists *>(param);
		const char *name = obs_source_get_name(source);
		if (!name || !*name || (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0)
			return true;
		const char *id = obs_source_get_unversioned_id(source);
		if (id && strcmp(id, kGuestSourceId) == 0)
			return true;
		if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE)
			out->scenes->append(QString::fromUtf8(name));
		else
			out->sources->append(QString::fromUtf8(name));
		return true;
	};
	obs_enum_scenes(collect, &lists);
	obs_enum_sources(collect, &lists);
	scenes.sort(Qt::CaseInsensitive);
	sources.sort(Qt::CaseInsensitive);
	return sources + scenes;
}

void selectData(QComboBox *combo, int value)
{
	const int index = combo->findData(value);
	combo->setCurrentIndex(index >= 0 ? index : 0);
}

} // namespace

SettingsDialog::SettingsDialog(SessionController *controller, QWidget *parent)
	: QDialog(parent),
	  controller_(controller)
{
	setWindowTitle(tr_("Settings.Title"));
	setMinimumSize(700, 500);
	const Settings &settings = controller_->settings();
	const ui::Colors c = ui::colors(this);

	// Builders that keep every page on the same rhythm: page title, one-line purpose, then titled groups.
	const auto makePage = [this](const char *title, const char *purpose, QVBoxLayout *&body) {
		auto *content = new QWidget(this);
		body = new QVBoxLayout(content);
		body->setContentsMargins(24, 20, 24, 20);
		body->setSpacing(8);
		auto *heading = new QLabel(tr_(title), content);
		ui::styleHeading(heading, 1.35);
		auto *summary = new QLabel(tr_(purpose), content);
		summary->setWordWrap(true);
		ui::styleMuted(summary, 1.0);
		body->addWidget(heading);
		body->addWidget(summary);
		body->addSpacing(6);

		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll->setWidget(content);
		scroll->viewport()->setAutoFillBackground(false);
		content->setAutoFillBackground(false);
		return scroll;
	};
	int groupCount = 0;
	const auto makeGroup = [this, &groupCount](QVBoxLayout *body, const char *title) {
		auto *label = new QLabel(tr_(title), body->parentWidget());
		ui::styleMuted(label, 0.88);
		label->setStyleSheet(label->styleSheet() + QStringLiteral(" font-weight: 700;"));
		QFont font = label->font();
		font.setBold(true);
		font.setCapitalization(QFont::AllUppercase);
		font.setLetterSpacing(QFont::PercentageSpacing, 108);
		label->setFont(font);
		label->setContentsMargins(2, 8, 0, 0);

		auto *card = new QWidget(body->parentWidget());
		card->setObjectName(QStringLiteral("quickCastSettingsGroup%1").arg(groupCount++));
		ui::styleCard(card);
		auto *form = new QFormLayout(card);
		form->setContentsMargins(14, 12, 14, 12);
		form->setHorizontalSpacing(14);
		form->setVerticalSpacing(8);
		form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		body->addWidget(label);
		body->addWidget(card);
		return form;
	};

	// ------------------------------------------------------------------------------------------------- Show
	QVBoxLayout *showBody = nullptr;
	QWidget *showPage = makePage("Settings.Show.Heading", "Settings.Show.Purpose", showBody);

	QFormLayout *identity = makeGroup(showBody, "Settings.Group.Identity");
	hostName_ = new QLineEdit(QString::fromStdString(settings.hostName), this);
	hostName_->setMaxLength(40);
	identity->addRow(tr_("Settings.HostName"), hostName_);
	identity->addRow(hint(tr_("Settings.HostNameHint"), this));

	QFormLayout *links = makeGroup(showBody, "Settings.Group.Links");
	room_ = new QLineEdit(QString::fromStdString(settings.room), this);
	room_->setMaxLength(static_cast<int>(vdo::kMaxRoomIdLength));
	password_ = new QLineEdit(QString::fromStdString(settings.password), this);
	password_->setMaxLength(64);
	auto *regenerate = new QPushButton(tr_("Settings.NewLinks"), this);
	regenerate->setToolTip(tr_("Settings.NewLinks.Tooltip"));
	ui::styleButton(regenerate, ui::ButtonKind::Danger);
	links->addRow(tr_("Settings.Room"), room_);
	links->addRow(tr_("Settings.Password"), password_);
	links->addRow(hint(tr_("Settings.LinksHint"), this));
	auto *regenerateRow = new QHBoxLayout();
	regenerateRow->addWidget(regenerate);
	regenerateRow->addStretch(1);
	links->addRow(regenerateRow);

	QFormLayout *startup = makeGroup(showBody, "Settings.Group.Startup");
	startWithObs_ = new QCheckBox(tr_("Settings.StartWithObs"), this);
	startWithObs_->setChecked(settings.startWithObs);
	startup->addRow(startWithObs_);
	showBody->addStretch(1);

	// ------------------------------------------------------------------------------------------ Guest video
	QVBoxLayout *guestBody = nullptr;
	QWidget *guestPage = makePage("Settings.Guests.Heading", "Settings.Guests.Purpose", guestBody);

	QFormLayout *quality = makeGroup(guestBody, "Settings.Group.Quality");
	quality_ = new QComboBox(this);
	quality_->addItem(tr_("Settings.Quality.Low"), 1500);
	quality_->addItem(tr_("Settings.Quality.Standard"), 2500);
	quality_->addItem(tr_("Settings.Quality.High"), 4000);
	quality_->addItem(tr_("Settings.Quality.VeryHigh"), 8000);
	selectData(quality_, static_cast<int>(settings.guestBitrateKbps));
	codec_ = new QComboBox(this);
	codec_->addItem(QStringLiteral("H.264"), static_cast<int>(VideoCodec::H264));
	codec_->addItem(QStringLiteral("VP8"), static_cast<int>(VideoCodec::VP8));
	codec_->addItem(QStringLiteral("VP9"), static_cast<int>(VideoCodec::VP9));
	selectData(codec_, static_cast<int>(settings.preferredCodec));
	quality->addRow(tr_("Settings.Quality"), quality_);
	quality->addRow(tr_("Settings.Codec"), codec_);
	quality->addRow(hint(tr_("Settings.CodecHint"), this));

	QFormLayout *performance = makeGroup(guestBody, "Settings.Group.Performance");
	hardwareDecoding_ = new QCheckBox(tr_("Settings.HardwareDecoding"), this);
	hardwareDecoding_->setChecked(settings.hardwareDecoding);
	performance->addRow(hardwareDecoding_);
	performance->addRow(hint(tr_("Settings.HardwareDecodingHint"), this));

	QFormLayout *scenes = makeGroup(guestBody, "Settings.Group.Scenes");
	fitToCanvas_ = new QCheckBox(tr_("Settings.FitToCanvas"), this);
	fitToCanvas_->setChecked(settings.fitToCanvas);
	scenes->addRow(fitToCanvas_);
	guestBody->addStretch(1);

	// ------------------------------------------------------------------------------------- What guests see
	QVBoxLayout *returnBody = nullptr;
	QWidget *returnPage = makePage("Settings.Return.Heading", "Settings.Return.Purpose", returnBody);

	returnEnabled_ = new QCheckBox(tr_("Settings.Return.Enable"), this);
	returnEnabled_->setChecked(settings.returnFeedEnabled);
	returnBody->addWidget(returnEnabled_);

	returnVideo_ = new QComboBox(this);
	returnVideo_->addItem(tr_("Settings.Return.Video.Program"), QString());
	for (const QString &name : videoSourceNames())
		returnVideo_->addItem(name, name);
	{
		// Keep a configured source selectable even if it is not in the current scene collection.
		const QString current = QString::fromStdString(settings.returnVideoSource);
		int index = returnVideo_->findData(current);
		if (index < 0) {
			returnVideo_->addItem(current, current);
			index = returnVideo_->count() - 1;
		}
		returnVideo_->setCurrentIndex(index);
	}

	// Second feed: off, the program, or any source. "Off" is a null variant; the program is an empty name.
	secondVideo_ = new QComboBox(this);
	secondVideo_->addItem(tr_("Settings.Return.Second.Off"), QVariant());
	secondVideo_->addItem(tr_("Settings.Return.Video.Program"), QString());
	for (const QString &name : videoSourceNames())
		secondVideo_->addItem(name, name);
	if (!settings.secondFeedEnabled) {
		secondVideo_->setCurrentIndex(0);
	} else {
		const QString current = QString::fromStdString(settings.secondVideoSource);
		int index = -1;
		for (int i = 1; i < secondVideo_->count(); ++i) {
			if (secondVideo_->itemData(i).toString() == current) {
				index = i;
				break;
			}
		}
		if (index < 0) {
			secondVideo_->addItem(current, current);
			index = secondVideo_->count() - 1;
		}
		secondVideo_->setCurrentIndex(index);
	}

	const auto makeResolution = [this](uint32_t height) {
		auto *combo = new QComboBox(this);
		combo->addItem(QStringLiteral("360p"), 360);
		combo->addItem(QStringLiteral("540p"), 540);
		combo->addItem(QStringLiteral("720p"), 720);
		combo->addItem(QStringLiteral("1080p"), 1080);
		selectData(combo, static_cast<int>(height));
		combo->setAccessibleName(tr_("Settings.Return.Resolution"));
		combo->setToolTip(tr_("Settings.Return.Resolution"));
		return combo;
	};
	const auto makeBitrate = [this](uint32_t kbps) {
		auto *spin = new QSpinBox(this);
		spin->setRange(200, 8000);
		spin->setSingleStep(100);
		spin->setSuffix(QStringLiteral(" kbps"));
		spin->setValue(static_cast<int>(kbps));
		spin->setAccessibleName(tr_("Settings.Return.Bitrate"));
		spin->setToolTip(tr_("Settings.Return.Bitrate"));
		return spin;
	};
	const auto qualityRow = [](QComboBox *resolution, QSpinBox *bitrate) {
		auto *row = new QHBoxLayout();
		row->setContentsMargins(0, 0, 0, 0);
		row->addWidget(resolution, 1);
		row->addWidget(bitrate, 1);
		return row;
	};
	returnResolution_ = makeResolution(settings.returnHeight);
	returnBitrate_ = makeBitrate(settings.returnVideoBitrateKbps);
	secondResolution_ = makeResolution(settings.secondHeight);
	secondBitrate_ = makeBitrate(settings.secondVideoBitrateKbps);

	QFormLayout *mainFeed = makeGroup(returnBody, "Settings.Return.Video");
	mainFeed->addRow(tr_("Settings.Return.Source"), returnVideo_);
	mainFeed->addRow(tr_("Settings.Return.Quality"), qualityRow(returnResolution_, returnBitrate_));
	mainFeed->addRow(hint(tr_("Settings.Return.MainHint"), this));

	QFormLayout *secondFeed = makeGroup(returnBody, "Settings.Return.Second");
	secondFeed->addRow(tr_("Settings.Return.Source"), secondVideo_);
	secondFeed->addRow(tr_("Settings.Return.Quality"), qualityRow(secondResolution_, secondBitrate_));
	secondFeed->addRow(hint(tr_("Settings.Return.SecondHint"), this));

	QFormLayout *audio = makeGroup(returnBody, "Settings.Group.Audio");
	returnTrack_ = new QComboBox(this);
	for (int track = 1; track <= 6; ++track)
		returnTrack_->addItem(tr_("Settings.Return.TrackN").arg(track), track);
	selectData(returnTrack_, settings.returnAudioTrack);
	audio->addRow(tr_("Settings.Return.Track"), returnTrack_);
	audio->addRow(hint(tr_("Settings.Return.TrackHint"), this));
	returnBody->addStretch(1);

	const auto syncReturnControls = [this] {
		const bool enabled = returnEnabled_->isChecked();
		returnVideo_->setEnabled(enabled);
		secondVideo_->setEnabled(enabled);
		returnTrack_->setEnabled(enabled);
		returnResolution_->setEnabled(enabled);
		returnBitrate_->setEnabled(enabled);
		const bool second = enabled && secondVideo_->currentIndex() > 0;
		secondResolution_->setEnabled(second);
		secondBitrate_->setEnabled(second);
	};
	connect(returnEnabled_, &QCheckBox::toggled, this, syncReturnControls);
	connect(secondVideo_, &QComboBox::currentIndexChanged, this, syncReturnControls);
	syncReturnControls();

	// ---------------------------------------------------------------------------------------------- Network
	QVBoxLayout *networkBody = nullptr;
	QWidget *networkPage = makePage("Settings.Network.Heading", "Settings.Network.Purpose", networkBody);
	QFormLayout *turn = makeGroup(networkBody, "Settings.Group.Turn");
	turnUrl_ = new QLineEdit(QString::fromStdString(settings.turnUrl), this);
	turnUrl_->setPlaceholderText(QStringLiteral("turn:turn.example.com:3478"));
	turnUsername_ = new QLineEdit(QString::fromStdString(settings.turnUsername), this);
	turnPassword_ = new QLineEdit(QString::fromStdString(settings.turnPassword), this);
	turnPassword_->setEchoMode(QLineEdit::Password);
	turn->addRow(tr_("Settings.Network.TurnUrl"), turnUrl_);
	turn->addRow(tr_("Settings.Network.TurnUser"), turnUsername_);
	turn->addRow(tr_("Settings.Network.TurnPassword"), turnPassword_);
	turn->addRow(hint(tr_("Settings.Network.Hint"), this));
	networkBody->addStretch(1);

	// ------------------------------------------------------------------------------------------ Navigation
	// A sidebar instead of tabs: the sections are few, unequal in size, and worth naming in full.
	auto *navigation = new QListWidget(this);
	navigation->setFixedWidth(176);
	navigation->setFrameShape(QFrame::NoFrame);
	navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	navigation->setAccessibleName(tr_("Settings.Title"));
	navigation->setStyleSheet(
		QStringLiteral(
			"QListWidget { background-color: %1; border: none; border-right: 1px solid %2;"
			" padding: 12px 8px; outline: 0; }"
			"QListWidget::item { padding: 8px 10px; border-radius: 6px; margin-bottom: 2px; color: %3; }"
			"QListWidget::item:hover { background-color: %4; }"
			"QListWidget::item:selected { background-color: %5; color: %6; }")
			.arg(ui::css(c.card), ui::css(c.border), ui::css(c.text), ui::css(c.cardHover),
			     ui::css(c.accent), ui::css(c.accentText)));
	auto *pages = new QStackedWidget(this);
	const auto addPage = [&](const char *name, QWidget *page) {
		navigation->addItem(tr_(name));
		pages->addWidget(page);
	};
	addPage("Settings.Show", showPage);
	addPage("Settings.Guests", guestPage);
	addPage("Settings.Return", returnPage);
	addPage("Settings.Network", networkPage);
	connect(navigation, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
	navigation->setCurrentRow(0);

	auto *saveButton = new QPushButton(tr_("Settings.Save"), this);
	saveButton->setDefault(true);
	ui::styleButton(saveButton, ui::ButtonKind::Primary);
	auto *cancelButton = new QPushButton(tr_("Settings.Cancel"), this);
	ui::styleButton(cancelButton, ui::ButtonKind::Secondary);
	connect(saveButton, &QPushButton::clicked, this, &SettingsDialog::save);
	connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
	connect(regenerate, &QPushButton::clicked, this, &SettingsDialog::regenerateLinks);

	auto *footer = new QWidget(this);
	footer->setObjectName(QStringLiteral("quickCastSettingsFooter"));
	footer->setAttribute(Qt::WA_StyledBackground, true);
	footer->setStyleSheet(
		QStringLiteral("#quickCastSettingsFooter { border-top: 1px solid %1; }").arg(ui::css(c.border)));
	auto *footerLayout = new QHBoxLayout(footer);
	footerLayout->setContentsMargins(16, 10, 16, 10);
	footerLayout->addStretch(1);
	footerLayout->addWidget(cancelButton);
	footerLayout->addWidget(saveButton);

	auto *body = new QHBoxLayout();
	body->setContentsMargins(0, 0, 0, 0);
	body->setSpacing(0);
	body->addWidget(navigation);
	body->addWidget(pages, 1);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addLayout(body, 1);
	layout->addWidget(footer);
}

void SettingsDialog::regenerateLinks()
{
	const auto answer = QMessageBox::warning(this, tr_("Settings.NewLinks.ConfirmTitle"),
						 tr_("Settings.NewLinks.ConfirmText"),
						 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
	if (answer != QMessageBox::Yes)
		return;
	room_->setText(QString::fromStdString(vdo::generateRoomName()));
	password_->setText(QString::fromStdString(vdo::generatePassword()));
}

void SettingsDialog::save()
{
	Settings settings = controller_->settings();

	const std::string room = vdo::sanitizeId(room_->text().toStdString(), vdo::kMaxRoomIdLength);
	if (room.empty()) {
		QMessageBox::warning(this, tr_("Settings.Title"), tr_("Settings.RoomRequired"));
		room_->setFocus();
		return;
	}

	settings.hostName = hostName_->text().trimmed().toStdString();
	if (settings.hostName.empty())
		settings.hostName = "Host";
	settings.room = room;
	settings.password = password_->text().trimmed().toStdString();
	// A show always has a password of its own; an empty one would silently fall back to a well-known default.
	if (settings.password.empty())
		settings.password = vdo::generatePassword();
	settings.startWithObs = startWithObs_->isChecked();

	settings.guestBitrateKbps = static_cast<uint32_t>(quality_->currentData().toInt());
	settings.preferredCodec = static_cast<VideoCodec>(codec_->currentData().toInt());
	settings.hardwareDecoding = hardwareDecoding_->isChecked();
	settings.fitToCanvas = fitToCanvas_->isChecked();

	settings.returnFeedEnabled = returnEnabled_->isChecked();
	settings.returnVideoSource = returnVideo_->currentData().toString().toStdString();
	settings.secondFeedEnabled = secondVideo_->currentIndex() > 0;
	settings.secondVideoSource = secondVideo_->currentData().toString().toStdString();
	settings.returnAudioTrack = returnTrack_->currentData().toInt();
	settings.returnHeight = static_cast<uint32_t>(returnResolution_->currentData().toInt());
	settings.returnVideoBitrateKbps = static_cast<uint32_t>(returnBitrate_->value());
	settings.secondHeight = static_cast<uint32_t>(secondResolution_->currentData().toInt());
	settings.secondVideoBitrateKbps = static_cast<uint32_t>(secondBitrate_->value());

	settings.turnUrl = turnUrl_->text().trimmed().toStdString();
	settings.turnUsername = turnUsername_->text().trimmed().toStdString();
	settings.turnPassword = turnPassword_->text().toStdString();

	controller_->applySettings(settings);
	accept();
}

} // namespace qc
