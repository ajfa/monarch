/*
 *	MONARCHSIM  -  Dynabyte Monarch (Z80 side), memory
 *
 *	Eight 8 KB slots, and a whole page map per bank. Which map the CPU
 *	runs from is chosen by port 85 -- that is literally all SELMEMORY in
 *	Dynabyte's XIOS does:
 *
 *		lc766h:  INC BC : INC BC : INC BC   ; MP/M descriptor: the
 *			 LD A,(BC)                  ; fourth byte is the bank
 *			 OUT (085h),A
 *			 RET
 *
 *	and which map is being *programmed* is chosen by port 87, written
 *	just before the D5 writes that fill it in. That is why slots 0-3 are
 *	only ever programmed once per bank yet banks switch constantly.
 */

#include <string.h>

#include "sim.h"
#include "simdefs.h"
#include "simglb.h"
#include "simmem.h"

#include "log.h"

BYTE phys[NPAGE * PAGE_SIZE];
BYTE mapset[NSET][NSLOT];
int  mmu_run, mmu_prog;

void init_memory(void)
{
	int s, i;

	/* every set starts as the identity map the boot PROM would leave */
	for (s = 0; s < NSET; s++)
		for (i = 0; i < NSLOT; i++)
			mapset[s][i] = i;
	mmu_run = mmu_prog = 0;
	memset(phys, 0, sizeof(phys));
}

void mmu_write_map(BYTE slot, BYTE page)
{
	mapset[mmu_prog][slot & (NSLOT - 1)] = page % NPAGE;
}

void mmu_select_run(BYTE set)
{
	mmu_run = set;
}

void mmu_select_prog(BYTE set)
{
	mmu_prog = set;
}

void mmu_dump(const char *when)
{
	int i, ident = 1;

	for (i = 0; i < NSLOT; i++)
		if (mapset[mmu_run][i] != i)
			ident = 0;
	LOG("mmu", "MAP %s: set %02X = %d %d %d %d %d %d %d %d  (%s)\r\n", when,
	    mmu_run, mapset[mmu_run][0], mapset[mmu_run][1], mapset[mmu_run][2],
	    mapset[mmu_run][3], mapset[mmu_run][4], mapset[mmu_run][5],
	    mapset[mmu_run][6], mapset[mmu_run][7], ident ? "identity" : "mapped");
}
