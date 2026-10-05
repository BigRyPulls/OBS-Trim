# OBS-Trim

Simple Windows OBS Studio plugin: when you stop a recording, it opens the finished file in a small trim window so you can pick Start/End, rename, and save.

Workflow: Stop recording -> trim popup -> choose In/Out -> name file -> Save -> lossless replacement.

**OBS-Trim uses stream-copy trimming. Video and audio are not re-encoded.**

Lossless (`-map 0 -c copy`) cuts can only start cleanly around keyframes, so the saved start may snap to a nearby keyframe. This is honest stream-copy behaviour: zero re-encoding matters more than frame-exact cuts. Same container is kept (`.mkv` stays `.mkv`, `.mp4` stays `.mp4`).

## Requirements

- OBS Studio 31.x (tested on 31.1.2), Windows 10/11 x64
- FFmpeg + FFprobe in `PATH` (e.g. `winget install Gyan.FFmpeg`, or gyan.dev essentials build 7.x). Set overrides via `OBS_TRIM_FFMPEG` / `OBS_TRIM_FFPROBE` env vars if needed.

## Install (from release zip)

1. Close OBS completely.
2. Copy `obs-trim` folder to `C:\ProgramData\obs-studio\plugins\` so you get `...\plugins\obs-trim\bin\64bit\obs-trim.dll`.
3. Start OBS. Use `Tools -> OBS-Trim: Open Last Recording` to reopen manually.

## Use

- Stop a recording -> editor opens automatically (toggle with the checkbox in the dialog) and starts playing.
- Preview: Play/Pause (Space), Restart (R), -5s (J) / +5s (L), seek via timeline/slider, `I` = set start, `O` = set end, `Left/Right` = ±100ms (`Shift` = ±1s), `Home/End` = jump to In/Out. Start/End can also be typed directly as `MM:SS.mmm`.
- Filename defaults to the recording name without extension. Only Windows-forbidden characters are sanitized.
- Save creates `.<name>.obs-trim.tmp.<ext>` in the same folder, runs lossless copy (`-map 0 -map_metadata 0 -map_chapters 0 -c copy`), verifies with ffprobe (exists, non-zero, video codec unchanged, all stream types/codecs unchanged incl. every audio track, sane duration), then renames to your name. With **Replace original ON** (default, remembered) the source is deleted/replaced only after verification; with it **OFF** the original is kept (full-range save copies instead of renaming). If anything fails the original is left untouched. Existing filenames are never overwritten silently.
- Cancel/close leaves the original exactly where it is.

## Build (Windows)

Requires VS2022, CMake 3.28+, internet (deps auto-fetch via `buildspec.json`: OBS 31.1.1 sources, obs-deps, Qt6).

```powershell
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo --parallel
cmake --install build_x64 --prefix release/RelWithDebInfo --config RelWithDebInfo
```

Output: `release/RelWithDebInfo/obs-trim/bin/64bit/obs-trim.dll` + `data/locale/en-US.ini`.

## Notes

- Uses `OBS_FRONTEND_EVENT_RECORDING_STOPPED` + `obs_frontend_get_last_recording()` only (no directory polling). Window is parented to the OBS main window.
- Preview uses OBS's own private `ffmpeg_source` + `obs_display` (no custom decoder). The preview source is private and never saved into scene collections.
- Settings: auto-open (default ON), replace-original (default ON) + window geometry, stored in `BigRyPulls/OBS-Trim` QSettings (`HKCU\Software\BigRyPulls\OBS-Trim` on Windows).
- The `record-rename` plugin (if installed) must stay disabled for recordings: per-profile `basic.ini` needs `[RecordRename] RenameRecord=false, RenameReplay=false`. OBS-Trim is the only plugin that should rename/replace finished recordings.
- License: GPL-2.0-or-later (see LICENSE). Architecture inspired by [obs-replay-clip-editor](https://github.com/ProbablyFineSoftware/obs-replay-clip-editor) (GPL-2.0-or-later) but this is a focused fresh implementation with no copied code.
