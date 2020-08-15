# Shenango

Shenango is a system that enables servers in datacenters to
simultaneously provide low tail latency and high CPU efficiency, by
rapidly reallocating cores across applications, at timescales as small
as every 5 microseconds.

## How to Run Shenango

1) Clone the Shenango repository.

```
git clone https://github.com/shenango/shenango
cd shenango
```

2) Setup DPDK, SPDK, and rdma-core.

```
./dpdk.sh
./spdk.sh
./rdma-core.sh
```

(3) Build the IOKernel, the Shenango runtime, and Ksched and perform some machine setup.
Before building, set the `CONFIG_*` parameters in shared.mk (e.g., `CONFIG_SPDK=y` to use
storage, `CONFIG_DIRECTPATH=y` to use directpath, and the MLX4 or MLX5 flags to use
directpath with MLX4 or MLX5, respectively).
```
make clean && make
pushd ksched
make clean & make
popd
./scripts/setup_machine.sh
```

To enable debugging, set `CONFIG_DEBUG=y` in shared.mk.

4) Install Rust and build a synthetic client-server application.

```
curl https://sh.rustup.rs -sSf | sh
rustup default nightly
```
```
cd apps/synthetic
cargo clean
cargo update
cargo build --release
```

5) Run the synthetic application with a client and server. The client
sends requests to the server, which performs a specified amount of
fake work (e.g., computing square roots for 10us), before responding.

On the server:
```
sudo ./iokerneld
./apps/synthetic/target/release/synthetic 192.168.1.3:5000 --config server.config --mode spawner-server
```

On the client:
```
sudo ./iokerneld
./apps/synthetic/target/release/synthetic 192.168.1.3:5000 --config client.config --mode runtime-client
```

## Supported Platforms

This code has been tested most thoroughly on Ubuntu 18.04, with kernel
5.2.0.

### NICs
This code has been tested with Intel 82599ES 10 Gbits/s NICs,
Mellanox ConnectX-3 Pro 10 Gbits/s NICs, and Mellanox Connect X-5 40 Gbits/s NICs.
If you use Mellanox NICs, you should install the Mellanox OFED as described in [DPDK's
documentation](https://doc.dpdk.org/guides/nics/mlx4.html). If you use
Intel NICs, you should insert the IGB UIO module and bind your NIC
interface to it (e.g., using the script `./dpdk/usertools/dpdk-setup.sh`). Directpath
is currently only supported with MLX4 and MLX5 NICs.

### Storage
This code has been tested with an Intel Optane SSD 900P Series NVMe device.