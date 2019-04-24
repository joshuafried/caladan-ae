#!/bin/bash

set -e
set -x

git submodule update --init --recursive
pushd rdma-core
git cherry-pick f5dab6f85f747520d4fa9b8a8c92a988bd12e042 0701c9b4f523ecfcbab7749f2585d799a24bf486
./build.sh
popd
