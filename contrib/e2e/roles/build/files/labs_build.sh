#!/bin/bash

PROFILE=$1
shift

source ~/.cargo/env

cargo build $PROFILE "$@"
