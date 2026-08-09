/*
 *	MONARCHSIM  -  Dynabyte Monarch (Z80 side), I/O
 *
 *	Nothing about this machine is documented. Every port below was
 *	recovered by disassembling Dynabyte's own LDRBIOS (inside
 *	FPYMPM.LDR) and XIOS1.SPR:
 *
 *	  B0   console UART data      (in/out)
 *	  B2   console UART status    bit0 = RX ready, bit2 = TX ready
 *	  D0   8272/uPD765 FDC        Main Status Register  (in)
 *	  D1   8272/uPD765 FDC        data register, programmed I/O
 *	  D3   8272/uPD765 FDC        data register, DMA path (DMA port A)
 *	  D6   Zilog Z8410 DMA        register file / commands
 *	  8C   board control latch    (2Dh written at boot; purpose unknown)
 *
 *	The 8272 command blocks the LDRBIOS builds are plain datasheet:
 *	SPECIFY 03, SENSE DRIVE STATUS 04, READ DATA 06/46, RECALIBRATE 07,
 *	SENSE INTERRUPT STATUS 08, READ ID 0A/4A, SEEK 0F.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "sim.h"
#include "simdefs.h"
#include "simglb.h"
#include "simcfg.h"
#include "simmem.h"
#include "simio.h"
#include "simport.h"

#include "unix_terminal.h"
#include "log.h"

static const char *TAG = "monarch";

int monarch_trace = 0;
#define TRC(...) do { if (monarch_trace) LOG(TAG, __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ disk */

#define TRACKS		77
#define SPT		26		/* physical sectors per track */
#define SECSZ		128		/* single density */

char monarch_disk[MAX_LFN] = "disks/drivea.dsk";
static int dfd = -1;
static int disk_ro;

int monarch_disk_open(void)
{
	dfd = open(monarch_disk, O_RDWR);
	if (dfd < 0) {
		dfd = open(monarch_disk, O_RDONLY);
		disk_ro = 1;
	}
	if (dfd < 0) {
		LOGE(TAG, "cannot open disk image %s", monarch_disk);
		return 1;
	}
	if (disk_ro)
		LOGW(TAG, "%s is read only; the guest will see write errors",
		     monarch_disk);
	return 0;
}

/* physical sector -> byte offset; sector numbers are 1-based */
static long secoff(int trk, int head, int sec)
{
	(void) head;			/* the boot diskettes are single sided */
	return ((long) trk * SPT + (sec - 1)) * SECSZ;
}


/* physical sector read, used by the synthesised boot PROM in simctl.c */
int monarch_read_phys(int trk, int sec, BYTE *buf)
{
	ssize_t n, got = 0;

	if (dfd < 0)
		return 1;
	if (lseek(dfd, secoff(trk, 0, sec), SEEK_SET) < 0)
		return 1;
	while (got < SECSZ) {			/* the tick must not truncate this */
		n = read(dfd, buf + got, SECSZ - got);
		if (n > 0)
			got += n;
		else if (n < 0 && errno == EINTR)
			continue;
		else
			return 1;
	}
	return 0;
}

static int monarch_write_phys(int trk, int sec, const BYTE *buf)
{
	ssize_t n, put = 0;

	if (dfd < 0 || disk_ro)
		return 1;
	if (lseek(dfd, secoff(trk, 0, sec), SEEK_SET) < 0)
		return 1;
	while (put < SECSZ) {			/* the tick must not truncate this */
		n = write(dfd, buf + put, SECSZ - put);
		if (n > 0)
			put += n;
		else if (n < 0 && errno == EINTR)
			continue;
		else
			return 1;
	}
	return 0;
}

/* ------------------------------------------------------- Z80 DMA (Z8410) */

static struct {
	WORD	a_addr, b_addr;		/* port A / port B start addresses */
	WORD	a_cur,  b_cur;
	WORD	len;			/* block length register (count-1)   */
	int	a_io,   b_io;		/* 1 = I/O port, 0 = memory          */
	int	a_fixed, b_fixed;
	int	a_to_b;			/* transfer direction                */
	int	enabled;
	int	expect;			/* parameter bytes still expected    */
	BYTE	which[8];		/* what each expected byte means     */
	int	widx;
	BYTE	status;
	int	int_eob;		/* interrupt at end of block	*/
	BYTE	ivec;			/* and the vector it uses	*/
	int	int_pend;
} dma;

enum { P_AL, P_AH, P_LL, P_LH, P_BL, P_BH, P_IC, P_IV, P_SKIP };

static void dma_reset(void)
{
	memset(&dma, 0, sizeof(dma));
	dma.status = 0x3a;
}

static void dma_param(BYTE d)
{
	switch (dma.which[dma.widx]) {
	case P_AL: dma.a_addr = (dma.a_addr & 0xff00) | d; break;
	case P_AH: dma.a_addr = (dma.a_addr & 0x00ff) | (d << 8); break;
	case P_LL: dma.len    = (dma.len    & 0xff00) | d; break;
	case P_LH: dma.len    = (dma.len    & 0x00ff) | (d << 8); break;
	case P_BL: dma.b_addr = (dma.b_addr & 0xff00) | d; break;
	case P_BH: dma.b_addr = (dma.b_addr & 0x00ff) | (d << 8); break;
	case P_IC:
		/* the interrupt control byte: bit 1 asks for an interrupt at
		   end of block, bit 4 says its vector is the next byte */
		dma.int_eob = (d & 0x02) ? 1 : 0;
		if (d & 0x10) {
			dma.which[dma.widx + 1] = P_IV;
			dma.expect++;
		}
		break;
	case P_IV: dma.ivec = d; break;
	default: break;
	}
	dma.widx++;
	dma.expect--;
}

