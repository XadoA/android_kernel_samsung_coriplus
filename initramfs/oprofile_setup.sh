#! /bin/sh

if [ -z "$1" ]; then
        profile_linux=0
else
        VMLINUX_PATH=$1
        profile_linux=1
fi

mkdir /dev/oprofile
ln -s /proc/mounts /etc/mtab

if [ $profile_linux -ne 0 ]; then
        opcontrol --vmlinux=$VMLINUX_PATH
else
        opcontrol --no-vmlinux
fi

opcontrol --start-daemon

#opcontrol --start; $profile_app; opcontrol --stop
