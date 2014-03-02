#!/bin/bash
#
# Entry point for the Doozer autobuild system
#
# (c) Andreas Öman 2013. All rights reserved.
#
#

set -eu

BUILD_API_VERSION=3
EXTRA_BUILD_NAME=""
JARGS=""
TARGET=""
WORKINGDIR="/var/tmp/stos-autobuild"
OP="build"
while getopts "vht:e:j:w:o:c:" OPTION
do
  case $OPTION in
      v)
	  echo $BUILD_API_VERSION
	  exit 0
	  ;;
      h)
	  echo "This script is intended to be used by the autobuild system only"
	  exit 0
	  ;;
      t)
	  TARGET="$OPTARG"
	  ;;
      e)
	  EXTRA_BUILD_NAME="$OPTARG"
	  ;;
      j)
	  JARGS="-j$OPTARG"
	  ;;
      w)
	  WORKINGDIR="$OPTARG"
	  ;;
      o)
	  OP="$OPTARG"
	  ;;
  esac
done

if [[ -z $TARGET ]]; then
    echo "target (-t) not specified"
    exit 1
fi


TYPE=release

build()
{
    rm -rf output
    ./build.sh ${JARGS} ${TARGET} ${TYPE} update_submodules
    ./build.sh ${JARGS} ${TARGET} ${TYPE} build
    ./build.sh ${TARGET} ${TYPE} doozer-artifacts
}

deps()
{
    echo No deps support right now
}

eval $OP
