/*
OBS-Trim
Copyright (C) 2026 BigRyPulls
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QWidget>

class TimelineWidget : public QWidget {
	Q_OBJECT
public:
	explicit TimelineWidget(QWidget *parent = nullptr);

	void setDuration(qint64 ms);
	void setInPoint(qint64 ms);
	void setOutPoint(qint64 ms);
	void setPlayhead(qint64 ms);

	qint64 inPoint() const { return inMs; }
	qint64 outPoint() const { return outMs; }
	qint64 playhead() const { return playMs; }
	qint64 duration() const { return durationMs; }

	QSize sizeHint() const override { return QSize(480, 64); }
	QSize minimumSizeHint() const override { return QSize(240, 48); }

signals:
	void inPointChanged(qint64 ms);
	void outPointChanged(qint64 ms);
	void seekRequested(qint64 ms);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;

private:
	enum class Drag { None, In, Out, Playhead };
	qint64 xToMs(double x) const;
	double msToX(qint64 ms) const;
	QRect barRect() const;
	bool nearHandle(double x, qint64 handleMs) const;

	qint64 durationMs = 1;
	qint64 inMs = 0;
	qint64 outMs = 0;
	qint64 playMs = 0;
	Drag drag = Drag::None;
};
