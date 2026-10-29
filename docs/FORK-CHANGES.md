# Fork changes

The history of this fork's work (`juanjsebgarcia/vita-moonlight`) on top of upstream `xyzz/vita-moonlight` 0.13.2, so anyone picking it up can see what exists, how the pull requests stack, and what's still open. Every change goes in through a pull request; nothing is merged directly.

Last updated: 2026-10-29. All 20 PRs are open drafts. The `perf-60fps` branch (a separate line of work, not part of the PR stack) is described in [its own section](#the-perf-60fps-branch-simplysanatorivita-moonlight).

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

## The `perf-60fps` branch (simplysanatori/vita-moonlight)

A different line of work from the PR stack above. `perf-60fps` starts from `juan/vita` (`c7624e2`), cherry-picks ten of Juan's small fix branches, then merges seven performance and robustness branches of its own. Nothing here is a draft PR yet, and **none of it has been built or run on a Vita as of 2026-10-29**: the machine that wrote it had no compiler, so the first CI build and the first device session are the real test. The design write-ups and reviews are in `analysis/` (`10` to `17` and `review-*.md`), outside this repo.

**Cloning:** `git clone --recurse-submodules <repo>`, or in an existing clone `git submodule sync --recursive && git submodule update --init --recursive`. `--recursive` is required because `third_party/moonlight-common-c` has a nested submodule (`nanors`), and `sync` is needed once because the submodule URL moved to `simplysanatori/moonlight-common-c`.

### Cherry-picked from Juan's fork (10 branches)

Cherry-picked with `-x` onto `perf-60fps`. Two conflicts (`audio/vita.c`, `ui_connect.c`) were resolved by keeping both sides; `fix/crash-risks` was ported without its i18n hunks, since this tree has no `tr()`.

| Branch | What it does |
|---|---|
| `perf/net-memory` | Logs the UDP receive buffer the Vita grants (`[NET] UDP receive buffer: asked N bytes, granted N`), through a link-time wrap of `bindUdpSocket` |
| `fix/mdns-socket-race` | Serialises the mDNS sniffer and really stops the host-scan threads before a stream |
| `fix/audio-latency-drift` | Drops queued audio to catch up after a network stall; the drop count is read atomically |
| `fix/audio-port-release` | Releases the audio port after each stream |
| `perf/no-decode-while-paused` | No decoding while paused; asks for a keyframe on resume and retries on a timer; the resume keyframe is never paced away |
| `fix/explicit-colorspace` | Asks the host for BT.601 limited range explicitly |
| `fix/connection-leaks` | Frees the server info and app list each connection replaces; keeps the server info if `serverinfo` doesn't parse |
| `fix/boost-while-connecting` | Boosts the clocks while connecting, pairing and starting a stream, under one mutex |
| `fix/http-timeouts-reuse` | Times out unreachable PCs and reuses the HTTP/TLS session |
| `fix/crash-risks` | Bounds host-controlled copies, clamps config indexes, no exit mid-run, reports a failed config write |

### Merged branches

| Branch | Adds | Setting (default) | Verify on the device |
|---|---|---|---|
| `feat/commonc-bump` | **moonlight-common-c** now comes from `simplysanatori/moonlight-common-c`, branch `vita-perf`: upstream `master` (about 50 commits: loss handling, thread fixes, RTSP parser, lock-free RTT query), Juan's reorder window on top, and a Vita patch that makes the local-address probe non-fatal (on failure it logs and falls back to wildcard binds, as before). FEC is now **nanors** with a NEON path instead of `reedsolomon/rs.c`, and the library target builds with `-std=gnu11` for `aligned_alloc`. The public API only gained things, so no client code changed | none | Build: `echo \| arm-vita-eabi-gcc -dM -E - \| grep __ARM_NEON`, and `arm-vita-eabi-objdump -d build/third_party/libmoonlight-common-c.a \| grep -c vtbl` above 0 (NEON is chosen at compile time and falls back to scalar silently). A stream that fails at "Resolving" points at the address probe; green blocks after loss, at FEC |
| `feat/presenter` | **Frame pacing** (below) | Stream tab > Frame pacing: Off / **Smooth** | `display refresh X Hz` at setup, `display refresh: reported ... measured ...` later, the `Smooth:` overlay line |
| `feat/rfi` | **Reference frame invalidation made safe** (below) | Stream tab > Enable reference frame invalidation (**off**) | `reference frame invalidation: decoder with N reference frames`, `SPS: host num_ref_frames ...`, `decoder output lags input by N frame(s)` |
| `feat/threads` | **Thread priorities and optional core pinning** (below) | System tab > CPU core pinning (**off**) | `[THREAD]` lines |
| `feat/audio` | **Opus loss concealment, any packet duration, 10 ms packets** (below) | Stream tab > Audio packet length: **5 ms** / 10 ms | `audio: N-sample packets (X ms)`, `concealed N lost packets` |
| `feat/robustness` | No OLED dimming mid-stream, motion thread clamp, async debug log, framebuffer message fix (below) | none | see below |
| `feat/wifiturbo` | **Wi-Fi Turbo plugin integration** (below) | System tab > Warn about Wi-Fi Turbo settings (**on**) | `[WIFITURBO]` lines |
| fix on top | The full stats overlay holds 12 lines (Smooth and Wi-Fi Turbo each raised the limit to 11 on their own and overflowed it together); lines past the limit are dropped. The Recommended presets' help now says they set Frame pacing | none | |

### Frame pacing: Off / Smooth

Without it, vita2d presents with `NEXTFRAME`: every frame waits for the next refresh (median ~6.5 ms) and some are replaced before ever being shown. **Smooth** moves that wait before the draw. The decode thread only decodes into one of four textures (two more than before, about 4 MB of CDRAM at 960x544) and queues it. A `VideoPresent` thread wakes at each vblank, sleeps until `period - margin` after it, then shows **one frame per refresh**, so the swap lands just before the next refresh.

- An empty queue repeats the previous picture (no draw, no swap). If the queue stays 3 or more deep for 30 refreshes (about 0.5 s), the oldest frame is dropped; after a burst (4 or more deep) it takes only 6 refreshes. The 60.000 vs 59.94 Hz difference shows up as about one such drop every 17 s.
- Every unit is still decoded, since P-frames are references; frames are dropped only at presentation.
- **Latency against Off**, estimated from the design and not yet measured on a device: about **+1 ms** while no reserve frame is held, about **+18 ms** typical (one reserve frame held), about **+35 ms** during a pending catch-up. Off is unchanged: draw and swap right after decode.
- Configs with no `frame_pacing` key become **Smooth** on upgrade. The Recommended settings pick Smooth, except the **Lowest delay** priority, which picks Off. A benchmark run forces Off.
- With Smooth, "Enable VITA vblank" and "Enable frame pacer" are not used; both apply only with Off.
- **Stats overlay** (full mode): `Smooth: shown N  repeat N  drop N  queue avg/max  wait X ms`. `shown` is frames put on screen, `repeat` refreshes that kept the previous picture (about 60 a second on a static screen is normal), `drop` frames discarded by catch-up or overflow, `queue` the depth at each refresh, `wait` how long a frame sat queued. With Off the line reads `Frame pacing off: ...`. The `[STATS]` log line gained `present_vblanks`, `present_repeat`, `present_drop`, `queue_avg`, `queue_max` and `queue_wait`, and the latency estimate includes the queue wait.
- **Hidden ini key `present_margin_us`** (default 4000, clamped 1000-12000): how long before the refresh the swap happens. It rises by itself if drawing is slower. A steady run of `repeat` with a deep queue means swaps land just after the vblank, so try 6000.
- Also check `[THREAD] video present ...` for the presenter's priority and stack (128 KB), and that `frame_textures[i]: not enough memory` does not appear in the log.

### Reference frame invalidation (RFI)

Still **off by default**. Turning it on used to give heavy artifacts after loss, because the client squeezed the decoder to one reference picture while the host used more. With RFI on it now does this:

- **Decoder reference probing:** at connect, the decoder is tried with 16, 8 and then 5 reference frames (16 and 8 only if the decoder memory block stays under 16 MiB; 5 is NVENC's default), and falls back to 4. Each try is a dry run (create, delete).
- **SPS rewrite** (`libgamestream/sps.c`) keeps the host's `num_ref_frames` (clamped to what the decoder holds), sets `max_dec_frame_buffering` to match, and skips the level override when the level's DPB couldn't hold them. With RFI off the SPS comes out as before. A malformed SPS is passed through unchanged, and out-of-range SPS fields are rejected instead of indexing past a table.
- **Output lag check:** if the decoder holds pictures back because of the bigger DPB, the log says `decoder output lags input by N frame(s) (ref frames N)`. Nothing is printed with RFI off. A lag of N costs about N x 16.7 ms of latency, which would make RFI a lossy-Wi-Fi-only option.
- **Hidden ini key `rfi_min_dpb_hint`** (default false, only with RFI on): keeps the host's `num_ref_frames` but tells the decoder `max_dec_frame_buffering = 1`. Try it if the lag line appears; `telling the decoder its DPB is one frame (rfi_min_dpb_hint)` confirms it is active. It only counts as a fix if the lag line disappears and recovery frames still clear instead of smearing.
- Other lines: `SPS: host num_ref_frames X, level L; sent to the decoder (holds N): ...`, and common-c's `Reference frame invalidation is not supported by this host`, which makes an A/B pointless (Sunshine with Intel Arc QSV H.264 doesn't offer it). A `sceAvcdecDecode` error right after the RFI messages would be a frame_num gap the decoder rejects.

### Thread plan, priorities and core pinning

`src/threadplan.c` holds one table. Common-c's threads are tuned through a link-time wrap of `PltCreateThread` (no common-c patch), the app's own threads through `threadplan_create()`. Lower number is more urgent; before, every common-c thread ran at 159.

| Thread | Priority | Core with pinning on |
|---|---|---|
| AudioRecv, AudioDec | 70, 71 | 2 |
| VideoRecv | 72 | 1 |
| VideoPresent | 73 | 0 |
| VideoDec | 74 | 0 |
| ControlRecv | 96 | any |
| ReqIdrFrame, InvRefFrames | 98 | any |
| vitainput_thread, InputSend | 100 | any |
| motion gyro and accel | 128 | any |
| LossStats | 130 | any |
| CtrlAsyncCb | 140 | any |
| VideoPing, AudioPing | 150 | any |
| AsyncTerm, frame_pacer | 160 | any |
| vitapower_thread | 170 | any |
| hostscan, mdns | 180 | any |
| debug_log_flush | 185 | any |

Audio ranks above video because a missed audio grain is audible, while video has a queue to absorb jitter. A thread with no row keeps its creator's priority (about 160), so a new thread needs a row.

- **"CPU core pinning"** (System tab, config `pin_threads`, **default off**, needs a new stream): receive on core 1, decode and present on core 0, audio on core 2. Threads marked "any" get all three user cores set explicitly. Whether it helps is unknown; see the checklist.
- **Build switches:** `cmake -DTHREAD_TUNING=OFF` leaves every thread as it was. `-DTHREAD_PINNING=OFF` compiles pinning out whatever the setting says (the row is still listed but does nothing).
- **Verify:** turn on "Enable debug log" for one run (it slows streaming, so don't benchmark with it). Each tuned thread logs `[THREAD] VideoRecv: wanted priority 72 mask 0x10000, set rc 0x0/0x0, was priority 159 mask 0x0, now priority 72 mask 0x10000, cpu N, stack N`. A non-zero `rc`, or a `now` that differs from `wanted`, means the kernel refused; a refusal is also printed with the debug log off. The bench report records `pin_threads`.

### Audio

- **Loss concealment (PLC):** a packet that the audio FEC couldn't rebuild is concealed by libopus instead of leaving a gap, and so is a packet that fails to decode. During a catch-up, placeholders are skipped like real packets. It covers only losses inside an FEC block that partly arrived; a block lost entirely leaves no placeholder. Opus in-band FEC is not used.
- **Any packet duration:** packets decode whatever their length (a 10 ms packet used to be dropped), into a ring of three 64-byte-aligned buffers. The output grain stays 960 samples (20 ms).
- **"Audio packet length"** (Stream tab, config `audio_packet_ms`, **5 ms** default or 10 ms, needs a new stream): 10 ms sends about 100 fewer audio packets a second for about 5 ms more audio delay, and each loss then conceals 10 ms. It works by setting common-c's `CAPABILITY_SLOW_OPUS_DECODER`, which also makes the RTSP URL `rtsp://0.0.0.0:port`; Sunshine and Apollo ignore that, while GFE would give low-quality stereo. Hosts older than app version 7 stay at 5 ms. The bench report records the negotiated duration.
- **Verify** (debug log on): `audio: 240-sample packets (5.0 ms)` or `audio: 480-sample packets (10.0 ms)` at stream start; `audio: dropped N packets in N catch-ups, concealed N lost packets` at stream end; `audio: host sends N samples a packet, M negotiated` if the host ignores the request. Under lossy Wi-Fi, listen for clicks against smooth gaps.

### Robustness

- **No OLED dimming mid-stream:** the 10 s power thread also ticks `DISABLE_OLED_DIMMING`, still only while streaming and with "Disable power save" on.
- **Motion clamp:** a host asking for more than 1000 Hz of motion reports used to make the motion threads spin, and rate 0 divided by zero. The period is now floored at 2 ms, so 1000 Hz becomes **500 Hz**, and periods are no longer rounded to whole milliseconds.
- **Async debug log:** lines go into a 64 KB buffer and a `debug_log_flush` thread writes it every 250 ms, so hot threads no longer wait on the memory card. It is flushed when a stream ends and at exit, so Quit loses nothing; a crash can lose the last 250 ms. A full buffer drops lines and writes `[LOG] n lines dropped`. If the thread can't start, lines are written directly.
- **Framebuffer check:** the debug-screen check was inverted and printed "Failure to set Framebuffer!" on every successful start. Fixed.

### Wi-Fi Turbo integration

Optional support for the [vita-turbo-wifi](https://github.com/juanjsebgarcia/vita-turbo-wifi) plugin (written against v0.4.1; MIT, notice in `third_party/wifiturbo/LICENSE`). Moonlight links four of its functions as **weak imports** (`third_party/wifiturbo/wifiturbo_imports.yml`, stubs generated by `vita-libs-gen` at build time), so it loads and runs without the plugin.

- **Detection:** at boot `wifiturbo_init()` checks that the import resolved and that `wqSelfTestPing()` returns `0x57544231`. Otherwise every Wi-Fi Turbo row and warning stays hidden. A `wifiturbo.probe` marker file stops it calling the plugin again after a crash.
- **Pre-stream warnings:** when a stream starts, a read-only check puts a message bar above the "Press START + L + R" hint for 9 s. It names settings that can interrupt a stream: Smart Mesh Roaming on, 40 MHz on, In-game HUD on, a receive buffer other than Default within 120 s of boot, or a power profile other than Max performance. Turn it off with System tab > **Warn about Wi-Fi Turbo settings** (config `wifiturbo_notices`, default on). Moonlight never changes the plugin's settings by itself.
- **Settings rows** (System tab, only with the plugin): "Wi-Fi Turbo settings for streaming" (reads "good for streaming" or "N to change"; the help lists them) and **Apply recommended Wi-Fi Turbo settings**. Apply turns off roaming, 40 MHz and the in-game HUD and picks Max performance, after a confirmation that lists the differences. The plugin saves them, so they persist. **Wi-Fi reconnects for a few seconds** when 40 MHz or the profile changes. **It is refused (the row is hidden) during a stream.**
- **Stats overlay** (full mode, sampled once a second): `SNR 31 dB  noise -89 dBm  profile Max performance`, or `SNR n/a` until the plugin has seen a link report.
- **Verify:** `[WIFITURBO] wqSelfTestPing stub: ...` (a `mvn r0,#0 / bx lr` placeholder means not imported, a `movw / svc / bx lr` sequence means resolved), `[WIFITURBO] wqSelfTestPing returned 0x57544231` with the plugin, `[WIFITURBO] SceWifiTurbo wasn't imported: the plugin isn't installed` without it, and `[WIFITURBO] wqUiSubmit("...") returned N` after Apply. Right after boot the plugin's state can still be all zeros; that counts as unknown, so no notice is shown.
- **Not done:** nothing is restored after a stream, since the app never changes settings by itself.

### Other ini keys

- **`video_reorder_wait_ms`** (default 20): how long common-c holds the next frame's packets while the current frame is incomplete. Clamped to **0-200** in `session.c`; 0 turns the window off.
- **`present_margin_us`** and **`rfi_min_dpb_hint`**: see above. All three are written to the config file when settings are saved, so they can be edited there.

### A/B test checklist

Nothing above is measured on a device yet. Use one setup (for example 960x544 at 60 fps, 10 Mbps, the same spot and host), change one thing at a time, run each side for about 5 minutes, and compare the full stats overlay (latency, `Lost/IDR/FEC`, `Net/Queue` maxima, the `Smooth:` line) with the freezes you see and hear. Keep the debug log off for timing runs.

1. **RFI on / off** (Stream tab): test on lossy Wi-Fi (about 55-60% signal, or 30 Mbps to overload 2.4 GHz). Look for the `decoder output lags` line first, then count freezes against smear and how fast artifacts clear. Try `rfi_min_dpb_hint` if it lags. Needs a host that supports RFI.
2. **CPU core pinning on / off** (System tab): compare the `Net/Queue` maxima, dropped frames and audio catch-ups, and confirm the cores in the `[THREAD]` lines.
3. **Frame pacing Off / Smooth** (Stream tab): compare the overlay latency (expect Smooth about +18 ms) with the repeat and drop counters and how even motion looks. Touch `present_margin_us` only if `repeat` stays high.
4. **Audio packet length 5 / 10 ms**: listen under Wi-Fi interference for clicks or gaps, and compare the `concealed N lost packets` line.
5. **Video packet size 1024 vs 1392** (Stream tab; 1024 is the default): 1392 sends fewer packets per frame. Go back to 1024 if the video breaks up or won't start.
6. **FEC 20% vs 30% on the host** (a Sunshine/Apollo setting, not a Vita one): compare `Lost/IDR/FEC` and freezes. More FEC protects better but uses more bandwidth.

## Main findings

- **Latency budget:** the Vita side is ~5 ms per frame (receive ~2.4, decode 2.4 with YUV, draw 0.2). Most of the end-to-end lag is the PC's (~60 ms) and, on the bench page, the browser's (~80-100 ms). A stream at 50 fps on a 60 Hz desktop costs ~30 ms.
- **vita2d presents with `NEXTFRAME`:** every frame waits for the next refresh (median ~6.5 ms), and ~18% of drawn frames were replaced before ever being shown. None of the earlier timings included this wait. Addressed by **Frame pacing: Smooth** (a vblank-locked presenter thread with a short queue, on by default; Off keeps the old behaviour). Existing configs without a `frame_pacing` key become Smooth (about +18 ms typical); Settings > Frame pacing > Off, or Recommended > Lowest delay, restores the old behaviour. Unmeasured on the device so far: compare the overlay's shown/repeat/drop counters. Details and latency figures are in the `perf-60fps` section above.
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
