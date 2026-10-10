#!/bin/sh
# Forced command for Home Assistant's camq SSH key (authorized_keys: restrict,command="/bin/sh /usr/bin/camq-ha.sh").
# Only these requests are accepted; anything else is refused.
#   ssh ... root@printer 720       -> 1280x720, prints the resolution
#   ssh ... root@printer 360       -> 640x360,  prints the resolution
#   ssh ... root@printer status    -> current resolution (also when no command is given)
#   ssh ... root@printer fps 15    -> cap the stream at 15 fps (0 = no cap), prints the fps
#   ssh ... root@printer fps       -> current fps cap (0 = none)
Q=/opt/usr/camera-quality
# camq is built into the firmware as /usr/bin/camq; older installs keep it in /opt/usr.
CAMQ=/usr/bin/camq; [ -x "$CAMQ" ] || CAMQ=/opt/usr/camq
res() { r=$(cat "$Q" 2> /dev/null); r=${r%%@*}; echo "${r:-720}"; }
fps() { case "$(cat "$Q" 2> /dev/null)" in *@*) f=$(cat "$Q"); echo "${f#*@}" ;; *) echo 0 ;; esac; }
set -f   # no globbing of the remote command
set -- $SSH_ORIGINAL_COMMAND
[ $# -le 2 ] || { echo "denied"; exit 1; }
case "$1 $2" in
    "360 "|"720 ")
        /bin/sh "$CAMQ" "$1" > /dev/null 2>&1
        res ;;
    " "|"status ")
        res ;;
    "fps ")
        fps ;;
    fps\ [0-9]|fps\ [0-9][0-9])
        /bin/sh "$CAMQ" fps "$2" > /dev/null 2>&1
        fps ;;
    *)
        echo "denied"
        exit 1 ;;
esac
