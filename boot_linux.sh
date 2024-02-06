#!/bin/bash

./main -d devicetree/singlehart.dtb -f fw_jump.elf -k Image -i images/rootfs.cpio $1
