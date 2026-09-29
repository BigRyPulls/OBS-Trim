/*
OBS-Trim
Copyright (C) 2026 BigRyPulls

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "trim-worker.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QRegularExpression>

namespace obs_trim {

namespace {
// Short bounded retry for Windows file replacement: the media backend can
// release its OS file handle slightly asynchronously after the preview
// source is destroyed, so the first rename/delete may hit sharing-violation.
// Total budget ~1s; genuine failures still surface as errors.
bool renameFileRetry(const QString &from, const QString &to)
{
	for (int i = 0; i < 10; ++i) {
		if (QFile::rename(from, to))
			return true;
		QThread::msleep(100);
	}
	return QFile::rename(from, to);
}

bool removeFileRetry(const QString &path)
{
	for (int i = 0; i < 10; ++i) {
		if (QFile::remove(path))
			return true;
		if (!QFile::exists(path))
			return true;
		QThread::msleep(100);
	}
	if (!QFile::exists(path))
		return true;
	return QFile::remove(path);
}
} // namespace

QString formatMs(qint64 ms)
{
	if (ms < 0)
		ms = 0;
	qint64 totalSecs = ms / 1000;
	int milli = int(ms % 1000);
	qint64 hours = totalSecs / 3600;
	qint64 mins = (totalSecs % 3600) / 60;
	qint64 secs = totalSecs % 60;
	if (hours > 0)
		return QString::asprintf("%02lld:%02lld:%02lld.%03d", (long long)hours, (long long)mins,
					 (long long)secs, milli);
	return QString::asprintf("%02lld:%02lld.%03d", (long long)mins, (long long)secs, milli);
}

QString sanitizeFilename(const QString &name)
{
	QString out = name.trimmed();
	// Windows forbidden: < > : " / \ | ? * + control chars 0-31
	static QRegularExpression bad(QStringLiteral("[<>:\"/\\\\|\\?\\*\\x00-\\x1F]"));
	out.replace(bad, QStringLiteral("_"));
	// Windows does not like trailing dots/spaces
	while (out.endsWith('.') || out.endsWith(' '))
		out.chop(1);
	if (out.isEmpty())
		out = QStringLiteral("trimmed");
	// Reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9).
	// Windows reserves these even with an extension (CON.txt, NUL.foo),
	// so check the stem before the first dot, not just the whole string.
	static QStringList reserved = {QStringLiteral("con"),  QStringLiteral("prn"), QStringLiteral("aux"),
				       QStringLiteral("nul"),  QStringLiteral("com1"), QStringLiteral("com2"),
				       QStringLiteral("com3"), QStringLiteral("com4"), QStringLiteral("com5"),
				       QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
				       QStringLiteral("com9"), QStringLiteral("lpt1"), QStringLiteral("lpt2"),
				       QStringLiteral("lpt3"), QStringLiteral("lpt4"), QStringLiteral("lpt5"),
				       QStringLiteral("lpt6"), QStringLiteral("lpt7"), QStringLiteral("lpt8"),
				       QStringLiteral("lpt9")};
	QString stem = out.section(QChar('.'), 0, 0);
	if (reserved.contains(out.toLower()) || reserved.contains(stem.toLower())) {
		// Mangle the stem itself: "CON.txt" -> "CON_.txt". Appending to the
		// very end ("CON.txt_") would NOT work because Windows compares the
		// part before the first dot.
		int dot = out.indexOf(QChar('.'));
		if (dot > 0)
			out = stem + QStringLiteral("_.") + out.mid(dot + 1);
		else
			out = out + QStringLiteral("_");
	}
	// Limit length to be safe (leave room for extension + tmp suffix)
	if (out.size() > 180)
		out = out.left(180).trimmed();
	return out;
}

QString findExecutable(const QString &name)
{
	// 1. PATH
	QString fromPath = QStandardPaths::findExecutable(name);
	if (!fromPath.isEmpty())
		return fromPath;
	// 2. Common install locations (Windows)
	QStringList candidates;
#ifdef Q_OS_WIN
	QString exe = name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)
			      ? name
			      : name + QStringLiteral(".exe");
	candidates << QStringLiteral("C:/Program Files/ffmpeg/bin/") + exe
		   << QStringLiteral("C:/Program Files (x86)/ffmpeg/bin/") + exe
		   << QStringLiteral("D:/Program Files/ffmpeg/bin/") + exe;
	// Scoop / Chocolatey / winget locations
	QString userProfile = QString::fromLocal8Bit(qgetenv("USERPROFILE"));
	if (!userProfile.isEmpty()) {
		candidates << userProfile + QStringLiteral("/scoop/shims/") + exe;
	}
	candidates << QStringLiteral("C:/ProgramData/chocolatey/bin/") + exe;
	for (const QString &c : candidates) {
		if (QFile::exists(c))
			return QDir::toNativeSeparators(c);
	}
#else
	(void)candidates;
#endif
	return QString();
}

QString findFfmpeg()
{
	QString custom = QString::fromLocal8Bit(qgetenv("OBS_TRIM_FFMPEG"));
	if (!custom.isEmpty() && QFile::exists(custom))
		return custom;
	return findExecutable(QStringLiteral("ffmpeg"));
}

QString findFfprobe()
{
	QString custom = QString::fromLocal8Bit(qgetenv("OBS_TRIM_FFPROBE"));
	if (!custom.isEmpty() && QFile::exists(custom))
		return custom;
	return findExecutable(QStringLiteral("ffprobe"));
}

static double qjsonToDouble(const QJsonValue &v, double fallback = 0.0)
{
	if (v.isDouble())
		return v.toDouble();
	if (v.isString()) {
		bool ok = false;
		// ffprobe may emit "60/1" style avg_frame_rate as string
		QString s = v.toString().trimmed();
		if (s.contains('/')) {
			QStringList parts = s.split('/');
			if (parts.size() == 2) {
				double n = parts[0].toDouble(&ok);
				if (!ok)
					return fallback;
				bool ok2 = false;
				double d = parts[1].toDouble(&ok2);
				if (!ok2 || d == 0.0)
					return fallback;
				return n / d;
			}
		}
		double d = s.toDouble(&ok);
		if (ok)
			return d;
	}
	return fallback;
}

MediaInfo probeMedia(const QString &ffprobePath, const QString &filePath, QString *errorOut)
{
	MediaInfo info;
	if (ffprobePath.isEmpty()) {
		if (errorOut)
			*errorOut = QStringLiteral("ffprobe not found. Install FFmpeg and ensure ffprobe is in PATH.");
		return info;
	}
	if (!QFile::exists(filePath)) {
		if (errorOut)
			*errorOut = QStringLiteral("Source file does not exist.");
		return info;
	}
	QProcess proc;
	QStringList args = {QStringLiteral("-v"), QStringLiteral("quiet"), QStringLiteral("-print_format"),
			    QStringLiteral("json"), QStringLiteral("-show_format"), QStringLiteral("-show_streams"),
			    filePath};
	proc.start(ffprobePath, args);
	if (!proc.waitForStarted(5000)) {
		if (errorOut)
			*errorOut = QStringLiteral("Failed to start ffprobe.");
		return info;
	}
	if (!proc.waitForFinished(15000)) {
		proc.kill();
		if (errorOut)
			*errorOut = QStringLiteral("ffprobe timed out.");
		return info;
	}
	if (proc.exitCode() != 0) {
		if (errorOut)
			*errorOut = QString::fromUtf8(proc.readAllStandardError()).trimmed();
		if (errorOut->isEmpty())
			*errorOut = QStringLiteral("ffprobe failed.");
		return info;
	}
	QJsonParseError perr{};
	QJsonDocument doc = QJsonDocument::fromJson(proc.readAllStandardOutput(), &perr);
	if (perr.error != QJsonParseError::NoError) {
		if (errorOut)
			*errorOut = QStringLiteral("Failed to parse ffprobe output.");
		return info;
	}
	QJsonObject root = doc.object();
	QJsonArray streams = root.value(QStringLiteral("streams")).toArray();
	QJsonObject format = root.value(QStringLiteral("format")).toObject();
	double durSec = qjsonToDouble(format.value(QStringLiteral("duration")), 0.0);
	if (durSec > 0.0)
		info.durationMs = qint64(durSec * 1000.0);

	info.totalStreams = streams.size();
	for (const QJsonValue &sv : streams) {
		QJsonObject st = sv.toObject();
		QString type = st.value(QStringLiteral("codec_type")).toString();
		QString codec = st.value(QStringLiteral("codec_name")).toString();
		info.streamTypes << type;
		info.streamCodecs << codec;
		if (type == QStringLiteral("video") && info.width == 0) {
			info.width = st.value(QStringLiteral("width")).toInt();
			info.height = st.value(QStringLiteral("height")).toInt();
			info.videoCodec = st.value(QStringLiteral("codec_name")).toString();
			double fps = qjsonToDouble(st.value(QStringLiteral("avg_frame_rate")), 0.0);
			if (fps <= 0.0)
				fps = qjsonToDouble(st.value(QStringLiteral("r_frame_rate")), 0.0);
			info.fps = fps;
			QString durStr = st.value(QStringLiteral("duration")).toString();
			if (info.durationMs == 0 && !durStr.isEmpty()) {
				bool ok = false;
				double d = durStr.toDouble(&ok);
				if (ok && d > 0.0)
					info.durationMs = qint64(d * 1000.0);
			}
		} else if (type == QStringLiteral("audio")) {
			info.audioStreamCount++;
			QString codec = st.value(QStringLiteral("codec_name")).toString();
			if (!codec.isEmpty())
				info.audioCodecs << codec;
		}
	}
	info.valid = (info.width > 0 && info.durationMs > 0);
	if (!info.valid && errorOut)
		*errorOut = QStringLiteral("No video stream found or duration unknown.");
	return info;
}

static QString secondsArg(qint64 ms)
{
	// Use seconds with millisecond precision, e.g. 12.345
	return QString::number(double(ms) / 1000.0, 'f', 3);
}

TrimResult trimLossless(const QString &ffmpegPath, const QString &ffprobePath, const TrimOptions &opts)
{
	TrimResult res;
	QFileInfo srcInfo(opts.sourcePath);
	if (!srcInfo.exists() || !srcInfo.isFile()) {
		res.error = QStringLiteral("Source recording no longer exists.");
		return res;
	}
	QDir dir = srcInfo.absoluteDir();
	QString ext = srcInfo.suffix(); // keep original container
	QString destName = opts.destFileName.trimmed();
	if (destName.isEmpty()) {
		res.error = QStringLiteral("Please enter a filename.");
		return res;
	}
	// If user included extension, strip and re-add canonical ext to avoid .mkv.mkv
	QFileInfo destInfoCheck(destName);
	if (!destInfoCheck.suffix().isEmpty()) {
		// Keep user extension only if it matches source (case-insensitive); else treat as part of name
		if (destInfoCheck.suffix().compare(ext, Qt::CaseInsensitive) == 0) {
			destName = destInfoCheck.completeBaseName();
		}
	}
	destName = sanitizeFilename(destName);
	if (destName.isEmpty()) {
		res.error = QStringLiteral("Invalid filename.");
		return res;
	}
	QString finalName = destName + (ext.isEmpty() ? QString() : QStringLiteral(".") + ext);
	QString finalPath = dir.absoluteFilePath(finalName);

	qint64 startMs = qMax<qint64>(0, opts.startMs);
	qint64 endMs = opts.endMs;
	if (endMs <= 0)
		endMs = 0; // resolved below via probe

	QString probeErr;
	MediaInfo srcMedia = probeMedia(ffprobePath, opts.sourcePath, &probeErr);
	if (!srcMedia.valid) {
		res.error = QStringLiteral("Could not read source: %1").arg(probeErr);
		return res;
	}
	if (endMs <= 0 || endMs > srcMedia.durationMs)
		endMs = srcMedia.durationMs;
	if (startMs >= endMs) {
		res.error = QStringLiteral("Start must be before End.");
		return res;
	}
	const qint64 kToleranceMs = 150;
	bool isFullRange = (startMs <= kToleranceMs) && ((srcMedia.durationMs - endMs) <= kToleranceMs);

	// Destination conflict: allow if destination IS the source (same file, rename-only no-op handled below)
	bool destIsSource = (QFileInfo(finalPath).absoluteFilePath().compare(srcInfo.absoluteFilePath(),
									      Qt::CaseInsensitive) == 0);
	if (!destIsSource && QFile::exists(finalPath)) {
		res.error = QStringLiteral("A file named \"%1\" already exists. Choose a different name.").arg(finalName);
		return res;
	}

	// Rename-only fast path (no trim): just rename file, no ffmpeg.
	if (isFullRange) {
		if (destIsSource) {
			res.ok = true;
			res.outputPath = srcInfo.absoluteFilePath();
			return res;
		}
		if (!renameFileRetry(srcInfo.absoluteFilePath(), finalPath)) {
			res.error = QStringLiteral("Could not rename file.");
			return res;
		}
		res.ok = true;
		res.outputPath = finalPath;
		return res;
	}

	if (ffmpegPath.isEmpty()) {
		res.error = QStringLiteral("ffmpeg not found. Install FFmpeg and ensure ffmpeg is in PATH.");
		return res;
	}

	// Temp file in same directory/filesystem
	QString tmpName = QStringLiteral(".%1.obs-trim.tmp.%2").arg(srcInfo.fileName(), ext);
	QString tmpPath = dir.absoluteFilePath(tmpName);
	if (QFile::exists(tmpPath))
		QFile::remove(tmpPath);

	qint64 durMs = endMs - startMs;
	// Stream-copy trim. Output seeking (-ss/-to after -i) is slower than
	// input seeking but considerably more accurate for -c copy (input
	// seeking can prepend a whole keyframe interval, e.g. 5s requested
	// becoming 6.2s). We deliberately do NOT re-encode: zero re-encoding
	// is more important than frame-exact cuts; the start may still snap
	// to a nearby keyframe.
	QStringList args;
	args << QStringLiteral("-hide_banner") << QStringLiteral("-y") << QStringLiteral("-i") << opts.sourcePath
	     << QStringLiteral("-ss") << secondsArg(startMs);
	// If trimming to EOF, omit -to to avoid rounding issues.
	const qint64 kEofToleranceMs = 200;
	if ((srcMedia.durationMs - endMs) > kEofToleranceMs)
		args << QStringLiteral("-to") << secondsArg(endMs);
	args << QStringLiteral("-map") << QStringLiteral("0") << QStringLiteral("-map_metadata")
	     << QStringLiteral("0") << QStringLiteral("-map_chapters") << QStringLiteral("0")
	     << QStringLiteral("-c") << QStringLiteral("copy") << QStringLiteral("-avoid_negative_ts")
	     << QStringLiteral("make_zero") << tmpPath;

	QProcess ffmpeg;
	ffmpeg.start(ffmpegPath, args);
	if (!ffmpeg.waitForStarted(5000)) {
		res.error = QStringLiteral("Failed to start ffmpeg.");
		return res;
	}
	// Allow generous time: 60s per minute of output, min 60s
	int timeoutMs = int(qMax<qint64>(60000, (durMs / 60000 + 1) * 60000));
	if (!ffmpeg.waitForFinished(timeoutMs)) {
		ffmpeg.kill();
		QFile::remove(tmpPath);
		res.error = QStringLiteral("ffmpeg timed out.");
		return res;
	}
	QByteArray stderrOut = ffmpeg.readAllStandardError();
	if (ffmpeg.exitCode() != 0) {
		QFile::remove(tmpPath);
		QString err = QString::fromUtf8(stderrOut).trimmed();
		if (err.size() > 1200)
			err = err.right(1200);
		res.error = QStringLiteral("ffmpeg failed: %1").arg(err.isEmpty() ? QStringLiteral("unknown error")
										   : err);
		return res;
	}

	// Verify output
	QFileInfo tmpInfo(tmpPath);
	if (!tmpInfo.exists() || tmpInfo.size() == 0) {
		QFile::remove(tmpPath);
		res.error = QStringLiteral("ffmpeg produced no output.");
		return res;
	}
	QString verifyErr;
	MediaInfo outMedia = probeMedia(ffprobePath, tmpPath, &verifyErr);
	if (!outMedia.valid) {
		QFile::remove(tmpPath);
		res.error = QStringLiteral("Output verification failed: %1").arg(verifyErr);
		return res;
	}
	if (outMedia.videoCodec.compare(srcMedia.videoCodec, Qt::CaseInsensitive) != 0) {
		// Stream copy must not change codecs; treat mismatch as failure (should not happen with -c copy)
		QFile::remove(tmpPath);
		res.error = QStringLiteral("Output video codec changed (expected stream copy). Aborted.");
		return res;
	}
	// Strict stream-copy check: same stream count, same types in order,
	// same codecs in order (covers every audio track, subtitles, attachments).
	if (outMedia.totalStreams != srcMedia.totalStreams) {
		QFile::remove(tmpPath);
		res.error = QStringLiteral("Stream count changed (%1 -> %2). Aborted to protect recording.")
				    .arg(srcMedia.totalStreams)
				    .arg(outMedia.totalStreams);
		return res;
	}
	if (outMedia.streamTypes != srcMedia.streamTypes) {
		QFile::remove(tmpPath);
		res.error = QStringLiteral("Stream types changed. Aborted to protect recording.");
		return res;
	}
	bool codecsMatch = (outMedia.streamCodecs.size() == srcMedia.streamCodecs.size());
	if (codecsMatch) {
		for (int i = 0; i < outMedia.streamCodecs.size(); ++i) {
			if (outMedia.streamCodecs[i].compare(srcMedia.streamCodecs[i], Qt::CaseInsensitive) !=
			    0) {
				codecsMatch = false;
				break;
			}
		}
	}
	if (!codecsMatch) {
		QFile::remove(tmpPath);
		res.error = QStringLiteral("Stream codecs changed (expected stream copy). Aborted to protect recording.");
		return res;
	}
	if (outMedia.audioStreamCount != srcMedia.audioStreamCount) {
		QFile::remove(tmpPath);
		res.error = QStringLiteral("Audio track count changed (%1 -> %2). Aborted to protect recording.")
				    .arg(srcMedia.audioStreamCount)
				    .arg(outMedia.audioStreamCount);
		return res;
	}
	// Duration sensible: within 3s or 10% of requested (keyframe rounding can shift start)
	qint64 diff = qAbs(outMedia.durationMs - durMs);
	if (diff > 3000 && diff > durMs / 10) {
		QFile::remove(tmpPath);
		res.error = QStringLiteral("Output duration looks wrong (expected ~%1, got %2). Original preserved.")
				    .arg(formatMs(durMs), formatMs(outMedia.durationMs));
		return res;
	}

	// Rename temp -> final, then remove original.
	// Short bounded retries tolerate the media backend releasing its OS
	// file handle slightly asynchronously after the preview is closed.
	if (!destIsSource) {
		if (!renameFileRetry(tmpPath, finalPath)) {
			removeFileRetry(tmpPath);
			res.error = QStringLiteral("Could not move trimmed file into place.");
			return res;
		}
		if (!removeFileRetry(srcInfo.absoluteFilePath())) {
			// New file is safe; warn but succeed. Try to keep both.
			res.ok = true;
			res.outputPath = finalPath;
			res.error = QStringLiteral("Trimmed file created, but the original could not be deleted.");
			return res;
		}
	} else {
		// Same name but trimmed range: replace via intermediate
		QString backup = dir.absoluteFilePath(QStringLiteral(".%1.obs-trim.bak.%2").arg(srcInfo.fileName(), ext));
		if (QFile::exists(backup))
			removeFileRetry(backup);
		if (!renameFileRetry(srcInfo.absoluteFilePath(), backup)) {
			removeFileRetry(tmpPath);
			res.error = QStringLiteral("Could not replace original file.");
			return res;
		}
		if (!renameFileRetry(tmpPath, finalPath)) {
			// Restore backup
			renameFileRetry(backup, srcInfo.absoluteFilePath());
			res.error = QStringLiteral("Could not replace original file. Original restored.");
			return res;
		}
		if (!removeFileRetry(backup)) {
			res.ok = true;
			res.outputPath = finalPath;
			res.error = QStringLiteral("Trimmed file replaced, but the backup (%1) could not be deleted. "
						   "Please remove it manually.")
					    .arg(QFileInfo(backup).fileName());
			return res;
		}
	}

	res.ok = true;
	res.outputPath = finalPath;
	return res;
}

} // namespace obs_trim
