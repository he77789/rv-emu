#!/bin/bash

{ spike -p1 -m512 --isa=rv64imac_zicsr_zifencei --dtb=devicetree/singlehart.dtb -l --kernel=Image fw_jump.elf 2>&1 >&3 3>&- \
 | grep -oP "((?<=core   0: 0x00000000)[0-9a-f]*)|((?<=core   0: 0x)ffffffff[0-9a-f]*)" > spike_pc_trace.txt 3>&-; } 3>&1
