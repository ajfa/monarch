/*
 *	MONARCHSIM  -  Dynabyte Monarch (Z80 side), system control
 *
 *	The Monarch's boot PROM was never dumped, but the diskette itself
 *	determines exactly what it must have done. The two reserved tracks
 *	of the MP/M II diskette are, byte for byte, the file FPYMPM.LDR --
 *	52 sectors, 6656 bytes, in plain physical order with no skew. The
 *	first thing that image does is
 *
 *		LD HL,0B0EDh    ; the opcodes for LDIR
 *		LD (00FEh),HL
 *		LD HL,1012h
 *		LD DE,0100h
 *		LD BC,2F00h
 *		JP 00FEh        ; run the LDIR, then fall through into 0100h
 *
 *	which only makes sense if the image was loaded at 1000h and entered
 *	there: 1012h is 1000h + 18, the byte just past this stub. So the
 *	PROM read the reserved tracks to 1000h and jumped to 1000h, and that
 *	is what is synthesised below.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "sim.h"
#include "simdefs.h"
#include "simglb.h"
#include "simcore.h"
#include "simmem.h"
#include "simio.h"
#include "simctl.h"
#ifdef WANT_ICE
#include "simice.h"
#endif

#include "unix_terminal.h"
#include "log.h"

static const char *TAG = "system";

#define BOOT_ADDR	0x1000
#define BOOT_TRACKS	2
#define BOOT_SPT	26
#define BOOT_SECSZ	128

extern int monarch_disk_open(void);
extern char monarch_disk[];
extern int monarch_trace;

int boot(int level)
{
	int trk, sec, i;
	WORD a = BOOT_ADDR;
	BYTE buf[BOOT_SECSZ];
	extern int monarch_read_phys(int trk, int sec, BYTE *buf);

	(void) level;

	LOG(TAG, "\r\nMonarch: loading the reserved tracks to %04XH\r\n", BOOT_ADDR);

	for (trk = 0; trk < BOOT_TRACKS; trk++)
		for (sec = 1; sec <= BOOT_SPT; sec++) {
			if (monarch_read_phys(trk, sec, buf)) {
				LOGE(TAG, "boot read failed at track %d sector %d",
				     trk, sec);
				return 1;
			}
			for (i = 0; i < BOOT_SECSZ; i++)
				putmem(a++, buf[i]);
		}

	LOG(TAG, "Monarch: %d bytes loaded, entering at %04XH\r\n\r\n",
	    BOOT_TRACKS * BOOT_SPT * BOOT_SECSZ, BOOT_ADDR);

	mmu_dump("after boot load");
	PC = BOOT_ADDR;
	return 0;
}

void mon(void)
{
	if (monarch_disk_open())
		exit(EXIT_FAILURE);

	if (boot(0))
		exit(EXIT_FAILURE);

	fflush(stdout);

#ifdef WANT_ICE
	ice_before_go = set_unix_terminal;
	ice_after_go = reset_unix_terminal;
	atexit(reset_unix_terminal);
	ice_cmd_loop(0);
#else
	set_unix_terminal();
	atexit(reset_unix_terminal);
	run_cpu();
	reset_unix_terminal();
	report_cpu_error();
	report_cpu_stats();
#endif
}
