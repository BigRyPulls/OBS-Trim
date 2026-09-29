/*
OBS-Trim
Copyright (C) 2026 BigRyPulls

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <QString>
#include <QStringList>

namespace obs_trim {

struct MediaInfo {
	bool valid = false;
	qint64 durationMs = 0;
	int width = 0;
	int height = 0;
	double fps = 0.0;
	QString videoCodec;
	QStringList audioCodecs;
	int audioStreamCount = 0;
	int totalStreams = 0;
	// Per-stream detail in file order, for strict stream-copy verification.
	QStringList streamTypes; // e.g. {"video", "audio", "audio"}
	QStringList streamCodecs; // e.g. {"h264", "aac", "aac"}
};

QString formatMs(qint64 ms);
QString sanitizeFilename(const QString &name);
QString findExecutable(const QString &name);
QString findFfmpeg();
QString findFfprobe();

MediaInfo probeMedia(const QString &ffprobePath, const QString &filePath, QString *errorOut = nullptr);

/* Options for lossless trim. */
struct TrimOptions {
	QString sourcePath;
	qint64 startMs = 0;
	qint64 endMs = 0; // exclusive; <=0 means EOF
	QString destFileName; // without directory, with extension preserved by caller
};

struct TrimResult {
	bool ok = false;
	QString outputPath;
	QString error;
};

/*
 * Perform safe lossless stream-copy trim.
 * - Creates temp file in same directory as source.
 * - Runs ffmpeg -i <src> -ss <start> [-to <end>] -map 0 -map_metadata 0
 *   -map_chapters 0 -c copy (output seeking: slower than input seeking
 *   but considerably more accurate for stream copy).
 * - Verifies output with ffprobe (stream count/types/codecs, duration).
 * - Renames temp to final dest, deletes source (with short bounded
 *   retry to tolerate async OS handle release on Windows).
 * - If start==0 and end covers full duration (rename-only), just renames safely.
 * Never overwrites an existing destination. Never re-encodes.
 */
TrimResult trimLossless(const QString &ffmpegPath, const QString &ffprobePath, const TrimOptions &opts);

} // namespace obs_trim
