# Printer-side scripts for the fork

Built into the custom firmware as `/usr/bin/camq`, `/usr/bin/camq-ha.sh` and `/usr/bin/camwatch.sh`, with `camwatch.sh` started from `/etc/rc.local`. They also work from `/opt/usr`. The setting itself lives in `/opt/usr/camera-quality` (UDISK), so it survives firmware updates.

| Script | What it does |
| --- | --- |
| `camq` | Sets resolution and frame-rate cap: `camq 720`, `camq 360`, `camq fps 15`, `camq 720@15`, `camq fps 0` (no cap). Writes `/opt/usr/camera-quality` and sends SIGHUP so the running camera applies it live. Builds without reload support are restarted instead. With no argument it prints the setting and `/health`. |
| `camq-ha.sh` | SSH forced command for Home Assistant's key: only `360`, `720`, `status`, `fps` and `fps N` are accepted. |
| `camwatch.sh` | Supervisor started from rc.local. It restarts `ai_camera` if the process is gone, or if `/health` fails 3 checks 15 s apart (only for builds that have `/health`). It is idle while `elegoo_printer` is not running or `/tmp/camwatch.off` exists. |