static void fdc_try_transfer(void);
static void win_try_dma(void);

static void dma_out(const BYTE data)
{
	if (dma.expect > 0) {
		dma_param(data);
		if (dma.expect == 0)
			TRC("dma: A=%04X %s%s  B=%04X %s%s  len=%04X  %s\r\n",
			    dma.a_addr, dma.a_io ? "io" : "mem",
			    dma.a_fixed ? " fixed" : " inc",
			    dma.b_addr, dma.b_io ? "io" : "mem",
			    dma.b_fixed ? " fixed" : " inc",
			    dma.len, dma.a_to_b ? "A->B" : "B->A");
		return;
	}

	dma.widx = 0;
	memset(dma.which, P_SKIP, sizeof(dma.which));

	if ((data & 0x80) == 0 && (data & 0x03) != 0) {		/* WR0 */
		dma.a_to_b = (data & 0x04) ? 1 : 0;
		if (data & 0x08) dma.which[dma.expect++] = P_AL;
		if (data & 0x10) dma.which[dma.expect++] = P_AH;
		if (data & 0x20) dma.which[dma.expect++] = P_LL;
		if (data & 0x40) dma.which[dma.expect++] = P_LH;
	} else if ((data & 0x87) == 0x04) {			/* WR1 port A */
		dma.a_io    = (data & 0x08) ? 1 : 0;
		dma.a_fixed = ((data & 0x30) == 0x20 || (data & 0x30) == 0x30);
		if (data & 0x40) dma.which[dma.expect++] = P_SKIP;
	} else if ((data & 0x87) == 0x00) {			/* WR2 port B */
		dma.b_io    = (data & 0x08) ? 1 : 0;
		dma.b_fixed = ((data & 0x30) == 0x20 || (data & 0x30) == 0x30);
		if (data & 0x40) dma.which[dma.expect++] = P_SKIP;
	} else if ((data & 0x83) == 0x80) {			/* WR3 */
		if (data & 0x08) dma.which[dma.expect++] = P_SKIP;
		if (data & 0x10) dma.which[dma.expect++] = P_SKIP;
	} else if ((data & 0x83) == 0x81) {			/* WR4 */
		if (data & 0x04) dma.which[dma.expect++] = P_BL;
		if (data & 0x08) dma.which[dma.expect++] = P_BH;
		if (data & 0x10) dma.which[dma.expect++] = P_IC;
	} else if ((data & 0x83) == 0x82) {			/* WR5 */
		/* ready/ce-wait configuration, nothing to model */
	} else if ((data & 0x83) == 0x83) {			/* WR6 command */
		switch (data) {
		case 0xc3:				/* reset */
			TRC("dma: reset\r\n"); dma_reset(); break;
		case 0xcf:				/* load */
			dma.a_cur = dma.a_addr; dma.b_cur = dma.b_addr;
			TRC("dma: load\r\n"); break;
		case 0x8b:				/* reinit status byte */
			dma.status = 0x3a; break;
		case 0x87:				/* enable DMA */
			dma.enabled = 1;
			TRC("dma: enable\r\n");
			fdc_try_transfer();
			win_try_dma();
			break;
		case 0x83:				/* disable DMA */
			dma.enabled = 0; break;
		case 0xbf: case 0xa7: case 0xbb: case 0xab:
			break;				/* read/status sequences */
		default:
			TRC("dma: command %02X\r\n", data);
			break;
		}
	}
}

static BYTE dma_in(void)
{
	return dma.status;
}

/* ------------------------------------------------------------ 8272 FDC */

#define MSR_D0B	0x01
#define MSR_CB	0x10			/* controller busy		*/
#define MSR_EXM	0x20			/* execution phase, non-DMA	*/
#define MSR_DIO	0x40			/* 1 = FDC -> CPU		*/
#define MSR_RQM	0x80			/* data register ready		*/

static struct {
	int	phase;			/* 0 idle, 1 command, 2 result	*/
	BYTE	cmd[9];
	int	ncmd, want;
	BYTE	res[7];
	int	nres, ridx;
	int	pcn;			/* present cylinder		*/
	int	seek_end;
	BYTE	st0_int;
	int	pending;		/* read waiting for the DMA	*/
} fdc;

static int cmd_len(BYTE c)
{
	switch (c & 0x1f) {
	case 0x03: return 3;		/* SPECIFY			*/
	case 0x04: return 2;		/* SENSE DRIVE STATUS		*/
	case 0x05: return 9;		/* WRITE DATA			*/
	case 0x06: return 9;		/* READ DATA			*/
	case 0x07: return 2;		/* RECALIBRATE			*/
	case 0x08: return 1;		/* SENSE INTERRUPT STATUS	*/
	case 0x0a: return 2;		/* READ ID			*/
	case 0x0d: return 6;		/* FORMAT TRACK			*/
	case 0x0f: return 3;		/* SEEK				*/
	default:   return 1;		/* invalid			*/
	}
}

