/*
OBS-Trim
Copyright (C) 2026 BigRyPulls
SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "preview-widget.hpp"

#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>

#include <obs-module.h>
#include <util/bmem.h>

ObsDisplayWidget::ObsDisplayWidget(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setMinimumSize(320, 180);
}

ObsDisplayWidget::~ObsDisplayWidget()
{
	shutdown();
}

void ObsDisplayWidget::destroyDisplay()
{
	if (display) {
		obs_display_remove_draw_callback(display, drawCallback, this);
		obs_display_destroy(display);
		display = nullptr;
	}
	boundHwnd = nullptr;
}

void ObsDisplayWidget::shutdown()
{
	destroyDisplay();
	source = nullptr;
}

bool ObsDisplayWidget::event(QEvent *event)
{
	if (event->type() == QEvent::WinIdChange && display)
		destroyDisplay();
	return QWidget::event(event);
}

void ObsDisplayWidget::createDisplay()
{
	if (!windowHandle() && !winId())
		return;
	if (display) {
		if (reinterpret_cast<void *>(winId()) == boundHwnd)
			return;
		destroyDisplay();
	}

	gs_init_data info = {};
	QSize size = this->size() * devicePixelRatioF();
	info.cx = (uint32_t)std::max(size.width(), 16);
	info.cy = (uint32_t)std::max(size.height(), 16);
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	info.window.hwnd = reinterpret_cast<void *>(winId());

	display = obs_display_create(&info, 0xFF101010);
	if (display) {
		obs_display_add_draw_callback(display, drawCallback, this);
		boundHwnd = info.window.hwnd;
	}
}

void ObsDisplayWidget::drawCallback(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<ObsDisplayWidget *>(data);
	obs_source_t *src = self->source;
	if (!src)
		return;
	uint32_t sw = obs_source_get_width(src);
	uint32_t sh = obs_source_get_height(src);
	if (!sw || !sh)
		return;
	float scale = std::min(float(cx) / float(sw), float(cy) / float(sh));
	int vpW = int(sw * scale);
	int vpH = int(sh * scale);
	int vpX = (int(cx) - vpW) / 2;
	int vpY = (int(cy) - vpH) / 2;

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, float(sw), 0.0f, float(sh), -100.0f, 100.0f);
	gs_set_viewport(vpX, vpY, vpW, vpH);
	obs_source_video_render(src);
	gs_projection_pop();
	gs_viewport_pop();
}

void ObsDisplayWidget::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	createDisplay();
}

void ObsDisplayWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	createDisplay();
	if (display) {
		QSize size = event->size() * devicePixelRatioF();
		obs_display_resize(display, (uint32_t)std::max(size.width(), 16),
				   (uint32_t)std::max(size.height(), 16));
	}
}

void ObsDisplayWidget::paintEvent(QPaintEvent *) {}

void ObsDisplayWidget::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton)
		emit clicked();
	QWidget::mousePressEvent(event);
}

/* ---------------- MediaPreview ---------------- */

MediaPreview::MediaPreview(QWidget *parent) : QWidget(parent)
{
	displayWidget = new ObsDisplayWidget(this);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(displayWidget);

	connect(displayWidget, &ObsDisplayWidget::clicked, this, [this]() { togglePlayPause(); });

	pollTimer.setInterval(100);
	connect(&pollTimer, &QTimer::timeout, this, [this]() { poll(); });
}

MediaPreview::~MediaPreview()
{
	shutdown();
}

void MediaPreview::shutdown()
{
	pollTimer.stop();
	closeFile();
	if (displayWidget)
		displayWidget->shutdown();
}

bool MediaPreview::openFile(const QString &path)
{
	closeFile();
	currentPath = path;

	obs_data_t *settings = obs_data_create();
	QByteArray utf8 = path.toUtf8();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", utf8.constData());
	obs_data_set_bool(settings, "looping", false);
	obs_data_set_bool(settings, "restart_on_activate", true);
	obs_data_set_bool(settings, "close_when_inactive", false);

	source = obs_source_create("ffmpeg_source", "obs-trim-preview", settings, nullptr);
	obs_data_release(settings);
	if (!source)
		return false;

	obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_MONITOR_ONLY);
	obs_source_inc_showing(source);
	obs_source_inc_active(source);
	displayWidget->setSource(source);

	cachedDuration = 0;
	cachedPosition = 0;
	lastPlaying = false;
	pollTimer.start();
	// Start paused on first frame; user presses play.
	obs_source_media_play_pause(source, true);
	return true;
}

void MediaPreview::closeFile()
{
	pollTimer.stop();
	if (displayWidget)
		displayWidget->setSource(nullptr);
	if (source) {
		// Detach from the audio monitor and stop decoding BEFORE release.
		// The monitor mix can otherwise keep the source (and its OS file
		// handle) alive after release, so Windows refuses delete/rename.
		obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_NONE);
		obs_source_media_stop(source);
		obs_source_dec_active(source);
		obs_source_dec_showing(source);
		obs_source_release(source);
		source = nullptr;
	}
	currentPath.clear();
	cachedDuration = 0;
	cachedPosition = 0;
	lastPlaying = false;
}

void MediaPreview::poll()
{
	if (!source)
		return;
	qint64 dur = obs_source_media_get_duration(source);
	if (dur > 0 && dur != cachedDuration) {
		cachedDuration = dur;
		emit durationChanged(dur);
	}
	qint64 pos = obs_source_media_get_time(source);
	if (pos != cachedPosition) {
		cachedPosition = pos;
		emit positionChanged(pos);
	}
	obs_media_state state = obs_source_media_get_state(source);
	bool playing = (state == OBS_MEDIA_STATE_PLAYING || state == OBS_MEDIA_STATE_BUFFERING ||
			state == OBS_MEDIA_STATE_OPENING);
	if (playing != lastPlaying) {
		lastPlaying = playing;
		emit playStateChanged(playing);
	}
	if (state == OBS_MEDIA_STATE_ENDED) {
		// Stay on last frame, report paused.
		if (lastPlaying) {
			lastPlaying = false;
			emit playStateChanged(false);
		}
	}
}

void MediaPreview::play()
{
	if (!source)
		return;
	obs_media_state state = obs_source_media_get_state(source);
	if (state == OBS_MEDIA_STATE_ENDED)
		obs_source_media_restart(source);
	else
		obs_source_media_play_pause(source, false);
}

void MediaPreview::pause()
{
	if (!source)
		return;
	obs_source_media_play_pause(source, true);
}

void MediaPreview::togglePlayPause()
{
	if (isPlaying())
		pause();
	else
		play();
}

bool MediaPreview::isPlaying() const
{
	if (!source)
		return false;
	obs_media_state state = obs_source_media_get_state(source);
	return (state == OBS_MEDIA_STATE_PLAYING || state == OBS_MEDIA_STATE_BUFFERING ||
		state == OBS_MEDIA_STATE_OPENING);
}

void MediaPreview::seekTo(qint64 ms)
{
	if (!source)
		return;
	if (ms < 0)
		ms = 0;
	if (cachedDuration > 0 && ms > cachedDuration)
		ms = cachedDuration;
	obs_source_media_set_time(source, ms);
	cachedPosition = ms;
	emit positionChanged(ms);
}
