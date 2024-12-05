/* memory.h - general constants/functions for memory, addresses, etc.
 *    for Linux DOS emulator, Robert Sanders, gt8134b@prism.gatech.edu
 */
#ifndef MEMORY_H
#define MEMORY_H

/* split segment 0xf000 into two region, 0xf0000 to 0xf7fff is read-write */
/*                                       0xf8000 to 0xfffff is read-only  */
/* so put anything need read-write into BIOSSEG and anything read-only */
/* to ROMBIOSSEG  */

#define ROM_BIOS_SELFTEST	0xe05b

#define INT09_SEG	BIOSSEG
#define INT09_OFF	0xe987		/* for 100% IBM compatibility */
#define INT09_ADD	((INT09_SEG << 4) + INT09_OFF)

/* The packet driver has some code in this segment which needs to be */
/* at BIOSSEG.  therefore use BIOSSEG and compensate for the offset. */
/* Memory required is about 2000 bytes, beware! */
#define PKTDRV_SEG	(BIOSSEG)
#define TCPDRV_SEG	(BIOSSEG)

#define LFN_HELPER_SEG	BIOSSEG
#define LFN_HELPER_ADD	((LFN_HELPER_SEG << 4) + LFN_HELPER_OFF)

/* don't change these for now, they're hardwired! */
#define Mouse_SEG       (BIOSSEG-1)
#define Mouse_INT_OFF	(MOUSE_INT33_OFF + 0x10)

/* intercept-stub for dosdebugger (catches INT21/AX=4B00 */
#define DBGload_SEG BIOSSEG

#define DOSEMU_LMHEAP_SEG  BIOSSEG
#define DOSEMU_LMHEAP_OFF  lmheap_off()
#define DOSEMU_LMHEAP_SIZE lmheap_size()

#ifndef ROMBIOSSEG
#define ROMBIOSSEG	0xf800
#endif

#define IRET_SEG	BIOSSEG

#define EMM_SEGMENT             (config.ems_frame)

#define INT08_SEG	BIOSSEG
#define INT08_ADD	((INT08_SEG << 4) + INT08_OFF)

#define INT70_SEG	BIOSSEG
#define INT70_ADD	((INT70_SEG << 4) + INT70_OFF)

/* IRQ9->IRQ2 default redirector */
#define INT71_SEG	BIOSSEG
#define INT71_ADD	((INT71_SEG << 4) + INT71_OFF)

#define INT75_SEG	BIOSSEG
#define INT75_ADD	((INT75_SEG << 4) + INT75_OFF)

#define INT1E_SEG	ROMBIOSSEG
#define INT1E_OFF	0x6fc7
#define INT41_SEG	ROMBIOSSEG
#define INT41_OFF	0x6401
#define INT46_SEG	ROMBIOSSEG
#define INT46_OFF	0x6420

#define INT42HOOK_SEG	ROMBIOSSEG
#define INT42HOOK_OFF	0x7065
#define INT42HOOK_ADD	((INT42HOOK_SEG << 4) + INT42HOOK_OFF)

#define POSTHOOK_ADD	((BIOSSEG << 4) + ROM_BIOS_SELFTEST)

/* int10 watcher for mouse support */
/* This was in BIOSSEG (a) so we could write old_int10,
 * when it made a difference...
 */
#define INT10_WATCHER_SEG	BIOSSEG

/* This inline interrupt is used for FCB open calls */
#define FCB_HLP_SEG	BIOSSEG
#define FCB_HLP_ADD	((INTE7_SEG << 4) + INTE7_OFF)

#define DPMI_SEG	BIOSSEG
#define DPMI_ADD	((DPMI_SEG << 4) + DPMI_OFF)

#define INT_RVC_SEG BIOSSEG

#define XMSControl_SEG  BIOSSEG
#define XMSControl_ADD  ((XMSControl_SEG << 4)+XMSControl_OFF+5)

/* For int15 0xc0 */
#define ROM_CONFIG_SEG  BIOSSEG
#define ROM_CONFIG_OFF  0xe6f5
#define ROM_CONFIG_ADD	((ROM_CONFIG_SEG << 4) + ROM_CONFIG_OFF)

/*
 * HLT block
 */
#define BIOS_HLT_BLK_SEG   (BIOSSEG + (bios_hlt_blk >> 4))
#define BIOS_HLT_BLK       (BIOS_HLT_BLK_SEG << 4)
#define BIOS_HLT_BLK_SIZE  0x00400

#define EMSControl_SEG  BIOS_HLT_BLK_SEG
#define IPXEsrEnd_SEG   BIOS_HLT_BLK_SEG
#define PKTRcvCall_SEG  BIOS_HLT_BLK_SEG

#define VBIOS_START	(SEGOFF2LINEAR(config.vbios_seg,0))
/*#define VBIOS_SIZE	(64*1024)*/
#define VBIOS_SIZE	(config.vbios_size)

