#!/usr/bin/env bash

# Echo each command
set -x

# Exit on error.
set -e

sudo apt-get install build-essential libgmp-dev libmpfr-dev

set +e
set +x