#!/bin/sh

BINDIR=./out

MODE=$1

if [ "$MODE" = "adb" ]; then
  adb push $BINDIR/zImage /data/local/tmp/zImage
  adb shell su -c "cat /data/local/tmp/zImage > /dev/block/platform/dw_mmc/by-name/KERNEL"
  adb shell su -c "rm /data/local/tmp/zImage"
  adb shell su -c "sync;sync;sync;sleep 2; reboot"
elif [ "$MODE" = "adb-nosu" ]; then
  adb push $BINDIR/zImage /tmp/zImage
#  adb push $BINDIR/zImage /data/local/tmp/zImage
  adb shell "cat /tmp/zImage > /dev/block/platform/dw_mmc/by-name/KERNEL"
#  adb shell "cat /data/local/tmp/zImage > /dev/block/platform/dw_mmc/by-name/KERNEL"
  adb shell "rm /tmp/zImage"
  adb shell "sync;sync;sync;sleep 2; reboot"
else
  sudo heimdall flash --kernel ./out/zImage --verbose
fi
