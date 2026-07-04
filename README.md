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

### Launch OBS at login (elevated)
**File → Launch OBS at Login (Elevated)** registers a Windows Scheduled Task that
starts OBS automatically at logon **with administrator rights and no UAC prompt** —
the only sanctioned way to auto-launch an elevated app at login (the normal Startup
Apps list and `shell:startup` folder silently refuse elevated programs).

The task points at the current OBS executable, runs with highest privileges, and fires
a few seconds after logon. Combined with *Start Recording on Startup*, this gives a
hands-off "boot → OBS runs elevated → recording begins" pipeline. Toggling it on
requires OBS to currently be running as administrator (so it has permission to create
the task); un-checking removes the task.

### "Open in Media Player" link
When a recording is saved, the status bar shows an **Open in Media Player** link that
opens the finished file directly in the modern Windows **Media Player** app
(`IApplicationActivationManager::ActivateForFile`), with automatic fallback to
Windows Media Player Legacy and then the system default player.

*(This replaces the earlier "Open in CapCut" link.)*

---

## Audio & sources

### Voice Level Match (auto mic leveling against your voice call)
A new **Voice Level Match** audio filter keeps your microphone's *typical speaking
level* matched to the *typical level of the voices in your voice call* (Desktop
Audio), so you sit at the same loudness as your friends without riding the fader.

- Both your mic and the reference source run a lightweight vocal-isolation chain
  (150 Hz–4 kHz speech band-pass + voice-activity detection); only detected speech
  contributes to the level estimates.
- The level trackers are streaming **medians**, so **yelling and whispering are
  outliers that barely move the baseline** — your dynamics pass through untouched.
  Only your sustained "normal talking" level is matched.
- The correction is a slow-slewing trim (≈1 dB/s, configurable), not a compressor.
- Click the **≈ button** on your mic's Audio Mixer row for a **live visualization**:
  both voice-level estimates with voice-activity lights, the applied gain, and a
  scrolling 60-second history graph. The button auto-creates the filter (reference
  defaults to Desktop Audio) on first use.

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
