/*
OBS-Trim
Copyright (C) 2026 BigRyPulls
SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "trim-dialog.hpp"

#include "preview-widget.hpp"
#include "timeline-widget.hpp"
#include "trim-worker.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QCloseEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSlider>
#include <QVBoxLayout>

using namespace obs_trim;

TrimDialog::TrimDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("OBS-Trim"));
	setModal(false);
	resize(760, 560);

	auto *main = new QVBoxLayout(this);

	fileLabel = new QLabel(this);
	fileLabel->setWordWrap(true);
	fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	main->addWidget(fileLabel);

	preview = new MediaPreview(this);
	preview->setMinimumHeight(300);
	main->addWidget(preview, 1);

	currentLabel = new QLabel(QStringLiteral("00:00.000"), this);
	currentLabel->setAlignment(Qt::AlignCenter);
	QFont mono = currentLabel->font();
	mono.setFamily(QStringLiteral("Consolas"));
	currentLabel->setFont(mono);
	main->addWidget(currentLabel);

	scrub = new QSlider(Qt::Horizontal, this);
	scrub->setRange(0, 0);
	main->addWidget(scrub);

	timeline = new TimelineWidget(this);
	main->addWidget(timeline);

	auto *labels = new QHBoxLayout();
	startLabel = new QLabel(this);
	endLabel = new QLabel(this);
	selLabel = new QLabel(this);
	startLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	endLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	selLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	labels->addWidget(startLabel);
	labels->addStretch(1);
	labels->addWidget(selLabel);
	labels->addStretch(1);
	labels->addWidget(endLabel);
	main->addLayout(labels);

	auto *markRow = new QHBoxLayout();
	playButton = new QPushButton(tr("Play"), this);
	setStartButton = new QPushButton(tr("Set Start [I]"), this);
	setEndButton = new QPushButton(tr("Set End [O]"), this);
	markRow->addWidget(playButton);
	markRow->addWidget(setStartButton);
	markRow->addWidget(setEndButton);
	main->addLayout(markRow);

	auto *nameRow = new QHBoxLayout();
	auto *nameLabel = new QLabel(tr("Filename:"), this);
	nameEdit = new QLineEdit(this);
	nameEdit->setPlaceholderText(tr("clip name without extension"));
	nameRow->addWidget(nameLabel);
	nameRow->addWidget(nameEdit, 1);
	main->addLayout(nameRow);

	warnLabel = new QLabel(this);
	warnLabel->setWordWrap(true);
	warnLabel->setStyleSheet(QStringLiteral("color: #c9a13b;"));
	warnLabel->setText(tr("Stream-copy trimming is lossless (no re-encode). Cuts snap to nearby keyframes, "
			      "so the start may shift slightly. Zero re-encoding matters more than frame-exact cuts."));
	main->addWidget(warnLabel);

	autoOpenCheck = new QCheckBox(tr("Automatically open after recording stops"), this);
	main->addWidget(autoOpenCheck);

	auto *btnRow = new QHBoxLayout();
	btnRow->addStretch(1);
	saveButton = new QPushButton(tr("Save Trimmed Recording"), this);
	saveButton->setDefault(true);
	cancelButton = new QPushButton(tr("Cancel"), this);
	btnRow->addWidget(saveButton);
	btnRow->addWidget(cancelButton);
	main->addLayout(btnRow);

	connect(preview, &MediaPreview::positionChanged, this, &TrimDialog::onPosition);
	connect(preview, &MediaPreview::durationChanged, this, &TrimDialog::onDuration);
	connect(preview, &MediaPreview::playStateChanged, this, &TrimDialog::onPlayState);
	connect(timeline, &TimelineWidget::inPointChanged, this, &TrimDialog::onInChanged);
	connect(timeline, &TimelineWidget::outPointChanged, this, &TrimDialog::onOutChanged);
	connect(timeline, &TimelineWidget::seekRequested, this, &TrimDialog::onSeekRequested);
	connect(playButton, &QPushButton::clicked, this, &TrimDialog::onPlayToggled);
	connect(setStartButton, &QPushButton::clicked, this, &TrimDialog::onSetStart);
	connect(setEndButton, &QPushButton::clicked, this, &TrimDialog::onSetEnd);
	connect(saveButton, &QPushButton::clicked, this, &TrimDialog::onSave);
	connect(cancelButton, &QPushButton::clicked, this, &TrimDialog::onCancel);
	connect(scrub, &QSlider::sliderPressed, this, [this]() { scrubDragging = true; });
	connect(scrub, &QSlider::sliderReleased, this, [this]() {
		scrubDragging = false;
		preview->seekTo(scrub->value());
	});
	connect(scrub, &QSlider::valueChanged, this, &TrimDialog::onScrubChanged);
	connect(nameEdit, &QLineEdit::textChanged, this, [this]() { updateSaveEnabled(); });
	connect(autoOpenCheck, &QCheckBox::toggled, this, [this]() { saveSettings(); });

	ffmpegPath = findFfmpeg();
	ffprobePath = findFfprobe();
	loadSettings();
	updateLabels();
}

TrimDialog::~TrimDialog()
{
	shutdown();
}

void TrimDialog::shutdown()
{
	if (preview)
		preview->shutdown();
}

void TrimDialog::loadSettings()
{
	QSettings s(QStringLiteral("BigRyPulls"), QStringLiteral("OBS-Trim"));
	bool autoOpen = s.value(QStringLiteral("autoOpen"), true).toBool();
	autoOpenCheck->setChecked(autoOpen);
	QByteArray geom = s.value(QStringLiteral("geometry")).toByteArray();
	if (!geom.isEmpty())
		restoreGeometry(geom);
	settingsLoaded = true;
}

void TrimDialog::saveSettings()
{
	if (!settingsLoaded)
		return;
	QSettings s(QStringLiteral("BigRyPulls"), QStringLiteral("OBS-Trim"));
	s.setValue(QStringLiteral("autoOpen"), autoOpenCheck->isChecked());
	s.setValue(QStringLiteral("geometry"), saveGeometry());
}

bool TrimDialog::autoOpenEnabled() const
{
	return autoOpenCheck ? autoOpenCheck->isChecked() : true;
}

void TrimDialog::setAutoOpenEnabled(bool on)
{
	if (autoOpenCheck)
		autoOpenCheck->setChecked(on);
}

void TrimDialog::showEvent(QShowEvent *event)
{
	QDialog::showEvent(event);
}

void TrimDialog::hideEvent(QHideEvent *event)
{
	saveSettings();
	// Pause when hidden to avoid background playback.
	if (preview)
		preview->pause();
	QDialog::hideEvent(event);
}

void TrimDialog::closeEvent(QCloseEvent *event)
{
	// Cancel behaviour: leave original untouched.
	saveSettings();
	if (preview)
		preview->pause();
	QDialog::closeEvent(event);
}

bool TrimDialog::openFile(const QString &path)
{
	QFileInfo fi(path);
	if (!fi.exists()) {
		QMessageBox::warning(this, tr("OBS-Trim"), tr("Recording not found:\n%1").arg(path));
		return false;
	}
	// Refresh ffmpeg paths (user may have installed after load)
	ffmpegPath = findFfmpeg();
	ffprobePath = findFfprobe();

	QString err;
	MediaInfo info = probeMedia(ffprobePath, path, &err);
	if (!info.valid) {
		QString msg = tr("Could not read recording:\n%1\n\n%2").arg(path, err);
		if (ffprobePath.isEmpty())
			msg += tr("\n\nInstall FFmpeg (ffmpeg + ffprobe) and ensure they are in PATH. "
				  "See README for details.");
		QMessageBox::warning(this, tr("OBS-Trim"), msg);
		return false;
	}
	sourcePath = fi.absoluteFilePath();
	media = info;
	currentMs = 0;

	fileLabel->setText(tr("%1  •  %2  •  %3  •  %4 audio track(s)")
				   .arg(fi.fileName(), formatMs(info.durationMs),
					info.videoCodec.isEmpty() ? tr("video")
								  : info.videoCodec,
					QString::number(info.audioStreamCount)));

	// Filename defaults to recording name without extension
	nameEdit->setText(fi.completeBaseName());

	timeline->setDuration(info.durationMs);
	timeline->setInPoint(0);
	timeline->setOutPoint(info.durationMs);
	timeline->setPlayhead(0);

	scrub->setRange(0, int(qMin<qint64>(info.durationMs, 2000000000)));
	scrub->setValue(0);

	if (!preview->openFile(sourcePath)) {
		QMessageBox::warning(this, tr("OBS-Trim"),
				     tr("Could not open preview for:\n%1").arg(path));
		return false;
	}
	if (ffmpegPath.isEmpty() || ffprobePath.isEmpty()) {
		warnLabel->setText(
			tr("ffmpeg/ffprobe not found in PATH. Preview works, but Save requires FFmpeg. "
			   "Install FFmpeg and restart OBS. (Stream-copy: no re-encode; cuts snap to keyframes.)"));
	} else {
		warnLabel->setText(
			tr("Stream-copy trimming is lossless (no re-encode). Cuts snap to nearby keyframes, "
			   "so the start may shift slightly. Zero re-encoding matters more than frame-exact cuts."));
	}
	updateLabels();
	updateSaveEnabled();
	show();
	raise();
	activateWindow();
	return true;
}

void TrimDialog::onPosition(qint64 ms)
{
	currentMs = ms;
	timeline->setPlayhead(ms);
	if (!scrubDragging)
		scrub->setValue(int(qMin<qint64>(ms, 2000000000)));
	currentLabel->setText(formatMs(ms));
}

void TrimDialog::onDuration(qint64 ms)
{
	// OBS media duration can refine ffprobe duration; keep the larger for UI.
	if (ms > media.durationMs) {
		media.durationMs = ms;
		timeline->setDuration(ms);
		scrub->setRange(0, int(qMin<qint64>(ms, 2000000000)));
		updateLabels();
	}
}

void TrimDialog::onPlayState(bool playing)
{
	playButton->setText(playing ? tr("Pause") : tr("Play"));
}

void TrimDialog::onInChanged(qint64)
{
	updateLabels();
}

void TrimDialog::onOutChanged(qint64)
{
	updateLabels();
}

void TrimDialog::onSeekRequested(qint64 ms)
{
	preview->seekTo(ms);
}

void TrimDialog::onScrubChanged(int value)
{
	if (scrubDragging) {
		preview->seekTo(value);
	}
}

void TrimDialog::onPlayToggled()
{
	preview->togglePlayPause();
}

void TrimDialog::onSetStart()
{
	timeline->setInPoint(currentMs);
}

void TrimDialog::onSetEnd()
{
	timeline->setOutPoint(currentMs);
	updateLabels();
}

void TrimDialog::updateLabels()
{
	qint64 in = timeline ? timeline->inPoint() : 0;
	qint64 out = timeline ? timeline->outPoint() : media.durationMs;
	if (out <= 0)
		out = media.durationMs;
	startLabel->setText(tr("Start: %1").arg(formatMs(in)));
	endLabel->setText(tr("End: %1").arg(formatMs(out)));
	selLabel->setText(tr("Selected: %1").arg(formatMs(qMax<qint64>(0, out - in))));
}

void TrimDialog::updateSaveEnabled()
{
	bool hasSource = !sourcePath.isEmpty();
	bool hasName = !nameEdit->text().trimmed().isEmpty();
	saveButton->setEnabled(hasSource && hasName);
}

void TrimDialog::onSave()
{
	if (sourcePath.isEmpty())
		return;
	QString name = nameEdit->text().trimmed();
	if (name.isEmpty()) {
		QMessageBox::information(this, tr("OBS-Trim"), tr("Please enter a filename."));
		return;
	}
	qint64 in = timeline->inPoint();
	qint64 out = timeline->outPoint();

	// Root cause of "original could not be deleted": the preview owns an
	// OBS ffmpeg_source on the source recording, so Windows keeps the file
	// open. Release the whole preview/source BEFORE any filesystem
	// replacement, not just pause(). Remember UI state to restore on failure.
	const qint64 savedPos = currentMs;
	const qint64 savedIn = in;
	const qint64 savedOut = out;
	preview->closeFile();

	saveButton->setEnabled(false);
	setCursor(Qt::WaitCursor);

	TrimOptions opts;
	opts.sourcePath = sourcePath;
	opts.startMs = in;
	opts.endMs = out;
	opts.destFileName = name;

	// Re-resolve in case PATH changed
	ffmpegPath = findFfmpeg();
	ffprobePath = findFfprobe();

	TrimResult r = trimLossless(ffmpegPath, ffprobePath, opts);

	unsetCursor();
	saveButton->setEnabled(true);

	if (!r.ok) {
		// Trimming failed and the original still exists: reopen it so the
		// user can adjust and retry, restoring playhead/In/Out/filename.
		if (!sourcePath.isEmpty() && QFile::exists(sourcePath)) {
			if (preview->openFile(sourcePath)) {
				currentMs = savedPos;
				timeline->setInPoint(savedIn);
				timeline->setOutPoint(savedOut);
				nameEdit->setText(name);
				preview->seekTo(savedPos);
				updateLabels();
				updateSaveEnabled();
			}
		}
		QMessageBox::warning(this, tr("OBS-Trim"), r.error);
		return;
	}
	if (!r.error.isEmpty()) {
		// Partial warning (e.g. original could not be deleted)
		QMessageBox::warning(this, tr("OBS-Trim"), r.error);
	}
	QMessageBox::information(this, tr("OBS-Trim"), tr("Saved:\n%1").arg(r.outputPath));
	emit trimCompleted(r.outputPath);
	saveSettings();
	close();
}

void TrimDialog::onCancel()
{
	// Leave original exactly where it is.
	close();
}

void TrimDialog::keyPressEvent(QKeyEvent *event)
{
	if (!preview) {
		QDialog::keyPressEvent(event);
		return;
	}
	switch (event->key()) {
	case Qt::Key_Space:
		preview->togglePlayPause();
		event->accept();
		return;
	case Qt::Key_I:
		onSetStart();
		event->accept();
		return;
	case Qt::Key_O:
		timeline->setOutPoint(currentMs);
		updateLabels();
		event->accept();
		return;
	case Qt::Key_Left: {
		qint64 step = (event->modifiers() & Qt::ShiftModifier) ? 1000 : 100;
		preview->seekTo(currentMs - step);
		event->accept();
		return;
	}
	case Qt::Key_Right: {
		qint64 step = (event->modifiers() & Qt::ShiftModifier) ? 1000 : 100;
		preview->seekTo(currentMs + step);
		event->accept();
		return;
	}
	case Qt::Key_Home:
		preview->seekTo(timeline->inPoint());
		event->accept();
		return;
	case Qt::Key_End:
		preview->seekTo(timeline->outPoint());
		event->accept();
		return;
	default:
		break;
	}
	QDialog::keyPressEvent(event);
}
