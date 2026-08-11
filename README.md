# Dynabyte Monarch — an emulator of the Z80 side

**Work in progress.** It boots the original 1985 MP/M II diskette and runs it
multiuser on four consoles. The hard disk does not work yet. See
[What works](#what-works) and [What does not](#what-does-not-work-yet).

Nothing else emulates this machine.

The Dynabyte Monarch was a dual-processor Z80 + 8086 multiuser system from
Dynabyte Business Computers, Milpitas, around 1983. No schematic, no technical
manual and no boot PROM dump survives — [bitsavers][bs] has only the two user's
guides. Everything this emulator does was recovered by disassembling Dynabyte's
own code: the `LDRBIOS` inside `FPYMPM.LDR` and `XIOS1.SPR`, off the MP/M II
DV1.4E distribution diskettes.

[bs]: http://www.bitsavers.org/pdf/dynabyte/

`FINDINGS.md` is the full record of how the hardware was worked out. It is
probably more interesting than the code.

## Running it

You need the MP/M II DV1.4E diskette images, which are **not** in this
repository — they are Digital Research and Dynabyte software. They are in the
TOSEC set as *Dynabyte Monarch - Operating Systems*. Convert diskette 1 to a
raw image and drop it in:

```sh
python3 tools/imd2raw.py 'Monarch MP-M II *Disk 1*.imd'
cp 'Monarch MP-M II ...Disk 1....raw' media/monarch-mpm2-disk1.raw
./run.sh
```

At the `>>>` prompt type `g`:

```
Initializing MMU Bank 0 1 2 3 4 , I/O Ports , Winchester, DMA, Clock
Initialization Complete.

Monarch MP/M II DV1.4E
0A>
```

`./run.sh --check` runs 19 verification gates headless. `Ctrl-\` quits.

Consoles 1 to 3 are TCP: `telnet localhost 4001`, `4002`, `4003`. Each prints
its banner the moment you connect and has its own drive, user number and
running program.

## What works

Verified by `./run.sh --check`, 19 gates:

- the synthesised boot PROM, the reserved tracks and Dynabyte's own MP/M II 2.1
  loader
- `MPM.SYS` loaded off the original diskette, and Dynabyte's XIOS initialising
- the MMU, with the five banks `MPM.SYS` declares
- the clock
- the MP/M prompt, and Dynabyte's own transient programs (`SDIR`, `MPMSTAT`,
  `PIP`, `DIR`, …)
- reading **and writing** the diskette
- four consoles at once

## What does not work yet

- **The Winchester.** Drives `B:` and `C:` are configured on the diskette. The
  controller is modelled, its label is read and accepted, the addressing and
  the DMA transfer work, and `dir b:` now gets as far as printing
  `Directory for User  0:` — then the directory scan stalls partway. The
  suspects are the sector cache at `lbc40` and the record selection within the
  1024-byte physical sector across successive reads. `mkwinch.py` builds an
  image with a valid Dynabyte label.
- **`FORMAT.COM`**, which needs the 8272's FORMAT TRACK command. Accepted, not
  implemented.
- **`DYNASYS`, `DYNASTAT`, `TAPE`** — they drive the hard disk and the
  cartridge tape.
- **The 8086 side.** Untouched. Its XIOS depends on a proprietary MMU with
  hardware user/supervisor protection *and* on an intelligent I/O Box with a
  message protocol that would have to be reverse engineered first.

## The machine, as far as it is now known

| port | device |
|---|---|
| `B0`–`B7` | two Z80 SIOs, four channels, four consoles |
| `8C` | FDC reset and drive select |
| `D0`/`D1` | 8272 floppy controller |
| `D3` | the same FIFO on the DMA path |
| `D5` | MMU page map — the slot rides on A8-A15, not the port number |
| `D6` | Zilog Z8410 DMA |
| `85`/`87` | MMU: the bank the CPU runs from, and the one being programmed |
| `E0`–`EF` | Winchester controller, Dynabyte's own discrete logic, with an 8253 |

Two things are worth knowing.

**The MMU keeps a whole page map per bank.** Port `87` selects the map being
programmed, `D5` writes one of its eight 8 KB slots, and `85` selects the map
the CPU runs from — which is literally all `SELMEMORY` does.

**Nothing has an interrupt line of its own.** All three peripherals hang off
modem-control pins of the serial chips:

| source | arrives as |
|---|---|
| 8272 floppy INT | DCD of SIO 0 channel B |
| clock tick | DCD of SIO 1 channel B |
| Winchester | DCD of SIO 0 channel A |

so each shows up as an External/Status Change interrupt and the handlers tell
them apart by one bit of the status register. The Z8410 is the only thing with
a vector of its own, `40`.

## Layout

```
run.sh              build / --check / --rebuild
check.py            the 19 verification gates
mkwinch.py          builds a Winchester image with a Dynabyte label
FINDINGS.md         how all of this was worked out
tools/              IMD conversion, CP/M image tools, the .SPR relocator,
                    a pty driver and a SIGINT-into-the-ICE probe
media/              put your diskette image here
z80pack/            Udo Munk's Z80 core, with monarchsim on top
```

## License

The code here is under the **GNU General Public License v3.0**, see
[LICENSE](LICENSE). Parts of this repository are other people's work and are
not covered by it — see [NOTICE.md](NOTICE.md). Contributions require the
agreement in [CLA.md](CLA.md); see [CONTRIBUTING.md](CONTRIBUTING.md). z80pack is Udo Munk's and keeps its own license, in
`z80pack/LICENSE`. Two small additions were made to its core: `io_porth`, which
exposes the high byte of the I/O cycle because the Monarch's MMU puts the slot
there, and a `machine_tick` hook so the clock runs on emulated time rather than
the host's.

No Digital Research or Dynabyte software is included.
