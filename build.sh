#!/bin/bash

set -e

if [ ! -f "Makefile" ]; then
    echo "Makefile not found!"
    exit 1
fi

make server
make client

echo "Successfully Compiled!"
