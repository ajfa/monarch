# Dynabyte Monarch, Z80 side — reverse engineering notes

No technical manual, no schematic and no boot PROM dump exists for this machine.
Everything below was recovered from Dynabyte's own code: the LDRBIOS inside
`FPYMPM.LDR` and `XIOS1.SPR`, both off the MP/M II DV1.4E distribution
diskettes, plus live tracing in `monarchsim`.

## Status

`monarchsim` boots the **real, unmodified Monarch diskette**. The synthesised
boot PROM loads the reserved tracks, Dynabyte's MPMLDR runs, finds and loads
`MPM.SYS` from the original image, and **Dynabyte's own XIOS1 initialises and
prints its banner**:

```
Initializing MMU Bank 0 1 2 3 4 , I/O Ports , Winchester, DMA, Clock
Initialization Complete.
```

It then wedges: the disk process blocks on XDOS Flag 23 and nothing wakes it,
because interrupts are not modelled yet. See "next step".

## The boot PROM, which is missing but fully determined

The two reserved tracks of the MP/M II diskette are, byte for byte, the file
`FPYMPM.LDR`: 52 sectors, 6656 bytes, physical order, no skew. That image opens
with

```
LD HL,0B0EDh      ; the opcodes for LDIR
LD (00FEh),HL
LD HL,1012h
LD DE,0100h
LD BC,2F00h
JP 00FEh          ; run the LDIR, then fall through into 0100h
```

`1012h` is `1000h + 18`, the byte just past this stub, so the PROM read the
reserved tracks to `1000h` and jumped there. That is all it had to do, and it is
what `simctl.c` synthesises.

## Port map

| port | device |
|---|---|
| `85`, `86`, `87`, `8E`, `A2` | MMU / bank control |
| `8C` | FDC reset + drive select latch (see below) |
| `B0`,`B2`,`B3` | Z80 SIO #1 — console. `B0` data, `B2`/`B3` control |
| `B4`,`B6`,`B7` | Z80 SIO #2 — further consoles |
| `D0` | 8272/µPD765 FDC — Main Status Register |
| `D1` | 8272 FDC — data register, programmed I/O |
| `D3` | 8272 FDC — data register on the DMA path (DMA port A, fixed) |
| `D6` | Zilog Z8410 DMA |
| `E0`–`EF` | Winchester controller |

### Floppy

Plain 8272. The LDRBIOS and the XIOS both build datasheet command blocks:
SPECIFY 03, SENSE DRIVE STATUS 04, READ DATA 06/46, RECALIBRATE 07, SENSE
INTERRUPT STATUS 08, READ ID 0A, SEEK 0F. Density is detected by issuing READ ID
in FM and treating failure as "this is an MFM diskette".

Data never moves through the CPU. The Z80 DMA is programmed with a 12-byte
block written to port `D6`:

```
7D            WR0  transfer, A->B, port A address and block length follow
xx 00         port A start address  (00D3 = the FDC data register)
ll hh         block length, count-1
3C            WR1  port A is I/O, fixed
10            WR2  port B is memory, incrementing
8D            WR4  byte mode, port B address follows
ll hh         port B start address = the CP/M DMA buffer
CF            WR6  Load
8B            WR6  Reinitialise status byte
```

then `87` (WR6 Enable DMA) once the FDC command has been sent.

### Port 8C is the FDC reset

Recovered from the XIOS's timeout path: if the Main Status Register still shows
CB (busy) after four polls, it does

```
XOR A
OUT (8Ch),A               ; 8C <- 00
LD A,(drive); INC A; OR 2Ch
OUT (8Ch),A               ; 8C <- 2C | drive+1
```

and retries the command. So `8C` bit pattern `2C` is the FDC's reset/enable line
with the drive number in the low bits. The boot sequence writes `00`, `2C`,
SPECIFY, then `2D`.

## The interrupt design, and why the disk wedges

The Z80 runs in IM 2 with `I = CBh`, so a 16-entry vector table at `CB00`. Every
one of those sixteen vectors belongs to a **Z80 SIO** — two chips, four
channels, four vectors each. There is no vector of its own for the floppy.

The 8272's INT line is instead wired into the **DCD input of SIO channel A**.
The disk interrupt therefore arrives as an External/Status Change interrupt
(vector `02`, handler at `CA67`), which does:

```
LD A,10h : OUT (0B3h),A     ; WR0 = Reset External/Status Interrupts
IN A,(0B3h)                 ; read RR0
AND 20h                     ; bit 5 = CTS  -> modem status variable
...
AND 08h                     ; bit 3 = DCD  -> this is the FDC interrupt
   IN A,(0D1h)              ; if set, take the result byte
...
LD A,038h : OUT (0B2h),A    ; WR0 command 7 = Return From Interrupt
LD E,17h  : ... Flag Set    ; wake whoever is waiting on flag 23
```

and the disk driver, having issued its command, is sitting in

```
LD E,17h        ; flag 23
LD C,084h       ; XDOS 132 = Flag Wait
CALL ...
```

So with no interrupt the flag is never set, the disk process never wakes, and
the MP/M dispatcher idles forever — which is exactly where a SIGINT into the ICE
caught it, at `DE8F`, mid context-restore inside XDOS.

