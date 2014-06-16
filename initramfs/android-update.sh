#! /bin/sh

#Varibales
FIRMWARE_FILENAME=bigisland-firmware.img
FIRMWARE_FILE=/sdcard/$FIRMWARE_FILENAME
TEMP_SPACE=/tmp
UPDATE_SCRIPT=/sdcard/update.sh
RLE_TO_FB=/sbin/rle_to_fb
UPDATE_RLE=/sbin/updating.rle
FAIL_RLE=/sbin/failed.rle
DONE_RLE=/sbin/done.rle
RELEASE_RLE=/tmp/release_info.rle
SPLASH_SCREEN_FILE="/proc/bcm2835_mdec"

#check Volup+Back key pressed 
EXE_PATH="/bin"
gpio_file=/sys/kernel/debug/gpio

volume_up=`$EXE_PATH/cat $gpio_file | $EXE_PATH/grep "Volume up"`
#Back button is actually "Search" in this gpio file
back=`$EXE_PATH/cat $gpio_file | $EXE_PATH/grep "Search"`

volume_up_state=`echo $volume_up | $EXE_PATH/cut -d ')' -f 2 | $EXE_PATH/cut -d ' ' -f 3`
back_state=`echo $back | $EXE_PATH/cut -d ')' -f 2 | $EXE_PATH/cut -d ' ' -f 3`

echo "State of Volume Up button = $volume_up_state"
echo "State of Back button = $back_state"

if [ $volume_up_state == "lo" -a $back_state == "lo" ]
then
	echo "**** Volume up + Back key pressed ****"
	echo "Force update from /sdcard/firmware/$(FIRMWARE_FILENAME)"
	mv /sdcard/firmware/$FIRMWARE_FILENAME $FIRMWARE_FILE
fi

#check for firmware update
if [ -f $FIRMWARE_FILE ]
then

	#Stop the splash screen
	echo 0 > $SPLASH_SCREEN_FILE

	#Stop the watch dog
	echo V > /dev/watchdog

	#create a firmware directory
	mkdir -p /sdcard/firmware

	echo "Update available! Extracting files..."

	$RLE_TO_FB $UPDATE_RLE

	#untar the image 
	tar -x -C $TEMP_SPACE -f $FIRMWARE_FILE

	#check status of last command
	if [ $? != 0 ]
	then
		echo "Error in extracting firmware file"
		echo "Please copy the update file again"
		exit 1
	fi

    #display the updating message
	if [ -f $RELEASE_RLE ]
	then
		$RLE_TO_FB $RELEASE_RLE
	fi

	if [ -f $UPDATE_SCRIPT ]
	then
		exec $UPDATE_SCRIPT
	else
	   #update boot1
	   if [ -f $TEMP_SPACE/boot1.bin_with_header.bin ]
       then
           echo "Info: Flashing boot1 to eMMC ..."
           if dd if=$TEMP_SPACE/boot1.bin_with_header.bin of=/dev/mmcblk0 bs=512 count=256;
           then
              echo "Info: Flashing boot1 done."
           else
              echo "Error: flashing boot1 failed."
             $RLE_TO_FB $FAIL_RLE
             exec /bin/busybox sh
           fi
       fi

	   #update u-boot
       if [ -f $TEMP_SPACE/u-boot.bin ]
       then
           echo "Info: Flashing u-boot to eMMC ..."
           if dd if=$TEMP_SPACE/u-boot.bin of=/dev/mmcblk0 bs=512 count=512 seek=2048;
           then
              echo "Info: Flashing u-boot done."
			  echo "Erasing u-boot environment.."
			  dd if=/dev/zero of=/dev/mmcblk0 bs=512 count=8 seek=2560;
			  echo ".done"
           else
              echo "Error: Flashing u-boot failed."
             $RLE_TO_FB $FAIL_RLE
             exec /bin/busybox sh
           fi
       fi
	   
	   #update u-boot-nvram
       if [ -f $TEMP_SPACE/u-boot-nvram.bin ]
       then
           echo "Info: Flashing u-boot NVRAM to eMMC ..."
           if dd if=$TEMP_SPACE/u-boot-nvram.bin of=/dev/mmcblk0 bs=512 count=8 seek=2560;
           then
              echo "Info: Flashing u-boot NVRAM done."
           else
              echo "Error: u-boot NVRAM failed."
             $RLE_TO_FB $FAIL_RLE
             exec /bin/busybox sh
           fi
       fi


	   #update system.tar
       if [ -f $TEMP_SPACE/system.tar ]
       then
          echo "doing /system"
          rm /system/* -rf
          if tar -x -C / -f $TEMP_SPACE/system.tar;
          then
             echo "/system done!"
          else
             echo "/system failed..."
             $RLE_TO_FB $FAIL_RLE
             exec /bin/busybox sh
          fi
       fi

       sleep 2
       sync
       sync
       sync


	   #update data.tar
       if [ -f $TEMP_SPACE/data.tar ]
       then
          echo "doing /data"
          rm /data/dalvik-cache/* -rf
          rm /data/data/* -rf
          rm /data/app/* -rf
          if tar -x -C / -f $TEMP_SPACE/data.tar;
          then
             echo "/data done!"
          else
             echo "/data failed..."
             $RLE_TO_FB $FAIL_RLE
             exec /bin/busybox sh
          fi
       fi

	   #uImage
       if [ -f $TEMP_SPACE/uImage ]
       then
          if cp $TEMP_SPACE/uImage /sdcard/firmware/uImage;
          then
             echo "uImage done!"
          else
             echo "uImage failed..."
             $RLE_TO_FB $FAIL_RLE
             exec /bin/busybox sh
          fi
       fi

	   #vcfirmware.bin
	   if [ -f $TEMP_SPACE/vcfirmware.bin ]
       then
          if cp $TEMP_SPACE/vcfirmware.bin /sdcard/firmware/vcfirmware.bin;
          then
			  echo "VC firmware done!"
          else
			  echo "VC firmware failed..."
             $RLE_TO_FB $FAIL_RLE
             exec /bin/busybox sh
          fi
       fi

	   #cp image
	   if [ -f $TEMP_SPACE/cpimage.tar ]
       then
          echo "updating cp image"
          if tar -x -C /sdcard/firmware -f $TEMP_SPACE/cpimage.tar;
          then
             echo "Updated cp image!"
          else
             echo "Failed to update cp image..."
             $RLE_TO_FB $FAIL_RLE
             exec /bin/busybox sh
          fi
       fi

	   #splash screen
       if [ -f $TEMP_SPACE/splash.h264 ]
       then
          if cp $TEMP_SPACE/splash.h264 /sdcard/firmware/splash.h264;
          then
             echo "Updated splash screen!"
          else
             echo "Failed to update splash screen"
             $RLE_TO_FB $FAIL_RLE
             exec /bin/busybox sh
          fi
       fi

      #boot sound
       if [ -f $TEMP_SPACE/boot_sound.wav ]
       then
          if cp $TEMP_SPACE/boot_sound.wav /sdcard/boot_sound.wav;
          then
             echo "Updated boot sound!"
          else
             echo "Failed to update boot sound"
             $RLE_TO_FB $FAIL_RLE
             exec /bin/busybox sh
          fi
       fi

	   #move the firmware file
	   mv $FIRMWARE_FILE /sdcard/firmware/

       sync
       sync
       sync
       sync
       sync
       sync
       sleep 10
       umount /data
       umount /sdcard
       $RLE_TO_FB $DONE_RLE
       umount /system
    fi

    #sleep for 30 seconds, then reboot
    sleep 30
    reboot -f

else
    echo "No update found"
fi
