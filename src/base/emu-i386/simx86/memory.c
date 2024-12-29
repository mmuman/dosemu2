/***************************************************************************
 *
 * All modifications in this file to the original code are
 * (C) Copyright 1992, ..., 2014 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 *
 *
 *  SIMX86 a Intel 80x86 cpu emulator
 *  Copyright (C) 1997,2001 Alberto Vignani, FIAT Research Center
 *				a.vignani@crf.it
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Additional copyright notes:
 *
 * 1. The kernel-level vm86 handling was taken out of the Linux kernel
 *  (linux/arch/i386/kernel/vm86.c). This code originally was written by
 *  Linus Torvalds with later enhancements by Lutz Molgedey and Hans Lermen.
 *
 ***************************************************************************/

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/mman.h>

#include "mapping.h"
#include "dosemu_debug.h"
#include "dlmalloc.h"
#include "emu86.h"
#include "trees.h"
#include "codegen.h"
#include "emudpmi.h"

#define CGRAN		0		/* 2^n */
#define CGRMASK		(0xfffff>>CGRAN)

#ifndef UINT64_WIDTH
#define UINT64_WIDTH 64
#endif

typedef struct _mpmap {
	struct _mpmap *next;
	int mega;
	unsigned char pagemap[32];	/* (32*8)=256 pages *4096 = 1M */
	uint64_t subpage[(0x100000>>CGRAN)/UINT64_WIDTH];	/* 2^CGRAN-byte granularity, 1M/2^CGRAN bits */
} tMpMap;

static tMpMap *MpH = NULL;
unsigned int mMaxMem = 0;
int PageFaults = 0;
static tMpMap *LastMp = NULL;

static int e_munprotect(unsigned int addr, size_t len);

/////////////////////////////////////////////////////////////////////////////

static inline tMpMap *FindM(unsigned int addr)
{
	register int a2l = addr >> (PAGE_SHIFT+8);
	register tMpMap *M = LastMp;

	if (M && (M->mega==a2l)) return M;
	M = MpH;
	while (M) {
		if (M->mega==a2l) {
		    LastMp = M; break;
		}
		M = M->next;
	}
	return M;
}


static int AddMpMap(unsigned int addr, unsigned int aend, int onoff)
{
	int bs=0, bp=0;
	register int page;
	tMpMap *M;

	do {
	    page = addr >> PAGE_SHIFT;
	    M = MpH;
	    while (M) {
		if (M->mega==(page>>8)) break;
		M = M->next;
	    }
	    if (M==NULL) {
		M = (tMpMap *)calloc(1,sizeof(tMpMap));
		M->next = MpH; MpH = M;
		M->mega = (page>>8);
	    }
	    if (bp < 32) {
		bs |= (((unsigned)(onoff? test_and_set_bit(page&255, M->pagemap) :
			    test_and_clear_bit(page&255, M->pagemap)) & 1) << bp);
		bp++;
	    }
	    if (debug_level('e')>1) {
		if (addr > mMaxMem) mMaxMem = addr;
		if (onoff)
		  dbug_printf("MPMAP:   protect page=%08x was %x\n",addr,bs);
		else
		  dbug_printf("MPMAP: unprotect page=%08x was %x\n",addr,bs);
	    }
	    addr += PAGE_SIZE;
	} while (addr <= aend);
	return bs;
}


int e_querymprot(dosaddr_t addr)
{
	register int a2 = addr >> PAGE_SHIFT;
	tMpMap *M = FindM(addr);

	if (M==NULL) return 0;
	return test_bit(a2&255, M->pagemap);
}

int e_querymprotrange(unsigned int addr, size_t len)
{
	int a2l, a2h;
	tMpMap *M = FindM(addr);

	a2l = addr >> PAGE_SHIFT;
	a2h = (addr+len-1) >> PAGE_SHIFT;

	while (M && a2l <= a2h) {
		if (test_bit(a2l&255, M->pagemap))
			return 1;
		a2l++;
		if ((a2l&255)==0)
			M = M->next;
	}
	return 0;
}


/////////////////////////////////////////////////////////////////////////////


