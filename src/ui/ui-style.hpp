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

#pragma once

#include <QColor>
#include <QIcon>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QToolButton;

/**
 * Quick Cast's visual language.
 *
 * Everything is derived from the application palette rather than hard-coded, and surfaces are translucent overlays
 * of the text colour, so the dock and dialogs sit naturally on whichever OBS theme the user runs (dark or light)
 * without needing to know the exact background the theme paints. Only the three status hues are fixed, because their meaning has
 * to stay recognisable everywhere; they are never the only carrier of that meaning.
 */
namespace qc::ui {

class ElidedLabel;

struct Colors {
	QColor text;
	/** Secondary text: readable, but clearly subordinate. */
	QColor muted;
	/** Raised surface for cards and grouped controls. */
	QColor card;
	QColor cardHover;
	QColor border;
	QColor accent;
	QColor accentText;
	QColor good;
	QColor warning;
	QColor danger;
};

Colors colors(const QWidget *widget);
QColor mix(const QColor &a, const QColor &b, double amount);
/** The colour at the given opacity, for surfaces that must sit on a background we cannot query. */
QColor tint(const QColor &color, double opacity);
QString css(const QColor &color);

enum class Icon { Link, Gear, More, Plus, Check, Speaker, SpeakerMuted, Copy, Play, Stop };

/** Line icon drawn at any size in the given colour (themes cannot recolour bitmap icons). */
QIcon icon(Icon which, const QColor &color, int size = 16);

enum class ButtonKind { Primary, Secondary, Danger };

void styleButton(QPushButton *button, ButtonKind kind);
/** Square, borderless icon button with a hover surface. Sets the accessible name and tooltip from `label`. */
void styleIconButton(QToolButton *button, Icon which, const QString &label);
/** Rounded surface that groups related content. The widget needs a unique object name. */
void styleCard(QWidget *card);
/** Small all-caps status tag. */
void stylePill(QLabel *label, const QColor &background, const QColor &foreground);
void styleMuted(QLabel *label, double scale = 0.92);
void styleHeading(QLabel *label, double scale);
void styleMuted(ElidedLabel *label, double scale = 0.92);
void styleHeading(ElidedLabel *label, double scale);

/** Single-line label that shortens its text with an ellipsis instead of clipping it. */
class ElidedLabel final : public QWidget {
public:
	explicit ElidedLabel(QWidget *parent = nullptr);
	void setText(const QString &text);
	void setColor(const QColor &color);
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	QString text_;
	QColor color_;
};

/** Circle with the guest's initials, plus a presence dot. */
class Avatar final : public QWidget {
public:
	enum class Presence { Offline, Connecting, Connected };

	explicit Avatar(QWidget *parent = nullptr);
	void setName(const QString &name);
	void setPresence(Presence presence);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	QString name_;
	Presence presence_ = Presence::Offline;
};

/** Three-bar connection quality meter, like a phone's signal indicator. */
class SignalBars final : public QWidget {
public:
	explicit SignalBars(QWidget *parent = nullptr);
	/** 0 = no signal .. 3 = good. */
	void setLevel(int level);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	int level_ = 0;
};

} // namespace qc::ui
