#!/bin/bash

spike $1 \
  --kernel=Image --dtb=devicetree/singlehart.dtb -p1 -m512 \
  fw_jump.elf

