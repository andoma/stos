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
    echo "    uconfig           - Configure buildroot"
    echo "    build             - Build"
    echo "    update_submodules - Sync and update submouldes"
    echo "    info              - Show some paths"
    echo "    get-toolchain     - Get toolchain + sysroot as .tar.gz"
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

artifact_sel() {
    echo "doozer-artifact:$PWD/$1:$2:$3:$4:$5"
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
    artifact_sel  output/${TARGET}/${TYPE}/boot/modules.sqfs  sqfs application/octet-stream modules.sqfs "machine=armv6l"
    artifact_sel  output/${TARGET}/${TYPE}/boot/modules_armv7l.sqfs  sqfs application/octet-stream modules_armv7l.sqfs "machine=armv7l"
    artifact_sel  output/${TARGET}/${TYPE}/boot/kernel.img    bin  application/octet-stream kernel.img  "machine=armv6l"
    artifact_sel  output/${TARGET}/${TYPE}/boot/kernel7.img    bin  application/octet-stream kernel7.img  "machine=armv7l"
    artifact      output/${TARGET}/${TYPE}/boot/config.txt    txt  application/octet-stream config.txt
    artifact      output/${TARGET}/${TYPE}/boot/cmdline.txt   txt  application/octet-stream cmdline.txt
    artifact      output/${TARGET}/${TYPE}/boot/bootcode.bin  bin  application/octet-stream bootcode.bin
    artifact      output/${TARGET}/${TYPE}/boot/fixup_x.dat   bin  application/octet-stream fixup_x.dat
    artifact      output/${TARGET}/${TYPE}/boot/start_x.elf   bin  application/octet-stream start_x.elf

    artifact_sel  output/${TARGET}/${TYPE}/boot/bcm2709-rpi-2-b.dtb    bin  application/octet-stream bcm2709-rpi-2-b.dtb    "machine=armv7l"
    artifact_sel  output/${TARGET}/${TYPE}/boot/bcm2710-rpi-3-b.dtb    bin  application/octet-stream bcm2710-rpi-3-b.dtb    "machine=armv7l"
    artifact_sel  output/${TARGET}/${TYPE}/boot/bcm2835-rpi-b.dtb      bin  application/octet-stream bcm2835-rpi-b.dtb      "machine=armv6l"
    artifact_sel  output/${TARGET}/${TYPE}/boot/bcm2835-rpi-b-plus.dtb bin  application/octet-stream bcm2835-rpi-b-plus.dtb "machine=armv6l"
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
	HOSTDIR="${BUILDDIR}/buildroot/host"
	UTC="${BUILDDIR}/buildroot/host/usr/bin/arm-buildroot-linux-gnueabihf-"
	SYSROOT="${BUILDDIR}/buildroot/host/usr/arm-buildroot-linux-gnueabihf/sysroot"
	DOOZER_ARTIFACTS=rpi_doozer_artifacts
	;;
    *)
	die "Unknown target"
	;;
esac

BR_CONFIG="${BUILDDIR}/buildroot/.config"

#===========================================================================
# What to do
#===========================================================================

case "${CMD}" in
    build)
	echo "Building ${STOS_VERSION} for ${TARGET}"
	;;

    update_submodules)
        echo "Git submodule status before sync"
        git submodule status

	git submodule sync

        echo "Git submodule status after sync"
        git submodule status

	git submodule update --init -f buildroot
	git submodule update --init -f mkfatimg
	git submodule update --init -f linux-firmware
	git submodule update --init -f musl-kernel-headers
	git submodule update --init -f ${KSRC}

        echo "Git submodule status after update"
        git submodule status
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
        echo "Sysroot                 ${SYSROOT}"
        exit 0
        ;;
    get-toolchain)
        TOOLCHAINOUT="/tmp/stos-host-${STOS_VERSION}.tar.bz2"
        echo "Packing toolchain from ${HOSTDIR} to ${TOOLCHAINOUT}"
        tar -cj -C "${HOSTDIR}/.." -f "${TOOLCHAINOUT}" host
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