static const char *cmd_name(BYTE c)
{
	switch (c & 0x1f) {
	case 0x03: return "SPECIFY";
	case 0x04: return "SENSE DRIVE";
	case 0x05: return "WRITE DATA";
	case 0x06: return "READ DATA";
	case 0x07: return "RECALIBRATE";
	case 0x08: return "SENSE INT";
	case 0x0a: return "READ ID";
	case 0x0d: return "FORMAT";
	case 0x0f: return "SEEK";
	default:   return "INVALID";
	}
}

static void sio_set_dcd(int chip, int ch, int level);

static void fdc_result(int n)
{
	fdc.nres = n; fdc.ridx = 0; fdc.phase = 2;
	sio_set_dcd(0, 1, 1);		/* INT: result phase ready */
}

static void fdc_idle(void)
{
	fdc.phase = 0; fdc.ncmd = 0; fdc.nres = 0;
	sio_set_dcd(0, 1, 0);		/* INT released */
}

/* commands with no result phase still interrupt when the seek ends */
static void fdc_idle_int(void)
{
	fdc.phase = 0; fdc.ncmd = 0; fdc.nres = 0;
	sio_set_dcd(0, 1, 1);
}

/* the actual sector transfer, once both the command and the DMA are ready */
static void fdc_try_transfer(void)
{
	BYTE buf[SECSZ];
	int trk, head, sec, i, n, writing, fail = 0;
	WORD addr;

	if (!fdc.pending || !dma.enabled)
		return;

	writing = (fdc.cmd[0] & 0x1f) == 0x05;	/* WRITE DATA vs READ DATA */
	head = fdc.cmd[3];
	trk  = fdc.cmd[2];
	sec  = fdc.cmd[4];
	/* whichever end of the DMA is not the controller is the memory end:
	   reads use port B for memory, writes swap and use port A */
	addr = dma.a_io ? dma.b_addr : dma.a_addr;
	n    = (int) dma.len + 1;
	if (n > SECSZ)
		n = SECSZ;

	if (writing) {
		/* a short transfer must leave the rest of the sector alone */
		memset(buf, 0xe5, sizeof(buf));
		if (n < SECSZ)
			monarch_read_phys(trk, sec, buf);
		for (i = 0; i < n; i++)
			buf[i] = dma_read(addr + i);
		fail = monarch_write_phys(trk, sec, buf);
		TRC("fdc: write C=%02d H=%d R=%02d <- %04X (%d bytes)%s\r\n",
		    trk, head, sec, addr, n, fail ? "  FAILED" : "");
	} else {
		memset(buf, 0xe5, sizeof(buf));
		if (monarch_read_phys(trk, sec, buf))
			memset(buf, 0xe5, sizeof(buf));
		for (i = 0; i < n; i++)
			dma_write(addr + i, buf[i]);
		TRC("fdc: read  C=%02d H=%d R=%02d -> %04X (%d bytes)\r\n",
		    trk, head, sec, addr, n);
	}

	fdc.pending = 0;
	dma.enabled = 0;
	fdc.pcn = trk;

	/* ST1 bit 1 is Not Writable, how a protected diskette reports itself */
	fdc.res[0] = fail ? 0x40 : 0x00;	/* ST0 */
	fdc.res[1] = fail ? 0x02 : 0x00;	/* ST1 */
	fdc.res[2] = 0x00;			/* ST2 */
	fdc.res[3] = trk;
	fdc.res[4] = head;
	fdc.res[5] = sec + 1;
	fdc.res[6] = fdc.cmd[5];
	fdc_result(7);
}

static void fdc_execute(void)
{
	BYTE c = fdc.cmd[0] & 0x1f;

	if (monarch_trace) {
		int i;
		char b[64] = "", t[8];
		for (i = 0; i < fdc.ncmd; i++) {
			snprintf(t, sizeof(t), "%02X ", fdc.cmd[i]);
			strncat(b, t, sizeof(b) - strlen(b) - 1);
		}
		LOG(TAG, "fdc: %-12s %s\r\n", cmd_name(fdc.cmd[0]), b);
	}

	switch (c) {
	case 0x03:				/* SPECIFY: no result */
		fdc_idle();
		break;
	case 0x04:				/* SENSE DRIVE STATUS */
		/* ST3: ready + track0 if there, single sided (bit 3 clear) */
		fdc.res[0] = 0x20 | (fdc.pcn == 0 ? 0x10 : 0x00)
			     | (fdc.cmd[1] & 0x07);
		fdc_result(1);
		break;
	case 0x07:				/* RECALIBRATE */
		fdc.pcn = 0;
		fdc.seek_end = 1;
		fdc.st0_int = 0x20;
		fdc_idle_int();
		break;
	case 0x08:				/* SENSE INTERRUPT STATUS */
		if (fdc.seek_end) {
			fdc.res[0] = fdc.st0_int;
			fdc.seek_end = 0;
		} else
			fdc.res[0] = 0x80;	/* invalid: no interrupt */
		fdc.res[1] = fdc.pcn;
		fdc_result(2);
		break;
	case 0x0a:				/* READ ID */
		if (fdc.cmd[0] & 0x40) {
			/* MFM asked of an FM diskette: fail, which is how the
			   LDRBIOS tells single from double density */
			fdc.res[0] = 0x40;
			fdc.res[1] = 0x01;
		} else {
			fdc.res[0] = 0x00;
			fdc.res[1] = 0x00;
		}
		fdc.res[2] = 0x00;
		fdc.res[3] = fdc.pcn;
		fdc.res[4] = 0;
		fdc.res[5] = 1;
		fdc.res[6] = 0;			/* N=0 -> 128 byte sectors */
		fdc_result(7);
		break;
	case 0x0f:				/* SEEK */
		fdc.pcn = fdc.cmd[2];
		fdc.seek_end = 1;
		fdc.st0_int = 0x20;
		fdc_idle_int();
		break;
	case 0x05:				/* WRITE DATA */
	case 0x06:				/* READ DATA */
		fdc.pending = 1;
		fdc.phase = 1;			/* stay busy until it happens */
		fdc_try_transfer();
		break;
	default:
		fdc.res[0] = 0x80;		/* invalid command */
		fdc_result(1);
		break;
	}
}

