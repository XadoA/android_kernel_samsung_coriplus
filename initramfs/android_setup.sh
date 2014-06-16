#!/bin/sh

mdev -s

board=`cat /proc/cpuinfo | awk '/^Hardware/ {print $3}'`
num_emmc_partitions=`ls /sys/class/block/mmcblk0* | wc -l`
boot_from_emmc=1

#Set use_raw_ramdisk (below) to 1 if CONFIG_USB_DUAL_DISK_SUPPORT=y is chosen in your platform's default configuration. Comment out the following line if dual disk support is not enabled
use_raw_ramdisk=1


# Always use eMMC to mount userdata and system regardless of the board
# we are running on
if [ ! -e /dev/mmcblk1p1 ]
then
    SDCARD_PARTITION=/dev/mmcblk1
else
    SDCARD_PARTITION=/dev/mmcblk1p1
fi

if [ $num_emmc_partitions -le 13 ]
then
    SYSTEM_PARTITION=/dev/mmcblk0p8
    DATA_PARTITION=/dev/mmcblk0p9
else
    if [ $board == "Island" ]
    then
        SYSTEM_PARTITION=/dev/mmcblk0p21
        DATA_PARTITION=/dev/mmcblk0p22
    else
        SYSTEM_PARTITION=/dev/mmcblk0p23
        DATA_PARTITION=/dev/mmcblk0p24
	CACHE_PARTITION=/dev/mmcblk0p21
    fi
fi

mount $SDCARD_PARTITION /sdcard
mount $SYSTEM_PARTITION /system
mount $DATA_PARTITION /data
mount $CACHE_PARTITION /cache
mount -o bind /system/etc /etc

export PATH=$PATH:/system/bin:/system/sbin
#ALSA utils
ln -s /system/bin/alsa_amixer  /bin/amixer
ln -s /system/bin/alsa_aplay /bin/arecord
ln -s /system/bin/alsa_aplay /bin/aplay

# Softlink for eMMC RPMB partition
ln -s /dev/mmcblk0rpmb /dev/block/mmcblk0rpmb

chmod -R 0777 /system
chmod -R 0777 /data

chmod 0777 /dev/fb0

#ril permisions
chmod 0777 /dev/bcm_*

# apanic related activities
#Create a directory to store the logs
mkdir -p /data/dontpanic

for FILE in /sys/block/mmcblk0/mmcblk0p*
   do
      APANIC_PARTITION_NAME=`cat ${FILE}/partition_name`
      APANIC_PARTITION_NUMBER=`cat ${FILE}/partition`
      if [ "${APANIC_PARTITION_NAME}" = "kpanic" ]
      then
         APANIC_PARTITION="/dev/mmcblk0p${APANIC_PARTITION_NUMBER}"
         echo "Found kpanic Partition:  ${APANIC_PARTITION}"
         break
      fi
done

# Trigger the apanic operation. This just verifies the header and
# creates /proc/apanic_console, /proc/apanic_threads using which
# we can read the panic info
#
# Note that -n option added to echo command. This means do not send the new
# line character. This is important because the driver behaviour would change
# based on this. From Android .rc file to do the same operation we use
# 'write' command which would not include the new line character. If we use
# echo without -n option it would send the new line and the driver will get
# one byte extra in its count. To avoid this we are including -n.
echo -n ${APANIC_PARTITION} > /proc/apanic

# Now read the actual panic content
cp /proc/apanic_console /data/dontpanic/console
cp /proc/apanic_threads /data/dontpanic/threads

# Attach timestamp to the created entries (if any)
mv /data/dontpanic/console /data/dontpanic/console_`date +%d%m%y_%H%M%S`
mv /data/dontpanic/threads /data/dontpanic/threads_`date +%d%m%y_%H%M%S`

# Erase the kpanic content
echo 1 > /proc/apanic_console

cp /gingerbread-init.rc /init.rc

# comment out the update firmware script
#/android-update.sh

#setup vold.fstab settings..
if [ $board == "Island" ]
then
# echo "Setting audio path ..." 
# /sbin/playback_spk_init_sh 
echo "Updating vold.fstab"
/sbin/voldupdater.sh mmc0
elif [ $board == "RheaRay" ]
then
echo "Updating vold.fstab"
/sbin/voldupdater.sh mmc2
else
   echo "Unknown board- Don't know how to update vold.fstab"
fi

#Check if we should boot android
#note no function support in busybox shell and hence live with this...

cd /sys/kernel/debug

version=Linux.version.3
kernel=`cat /proc/version | grep $version`
if [ -z $kernel ]
then
version=2.6
else
version=3.x
fi
echo Kernel Version is $version

# usb_mass_storage
if [ $version == "3.x" ]
then
echo $SDCARD_PARTITION > /sys/class/android_usb/android0/f_mass_storage/lun0/file
else
echo $SDCARD_PARTITION > /sys/devices/platform/usb_mass_storage/lun0/file
fi

if [ $use_raw_ramdisk -eq 1 ]
then
#Create a raw ramdisk that can be formatted from the PC/USB Host
dd if=/dev/zero of=/dev/raw-ramdisk bs=512 count=512
if [ $version == "3.x" ]
then
echo /dev/raw-ramdisk > /sys/class/android_usb/android0/f_mass_storage/lun1/file
else
echo /dev/raw-ramdisk > /sys/devices/platform/usb_mass_storage/lun1/file
fi
else
echo "Raw ramdisk NOT created to demonstrate multi-LUN support in USB MSC"
fi

EXE_PATH="/bin"

#note that the gpio for Back and search are swapped!!
gpio_state=`$EXE_PATH/cat gpio | $EXE_PATH/grep Back`

cd /

#state=`echo $gpio_state | $EXE_PATH/cut -d')' -f2 | $EXE_PATH/cut -d' ' -f3`
state=`echo low`

echo "gpio_state of Search button = $state"

# Run usb_portd to automatically launch atx when USB configuration changes.
/system/bin/usb_portd&

# Run bkmgrd automatically on startup
/system/bin/bkmgrd&

# Setup ADB Daemon.  The loopback network interface needs to be brought up first
ifconfig lo up
adbd&

#For LMP broadcom boot_mode run usb RNDIS unless uboot usbdev_mode option is set
/system/bin/usbdev_mode
result=$?
if  [ $result == 1 ]
then
	echo Using Uboot USB Configuration Setting
else
	echo Configure USB as RNDIS
	sh /system/etc/usbdev.sh rndis
fi

if [ $state == "hi" ]
then
# Set the date to 1st Feb, 2011
    echo "Setting date to Feb 11, 2011"
   /system/bin/date -s 20110220
#Start Sec Watchdog daemon to enable sec watchdog
   /sbin/watchdog-simple &
   echo "******************************* BOOTING ANDROID (GINGERBREAD) *************************"
   echo 0 > /proc/bcm2835_mdec
   cp /gingerbread-init /init
   sync
   /init
else
#Enable panic on oops
   echo 1 > /proc/sys/kernel/panic_on_oops
#Disable sec watchdog
   echo V > /dev/watchdog
   echo "******************************* BUSYBOX SHELL ******************************************"
   echo 0 > /proc/bcm2835_mdec
   exec setsid sh -c 'exec sh </dev/ttyS0 >/dev/ttyS0 2>&1'
fi
