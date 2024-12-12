#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu.h"
#include "utilities.h"
#include "memory.h"
#include "kvm.h"
#include "mapping.h"

/* Notes:
 *   1.  leavedos() needs a real parameter
 */

/*
 * The point of these procedures is going to be to create a map of the
 * available memory to facilitate:
 *
 * 1.  check for memory conflicts (hardware page/ems page frame/etc.)
 * 2.  facilitate searching for frames (EMS frame, UMB blocks, etc.)
 */

#define GRAN_SIZE               1024    /* Size of granularity in KB   */
#define MAX_PAGE (LOWMEM_SIZE/GRAN_SIZE)   /* Number of 'pages' in memory */

static unsigned char mem_map[MAX_PAGE];          /* Map of memory contents      */
static const char *mem_names[256];             /* List of id. strings         */

struct system_memory_map {
  Bit32u base, hibase, length, hilength, type;
};

struct system_memory_map *system_memory_map;
size_t system_memory_map_size;

static inline void round_addr(dosaddr_t *addr)
{
  *addr = (*addr + GRAN_SIZE - 1) / GRAN_SIZE;
  *addr *= GRAN_SIZE;
}

int memcheck_addtype(unsigned char map_char, const char *name)
{
  if (mem_names[map_char] != NULL) {
    if (strcmp(mem_names[map_char], name) != 0) {
      error("CONF: memcheck, conflicting map type '%c' defined for '%s' \
& '%s'\n", map_char, mem_names[map_char], name);
      config.exitearly = 1;
    }
    else
      c_printf("CONF: memcheck, map type '%c' re-defined for '%s'\n",
	       map_char, name);
  }
  else {
    mem_names[map_char] = name;
  }
  return(0);
}

int memcheck_map_reserve(unsigned char map_char, dosaddr_t addr_start,
    uint32_t size)
{
  int cntr;
  dosaddr_t addr_end;

  c_printf("CONF: reserving %uKb at 0x%5.5X for '%c' (%s)\n", size/1024,
	   addr_start, map_char, mem_names[map_char]);

  addr_end = addr_start + size;
  round_addr(&addr_end);

  for (cntr = addr_start / GRAN_SIZE;
       cntr < addr_end / GRAN_SIZE && cntr < MAX_PAGE; cntr++) {
    if (mem_map[cntr]) {
      if (cntr != addr_start / GRAN_SIZE) {
	error("CONF: memcheck - Fatal error.  Memory conflict!\n");
	error("    Memory at 0x%4.4X:0x0000 is mapped to both:\n",
		(cntr * GRAN_SIZE) / 16);
	error("    '%s' & '%s'\n", mem_names[map_char],
		mem_names[mem_map[cntr]]);
	memcheck_dump();
	config.exitearly = 1;
	return -2;
      }
      if (mem_map[cntr] == map_char) {
	dosemu_error("The memory type '%s' has "
		"been mapped twice to the same location (0x%X)\n",
		mem_names[map_char], addr_start);
	return -2;
      }
      return -1;
    } else {
      mem_map[cntr] = map_char;
    }
  }

  return 0;
}

void memcheck_e820_reserve(dosaddr_t addr_start, uint32_t size, int reserved)
{
  struct system_memory_map *entry;
  int len = system_memory_map_size / sizeof(*system_memory_map);
  int i;

  system_memory_map_size += sizeof(*system_memory_map);
  system_memory_map = realloc(system_memory_map, system_memory_map_size);
  for (i = 0; i < len; i++) {
    if (addr_start < system_memory_map[i].base)
      break;
  }
  if (i < len)
    memmove(&system_memory_map[i + 1], &system_memory_map[i],
        sizeof(*system_memory_map) * (len - i));
  entry = system_memory_map + i;
  entry->base = addr_start;
  entry->hibase = 0;
  entry->length = size;
  entry->hilength = 0;
  entry->type = reserved + 1;
}

void memcheck_reserve(unsigned char map_char, dosaddr_t addr_start,
    uint32_t size)
{
  int err;

  err = memcheck_map_reserve(map_char, addr_start, size);
  if (err) {
    if (err == -1) {
      dosemu_error("CONF: memcheck - Fatal error.  Memory conflict! %c\n",
          map_char);
      config.exitearly = 1;
    }
    return;
  }
  /* 'r' is dosemu heap. It is normally reserved, but fdpp can drill
   * the UMB holes in it, so mark as available. */
  memcheck_e820_reserve(addr_start, size,
      !(map_char == 'd' || map_char == 'U' || map_char == 'X' ||
        map_char == 'H' || map_char == 'r'));

  if (memcheck_is_rom(addr_start))
    mprotect_mapping(MAPPING_LOWMEM, addr_start, size, PROT_READ | _PROT_EXEC);
}

void memcheck_map_free(unsigned char map_char)
{
  int i;

  c_printf("CONF: freeing region for '%c' (%s)\n",
	   map_char, mem_names[map_char]);

  for (i = 0; i < MAX_PAGE; i++) {
    if (mem_map[i] == map_char)
      mem_map[i] = 0;
  }
}

