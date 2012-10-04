#!/bin/bash

BUILD_TYPE=$1
RELEASE_DIR=$2

KERNEL_DIR=$PWD



if [ -f $KERNEL_DIR/release.conf ]; then
	BUILD_SAM=`grep BUILD_SAM $KERNEL_DIR/release.conf | cut -d'=' -f2`
	BUILD_AOSP=`grep BUILD_AOSP $KERNEL_DIR/release.conf | cut -d'=' -f2`
	BUILD_MULTI=`grep BUILD_MULTI $KERNEL_DIR/release.conf | cut -d'=' -f2`
	BUILD_COMMON=`grep BUILD_COMMON $KERNEL_DIR/release.conf | cut -d'=' -f2`
else
	BUILD_SAM=1
	BUILD_AOSP=1
	BUILD_MULTI=1
	BUILD_COMMON=1
fi


if [ -z ../sc02c_initramfs ]; then
  echo 'error: sc02c_initramfs directory not found'
  exit -1
fi

cd ../sc02c_initramfs
if [ ! -n "`git status | grep clean`" ]; then
  echo 'error: sc02c_initramfs is not clean'
  exit -1
fi

cd $KERNEL_DIR

if [ -z "$BUILD_TYPE" ]; then
	read -p "select build type? [(r)elease/(n)ightly] " BUILD_TYPE
fi
if [ "$BUILD_TYPE" = 'release' -o "$BUILD_TYPE" = 'r' ]; then
  export RELEASE_BUILD=y
else
  unset RELEASE_BUILD
fi
. mod_version

# create release dir＿
if [ -z $RELEASE_DIR ]; then
  RELEASE_DIR=../release
fi
RELEASE_DIR=$RELEASE_DIR/$BUILD_VERSION
mkdir -p $RELEASE_DIR
echo $RELEASE_DIR

# build for samsung
if [ $BUILD_SAM == 1 ]; then
	bash ./build-samsung.sh a
	if [ $? != 0 ]; then
	  echo 'error: samsung build fail'
	  exit -1
	fi
	cp -v ./out/SAM/bin/* $RELEASE_DIR/
fi

# build for aosp
if [ $BUILD_AOSP == 1 ]; then
	bash ./build-aosp.sh a
	if [ $? != 0 ]; then
	  echo 'error: aosp build fail'
	  exit -1
	fi
	cp -v ./out/AOSP/bin/* $RELEASE_DIR/
fi

# build for common
if [ $BUILD_COMMON == 1 ]; then
	bash ./build-common.sh a
	if [ $? != 0 ]; then
	  echo 'error: common build fail'
	  exit -1
	fi
	cp -v ./out/COMMON/bin/* $RELEASE_DIR/
fi

# build for multiboot
if [ $BUILD_MULTI == 1 ]; then
	bash ./build-multi.sh a
	if [ $? != 0 ]; then
	  echo 'error: multi build fail'
	  exit -1
	fi
	cp -v ./out/MULTI/bin/* $RELEASE_DIR/
fi


