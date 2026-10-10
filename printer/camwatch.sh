#!/bin/sh
# camwatch: keeps the CC2 camera service (ai_camera) running.
# Elegoo's /etc/init.d/printer starts ai_camera once with nothing watching it; this loop
#  - starts it again if the process is gone, and
#  - restarts it if it stops answering /health (hung); only for our fork, the stock camera has no /health.
# Started in the background from /etc/rc.local. It does nothing while the printer stack is
# stopped (no elegoo_printer process) or while /tmp/camwatch.off exists (manual testing).
BIN=/opt/bin/ai_camera
LOG=/tmp/ai_camera.log
HEALTH=http://127.0.0.1:8080/health
INTERVAL=15      # seconds between checks
MAX_MISSES=3     # consecutive failed /health probes before a restart

log() { echo "[camwatch] $(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"; }

start_camera() {
    cd / && start-stop-daemon -S -b -x "$BIN"
}

# BusyBox 1.27 wget has no timeout option, so bound the probe ourselves (5 s).
probe() {
    wget -q -O /dev/null "$HEALTH" 2>/dev/null &
    w=$!
    i=0
    while kill -0 "$w" 2>/dev/null; do
        if [ "$i" -ge 5 ]; then
            kill "$w" 2>/dev/null
            wait "$w" 2>/dev/null
            return 1
        fi
        sleep 1
        i=$((i + 1))
    done
    wait "$w"
}

# Our fork's binary carries the /health field names; the stock camera does not.
has_health() { grep -q camera_reopens "$BIN" 2> /dev/null; }

sleep 60   # let /etc/init.d/printer start everything first
misses=0
while :; do
    sleep "$INTERVAL"
    [ -e /tmp/camwatch.off ] && continue
    pidof elegoo_printer > /dev/null 2>&1 || continue

    pid=$(pidof ai_camera)
    if [ -z "$pid" ]; then
        log "ai_camera not running, starting it"
        start_camera
        misses=0
        continue
    fi

    has_health || continue
    if probe; then
        misses=0
    else
        misses=$((misses + 1))
        if [ "$misses" -ge "$MAX_MISSES" ]; then
            log "ai_camera (pid $pid) stopped answering /health, restarting it"
            kill "$pid" 2>/dev/null
            sleep 3
            kill -9 "$pid" 2>/dev/null
            sleep 1
            start_camera
            misses=0
        fi
    fi
done