void memcheck_type_init(void)
{
  static int once = 0;
  if (once) return;
  once = 1;
  memcheck_addtype('d', "Base DOS memory (first 640K)");
  memcheck_addtype('r', "Dosemu reserved area");
  memcheck_addtype('b', "BIOS");
  memcheck_addtype('h', "Direct-mapped hardware page frame");
  memcheck_addtype('v', "Video memory");
}

void memcheck_init(void)
{
  memcheck_type_init();
  memcheck_reserve('d', 0x00000, config.mem_size*1024); /* dos memory  */
  memcheck_reserve('r', 0xF0000 + DOSEMU_LMHEAP_OFF, DOSEMU_LMHEAP_SIZE);
  assert(DOSEMU_LMHEAP_OFF + DOSEMU_LMHEAP_SIZE == bios_data_start);
  /* dosemu bios */
  memcheck_reserve('b', 0xF0000 + bios_data_start, DOSEMU_BIOS_SIZE());
}

int memcheck_isfree(dosaddr_t addr_start, uint32_t size)
{
  int cntr;
  dosaddr_t addr_end;

  addr_end = addr_start + size;
  round_addr(&addr_end);

  for (cntr = addr_start / GRAN_SIZE; cntr < addr_end / GRAN_SIZE; cntr++) {
    if (mem_map[cntr])
      return FALSE;
  }
  return TRUE;
}

int memcheck_is_reserved(dosaddr_t addr_start, uint32_t size,
	unsigned char map_char)
{
  int cntr;
  dosaddr_t addr_end;

  addr_end = addr_start + size;
  round_addr(&addr_end);

  for (cntr = addr_start / GRAN_SIZE; cntr < addr_end / GRAN_SIZE; cntr++) {
    if (mem_map[cntr] != map_char) {
      error("memcheck type mismatch at 0x%x: %c %c\n",
	    cntr * GRAN_SIZE, mem_map[cntr], map_char);
      return FALSE;
    }
  }
  return TRUE;
}

int memcheck_is_rom(dosaddr_t addr)
{
  if (addr >= LOWMEM_SIZE)
    return 0;
  return mem_map[addr / GRAN_SIZE] == 'R';
}

int memcheck_is_hardware_ram(dosaddr_t addr)
{
  if (addr >= LOWMEM_SIZE)
    return 0;
  switch (mem_map[addr / GRAN_SIZE]) {
  case 'e':
  case 'v':
  case 'h':
    return 1;
  }
  return 0;
}

int memcheck_is_system_ram(dosaddr_t addr)
{
  return !memcheck_is_rom(addr) &&
    !memcheck_is_hardware_ram(addr);
}

int memcheck_findhole(dosaddr_t *start_addr, uint32_t min_size,
    uint32_t max_size)
{
  int cntr;

  for (cntr = *start_addr/GRAN_SIZE; cntr < MAX_PAGE; cntr++) {
    int cntr2, end_page;

    /* any chance of finding anything? */
    if ((MAX_PAGE - cntr) * GRAN_SIZE < min_size)
      return 0;

    /* if something's already there, no go */
    if (mem_map[cntr])
      continue;

    end_page = cntr + (max_size / GRAN_SIZE);
    if (end_page > MAX_PAGE)
      end_page = MAX_PAGE;

    for (cntr2 = cntr+1; cntr2 < end_page; cntr2++) {
      if (mem_map[cntr2]) {
	if ((cntr2 - cntr) * GRAN_SIZE >= min_size) {
	  *start_addr = cntr * GRAN_SIZE;
	  return ((cntr2 - cntr) * GRAN_SIZE);
	} else {
	  /* hole isn't big enough, skip to the next one */
	  cntr = cntr2;
	  break;
	}
      }
    }
  }
  return 0;
}

void memcheck_dump(void)
{
  int cntr;
  c_printf("CONF:  Memory map dump:\n");
  for (cntr = 0; cntr < MAX_PAGE; cntr++) {
    if (cntr % 64 == 0)
      c_printf("0x%5.5X:  ", cntr * GRAN_SIZE);
    c_printf("%c", (mem_map[cntr]) ? mem_map[cntr] : '.');
    if (cntr % 64 == 63)
      c_printf("\n");
  }
  c_printf("\nKey:\n");
  for (cntr = 0; cntr < 256; cntr++) {
    if (mem_names[cntr])
      c_printf("%c:  %s\n", cntr, mem_names[cntr]);
  }
  c_printf(".:  (unused)\n");
  c_printf("CONF:  End dump\n");
}

#if 0
void *lowmemp(const unsigned char *ptr)
{
  dosaddr_t addr = DOSADDR_REL(ptr);
#ifdef __x86_64__
  if (addr > 0xffffffff)
    return (void *)ptr;
#endif
  return dosaddr_to_unixaddr(addr);
}
#endif
