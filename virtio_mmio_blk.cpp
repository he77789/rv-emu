#include <thread>
#include <mutex>
#include <condition_variable>

#include "constants.h"
#include "virtio_common.h"
#include "virtio_mmio_blk.h"

std::thread blk_thread;

std::mutex flags_mtx;
std::condition_variable flags_cv;
bool needs_io = false;
bool blk_end = false;

const uint32_t devfeatsupp = 0b0;
const uint32_t devfeathisupp = 0b1; // VIRTIO_F_VERSION_1

virtio_dev_cfg virtio_mmio_blk_devcfg;
// VIRTIO_BLK_F_MQ is not supported for now
virtqueue virtio_mmio_blk_queues[1];
virtio_mmio_blk_config vblkcfg;

uint32_t configgen = 0;

// virtio disabled for now
void virtio_mmio_blk_init() {
  //blk_thread = std::thread(virtio_mmio_blk_loop);
  virtio_mmio_blk_devcfg.deviceid = 2;
  virtio_mmio_blk_devcfg.vendorid = 0x554d4551; // qemu vendor...?
  virtio_mmio_blk_devcfg.devfeat = devfeatsupp;
}
void virtio_mmio_blk_uninit() {
  blk_end = true;
  flags_cv.notify_all();
  //blk_thread.join();
}

void virtio_mmio_blk_loop() {
  std::unique_lock<std::mutex> flags_lock (flags_mtx);
  while (true) {
    flags_cv.wait(flags_lock, []{return needs_io || blk_end;});
    if (blk_end) break;
    ; // TODO: handle IO requests
  }
}

// struct access by offset
#define SAO(st,off) *((uint32_t*)&st + off / sizeof(uint32_t))

void* virtio_mmio_blk_r (uint64_t offset, [[maybe_unused]] uint8_t len) {
  // device config
  if (offset <= 0x30){
    if (offset == 0x010) offset += 2 * sizeof(uint32_t) * virtio_mmio_blk_devcfg.devfeatsel;
    if (offset == 0x020) offset += 2 * sizeof(uint32_t) * virtio_mmio_blk_devcfg.drifeatsel;
    return &SAO(virtio_mmio_blk_devcfg,offset);
  }
  
  // interrupts and status
  if (0x50 <= offset && offset <= 0x70) {
    return &SAO(virtio_mmio_blk_devcfg,offset);
  }
  
  // queue config
  if (0x34 <= offset && offset <= 0xa4) {
    return (uint32_t*)(&(virtio_mmio_blk_queues[virtio_mmio_blk_devcfg.queuesel])) + (offset - 0x34) / sizeof(uint32_t);
  }
  
  // configgen
  if (offset == 0xfc) {
    return &configgen;
  }
  
  // device-specific config
  if (offset >= 0x100) {
    return (uint32_t*)&vblkcfg + (offset - 0x100) / sizeof(uint32_t);
  }
  return &ZERO;
}
void virtio_mmio_blk_w (uint64_t offset, void* dataptr, [[maybe_unused]] uint8_t len) {
  // device config
  if (offset <= 0x30){
    if (offset == 0x010) offset += 2 * sizeof(uint32_t) * virtio_mmio_blk_devcfg.devfeatsel;
    if (offset == 0x020) offset += 2 * sizeof(uint32_t) * virtio_mmio_blk_devcfg.drifeatsel;
    SAO(virtio_mmio_blk_devcfg, offset) = *((uint32_t*)dataptr);
  }
  
  // interrupts and status
  if (0x50 <= offset && offset <= 0x70) {
    SAO(virtio_mmio_blk_devcfg, offset) = *((uint32_t*)dataptr);
  }
  
  // queue config
  if (0x34 <= offset && offset <= 0xa4) {
    SAO(virtio_mmio_blk_queues[virtio_mmio_blk_devcfg.queuesel],offset - 0x34) = *((uint32_t*)dataptr);
  }
  
  // configgen is read-only
  
  // device-specific config
  if (offset >= 0x100) {
    SAO(vblkcfg,offset - 0x100) = *((uint32_t*)dataptr);
  }
  
  // TODO:handle specific fields
  
}