static BYTE fdc_msr_in(void)
{
	BYTE m = MSR_RQM;

	switch (fdc.phase) {
	case 0: m = MSR_RQM; break;
	case 1: m = MSR_RQM | MSR_CB; break;
	case 2: m = MSR_RQM | MSR_DIO | MSR_CB; break;
	}
	return m;
}

static BYTE fdc_data_in(void)
{
	BYTE d;

	if (fdc.phase != 2)
		return 0x00;
	d = fdc.res[fdc.ridx++];
	if (fdc.ridx >= fdc.nres)
		fdc_idle();
	return d;
}

static void fdc_data_out(const BYTE data)
{
	if (fdc.phase == 0) {
		fdc.ncmd = 0;
		fdc.want = cmd_len(data);
		fdc.phase = 1;
	}
	if (fdc.ncmd < (int) sizeof(fdc.cmd))
		fdc.cmd[fdc.ncmd++] = data;
	if (fdc.ncmd >= fdc.want)
		fdc_execute();
}


/* ------------------------------------------------------- Z80 SIO (Z8440) */
/*
 *	Two SIOs. The vector table the XIOS installs is 16 entries at CB00
 *	and every entry belongs to one of them:
 *
 *	  SIO 0   data A B0, data B B1, control A B2, control B B3, base 00
 *	  SIO 1   data A B4, data B B5, control A B6, control B B7, base 10
 *
 *	The console is SIO 0 channel A -- which is why the loader's BIOS can
 *	poll B2 for RR0 bit 0 (Rx available) and bit 2 (Tx empty).
 *
 *	The interesting part is what Dynabyte hung off the modem pins:
 *
 *	  SIO 0 channel B, DCD  <-  the 8272 floppy controller's INT line
 *	  SIO 1 channel B, DCD  <-  the periodic clock tick
 *
 *	so both arrive as External/Status Change interrupts (vectors 02 and
 *	12), and the handlers tell them apart by testing RR0 bit 3.
 */

#define TICK_HZ		60	/* in the guest's own time */
#define CPU_HZ		4000000
#define TICK_TSTATES	(CPU_HZ / TICK_HZ)

/* interrupt cause codes as they appear in bits 3-1 of the SIO vector */
#define CAUSE_B_TX	0
#define CAUSE_B_EXT	1
#define CAUSE_B_RX	2
#define CAUSE_A_TX	4
#define CAUSE_A_EXT	5
#define CAUSE_A_RX	6

typedef struct {
	BYTE	wr[8];
	int	ptr;			/* register pointer, from WR0 bits 0-2 */
	BYTE	rx;
	int	rx_full;
	int	dcd, cts;
	int	ext_pend, rx_pend, tx_pend;
} sio_ch_t;

static struct {
	sio_ch_t ch[2];			/* [0] = channel A, [1] = channel B */
} sio[2];

#define NNETCON	3

static const struct { int chip, ch, port; } netcon[NNETCON] = {
	{ 0, 1, 4001 },			/* console 1: SIO 0 channel B */
	{ 1, 0, 4002 },			/* console 2: SIO 1 channel A */
	{ 1, 1, 4003 },			/* console 3: SIO 1 channel B */
};

static int lsock[NNETCON] = { -1, -1, -1 };
static int csock[NNETCON] = { -1, -1, -1 };
static int net_index(int chip, int ch);

static int ext_enabled(sio_ch_t *c) { return c->wr[1] & 0x01; }
static int rx_enabled(sio_ch_t *c)  { return (c->wr[1] & 0x18) != 0; }
static int tx_enabled(sio_ch_t *c)  { return c->wr[1] & 0x02; }

static void sio_update_int(void)
{
	int chip, cause = -1, found = -1;

	for (chip = 0; chip < 2 && cause < 0; chip++) {
		sio_ch_t *a = &sio[chip].ch[0], *b = &sio[chip].ch[1];

		if (a->rx_pend)       cause = CAUSE_A_RX;
		else if (a->tx_pend)  cause = CAUSE_A_TX;
		else if (a->ext_pend) cause = CAUSE_A_EXT;
		else if (b->rx_pend)  cause = CAUSE_B_RX;
		else if (b->tx_pend)  cause = CAUSE_B_TX;
		else if (b->ext_pend) cause = CAUSE_B_EXT;
		if (cause >= 0)
			found = chip;
	}
	if (cause < 0)
		return;

	/* the vector lives in WR2 of channel B; the cause replaces bits 3-1 */
	int_data = (sio[found].ch[1].wr[2] & 0xf1) | (cause << 1);
	int_int = true;
}

/* drive a modem input; the External/Status interrupt latches on a change */
static void sio_set_dcd(int chip, int ch, int level)
{
	sio_ch_t *c = &sio[chip].ch[ch];

	if (c->dcd == level)
		return;
	c->dcd = level;
	if (ext_enabled(c)) {
		c->ext_pend = 1;
		sio_update_int();
	}
}