## Next step

Model the two Z80 SIOs: RR0 status, the WR0 command set actually used
(`10` reset ext/status, `38` return from interrupt, `F0`), IM 2 vector
generation, and the FDC INT feeding RR0 bit 3 on channel A. Then the clock
tick — the handler near `C942` counts to 40 and sets flag 25, so something is
interrupting periodically and must be found. After that, the MMU (`85`/`86`/`87`
/`8E`/`A2`) has to become real, because MP/M II is banked and currently runs on
flat 64K.

## Rig

```
~/monarch/z80pack/monarchsim/     the machine: srcsim/{sim.h,simio.c,simctl.c}
~/monarch/re/                     disassemblies and the SPR relocator
   reloc_spr.py                   relocate an MP/M .SPR to its load page
   ldrbios.asm                    the loader's BIOS, 1700-1A0D
   xios.asm                       XIOS1.SPR relocated to AF00 and disassembled
~/monarch/probe.py                boot, SIGINT into the ICE, run ICE commands
```

Trace every FDC command and DMA program with `monarch_trace = 1` in `simio.c`.


---

# Update: SIOs and clock done, MP/M reaches its prompt

`monarchsim` now boots the unmodified Monarch diskette all the way to the MP/M
command prompt, and runs Dynabyte's own transient programs off it:

```
, Clock
0A>dir
A:DIR     .PRL
 : COPY1REV SUB
A: QA1      COM
System Files Exist
0A>
```

## What was added

**Two Z80 SIOs.** SIO 0: data A `B0`, data B `B1`, control A `B2`, control B
`B3`, vector base `00`. SIO 1: `B4`/`B5`/`B6`/`B7`, vector base `10`. The
console is SIO 0 channel A, which is why the loader's BIOS could poll `B2` for
RR0 bit 0 and bit 2. Modelled: the RR0 status bits actually read, the WR0
commands actually issued (`10` reset ext/status, `38` return from interrupt,
channel reset), the register pointer, and IM 2 vector generation with the cause
in bits 3-1.

The vector layout is confirmed by the handlers themselves: vector `02` is
Ch B External/Status and its handler talks to `B3`; vector `08` is Ch A Transmit
and its handler loads `C` with `B2`; vector `18` is the same on the second chip
and loads `B6`. All sixteen entries line up.

**The two interrupts that are not serial at all.** Dynabyte hung both the floppy
controller and the clock off modem-control pins:

* SIO 0 channel B, DCD  <-  the 8272's INT line
* SIO 1 channel B, DCD  <-  the periodic clock tick

Both therefore arrive as External/Status Change interrupts and the handlers tell
them apart by testing RR0 bit 3. The disk handler then does XDOS Flag Set on
flag 23, which is what the disk driver is blocked on; the clock handler runs the
tick counter and sets flag 25 every fortieth tick.

**The clock needs a realistic CPU speed.** With the simulator free-running, the
XIOS's calibrated wait for the first tick expires before one can arrive and it
prints `NO CLOCK!` and rings the bell. At `CPU_SPEED 4` (4 MHz) the test passes
and the banner ends cleanly at `, Clock`.

## What is left: the MMU

`DIR` works but drops characters, `SDIR` loads and prints nothing, and `MPMSTAT`
does not survive. This is not a serial problem. With the MMU ports traced, one
boot plus one `SDIR` issues **3178 MMU writes**:

```
   2584  port 85 <- 00
    191  port 8E <- 01
    191  port 86 <- 00
      7  port 85 <- FF
      1  port 87 <- 5F, 60, 61, 62, 63, 64   (consecutive page numbers)
```

So MP/M II is banking hard -- port `87` is being fed a page map -- and the
simulator is still running on flat 64K, which means every bank switch silently
lands in the same memory and the operating system overwrites itself. Only
programs that fit the bank 0 user segment survive, which is exactly the
observed symptom.

Next: make `85`/`86`/`87`/`8E`/`A2` a real page-mapping MMU. The XIOS's
SELMEMORY entry point and the `Initializing MMU Bank 0 1 2 3 4` code around
`B040`-`B0A0` and `C2B0`-`C3E0` are where the semantics are written down.


---

# Update: the MMU is decoded and implemented, with one regression open

## What the MMU is

Traced with the high address byte of the I/O cycle exposed (z80core did not
pass it to port handlers; `io_porth` was added for that), one boot produces:

```
   1233  port AC (A8-15 01) <- 01        \ written in pairs, around
   1233  port AC (A8-15 00) <- 00        / bank switches
    349  port 85 (A8-15 00) <- 00
    102  port D5 (A8-15 C0) <- 06
     99  port D5 (A8-15 A0) <- 05
     99  port D5 (A8-15 80) <- 04
     96  port D5 (A8-15 F0) <- 07
     35  port 8E (A8-15 01) <- 01        \ before every DMA transfer
     35  port 86 (A8-15 00) <- 00        /
      1  port D5 (A8-15 A0) <- 0D, 13, 19, 1F
      1  port D5 (A8-15 80) <- 0C, 12, 18, 1E
```

