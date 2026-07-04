#!/bin/bash
# Copyright 2017 Google Inc. All Rights Reserved.
# Licensed under the Apache License, Version 2.0 (the "License");

PROFILE=false
while getopts "p" opt; do
  case $opt in
    p) PROFILE=true ;;
    *) echo "Usage: $0 [-p] [mode] [hooks_file]" ; exit 1 ;;
  esac
done
shift $((OPTIND -1))

export PROFILE


. $(dirname $0)/../custom-build.sh $1 $2
. $(dirname $0)/../common.sh

build_lib() {
  rm -rf BUILD
  cp -rf SRC BUILD
  (cd BUILD && autoreconf -fiv && ./configure --disable-shared && make -j $JOBS)
}

get_git_revision https://github.com/libjpeg-turbo/libjpeg-turbo.git b0971e47d76fdb81270e93bbf11ff5558073350d SRC


export CFLAGS="$CFLAGS -std=gnu89 -flto $LIBJPEG_FLAGS"
export CXXFLAGS="$CXXFLAGS -flto $LIBJPEG_FLAGS"

export AR=llvm-ar
export NM=llvm-nm
export RANLIB=llvm-ranlib

build_lib
build_fuzzer

if [[ $FUZZING_ENGINE == "hooks" ]]; then
  # Link ASan runtime so we can hook memcmp et al.
  LIB_FUZZING_ENGINE="$LIB_FUZZING_ENGINE -fsanitize=address"
fi
set -x

$CXX $CXXFLAGS -std=c++11 -I BUILD \
    -c $(dirname $0)/libjpeg_turbo_fuzzer.cc -o fuzzer_wrapper.o

PLUGIN_PATH="${PLUGIN_PATH:-/home/sas/Coding/Fuss/RemoveAsanFromFunc.so}"

LIBS="fuzzer_wrapper.o BUILD/.libs/libturbojpeg.a $LIB_FUZZING_ENGINE -lm"

if [ -f "$PLUGIN_PATH" ]; then
    echo "Using FUSS plugin at: $PLUGIN_PATH"
    
  FUSS_CUTOFF=1 $CXX $CXXFLAGS -std=c++11 -I BUILD \
      -fuse-ld=lld \
      -fpass-plugin=$PLUGIN_PATH \
      -Wl,--load-pass-plugin=$PLUGIN_PATH \
      $LIBS \
      $LIB_FUZZING_ENGINE -o $EXECUTABLE_NAME_BASE

else
    echo "Warning: Plugin not found at $PLUGIN_PATH. Building without FUSS."
    
    $CXX $CXXFLAGS \
        $LIBS -o "$EXECUTABLE_NAME_BASE"
fi