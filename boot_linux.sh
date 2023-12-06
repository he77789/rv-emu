#!/bin/bash

./main -d devicetree/singlehart.dtb -f fw_jump.bin -k Image -i images/rootfs.cpio $1
