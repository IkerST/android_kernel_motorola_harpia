#!/usr/bin/env bash

figlet "GKernel"
figlet "Testing"
echo "Starting build"

KERNEL_DIR=$PWD
ANYKERNEL_DIR=$KERNEL_DIR/Anykernel2/harpia
TOOLCHAINDIR=$(pwd)/toolchain/linaro-7.2
DATE=$(date +"%d%m%Y")
KERNEL_NAME="GKernel"
DEVICE="-harpia-"
VER=$(cat version)
TYPE="-OREO-"
FINAL_ZIP="$KERNEL_NAME""$DEVICE""$DATE""$TYPE""$VER".zip
PATCHES=$(ls -1 $(pwd)/patches/ | grep .patch)
CORES=$( nproc --all)
THREADS=$( echo $CORES + $CORES | bc )

export ARCH=arm
export KBUILD_BUILD_USER="ist"
export KBUILD_BUILD_HOST="travis"
export CROSS_COMPILE=$TOOLCHAINDIR/bin/arm-eabi-
export USE_CCACHE=1

if [ -e  arch/arm/boot/zImage ];
then
#Just to make sure it doesn't make flashable zip with previous zImage
rm arch/arm/boot/zImage
fi;

echo "Preparing build"
make harpia_defconfig
#echo "Applying patches"
#cd patches
#for a in $PATCHES
#do
#  patch patch -p1 < $a
#done
#cd ..
echo "Building with " $CORES " CPU(s)"
echo "And " $THREADS " threads"
make -j$THREADS

if [ -e  arch/arm/boot/zImage ];
then
echo "Kernel compilation completed"
cp $KERNEL_DIR/arch/arm/boot/zImage $ANYKERNEL_DIR/
cd $ANYKERNEL_DIR
echo "Making Flashable zip"
echo "Generating changelog"
git log --graph --pretty=format:'%s' --abbrev-commit -n 200  > changelog.txt
echo "Changelog generated"
zip -r9 $FINAL_ZIP * -x *.zip $FINAL_ZIP
echo "Flashable zip Created"
echo "Uploading file"
curl -H "Max-Days: 1" --upload-file $FINAL_ZIP https://transfer.sh/$FINAL_ZIP
echo ""
else
echo "Kernel not compiled,fix errors and compile again"
fi
echo "Uploading logs"
echo ""
