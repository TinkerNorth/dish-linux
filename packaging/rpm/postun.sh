# %postun — $1 is 0 on the final erase and 1 on an upgrade's cleanup. Only the
# erase needs the reload; on an upgrade the new %post already did it.
if [ "$1" = 0 ]; then
    if [ -x /usr/bin/udevadm ]; then
        /usr/bin/udevadm control --reload-rules >/dev/null 2>&1 || :
        /usr/bin/udevadm trigger --subsystem-match=hidraw >/dev/null 2>&1 || :
    fi
fi