int e_markpage(unsigned int addr, size_t len)
{
	unsigned int abeg, aend;
	tMpMap *M = FindM(addr);

	if (M == NULL || len == 0) return 0;

	abeg = addr >> CGRAN;
	aend = (addr+len-1) >> CGRAN;

	if (debug_level('e')>1)
		dbug_printf("MARK from %08x to %08x for %08x\n",
			    abeg<<CGRAN,((aend+1)<<CGRAN)-1,addr);
	while (M && abeg <= aend) {
		assert(!test_bit(abeg&CGRMASK, M->subpage));
		set_bit(abeg&CGRMASK, M->subpage);
		abeg++;
		if ((abeg&CGRMASK) == 0)
			M = M->next;
	}
	return 1;
}

int e_unmarkpage(unsigned int addr, size_t len)
{
	unsigned int abeg, aend;
	tMpMap *M = FindM(addr);

	if (M == NULL || len == 0) return 0;

	abeg = addr >> CGRAN;
	aend = (addr+len-1) >> CGRAN;

	if (debug_level('e')>1)
		dbug_printf("UNMARK from %08x to %08x for %08x\n",
			    abeg<<CGRAN,((aend+1)<<CGRAN)-1,addr);
	while (M && abeg <= aend) {
		clear_bit(abeg&CGRMASK, M->subpage);
		abeg++;
		if ((abeg&CGRMASK) == 0)
			M = M->next;
	}

	/* check if unmarked pages have no more code, and if so, unprotect */
	abeg = addr & _PAGE_MASK;
	aend = (addr + len) & _PAGE_MASK;
	/* don't unprotect partial first page with code (if not also last) */
	if (aend != abeg && abeg != addr && e_querymark(abeg, PAGE_SIZE))
		abeg += PAGE_SIZE;
	/* unprotect partial last page without code */
	if (aend != addr+len && !e_querymark(aend, PAGE_SIZE))
		aend += PAGE_SIZE;

	if (aend > abeg)
		e_munprotect(abeg, aend - abeg);

	return 1;
}

int e_querymark(unsigned int addr, size_t len)
{
	unsigned int abeg, aend, idx;
	tMpMap *M = FindM(addr);
	uint64_t mask;

	if (M == NULL) return 0;

	abeg = addr >> CGRAN;
	aend = ((addr+len-1) >> CGRAN) + 1;

	if (debug_level('e')>2)
		dbug_printf("QUERY MARK from %08x to %08x for %08x\n",
			    abeg<<CGRAN,((aend+1)<<CGRAN)-1,addr);
	if (len == 1) {
		// common case, fast path
		if (test_bit(abeg&CGRMASK, M->subpage))
			goto found;
		return 0;
	}

	idx = (abeg&CGRMASK) / UINT64_WIDTH;
	// mask for first partial longword
	mask = ~0ULL << (abeg & (UINT64_WIDTH-1));
	while (abeg < (aend & ~(UINT64_WIDTH-1))) {
		if (M->subpage[idx] & mask)
			goto found;
		abeg = (abeg + UINT64_WIDTH) & ~(UINT64_WIDTH-1);
		idx++;
		mask = ~0ULL;
		if (idx == sizeof(M->subpage)/sizeof(M->subpage[0])) {
			M = M->next;
			if (!M)
				return 0;
			idx = 0;
		}
	}
	if (aend & (UINT64_WIDTH-1)) {
		// mask for last partial longword
		mask &= ~0ULL >> (UINT64_WIDTH - (aend & (UINT64_WIDTH-1)));
		if (M->subpage[idx] & mask)
			goto found;
	}
	return 0;
found:
	if (debug_level('e')>1) {
//		if (len > 1) abeg += ffsll(M->subpage[idx] & mask) - 1;
		dbug_printf("QUERY MARK found code at "
			    "%08x to %08x for %08x\n",
			    abeg<<CGRAN, ((abeg+1)<<CGRAN)-1,
			    addr);
	}
	return 1;
}

/* for debugging only */
int e_querymark_all(unsigned int addr, size_t len)
{
	unsigned int abeg, aend;
	tMpMap *M = FindM(addr);

	if (M == NULL) return 0;

	abeg = addr >> CGRAN;
	aend = (addr+len-1) >> CGRAN;

	while (M && abeg <= aend) {
		if (!test_bit(abeg&CGRMASK, M->subpage))
			return 0;
		abeg++;
		if ((abeg&CGRMASK) == 0)
			M = M->next;
	}
	return 1;
}

