#!/bin/bash

NAME=arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi

wget "https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/$NAME.tar.xz"
tar xvf "$NAME.tar.xz"
mv "$NAME" gcc
rm "$NAME.tar.xz"
mkdir -p bin
touch .setup
