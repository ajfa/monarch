#ifndef SIMIO_INC
#define SIMIO_INC
#include "sim.h"
#include "simdefs.h"
#define IO_DATA_UNUSED 0xff
extern in_func_t *const port_in[256];
extern out_func_t *const port_out[256];
extern void init_io(void);
extern void exit_io(void);
#endif
