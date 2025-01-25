#!/bin/bash

sudo apt-get update && sudo apt-get upgrade -y
sudo apt-get install -y gcc-9 g++-9 cmake zlib1g-dev libncurses5 libsnappy-dev libconfig++-dev

sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-9 100
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-9 100

wget -q http://releases.llvm.org/5.0.1/clang+llvm-5.0.1-x86_64-linux-gnu-ubuntu-14.04.tar.xz
tar -xf clang+llvm-5.0.1-x86_64-linux-gnu-ubuntu-14.04.tar.xz
sudo mkdir -p /usr/local/clang-5.0.1 && sudo mv clang+llvm-5.0.1-x86_64-linux-gnu-ubuntu-14.04/* /usr/local/clang-5.0.1
echo 'export PATH=/usr/local/clang-5.0.1/bin:$PATH' >> ~/.bashrc

git clone https://github.com/codverch/scarab.git

wget -q http://software.intel.com/sites/landingpage/pintool/downloads/pin-3.15-98253-gb56e429b1-gcc-linux.tar.gz
tar -xzf pin-3.15-98253-gb56e429b1-gcc-linux.tar.gz
mv pin-3.15-98253-gb56e429b1-gcc-linux scarab/src/pin-3.15

echo 'export PIN_ROOT=~/scarab/src/pin-3.15' >> ~/.bashrc
echo 'export SCARAB_ENABLE_PT_MEMTRACE=1' >> ~/.bashrc
source ~/.bashrc