**Port D5 is the page map, and the slot being written rides on A8-A15, not on
the port number.** The XIOS lays down the identity map with

```
LD A,0FFh : OUT (087h),A
LD DE,0008h : LD B,000h : LD C,0D5h
loop:   OUT (C),D          ; slot = B >> 5, physical page = D
        INC D
        LD A,020h : ADD A,B : LD B,A
        DEC E : JR NZ,loop
```

Eight slots of 8 KB. The layout that falls out of the trace is exactly the
classic MP/M II one:

* slots 6 and 7 (`C000-FFFF`) pinned to pages 06/07 -- the 16 KB common area
* slots 0-5 (`0000-BFFF`) swapped -- 48 KB banks, consecutive banks six pages
  apart (`0C/0D`, `12/13`, `18/19`, `1E/1F`)

Ports `85`, `86`, `87`, `8E`, `A2`, `AC` are secondary: `8E`/`86` are written
before every DMA transfer, `AC` in pairs around bank switches, `85` around the
inter-bank block moves. They are accepted and ignored for now.

The XIOS also *sizes* memory through this map: it walks pages up to `7F`
mapping each into slot 0 and testing it, which is where the
`Initializing MMU Bank 0 1 2 3 4` count comes from. Physical memory is
therefore modelled as 1 MB, 128 pages of 8 KB.

`simmem.h`/`simmem.c` now implement that page map, and `simio.c` decodes
port D5 with `io_porth >> 5` as the slot.

## Open regression

With the page map in, **the behaviour became dependent on whether the I/O
trace is compiled in**:

* `monarch_trace = 1` -- the loader prints its whole memory segment table and
  the XIOS runs, as before.
* `monarch_trace = 0` -- the loader stops three lines into that table and the
  Z80 runs away into zeroed RAM (caught at `PC=093A` and `PC=8119`, executing
  NOPs, with a plausible stack).

Ruled out so far, each by direct experiment rather than reasoning:

* **the clock tick** -- disabling `setitimer` entirely reproduces the failure
  unchanged, so the timer and its signal are not involved;
* **truncated disk reads** -- `SA_RESTART` plus an `EINTR` retry loop were
  added, and an unconditional log of read failures and short DMA transfers
  fires zero times across a whole boot;
* **the page map itself** -- the first write to port D5 happens in the XIOS,
  which runs *after* the point where the loader now dies, so no map change has
  taken place yet when it fails.

That leaves the memory implementation itself or an ordering assumption in the
new `simmem`, with the trace only shifting the timing enough to hide it. The
next thing to check is whether the identity map is actually in place for the
whole of the loader's run -- a hardware breakpoint on a write to the loader's
own code, or simply asserting `pagemap[i] == i` on every access while the
loader is live, would settle it. `re/simio.current.c` is the current
`simio.c`.


---

# Update: the identity map is confirmed, and the real culprit was the throttle

## The map was never the problem

Two probes settled it: `mmu_dump()` right after the boot load, and an
unconditional log of every page-map write with the PC that issued it.

```
MAP after boot load: 0 1 2 3 4 5 6 7  (identity)
MAPWRITE count: 0
```

The map is identity when the loader runs and **not one write to it happens
before the failure point**. With an identity map `mmu_phys(addr) == addr`, so
memory behaves exactly as the previous flat 64 KB did. The MMU is exonerated.

Calling it an "MMU regression" was wrong: the trace had never been switched off
before that build, so the fault was not new -- it had simply always been hidden.

## What it actually was

Isolated by running the four combinations:

| CPU speed | trace | result |
|---|---|---|
| unlimited | off | works |
| 4 MHz | on | works |
| 4 MHz | off | **wedges**, still nothing after 75 s |

The trace makes the emulator slower than 4 MHz, so with it on the throttle
never engages. **z80pack's CPU speed limiting wedges the guest whenever it
actually takes effect.**

Related, and worth knowing regardless: **z80pack owns `SIGALRM`** -- `int_off()`
in `z80core/simint.c` sets it to `SIG_IGN`, and the ICE uses it to measure clock
frequency. The Monarch tick was moved to `SIGVTALRM`/`ITIMER_VIRTUAL` so it
stops fighting the core. (That was a genuine bug, but not this one: the wedge
reproduces with the tick disabled entirely.)

## The way around it

Run unthrottled and let the tick compensate. The XIOS's clock test waits a
bounded number of instructions for the first tick, so at unlimited speed a
60 Hz real-time tick is far too slow in emulated terms and it prints
`NO CLOCK!`. At `CPU_SPEED 0` with `TICK_HZ 2000` the test passes, `NO CLOCK!`
is gone, and the loader prints its whole memory segment table.

The proper fix, when there is time, is to drive the tick off the emulated
T-state counter instead of host time -- then it is 60 Hz in the guest's own
frame regardless of how fast the host runs, and none of this matters.

## Current frontier

With the real page map in place the machine now reaches

```
Initializing MMU
```

and stops there, inside the XIOS's memory sizing loop -- the one that walks
physical pages into slot 0 and tests them. That loop used to run against flat
memory that answered everywhere; it is now meeting a real 128-page map for the
first time. That is the next thing to work on, and it is a much better place to
be stuck than before.


