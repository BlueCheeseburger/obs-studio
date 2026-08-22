# Claude Code Instructions

## Branching
- Always push directly to `master`. Do not create new branches.
- If you are on a different branch, merge into master and push.

## Building
- This project builds with CMake using the `windows-x64-local` preset.
- Always set `CMAKE_TLS_VERIFY=0` when downloading dependencies.

## Game Capture hook DLL (BattlEye)
- `graphics-hook64.dll`/`graphics-hook32.dll` built locally are unsigned, and
  BattlEye (War Thunder) blocks unsigned code from injecting into the game
  process, so Game Capture silently fails to hook while Display/Window
  Capture are unaffected.
- Fix applied: copied the officially signed hook DLLs (from the stock
  `C:\Program Files\obs-studio` install, v1.8.7.0) over the locally-built
  ones (v1.8.8.0) in BOTH places that matter:
  - `build_x64\rundir\RelWithDebInfo\data\obs-plugins\win-capture\`
  - `C:\ProgramData\obs-studio-hook\`
  Verified safe first: diffed every commit between the 1.8.7 and 1.8.8 hook
  version bumps — only formatting/unrelated changes, no protocol/ABI change,
  and `HOOK_VER_MAJOR` matches. Unsigned originals backed up to
  `/tmp/hook-backup/` before overwriting.
- **Any future build that touches `win-capture` regenerates the unsigned
  1.8.8.0 hook and silently undoes this.** After such a build, re-copy the
  signed DLLs from `C:\Program Files\obs-studio\data\obs-plugins\win-capture\`
  into both locations above before assuming Game Capture works.
- OBS's own hook updater (`update_hook_file` in
  `plugins/win-capture/game-capture-file-init.c`) refuses to downgrade the
  ProgramData copy on its own, so replacing only the data dir copy is not
  enough — always replace both.
