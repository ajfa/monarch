# Notices

The GPL-3.0 license in `LICENSE` covers the code written for this repository.
The following parts are other people's work and are not covered by it.

## `z80pack/`

Copyright (c) Udo Munk and others, distributed under its own MIT license; see
`z80pack/LICENSE`. MIT is compatible with the GPL, so it may be combined here;
its notice must be preserved.

Two small additions were made to its core: `io_porth`, which exposes the high
byte of the I/O cycle because the Monarch's MMU puts the slot there, and a
`machine_tick` hook so the clock runs on emulated time rather than the host's.

## Digital Research and Dynabyte software

**Not included.** No CP/M/MP-M binaries or Dynabyte software are redistributed
here.