---

# Update: the sizing loop is decoded, and port 85 is the missing piece

The memory sizing loop lives at `C392`-`C3C5` and is straightforward:

```
lc392:  OUT (087h),A            ; A = E = the running bank counter
        LD C,0D5h
        LD B,080h : LD H,004h : OUT (C),H   ; slot 4 <- page 04  \ keep the code
        LD B,0A0h : INC H     : OUT (C),H   ; slot 5 <- page 05  | and stack
        LD B,0C0h : LD H,006h : OUT (C),H   ; slot 6 <- page 06  | mapped while
        LD B,0F0h : INC H     : OUT (C),H   ; slot 7 <- page 07  / testing
        LD B,000h : OUT (C),D               ; slot 0 <- page D, the one on test
        OUT (085h),A
        LD HL,0000h
        XOR A  : LD (HL),A : CP (HL) : JR NZ,done    ; write 00, read back
        DEC A  : LD (HL),A : CP (HL) : JR NZ,done    ; write FF, read back
        INC D : INC E
        LD A,D : CP 080h : LD A,E : JR NZ,lc392      ; stop at page 80
done:   DEC E
        ...  LD A,E : SUB B : ... ADD HL,HL : ADD HL,HL : DEC HL
        LD (lc4EAh),HL          ; the memory size it settled on
```

So a page is "there" if address 0000 holds both 00 and FF after being written,
and the walk stops either at the first page that fails or at page 80. With 128
pages of real RAM the loop terminates on the page-80 limit, so the loop itself
is not what hangs.

**What hangs is what comes next.** After sizing, the XIOS does

```
        OUT (085h),A
        LD HL,0000h : LD DE,0001h : LD BC,0800h
        LD (HL),0E5h : LDIR         ; clear 2 KB of the selected bank
        LD A,0FFh : OUT (085h),A    ; back to normal
```

and a SIGINT into the ICE catches the machine at `PC=79E9` with `DE=E5E5` and
`HL=08E5` -- i.e. running inside the region that fill just wrote. The fill went
into the wrong bank and the XIOS overwrote itself.

`OUT (085h),A` is what selects which bank that fill lands in, and port 85 is
still being accepted and ignored. The same register appears in the inter-bank
block move found earlier:

```
sub_b069:  ...  DI : OUT (085h),A   ; select the other bank
           ...  LDIR                ; move 128 bytes
           XOR A : OUT (085h),A : EI ; back to bank 0
```

so 85 is an override that redirects the low window at another bank while the
main page map stays put -- MP/M's cross-bank move needs exactly that. It tracks
the D5 slot-0 writes in the sizing loop (both get the same counter), which is
consistent with a two-level scheme: 85 picks the bank, the page map picks the
page within it.

**Next action:** work out 85's exact semantics from `sub_b069` (which computes
its value from `(lc49Ch)` plus the low byte of `(lbc7Ah)`) and from this sizing
loop, then implement it. Until then the machine stops at `Initializing MMU`.


---

# Update: port 85 decoded, MMU finished, MP/M running on the reconstructed Monarch

`./check.py` from the monarchsim directory: **12/12**.

## Port 85, from SELMEMORY

No inference needed in the end -- MP/M's own SELMEMORY entry point says it. The
XIOS jump table is at `AF00`, SELMEMORY is entry 51:

```
AF33:   JP  0C766h

lc766h: INC BC : INC BC : INC BC   ; MP/M's memory descriptor is
        LD A,(BC)                  ; base, size, attributes, bank
        OUT (085h),A               ; the bank goes straight to port 85
        RET
```

**Port 85 is the bank select register**, and nothing else.

## Which makes the whole MMU fall out

Port 85 alone switches banks, yet slots 0-3 of the page map are programmed once
at init and never again. A single global page map cannot do that. So there is
**one page map per bank**, and:

* **port 87** selects the map set being *programmed*
* **port D5** writes one slot of it, the slot riding on A8-A15
* **port 85** selects the map set the CPU *runs* from

The ordering in the memory sizing loop confirms it: `OUT (087h),A` first, then
the D5 writes that fill that set in, then `OUT (085h),A` to run from it.

Implemented as `mapset[256][8]`, all sets starting as the identity map the boot
PROM would have left. The sizing loop then walks pages into slot 0 and reports
**exactly the five banks MPM.SYS declares** -- `Initializing MMU Bank 0 1 2 3 4`.

## And it fixes what it was supposed to fix

`DIR` used to drop characters, `SDIR` printed nothing and `MPMSTAT` did not
survive, because every bank switch landed in the same flat memory. All three now
work:

```
0A>sdir
Directory For Drive A:  User  0
ABORT    PRL     1k      5 Sys RW       ASM      PRL    10k     74 Sys RW
...
Total Bytes     =    212k  Total Records =    1571  Files Found =   31

0A>mpmstat
Memory is bank switched
BDOS disk file management is bank switched
```

31 files and 212k -- the real contents of the original diskette.

## The clock, done properly

