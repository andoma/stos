#!/bin/bash

set -e

TOPDIR=`dirname $0`
STOSROOT=`cd ${TOPDIR} && pwd`

# ---- Misc helpers ----

function die() {
    echo "usage: $0 TARGET BUILDTYPE CMD"
    echo "  List of targets:"
    echo "    rpi"
    echo
    echo "  BUILDTYPE can be   debug   or   release"
    echo
    echo "  List of commands"
    echo "    kconfig           - Configure kernel"
    echo "    uconfig           - Configure buildroot"
    echo "    build             - Build"
    echo "    update_submodules - Sync and update submouldes"
    echo

    echo "$1"
    exit 1
}

# ---- Doozer helpers ----

#
# $1 = local file path
# $2 = type
# $3 = content-type
# $4 = filename
#


artifact() {
    echo "doozer-artifact:$PWD/$1:$2:$3:$4"
}

artifact_gzip() {
    echo "doozer-artifact-gzip:$PWD/$1:$2:$3:$4"
}

versioned_artifact() {
    echo "doozer-versioned-artifact:$PWD/$1:$2:$3:$4"
}

rpi_doozer_artifacts() {
    artifact_gzip output/${TARGET}/${TYPE}/sd.img             img  application/octet-stream sd.img
    artifact      output/${TARGET}/${TYPE}/boot/firmware.sqfs sqfs application/octet-stream firmware.sqfs
    artifact      output/${TARGET}/${TYPE}/boot/rootfs.sqfs   sqfs application/octet-stream rootfs.sqfs
    artifact      output/${TARGET}/${TYPE}/boot/modules.sqfs  sqfs application/octet-stream modules.sqfs
    artifact      output/${TARGET}/${TYPE}/boot/kernel.img    bin  application/octet-stream kernel.img
    artifact      output/${TARGET}/${TYPE}/boot/config.txt    txt  application/octet-stream config.txt
    artifact      output/${TARGET}/${TYPE}/boot/cmdline.txt   txt  application/octet-stream cmdline.txt
    artifact      output/${TARGET}/${TYPE}/boot/bootcode.bin  bin  application/octet-stream bootcode.bin
    artifact      output/${TARGET}/${TYPE}/boot/fixup_x.dat   bin  application/octet-stream fixup_x.dat
    artifact      output/${TARGET}/${TYPE}/boot/start_x.elf   bin  application/octet-stream start_x.elf
}

# ------------------------

JARGS=""

while getopts "j:" o; do
    case "${o}" in
        j)
	    JARGS="-j${OPTARG}"
            ;;
        *)
            usage
            ;;
    esac
done

shift $((OPTIND-1))

TARGET=$1
TYPE=$2
CMD=$3

export STOS_VERSION=`git describe --dirty --abbrev=5 2>/dev/null | sed  -e 's/-/./g'`

[ -z "${TARGET}" ] && die "No target specified"
[ -z "${TYPE}"   ] && die "No type specified"
[ -z "${CMD}"    ] && die "No command specified"

BUILDDIR="${STOSROOT}/output/${TARGET}/${TYPE}"

case "${TARGET}" in
    rpi)
	ARCH=arm
	UTC="${BUILDDIR}/buildroot/host/usr/bin/arm-buildroot-linux-gnueabihf-"
	KTC="${UTC}"
	DOOZER_ARTIFACTS=rpi_doozer_artifacts
	;;
    *)
	die "Unknown target"
	;;
esac

export KCONFIG_CONFIG="${STOSROOT}/config/kernel-${TARGET}-${TYPE}.config"
BR_CONFIG="${BUILDDIR}/buildroot/.config"

#===========================================================================
# What to do
#===========================================================================

case "${CMD}" in
    build)
	echo "Building ${STOS_VERSION} for ${TARGET}"
	;;

    update_submodules)
	git submodule sync
	git submodule update --init -f buildroot
	git submodule update --init -f mkfatimg
	git submodule update --init -f linux-firmware
	git submodule update --init -f linux-${TARGET}
	;;

    kconfig)
	make -C ${STOSROOT}/linux-${TARGET} O=${BUILDDIR}/kernel/ ARCH=${ARCH} CROSS_COMPILE=${KTC} menuconfig
	exit 0
	;;
    uconfig)

	mkdir -p "${BUILDDIR}/buildroot"
	cp "${STOSROOT}/config/buildroot-${TARGET}-${TYPE}.config" "${BR_CONFIG}"

	make -C ${STOSROOT}/buildroot O=${BUILDDIR}/buildroot/ menuconfig
	cp "${BR_CONFIG}" "${STOSROOT}/config/buildroot-${TARGET}-${TYPE}.config"
	exit 0
	;;
    doozer-artifacts)
	eval $DOOZER_ARTIFACTS
	exit 0
	;;
    info)
        echo "Userland toolchain      ${UTC}"
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
# Buildroot (Root filesystem)
#===========================================================================


mkdir -p "${BUILDDIR}/buildroot"
cp "${STOSROOT}/config/buildroot-${TARGET}-${TYPE}.config" "${BR_CONFIG}"

make -C ${STOSROOT}/buildroot O=${BUILDDIR}/buildroot/
cp "${BUILDDIR}/buildroot/images/rootfs.squashfs" "${BUILDDIR}/boot/rootfs.sqfs"

#===========================================================================
# Initrd for mounting sqfs as root
#===========================================================================

rm -rf "${BUILDDIR}/initrd"
mkdir -p "${BUILDDIR}/initrd"

${UTC}gcc -O2 -static -o "${BUILDDIR}/initrd/init" ${STOSROOT}/src/*.c

#===========================================================================
# Linux kernel
#===========================================================================

mkdir -p "${BUILDDIR}/kernel"

make -C ${STOSROOT}/linux-${TARGET} O=${BUILDDIR}/kernel/ ARCH=${ARCH} CROSS_COMPILE=${KTC} zImage ${JARGS}

cp "${BUILDDIR}/kernel/arch/arm/boot/zImage" "${BUILDDIR}/boot/kernel.img"

#===========================================================================
# Linux kernel modules
#===========================================================================

make -C ${STOSROOT}/linux-${TARGET} O=${BUILDDIR}/kernel/ ARCH=${ARCH} CROSS_COMPILE=${KTC} modules ${JARGS}
rm -rf "${BUILDDIR}/lib/modules"
make -C ${STOSROOT}/linux-${TARGET} O=${BUILDDIR}/kernel/ ARCH=${ARCH} CROSS_COMPILE=${KTC} INSTALL_MOD_PATH=${BUILDDIR} modules_install ${JARGS}
mksquashfs "${BUILDDIR}/lib/modules" "${BUILDDIR}/boot/modules.sqfs" -comp xz

#===========================================================================
# Linux firmware
#===========================================================================

mksquashfs "${STOSROOT}/linux-firmware" "${BUILDDIR}/boot/firmware.sqfs" -comp xz -wildcards -ef "${STOSROOT}/exclude.txt"

#===========================================================================
# Showtime release
#===========================================================================

DLINFO=`curl -L https://showtimemediacenter.com/upgrade/1/stable-${TARGET}.json | python -c 'import json,sys;obj=json.load(sys.stdin);v= [x for x in obj["artifacts"] if x["type"] == "sqfs"][0]; print "%s %s" % (v["url"],obj["version"])'`

echo "Using Showtime: $DLINFO"

DLURL=`echo "$DLINFO" | cut -d" " -f1`

wget -O "${BUILDDIR}/boot/showtime.sqfs" $DLURL


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
