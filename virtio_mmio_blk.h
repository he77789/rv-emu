#pragma once

void virtio_mmio_blk_init();
void virtio_mmio_blk_uninit();

void virtio_mmio_blk_loop();

void* virtio_mmio_blk_r (uint64_t offset, uint8_t len);
void virtio_mmio_blk_w (uint64_t offset, void* dataptr, uint8_t len);

struct __attribute__ ((packed)) virtio_mmio_blk_queue {
  uint32_t type;
  uint32_t _res0[1];
  uint64_t sector;
  uint8_t data[512];
  uint8_t status;
};

struct __attribute__ ((packed)) virtio_mmio_blk_config {
  uint64_t capacity;
};
