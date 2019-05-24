#!/bin/bash

git submodule update --init
patch -p 1 -d spdk < spdk.patch
cd spdk
./configure
make
sudo scripts/setup.sh
