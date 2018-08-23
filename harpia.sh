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
LOG_FILE=FINAL_ZIP="$KERNEL_NAME""$DEVICE""$DATE""$TYPE""$VER".log

export ARCH=arm
export KBUILD_BUILD_USER="ist"
export KBUILD_BUILD_HOST="travis"
export CROSS_COMPILE=$TOOLCHAINDIR/bin/arm-eabi-
export USE_CCACHE=1

if [ -e  arch/arm/boot/zImage ];
then
rm arch/arm/boot/zImage #Just to make sure it doesn't make flashable zip with previous zImage
fi;

make harpia_defconfig
echo "Building with " $( nproc --all) " CPU(s)"
make -j$( nproc --all ) 2>&1 | tee $LOG_FILE

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
curl -H "Max-Downloads: 1" -H "Max-Days: 1" --upload-file $FINAL_ZIP https://transfer.sh/$FINAL_ZIP
else
echo "Kernel not compiled,fix errors and compile again"
fi;
