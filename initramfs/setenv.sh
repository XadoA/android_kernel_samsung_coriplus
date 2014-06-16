#! /bin/sh
mdev -s

if [ $# -ne 2 ]
then
    echo "Usage : . `basename $0` <buildroot-device> <buildroot-mountpoint>"
    exit 1;
fi

BUILDROOT_DEVICE=$1
BUILDROOT_MOUNT=$2

echo "mounting buildroot filesystem .."
mount -t ext3 /dev/$BUILDROOT_DEVICE $BUILDROOT_MOUNT


echo "setting up buildroot environment .."
# Buildroot environment setup
rm -r /lib
ln -s /$BUILDROOT_MOUNT/lib /lib
rm -r /usr
ln -s /$BUILDROOT_MOUNT/usr /usr
export PATH=$PATH:/$BUILDROOT_MOUNT/bin:/$BUILDROOT_MOUNT/sbin
export LD_LIBRARY_PATH=$BUILDROOT_MOUNT/lib:$LD_LIBRARY_PATH

echo "setting up buildroot environment .."
#QT environment setup
export QTDIR=/usr
export LD_LIBRARY_PATH=$QTDIR/lib:$LD_LIBRARY_PATH
export QT_QWS_FONTDIR=$QTDIR/lib/fonts
export QWS_DISPLAY="LinuxFb:/dev/fb0"
export QWS_MOUSE_PROTO="linuxinput:/dev/input/event1"

echo "Done ... !"