/////////////////////////////////////////////////////////////////////////////


int e_mprotect(unsigned int addr, size_t len)
{
	int e;
	unsigned int abeg, aend, aend1;
	unsigned int abeg1 = (unsigned)-1;
	unsigned a;
	int ret = 1;

	abeg = addr & _PAGE_MASK;
	if (len==0) {
	    return 0;
	}
	else {
	    aend = (addr+len-1) & _PAGE_MASK;
	}
	/* only protect ranges that were not already protected by e_mprotect */
	for (a = abeg; a <= aend; a += PAGE_SIZE) {
	    int qp = e_querymprot(a);
	    if (!qp) {
		if (abeg1 == (unsigned)-1)
		    abeg1 = a;
		aend1 = a;
	    }
	    if ((a == aend || qp) && abeg1 != (unsigned)-1) {
		e = mprotect_mapping(MAPPING_CPUEMU, abeg1, aend1-abeg1+PAGE_SIZE,
			    PROT_READ | _PROT_EXEC);
		if (e<0) {
		    e_printf("MPMAP: %s\n",strerror(errno));
		    return -1;
		}
		ret = AddMpMap(abeg1, aend1+PAGE_SIZE-1, 1);
		abeg1 = (unsigned)-1;
	    }
	}
	return ret;
}

static int e_munprotect(unsigned int addr, size_t len)
{
	int e;
	unsigned int abeg, aend, aend1;
	unsigned int abeg1 = (unsigned)-1;
	unsigned a;
	int ret = 0;

	abeg = addr & _PAGE_MASK;
	if (len==0) {
	    aend = abeg;
	}
	else {
	    aend = (addr+len-1) & _PAGE_MASK;
	}
	/* only unprotect ranges that were protected by e_mprotect */
	for (a = abeg; a <= aend; a += PAGE_SIZE) {
	    int qp = e_querymprot(a);
	    if (qp) {
		if (abeg1 == (unsigned)-1)
		    abeg1 = a;
		aend1 = a;
	    }
	    if ((a == aend || !qp) && abeg1 != (unsigned)-1) {
		e = mprotect_mapping(MAPPING_CPUEMU, abeg1, aend1-abeg1+PAGE_SIZE,
			     PROT_RWX);
		if (e<0) {
		    e_printf("MPUNMAP: %s\n",strerror(errno));
		    return -1;
		}
		ret = AddMpMap(abeg1, aend1+PAGE_SIZE-1, 0);
		abeg1 = (unsigned)-1;
	    }
	}
	return ret;
}