/* the other modem input the XIOS watches: clear to send */
static void sio_set_cts(int chip, int ch, int level)
{
	sio_ch_t *c = &sio[chip].ch[ch];

	if (c->cts == level)
		return;
	c->cts = level;
	if (ext_enabled(c)) {
		c->ext_pend = 1;
		sio_update_int();
	}
}

static BYTE sio_rr(sio_ch_t *c)
{
	switch (c->ptr) {
	case 0:
		return (BYTE) ((c->rx_full ? 0x01 : 0x00)	/* Rx available */
			     | 0x04				/* Tx empty     */
			     | (c->dcd ? 0x08 : 0x00)		/* DCD          */
			     | (c->cts ? 0x20 : 0x00));		/* CTS          */
	case 1:
		return 0x01;					/* All Sent     */
	case 2:
		return c->wr[2];
	default:
		return 0x00;
	}
}

static void sio_ctrl_out(int chip, int ch, BYTE data)
{
	sio_ch_t *c = &sio[chip].ch[ch];

	if (c->ptr == 0) {
		c->wr[0] = data;
		c->ptr = data & 0x07;
		switch ((data >> 3) & 0x07) {
		case 1:					/* reset ext/status ints */
		case 2:
			c->ext_pend = 0;
			break;
		case 3:					/* channel reset */
			memset(c, 0, sizeof(*c));
			break;
		case 5:					/* reset Tx int pending */
			c->tx_pend = 0;
			break;
		case 7:					/* return from interrupt */
			break;
		default:
			break;
		}
		if (((data >> 3) & 0x07) == 2)
			c->ext_pend = 0;
		sio_update_int();
		return;
	}
	c->wr[c->ptr] = data;
	c->ptr = 0;
	sio_update_int();
}

static BYTE sio_ctrl_in(int chip, int ch)
{
	sio_ch_t *c = &sio[chip].ch[ch];
	BYTE d = sio_rr(c);

	c->ptr = 0;
	/*
	 *	The tick is a pulse, not a level: latch it until the status
	 *	register has been read, then drop it so the next tick is a
	 *	fresh transition. The floppy's INT on SIO 0 channel B is a
	 *	genuine level and is left alone.
	 */
	if (chip == 1 && ch == 1) {
		if (c->dcd)
			c->dcd = 0;
	}
	return d;
}

static BYTE sio_data_in(int chip, int ch)
{
	sio_ch_t *c = &sio[chip].ch[ch];
	BYTE d = c->rx;

	c->rx_full = 0;
	c->rx_pend = 0;
	sio_update_int();
	return d;
}

static void sio_data_out(int chip, int ch, BYTE data)
{
	if (chip == 0 && ch == 0) {		/* console 0: this terminal */
		putchar(data & 0x7f);
		fflush(stdout);
	} else {				/* consoles 1-3: their sockets */
		int i = net_index(chip, ch);
		BYTE c = data & 0x7f;

		if (i >= 0 && csock[i] >= 0)
			if (write(csock[i], &c, 1) < 0 && errno != EAGAIN) {
				close(csock[i]);
				csock[i] = -1;
			}
	}
	if (tx_enabled(&sio[chip].ch[ch])) {
		sio[chip].ch[ch].tx_pend = 1;
		sio_update_int();
	}
}



/* ------------------------------------------------- Winchester controller */

#define WIN_SECSZ	1024		/* eight CP/M records per sector */
#define WIN_RECSZ	128
#define WIN_HEADS	16
#define WIN_SPT		256		/* generous: the mapping only has to
					   be consistent, not to match iron */
#define WIN_LABEL_CYL	0xffff

static int wfd = -1;
char monarch_winchester[MAX_LFN] = "disks/winchester.img";

static struct {
	BYTE	sel;			/* E1: what E2 means next	*/
	int	acnt;			/* address bytes still expected	*/
	BYTE	head, sec;
	WORD	cyl;
	BYTE	buf[2048];		/* what a read left behind	*/
	int	bptr;			/* where E2 reads from		*/
	int	writing;
	int	done;
	BYTE	label[2048];
} win;

static void sio_set_dcd(int chip, int ch, int level);

/* one consistent address -> offset map; the DPBs we hand out stay inside it */
static long win_off(WORD cyl, BYTE head, BYTE sec)
{
	return (((long) cyl * WIN_HEADS + head) * WIN_SPT + sec) * WIN_SECSZ;
}

/* the E3 strobe steps a record at a time inside the physical sector */

/*
 * The label the driver validates before it will touch the drive: the string
 * DYNABYTE, a version compared with >=, sixteen parameters, and then four
 * fifteen-byte records which are plain CP/M disk parameter blocks.
 */
/* kept so the image builder and the driver agree on the format */
static void win_build_label(void)
{
	static const BYTE dpb[15] = {
		0x40, 0x00,		/* SPT   64 records per track	*/
		0x04,			/* BSH   2 KB blocks		*/
		0x0f,			/* BLM				*/
		0x00,			/* EXM				*/
		0xff, 0x03,		/* DSM   1023 -> 2 MB		*/
		0xff, 0x01,		/* DRM   512 directory entries	*/
		0xff, 0xff,		/* AL0 AL1			*/
		0x00, 0x00,		/* CKS   0, it is not removable	*/
		0x00, 0x00		/* OFF				*/
	};
	int i;

	memset(win.label, 0, sizeof(win.label));
	memcpy(win.label, "DYNABYTE", 8);
	win.label[8] = '2';
	win.label[9] = '1';
	/* 16 parameter bytes at offset 10, then the four DPBs at 512 */
	for (i = 0; i < 4; i++)
		memcpy(win.label + 512 + i * 15, dpb, 15);
}

