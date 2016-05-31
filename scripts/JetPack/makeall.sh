#!/bin/bash
#This script is used to builds linux kernels for tegra TX1 systems based on the Nvidia JetPack
# installed from JetPack-L4T-2.1-linux-x64.run. It is for a 64bit build. This script must
# be executed within the TX1/Linux_for_Tegra_tx1/ directory.
#
# The JetPack kernel builds are paritcularly finicky. The only verified process is to run mrproper 
# in the kernel_sources directory, install the default config, modify the default config, then 
# make files
#
# On installation, a kernel release must be selected from the sources/kernel_sources directory. A
# release is selected by typing git checkout <release>.
#
# Modifications to kernel files are required. See https://devtalk.nvidia.com/default/topic/914941/jetson-tx1/custom-kernel-compilations/2

#check for a -h on start
if [ "$1" == "-h" ]; then
   echo "Usage: makeall.sh <name of destination directory>"
   echo
   echo This script is used to builds linux kernels for tegra TX1 systems based on the Nvidia JetPack
   echo installed from JetPack-L4T-2.1-linux-x64.run. It is for a 64bit build. This script must
   echo be executed within the TX1/Linux_for_Tegra_tx1/ directory.
   echo
   echo The JetPack kernel builds are paritcularly finicky. The only verified process is to run mrproper 
   echo in the kernel_sources directory, install the default config, modify the default config, then 
   echo make files
   echo
   echo On installation, a kernel release must be selected from the sources/kernel_sources directory. A
   echo release is selected by typing git checkout \<release\>.
   echo
   exit
fi

#define global settings.
JETPACK_HOME="$PWD" 
DEFAULT_IMAGE=tegra_t210ref_gnu_linux_defconfig            #Default config file to use 
KERNEL_PATH="$JETPACK_HOME/kernel"
ROOTFS_PATH="$JETPACK_HOME/rootfs"
SOURCE_PATH="$JETPACK_HOME/sources/kernel_source"
export TEGRA_KERNEL_OUT=$1
mkdir -p $TEGRA_KERNEL_OUT
export MODULES_OUT=$TEGRA_KERNEL_OUT/modules/arm64

export ARCH=arm64
export CROSS_COMPILE=/usr/bin/aarch64-linux-gnu-
export CROSS32CC=/usr/bin/arm-linux-gnueabihf-gcc

echo output: $TEGRA_KERNEL_OUT, $ARCH, $CROSS_COMPILE, $MODULES_OUT

#change directory to the kernel_sources directory
cmd="cd $SOURCE_PATH"
eval $cmd
echo "Working in: $PWD"

#copy current config file
cp .config config.previous

#This will overwrite the existin config file with the default image
echo "Do you want to load the default image to $DEFAULT_IMAGE (YES to overwrite): "
read text
if [ "$text" == "YES" ]; then
   echo "loading default image"
   make mrproper
   make O=$TEGRA_KERNEL_OUT mrproper
   rm -r $TEGRA_KERNEL_OUT/*
   make O=$TEGRA_KERNEL_OUT $DEFAULT_IMAGE
fi

#Update config using the ncurses interface
make O=$TEGRA_KERNEL_OUT menuconfig

#Build actual components
echo "Enter to build zImage"
read
make O=$TEGRA_KERNEL_OUT zImage -j47
echo "Enter to build dtbs"
read
make O=$TEGRA_KERNEL_OUT dtbs -j47
echo "Enter to build modules"
read
make O=$TEGRA_KERNEL_OUT modules -j47
echo "Enter to install modules"
read
make O=$TEGRA_KERNEL_OUT modules_install INSTALL_MOD_PATH=$MODULES_OUT


#Check if we want to install files
echo "Do you want replace JetPack the install kernel? (YES to overwrite) "
read text
if [ "$text" != "YES" ]; then
   exit
fi

echo "Press any key to update kernel"
read

cp $TEGRA_KERNEL_OUT/arch/arm64/boot/Image $KERNEL_PATH
cp $TEGRA_KERNEL_OUT/arch/arm64/boot/zImage $KERNEL_PATH
cp $TEGRA_KERNEL_OUT/arch/arm64/boot/dts/* $KERNEL_PATH/dtb

echo "Copying modules:"
echo "sudo cp -r $MODULES_OUT/lib/modules/3.10.96aqueti+/* $ROOTFS_PATH/lib/modules/3.10.67-g458d45c"
sudo cp -r $MODULES_OUT/lib/modules/3.10.96aqueti+/* $ROOTFS_PATH/lib/modules/3.10.67-g458d45c
sudo cp -r $MODULES_OUT/lib/modules/3.10.96aqueti+ $ROOTFS_PATH/lib/modules/

#go back to the home directory
cd $JETPACK_HOME
#Check if we want to install files
echo "Do you want replace flash the Tegra? (YES to flash) "
read text
if [ "$text" != "YES" ]; then
   exit
fi

cmd="sudo ./flash.sh  jetson-tx1 mmcblk0p1"
echo "command: $cmd"
eval $cmd

echo "completed: sudo ./flash.sh  jetson-tx1 mmcblk0p1"
echo "Installation complete - hope all worked well"
echo
