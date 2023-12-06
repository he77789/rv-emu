#pragma once

#include <cstdint>

struct __attribute__ ((packed)) virtio_dev_cfg {
  uint32_t magic = 0x74726976;
  uint32_t version = 0x2;
  uint32_t deviceid = 0;
  uint32_t vendorid = 0;
  uint32_t devfeat;
  uint32_t devfeatsel;
  uint32_t devfeathi;
  uint32_t _res0[1]; // offset 0x1c
  uint32_t drifeat;
  uint32_t drifeatsel;
  uint32_t drifeathi;
  uint32_t _res1[1]; // offset 0x2c
  uint32_t queuesel;

  uint32_t _res2[41]; // offset 0x34-0x5c
  uint32_t intstatus;
  uint32_t intack;
  uint32_t _res3[2]; // offset 0x68-0x6c
  uint32_t status;
};

struct __attribute__ ((packed)) virtqueue {
  uint32_t queuesel;
  uint32_t queuenummax;
  uint32_t queuenum;
  uint32_t _res0[2]; // offset 0x3c-0x40
  uint32_t queueready;
  uint32_t _res1[2]; // offset 0x48-0x4c
  uint32_t queuenotify;
  
  uint32_t _res2[41]; // offset 0x54-0x7c
  
  // the following are two uint32_t packed together
  
  uint64_t queuedesc;
  uint32_t _res3[1]; // offset 0x8c
  uint64_t queuedri;
  uint32_t _res4[1]; // offset 0x9c
  uint64_t queuedev;
};
