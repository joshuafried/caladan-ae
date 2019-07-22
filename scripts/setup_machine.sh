#!/bin/bash
# run with sudo

# needed for the iokernel's shared memory
sysctl -w kernel.shm_rmid_forced=1
sysctl -w kernel.shmmax=18446744073692774399
sysctl -w vm.hugetlb_shm_group=27
sysctl -w vm.max_map_count=16777216
sysctl -w net.core.somaxconn=3072

# build ksched.ko
cd ksched && make && cd ..

# set up the ksched module
rmmod ksched
rm /dev/ksched
insmod $(dirname $0)/../ksched/build/ksched.ko
mknod /dev/ksched c 280 0
chmod uga+rwx /dev/ksched
rm /dev/pcicfg
mknod /dev/pcicfg c 281 0
chmod uga+rwx /dev/pcicfg

# reserve huge pages
echo 8192 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
for n in /sys/devices/system/node/node[1-9]; do
	echo 0 > $n/hugepages/hugepages-2048kB/nr_hugepages
done

# turn on cstate
sudo killall cstate
cd scripts
gcc cstate.c -o cstate
./cstate 0 &
cd ..

# Disable frequency scaling
echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Disable turbo boost
echo 1 | tee /sys/devices/system/cpu/intel_pstate/no_turbo
