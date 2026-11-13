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
#include "vdo/identity.hpp"

#include <obs.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <cstring>

namespace qc {

namespace {

QLabel *hint(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text, parent);
	label->setWordWrap(true);
	label->setEnabled(false);
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
	setMinimumWidth(460);
	const Settings &settings = controller_->settings();

	// --- Show ---
	auto *showGroup = new QWidget(this);
	auto *showForm = new QFormLayout(showGroup);
	hostName_ = new QLineEdit(QString::fromStdString(settings.hostName), showGroup);
	hostName_->setMaxLength(40);
	room_ = new QLineEdit(QString::fromStdString(settings.room), showGroup);
	room_->setMaxLength(static_cast<int>(vdo::kMaxRoomIdLength));
	password_ = new QLineEdit(QString::fromStdString(settings.password), showGroup);
	password_->setMaxLength(64);
	auto *regenerate = new QPushButton(tr_("Settings.NewLinks"), showGroup);
	regenerate->setToolTip(tr_("Settings.NewLinks.Tooltip"));
	startWithObs_ = new QCheckBox(tr_("Settings.StartWithObs"), showGroup);
	startWithObs_->setChecked(settings.startWithObs);

	showForm->addRow(tr_("Settings.HostName"), hostName_);
	showForm->addRow(tr_("Settings.Room"), room_);
	showForm->addRow(tr_("Settings.Password"), password_);
	showForm->addRow(QString(), regenerate);
	showForm->addRow(hint(tr_("Settings.LinksHint"), showGroup));
	showForm->addRow(QString(), startWithObs_);

	// --- Guests ---
	auto *guestGroup = new QWidget(this);
	auto *guestForm = new QFormLayout(guestGroup);
	quality_ = new QComboBox(guestGroup);
	quality_->addItem(tr_("Settings.Quality.Low"), 1500);
	quality_->addItem(tr_("Settings.Quality.Standard"), 2500);
	quality_->addItem(tr_("Settings.Quality.High"), 4000);
	quality_->addItem(tr_("Settings.Quality.VeryHigh"), 8000);
	selectData(quality_, static_cast<int>(settings.guestBitrateKbps));
	codec_ = new QComboBox(guestGroup);
	codec_->addItem(QStringLiteral("H.264"), static_cast<int>(VideoCodec::H264));
	codec_->addItem(QStringLiteral("VP8"), static_cast<int>(VideoCodec::VP8));
	codec_->addItem(QStringLiteral("VP9"), static_cast<int>(VideoCodec::VP9));
	selectData(codec_, static_cast<int>(settings.preferredCodec));
	hardwareDecoding_ = new QCheckBox(tr_("Settings.HardwareDecoding"), guestGroup);
	hardwareDecoding_->setChecked(settings.hardwareDecoding);
	fitToCanvas_ = new QCheckBox(tr_("Settings.FitToCanvas"), guestGroup);
	fitToCanvas_->setChecked(settings.fitToCanvas);
	guestForm->addRow(tr_("Settings.Quality"), quality_);
	guestForm->addRow(tr_("Settings.Codec"), codec_);
	guestForm->addRow(hint(tr_("Settings.CodecHint"), guestGroup));
	guestForm->addRow(QString(), hardwareDecoding_);
	guestForm->addRow(hint(tr_("Settings.HardwareDecodingHint"), guestGroup));
	guestForm->addRow(QString(), fitToCanvas_);

