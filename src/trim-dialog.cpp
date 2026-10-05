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

namespace {
// Parse "MM:SS.mmm", "HH:MM:SS.mmm", "SS.mmm" or plain seconds.
// Returns milliseconds, or -1 on invalid input. Never throws.
qint64 parseTimeMs(const QString &text)
{
	QString t = text.trimmed();
	if (t.isEmpty())
		return -1;
	t.replace(',', '.');
	QStringList parts = t.split(':');
	if (parts.size() < 1 || parts.size() > 3)
		return -1;
	for (QString &p : parts)
		p = p.trimmed();
	// Last part carries optional .mmm fraction.
	QString secPart = parts.back();
	parts.pop_back();
	long long millis = 0;
	QString intSec = secPart;
	QString frac;
	int dot = secPart.indexOf('.');
	if (dot >= 0) {
		intSec = secPart.left(dot);
		frac = secPart.mid(dot + 1);
	}
	if (intSec.isEmpty())
		intSec = QStringLiteral("0");
	bool ok = false;
	long long secs = intSec.toLongLong(&ok);
	if (!ok || secs < 0)
		return -1;
	if (!frac.isEmpty()) {
		if (frac.size() > 3)
			frac = frac.left(3);
		while (frac.size() < 3)
			frac += '0';
		long long ms = frac.toLongLong(&ok);
		if (!ok || ms < 0)
			return -1;
		millis += ms;
	}
	if (parts.size() == 0) {
		millis += secs * 1000;
	} else if (parts.size() == 1) {
		long long mins = parts[0].toLongLong(&ok);
		if (!ok || mins < 0 || secs > 59)
			return -1;
		millis += (mins * 60 + secs) * 1000;
	} else {
		long long hours = parts[0].toLongLong(&ok);
		if (!ok || hours < 0)
			return -1;
		long long mins = parts[1].toLongLong(&ok);
		if (!ok || mins < 0 || mins > 59 || secs > 59)
			return -1;
		millis += ((hours * 60 + mins) * 60 + secs) * 1000;
	}
	return millis;
}
} // namespace

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
	auto *startCaption = new QLabel(tr("Start:"), this);
	startEdit = new QLineEdit(this);
	startEdit->setPlaceholderText(tr("00:00.000"));
	startEdit->setMaximumWidth(110);
	startEdit->setToolTip(tr("Trim start (MM:SS.mmm). Enter to apply."));
	auto *endCaption = new QLabel(tr("End:"), this);
	endEdit = new QLineEdit(this);
	endEdit->setPlaceholderText(tr("00:00.000"));
	endEdit->setMaximumWidth(110);
	endEdit->setToolTip(tr("Trim end (MM:SS.mmm). Enter to apply."));
	selLabel = new QLabel(this);
	selLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	labels->addWidget(startCaption);
	labels->addWidget(startEdit);
	labels->addSpacing(12);
	labels->addWidget(endCaption);
	labels->addWidget(endEdit);
	labels->addStretch(1);
	labels->addWidget(selLabel);
	main->addLayout(labels);

	auto *markRow = new QHBoxLayout();
	restartButton = new QPushButton(tr("Restart"), this);
	restartButton->setToolTip(tr("Back to beginning (R)"));
	back5Button = new QPushButton(tr("-5s"), this);
	back5Button->setToolTip(tr("Back 5 seconds (J)"));
	playButton = new QPushButton(tr("Play"), this);
	fwd5Button = new QPushButton(tr("+5s"), this);
	fwd5Button->setToolTip(tr("Forward 5 seconds (L)"));
	setStartButton = new QPushButton(tr("Set Start [I]"), this);
	setEndButton = new QPushButton(tr("Set End [O]"), this);
	markRow->addWidget(restartButton);
	markRow->addWidget(back5Button);
	markRow->addWidget(playButton);
	markRow->addWidget(fwd5Button);
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

	replaceCheck = new QCheckBox(tr("Replace original (delete source after verified save)"), this);
	replaceCheck->setToolTip(tr("ON: original is removed only after the trimmed file is verified. "
				    "OFF: original is kept; the result is saved under the new name."));
	main->addWidget(replaceCheck);

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
	connect(restartButton, &QPushButton::clicked, this, &TrimDialog::onRestart);
	connect(back5Button, &QPushButton::clicked, this, &TrimDialog::onBack5);
	connect(fwd5Button, &QPushButton::clicked, this, &TrimDialog::onFwd5);
	connect(setStartButton, &QPushButton::clicked, this, &TrimDialog::onSetStart);
	connect(setEndButton, &QPushButton::clicked, this, &TrimDialog::onSetEnd);
	connect(startEdit, &QLineEdit::editingFinished, this, &TrimDialog::onStartEdited);
	connect(endEdit, &QLineEdit::editingFinished, this, &TrimDialog::onEndEdited);
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
	connect(replaceCheck, &QCheckBox::toggled, this, [this]() { saveSettings(); });

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
	bool replace = s.value(QStringLiteral("replaceOriginal"), true).toBool();
	replaceCheck->setChecked(replace);
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
	s.setValue(QStringLiteral("replaceOriginal"), replaceCheck->isChecked());
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
					info.videoCodec.isEmpty() ? tr("video") : info.videoCodec,
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
		QMessageBox::warning(this, tr("OBS-Trim"), tr("Could not open preview for:\n%1").arg(path));
		return false;
	}
	// Autoplay: the just-finished recording should start playing immediately.
	preview->play();
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

void TrimDialog::onRestart()
{
	preview->restart();
}

void TrimDialog::onBack5()
{
	preview->seekRelative(-5000);
}

void TrimDialog::onFwd5()
{
	preview->seekRelative(5000);
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

void TrimDialog::onStartEdited()
{
	if (syncingEdits || !timeline || !startEdit)
		return;
	qint64 v = parseTimeMs(startEdit->text());
	if (v < 0) {
		// Invalid: revert, do not corrupt state.
		syncingEdits = true;
		startEdit->setText(formatMs(timeline->inPoint()));
		syncingEdits = false;
		return;
	}
	timeline->setInPoint(v);
	updateLabels();
}

void TrimDialog::onEndEdited()
{
	if (syncingEdits || !timeline || !endEdit)
		return;
	qint64 v = parseTimeMs(endEdit->text());
	if (v < 0) {
		syncingEdits = true;
		endEdit->setText(formatMs(timeline->outPoint()));
		syncingEdits = false;
		return;
	}
	timeline->setOutPoint(v);
	updateLabels();
}

void TrimDialog::updateLabels()
{
	qint64 in = timeline ? timeline->inPoint() : 0;
	qint64 out = timeline ? timeline->outPoint() : media.durationMs;
	if (out <= 0)
		out = media.durationMs;
	if (!syncingEdits) {
		syncingEdits = true;
		// Do not clobber the field the user is actively typing in.
		if (startEdit && !startEdit->hasFocus())
			startEdit->setText(formatMs(in));
		if (endEdit && !endEdit->hasFocus())
			endEdit->setText(formatMs(out));
		syncingEdits = false;
	}
	if (selLabel)
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
	opts.replaceOriginal = replaceCheck ? replaceCheck->isChecked() : true;

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
	// While typing in a text field, keys belong to the field (Start/End
	// time boxes, filename). Otherwise typing "r"/space would restart or
	// toggle playback instead of entering text.
	if (qobject_cast<QLineEdit *>(focusWidget())) {
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
	case Qt::Key_R:
		preview->restart();
		event->accept();
		return;
	case Qt::Key_J:
		preview->seekRelative(-5000);
		event->accept();
		return;
	case Qt::Key_L:
		preview->seekRelative(5000);
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
