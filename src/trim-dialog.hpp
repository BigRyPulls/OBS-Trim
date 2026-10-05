/*
OBS-Trim
Copyright (C) 2026 BigRyPulls
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QDialog>

#include "trim-worker.hpp"

class MediaPreview;
class TimelineWidget;

class QLabel;
class QPushButton;
class QLineEdit;
class QCheckBox;
class QSlider;

class TrimDialog : public QDialog {
	Q_OBJECT
public:
	explicit TrimDialog(QWidget *parent = nullptr);
	~TrimDialog() override;

	bool openFile(const QString &path);
	void shutdown();

	bool autoOpenEnabled() const;
	void setAutoOpenEnabled(bool on);

signals:
	void trimCompleted(const QString &outputPath);

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void closeEvent(QCloseEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private slots:
	void onPosition(qint64 ms);
	void onDuration(qint64 ms);
	void onPlayState(bool playing);
	void onInChanged(qint64 ms);
	void onOutChanged(qint64 ms);
	void onSeekRequested(qint64 ms);
	void onPlayToggled();
	void onRestart();
	void onBack5();
	void onFwd5();
	void onSetStart();
	void onSetEnd();
	void onStartEdited();
	void onEndEdited();
	void onSave();
	void onCancel();
	void onScrubChanged(int value);

private:
	void updateLabels();
	void updateSaveEnabled();
	void loadSettings();
	void saveSettings();

	MediaPreview *preview = nullptr;
	TimelineWidget *timeline = nullptr;
	QSlider *scrub = nullptr;

	QLabel *fileLabel = nullptr;
	QLabel *currentLabel = nullptr;
	QLabel *selLabel = nullptr;
	QLabel *warnLabel = nullptr;

	QPushButton *restartButton = nullptr;
	QPushButton *back5Button = nullptr;
	QPushButton *playButton = nullptr;
	QPushButton *fwd5Button = nullptr;
	QPushButton *setStartButton = nullptr;
	QPushButton *setEndButton = nullptr;
	QPushButton *saveButton = nullptr;
	QPushButton *cancelButton = nullptr;
	QLineEdit *startEdit = nullptr;
	QLineEdit *endEdit = nullptr;
	QLineEdit *nameEdit = nullptr;
	QCheckBox *replaceCheck = nullptr;
	QCheckBox *autoOpenCheck = nullptr;

	QString sourcePath;
	obs_trim::MediaInfo media;
	QString ffmpegPath;
	QString ffprobePath;
	qint64 currentMs = 0;
	bool scrubDragging = false;
	bool settingsLoaded = false;
	bool syncingEdits = false;
};