static int win_open(void)
{
	wfd = open(monarch_winchester, O_RDWR);
	if (wfd < 0) {
		LOGW(TAG, "no %s, drives B: and C: will not select",
		     monarch_winchester);
		return 1;
	}
	return 0;
}

static void win_read_sector(void)
{
	memset(win.buf, 0xe5, sizeof(win.buf));
	if (wfd < 0)
		return;
	if (lseek(wfd, win_off(win.cyl, win.head, win.sec), SEEK_SET) < 0)
		return;
	/*
	 * More than one sector: the driver reads the drive label by strobing
	 * E3 to step through fields well past the first 128 bytes, so the
	 * board plainly buffers more than a sector at a time.
	 */
	if (read(wfd, win.buf, sizeof(win.buf)) < 0)
		memset(win.buf, 0xe5, sizeof(win.buf));
}

static void win_write_sector(void)
{
	if (wfd < 0 || win.cyl == WIN_LABEL_CYL)
		return;
	if (lseek(wfd, win_off(win.cyl, win.head, win.sec), SEEK_SET) < 0)
		return;
	if (write(wfd, win.buf, WIN_SECSZ) != WIN_SECSZ)
		LOGW(TAG, "winchester write failed");
}

/* the driver kicks the operation off, and the board answers with an interrupt */
static void win_go(void)
{
	if (win.writing)
		win_write_sector();
	else
		win_read_sector();
	win.bptr = 0;
	TRC("win: %s cyl=%04X head=%d sec=%d PC=%04X\r\n",
	    win.writing ? "write" : "read ", win.cyl, win.head, win.sec, PC);
	win.writing = 0;
	win.done = 2;				/* answer a couple of ticks later */
}

/* a sector the CPU has just filled goes back to the image */
static void win_flush(void)
{
	if (wfd < 0)
		return;
	if (lseek(wfd, win_off(win.cyl, win.head, win.sec), SEEK_SET) < 0)
		return;
	if (write(wfd, win.buf, WIN_SECSZ) != WIN_SECSZ)
		LOGW(TAG, "winchester write failed");
}

/* called from the machine tick: the board finishing its business */
static void win_poll(void)
{
	if (win.done && --win.done == 0)
		sio_set_dcd(0, 0, 1);		/* completion -> SIO 0 chA DCD */
	if (dma.int_pend) {			/* Z8410 end of block */
		dma.int_pend = 0;
		int_data = dma.ivec;
		int_int = true;
	}
}

static BYTE win_e0_in(void)  { return 0x00; }
static BYTE win_e3_in(void)  { return 0x00; }
static BYTE win_e4_in(void)  { return 0x90; }	/* ready, no error */
static BYTE win_e9_in(void)  { return 0x00; }

static int win_rdlog;

static BYTE win_data_in(void)
{
	BYTE d = win.buf[win.bptr & (sizeof(win.buf) - 1)];

	if (monarch_trace && win_rdlog < 700) {
		win_rdlog++;
		LOG(TAG, "win: E2 read [%04X] = %02X '%c'\r\n", win.bptr, d,
		    (d >= 32 && d < 127) ? d : '.');
	}
	win.bptr++;
	return d;
}

static void win_data_out(const BYTE data)
{
	if (win.acnt > 0) {			/* head, cyl hi, cyl lo, sector */
		switch (win.acnt) {
		case 4: win.head = data; break;
		case 3: win.cyl = (win.cyl & 0x00ff) | (data << 8); break;
		case 2: win.cyl = (win.cyl & 0xff00) | data; break;
		case 1: win.sec = data; break;
		}
		win.acnt--;
		return;
	}
	if (win.bptr < (int) sizeof(win.buf))	/* sector data on its way out */
		win.buf[win.bptr++] = data;
	win.writing = 1;
}

static void win_cmd_out(const BYTE data)
{
	win.sel = data;
	if (data == 0x04) {			/* an address follows */
		win.acnt = 4;
		return;
	}
	if (data == 0x00)			/* rewind the buffer */
		win.bptr = 0;
	if (data == 0x01)			/* go */
		win_go();
}

static void win_ctl_out(const BYTE data)
{
	if (data == 0x0b)			/* arm for the next operation */
		win.bptr = 0;
	if (data == 0x09)			/* the ISR acknowledging us */
		sio_set_dcd(0, 0, 0);
}

static void win_ptr_out(const BYTE data)
{
	(void) data;				/* written as a countdown */
	win.bptr += WIN_RECSZ;
	if (win.bptr >= (int) sizeof(win.buf))
		win.bptr = 0;
}

static void win_sel_out(const BYTE data) { (void) data; }
static void win_8253_out(const BYTE data) { (void) data; }


/*
 * The Winchester moves its data through the Z8410 like the floppy does, only
 * with the controller's data port on the I/O end instead of the FDC's.
 */
