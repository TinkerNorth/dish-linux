# %post — see packaging/deb/postinst for why the retrigger is not optional.
# $1 is 1 on a fresh install and 2 on an upgrade; both want the reload.
if [ -x /usr/bin/udevadm ]; then
    /usr/bin/udevadm control --reload-rules >/dev/null 2>&1 || :
    /usr/bin/udevadm trigger --subsystem-match=hidraw >/dev/null 2>&1 || :
fi
