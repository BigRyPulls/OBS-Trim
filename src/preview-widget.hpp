/*
OBS-Trim
Copyright (C) 2026 BigRyPulls
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QWidget>
#include <QTimer>

#include <obs.h>

class ObsDisplayWidget : public QWidget {
	Q_OBJECT
public:
	explicit ObsDisplayWidget(QWidget *parent = nullptr);
	~ObsDisplayWidget() override;

	void setSource(obs_source_t *s) { source = s; }
	void shutdown();

	QPaintEngine *paintEngine() const override { return nullptr; }

signals:
	void clicked();

protected:
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	bool event(QEvent *event) override;

private:
	void createDisplay();
	void destroyDisplay();
	static void drawCallback(void *data, uint32_t cx, uint32_t cy);

	obs_source_t *source = nullptr; // not owned
	obs_display_t *display = nullptr;
	void *boundHwnd = nullptr;
};

/*
 * Simple preview built on OBS's own ffmpeg_source (media source).
 * No custom FFmpeg decode thread: OBS decodes, we render via obs_display
 * and control via obs_source_media_*.
 */
class MediaPreview : public QWidget {
	Q_OBJECT
public:
	explicit MediaPreview(QWidget *parent = nullptr);
	~MediaPreview() override;

	bool openFile(const QString &path);
	void closeFile();
	void shutdown();

	void play();
	void pause();
	void togglePlayPause();
	bool isPlaying() const;

	qint64 durationMs() const { return cachedDuration; }
	qint64 positionMs() const { return cachedPosition; }

	void seekTo(qint64 ms);

signals:
	void positionChanged(qint64 ms);
	void durationChanged(qint64 ms);
	void playStateChanged(bool playing);

private:
	void poll();

	obs_source_t *source = nullptr; // owned
	ObsDisplayWidget *displayWidget = nullptr;
	QTimer pollTimer;
	qint64 cachedDuration = 0;
	qint64 cachedPosition = 0;
	bool lastPlaying = false;
	QString currentPath;
};
