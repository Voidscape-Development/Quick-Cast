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

#include "ui-style.hpp"

#include <QApplication>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QToolButton>

#include <algorithm>
#include <cmath>

namespace qc::ui {

QColor mix(const QColor &a, const QColor &b, double amount)
{
	const double keep = 1.0 - amount;
	return QColor(static_cast<int>(a.red() * keep + b.red() * amount + 0.5),
		      static_cast<int>(a.green() * keep + b.green() * amount + 0.5),
		      static_cast<int>(a.blue() * keep + b.blue() * amount + 0.5));
}

QColor tint(const QColor &color, double opacity)
{
	QColor result = color;
	result.setAlphaF(static_cast<float>(opacity));
	return result;
}

QString css(const QColor &color)
{
	if (color.alpha() == 255)
		return color.name(QColor::HexRgb);
	return QStringLiteral("rgba(%1, %2, %3, %4)")
		.arg(color.red())
		.arg(color.green())
		.arg(color.blue())
		.arg(color.alphaF(), 0, 'f', 3);
}

Colors colors(const QWidget *)
{
	// The application palette, not the widget's: OBS themes give individual widget classes their own palettes,
	// which would make the same role resolve to different colours from one control to the next.
	const QPalette palette = QApplication::palette();
	Colors result;
	result.text = palette.color(QPalette::WindowText);
	result.muted = tint(result.text, 0.68);
	result.card = tint(result.text, 0.06);
	result.cardHover = tint(result.text, 0.12);
	result.border = tint(result.text, 0.15);
	result.accent = palette.color(QPalette::Highlight);
	result.accentText = palette.color(QPalette::HighlightedText);

	// Status hues, darker on light themes so they keep their contrast.
	const bool dark = result.text.lightness() >= 128;
	result.good = dark ? QColor(0x3f, 0xb9, 0x50) : QColor(0x1a, 0x7f, 0x37);
	result.warning = dark ? QColor(0xd2, 0x99, 0x22) : QColor(0x9a, 0x67, 0x00);
	result.danger = dark ? QColor(0xf8, 0x51, 0x49) : QColor(0xcf, 0x22, 0x2e);
	return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------------------------------------------------

namespace {

/** Draws on a 16x16 design grid, whatever the output size. */
void paintIcon(QPainter &p, Icon which, const QColor &color)
{
	QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);

	switch (which) {
	case Icon::Link: {
		// Two chain links on a diagonal.
		p.save();
		p.translate(8, 8);
		p.rotate(-45);
		p.drawRoundedRect(QRectF(-6.5, -2.5, 7, 5), 2.5, 2.5);
		p.drawRoundedRect(QRectF(-0.5, -2.5, 7, 5), 2.5, 2.5);
		p.restore();
		break;
	}
	case Icon::Copy:
		p.drawRoundedRect(QRectF(5.5, 5.5, 8, 8), 1.5, 1.5);
		p.drawPolyline(
			QPolygonF({QPointF(10.5, 2.5), QPointF(3.5, 2.5), QPointF(2.5, 3.5), QPointF(2.5, 10.5)}));
		break;
	case Icon::Gear: {
		p.drawEllipse(QPointF(8, 8), 2.2, 2.2);
		p.drawEllipse(QPointF(8, 8), 4.6, 4.6);
		for (int i = 0; i < 8; ++i) {
			const double angle = i * 3.14159265358979323846 / 4.0;
			p.drawLine(QPointF(8 + std::cos(angle) * 4.6, 8 + std::sin(angle) * 4.6),
				   QPointF(8 + std::cos(angle) * 6.6, 8 + std::sin(angle) * 6.6));
		}
		break;
	}
	case Icon::More:
		p.setBrush(color);
		p.setPen(Qt::NoPen);
		for (const double x : {3.0, 8.0, 13.0})
			p.drawEllipse(QPointF(x, 8), 1.4, 1.4);
		break;
	case Icon::Plus:
		p.drawLine(QPointF(8, 3), QPointF(8, 13));
		p.drawLine(QPointF(3, 8), QPointF(13, 8));
		break;
	case Icon::Check:
		p.drawPolyline(QPolygonF({QPointF(3, 8.5), QPointF(6.5, 12), QPointF(13, 4.5)}));
		break;
	case Icon::Speaker:
	case Icon::SpeakerMuted: {
		QPainterPath body;
		body.moveTo(2.5, 6);
		body.lineTo(5, 6);
		body.lineTo(8.5, 3);
		body.lineTo(8.5, 13);
		body.lineTo(5, 10);
		body.lineTo(2.5, 10);
		body.closeSubpath();
		p.drawPath(body);
		if (which == Icon::Speaker) {
			p.drawArc(QRectF(7.5, 5, 5, 6), -60 * 16, 120 * 16);
			p.drawArc(QRectF(7, 2.5, 8, 11), -55 * 16, 110 * 16);
		} else {
			p.drawLine(QPointF(11, 6), QPointF(14.5, 10));
			p.drawLine(QPointF(14.5, 6), QPointF(11, 10));
		}
		break;
	}
	case Icon::Play: {
		QPainterPath triangle;
		triangle.moveTo(5, 3.5);
		triangle.lineTo(12.5, 8);
		triangle.lineTo(5, 12.5);
		triangle.closeSubpath();
		p.setBrush(color);
		p.drawPath(triangle);
		break;
	}
	case Icon::Stop:
		p.setBrush(color);
		p.drawRoundedRect(QRectF(4, 4, 8, 8), 1.5, 1.5);
		break;
	case Icon::Layout:
		// A frame split into one wide tile and two small ones.
		p.drawRoundedRect(QRectF(2.5, 3.5, 11, 9), 1.5, 1.5);
		p.drawLine(QPointF(8.5, 3.5), QPointF(8.5, 12.5));
		p.drawLine(QPointF(8.5, 8), QPointF(13.5, 8));
		break;
	case Icon::Enter:
		// An arrow going in through a doorway.
		p.drawPolyline(
			QPolygonF({QPointF(9.5, 2.5), QPointF(13.5, 2.5), QPointF(13.5, 13.5), QPointF(9.5, 13.5)}));
		p.drawLine(QPointF(2.5, 8), QPointF(9.5, 8));
		p.drawPolyline(QPolygonF({QPointF(6.5, 5), QPointF(9.5, 8), QPointF(6.5, 11)}));
		break;
	}
}

} // namespace

QIcon icon(Icon which, const QColor &color, int size)
{
	QIcon result;
	// 1x and 2x renditions so the icon stays crisp on high-DPI displays.
	for (const int scale : {1, 2, 3}) {
		QPixmap pixmap(size * scale, size * scale);
		pixmap.fill(Qt::transparent);
		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.scale(size * scale / 16.0, size * scale / 16.0);
		paintIcon(painter, which, color);
		painter.end();
		pixmap.setDevicePixelRatio(scale);
		result.addPixmap(pixmap);
	}
	return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Styling
// ---------------------------------------------------------------------------------------------------------------------

void styleButton(QPushButton *button, ButtonKind kind)
{
	const Colors c = colors(button);
	QColor background, hover, pressed, text, border;
	switch (kind) {
	case ButtonKind::Primary:
		background = c.accent;
		hover = mix(c.accent, c.accentText, 0.12);
		pressed = mix(c.accent, QColor(Qt::black), 0.2);
		text = c.accentText;
		border = c.accent;
		break;
	case ButtonKind::Danger:
		background = QColor(0, 0, 0, 0);
		hover = tint(c.danger, 0.16);
		pressed = tint(c.danger, 0.28);
		text = c.danger;
		border = tint(c.danger, 0.6);
		break;
	case ButtonKind::Secondary:
		background = c.card;
		hover = c.cardHover;
		pressed = c.border;
		text = c.text;
		border = c.border;
		break;
	}
	button->setCursor(Qt::PointingHandCursor);
	button->setStyleSheet(
		QStringLiteral("QPushButton { background-color: %1; color: %2; border: 1px solid %3;"
			       " border-radius: 6px; padding: 6px 12px; font-weight: 600; min-height: 18px; }"
			       "QPushButton:hover { background-color: %4; }"
			       "QPushButton:pressed { background-color: %5; }"
			       "QPushButton:focus { border: 1px solid %6; }"
			       "QPushButton:disabled { background-color: %7; color: %8; border-color: %7; }")
			.arg(css(background), css(text), css(border), css(hover), css(pressed),
			     css(kind == ButtonKind::Primary ? c.accentText : c.accent), css(c.card), css(c.muted)));
}

void styleIconButton(QToolButton *button, Icon which, const QString &label)
{
	const Colors c = colors(button);
	button->setIcon(icon(which, c.text));
	button->setIconSize(QSize(16, 16));
	button->setFixedSize(30, 30);
	button->setToolTip(label);
	button->setAccessibleName(label);
	button->setCursor(Qt::PointingHandCursor);
	button->setAutoRaise(true);
	button->setStyleSheet(QStringLiteral("QToolButton { background: transparent; border: 1px solid transparent;"
					     " border-radius: 6px; padding: 0; }"
					     "QToolButton:hover { background-color: %1; }"
					     "QToolButton:pressed, QToolButton:checked { background-color: %2; }"
					     "QToolButton:focus { border: 1px solid %3; }"
					     "QToolButton::menu-indicator { image: none; width: 0; }")
				      .arg(css(c.cardHover), css(c.border), css(c.accent)));
}

void styleCard(QWidget *card)
{
	const Colors c = colors(card);
	card->setAttribute(Qt::WA_StyledBackground, true);
	card->setStyleSheet(QStringLiteral("#%1 { background-color: %2; border: 1px solid %3; border-radius: 8px; }")
				    .arg(card->objectName(), css(c.card), css(c.border)));
}

void stylePill(QLabel *label, const QColor &background, const QColor &foreground)
{
	QFont font = label->font();
	font.setBold(true);
	font.setPointSizeF(font.pointSizeF() * 0.82);
	font.setLetterSpacing(QFont::PercentageSpacing, 106);
	label->setFont(font);
	label->setStyleSheet(QStringLiteral("background-color: %1; color: %2; border-radius: 4px; padding: 2px 7px;")
				     .arg(css(background), css(foreground)));
	label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

namespace {
/** Font sizes go through the stylesheet: OBS themes set sizes there, which would override QWidget::setFont. */
QString fontSize(double scale)
{
	return QStringLiteral("font-size: %1pt;").arg(QApplication::font().pointSizeF() * scale, 0, 'f', 1);
}
} // namespace

void styleMuted(QLabel *label, double scale)
{
	label->setStyleSheet(
		QStringLiteral("color: %1; background: transparent; %2").arg(css(colors(label).muted), fontSize(scale)));
}

void styleMuted(ElidedLabel *label, double scale)
{
	QFont font = QApplication::font();
	font.setPointSizeF(font.pointSizeF() * scale);
	label->setFont(font);
	label->setColor(colors(label).muted);
}

void styleHeading(ElidedLabel *label, double scale)
{
	QFont font = QApplication::font();
	font.setBold(true);
	font.setPointSizeF(font.pointSizeF() * scale);
	label->setFont(font);
	label->setColor(colors(label).text);
}

void styleHeading(QLabel *label, double scale)
{
	label->setStyleSheet(QStringLiteral("background: transparent; font-weight: 700; %1").arg(fontSize(scale)));
}

// ---------------------------------------------------------------------------------------------------------------------
// Elided label
// ---------------------------------------------------------------------------------------------------------------------

ElidedLabel::ElidedLabel(QWidget *parent) : QWidget(parent)
{
	setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}

void ElidedLabel::setText(const QString &text)
{
	if (text_ == text)
		return;
	text_ = text;
	setToolTip(text);
	setAccessibleName(text);
	update();
}

void ElidedLabel::setColor(const QColor &color)
{
	color_ = color;
	updateGeometry();
	update();
}

QSize ElidedLabel::sizeHint() const
{
	const QFontMetrics metrics(font());
	return QSize(metrics.horizontalAdvance(text_), metrics.height() + 2);
}

QSize ElidedLabel::minimumSizeHint() const
{
	return QSize(24, QFontMetrics(font()).height() + 2);
}

void ElidedLabel::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setFont(font());
	painter.setPen(color_.isValid() ? color_ : colors(this).text);
	const QString shown = painter.fontMetrics().elidedText(text_, Qt::ElideRight, width());
	painter.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, shown);
}

// ---------------------------------------------------------------------------------------------------------------------
// Avatar
// ---------------------------------------------------------------------------------------------------------------------

Avatar::Avatar(QWidget *parent) : QWidget(parent)
{
	setFixedSize(38, 38);
}

void Avatar::setName(const QString &name)
{
	if (name_ == name)
		return;
	name_ = name;
	update();
}

void Avatar::setPresence(Presence presence)
{
	if (presence_ == presence)
		return;
	presence_ = presence;
	update();
}

void Avatar::paintEvent(QPaintEvent *)
{
	const Colors c = colors(this);
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	// A stable colour per name makes people easy to tell apart at a glance.
	uint hash = 0;
	for (const QChar ch : name_)
		hash = hash * 31 + ch.unicode();
	QColor fill = QColor::fromHsl(static_cast<int>(hash % 360), 110, 105);
	if (presence_ == Presence::Offline)
		fill = tint(fill, 0.38);

	const QRectF circle(1, 1, 34, 34);
	painter.setPen(Qt::NoPen);
	painter.setBrush(fill);
	painter.drawEllipse(circle);

	QString initials;
	const QStringList words = name_.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	if (!words.isEmpty())
		initials += words.first().at(0).toUpper();
	if (words.size() > 1)
		initials += words.last().at(0).toUpper();
	if (initials.isEmpty())
		initials = QStringLiteral("?");

	QFont font = this->font();
	font.setBold(true);
	font.setPixelSize(initials.size() > 1 ? 13 : 15);
	painter.setFont(font);
	painter.setPen(presence_ == Presence::Offline ? tint(c.text, 0.7) : QColor(Qt::white));
	painter.drawText(circle, Qt::AlignCenter, initials);

	if (presence_ != Presence::Offline) {
		// Presence dot with a dark ring so it reads against any avatar colour.
		const QRectF dot(25, 25, 12, 12);
		painter.setPen(QPen(QColor(0, 0, 0, 150), 2));
		painter.setBrush(presence_ == Presence::Connected ? c.good : c.warning);
		painter.drawEllipse(dot);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Signal bars
// ---------------------------------------------------------------------------------------------------------------------

SignalBars::SignalBars(QWidget *parent) : QWidget(parent)
{
	setFixedSize(16, 12);
}

void SignalBars::setLevel(int level)
{
	level = std::clamp(level, 0, 3);
	if (level_ == level)
		return;
	level_ = level;
	update();
}

void SignalBars::paintEvent(QPaintEvent *)
{
	const Colors c = colors(this);
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);

	const QColor active = level_ >= 3 ? c.good : level_ == 2 ? c.warning : c.danger;
	for (int bar = 0; bar < 3; ++bar) {
		const double height = 4.0 + bar * 4.0;
		painter.setBrush(bar < level_ ? active : c.border);
		painter.drawRoundedRect(QRectF(bar * 5.5, 12.0 - height, 4.0, height), 1.0, 1.0);
	}
}

} // namespace qc::ui
