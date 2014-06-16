#! /bin/sh
# Simple script to setup usb0 interface

usage()
{
	echo "usage:`basename $0` [OPTION] ..."
	echo "Setup usb0 interface and mount remote filesystem using NFS"
	echo "Mandatory arguments to long options are mandatory for short options too"
	echo "   -i, --ipaddress IP         use IP for usb0 interface"
	echo "                              default ipaddress if not specified IP=192.168.0.10"
	echo "   -r, --remote NFSPATH       mount remote nfs filesystem specified by NFSPATH "
	echo "                              NFSPATH is of the form <NFS SERVER IP>:<PATH TO NFS SHARE>"
	echo "                              default NFSPATH is 192.168.0.1:/srv/nfsroot"
	echo "   -m, --mount  MOUNTPOINT    mount remote filesystem at path specified by MOUNTPOINT"
	echo "                              default MOUNTPOINT is /system"
	echo "   -a, --android              Specify if remote filesystem is android"
	echo "                              default is no android filesystem"
	echo "Example:"
	echo "   `basename $0` --ipaddress 192.168.0.5 --remote 10.19.75.159:/srv/nfsbuildroot -m /system"

} 

USB0_IP=192.168.0.10
NFS_DEVICE=192.168.0.1:/srv/nfsroot
NFS_MOUNT=/system
ANDROID_FS=0
OLDDIR=`pwd`

while [ "$1" != "" ]; do
    case $1 in
        -i | --ipaddress )      shift
                                USB0_IP=$1
                                ;;
        -r | --remote )			shift
								NFS_DEVICE=$1
                                ;;
		-m | --mount )			shift
								NFS_MOUNT=$1
								;;
		-a | --android )		ANDROID_FS=1
								;;
        -h | --help )           usage
                                exit
                                ;;
        * )                     usage
                                exit 1
    esac
    shift
done

#check if we have kernel 2.6
if [ -e /sys/class/usb_composite ]
then
	cd /sys/class/usb_composite/
	echo "Enabling adb interface"
	echo 1 > adb/enable
	echo "Enabling usb mass storage"
	echo 1 > usb_mass_storage/enable
	echo "Disabling ACM"
	echo 0 > acm/enable
	echo "Enabling RNDIS interface"
	echo 1 > rndis/enable
	echo "Configuring usb0 interface with IP "$USB0_IP
	ifconfig usb0 $USB0_IP up 
	route add default gw 192.168.0.1 usb0
else
	pid="4e13"
	echo "Enable RNDIS Composite Device PID:$pid"
	echo 0 > /sys/class/android_usb/android0/enable
	echo $pid > /sys/class/android_usb/android0/idProduct
	echo rndis > /sys/class/android_usb/android0/functions
	echo 1 > /sys/class/android_usb/android0/enable
	echo "Configuring rndis0 interface with IP "$USB0_IP
	ifconfig rndis0 $USB0_IP up 
	route add default gw 192.168.0.1 rndis0
fi	

if [ $ANDROID_FS -eq 1 ]
then
		echo "Mounting android filesystem"
		echo "Mount "$NFS_DEVICE"/system at /system"
		mount -t nfs -onolock $NFS_DEVICE/system /system
		echo "Mount "$NFS_DEVICE"/data at /data"
		mount -t nfs -onolock $NFS_DEVICE/data /data
else
        echo "Mounting remote filesystem "$NFS_DEVICE "at "$NFS_MOUNT
		mount -t nfs -onolock $NFS_DEVICE $NFS_MOUNT
fi
cd $OLDDIR
