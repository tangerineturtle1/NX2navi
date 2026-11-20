#!/bin/sh
# PolicyKit action for the launcher's confirmed udmabuf setup or removal.
set -eu

RULE=/etc/udev/rules.d/70-udmabuf-uaccess.rules
TMPFILE=/etc/tmpfiles.d/udmabuf.conf
PARAM=/sys/module/udmabuf/parameters/size_limit_mb

if [ "$(id -u)" -ne 0 ]; then
    echo 'This action needs administrator authorization.' >&2
    exit 1
fi

case "${1:-}" in
    apply)
        [ -e "$PARAM" ] || { echo 'The udmabuf module is not loaded.' >&2; exit 1; }
        printf '%s\n' 'KERNEL=="udmabuf", SUBSYSTEM=="misc", TAG+="uaccess"' > "$RULE"
        udevadm control --reload
        udevadm trigger --name-match=udmabuf
        printf '%s\n' 1024 > "$PARAM"
        printf '%s\n' 'w /sys/module/udmabuf/parameters/size_limit_mb - - - - 1024' > "$TMPFILE"
        ;;
    remove)
        rm -f "$RULE" "$TMPFILE"
        udevadm control --reload
        udevadm trigger --name-match=udmabuf
        ;;
    *)
        echo 'Usage: udmabuf_fix.sh apply|remove' >&2
        exit 2
        ;;
esac