#ifdef X86_JIT
int e_handle_pagefault(dosaddr_t addr, unsigned err, sigcontext_t *scp)
{
	register int v;
	unsigned char *p;
	int in_dosemu;

	/* err:
	 * bit 0 = 1	page protect
	 * bit 1 = 1	writing
	 * bit 2 = 1	user mode
	 * bit 3 = 0	no reserved bit err
	 */
	if ((err & 0x06) != 0x06)
		return 0;
	/* additionally check if page is not mapped */
	if (addr >= LOWMEM_SIZE + HMASIZE &&
			/* can't fully trust err as page may be swapped out */
			!(err & 1) && !dpmi_read_access(addr))
		return 0;
	if (!e_querymprot(addr))
		return 0;

	/* Got a fault in a write-protected memory page, that is,
	 * a page _containing_code_. 99% of the time we are
	 * hitting data or stack in the same page, NOT code.
	 *
	 * We check using e_querymprot whether we protected the
	 * page ourselves. Additionally an error code of 7 should
	 * have been given.
	 *
	 * _cr2 keeps the address where the code tries to write
	 * _rip keeps the address of the faulting instruction
	 *	(in the code buffer or in the tree)
	 *
	 * The faults can also come from
	 * - native/KVM DPMI code if only vm86() is emulated
	 * - native/KVM vm86 code if only DPMI is emulated
	 * - DOSEMU itself (to be avoided)
	 *
	 * Possible instructions we'll find here are (see sigsegv.v):
	 *	8807	movb	%%al,(%%edi)
	 *	(66)8907	mov{wl}	%%{e}ax,(%%edi)
	 *	(f3)(66)a4,a5	movs
	 *	(f3)(66)aa,ab	stos
	 */
#if PROFILE
	if (debug_level('e')) PageFaults++;
#endif
	in_dosemu = !(InCompiledCode || in_vm86 || DPMIValidSelector(_scp_cs));
	if (in_vm86)
		p = SEG_ADR((unsigned char *), cs, ip);
	else if (DPMIValidSelector(_scp_cs))
		p = (unsigned char *)EMU_BASE32(GetSegmentBase(_scp_cs) + _scp_rip);
	else
		p = (unsigned char *) _scp_rip;
	if (debug_level('e')>1 || in_dosemu) {
		v = *((int *)p);
		__asm__("bswap %0" : "=r" (v) : "0" (v));
		e_printf("Faulting ops: %08x\n",v);

		if (!InCompiledCode) {
			unsigned int cs = in_vm86 ? _CS : _scp_cs;
			greg_t eip = in_vm86 ? _IP : _scp_rip;
			e_printf("*\tFault out of %scode, cs:eip=%x:%llx,"
				    " cr2=%x, fault_cnt=%d\n",
				    in_dosemu ? "DOSEMU " : "",
				    cs, (unsigned long long) eip, addr,
				    fault_cnt);
		}
		if (e_querymark(addr, 1)) {
			e_printf("CODE node hit at %08x\n",addr);
		}
		else if (InCompiledCode) {
			e_printf("DATA node hit at %08x\n",addr);
		}
	}
	/* the page is not unprotected here, the code
	 * linked by Cpatch will do it */
	/* ACH: we can set up a data patch for code
	 * which has not yet been executed! */
#ifndef SKIP_CPATCH
	if (InCompiledCode && Cpatch(scp))
		return 1;
#endif
	/* We HAVE to invalidate all the code in the page
	 * if the page is going to be unprotected */
	addr &= _PAGE_MASK;
	return InvalidateNodeRange(addr, PAGE_SIZE, p);
}

int e_handle_fault(sigcontext_t *scp)
{
	if (!InCompiledCode)
		return 0;
#ifdef __x86_64__
	if (IS_EMU_JIT() && _scp_trapno == 0xd &&
			_scp_rbp + _scp_rdi >= (1ULL << 47)) {
		/* translate to PF */
		_scp_trapno = 0xe;
		_scp_cr2 = _scp_rbp + _scp_rdi;
		error("Non-canonical address exception at 0x%"PRI_RG"\n", _scp_cr2);
		return 0;
	}
#endif
	/* page-faults are handled not here and only DE remains */
	if (_scp_trapno != 0) {
		error("Fault %i in jit-compiled code\n", _scp_trapno);
		return 0;
	}
	TheCPU.err = EXCP00_DIVZ + _scp_trapno;
	_scp_eax = TheCPU.cr2;
	_scp_edx = _scp_eflags;
	TheCPU.cr2 = EMUADDR_REL(LINP(_scp_cr2));
	_scp_rip = *(long *)_scp_rsp;
	_scp_rsp += sizeof(long);
	return 1;
}
#endif

/////////////////////////////////////////////////////////////////////////////

void mprot_init(void)
{
	MpH = NULL;
	AddMpMap(0,0,0);	/* first mega in first entry */
	PageFaults = 0;
}

void mprot_end(void)
{
	tMpMap *M = MpH;
	int i;
	unsigned char b;

	while (M) {
	    tMpMap *M2 = M;
	    for (i=0; i<32; i++) if ((b=M->pagemap[i])) {
		unsigned int addr = (M->mega<<20) | (i<<15);
		while (b) {
		    if (b & 1) {
			if (debug_level('e')>1)
			    dbug_printf("MP_END %08x = RWX\n",addr);
			mprotect_mapping(MAPPING_CPUEMU, addr, PAGE_SIZE, PROT_RWX);
		    }
		    addr += PAGE_SIZE;
	 	    b >>= 1;
		}
	    }
	    M = M->next;
	    free(M2);
	}
	MpH = LastMp = NULL;
}

/////////////////////////////////////////////////////////////////////////////
