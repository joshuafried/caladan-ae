#!/bin/bash

set -e
set -x

sudo rm /dev/ksched || true
sudo rmmod ksched || true

pushd ksched
make -j
sudo insmod build/ksched.ko

major=$(grep ksched /proc/devices | awk '{print $1}')

sudo mknod /dev/ksched c ${major} 0

popd
