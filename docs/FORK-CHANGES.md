# Fork changes

The history of this fork's work (`juanjsebgarcia/vita-moonlight`) on top of upstream `xyzz/vita-moonlight` 0.13.2, so anyone picking it up can see what exists, how the pull requests stack, and what's still open. Every change goes in through a pull request; nothing is merged directly.

Last updated: 2026-10-17. All 20 PRs are open drafts.

To build, deploy, drive a Vita remotely and get measurements or screenshots back, see [`REMOTE-VITA.md`](REMOTE-VITA.md) (and [`DEVELOPMENT.md`](DEVELOPMENT.md) in #5 for the build and one-time setup).

## How the PRs stack

Several PRs build on others. Each one's base is the branch it was written on, so its diff shows only its own commits. Merge from the bottom up.

```
vita (upstream 0.13.2)
├── #5  fix/build-current-vitasdk      build fixes, Docker build, deploy tooling, linker gap
├── #2  perf/compiler-optimisation     -O2, build clean-up
├── #1  perf/hot-path-improvements     streaming hot path, decode thread
│   └── #3  perf/stats-overlay         stats overlay
│       └── #4  perf/toggles           diagnostic toggles
│           └── #7  feat/settings-help help screens, button glyphs, word wrap
│               ├── #8  feat/settings-in-stream   Settings from the pause menu
│               │   ├── #10 feat/bench-mode       developer-mode benchmark (Vita side)
│               │   │   └── #13 feat/yuv-decode    YUV decode, GPU colour conversion
│               │   └── #14 feat/bluetooth-warning
│               │       └── #16 feat/recommended-settings   (also merges #10)
│               │           └── #17 feat/bitrate-steps
│               │               ├── #18 feat/box-art
│               │               └── #19 feat/settings-tabs
│               └── #20 feat/wol-connect
├── #6  fix/main-menu-height
├── #9  feat/bench-target              benchmark page and host tools
├── #11 fix/english-ui
├── #12 fix/host-rename                display names
└── #15 fix/video-reorder-window       moonlight-common-c fork: reorder window
```

**Merge order:** #5 first, since every other branch needs it to build with the current VitaSDK. Then #2, #1, #3, #4, #7, #8, #10, #13, #14, #16, #17, #18, #19 and #20 in stack order. #6, #9, #11, #12 and #15 are independent and can go in any time after #5. As each base merges, retarget the PR above it to `vita`.

**Known conflicts when branches meet:**
- **#18 and #19:** both add the last entry of the settings `enum`. Keep both (`SETTINGS_ABOUT,` then `SETTINGS_APP_LIST_STYLE`).
- **#12 and #16, #12 and #20:** #12 adds `device_label()` (display names). #16's auto-detect, the benchmark and #20's moved IP-changed prompt use the identity name (`host->name`) because #12 isn't in their stack. Switch them to `device_label(info)` once they meet.
- **#13 and #10's later commits:** config keys are added on adjacent lines (`decode_yuv`, `bench_grab`, `bench_idr_hz`). Keep both sides.
- **`touchabsolute.c` has CRLF line endings.** Keep them when rebasing or editing.

## What each PR adds

### Build and tooling

| PR | Adds | Notes |
|---|---|---|
| #5 | Builds with the current VitaSDK (GCC 15, OpenSSL 1.0.2 swap), a Docker image pinned by digest, CI on every PR, `tools/vita.sh` (build, push, launch, log, crash) over vitacompanion, `docs/DEVELOPMENT.md`, `.claude/` gitignored | Later added **`sce_gap.ld`**: pads the code so `vita-elf-create` always has room for its module tables. Without it, a build fails by luck of code size ("segment 1 overlaps") |
| #2 | `-O2` (`VITA_OPT_LEVEL`), no double compile of third-party sources, `APP_VER` without Python | |
| #9 | `tools/bench/`: `bench.html` (a deterministic page: marker, tick barcode, cursor lane, Winamp-style visualiser), `run.sh` (unattended variants), `compare.py`, `conf_edit.py`, `quality.py` (frame grabs to PNG, PSNR), `shot.sh` (menu screenshots) | The page's animation follows the tick, so a tick shows the same picture every run |

### Streaming performance and correctness

| PR | Adds | Measured |
|---|---|---|
| #1 | SPS length fix, clock boost (444/222/222/166 MHz), packet size parsing, touch dedupe, input pacing, a decode thread with two textures, `[THREAD]` diagnostics | |
| #3 | **Stats overlay** (off / fps / full): latency estimate, bitrate, loss, timings, Wi-Fi (yellow below 60%) | Full mode costs ~1.2 ms render; fps mode ~0.1 ms |
| #4 | Toggles: packet size, legacy decode, input sync; frame pacer off by default | |
| #13 | **Fast colour decoding** (default on): the decoder outputs NV21 into `YVU420P2_CSC0` textures and the GPU converts colour | Hardware decode 4.6 → 2.4 ms (−45%). Colours verified against RGBA by pixel readback |
| #15 | **Reorder window:** the submodule points at `simplysanatori/moonlight-common-c` (branch `vita-perf`): upstream `master` (nanors FEC, loss handling, thread fixes) plus the reorder window from `juanjsebgarcia/moonlight-common-c`, rebased on top, and a Vita fork patch that makes the local-address probe non-fatal. The window holds the next frame's packets while the current frame is incomplete. `video_reorder_wait_ms` (default 20), applied via `LiSetVideoReorderWaitUs()` | At ~60% Wi-Fi: freezes 23-37/min → 1-3/min, same latency. ~10% of video packets arrive out of order on the Vita |

### Settings and menus

| PR | Adds | Reusable pieces |
|---|---|---|
| #6 | Paired PCs listed first; the main menu box fits its rows | `make_geom_fitted()`, `MENU_ROW_HEIGHT`, `MENU_MAX_HEIGHT` |
| #7 | △ help for every setting, start-of-stream hint, word wrap, scrolling help | **`draw_button_glyph()`** with `BUTTON_GLYPH_*` (face buttons, D-pad, L/R/L2/R2, START/SELECT, PS, sticks, L3/R3, touch), drawn as shapes because the font has no PlayStation symbols. `wrap_text()`, `display_help()`. Glyph labels use their own font instance (`glyph_font`), because vita2d caches each character at the first size drawn |
| #8 | Settings from the pause menu, showing only what applies on Resume | `MENU_ENTRY_BETWEEN_STREAMS`, `SET_ROW_VALUE` |
| #11 | Keyboard layout names in English; dead hotkey code removed | |
| #12 | Rename a PC with a display name; connection messages show "Name (address)" | `device_label()`, `device_label_for_address()` |
| #14 | Yellow Settings row when Bluetooth is on | `vita_bluetooth_enabled()` (registry `/CONFIG/BT` `bt_enable`), `menu_entry.warning` |
| #16 | **Apply recommended settings:** connection (Good / Weak) × priority (Balanced / Lowest delay / Battery life), **auto-detect** (signal, then a 10 s test stream of the PC's Desktop for packet loss and jitter), PS TV / wired / 3G handling, restart offer mid-stream; alerts sized to their text | `vita_wifi_percent()`, `vita_network_type()`, `ui_connect_find_app()`, `ui_connect_restart_stream()`, `SETTINGS_RESTART_STREAM`, `alert_geom_for()` |
| #17 | Bitrate steps 5-30 Mbps + Custom, with a Custom bitrate row in kbps that's greyed out unless Custom | Saved non-step values open on Custom unchanged |
| #18 | **Box art** carousel for a PC's apps, from Sunshine's `/appasset`, cached on the memory card; "App list" setting (box art / text) | `gs_app_boxart()`, `src/gui/ui_boxart.c`. Covers ≤ 1 KB (Sunshine's grey default) show the app's name instead |
| #19 | **Settings tabs** (Stream, System, Controls, Developer, About), switched with L and R; About tab | `MENU_TAB`, `TAB_*`. The menus see L/R on the **L2/R2 bits** (`sceCtrlPeekBufferPositive`), so check `SCE_CTRL_L1 \| SCE_CTRL_L2` |
| #20 | **Wake on LAN and connect:** wakes, waits for the PC (rechecks every 2 s, resends every 10 s, gives up after 2 min), then connects; □ sends the packet only, with a "□ WoL only" hint | `connect_host()`, `send_wake_packet()` |

### Developer tools on the Vita (#10)

Behind a hidden **developer mode** (hold L and R, press △ in Settings):
- **The benchmark:** runs against the connected PC's "Moonlight Bench" app. It measures input lag, a latency estimate, send-path timing (link-time wraps of ENet), decode split (assembly and hardware), frame drops from the page's tick barcode, the wait for the screen refresh, and **battery drain**. Reports are JSON in `<key_dir>reports/`.
- **Frame grabs** (`bench_grab`): decoded frames at fixed page ticks, for `quality.py`.
- **`bench_input=none`**, to stream any app, and **`bench_idr_hz`**, to request keyframes.
- **Real video packet loss** (`src/netcount.c`): a link-time wrap of `recvUdpSocket` that counts gaps in RTP sequence numbers. moonlight-common-c's own recovered and failed counters are only kept for audio.
- **Menu screenshots** (`src/devshot.c`): a request file triggers a capture, which `tools/bench/shot.sh` turns into a PNG. Developer mode also keeps the screen on while the hook runs.

`docs/DECODE.md` (in #10, results added in #13) holds the latency and decode measurements and the experiments.

## Main findings

- **Latency budget:** the Vita side is ~5 ms per frame (receive ~2.4, decode 2.4 with YUV, draw 0.2). Most of the end-to-end lag is the PC's (~60 ms) and, on the bench page, the browser's (~80-100 ms). A stream at 50 fps on a 60 Hz desktop costs ~30 ms.
- **vita2d presents with `NEXTFRAME`:** every frame waits for the next refresh (median ~6.5 ms), and ~18% of drawn frames were replaced before ever being shown. None of the earlier timings included this wait. Addressed by **Frame pacing: Smooth** (a vblank-locked presenter thread with a one-frame queue, on by default, about +17 ms of latency; Off keeps the old behaviour). Existing configs without a `frame_pacing` key become Smooth (about +18 ms); Settings > Frame pacing > Off, or Recommended > Lowest delay, restores the old behaviour. Unmeasured on the device so far: compare the overlay's shown/repeat/drop counters.
- **Bitrate:** right by the access point the Wi-Fi carried ~31 Mbps without loss. Higher bitrate costs latency (~0.4 ms per Mbps by the AP, more at a weaker signal). On the 960x544 panel, 540p and 720p look alike at the same bitrate, and quality stops improving at ~15-20 Mbps. 720p at 10 Mbps makes the PC drop frames on busy content.
- **The "missing frames" were mostly reorder freezes,** fixed by #15. RFI isn't offered by Sunshine with Intel Arc QSV H.264, so each abandoned frame cost a ~13-frame wait for a keyframe.
- **Battery** (10 min each, stress page): Balanced 426 mA, Lowest delay 378 mA, Battery life 312 mA (~5 h against ~7 h).
- **Possible bug: the menus stop drawing once the screen turns itself off** with nobody touching the Vita, and didn't recover remotely. To check by hand.

## Open items

- **Upstream:** the maintainer is finishing **Moonlight 1.0** with a second, **Borealis-based** interface alongside the legacy one. Engine work (#1, #2, #5, #13, #15, the benchmark, presets logic, box art fetching) should carry over. Menu work (#6, #7, #8, #14, #16-#20 screens) targets the legacy interface and would need porting. Waiting on the 1.0 source.
- **moonlight-common-c:** whether to propose the reorder window upstream, behind a `STREAM_CONFIGURATION` field defaulting to off. That needs evidence from another device first, e.g. Moonlight on a Mac over Wi-Fi.
- **The screen-off freeze** above.
- **Deferred, not rejected:** checking the connection during the next real stream (auto-detect option B), a restart offer for the individual Resolution/FPS/Bitrate rows, true 720p output on PS TV.
- **Not started:** upstream cherry-picks (xml_status precedence, `SIGNATURE_LEN`, uniqueid read check, `snprintf` in `http.c`), a Controls and shortcuts help screen, i18n, audio-video offset, the remaining quality-of-life ideas.
