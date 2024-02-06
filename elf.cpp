#include "elf.h"
#include <cstddef>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

#include <gelf.h>

#include "cpu.h"
#include "mem.h"

size_t load_elf(int fd, uint64_t mem_offset = 0) {
  Elf* e;
  size_t file_size = 0;
  
  // init libelf
  if (elf_version(EV_CURRENT) == EV_NONE) {
    // init failed
    return 0;
  }
  
  e = elf_begin(fd, ELF_C_READ, NULL);
  if (!e) {
    // can't open ELF, fallback to treating file as flat binary
    return read(fd, main_mem + mem_offset, 0x200'0000);
  }
  
  if (elf_kind(e) != ELF_K_ELF) {
    // ELF has wrong format, fallback to treating file as flat binary
    return read(fd, main_mem + mem_offset, 0x200'0000);
  }
  
  size_t n = 0;
  GElf_Phdr phdr;
  elf_getphdrnum(e, &n);
  for (size_t i = 0; i < n; i++) {
    gelf_getphdr(e, i, &phdr);
    if (phdr.p_type != PT_LOAD) continue;
    if (phdr.p_paddr < 0x8000'0000) continue;
    uint64_t load_addr = phdr.p_paddr + mem_offset - 0x8000'0000;
    ssize_t loaded_size = pread(fd, main_mem + load_addr, phdr.p_memsz, phdr.p_offset);
    if (loaded_size == -1) {
      return 0;
    }
    file_size += loaded_size;
  }
  
  if (sig_mode) {
    // find the begin_signature and end_signature symbols and save them
    Elf_Scn* scn = nullptr;
    GElf_Shdr shdr;
    bool not_found = false;
    do {
      scn = elf_nextscn(e, scn);
      if (!gelf_getshdr(scn, &shdr)) {
        not_found = true;
        break;
      }
    } while (shdr.sh_type != SHT_SYMTAB);
    
    if (!not_found) {
      Elf_Data* data = elf_getdata(scn, 0);
      size_t count = shdr.sh_size / shdr.sh_entsize;
      
      for (size_t i = 0; i < count; i++) {
        GElf_Sym symbol;
        gelf_getsym(data, i, &symbol);
        char* symbol_name = elf_strptr(e, shdr.sh_link, symbol.st_name);
        if (strcmp(symbol_name, "begin_signature") == 0) {
          begin_signature = symbol.st_value;
        } else if (strcmp(symbol_name, "end_signature") == 0) {
          end_signature = symbol.st_value;
        }
      }
    }
  }
  
  elf_end(e);
  return file_size;
}