static void win_try_dma(void)
{
	WORD io_addr, mem_addr;
	int i, n, to_memory;

	if (!dma.enabled)
		return;
	io_addr  = dma.a_io ? dma.a_addr : dma.b_addr;
	mem_addr = dma.a_io ? dma.b_addr : dma.a_addr;
	if ((io_addr & 0xff) != 0xe2)		/* not us */
		return;

	/* the memory end is the destination on a read */
	to_memory = dma.a_to_b ? dma.a_io : !dma.a_io;

	n = (int) dma.len + 1;
	if (n > (int) sizeof(win.buf) - win.bptr)
		n = (int) sizeof(win.buf) - win.bptr;

	for (i = 0; i < n; i++) {
		if (to_memory)
			dma_write(mem_addr + i, win.buf[win.bptr + i]);
		else
			win.buf[win.bptr + i] = dma_read(mem_addr + i);
	}
	if (!to_memory)
		win_flush();

	TRC("win: dma %s %d bytes, buffer %04X <-> memory %04X\r\n",
	    to_memory ? "->mem" : "<-mem", n, win.bptr, mem_addr);

	win.bptr += n;
	dma.enabled = 0;
	if (dma.int_eob)
		dma.int_pend = 1;		/* answered on the next tick */
}

/* ------------------------------------------------ consoles 1-3 over TCP */

static void net_listen(void)
{
	struct sockaddr_in a;
	int i, on = 1;

	for (i = 0; i < NNETCON; i++) {
		if ((lsock[i] = socket(AF_INET, SOCK_STREAM, 0)) < 0)
			continue;
		setsockopt(lsock[i], SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = htons(netcon[i].port);
		if (bind(lsock[i], (struct sockaddr *) &a, sizeof(a)) < 0 ||
		    listen(lsock[i], 1) < 0) {
			close(lsock[i]);
			lsock[i] = -1;
			continue;
		}
		fcntl(lsock[i], F_SETFL, O_NONBLOCK);
		LOG(TAG, "console %d listening on port %d\r\n", i + 1, netcon[i].port);
	}
}

/* accept anyone waiting, and pull whatever they typed into the SIO */
static void net_poll(void)
{
	struct pollfd p[1];
	unsigned char c;
	sio_ch_t *ch;
	int i, n, on = 1;

	for (i = 0; i < NNETCON; i++) {
		if (csock[i] < 0 && lsock[i] >= 0) {
			n = accept(lsock[i], NULL, NULL);
			if (n >= 0) {
				setsockopt(n, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
				fcntl(n, F_SETFL, O_NONBLOCK);
				csock[i] = n;
				sio_set_cts(netcon[i].chip, netcon[i].ch, 1);
			}
		}
		if (csock[i] < 0)
			continue;

		ch = &sio[netcon[i].chip].ch[netcon[i].ch];
		if (ch->rx_full)
			continue;
		p[0].fd = csock[i];
		p[0].events = POLLIN;
		p[0].revents = 0;
		if (poll(p, 1, 0) <= 0 || !(p[0].revents & POLLIN))
			continue;
		n = read(csock[i], &c, 1);
		if (n <= 0) {
			if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
				close(csock[i]);
				csock[i] = -1;
				sio_set_cts(netcon[i].chip, netcon[i].ch, 0);
			}
			continue;
		}
		if (c == 255) {			/* skip a telnet IAC sequence */
			unsigned char junk[2];
			read(csock[i], junk, 2);
			continue;
		}
		if (c == 0)
			continue;
		ch->rx = c;
		ch->rx_full = 1;
		if (rx_enabled(ch)) {
			ch->rx_pend = 1;
			sio_update_int();
		}
	}
}

static int net_index(int chip, int ch)
{
	int i;

	for (i = 0; i < NNETCON; i++)
		if (netcon[i].chip == chip && netcon[i].ch == ch)
			return i;
	return -1;
}

/* stdin -> SIO 0 channel A */
static void sio_poll_console(void)
{
	struct pollfd p[1];
	sio_ch_t *c = &sio[0].ch[0];
	unsigned char ch;

	if (c->rx_full)
		return;
	p[0].fd = fileno(stdin);
	p[0].events = POLLIN;
	p[0].revents = 0;
	if (poll(p, 1, 0) <= 0 || !(p[0].revents & POLLIN))
		return;
	if (read(fileno(stdin), &ch, 1) != 1)
		return;
	c->rx = ch;
	c->rx_full = 1;
	if (rx_enabled(c)) {
		c->rx_pend = 1;
		sio_update_int();
	}
}

/*
 *	The host timer only provides opportunities; whether a tick is due is
 *	decided by how many Z80 T-states have gone by, so the guest sees a
 *	steady 60 Hz in its own time no matter how fast the host runs. Doing
 *	it the other way round is what made the clock depend on the
 *	simulator's speed throttle.
 */
/*
 *	Called from the CPU loop every MACHINE_TICK emulated T-states, so the
 *	guest sees a steady 60 Hz in its own time however fast the host runs.
 */
void machine_tick(void)
{
	sio_set_dcd(1, 1, 1);			/* clock tick -> SIO 1 ch B DCD */
	win_poll();				/* last: its vector must not be
						   overwritten by the tick's */
}

/* the host timer only samples the keyboard; it paces nothing */
static void console_poll_sig(int sig)
{
	(void) sig;
	sio_poll_console();
	net_poll();
}

static void tick_start(void)
{
	struct itimerval t;
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = console_poll_sig;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGVTALRM, &sa, NULL);

	t.it_value.tv_sec = 0;
	t.it_value.tv_usec = 20000;		/* 50 Hz keyboard sampling */
	t.it_interval = t.it_value;
	setitimer(ITIMER_VIRTUAL, &t, NULL);
}

