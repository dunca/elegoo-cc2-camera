# ai_camera (cc2-opt fork)

This is a fork of [viridivn/ai_camera](https://github.com/viridivn/ai_camera) v0.2.3 for the Elegoo Centauri Carbon 2. The `cc2-opt` branch adds the following; the original README follows below.

**Faster timelapse encoding.** 150 frames at 720p, measured on the printer:

| Change | Encode time | Peak memory |
| --- | --- | --- |
| v0.2.3 release | 44.8 s | 17.8 MB |
| Built for Cortex-A7 + NEON | 38.1 s | 18.0 MB |
| stb_image NEON paths | 37.3 s | 18.0 MB |
| libjpeg-turbo decodes JPEG straight to YUV (no RGB round trip) | 20.7 s | 14.6 MB |
| Decode the next frame on the second core while x264 encodes | 17.0 s | 15.9 MB |

The output matches the original path (56.6 dB PSNR on the YUV input).

**Reliability.**
- After a read error, an unplug or 5 s without frames, the capture thread reopens the camera, and viewers stay connected.
- The service also starts without a camera and waits for one.
- At most 8 stream viewers; stalled clients are dropped after 10 s.
- Waits use a monotonic clock, so the boot-time clock jump does not affect them.

**Settings and endpoints.**
- `/opt/usr/camera-quality` holds `<resolution>[@<fps>]`, e.g. `720`, `360@10`. It is read at start and applied live on `SIGHUP`: a new frame-rate cap takes effect on the next frame; a new resolution reopens the camera inside the process.
- `GET /snapshot` returns one JPEG, or 503 when there is no recent frame.
- `GET /health` returns JSON: resolution, fps cap, measured fps, age of the last frame, reopen count, viewers, uptime, and the timelapse encoder's status.
- The log goes to `/tmp/ai_camera.log` and is rotated at 256 KB.

**Timelapse frames of mixed sizes**, after a resolution change mid-print, are scaled to fit. Before, they were cropped with the wrong stride.

**Guarded timelapse encoding.** The printer has ~108 MB of RAM and no swap. An encode that starts while memory is short can stall for minutes and freeze the whole printer; it did once.
- **Separate process.** The service runs the encode as a child process (`ai_camera --encoder-child -E …`), one at a time, at nice 19. The child is the kernel's first out-of-memory victim (`oom_score_adj` 1000). All of its memory (~16 MB at 720p, independent of length) goes back when it exits, and its peak is measured.
- **Memory gate.** An encode starts only when `MemAvailable` is at least 30 MB; it re-checks every 5 s for up to 30 min. While it runs, a watchdog stops it if `MemAvailable` stays below 10 MB for 10 s. A stopped or failed encode keeps its frames and is retried once. Tune with `--encode-start-mb`, `--encode-floor-mb` and `--encode-max-wait-min`.
- **No truncated videos.** The video is written to `<name>.mp4.part` and renamed once complete. An `.mp4` without its index (`moov`), left by an older build or a power cut, is treated as missing: it is re-encoded instead of being linked to the print and having its frames deleted.
- **Leftover recovery.** 2 minutes after the service starts, finished timelapses still in `/opt/usr/picture` (the printer or service stopped before the encode completed) are encoded through the same guarded path.
- **Failures are not reported to the printer.** Elegoo's `elegoo_printer` crashes on the upstream "Time-Lapse Composite Video Failed" status report.
- **Logging.** Each encode adds one line to `/opt/usr/timelapse-encode.log` (survives reboots, rotated at 64 KB), for example:
  `2026-10-11 17:29:32 ok 't200.mp4' frames=200 waited=0s available_at_start=51MB lowest_available=32MB encoder_peak=15MB took=29.1s size=12596082`.
  Other outcomes are `gave_up_waiting`, `stopped_low_memory`, `killed_signal_N` and `failed`. The child's output appears in the service log prefixed `[encoder]`, and `/health` has an `encode` object: state (`idle`/`waiting`/`encoding`) and the last result, wait, memory and duration.

**Printer scripts** are in [`printer/`](printer/): `camq` (set resolution and fps), an SSH forced-command wrapper for home automation, and `camwatch.sh` (restarts the service if it dies or hangs).

**Getting into the printer** (SSH, and recovering from a bootloop over UART + FEL): see [dunca/elegoo-cc2-recovery](https://github.com/dunca/elegoo-cc2-recovery).

### Build

```
./build.sh                     # zig cc (macOS or Linux): build/zig-cortex_a7/ai_camera
CC2_TOOLCHAIN=gcc ./build.sh   # musl.cc gcc, x86_64 Linux only
```

`build.sh` fetches and builds pinned x264 and libjpeg-turbo, and links everything statically. `tools/bench.sh` compares two builds on the printer without touching the running service.

Licence: AGPLv3, as upstream.

---

# ai_camera

A lightweight replacement for the stock ai_camera service on the Elegoo Centauri Carbon 2. Supports higher resolution streaming, timelapses and auto-encoding while (hopefully) being less prone to crashing.

---

## Use

0. **Back up the original ai_camera**:
   ```
   root@TinaLinux:~# cp /opt/bin/ai_camera /opt/bin/ai_camera.bak
   ```

1. **Compile**:
   ```
   you@host:ai_camera$ make
   ```

2. **Stop the printer service**:
   ```
   root@TinaLinux:~# /etc/init.d/printer stop
   ```

3. **Copy executable to printer**:
   ```
   you@host:ai_camera$ scp ai_camera root@[IP_ADDRESS]:/opt/bin/ai_camera
   ```

4. **Reboot printer**:
   ```
   root@TinaLinux:~# reboot
   ```

---

## Open issues

- All "AI" functionality is removed. This is intentional.
- Needs more stress testing
- Unknown if higher resolutions can overwhelm the USB bus
- Make system may or may not work on your machine

---

## Warnings

- Only tested in LAN only mode
- Not tested on all firmware versions
- Only tested on Elegoo Centauri Carbon 2
- Author is not responsible for any damage caused by this software
- Could eat your dog

---

## Acknowledgements

This project is made possible thanks to the following open-source libraries:

- **[x264](https://code.videolan.org/videolan/x264)**
- **[minimp4](https://github.com/lieff/minimp4)**
- **[stb_image](https://github.com/nothings/stb)**
- **[musl libc](https://musl.libc.org/)** & **[musl-cross](https://musl.cc/)**
- **[sqlite](https://sqlite.org/)**

---

## License

This project is licensed under AGPLv3. Elegoo is not exempt from the GPL no matter how much they pretend otherwise.

