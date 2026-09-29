/*
OBS-Trim
Copyright (C) 2026 BigRyPulls
SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "timeline-widget.hpp"

#include <QPainter>
#include <QMouseEvent>
#include <QtMath>

TimelineWidget::TimelineWidget(QWidget *parent) : QWidget(parent)
{
	setMouseTracking(true);
}

void TimelineWidget::setDuration(qint64 ms)
{
	durationMs = qMax<qint64>(1, ms);
	if (outMs == 0 || outMs > durationMs)
		outMs = durationMs;
	inMs = qBound<qint64>(0, inMs, outMs);
	playMs = qBound<qint64>(0, playMs, durationMs);
	update();
}

void TimelineWidget::setInPoint(qint64 ms)
{
	ms = qBound<qint64>(0, ms, outMs);
	if (ms != inMs) {
		inMs = ms;
		update();
		emit inPointChanged(inMs);
	}
}

void TimelineWidget::setOutPoint(qint64 ms)
{
	if (durationMs <= 0)
		return;
	ms = qBound<qint64>(inMs, ms, durationMs);
	if (ms != outMs) {
		outMs = ms;
		update();
		emit outPointChanged(outMs);
	}
}

void TimelineWidget::setPlayhead(qint64 ms)
{
	ms = qBound<qint64>(0, ms, durationMs);
	if (ms != playMs) {
		playMs = ms;
		update();
	}
}

QRect TimelineWidget::barRect() const
{
	const int pad = 12;
	return QRect(pad, height() / 2 - 10, width() - pad * 2, 20);
}

qint64 TimelineWidget::xToMs(double x) const
{
	QRect r = barRect();
	double t = (x - r.left()) / double(qMax(1, r.width()));
	t = qBound(0.0, t, 1.0);
	return qint64(t * double(durationMs));
}

double TimelineWidget::msToX(qint64 ms) const
{
	QRect r = barRect();
	double t = double(ms) / double(qMax<qint64>(1, durationMs));
	return r.left() + t * r.width();
}

bool TimelineWidget::nearHandle(double x, qint64 handleMs) const
{
	return qAbs(x - msToX(handleMs)) <= 8.0;
}

void TimelineWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.fillRect(rect(), palette().color(QPalette::Base));

	QRect bar = barRect();
	// full bar
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(60, 60, 60));
	p.drawRoundedRect(bar, 6, 6);
	// selected range
	int x1 = int(msToX(inMs));
	int x2 = int(msToX(outMs));
	QRect sel(x1, bar.top(), qMax(2, x2 - x1), bar.height());
	p.setBrush(QColor(45, 120, 200, 180));
	p.drawRoundedRect(sel, 6, 6);
	// in handle
	p.setBrush(QColor(80, 200, 120));
	p.drawRect(QRect(x1 - 3, bar.top() - 4, 6, bar.height() + 8));
	// out handle
	p.setBrush(QColor(220, 90, 90));
	p.drawRect(QRect(x2 - 3, bar.top() - 4, 6, bar.height() + 8));
	// playhead
	int xp = int(msToX(playMs));
	p.setPen(QPen(QColor(240, 240, 240), 2));
	p.drawLine(xp, bar.top() - 8, xp, bar.bottom() + 8);
}

void TimelineWidget::mousePressEvent(QMouseEvent *event)
{
	double x = event->position().x();
	if (nearHandle(x, inMs)) {
		drag = Drag::In;
	} else if (nearHandle(x, outMs)) {
		drag = Drag::Out;
	} else if (barRect().contains(event->pos())) {
		drag = Drag::Playhead;
		qint64 ms = xToMs(x);
		setPlayhead(ms);
		emit seekRequested(ms);
	} else {
		drag = Drag::None;
	}
}

void TimelineWidget::mouseMoveEvent(QMouseEvent *event)
{
	if (drag == Drag::None)
		return;
	qint64 ms = xToMs(event->position().x());
	if (drag == Drag::In) {
		ms = qBound<qint64>(0, ms, outMs - 1);
		if (ms != inMs) {
			inMs = ms;
			update();
			emit inPointChanged(inMs);
		}
	} else if (drag == Drag::Out) {
		ms = qBound<qint64>(inMs + 1, ms, durationMs);
		if (ms != outMs) {
			outMs = ms;
			update();
			emit outPointChanged(outMs);
		}
	} else if (drag == Drag::Playhead) {
		setPlayhead(ms);
		emit seekRequested(ms);
	}
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent *)
{
	drag = Drag::None;
}
