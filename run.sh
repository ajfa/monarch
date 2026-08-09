#!/bin/bash
#
#  Dynabyte Monarch, Z80 side -- an emulator built from Dynabyte's own code
#
#    ./run.sh            build if needed, verify, then run it
#    ./run.sh --check    headless verification, 19 gates, safe over SSH
#    ./run.sh --rebuild  clean and build again
#
set -euo pipefail
cd "$(dirname "$0")"

say()  { printf '\033[1;36m==\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

if [ "$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo"; fi

case "$PWD" in
  *[[:space:]]*) die "the path to this pack contains a space: $PWD
   make cannot cope. Move it somewhere without spaces." ;;
esac

if [ -n "$(find . ! -user "$(id -un)" -print -quit 2>/dev/null)" ]; then
    say "reclaiming file ownership for $(id -un)"
    $SUDO chown -R "$(id -u):$(id -g)" .
fi

CHECK_ONLY=0; REBUILD=0
for a in "$@"; do
  case "$a" in
    --check)   CHECK_ONLY=1 ;;
    --rebuild) REBUILD=1 ;;
    *)         die "unknown option: $a  (use --check and/or --rebuild)" ;;
  esac
done

SIM="$PWD/z80pack/monarchsim"

NEED=""
for t in cc make python3; do command -v "$t" >/dev/null 2>&1 || NEED="$NEED $t"; done
if [ -n "$NEED" ]; then
    say "installing build dependencies ($NEED )"
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get update -qq
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential python3
else
    say "build tools already present, skipping apt"
fi

if [ "$REBUILD" = 1 ]; then
    say "cleaning"
    make -C "$SIM/srcsim" clean >/dev/null 2>&1 || true
    rm -f "$SIM/monarchsim"
fi

FRESH=0
if [ ! -x "$SIM/monarchsim" ]; then
    say "building monarchsim"
    make -C "$SIM/srcsim" >/dev/null
    [ -x "$SIM/monarchsim" ] || die "monarchsim did not build"
    FRESH=1
else
    say "monarchsim already built (use --rebuild to force)"
fi

# a fresh copy of the original diskette: the guest can write to it
mkdir -p "$SIM/disks"
[ -f media/monarch-mpm2-disk1.raw ] || die "no media/monarch-mpm2-disk1.raw
   This repository does not ship Digital Research or Dynabyte software.
   Convert diskette 1 of the MP/M II DV1.4E set and put it there:
       python3 tools/imd2raw.py 'Monarch MP-M II *Disk 1*.imd'"
cp media/monarch-mpm2-disk1.raw "$SIM/disks/drivea.dsk"
chmod 644 "$SIM/disks/drivea.dsk"
[ -f "$SIM/disks/winchester.img" ] || python3 mkwinch.py "$SIM/disks/winchester.img" >/dev/null

if [ "$CHECK_ONLY" = 1 ] || [ "$FRESH" = 1 ]; then
    say "verifying"
    ( cd "$SIM" && python3 "$OLDPWD/check.py" ) || die "verification failed"
    [ "$CHECK_ONLY" = 1 ] && exit 0
fi

cat <<'EOF'

=======================================================================
  Dynabyte Monarch, Z80 side

  This is not a Monarch running someone's rewrite of its software: it is
  the original 1985 diskette, booting Dynabyte's own loader and XIOS on
  hardware reconstructed from that code. No schematic, no technical
  manual and no boot PROM for this machine survives.

  At the  >>>  prompt type:   g

  The machine boots and you get   0A>   -- MP/M II, console 0, drive A.

     dir           the diskette
     sdir          Dynabyte's own directory utility
     mpmstat       processes, consoles, memory banks
     pip a:x.txt=con:   type a file in, ^Z to end it
     type x.txt

  Consoles 1 to 3 are TCP. From another terminal:

     telnet localhost 4001        (also 4002, 4003)

  each gets its own prompt, drive and user number, all at once.

  Ctrl-\  stops the simulator.
=======================================================================

EOF

SAVED="$(stty -g 2>/dev/null || true)"
restore() { [ -n "$SAVED" ] && stty "$SAVED" 2>/dev/null || true; }
trap restore EXIT INT TERM QUIT

cd "$SIM"
./monarchsim || true
restore
echo
say "simulator stopped"