/* Memory adresses for all common video adapters */

#define MDA_PHYS_TEXT_BASE  0xB0000
#define MDA_VIRT_TEXT_BASE  0xB0000

#define CGA_PHYS_TEXT_BASE  0xB8000
#define CGA_VIRT_TEXT_BASE  0xB8000

#define EGA_PHYS_TEXT_BASE  0xB8000
#define EGA_VIRT_TEXT_BASE  0xB8000

#define VGA_PHYS_TEXT_BASE  0xB8000
#define VGA_VIRT_TEXT_BASE  0xB8000
#define VGA_TEXT_SIZE       0x8000

#define CO      80 /* A-typical screen width */
#define LI      25 /* Normal rows on a screen */
#define TEXT_SIZE(co,li) (((co*li*2)|0xff)+1)

#define VMEM_BASE 0xA0000
#define VMEM_SIZE 0x20000
#define GRAPH_BASE 0xA0000
#define GRAPH_SIZE 0x10000

#define BIOS_DATA_SEG   (0x400)	/* for absolute adressing */

/* Correct HMA size is 64*1024 - 16, but IPC seems not to like this
   hence I would consider that those 16 missed bytes get swapped back
   and forth and may cause us grief - a BUG */
#define HMASIZE (64*1024)
#define LOWMEM_SIZE 0x100000
#define EXTMEM_SIZE ((unsigned)(config.ext_mem << 10))
#define XMS_SIZE ((unsigned)(config.xms_size << 10))
#define xms_base (LOWMEM_SIZE + HMASIZE + EXTMEM_SIZE)

#ifndef __ASSEMBLER__

#include "types.h"
#include <assert.h>
#include <string.h>

typedef uint32_t dosaddr_t;

u_short INT_OFF(u_char i);
#define CBACK_SEG SREG(cs)
#define CBACK_OFF LWORD(eip)

uint16_t lmheap_off(void);
uint16_t lmheap_size(void);
#define FDPP_LMHEAP_ADD (1024 * 3)

/* memcheck memory conflict finder definitions */
int  memcheck_addtype(unsigned char map_char, const char *name);
void memcheck_reserve(unsigned char map_char, dosaddr_t addr_start,
    uint32_t size);
int memcheck_map_reserve(unsigned char map_char, dosaddr_t addr_start,
    uint32_t size);
void memcheck_e820_reserve(dosaddr_t addr_start, uint32_t size, int reserved);
void memcheck_map_free(unsigned char map_char);
void memcheck_init(void);
int  memcheck_isfree(dosaddr_t addr_start, uint32_t size);
int  memcheck_findhole(dosaddr_t *start_addr, uint32_t min_size,
    uint32_t max_size);
int memcheck_is_reserved(dosaddr_t addr_start, uint32_t size,
	unsigned char map_char);
int memcheck_is_rom(dosaddr_t addr);
int memcheck_is_hardware_ram(dosaddr_t addr);
int memcheck_is_system_ram(dosaddr_t addr);
void memcheck_dump(void);
void memcheck_type_init(void);
extern struct system_memory_map *system_memory_map;
extern size_t system_memory_map_size;
void *dosaddr_to_unixaddr(dosaddr_t addr);
void *physaddr_to_unixaddr(unsigned addr);
dosaddr_t physaddr_to_dosaddr(unsigned addr, int len);

#ifndef MAP_FAILED
#define MAP_FAILED (void*)-1
#endif
//void *lowmemp(const unsigned char *ptr);

/* This is the global mem_base pointer: *all* memory is with respect
   to this base. It is normally set to 0 but with mmap_min_addr
   restrictions it can be non-zero. Non-zero values block vm86 but at least
   give NULL pointer protection.
*/
unsigned char *_mem_base(void);
#define mem_base _mem_base()
extern uintptr_t mem_base_mask;

#define LINP(a) ((unsigned char *)(uintptr_t)(a))
static inline unsigned char *MEM_BASE32(dosaddr_t a) {
#if defined(__i386__)
  /* we want to wrap around 4G; &mem_base[a] may cause issues with UBSAN */
  return LINP((uintptr_t)mem_base + a);
#elif defined(__x86_64__)
  return LINP(((uintptr_t)mem_base + a) & mem_base_mask);
#else
  return &mem_base[a];
#endif
}
static inline dosaddr_t DOSADDR_REL(const unsigned char *a)
{
    return (a - mem_base);
}

/* lowmem_base points to a shared memory image of the area 0--1MB+64K.
   It does not have any holes or mapping for video RAM etc.
   The difference is that the mirror image is not read or write protected so
   DOSEMU writes will not be trapped. This allows easy interference with
   simx86, NULL page protection, and removal of the VGA protected memory
   access hack.

   It is set "const" to help GCC optimize accesses. In reality it is set only
   once, at startup
*/
extern uint8_t *lowmem_base;

