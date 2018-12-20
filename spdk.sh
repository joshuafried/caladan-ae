patch -p 1 -d spdk < spdk.patch
cd spdk
git submodule update --init
scripts/pkgdep.sh
./configure
make
sudo scripts/setup.sh
