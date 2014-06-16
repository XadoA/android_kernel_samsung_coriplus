#!/bin/sh

if [ $# != 1 ]
then
    echo "$0 : Invalid or no arguments"
    exit 1;
fi

MMC=$1

files=`ls /sys/devices/platform/mmc_host/$MMC`

EXE_PATH=/bin
VOLD_FSTAB=/system/etc/vold.fstab
TMP_FSTAB=/tmp/vold.fstab

addr="xxxx"

for i in $files
do
   name=`echo $i | $EXE_PATH/cut -d":" -f1`
   if [ $name == "$MMC" ]
   then
      addr=`echo $i | $EXE_PATH/cut -d":" -f2`
      echo "SDCARD address = $addr"
      break;
   fi
done

if [ $addr == "xxxx" ]
then
   echo "FATAL Error: Cannot determine SDCard addr, Android will not be able to detect SDCARD..."
else
   echo "Updating vold.fstab entries....."
   #Delete all lines matching dev_mount
   cat $VOLD_FSTAB | $EXE_PATH/awk '/^#/ {printf line; line=$0"\n"; next} /dev_mount/ {line=""} ! /dev_mount/ {printf line; print; line=""}' >> $TMP_FSTAB

   $EXE_PATH/mv $TMP_FSTAB $VOLD_FSTAB

   #Append vold.fstab file...
   echo "dev_mount sdcard /mnt/sdcard 1 /devices/platform/mmc_host/$MMC/$MMC:$addr/block/mmcblk`echo $MMC | cut -d 'c' -f 2`" >> $VOLD_FSTAB
fi

