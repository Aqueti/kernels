#!/bin/bash
# This script is used to builds linux kernels for tegra TX1 systems. based on the Nvidia JetPack
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
INTERACTIVE="false";

#check for a -h on start
if [ "$1" == "-h" ]||[ -z "$1" ] ; then
   echo
   echo "Usage: makeall.sh <name of destination directory>"
   echo
   echo This script is used to builds linux kernels for tegra TX1 systems based on the Nvidia JetPack
   echo installed from JetPack-L4T-2.1-linux-x64.run. It is for a 64bit build. This script must
   echo be executed within the TX1/Linux_for_Tegra_tx1/ directory.
   echo
   echo On installation, a kernel release must be selected from the sources/kernel_sources directory. A
   echo release is selected by typing git checkout \<release\>.
   echo
   exit
fi

#define global settings.
BUILD_HOME=$PWD 
DEFAULT_IMAGE=tegra_t210ref_gnu_linux_defconfig            #Default config file to use 
SOURCE_PATH="$BUILD_HOME/kernel_source"
CONFIG_PATH="arch/arm64/configs"

#Specify output directory
export TEGRA_KERNEL_OUT=$1
export ARCH=arm64
export MODULES_OUT=$TEGRA_KERNEL_OUT/modules/$ARCH

export CROSS_COMPILE=/usr/bin/aarch64-linux-gnu-
export CROSS32CC=/usr/bin/arm-linux-gnueabihf-gcc

if [ -d $TEGRA_KERNEL_OUT ]; then
   echo "Do you want to replace the data in $TEGRA_KERNEL_OUT? (YES to accept)"
   read text;
   if [ "$text" == "YES" ]; then
      rm -r $TEGRA_KERNEL_OUT/*
   fi
fi

mkdir -p $TEGRA_KERNEL_OUT

#change directory to the kernel_sources directory
cmd="cd $SOURCE_PATH"
eval $cmd
echo "Working in: $SOURCE_PATH"

#echo show available config files
echo
echo "Available config files:"
echo
echo "default"
ls $CONFIG_PATH
echo

echo "Enter config file to use, 'default' for kernel default ($DEFAULT_IMAGE), or blank for previous"
read text

#Determine which optoin
if [ -z "$text" ]; then
   echo "Using previous config file"
else
   if [ "$text" == "default" ]; then
      CONFIG_FILE=$DEFAULT_IMAGE;
   else
      CONFIG_FILE="$text"
   fi

   if [ -e "$TEGRA_KERNEL_OUT/.config" ]; then
      echo "Are you sure you want to replace the existing config file with $CONFIG_FILE? (YES to accept)?"
      read CONFIRM;

      if [ "$CONFIRM" != "YES" ]; then
         echo "aborting"
         exit 1
      fi
   fi
   
   #This will overwrite the existin config file with the default image
   echo "loading $CONFIG_FILE"
   make mrproper
   make O=$TEGRA_KERNEL_OUT mrproper

   #get the right config data
   make O=$TEGRA_KERNEL_OUT $CONFIG_FILE
fi

#Update config using the ncurses interface
make O=$TEGRA_KERNEL_OUT menuconfig

#Build actual components
echo "Enter to build zImage"
if [ "$INTERACTIVE" == "true" ]; then
   read
fi
make O=$TEGRA_KERNEL_OUT zImage -j47
echo "Enter to build dtbs"
if [ "$INTERACTIVE" == "true" ]; then
   read
fi
make O=$TEGRA_KERNEL_OUT dtbs -j47
echo "Enter to build modules"
if [ "$INTERACTIVE" == "true" ]; then
   read
fi
make O=$TEGRA_KERNEL_OUT modules -j47
echo "Enter to install modules"
if [ "$INTERACTIVE" == "true" ]; then
   read
fi
make O=$TEGRA_KERNEL_OUT modules_install INSTALL_MOD_PATH=$MODULES_OUT


echo "Do you want to create a copy of the config file for this kernel (YES to accept)"
read text
if [ "$text" == "YES" ]; then
   echo "Please Enter the name for the config data."
   read name

   output="$CONFIG_PATH/$name"
   if [ -e $output ]; then
      echo "$output exists. Overwrite (YES to accept)"
      read text;
      if [ "$text" != "YES" ]; then
         echo "aborting.. (all was done but the config copy)"
         exit 1;
      fi
   fi

   cmd="cp $TEGRA_KERNEL_OUT/.config $output"
   echo $cmd
   eval $cmd
fi
echo 
echo "Installation complete - hope all worked well"
echo