`NO CLOCK!` turned out not to be a modelling error but a pacing one. The XIOS's
test is a pure poll:

```
LD HL,0BB8h                      ; 3000 tries
lc5de:  LD A,010h : OUT (0B7h),A ; reset ext/status on SIO 1 channel B
        IN A,(0B7h)              ; read RR0
        AND 008h                 ; DCD = the tick
        JR NZ,found
        DEC HL : LD A,H : OR L : JR NZ,lc5de
        ...print ".  NO CLOCK!"
```

Instrumented, the guest polls **every 69 T-states** -- 207,000 for the whole
loop -- while a host timer could only deliver a tick every **~4,000,000**
T-states, because `ITIMER_VIRTUAL` is clamped to the scheduler tick and the
unthrottled simulator runs at roughly a gigahertz-equivalent. A host timer
simply cannot pace a guest clock here.

So the tick now comes from the CPU loop itself. `z80core/simz80.c` calls a
`machine_tick()` hook every `MACHINE_TICK` T-states (guarded by `#ifdef` so the
other machines in the tree are unaffected), set to `4000000 / 60` -- a steady
60 Hz in the guest's own time, whatever the host does. The host timer is kept
only to sample the keyboard, which does not need to be cycle accurate.

`NO CLOCK!` is gone and the dayfile timestamps advance.

## State

