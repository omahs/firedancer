#!/bin/bash

FD_AUTO_INSTALL_PACKAGES=1
MACHINE=$1
shift

./deps.sh check install
make -j "$@"
