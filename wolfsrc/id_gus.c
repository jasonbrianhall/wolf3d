// ID_GUS.C -- the Gravis UltraSound
//
// The GUS has no AdLib chip and no Sound Blaster DSP, so the game's AdLib
// register writes go to an OPL2 in software (oplemu.cpp) and its
// digitized sounds are mixed in, and the result is streamed to the card:
// one voice loops over a ring buffer in the GUS's own memory, and the
// timer interrupt (700 times a second) writes the next samples just ahead
// of where the voice is playing. Programmed I/O only: no GUS interrupt or
// DMA channel is used, so only the port from ULTRASND matters.

#include "id_heads.h"
#include <stdint.h>
#pragma hdrstop

void	OPLEMU_Init (int rate);
void	OPLEMU_Write (int reg, int val);
void	OPLEMU_Generate (int32_t *buf, int samples);
void	SDL_DigitizedDone (void);

#define MIXRATE		22050
#define DIGIRATE	7042			// the game's digitized sounds
#define RINGSTART	0				// in GUS memory
#define RINGLEN		4096
#define AHEAD		1100			// samples written ahead of the voice (~50 ms)
#define CHUNK		256
#define MAXFILL		2048

static	int		gusbase;
static	int		gusstarted;
static	unsigned	writepos;		// next ring offset to write

static	byte * volatile	digiptr;
static	volatile longword	digilen;
static	longword	digipos;		// 16.16
static	longword	digistep;

static	int32_t		oplbuf[CHUNK];

#define P_MIX		(gusbase)
#define P_STATUS	(gusbase+0x006)
#define P_VOICE		(gusbase+0x102)
#define P_REG		(gusbase+0x103)
#define P_DATALO	(gusbase+0x104)
#define P_DATAHI	(gusbase+0x105)
#define P_DRAM		(gusbase+0x107)

static void Delay (void)
{
	int i;
	for (i=0;i<7;i++)
		inportb (P_STATUS);
}

static void Set8 (int reg, int val)
{
	outportb (P_REG,reg);
	outportb (P_DATAHI,val);
}

static void Set16 (int reg, int val)
{
	outportb (P_REG,reg);
	outportw (P_DATALO,val);
}

static int Get16 (int reg)
{
	outportb (P_REG,reg);
	return inportw (P_DATALO);
}

static void Poke (longword addr, int val)
{
	Set16 (0x43,addr & 0xffff);
	Set8 (0x44,(addr >> 16) & 0xff);
	outportb (P_DRAM,val);
}

static int Peek (longword addr)
{
	Set16 (0x43,addr & 0xffff);
	Set8 (0x44,(addr >> 16) & 0xff);
	return inportb (P_DRAM);
}

static void SetAddr (int hireg, int loreg, longword addr)
{
	Set16 (hireg,(addr >> 7) & 0x1fff);
	Set16 (loreg,(addr & 0x7f) << 9);
}

// some voice registers need writing twice (a GF1 quirk)
static void Set8Twice (int reg, int val)
{
	Set8 (reg,val);
	Delay ();
	Set8 (reg,val);
}

static longword VoicePos (void)
{
	int		hi,lo,hi2;

	outportb (P_VOICE,0);
	do
	{
		hi = Get16 (0x8a);
		lo = Get16 (0x8b);
		hi2 = Get16 (0x8a);
	} while (hi != hi2);
	return ((longword)(hi & 0x1fff) << 7) | ((lo >> 9) & 0x7f);
}

static void ResetCard (void)
{
	int v;

	Set8 (0x4c,0);					// reset
	Delay (); Delay ();
	Set8 (0x4c,1);					// run
	Delay (); Delay ();

	Set8 (0x0e,0xc0 | (14-1));		// 14 voices: 44.1 kHz output
	for (v=0;v<32;v++)
	{
		outportb (P_VOICE,v);
		Set8Twice (0x00,3);			// stop the voice
		Set8Twice (0x0d,3);			// and its volume ramp
		Set16 (0x09,0);				// silent
	}
}


/*
===================
=
= GUS_Startup
=
= Finds the card from ULTRASND=port,dma,dma,irq,irq and starts the stream
=
===================
*/

