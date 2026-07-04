# OBS Studio — Customized Build

This is a customized fork of [OBS Studio](https://obsproject.com) with a number of
quality-of-life and workflow features added on top of the upstream project. The
original project README is preserved in [`README.rst`](README.rst).

Everything below is **additive** — no stock functionality was removed except where
explicitly noted. All customizations are Windows-focused.

---

## Recording workflow

### Taskbar thumbnail recording controls
Hovering the OBS icon on the Windows taskbar shows a live-preview popup with two
media-player-style buttons beneath it (like Spotify's controls):

- **● Start Recording** — enabled only while *not* recording.
- **■ Stop Recording** — enabled only *while* recording.

Each button is contextual (the invalid one is greyed out), so you can start and stop
recording without bringing the OBS window to the foreground. The Stop button still
honors the "Warn before stopping recording" setting.

*Implemented with the Win32 `ITaskbarList3` thumbnail-toolbar API; clicks are routed
through OBS's native event filter to the existing record actions.*

### Auto-start recording on startup
OBS can automatically begin recording as soon as it finishes loading — ideal for
using OBS as a startup/kiosk app.

- Toggle under **File → Start Recording on Startup** (checkable, **on by default**).
- Persisted to user config (`BasicWindow/RecordOnStartup`).
- Fires right after the scene collection loads (outputs ready), and is skipped in
  Safe Mode.

> **Note:** If OBS is configured to *Run as administrator*, Windows cannot silently
> auto-launch it from the Startup Apps list. To combine "run at login" with
> "auto-record," either remove the admin requirement, or create a Task Scheduler task
> with **Run with highest privileges**.

### "Open in Media Player" link
When a recording is saved, the status bar shows an **Open in Media Player** link that
opens the finished file directly in the modern Windows **Media Player** app
(`IApplicationActivationManager::ActivateForFile`), with automatic fallback to
Windows Media Player Legacy and then the system default player.

*(This replaces the earlier "Open in CapCut" link.)*

---

## Audio & sources

### Audio-only sources managed from the Audio Mixer
Audio-only sources are hidden from the **Sources** panel and managed entirely from the
**Audio Mixer**, which gains per-source controls:

- Remove a source directly from the mixer.
- Toggle output/monitoring visibility.
- Toggle **noise suppression** with a dedicated icon.

---

## Streaming

### Automatic YouTube live thumbnail grabber
An opt-in tool (under **Settings → Stream**) that automatically updates your YouTube
live broadcast thumbnail from the program feed while you stream:

- Periodic program-frame capture with quality filters.
- Per-scene exclusion.
- Upload de-duplication and a daily upload cap.
- Backoff handling to stay within YouTube API quota.

---

## Filters & UI

- Expanded **Filters** dialog functionality and UI refinements.
- **Compact scene list** items for a denser scene list.
- Removed the **Check for Updates** link (this is a custom build).

---

## Stability

- **Clean shutdown on OS session-end** — fixes a WASAPI crash that could occur when
  Windows logs off or shuts down while OBS is running.

---

## Building

This fork builds with CMake using the `windows-x64-local` preset (Visual Studio /
MSVC, Qt 6). See [`README.rst`](README.rst) and the upstream
[build instructions](https://github.com/obsproject/obs-studio/wiki/Building-OBS-Studio)
for prerequisites and full details.

```pwsh
cmake --preset windows-x64-local
cmake --build build_x64 --config RelWithDebInfo
```

## License

GPL-2.0-or-later, same as upstream OBS Studio. See [`COPYING`](COPYING).
