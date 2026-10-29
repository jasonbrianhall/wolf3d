// OPLEMU.CPP -- an AdLib (OPL2) in software, for sound cards without one
// (the Gravis UltraSound). DOSBox's dbopl does the work.

#include <stddef.h>
#include <stdint.h>
#include "dbopl.h"

static DBOPL::Handler	opl;
static int				oplready;


extern "C" void OPLEMU_Init (int rate)
{
	opl.Init ((Bitu)rate);			// builds its tables: uses the FPU
	oplready = 1;
}

extern "C" void OPLEMU_Write (int reg, int val)
{
	if (oplready)
		opl.WriteReg ((Bit32u)reg,(Bit8u)val);
}

// mono, about +-32767 at full scale; integer only (safe in an interrupt)
extern "C" void OPLEMU_Generate (int32_t *buf, int samples)
{
	opl.Generate ((Bit32s *)buf,(Bitu)samples);
}
