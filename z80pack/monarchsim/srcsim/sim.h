/*
 *	MONARCHSIM  -  a Dynabyte Monarch (Z80 side) simulator
 *
 *	The Monarch is not documented anywhere: no technical manual, no
 *	schematic, no boot ROM dump survives. Every hardware fact modelled
 *	here was reverse engineered out of Dynabyte's own code -- the LDRBIOS
 *	inside FPYMPM.LDR and the XIOS1.SPR from the MP/M II DV1.4E
 *	distribution diskettes.
 */

#ifndef SIM_INC
#define SIM_INC

#define DEF_CPU		Z80	/* the Monarch's 8-bit side is a Z80 */
#define CPU_SPEED	0	/* unlimited; the tick compensates */
#define UNDOC_INST
#ifndef EXCLUDE_Z80
#define FAST_BLOCK
#endif

#define WANT_ICE		/* the built-in debugger is the RE tool here */
#define WANT_TIM
#define HISIZE		1000
#define SBSIZE		12

/* clock tick every 1/60 s of emulated time at 4 MHz */
#define MACHINE_TICK	(4000000 / 60)

#define HAS_DISKS

#define USR_COM	"Dynabyte Monarch (Z80 side) Simulation"
#define USR_REL	"0.1"
#define USR_CPR	"Reverse engineered from Dynabyte's own XIOS"

#endif /* !SIM_INC */
