#!/bin/bash

./main -p -c1 -d devicetree/singlehart.dtb -k Image -f fw_jump.elf 2>main_pc_trace.txt