	// --- Return feed ---
	auto *returnGroup = new QWidget(this);
	auto *returnForm = new QFormLayout(returnGroup);
	returnEnabled_ = new QCheckBox(tr_("Settings.Return.Enable"), returnGroup);
	returnEnabled_->setChecked(settings.returnFeedEnabled);
	returnVideo_ = new QComboBox(returnGroup);
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
	secondVideo_ = new QComboBox(returnGroup);
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
	returnTrack_ = new QComboBox(returnGroup);
	for (int track = 1; track <= 6; ++track)
		returnTrack_->addItem(tr_("Settings.Return.TrackN").arg(track), track);
	selectData(returnTrack_, settings.returnAudioTrack);
	const auto makeResolution = [returnGroup](uint32_t height) {
		auto *combo = new QComboBox(returnGroup);
		combo->addItem(QStringLiteral("360p"), 360);
		combo->addItem(QStringLiteral("540p"), 540);
		combo->addItem(QStringLiteral("720p"), 720);
		combo->addItem(QStringLiteral("1080p"), 1080);
		selectData(combo, static_cast<int>(height));
		combo->setAccessibleName(tr_("Settings.Return.Resolution"));
		combo->setToolTip(tr_("Settings.Return.Resolution"));
		return combo;
	};
	const auto makeBitrate = [returnGroup](uint32_t kbps) {
		auto *spin = new QSpinBox(returnGroup);
		spin->setRange(200, 8000);
		spin->setSingleStep(100);
		spin->setSuffix(QStringLiteral(" kbps"));
		spin->setValue(static_cast<int>(kbps));
		spin->setAccessibleName(tr_("Settings.Return.Bitrate"));
		spin->setToolTip(tr_("Settings.Return.Bitrate"));
		return spin;
	};
	// Each feed gets its own "resolution at bitrate" row directly under its source.
	const auto qualityRow = [returnGroup](QComboBox *resolution, QSpinBox *bitrate) {
		auto *row = new QWidget(returnGroup);
		auto *layout = new QHBoxLayout(row);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->addWidget(resolution, 1);
		layout->addWidget(bitrate, 1);
		return row;
	};
	returnResolution_ = makeResolution(settings.returnHeight);
	returnBitrate_ = makeBitrate(settings.returnVideoBitrateKbps);
	secondResolution_ = makeResolution(settings.secondHeight);
	secondBitrate_ = makeBitrate(settings.secondVideoBitrateKbps);

	returnForm->addRow(QString(), returnEnabled_);
	returnForm->addRow(tr_("Settings.Return.Video"), returnVideo_);
	returnForm->addRow(tr_("Settings.Return.Quality"), qualityRow(returnResolution_, returnBitrate_));
	returnForm->addRow(tr_("Settings.Return.Second"), secondVideo_);
	returnForm->addRow(tr_("Settings.Return.Quality"), qualityRow(secondResolution_, secondBitrate_));
	returnForm->addRow(hint(tr_("Settings.Return.VideoHint"), returnGroup));
	returnForm->addRow(tr_("Settings.Return.Track"), returnTrack_);
	returnForm->addRow(hint(tr_("Settings.Return.TrackHint"), returnGroup));

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

	// --- Network ---
	auto *networkGroup = new QWidget(this);
	auto *networkForm = new QFormLayout(networkGroup);
	turnUrl_ = new QLineEdit(QString::fromStdString(settings.turnUrl), networkGroup);
	turnUrl_->setPlaceholderText(QStringLiteral("turn:turn.example.com:3478"));
	turnUsername_ = new QLineEdit(QString::fromStdString(settings.turnUsername), networkGroup);
	turnPassword_ = new QLineEdit(QString::fromStdString(settings.turnPassword), networkGroup);
	turnPassword_->setEchoMode(QLineEdit::Password);
	networkForm->addRow(hint(tr_("Settings.Network.Hint"), networkGroup));
	networkForm->addRow(tr_("Settings.Network.TurnUrl"), turnUrl_);
	networkForm->addRow(tr_("Settings.Network.TurnUser"), turnUsername_);
	networkForm->addRow(tr_("Settings.Network.TurnPassword"), turnPassword_);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::save);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(regenerate, &QPushButton::clicked, this, &SettingsDialog::regenerateLinks);

	// Tabs keep the dialog short enough for small screens and put the everyday options first.
	auto *tabs = new QTabWidget(this);
	tabs->addTab(showGroup, tr_("Settings.Show"));
	tabs->addTab(guestGroup, tr_("Settings.Guests"));
	tabs->addTab(returnGroup, tr_("Settings.Return"));
	tabs->addTab(networkGroup, tr_("Settings.Network"));

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(tabs);
	layout->addWidget(buttons);
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