Two changes live in `z80core` and are worth remembering: `io_porth`, exposing
the high byte of the I/O cycle (the Monarch's MMU needs it), and the
`MACHINE_TICK` hook. Both are additive and guarded.

Still open: the multiple consoles (the second SIO's channels are modelled but
not wired to anything), and the Winchester at `E0`-`EF`, which is what
`DYNASYS`, `DYNASTAT`, `FORMAT` and `TAPE` would need.


---

# Update: four consoles, and the Monarch is a multiuser machine again

`./check.py` from the monarchsim directory: **15/15**.

## The console map, read out of the running machine

The XIOS keeps a console -> device table; `(lc481h)` points at it. Dumped live
from the ICE it is `00 01 02 03`, so consoles 0-3 are devices 0-3. The device to
port decoder is `sub_c858`:

```
sub_c858h:  CP 008h : LD B,0B0h : JR C,lc862h
            LD B,090h : SUB 008h        ; devices 8+ live at 90
lc862h:     AND 0FEh : ADD A,A : ADD A,B
            BIT 0,D : JR Z,lc86bh : INC A
lc86bh:     LD C,A
```

that is, `port = B0 + ((n & 0xFE) * 2) + (n & 1)`, data there and status at +2:

| console | device | data | status | channel |
|---|---|---|---|---|
| 0 | 0 | B0 | B2 | SIO 0 channel A |
| 1 | 1 | B1 | B3 | SIO 0 channel B |
| 2 | 2 | B4 | B6 | SIO 1 channel A |
| 3 | 3 | B5 | B7 | SIO 1 channel B |

The decoder's `90` branch is why the machine could carry sixteen ports: devices
8 and up decode into a second block. Only four are configured here.

CONIN waits on flag *device+4*, which is exactly what the receive handlers set
(4, 5, 6, 7) -- so the flag numbers seen earlier were never device numbers, and
the interrupt vectors already lined up with the SIO model.

Consoles 1-3 are wired to TCP 4001-4003; console 0 stays on the terminal.

## What was missing: CTS

With the sockets attached, input reached the SIO and the receive interrupt fired
with the right vector, but nothing ever came back. Consoles 0 and 2 worked;
1 and 3 were mute.

The XIOS reads **RR0 bit 5, CTS, in every External/Status handler** and keeps it
per channel. A channel with nothing asserting CTS is a terminal that is not
there. Asserting CTS when a socket connects fixed it immediately -- and does it
properly, because the transition raises the External/Status interrupt, so the
newly attached console prints its banner and prompt the moment you connect:

```
$ telnet localhost 4001

Monarch MP/M II DV1.4E
1A>user 1
01:52:26 A:USER    .PRL  (User 0)
User Number = 1
1A>
```

Console 0 is marked permanently ready; 1-3 follow their sockets, and dropping a
connection deasserts CTS again.

## Where it stands

The Monarch runs Dynabyte's MP/M II off the original diskette with four
independent consoles, each with its own drive, user number and running program,
on hardware entirely reconstructed from Dynabyte's own code -- no schematic, no
technical manual, no boot PROM.

Still open: the Winchester at `E0`-`EF`, which is what `DYNASYS`, `DYNASTAT`,
`FORMAT` and `TAPE` need, and writing to the floppy (the FDC models READ but not
WRITE).


---

# Update: the diskette is writable

`./check.py`: **19/19**. The check now starts each run from a pristine copy of
the original diskette, because the guest can change it.

```
0A>pip a:hello.txt=con:
HOLA MONARCH DESDE MP/M
^Z
0A>dir
A: MPM      SYS : DYNASTAT COM : COPY1    SUB : COPY1REV SUB
A: QA1      COM : HELLO    TXT
0A>type hello.txt
HOLA MONARCH DESDE MP/M
```

and on the host, the image now contains both the directory entry `HELLO   TXT`
and the text.

## WRITE DATA

Command 05/45 goes down the same path as READ DATA: nine command bytes, the
transfer, then the seven result bytes. A short transfer reads the sector first
so the untouched remainder is preserved. Failure is reported the way the 8272
does it, with ST0 bit 6 and **ST1 bit 1, Not Writable**, which is also what a
write protected diskette would return -- an image that can only be opened read
only now reports itself honestly instead of silently dropping writes.

## The one real catch: the DMA ends swap

The first attempt wrote to the right sectors with the wrong contents, and
scrambled the directory. The trace said why:

```
fdc: WRITE DATA   05 00 02 00 17 00 17 07 80
fdc: write C=02 H=0 R=23 <- 00D3 (128 bytes)
```

`00D3` is the controller's own data port, not a memory address. **On a read the
XIOS programs the Z8410 with port A on the controller and port B in memory; on
a write it swaps them**, so the memory address arrives as port A. Reading
`dma.b_addr` unconditionally meant the write was sourcing its data from address
`00D3` in memory.

The fix is to take whichever end is not the I/O side:

```c
addr = dma.a_io ? dma.b_addr : dma.a_addr;
```

which is what the hardware itself does, and works whichever way round the
driver programs it.

## Where it stands

The reconstructed Monarch now boots the original diskette, runs Dynabyte's MP/M
II on four independent consoles, and reads *and writes* its disk -- on hardware
recovered entirely from Dynabyte's own code.

Still open: the Winchester at `E0`-`EF`, which `DYNASYS`, `DYNASTAT`, `FORMAT`
and `TAPE` need. `FORMAT` would also want the 8272's FORMAT TRACK command (0D),
which is accepted but not implemented.


---

# The Winchester: mapped, not yet implemented

## It is configured, and it is reachable

`SELDSK` in the XIOS looks the drive up in a type table at `AF6B`, dumped live
from the running machine:

```
af6b - 00 04 05 14 ff ff ...
```

* `A:` type 00 -- floppy unit 0
* `B:` type 04 -- Winchester
* `C:` type 05 -- Winchester
* `D:` type 14 -- something that needs no hardware; it already selects and
  reports an empty directory
* the rest `FF`, absent

`SELDSK` sends type < 4 to the floppy driver, `FF` to the error return, `14` to
its own path, and everything else to the Winchester driver at `B717`. So the
diskette we have is configured for a machine with a hard disk, and today

```
0A>dir b:
Bdos Err On B: Select
```

which is the driver failing cleanly against absent hardware.

## The controller is Dynabyte's own

No WD1000, no Xebec. The board is discrete logic with an **8253 on it**: the
initialisation writes `36`, `72`, `B8` to port `EF` -- three counter control
words -- and loads counters through `EC`/`ED`. The low-level primitive at
`B8F0` writes a command to `E0`, a 16-bit count to the `ED` counter, triggers
through `E1`, then polls `E4` for `(status & 0x30) == 0x10` and finally tests
bit 0 of `E0`. That is an ST506 interface built out of parts, which is what the
"the disk controller is VERY weird" remark on VCFed was about.

Ports the driver uses: `E0` command and bit-level status, `E1` trigger, `E2`
**data**, `E3` strobe, `E4` status, `E9`, and `EC`/`ED` the 8253 counters.

## The data path is simple, which makes this tractable

The Winchester driver never touches the Z80 DMA -- zero references to port `D6`
in the whole driver. Data moves by **`INIR` from port `E2`**. That means a
functional implementation does not have to reproduce ST506 serialisation at
all: satisfy the command handshake, then stream sector bytes on `E2`.

## The disk carries a Dynabyte label, and we know its format

Before anything else, the driver reads a label off the drive and validates it:

```
LD C,0E2h : LD B,008h : LD HL,0BCEEh
lb78c:  IN A,(C) : CP (HL) : JP NZ,fail : INC HL : DJNZ lb78c   ; signature
        LD HL,0BC80h : LD B,002h
lb79a:  IN A,(C) : CP (HL) : JP C,fail : INC HL : DJNZ lb79a    ; version >=
        LD B,010h : LD DE,0BC59h
lb7a8:  IN A,(C) : LD (DE),A : INC DE : DJNZ lb7a8              ; 16 parameters
        ...
        LD C,0E2h : LD HL,0BCF6h : LD B,080h : INIR             ; 128 more
        ...
        four rounds of  LD B,00Fh : INIR                        ; 4 x 15 bytes
```

and the constants it compares against are in the XIOS itself:

```
BCEE:  44 59 4E 41 42 59 54 45     "DYNABYTE"
BC80:  32 31                       "21", compared with >=
```

So a Dynabyte Winchester starts with the literal string **`DYNABYTE`**, a
two-character version, sixteen parameter bytes, a 128-byte block, and then
**four fifteen-byte records** that the driver turns into disk parameter blocks
-- four logical drives on the one spindle.

That is the reason a blank image would be useless, and also the reason this is
finishable: the label is a documented-by-its-own-checker structure, and it can
be synthesised.

## What finishing it takes

1. Model the `E0`/`E1`/`E4` handshake far enough for the driver's read and
   write commands to complete -- the sequences are all in `B717`-`BB93`, and
   the state to satisfy is small because the transfer itself is just `INIR`.
2. Build an image beginning with a valid label: `DYNABYTE`, version `21`, the
   sixteen parameters, and four DPB records whose geometry matches the drive
   size chosen.
3. Format the data area as empty CP/M directories so `B:` and `C:` come up as
   usable blank volumes.

Not started. Everything above is analysis; no controller code has been written.


---

# The Winchester: controller written, label accepted, drive not up yet

The existing 19 gates still pass, so nothing has regressed.

## Implemented

`simio.c` now carries a Dynabyte Winchester controller, and `mkwinch.py`
builds an image for it.

| port | what it does |
|---|---|
| `E0` | drive/command select, bit 0 readable |
| `E1` | what `E2` means next: `04` = four address bytes follow, `00` = rewind the buffer, `01` = go |
| `E2` | data both ways: written it takes the address, read it streams the sector |
| `E3` | buffer pointer strobe, written as a countdown |
| `E4` | status when read, control latch when written |
| `EC`/`ED`/`EF` | the 8253 on the controller board, accepted and ignored |

**Status is `0x90`.** That single value satisfies every test the driver makes:
`sub_bb93` needs bit 7 with bit 3 clear, `b750` needs bit 5 clear, `b900` wants
`(s & 0x30) == 0x10`, `b8a7` loops while bit 6 is set, and `sub_bbb5` accepts a
completion only when `(s & 0x1A) == 0x10`.

**Completion is an interrupt on the DCD of SIO 0 channel A** -- the third
peripheral hung off a modem pin, after the floppy on SIO 0 channel B and the
clock on SIO 1 channel B. The handler at `CBA0` reads `E4`, stores it at
`lc41f` and sets flag 25, which is what `sub_bbb5` waits on.

That last one has a subtlety worth recording: **console 0 is on the same
channel**, and its CONST polls RR0 continuously, so clearing the completion on
a status read loses the interrupt to whichever polls first. It is cleared where
the interrupt service routine actually acknowledges the board, on the write of
`09` to `E4` at `CBB1`.

The address the driver sends is head, cylinder high, cylinder low, sector, and
the buffer pointer works out as: `E1 = 0` rewinds, each `E3` strobe steps on by
a sector. Since `sub_bb27` writes `E1 = 0` and then counts `(iy+7)` down to 1,
the pointer lands `(iy+7)` sectors in -- 0, 256, 512 -- which is exactly where
the label keeps its signature, its 128-byte block and its DPBs. The two agree
without being made to.

## The label works

The label is not at a magic address: the driver asks for **cylinder 0, head 0,
sector 9**. `mkwinch.py` writes one there, and the XIOS reads and accepts it:

```
win: read  cyl=0000 head=0 sec=9
win: E2 read [0000] = 44 'D'
win: E2 read [0001] = 59 'Y'
...
win: E2 read [0008] = 32 '2'
win: E2 read [0009] = 31 '1'
win: E2 read [000A] = 10 '.'      <- into the sixteen parameters
```

It reads straight past the signature and the version, which are the two checks
that bail out on mismatch, so both passed. The four fifteen-byte records are
ordinary CP/M DPBs and are generated for four 2 MB logical drives.

## Where it stops

After the label the driver issues one more operation and then goes quiet:

```
win: read  cyl=0000 head=0 sec=9
win: read  cyl=FFFF head=0 sec=1
```

`FFFF` is the driver's marker for "not positioned" rather than a real cylinder,
so this is part of the identify sequence and not a data read -- something in
`sub_b87b` / `b804` / `sub_bac4` is being asked for and not answered. `dir b:`
therefore still produces nothing rather than a directory.

So: the controller answers, the handshake and the interrupt path work, and the
label round trip is proven. What is left is the tail of the identify sequence,
and then the data path itself, which should be easy once the drive comes up
because reads are plain `INIR` from a buffer that is already in place.

`~/monarch/mkwinch.py` builds the image; the controller is in `simio.c` under
"Winchester controller".


## Identify sequence: it completes

Still 19/19 on everything else.

Two things were wrong in the label and both are now fixed and confirmed.

**The drive records are 17 bytes, not 15.** The loop at `B7CF` reads

```
LD B,00Fh : INIR          ; fifteen bytes, an ordinary CP/M DPB
...
IN L,(C) : IN H,(C)       ; two more
ADD HL,DE : EX DE,HL      ; which accumulate across the four records
```

so each record carries a sixteen-bit value after its DPB -- the size of that
drive's allocation vector, which the XIOS sums as it walks the four records to
carve the buffers out of `BD78`. Writing them at a fifteen byte stride left
every record after the first misaligned.

**The 128-byte block is a defect map.** `sub_b916` walks `BCF6` as pairs of
sixteen-bit values, comparing each against the requested track and stopping at
a zero pair, substituting the index when it matches. Zeros are therefore a
valid empty map, which is what the generated image carries.

With the stride fixed, the identify runs to completion. Counting the reads the
controller serves proves it exactly:

```
total E2 reads: 222      = 26 + 128 + 68
last offset:    0x243
```

26 bytes of signature, version and parameters from offset 0; 128 bytes of
defect map from 256; four 17-byte records from 512, ending precisely at 579.
The XIOS consumed the whole label and asked for nothing more.

## What is still wrong

After the identify the driver issues one more operation and stops:

```
win: read  cyl=0000 head=0 sec=9  PC=B9F9
win: read  cyl=FFFF head=0 sec=1  PC=B9F9
```

`FFFF` in the cylinder is what `lb776` writes when it gives up -- so something
after the label is taking a failure path, and `dir b:` still returns nothing.
The remaining suspects are the tail of `lb804` (which checks the drive is
present by testing the first DPB byte through `sub_b833`, a 31-byte stride per
drive) and the track-to-cylinder conversion that follows `lb958`.

The controller itself is not in question any more: it answers every handshake,
the interrupt path works, and it served a 222-byte label the XIOS accepted
without complaint.


## The address arithmetic, and where it still stops

Still 19/19 on the floppy, consoles and writing.

**The FFFF cylinder was a divide by zero, and it is fixed.** `lb961` converts a
track to an address by dividing:

```
LD C,(IY+00Dh) : LD B,000h       ; the head count, out of the label
LD HL,(track)
CALL sub_b98a                    ; HL = quotient, DE = remainder
LD (IY+0),L : LD (IY+1),H        ; cylinder
LD (IY+4),E                      ; head
LD A,(sector) : DEC A : LD B,A
AND 007h : LD (IY+7),A           ; record within the physical sector
LD A,B : AND 0F8h : RRCA x3 : INC A
LD (IY+6),A                      ; physical sector
```

`sub_b98a` is a plain sixteen-bit divide. With the label's parameters left as
zeros the divisor was zero and the quotient came out `FFFF` -- which is exactly
the cylinder that kept appearing, and it was never a failure marker at all.

That also settles the geometry: **a physical sector is 1024 bytes holding eight
CP/M records**, the low three bits of the CP/M sector picking the record. Which
is precisely what the E3 strobe selects, and why the label could be read in
fields at 0, 256 and 512 of one sector.

Both are now modelled: `WIN_SECSZ` is 1024, the strobe steps 128 at a time, and
the generated label carries the head count. The address the driver computes is
sane:

```
win: read  cyl=0000 head=0 sec=9    <- the label
win: read  cyl=0000 head=0 sec=1    <- the directory
```

**It still stops there.** A SIGINT into the ICE catches the machine at `DC94`
inside XDOS with every register zero -- the dispatcher's idle loop, so MP/M is
not crashed, the process that asked for the directory is simply still blocked.
Deferring the completion interrupt to the machine tick, so the service routine
cannot run inside the OUT that starts the operation, did not change it.

So the directory sector is fetched and the requester never wakes. The next
thing to look at is the block move out of the controller: `sub_bc15` sets the
DMA bank through ports `8E` and `86` before running the patched mover at
`lb684`, and those two ports are accepted and ignored. The floppy tolerates
that because its transfer goes through the Z8410 and lands via the same map the
CPU is using, but the Winchester's transfer is an `INIR` executed by the CPU,
and if `8E`/`86` are meant to redirect it into another bank then the directory
is being delivered somewhere MP/M is not looking.

That is the one concrete lead left, and it is a small experiment: model `86`
and `8E` as a bank override for the duration of the move.


## The Winchester data path: it is the DMA, and drive B: now selects

Still 19/19 on everything else.

I had this wrong. `sub_bc15` does not move the data with `INIR` -- it patches a
descriptor with the port and count and hands it to `sub_b4f1`, which is the
block sender for the Z8410 on port `D6`, then waits on flag 24. The `INIR`s are
only the label and identify. **The Winchester's data transfer is a DMA with the
controller's data port, E2, on the I/O end**, exactly like the floppy but with
`D3` swapped for `E2`.

The end of that transfer is what sets flag 24, and the DMA initialisation block
at `C67E` says how:

```
... 91 12 40 ...
```

WR4 announcing an interrupt control byte, `12` asking for an interrupt at end
of block with a vector, and `40` being the vector. With `I = CB` it lands at
`CB40`, which holds `03 CC` -- a real handler at `CC03`, and that is where the
`LD E,018h` that sets flag 24 lives.

Two bugs of mine fell out of this:

* the DMA parser never expected the vector byte after the interrupt control
  byte, so `40` was being read back as a WR2 write and quietly corrupting the
  port B configuration;
* `machine_tick` raised the DMA interrupt and then immediately let the clock
  tick overwrite `int_data` with the SIO's vector, losing it. The DMA
  interrupt is now raised last.

With those fixed the operation count for a single `dir b:` went from 3 to 147,
and the drive selects:

```
0A>dir b:
Directory for User  0:
```

which is a long way from `Bdos Err On B: Select`.

**It does not finish.** The header prints and the scan stalls partway through
the directory. So SELDSK, the label, the addressing and the transfer all work,
and something in the repeat -- the sector cache at `lbc40`, or the record
selection within the 1024-byte sector across successive reads -- does not.
