#!/usr/bin/env bash
# Benchmark ai_camera builds on the printer without touching the running service.
#
# Usage: CC2_HOST=root@<printer> tools/bench.sh FRAMES_DIR BUILD_DIR...
#   FRAMES_DIR  directory of 1280x720 JPEG frames (e.g. grabbed from the :8080 stream)
#   BUILD_DIR   build/zig-<cpu> directories (each with ai_camera + stagebench); "release" = /opt/bin/ai_camera
# Runs each full encode (ai_camera -E) and the per-stage benchmark twice, then removes everything it uploaded.
set -euo pipefail
HOST="${CC2_HOST:?set CC2_HOST=root@<printer-ip>}"
FRAMES="$1"; shift
REMOTE=/opt/usr/bench

ssh -o BatchMode=yes "$HOST" "rm -rf $REMOTE && mkdir -p $REMOTE/frames"
tar cf - -C "$FRAMES" . | ssh -o BatchMode=yes "$HOST" "tar xf - -C $REMOTE/frames"
NAMES=()
for b in "$@"; do
  if [[ "$b" == release ]]; then
    ssh -o BatchMode=yes "$HOST" "mkdir -p $REMOTE/release && cp /opt/bin/ai_camera $REMOTE/release/ai_camera"
    NAMES+=(release)
  else
    name="$(basename "$b")"
    ssh -o BatchMode=yes "$HOST" "mkdir -p $REMOTE/$name"
    tar cf - -C "$b" ai_camera stagebench | ssh -o BatchMode=yes "$HOST" "tar xf - -C $REMOTE/$name"
    NAMES+=("$name")
  fi
done

ssh -o BatchMode=yes "$HOST" "cd $REMOTE && chmod -R 755 . && n=\$(ls frames | wc -l) && echo \"\$n frames\" && for round in 1 2; do for b in ${NAMES[*]}; do
  rm -f out.mp4; t0=\$(cut -d' ' -f1 /proc/uptime)
  ./\$b/ai_camera -E frames -o out.mp4 -r 15 > /dev/null 2>&1 & P=\$!; hwm=0
  while kill -0 \$P 2>/dev/null; do h=\$(awk '/VmHWM/{print \$2}' /proc/\$P/status 2>/dev/null); [ -n \"\$h\" ] && hwm=\$h; usleep 100000; done
  t1=\$(cut -d' ' -f1 /proc/uptime)
  echo \"\$b: full=\$(awk \"BEGIN{printf \\\"%.1f\\\", \$t1-\$t0}\")s peak=\${hwm}kB out=\$(wc -c < out.mp4)B load=\$(cut -d' ' -f1 /proc/loadavg)\"
  [ -x \$b/stagebench ] && echo \"   \$(./\$b/stagebench frames/*.jpeg 2>/dev/null)\"
done; done; rm -rf $REMOTE"