boolean GUS_Startup (void)
{
	char	*env;
	long	port;
	longword i;
	int		fc;

	env = getenv ("ULTRASND");
	if (!env)
		return false;
	port = strtol (env,NULL,16);
	if (port < 0x210 || port > 0x280 || (port & 0xf))
		return false;
	gusbase = port;

	ResetCard ();

	// is there memory to write to and read back?
	Poke (RINGSTART,0xaa);
	Poke (RINGSTART+1,0x55);
	if (Peek (RINGSTART) != 0xaa || Peek (RINGSTART+1) != 0x55)
		return false;

	OPLEMU_Init (MIXRATE);
	digistep = ((longword)DIGIRATE << 16) / MIXRATE;

	for (i=0;i<RINGLEN;i++)			// silence (signed 8 bit)
		Poke (RINGSTART+i,0);
	writepos = AHEAD;

	DISABLE();
	outportb (P_VOICE,0);
	fc = (((longword)MIXRATE << 9) + (44100 >> 1)) / 44100;
	Set16 (0x01,fc << 1);
	SetAddr (0x02,0x03,RINGSTART);	// loop start
	SetAddr (0x04,0x05,RINGSTART+RINGLEN);	// loop end
	SetAddr (0x0a,0x0b,RINGSTART);	// from the start
	Set8 (0x0c,7);					// centre
	Set16 (0x09,0xf400);			// volume
	Set8Twice (0x0d,3);				// no volume ramp
	Set8Twice (0x00,0x08);			// 8 bit, looping: go
	Set8 (0x4c,3);					// run, DACs on (no GF1 interrupts)
	outportb (P_MIX,0x01);			// line out on, line in off
	gusstarted = true;
	RESTORE();

	return true;
}

void GUS_Shutdown (void)
{
	if (!gusstarted)
		return;
	DISABLE();
	gusstarted = false;
	outportb (P_VOICE,0);
	Set8Twice (0x00,3);
	Set16 (0x09,0);
	RESTORE();
}


/*
===================
=
= The game's interface: AdLib registers and digitized sounds
=
===================
*/

void GUS_AdLibWrite (int reg, int val)
{
	OPLEMU_Write (reg,val);
}

void GUS_PlaySample (byte *data, longword len)
{
	DISABLE();
	digipos = 0;
	digilen = len;
	digiptr = data;
	RESTORE();
}

void GUS_StopSample (void)
{
	DISABLE();
	digiptr = NULL;
	RESTORE();
}


/*
===================
=
= GUS_Service
=
= From the timer interrupt: mix and write what the voice has played since
= last time
=
===================
*/

void GUS_Service (void)
{
	longword	pos,target,n,count,i,addr;
	int32_t		s;
	int			d;

	if (!gusstarted)
		return;

	pos = VoicePos () - RINGSTART;
	if (pos >= RINGLEN)
		pos = 0;
	target = (pos + AHEAD) % RINGLEN;
	n = (target + RINGLEN - writepos) % RINGLEN;
	if (n > MAXFILL)
		n = MAXFILL;

	Set8 (0x44,(RINGSTART >> 16) & 0xff);
	while (n)
	{
		count = n > CHUNK ? CHUNK : n;
		OPLEMU_Generate (oplbuf,count);

		outportb (P_REG,0x43);
		for (i=0;i<count;i++)
		{
			s = oplbuf[i] << 2;			// dbopl is quiet next to a real OPL

			if (digiptr)
			{
				s += ((int)digiptr[digipos >> 16] - 128) << 8;
				digipos += digistep;
				if ((digipos >> 16) >= digilen)
				{
					digiptr = NULL;
					SDL_DigitizedDone ();	// may start the next segment
				}
			}

			if (s > 32767)
				s = 32767;
			else if (s < -32768)
				s = -32768;
			d = (s >> 8) & 0xff;

			addr = RINGSTART + writepos;
			outportw (P_DATALO,addr & 0xffff);
			outportb (P_DRAM,d);
			if (++writepos == RINGLEN)
				writepos = 0;
		}
		n -= count;
	}
}