static BYTE sio0a_data_in(void)  { return sio_data_in(0, 0); }
static BYTE sio0b_data_in(void)  { return sio_data_in(0, 1); }
static BYTE sio0a_ctrl_in(void)  { return sio_ctrl_in(0, 0); }
static BYTE sio0b_ctrl_in(void)  { return sio_ctrl_in(0, 1); }
static BYTE sio1a_data_in(void)  { return sio_data_in(1, 0); }
static BYTE sio1b_data_in(void)  { return sio_data_in(1, 1); }
static BYTE sio1a_ctrl_in(void)  { return sio_ctrl_in(1, 0); }
static BYTE sio1b_ctrl_in(void)  { return sio_ctrl_in(1, 1); }

static void sio0a_data_out(const BYTE d) { sio_data_out(0, 0, d); }
static void sio0b_data_out(const BYTE d) { sio_data_out(0, 1, d); }
static void sio0a_ctrl_out(const BYTE d) { sio_ctrl_out(0, 0, d); }
static void sio0b_ctrl_out(const BYTE d) { sio_ctrl_out(0, 1, d); }
static void sio1a_data_out(const BYTE d) { sio_data_out(1, 0, d); }
static void sio1b_data_out(const BYTE d) { sio_data_out(1, 1, d); }
static void sio1a_ctrl_out(const BYTE d) { sio_ctrl_out(1, 0, d); }
static void sio1b_ctrl_out(const BYTE d) { sio_ctrl_out(1, 1, d); }

/* --------------------------------------------------------- board latch */


static void latch_out(const BYTE data)
{
	TRC("latch: port 8C <- %02X\r\n", data);
}

/* ------------------------------------------------------------- plumbing */


/*
 *	MMU. Port D5 writes one slot of the map set currently selected for
 *	programming; the slot rides on A8-A15, not in the port number.
 *	Port 87 picks the set being programmed, port 85 the set the CPU runs
 *	from -- SELMEMORY writes the MP/M bank number straight to 85.
 */
static void mmu_map_out(const BYTE data)
{
	mmu_write_map(io_porth >> 5, data);
	TRC("mmu: set %02X slot %d -> page %02X\r\n",
	    mmu_prog, io_porth >> 5, data);
}

static void mmu_prog_out(const BYTE data)
{
	mmu_select_prog(data);
	TRC("mmu: programming set %02X\r\n", data);
}

static void mmu_run_out(const BYTE data)
{
	mmu_select_run(data);
	TRC("mmu: running set %02X  PC=%04X\r\n", data, PC);
}

/* written around DMA transfers and bank switches; no effect modelled */
static void mmu_ctl_out(const BYTE data)
{
	TRC("mmu: ctl port %02X (A8-15 %02X) <- %02X\r\n", io_port, io_porth, data);
}

in_func_t *const port_in[256] = {
	[0xb0] = sio0a_data_in,
	[0xb1] = sio0b_data_in,
	[0xb2] = sio0a_ctrl_in,
	[0xb3] = sio0b_ctrl_in,
	[0xb4] = sio1a_data_in,
	[0xb5] = sio1b_data_in,
	[0xb6] = sio1a_ctrl_in,
	[0xb7] = sio1b_ctrl_in,
	[0xe0] = win_e0_in,
	[0xe2] = win_data_in,
	[0xe3] = win_e3_in,
	[0xe4] = win_e4_in,
	[0xe9] = win_e9_in,
	[0xd0] = fdc_msr_in,
	[0xd1] = fdc_data_in,
	[0xd6] = dma_in,
};

out_func_t *const port_out[256] = {
	[0x85] = mmu_run_out,
	[0x86] = mmu_ctl_out,
	[0x87] = mmu_prog_out,
	[0x8c] = latch_out,
	[0x8e] = mmu_ctl_out,
	[0xa2] = mmu_ctl_out,
	[0xac] = mmu_ctl_out,
	[0xb0] = sio0a_data_out,
	[0xb1] = sio0b_data_out,
	[0xb2] = sio0a_ctrl_out,
	[0xb3] = sio0b_ctrl_out,
	[0xb4] = sio1a_data_out,
	[0xb5] = sio1b_data_out,
	[0xb6] = sio1a_ctrl_out,
	[0xb7] = sio1b_ctrl_out,
	[0xe0] = win_sel_out,
	[0xe1] = win_cmd_out,
	[0xe2] = win_data_out,
	[0xe3] = win_ptr_out,
	[0xe4] = win_ctl_out,
	[0xec] = win_8253_out,
	[0xed] = win_8253_out,
	[0xef] = win_8253_out,
	[0xd1] = fdc_data_out,
	[0xd5] = mmu_map_out,
	[0xd6] = dma_out,
};

void init_io(void)
{
	win_build_label();
	win_open();
	net_listen();
	sio[0].ch[0].cts = 1;	/* console 0 is this terminal */
	dma_reset();
	fdc_idle();
	fdc.pcn = 0;
	tick_start();
}

void exit_io(void)
{
	int i;

	for (i = 0; i < NNETCON; i++) {
		if (csock[i] >= 0) close(csock[i]);
		if (lsock[i] >= 0) close(lsock[i]);
	}
	if (wfd >= 0)
		close(wfd);
	if (dfd >= 0)
		close(dfd);
}
