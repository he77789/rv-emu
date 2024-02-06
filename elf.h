#pragma once
#include <cstdint>
#include <cstddef>

size_t load_elf(int fd, uint64_t mem_offset);