make -C init O="${BUILDDIR}/init" ARCH=arm CROSS_COMPILE="${UTC}" KHEADERS="${STOSROOT}/musl-kernel-headers" MODE=stos INITRD="${BUILDDIR}/initrd" install


#===========================================================================
# Linux kernel
#===========================================================================


case "${TARGET}" in
    rpi)

        # For rpi we will build two kernels

        # rpi2 (kernel7)
        export KCONFIG_CONFIG="${STOSROOT}/config/kernel-rpi2-${TYPE}.config"
        KSRC="${STOSROOT}/linux-rpi"
        mkdir -p "${BUILDDIR}/kernel7"
        make -C ${KSRC} O=${BUILDDIR}/kernel7/ ARCH=arm CROSS_COMPILE=${UTC} zImage dtbs ${JARGS}

        "${KSRC}/scripts/mkknlimg" "${BUILDDIR}/kernel7/arch/arm/boot/zImage" "${BUILDDIR}/boot/kernel7.img"
        make -C ${KSRC} O=${BUILDDIR}/kernel7/ ARCH=arm CROSS_COMPILE=${UTC} modules ${JARGS}
        rm -rf "${BUILDDIR}/modinst7/lib/modules"
        make -C ${KSRC} O=${BUILDDIR}/kernel7/ ARCH=arm CROSS_COMPILE=${UTC} INSTALL_MOD_PATH=${BUILDDIR}/modinst7 modules_install ${JARGS}
        mksquashfs "${BUILDDIR}/modinst7/lib/modules" "${BUILDDIR}/boot/modules_armv7l.sqfs" -comp xz

        # rpi1
        export KCONFIG_CONFIG="${STOSROOT}/config/kernel-rpi-${TYPE}.config"
        KSRC="${STOSROOT}/linux-rpi"
        mkdir -p "${BUILDDIR}/kernel"
        make -C ${KSRC} O=${BUILDDIR}/kernel/ ARCH=arm CROSS_COMPILE=${UTC} ${JARGS} zImage dtbs

        "${KSRC}/scripts/mkknlimg" "${BUILDDIR}/kernel/arch/arm/boot/zImage" "${BUILDDIR}/boot/kernel.img"

        make -C ${KSRC} O=${BUILDDIR}/kernel/ ARCH=arm CROSS_COMPILE=${UTC} modules ${JARGS}
        rm -rf "${BUILDDIR}/modinst/lib/modules"
        make -C ${KSRC} O=${BUILDDIR}/kernel/ ARCH=arm CROSS_COMPILE=${UTC} INSTALL_MOD_PATH=${BUILDDIR}/modinst modules_install ${JARGS}
        mksquashfs "${BUILDDIR}/modinst/lib/modules" "${BUILDDIR}/boot/modules.sqfs" -comp xz


	;;
    *)
	die "Unknown target for kernel build"
	;;
esac




#===========================================================================
# Linux firmware
#===========================================================================

rm -rf "${BUILDDIR}/firmware"
mkdir -p "${BUILDDIR}/firmware"
cp -r ${STOSROOT}/linux-firmware/* "${BUILDDIR}/firmware/"
cp -r ${STOSROOT}/rpi-firmware/* "${BUILDDIR}/firmware/"
rm -f "${BUILDDIR}/boot/firmware.sqfs"

mksquashfs "${BUILDDIR}/firmware" "${BUILDDIR}/boot/firmware.sqfs" -comp xz -wildcards -ef "${STOSROOT}/exclude.txt"

#===========================================================================
# Showtime release
#===========================================================================

DLINFO=`curl -L http://upgrade.movian.tv/upgrade/2/testing-${TARGET}.json | python -c 'import json,sys;obj=json.load(sys.stdin);v= [x for x in obj["artifacts"] if x["type"] == "sqfs"][0]; print "%s %s" % (v["url"],obj["version"])'`

echo "Using Movian: $DLINFO"

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