#define UNIX_READ_BYTE(addr)		(*(Bit8u *) (addr))
#define UNIX_WRITE_BYTE(addr, val)	(*(Bit8u *) (addr) = (val) )
#define UNIX_READ_WORD(addr)		(*(Bit16u *) (addr))
#define UNIX_WRITE_WORD(addr, val)	(*(Bit16u *) (addr) = (val) )
#define UNIX_READ_DWORD(addr)		(*(Bit32u *) (addr))
#define UNIX_WRITE_DWORD(addr, val)	(*(Bit32u *) (addr) = (val) )

#define LOWMEM(addr) ((void *)(&lowmem_base[addr]))

#define LOWMEM_READ_BYTE(addr)		UNIX_READ_BYTE(LOWMEM(addr))
#define LOWMEM_WRITE_BYTE(addr, val)	UNIX_WRITE_BYTE(LOWMEM(addr), val)
#define LOWMEM_READ_WORD(addr)		UNIX_READ_WORD(LOWMEM(addr))
#define LOWMEM_WRITE_WORD(addr, val)	UNIX_WRITE_WORD(LOWMEM(addr), val)
#define LOWMEM_READ_DWORD(addr)		UNIX_READ_DWORD(LOWMEM(addr))
#define LOWMEM_WRITE_DWORD(addr, val)	UNIX_WRITE_DWORD(LOWMEM(addr), val)

static inline void *LINEAR2UNIX(unsigned int addr)
{
	return dosaddr_to_unixaddr(addr);
}

#define READ_BYTE(addr)		UNIX_READ_BYTE(LINEAR2UNIX(addr))
#define WRITE_BYTE(addr, val)	UNIX_WRITE_BYTE(LINEAR2UNIX(addr), val)
#define READ_WORD(addr)		UNIX_READ_WORD(LINEAR2UNIX(addr))
#define WRITE_WORD(addr, val)	UNIX_WRITE_WORD(LINEAR2UNIX(addr), val)
#define READ_DWORD(addr)	UNIX_READ_DWORD(LINEAR2UNIX(addr))
#define WRITE_DWORD(addr, val)	UNIX_WRITE_DWORD(LINEAR2UNIX(addr), val)

#define MEMCPY_2UNIX(unix_addr, dos_addr, n) \
	memcpy((unix_addr), LINEAR2UNIX(dos_addr), (n))

#define MEMCPY_2DOS(dos_addr, unix_addr, n) \
	memcpy(LINEAR2UNIX(dos_addr), (unix_addr), (n))

#define MEMCPY_DOS2DOS(dos_addr1, dos_addr2, n) \
	memcpy(LINEAR2UNIX(dos_addr1), LINEAR2UNIX(dos_addr2), (n))

#define MEMMOVE_DOS2DOS(dos_addr1, dos_addr2, n) \
        memmove(LINEAR2UNIX(dos_addr1), LINEAR2UNIX(dos_addr2), (n))

#define MEMCMP_DOS_VS_UNIX(dos_addr, unix_addr, n) \
	memcmp(LINEAR2UNIX(dos_addr), (unix_addr), (n))

#define MEMSET_DOS(dos_addr, val, n) \
        memset(LINEAR2UNIX(dos_addr), (val), (n))

/* The "P" macros all take valid pointer addresses; the pointers are
   aliased from mem_base to lowmem_base if possible.
   The non-P macros take integers with respect to mem_base or lowmem_base.
   Usually its easiest to deal with integers but some functions accept both
   pointers into DOSEMU data and pointers into DOS space.
 */
#define READ_BYTEP(addr)	UNIX_READ_BYTE(addr)
#define WRITE_BYTEP(addr, val)	UNIX_WRITE_BYTE(addr, val)
#define READ_WORDP(addr)	UNIX_READ_WORD(addr)
#define WRITE_WORDP(addr, val)	UNIX_WRITE_WORD(addr, val)
#define READ_DWORDP(addr)	UNIX_READ_DWORD(addr)
#define WRITE_DWORDP(addr, val)	UNIX_WRITE_DWORD(addr, val)

#define WRITE_P(loc, val) ((loc) = (val))

#define READ_BYTE_S(b, s, m)	READ_BYTE(b + offsetof(s, m))
#define READ_WORD_S(b, s, m)	READ_WORD(b + offsetof(s, m))
#define READ_DWORD_S(b, s, m)	READ_DWORD(b + offsetof(s, m))

#define MEMCPY_P2UNIX(unix_addr, dos_addr, n) \
	memcpy((unix_addr), (dos_addr), (n))

#define MEMCPY_2DOSP(dos_addr, unix_addr, n) \
	memcpy((dos_addr), (unix_addr), (n))

#endif

#endif /* MEMORY_H */
