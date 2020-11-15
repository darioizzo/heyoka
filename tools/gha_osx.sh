#!/usr/bin/env bash

# Echo each command
set -x

# Exit on error.
set -e

# Install conda+deps.
wget https://repo.anaconda.com/miniconda/Miniconda3-latest-MacOSX-x86_64.sh -O miniconda.sh
export deps_dir=$HOME/local
export PATH="$HOME/miniconda/bin:$PATH"
bash miniconda.sh -b -p $HOME/miniconda
conda config --add channels conda-forge
conda config --set channel_priority strict
conda_pkgs="clang clangxx libcxx cmake llvmdev boost-cpp sleef xtensor xtensor-blas"
conda create -q -p $deps_dir -y
source activate $deps_dir
conda install mamba -y
mamba install $conda_pkgs -y

# Create the build dir and cd into it.
mkdir build
cd build

# GCC build.
CXX=clang++ CC=clang cmake ../ -DCMAKE_PREFIX_PATH=$deps_dir -DCMAKE_BUILD_TYPE=Debug -DHEYOKA_BUILD_TESTS=yes -DHEYOKA_WITH_SLEEF=yes -DHEYOKA_ENABLE_IPO=yes -DBoost_NO_BOOST_CMAKE=ON
make -j2 VERBOSE=1
ctest -V -j2

set +e
set +x
