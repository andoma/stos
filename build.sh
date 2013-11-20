#!/bin/bash

set -e

TARGET=$1
TYPE=$2
CMD=$3
TOPDIR=`dirname $0`
STOSROOT=`cd ${TOPDIR} && pwd`


function die() {
    echo "$1"
    exit 1
}

[ -z "${TARGET}" ] && die "No target specified"
[ -z "${TYPE}"   ] && die "No type specified"
[ -z "${CMD}"    ] && die "No command specified"

BUILDDIR="${STOSROOT}/output/${TARGET}/${TYPE}"

case "${TARGET}" in
    rpi)
	ARCH=arm
	KTC=/usr/bin/arm-linux-gnueabi-
	UTC="${BUILDDIR}/buildroot/host/usr/bin/arm-unknown-linux-gnueabi-"
	TOOLCHAIN_URL=http://www.lonelycoder.com/download/arm-unknown-linux-gnueabi.tar.gz
	TOOLCHAIN_DIR="${BUILDDIR}/arm-unknown-linux-gnueabi"
	;;
    *)
	die "Unknown target"
	;;
esac

export KCONFIG_CONFIG="${STOSROOT}/config/kernel-${TARGET}-${TYPE}.config"


#===========================================================================
# What to do
#===========================================================================

case "${CMD}" in
    build)
	echo "Building for Raspberry Pi"
	;;
    kconfig)
	make -C ${STOSROOT}/linux O=${BUILDDIR}/kernel/ ARCH=${ARCH} CROSS_COMPILE=${KTC} menuconfig
	exit 0
	;;
    *)
	die "Unknown target"
	;;
esac


export BUILDDIR
rm -rf "${BUILDDIR}/boot"
mkdir -p "${BUILDDIR}/boot"

#===========================================================================
# Download toolchain
#===========================================================================

echo "Toolchain from: '${TOOLCHAIN_URL}' Local install in: ${TOOLCHAIN_DIR}"
if [ -d "${TOOLCHAIN_DIR}" ]; then
    echo "Toolchain seems to exist"
else
    curl -L "${TOOLCHAIN_URL}" | tar xfz - -C ${BUILDDIR}
	
    STATUS=$?
    if [ $STATUS -ne 0 ]; then
	echo "Unable to stage toolchain"
	exit 1
    fi
fi

#===========================================================================
# Buildroot (Root filesystem)
#===========================================================================

BR_CONFIG="${BUILDDIR}/buildroot/.config"

export STOS_TOOLCHAIN_DIR=${TOOLCHAIN_DIR}

mkdir -p "${BUILDDIR}/buildroot"
cp "${STOSROOT}/config/buildroot-${TARGET}-${TYPE}.config" "${BR_CONFIG}"

make -C ${STOSROOT}/buildroot O=${BUILDDIR}/buildroot/
cp "${BUILDDIR}/buildroot/images/rootfs.squashfs" "${BUILDDIR}/boot/rootfs.sqfs"

#===========================================================================
# Initrd for mounting sqfs as root
#===========================================================================

rm -rf "${BUILDDIR}/initrd"
mkdir -p "${BUILDDIR}/initrd"

${UTC}gcc -O2 -static -o "${BUILDDIR}/initrd/init" ${STOSROOT}/src/init.c

#===========================================================================
# Linux kernel
#===========================================================================

mkdir -p "${BUILDDIR}/kernel"

make -C ${STOSROOT}/linux O=${BUILDDIR}/kernel/ ARCH=${ARCH} CROSS_COMPILE=${KTC} zImage -j5

cp "${BUILDDIR}/kernel/arch/arm/boot/zImage" "${BUILDDIR}/boot/kernel.img"

#===========================================================================
# Linux kernel modules
#===========================================================================

make -C ${STOSROOT}/linux O=${BUILDDIR}/kernel/ ARCH=${ARCH} CROSS_COMPILE=${KTC} modules -j5
rm -rf "${BUILDDIR}/lib/modules"
make -C ${STOSROOT}/linux O=${BUILDDIR}/kernel/ ARCH=${ARCH} CROSS_COMPILE=${KTC} INSTALL_MOD_PATH=${BUILDDIR} modules_install -j5
mksquashfs "${BUILDDIR}/lib/modules" "${BUILDDIR}/boot/modules.sqfs" -comp xz

#===========================================================================
# Linux firmware
#===========================================================================

mksquashfs "${STOSROOT}/linux-firmware" "${BUILDDIR}/boot/firmware.sqfs" -comp xz -wildcards -ef "${STOSROOT}/exclude.txt"

#===========================================================================
# Showtime release
#===========================================================================

wget -O "${BUILDDIR}/boot/showtime.sqfs" http://pam.lonelycoder.com/file/92f93616971bc164028c7d91d695296c082b942b

#===========================================================================
# Other stuff
#===========================================================================

cp ${STOSROOT}/extra/${TARGET}/* "${BUILDDIR}/boot/"

#===========================================================================
# Create SD image
#===========================================================================

make -C ${STOSROOT}/mkfatimg all
${STOSROOT}/mkfatimg/build/mkfatimg -o "${BUILDDIR}/sd.img" -f "${BUILDDIR}/boot"
gzip -9 -c <"${BUILDDIR}/sd.img" >"${BUILDDIR}/sd.img.gz"

echo
echo All done
echo
ls -l "${BUILDDIR}/sd.img"
ls -l "${BUILDDIR}/sd.img.gz"
