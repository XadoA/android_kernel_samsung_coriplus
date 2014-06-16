#! /bin/sh

if [ $# -ne 1 ]
then
    echo "USAGE : `basename $0` <buildroot-mountpoint>";
    exit 1;
fi

BUILDROOT_MOUNT=$1

echo "loading usbshim .."
insmod $BUILDROOT_MOUNT/lib/modules/2.6.32.9/kernel/drivers/net/bcm_usbshim.ko

echo "loading wifi driver.."
insmod $BUILDROOT_MOUNT/lib/modules/2.6.32.9/kernel/drivers/net/wl.ko


echo "Instructions for setting up wifi from now on .."
echo "-----------------------------------------------"
echo " # iwconfig "
echo " # ifconfig -a "
echo " # ifconfig eth0 up "
echo " # wlarm scan  (Scans for available networks)"
echo " # wlarm scanresults (Shows results from previous scan) "
echo " # wlarm join SSID (join an open network)"
echo " # wlarm assoc "
echo " # ifconfig eth0 <ip> netmask <netmask> "
echo " # ping <ip> & "

echo "Done ..!"
