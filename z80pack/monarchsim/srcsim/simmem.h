/*
 *	MONARCHSIM  -  Dynabyte Monarch (Z80 side), memory
 *
 *	The Monarch maps memory in eight 8 KB slots. The page map is written
 *	through port D5, and the slot is not in the port number -- it is on
 *	A8-A15, so the write is done with OUT (C),r and B carrying the slot
 *	in its top three bits:
 *
 *		LD A,0FFh : OUT (087h),A
 *		LD DE,0008h : LD B,000h : LD C,0D5h
 *	loop:	OUT (C),D            ; slot = B >> 5, physical page = D
 *		INC D
 *		LD A,020h : ADD A,B : LD B,A
 *		DEC E : JR NZ,loop
 *
 *	which lays down the identity map at reset. MP/M then keeps slots 6
 *	and 7 (C000-FFFF) pinned to pages 06/07 -- the 16 KB common area --
 *	and swaps slots 0-5 (0000-BFFF, 48 KB) to move between banks, the
 *	physical pages of consecutive banks running six apart.
 */

#ifndef SIMMEM_INC
#define SIMMEM_INC

#include "sim.h"
#include "simdefs.h"
#ifdef WANT_ICE
#include "simice.h"
#endif

#define PAGE_BITS	13			/* 8 KB pages		*/
#define PAGE_SIZE	(1 << PAGE_BITS)
#define NSLOT		8			/* 8 x 8 KB = 64 KB	*/
#define NPAGE		128			/* 1 MB of physical	*/
#define NSET		256			/* one page map per bank */

extern BYTE phys[NPAGE * PAGE_SIZE];
extern BYTE mapset[NSET][NSLOT];	/* page map, one set per bank	*/
extern int  mmu_run;			/* set the CPU executes from	*/
extern int  mmu_prog;			/* set being programmed		*/

extern void init_memory(void);
extern void mmu_write_map(BYTE slot, BYTE page);
extern void mmu_select_run(BYTE set);
extern void mmu_select_prog(BYTE set);
extern void mmu_dump(const char *when);

static inline unsigned mmu_phys(WORD addr)
{
	return ((unsigned) mapset[mmu_run][addr >> PAGE_BITS] << PAGE_BITS)
	       | (addr & (PAGE_SIZE - 1));
}

static inline void memwrt(WORD addr, BYTE data)
{
	phys[mmu_phys(addr)] = data;
}

static inline BYTE memrdr(WORD addr)
{
	return phys[mmu_phys(addr)];
}

static inline void dma_write(WORD addr, BYTE data)
{
	phys[mmu_phys(addr)] = data;
}

static inline BYTE dma_read(WORD addr)
{
	return phys[mmu_phys(addr)];
}

static inline void putmem(WORD addr, BYTE data)
{
	phys[mmu_phys(addr)] = data;
}

static inline BYTE getmem(WORD addr)
{
	return phys[mmu_phys(addr)];
}

#endif /* !SIMMEM_INC */
