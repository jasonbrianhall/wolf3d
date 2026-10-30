//
//	ID Engine
//	ID_SD.c - Sound Manager for Wolfenstein 3D
//	v1.2
//	By Jason Blochowiak
//

//
//	This module handles dealing with generating sound on the appropriate
//		hardware
//
//	Depends on: User Mgr (for parm checking)
//
//	Globals:
//		For User Mgr:
//			SoundSourcePresent - Sound Source thingie present?
//			SoundBlasterPresent - SoundBlaster card present?
//			AdLibPresent - AdLib card present?
//			SoundMode - What device is used for sound effects
//				(Use SM_SetSoundMode() to set)
//			MusicMode - What device is used for music
//				(Use SM_SetMusicMode() to set)
//			DigiMode - What device is used for digitized sound effects
//				(Use SM_SetDigiDevice() to set)
//
//		For Cache Mgr:
//			NeedsDigitized - load digitized sounds?
//			NeedsMusic - load music?
//

#pragma hdrstop		// Wierdo thing with MUSE

#include <dos.h>

#ifdef	_MUSE_      // Will be defined in ID_Types.h
#include "id_sd.h"
#else
#include "id_heads.h"
#endif
#pragma	hdrstop
#pragma	warn	-pia

#ifdef	nil
#undef	nil
#endif
#define	nil	0

#define	SDL_SoundFinished()	{SoundNumber = SoundPriority = 0;}

// Macros for SoundBlaster stuff
#define	sbOut(n,b)	outportb((n) + sbLocation,b)
#define	sbIn(n)		inportb((n) + sbLocation)
#define	sbWriteDelay()	while (sbIn(sbWriteStat) & 0x80);
#define	sbReadDelay()	while (sbIn(sbDataAvail) & 0x80);

// Macros for AdLib stuff
#define	selreg(n)	outportb(alFMAddr,n)
#define	writereg(n)	outportb(alFMData,n)
#define	readstat()	inportb(alFMStatus)

//	The timer 0 services (were ID_SD_A.ASM), see the end of the file
#define	t0_Slow		0		// TickBase * 2: PC / AdLib sound effects
#define	t0_Fast		1		// TickBase * 10: + AdLib music, Sound Source
#define	t0_Extreme	2		// TickBase * 100: + PC speaker digitized sound

static	void			SDL_t0Service(void);
static	void			SDL_IndicatePC(boolean on);
static	void			SDL_InstallTimer(void);
static	void			SDL_RemoveTimer(void);
static	void			SDL_Delay(int usec);

//	Global variables
	boolean		SoundSourcePresent,
				AdLibPresent,
				SoundBlasterPresent,SBProPresent,
				NeedsDigitized,NeedsMusic,
				SoundPositioned;
	SDMode		SoundMode;
	SMMode		MusicMode;
	SDSMode		DigiMode;
volatile	longword	TimeCount;
volatile	word		HackCount;
	byte		**SoundTable;
	boolean		ssIsTandy;
	word		ssPort = 2;
	int			DigiMap[LASTSOUND];

//	Internal variables
static	boolean			SD_Started;
static	boolean			gusNoCheck;
		boolean			nextsoundpos;
volatile longword		TimerDivisor,TimerCount;
static	char			*ParmStrings[] =
						{
							"noal",
							"nosb",
							"nopro",
							"noss",
							"sst",
							"ss1",
							"ss2",
							"ss3",
							"nogus",
							nil
						};

//	Gravis UltraSound (id_gus.c): an AdLib in software plus the digitized
//	sounds, streamed to the card
		boolean		GUSPresent;
boolean	GUS_Startup (void);
void	GUS_Shutdown (void);
void	GUS_Service (void);
void	GUS_AdLibWrite (int reg, int val);
void	GUS_PlaySample (byte *data, longword len);
void	GUS_SetVolume (int leftpos, int rightpos);
void	GUS_StopSample (void);
static	void			(*SoundUserHook)(void);
volatile soundnames		SoundNumber,DigiNumber;
volatile word			SoundPriority,DigiPriority;
		int				LeftPosition,RightPosition;
static	_go32_dpmi_seginfo	t0OldService,t0NewService;
static	boolean			t0Installed;
static	volatile int	t0Mode;
volatile long			LocalTime;
		word			TimerRate;

		word			NumDigi,DigiLeft,DigiPage;
		word			*DigiList;
		word			DigiLastStart,DigiLastEnd;
volatile boolean		DigiPlaying;
static	volatile boolean	DigiMissed,DigiLastSegment;
static	memptr volatile	DigiNextAddr;
static	byte		*DigiRaw;		// SD_PlaySample's buffer (else the page file)
static	volatile word	DigiNextLen;

//	SoundBlaster variables
static	boolean					sbNoCheck,sbNoProCheck;
static	volatile boolean		sbSamplePlaying;
static	byte					sbOldIntMask = -1;
static	byte * volatile		sbNextSegPtr;
static	byte					sbDMA = 1,
								sbDMAa1 = 0x83,sbDMAa2 = 2,sbDMAa3 = 3,
								sba1Vals[] = {0x87,0x83,0,0x82},
								sba2Vals[] = {0,2,0,6},
								sba3Vals[] = {1,3,0,7};
static	int						sbLocation = -1,sbInterrupt = 7,sbIntVec = 0xf,
								sbIntVectors[] = {-1,-1,0xa,0xb,-1,0xd,-1,0xf,-1,-1,0x72};
static	volatile longword		sbNextSegLen;
static	_go32_dpmi_seginfo		sbOldIntHand,sbNewIntHand;
static	byte					sbpOldFMMix,sbpOldVOCMix;

// The DSP plays from a buffer in conventional memory (ISA DMA can't reach
// the rest of our memory), which the samples are copied into a piece at a
// time. SBBUFSIZE is the most played at once.
#define	SBBUFSIZE	PMPageSize
static	int						sbDOSSel = -1;
static	longword				sbDOSAddr;		// physical (= linear) address

//	SoundSource variables
		boolean				ssNoCheck;
		boolean				ssActive;
		word				ssControl,ssStatus,ssData;
		byte				ssOn,ssOff;
		byte * volatile		ssSample;
		volatile longword	ssLengthLeft;

//	PC Sound variables
		volatile byte	pcLastSample;
		byte * volatile	pcSound;
		volatile longword	pcLengthLeft;
		word			pcSoundLookup[255];

//	AdLib variables
		boolean			alNoCheck;
		byte * volatile	alSound;
		word			alBlock;
		volatile longword	alLengthLeft;
		volatile longword	alTimeCount;
		Instrument		alZeroInst;

// This table maps channel numbers to carrier and modulator op cells
static	byte			carriers[9] =  { 3, 4, 5,11,12,13,19,20,21},
						modifiers[9] = { 0, 1, 2, 8, 9,10,16,17,18},
// This table maps percussive voice numbers to op cells
						pcarriers[5] = {19,0xff,0xff,0xff,0xff},
						pmodifiers[5] = {16,17,18,20,21};

//	Sequencer variables
volatile boolean		sqActive;
static	word			alFXReg;
static	ActiveTrack		*tracks[sqMaxTracks],
						mytracks[sqMaxTracks];
static	word			sqMode,sqFadeStep;
		word			*sqHack;
		word * volatile	sqHackPtr;
volatile word			sqHackLen;
		word			sqHackSeqLen;
volatile long			sqHackTime;

//	Internal routines
		void			SDL_DigitizedDone(void);

///////////////////////////////////////////////////////////////////////////
//
//	SDL_SetTimer0() - Sets system timer 0 to the specified speed
//
///////////////////////////////////////////////////////////////////////////
#pragma	argsused
static void
SDL_SetTimer0(word speed)
{
	DISABLE();

	outportb(0x43,0x36);				// Change timer 0
	outportb(0x40,speed);
	outportb(0x40,speed >> 8);
	// Kludge to handle special case for digitized PC sounds
	if (TimerDivisor == (1192030 / (TickBase * 100)))
		TimerDivisor = (1192030 / (TickBase * 10));
	else
		TimerDivisor = speed;

	RESTORE();
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_SetIntsPerSec() - Uses SDL_SetTimer0() to set the number of
//		interrupts generated by system timer 0 per second
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_SetIntsPerSec(word ints)
{
	TimerRate = ints;
	SDL_SetTimer0(1192030 / ints);
}

static void
SDL_SetTimerSpeed(void)
{
	word	rate;
	int		isr;

	if ((DigiMode == sds_PC) && DigiPlaying)
	{
		rate = TickBase * 100;
		isr = t0_Extreme;
	}
	else if
	(
		(MusicMode == smm_AdLib)
	||	((DigiMode == sds_SoundSource) && DigiPlaying)
	||	GUSPresent					// the stream needs filling often
	)
	{
		rate = TickBase * 10;
		isr = t0_Fast;
	}
	else
	{
		rate = TickBase * 2;
		isr = t0_Slow;
	}

	if (rate != TimerRate)
	{
		DISABLE();
		t0Mode = isr;
		SDL_InstallTimer();
		SDL_SetIntsPerSec(rate);
		TimerRate = rate;
		RESTORE();
	}
}

//
//	SoundBlaster code
//

///////////////////////////////////////////////////////////////////////////
//
//	SDL_SBStopSample() - Stops any active sampled sound and causes DMA
//		requests from the SoundBlaster to cease
//
///////////////////////////////////////////////////////////////////////////
#ifdef	_MUSE_
void
#else
static void
#endif
SDL_SBStopSample(void)
{
	byte	is;
	int		picport,picbit;

	if (GUSPresent)
	{
		GUS_StopSample();
		return;
	}
	{
	DISABLE();

	if (sbSamplePlaying)
	{
		sbSamplePlaying = false;

		sbWriteDelay();
		sbOut(sbWriteCmd,0xd0);	// Turn off DSP DMA

		picport = sbInterrupt < 8 ? 0x21 : 0xa1;
		picbit = 1 << (sbInterrupt & 7);
		is = inportb(picport);	// Restore interrupt mask bit
		if (sbOldIntMask & picbit)
			is |= picbit;
		else
			is &= ~picbit;
		outportb(picport,is);
	}

	RESTORE();
	}
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_SBPlaySeg() - Plays a chunk of sampled sound on the SoundBlaster
//	Insures that the chunk doesn't cross a bank boundary, programs the DMA
//	 controller, and tells the SB to start doing DMA requests for DAC
//
///////////////////////////////////////////////////////////////////////////
static longword
SDL_SBPlaySeg(byte *data,longword length)
{
	unsigned		datapage;
	longword		dataofs,uselen;

	uselen = length;
	if (uselen > SBBUFSIZE)
		uselen = SBBUFSIZE;

	// Copy it where the DMA controller can get at it
	dosmemput(data,uselen,sbDOSAddr);
	datapage = sbDOSAddr >> 16;
	dataofs = sbDOSAddr & 0xffff;		// the buffer doesn't cross a 64k page

	uselen--;

	// Program the DMA controller
	DISABLE();
	outportb(0x0a,sbDMA | 4);					// Mask off DMA on channel sbDMA
	outportb(0x0c,0);							// Clear byte ptr flip-flop to lower byte
	outportb(0x0b,0x49);						// Set transfer mode for D/A conv
	outportb(sbDMAa2,(byte)dataofs);			// Give LSB of address
	outportb(sbDMAa2,(byte)(dataofs >> 8));		// Give MSB of address
	outportb(sbDMAa1,(byte)datapage);			// Give page of address
	outportb(sbDMAa3,(byte)uselen);				// Give LSB of length
	outportb(sbDMAa3,(byte)(uselen >> 8));		// Give MSB of length
	outportb(0x0a,sbDMA);						// Re-enable DMA on channel sbDMA

	// Start playing the thing
	sbWriteDelay();
	sbOut(sbWriteCmd,0x14);
	sbWriteDelay();
	sbOut(sbWriteData,(byte)uselen);
	sbWriteDelay();
	sbOut(sbWriteData,(byte)(uselen >> 8));
	RESTORE();

	return(uselen + 1);
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_SBService() - Services the SoundBlaster DMA interrupt
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_SBService(void)
{
	longword	used;

	sbIn(sbDataAvail);	// Ack interrupt to SB

	if (sbNextSegPtr)
	{
		used = SDL_SBPlaySeg(sbNextSegPtr,sbNextSegLen);
		if (sbNextSegLen <= used)
			sbNextSegPtr = nil;
		else
		{
			sbNextSegPtr += used;
			sbNextSegLen -= used;
		}
	}
	else
	{
		SDL_SBStopSample();
		SDL_DigitizedDone();
	}

	if (sbInterrupt >= 8)
		outportb(0xa0,0x20);	// Ack interrupt (slave PIC)
	outportb(0x20,0x20);	// Ack interrupt
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_SBPlaySample() - Plays a sampled sound on the SoundBlaster. Sets up
//		DMA to play the sound
//
///////////////////////////////////////////////////////////////////////////
#ifdef	_MUSE_
void
#else
static void
#endif
SDL_SBPlaySample(byte *data,longword len)
{
	longword	used;
	int			picport;

	if (GUSPresent)
	{
		GUS_PlaySample(data,len);
		return;
	}

	SDL_SBStopSample();

	DISABLE();

	used = SDL_SBPlaySeg(data,len);
	if (len <= used)
		sbNextSegPtr = nil;
	else
	{
		sbNextSegPtr = data + used;
		sbNextSegLen = len - used;
	}

	// Save old interrupt status and unmask ours
	picport = sbInterrupt < 8 ? 0x21 : 0xa1;
	sbOldIntMask = inportb(picport);
	outportb(picport,sbOldIntMask & ~(1 << (sbInterrupt & 7)));

	sbWriteDelay();
	sbOut(sbWriteCmd,0xd4);						// Make sure DSP DMA is enabled

	sbSamplePlaying = true;

	RESTORE();
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_PositionSBP() - Sets the attenuation levels for the left and right
//		channels by using the mixer chip on the SB Pro. This hits a hole in
//		the address map for normal SBs.
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_PositionSBP(int leftpos,int rightpos)
{
	byte	v;

	if (!SBProPresent || GUSPresent)
		return;

	leftpos = 15 - leftpos;
	rightpos = 15 - rightpos;
	v = ((leftpos & 0x0f) << 4) | (rightpos & 0x0f);

	{
	DISABLE();

	sbOut(sbpMixerAddr,sbpmVoiceVol);
	sbOut(sbpMixerData,v);

	RESTORE();
	}
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_CheckSB() - Checks to see if a SoundBlaster resides at a
//		particular I/O location
//
///////////////////////////////////////////////////////////////////////////
static boolean
SDL_CheckSB(int port)
{
	int	i;

	sbLocation = port << 4;		// Initialize stuff for later use

	sbOut(sbReset,true);		// Reset the SoundBlaster DSP
	SDL_Delay(4);				// Wait >4usec

	sbOut(sbReset,false);		// Turn off sb DSP reset
	SDL_Delay(100);				// Wait >100usec

	for (i = 0;i < 100;i++)
	{
		if (sbIn(sbDataAvail) & 0x80)		// If data is available...
		{
			if (sbIn(sbReadData) == 0xaa)	// If it matches correct value
				return(true);
			else
			{
				sbLocation = -1;			// Otherwise not a SoundBlaster
				return(false);
			}
		}
	}
	sbLocation = -1;						// Retry count exceeded - fail
	return(false);
}

///////////////////////////////////////////////////////////////////////////
//
//	Checks to see if a SoundBlaster is in the system. If the port passed is
//		-1, then it scans through all possible I/O locations. If the port
//		passed is 0, then it uses the default (2). If the port is >0, then
//		it just passes it directly to SDL_CheckSB()
//
///////////////////////////////////////////////////////////////////////////
static boolean
SDL_DetectSoundBlaster(int port)
{
	int	i;

	if (port == 0)					// If user specifies default, use 2
		port = 2;
	if (port == -1)
	{
		if (SDL_CheckSB(2))			// Check default before scanning
			return(true);

		if (SDL_CheckSB(4))			// Check other SB Pro location before scan
			return(true);

		for (i = 1;i <= 6;i++)		// Scan through possible SB locations
		{
			if ((i == 2) || (i == 4))
				continue;

			if (SDL_CheckSB(i))		// If found at this address,
				return(true);		//	return success
		}
		return(false);				// All addresses failed, return failure
	}
	else
		return(SDL_CheckSB(port));	// User specified address or default
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_SBSetDMA() - Sets the DMA channel to be used by the SoundBlaster
//		code. Sets up sbDMA, and sbDMAa1-sbDMAa3 (used by SDL_SBPlaySeg()).
//
///////////////////////////////////////////////////////////////////////////
void
SDL_SBSetDMA(byte channel)
{
	if (channel > 3)
		Quit("SDL_SBSetDMA() - invalid SoundBlaster DMA channel");

	sbDMA = channel;
	sbDMAa1 = sba1Vals[channel];
	sbDMAa2 = sba2Vals[channel];
	sbDMAa3 = sba3Vals[channel];
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_StartSB() - Turns on the SoundBlaster
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_StartSB(void)
{
	byte	timevalue,test;

	sbIntVec = sbIntVectors[sbInterrupt];
	if (sbIntVec < 0)
		Quit("SDL_StartSB: Illegal or unsupported interrupt number for SoundBlaster");

	// The DMA buffer: twice the size in conventional memory, and the half
	// of it that doesn't cross a 64k page
	if (sbDOSSel == -1)
	{
		int	seg;

		seg = __dpmi_allocate_dos_memory((SBBUFSIZE * 2 + 15) / 16,&sbDOSSel);
		if (seg == -1)
			Quit("SDL_StartSB: No conventional memory for the DMA buffer");
		sbDOSAddr = (longword)seg << 4;
		if ((sbDOSAddr >> 16) != ((sbDOSAddr + SBBUFSIZE - 1) >> 16))
			sbDOSAddr = (sbDOSAddr + 0xffff) & ~0xfffful;
	}

	_go32_dpmi_get_protected_mode_interrupt_vector(sbIntVec,&sbOldIntHand);	// Get old interrupt handler
	sbNewIntHand.pm_offset = (unsigned long)SDL_SBService;
	sbNewIntHand.pm_selector = _go32_my_cs();
	if (_go32_dpmi_allocate_iret_wrapper(&sbNewIntHand))
		Quit("SDL_StartSB: Can't hook the SoundBlaster interrupt");
	_go32_dpmi_set_protected_mode_interrupt_vector(sbIntVec,&sbNewIntHand);	// Set mine

	sbWriteDelay();
	sbOut(sbWriteCmd,0xd1);				// Turn on DSP speaker

	// Set the SoundBlaster DAC time constant for 7KHz
	timevalue = 256 - (1000000 / 7000);
	sbWriteDelay();
	sbOut(sbWriteCmd,0x40);
	sbWriteDelay();
	sbOut(sbWriteData,timevalue);

	SBProPresent = false;
	if (sbNoProCheck)
		return;

	// Check to see if this is a SB Pro
	sbOut(sbpMixerAddr,sbpmFMVol);
	sbpOldFMMix = sbIn(sbpMixerData);
	sbOut(sbpMixerData,0xbb);
	test = sbIn(sbpMixerData);
	if (test == 0xbb)
	{
		// Boost FM output levels to be equivilent with digitized output
		sbOut(sbpMixerData,0xff);
		test = sbIn(sbpMixerData);
		if (test == 0xff)
		{
			SBProPresent = true;

			// Save old Voice output levels (SB Pro)
			sbOut(sbpMixerAddr,sbpmVoiceVol);
			sbpOldVOCMix = sbIn(sbpMixerData);

			// Turn SB Pro stereo DAC off
			sbOut(sbpMixerAddr,sbpmControl);
			sbOut(sbpMixerData,0);				// 0=off,2=on
		}
	}
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_ShutSB() - Turns off the SoundBlaster
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_ShutSB(void)
{
	SDL_SBStopSample();

	if (SBProPresent)
	{
		// Restore FM output levels (SB Pro)
		sbOut(sbpMixerAddr,sbpmFMVol);
		sbOut(sbpMixerData,sbpOldFMMix);

		// Restore Voice output levels (SB Pro)
		sbOut(sbpMixerAddr,sbpmVoiceVol);
		sbOut(sbpMixerData,sbpOldVOCMix);
	}

	_go32_dpmi_set_protected_mode_interrupt_vector(sbIntVec,&sbOldIntHand);	// Set vector back
	_go32_dpmi_free_iret_wrapper(&sbNewIntHand);
	if (sbDOSSel != -1)
	{
		__dpmi_free_dos_memory(sbDOSSel);
		sbDOSSel = -1;
	}
}

//	Sound Source Code

///////////////////////////////////////////////////////////////////////////
//
//	SDL_SSStopSample() - Stops a sample playing on the Sound Source
//
///////////////////////////////////////////////////////////////////////////
#ifdef	_MUSE_
void
#else
static void
#endif
SDL_SSStopSample(void)
{
	DISABLE();

	ssSample = 0;

	RESTORE();
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_SSService() - Handles playing the next sample on the Sound Source
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_SSService(void)
{
	byte	v;

	while (ssSample)
	{
		if (inportb(ssStatus) & 0x40)	// Check to see if FIFO is currently empty
			break;						// Nope - don't push any more data out

		v = *ssSample;
		ssSample = ssSample + 1;
		if (!(--ssLengthLeft))
		{
			ssSample = 0;
			SDL_DigitizedDone();
		}

		outportb(ssData,v);			// Pump the value out

		outportb(ssControl,ssOff);	// Pulse printer select
		inportb(ssStatus);
		outportb(ssControl,ssOn);

		inportb(ssStatus);			// Delay a short while
		inportb(ssStatus);
	}
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_SSPlaySample() - Plays the specified sample on the Sound Source
//
///////////////////////////////////////////////////////////////////////////
#ifdef	_MUSE_
void
#else
static void
#endif
SDL_SSPlaySample(byte *data,longword len)
{
	DISABLE();

	ssLengthLeft = len;
	ssSample = data;

	RESTORE();
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_StartSS() - Sets up for and turns on the Sound Source
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_StartSS(void)
{
	if (ssPort == 3)
		ssControl = 0x27a;	// If using LPT3
	else if (ssPort == 2)
		ssControl = 0x37a;	// If using LPT2
	else
		ssControl = 0x3be;	// If using LPT1
	ssStatus = ssControl - 1;
	ssData = ssStatus - 1;

	ssOn = 0x04;
	if (ssIsTandy)
		ssOff = 0x0e;				// Tandy wierdness
	else
		ssOff = 0x0c;				// For normal machines

	outportb(ssControl,ssOn);		// Enable SS
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_ShutSS() - Turns off the Sound Source
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_ShutSS(void)
{
	outportb(ssControl,ssOff);
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_CheckSS() - Checks to see if a Sound Source is present at the
//		location specified by the sound source variables
//
///////////////////////////////////////////////////////////////////////////
static boolean
SDL_CheckSS(void)
{
	boolean		present = false;
	longword	lasttime;
	int			i;

	// Turn the Sound Source on and wait awhile (4 ticks)
	SDL_StartSS();

	lasttime = TimeCount;
	while (TimeCount < lasttime + 4)
		;

	if (inportb(ssStatus) & 0x40)	// Check to see if FIFO is currently empty
		goto checkdone;				// Nope - Sound Source not here

	for (i = 0;i < 32;i++)			// Force FIFO overflow (FIFO is 16 bytes)
	{
		outportb(ssData,0x80);		// Pump a neutral value out

		outportb(ssControl,ssOff);	// Pulse printer select
		inportb(ssStatus);
		outportb(ssControl,ssOn);

		inportb(ssStatus);			// Delay a short while before we do this again
		inportb(ssStatus);
	}

	if (!(inportb(ssStatus) & 0x40))	// Is FIFO overflowed now?
		goto checkdone;				// Nope, still not - Sound Source not here

	present = true;			// Yes - it's here!

checkdone:
	SDL_ShutSS();
	return(present);
}

static boolean
SDL_DetectSoundSource(void)
{
	for (ssPort = 1;ssPort <= 3;ssPort++)
		if (SDL_CheckSS())
			return(true);
	return(false);
}

//
//	PC Sound code
//

///////////////////////////////////////////////////////////////////////////
//
//	SDL_PCPlaySample() - Plays the specified sample on the PC speaker
//
///////////////////////////////////////////////////////////////////////////
#ifdef	_MUSE_
void
#else
static void
#endif
SDL_PCPlaySample(byte *data,longword len)
{
	DISABLE();

	SDL_IndicatePC(true);

	pcLengthLeft = len;
	pcSound = data;

	RESTORE();
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_PCStopSample() - Stops a sample playing on the PC speaker
//
///////////////////////////////////////////////////////////////////////////
#ifdef	_MUSE_
void
#else
static void
#endif
SDL_PCStopSample(void)
{
	DISABLE();

	pcSound = 0;

	SDL_IndicatePC(false);

	outportb(0x61,inportb(0x61) & 0xfd);	// Turn the speaker off

	RESTORE();
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_PCPlaySound() - Plays the specified sound on the PC speaker
//
///////////////////////////////////////////////////////////////////////////
#ifdef	_MUSE_
void
#else
static void
#endif
SDL_PCPlaySound(PCSound *sound)
{
	DISABLE();

	pcLastSample = -1;
	pcLengthLeft = sound->common.length;
	pcSound = sound->data;

	RESTORE();
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_PCStopSound() - Stops the current sound playing on the PC Speaker
//
///////////////////////////////////////////////////////////////////////////
#ifdef	_MUSE_
void
#else
static void
#endif
SDL_PCStopSound(void)
{
	DISABLE();

	pcSound = 0;

	outportb(0x61,inportb(0x61) & 0xfd);	// Turn the speaker off

	RESTORE();
}

#if 0
///////////////////////////////////////////////////////////////////////////
//
//	SDL_PCService() - Handles playing the next sample in a PC sound
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_PCService(void)
{
	byte	s;
	word	t;

	if (pcSound)
	{
		s = *pcSound++;
		if (s != pcLastSample)
		{
		asm	pushf
		asm	cli

			pcLastSample = s;
			if (s)					// We have a frequency!
			{
				t = pcSoundLookup[s];
			asm	mov	bx,[t]

			asm	mov	al,0xb6			// Write to channel 2 (speaker) timer
			asm	out	43h,al
			asm	mov	al,bl
			asm	out	42h,al			// Low byte
			asm	mov	al,bh
			asm	out	42h,al			// High byte

			asm	in	al,0x61			// Turn the speaker & gate on
			asm	or	al,3
			asm	out	0x61,al
			}
			else					// Time for some silence
			{
			asm	in	al,0x61		  	// Turn the speaker & gate off
			asm	and	al,0xfc			// ~3
			asm	out	0x61,al
			}

		asm	popf
		}

		if (!(--pcLengthLeft))
		{
			SDL_PCStopSound();
			SDL_SoundFinished();
		}
	}
}
#endif

///////////////////////////////////////////////////////////////////////////
//
//	SDL_ShutPC() - Turns off the pc speaker
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_ShutPC(void)
{
	DISABLE();

	pcSound = 0;

	outportb(0x61,inportb(0x61) & 0xfc);	// Turn the speaker & gate off

	RESTORE();
}

//
//	Stuff for digitized sounds
//
memptr
SDL_LoadDigiSegment(word page)
{
	memptr	addr;

#if 0	// for debugging
asm	mov	dx,STATUS_REGISTER_1
asm	in	al,dx
asm	mov	dx,ATR_INDEX
asm	mov	al,ATR_OVERSCAN
asm	out	dx,al
asm	mov	al,10	// bright green
asm	out	dx,al
#endif

	if (DigiRaw)
		return(DigiRaw + (longword)page * PMPageSize);

	addr = PM_GetSoundPage(page);
	PM_SetPageLock(PMSoundStart + page,pml_Locked);

#if 0	// for debugging
asm	mov	dx,STATUS_REGISTER_1
asm	in	al,dx
asm	mov	dx,ATR_INDEX
asm	mov	al,ATR_OVERSCAN
asm	out	dx,al
asm	mov	al,3	// blue
asm	out	dx,al
asm	mov	al,0x20	// normal
asm	out	dx,al
#endif

	return(addr);
}

void
SDL_PlayDigiSegment(memptr addr,word len)
{
	switch (DigiMode)
	{
	case sds_PC:
		SDL_PCPlaySample(addr,len);
		break;
	case sds_SoundSource:
		SDL_SSPlaySample(addr,len);
		break;
	case sds_SoundBlaster:
		SDL_SBPlaySample(addr,len);
		break;
	}
}

void
SD_StopDigitized(void)
{
	int	i;
	DISABLE();

	DigiLeft = 0;
	DigiNextAddr = nil;
	DigiNextLen = 0;
	DigiMissed = false;
	DigiPlaying = false;
	DigiNumber = DigiPriority = 0;
	SoundPositioned = false;
	if ((DigiMode == sds_PC) && (SoundMode == sdm_PC))
		SDL_SoundFinished();

	switch (DigiMode)
	{
	case sds_PC:
		SDL_PCStopSample();
		break;
	case sds_SoundSource:
		SDL_SSStopSample();
		break;
	case sds_SoundBlaster:
		SDL_SBStopSample();
		break;
	}

	RESTORE();

	for (i = DigiLastStart;i < DigiLastEnd;i++)
		PM_SetPageLock(i + PMSoundStart,pml_Unlocked);
	DigiLastStart = 1;
	DigiLastEnd = 0;
}

void
SD_Poll(void)
{
	if (DigiLeft && !DigiNextAddr)
	{
		DigiNextLen = (DigiLeft >= PMPageSize)? PMPageSize : (DigiLeft % PMPageSize);
		DigiLeft -= DigiNextLen;
		if (!DigiLeft)
			DigiLastSegment = true;
		DigiNextAddr = SDL_LoadDigiSegment(DigiPage++);
	}
	if (DigiMissed && DigiNextAddr)
	{
		SDL_PlayDigiSegment(DigiNextAddr,DigiNextLen);
		DigiNextAddr = nil;
		DigiMissed = false;
		if (DigiLastSegment)
		{
			DigiPlaying = false;
			DigiLastSegment = false;
		}
	}
	SDL_SetTimerSpeed();
}

void
SD_SetPosition(int leftpos,int rightpos)
{
	if
	(
		(leftpos < 0)
	||	(leftpos > 15)
	||	(rightpos < 0)
	||	(rightpos > 15)
	||	((leftpos == 15) && (rightpos == 15))
	)
		Quit("SD_SetPosition: Illegal position");

	switch (DigiMode)
	{
	case sds_SoundBlaster:
		if (GUSPresent)
			GUS_SetVolume(leftpos,rightpos);
		else
			SDL_PositionSBP(leftpos,rightpos);
		break;
	}
}

void
SD_PlayDigitized(word which,int leftpos,int rightpos)
{
	word	len;
	memptr	addr;

	if (!DigiMode)
		return;

	SD_StopDigitized();
	DigiRaw = nil;
	if (which >= NumDigi)
		Quit("SD_PlayDigitized: bad sound number");

	SD_SetPosition(leftpos,rightpos);

	DigiPage = DigiList[(which * 2) + 0];
	DigiLeft = DigiList[(which * 2) + 1];

	DigiLastStart = DigiPage;
	DigiLastEnd = DigiPage + ((DigiLeft + (PMPageSize - 1)) / PMPageSize);

	len = (DigiLeft >= PMPageSize)? PMPageSize : (DigiLeft % PMPageSize);
	addr = SDL_LoadDigiSegment(DigiPage++);

	DigiPlaying = true;
	DigiLastSegment = false;

	SDL_PlayDigiSegment(addr,len);
	DigiLeft -= len;
	if (!DigiLeft)
		DigiLastSegment = true;

	SD_Poll();
}

void
SDL_DigitizedDone(void)
{
	if (DigiNextAddr)
	{
		SDL_PlayDigiSegment(DigiNextAddr,DigiNextLen);
		DigiNextAddr = nil;
		DigiMissed = false;
	}
	else
	{
		if (DigiLastSegment)
		{
			DigiPlaying = false;
			DigiLastSegment = false;
			if ((DigiMode == sds_PC) && (SoundMode == sdm_PC))
			{
				SDL_SoundFinished();
			}
			else
				DigiNumber = DigiPriority = 0;
			SoundPositioned = false;
		}
		else
			DigiMissed = true;
	}
}

void
SD_SetDigiDevice(SDSMode mode)
{
	boolean	devicenotpresent;

	if (mode == DigiMode)
		return;

	SD_StopDigitized();

	devicenotpresent = false;
	switch (mode)
	{
	case sds_SoundBlaster:
		if (!SoundBlasterPresent)
		{
			if (SoundSourcePresent)
				mode = sds_SoundSource;
			else
				devicenotpresent = true;
		}
		break;
	case sds_SoundSource:
		if (!SoundSourcePresent)
			devicenotpresent = true;
		break;
	}

	if (!devicenotpresent)
	{
		if (DigiMode == sds_SoundSource)
			SDL_ShutSS();

		DigiMode = mode;

		if (mode == sds_SoundSource)
			SDL_StartSS();

		SDL_SetTimerSpeed();
	}
}

void
SDL_SetupDigi(void)
{
	memptr	list;
	word	*p,
			pg;
	int		i;

	PM_UnlockMainMem();
	MM_GetPtr(&list,PMPageSize);
	PM_CheckMainMem();
	p = (word *)PM_GetPage(ChunksInFile - 1);
	memcpy(list,p,PMPageSize);
	pg = PMSoundStart;
	for (i = 0;i < PMPageSize / (sizeof(word) * 2);i++,p += 2)
	{
		if (pg >= ChunksInFile - 1)
			break;
		pg += (p[1] + (PMPageSize - 1)) / PMPageSize;
	}
	PM_UnlockMainMem();
	MM_GetPtr((memptr *)&DigiList,i * sizeof(word) * 2);
	memcpy(DigiList,list,i * sizeof(word) * 2);
	MM_FreePtr(&list);
	NumDigi = i;

	for (i = 0;i < LASTSOUND;i++)
		DigiMap[i] = -1;
}

// 	AdLib Code

///////////////////////////////////////////////////////////////////////////
//
//	alOut(n,b) - Puts b in AdLib card register n
//
///////////////////////////////////////////////////////////////////////////
void
alOut(byte n,byte b)
{
	int	i;

	if (GUSPresent)
	{
		DISABLE();
		GUS_AdLibWrite(n,b);
		RESTORE();
		return;
	}
	{
	DISABLE();

	outportb(alFMAddr,n);
	for (i = 0;i < 6;i++)		// the OPL needs 3.3 usec after the index
		inportb(alFMStatus);
	outportb(alFMData,b);

	RESTORE();

	for (i = 0;i < 35;i++)		// and 23 usec after the data
		inportb(alFMStatus);
	}
}

#if 0
///////////////////////////////////////////////////////////////////////////
//
//	SDL_SetInstrument() - Puts an instrument into a generator
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_SetInstrument(int track,int which,Instrument far *inst,boolean percussive)
{
	byte		c,m;

	if (percussive)
	{
		c = pcarriers[which];
		m = pmodifiers[which];
	}
	else
	{
		c = carriers[which];
		m = modifiers[which];
	}

	tracks[track - 1]->inst = *inst;
	tracks[track - 1]->percussive = percussive;

	alOut(m + alChar,inst->mChar);
	alOut(m + alScale,inst->mScale);
	alOut(m + alAttack,inst->mAttack);
	alOut(m + alSus,inst->mSus);
	alOut(m + alWave,inst->mWave);

	// Most percussive instruments only use one cell
	if (c != 0xff)
	{
		alOut(c + alChar,inst->cChar);
		alOut(c + alScale,inst->cScale);
		alOut(c + alAttack,inst->cAttack);
		alOut(c + alSus,inst->cSus);
		alOut(c + alWave,inst->cWave);
	}

	alOut(which + alFeedCon,inst->nConn);	// DEBUG - I think this is right
}
#endif

///////////////////////////////////////////////////////////////////////////
//
//	SDL_ALStopSound() - Turns off any sound effects playing through the
//		AdLib card
//
///////////////////////////////////////////////////////////////////////////
#ifdef	_MUSE_
void
#else
static void
#endif
SDL_ALStopSound(void)
{
	DISABLE();

	alSound = 0;
	alOut(alFreqH + 0,0);

	RESTORE();
}

static void
SDL_AlSetFXInst(Instrument *inst)
{
	byte		c,m;

	m = modifiers[0];
	c = carriers[0];
	alOut(m + alChar,inst->mChar);
	alOut(m + alScale,inst->mScale);
	alOut(m + alAttack,inst->mAttack);
	alOut(m + alSus,inst->mSus);
	alOut(m + alWave,inst->mWave);
	alOut(c + alChar,inst->cChar);
	alOut(c + alScale,inst->cScale);
	alOut(c + alAttack,inst->cAttack);
	alOut(c + alSus,inst->cSus);
	alOut(c + alWave,inst->cWave);

	// Note: Switch commenting on these lines for old MUSE compatibility
//	alOut(alFeedCon,inst->nConn);
	alOut(alFeedCon,0);
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_ALPlaySound() - Plays the specified sound on the AdLib card
//
///////////////////////////////////////////////////////////////////////////
#ifdef	_MUSE_
void
#else
static void
#endif
SDL_ALPlaySound(AdLibSound *sound)
{
	Instrument	*inst;
	byte		*data;

	SDL_ALStopSound();

	{
	DISABLE();

	alLengthLeft = sound->common.length;
	data = sound->data;
	alSound = data;
	alBlock = ((sound->block & 7) << 2) | 0x20;
	inst = &sound->inst;

	if (!(inst->mSus | inst->cSus))
	{
		RESTORE();
		Quit("SDL_ALPlaySound() - Bad instrument");
	}

	SDL_AlSetFXInst(&alZeroInst);	// DEBUG
	SDL_AlSetFXInst(inst);

	RESTORE();
	}
}

#if 0
///////////////////////////////////////////////////////////////////////////
//
// 	SDL_ALSoundService() - Plays the next sample out through the AdLib card
//
///////////////////////////////////////////////////////////////////////////
//static void
void
SDL_ALSoundService(void)
{
	byte	s;

	if (alSound)
	{
		s = *alSound++;
		if (!s)
			alOut(alFreqH + 0,0);
		else
		{
			alOut(alFreqL + 0,s);
			alOut(alFreqH + 0,alBlock);
		}

		if (!(--alLengthLeft))
		{
			(long)alSound = 0;
			alOut(alFreqH + 0,0);
			SDL_SoundFinished();
		}
	}
}
#endif

#if 0
void
SDL_ALService(void)
{
	byte	a,v;
	word	w;

	if (!sqActive)
		return;

	while (sqHackLen && (sqHackTime <= alTimeCount))
	{
		w = *sqHackPtr++;
		sqHackTime = alTimeCount + *sqHackPtr++;
		a = w & 0xff;
		v = w >> 8;
		alOut(a,v);
		sqHackLen -= 4;
	}
	alTimeCount++;
	if (!sqHackLen)
	{
		sqHackPtr = (word *)sqHack;
		sqHackLen = sqHackSeqLen;
		alTimeCount = sqHackTime = 0;
	}
}
#endif

///////////////////////////////////////////////////////////////////////////
//
//	SDL_ShutAL() - Shuts down the AdLib card for sound effects
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_ShutAL(void)
{
	DISABLE();

	alOut(alEffects,0);
	alOut(alFreqH + 0,0);
	SDL_AlSetFXInst(&alZeroInst);
	alSound = 0;

	RESTORE();
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_CleanAL() - Totally shuts down the AdLib card
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_CleanAL(void)
{
	int	i;
	DISABLE();

	alOut(alEffects,0);
	for (i = 1;i < 0xf5;i++)
		alOut(i,0);

	RESTORE();
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_StartAL() - Starts up the AdLib card for sound effects
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_StartAL(void)
{
	alFXReg = 0;
	alOut(alEffects,alFXReg);
	SDL_AlSetFXInst(&alZeroInst);
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_DetectAdLib() - Determines if there's an AdLib (or SoundBlaster
//		emulating an AdLib) present
//
///////////////////////////////////////////////////////////////////////////
static boolean
SDL_DetectAdLib(void)
{
	byte	status1,status2;
	int		i;

	alOut(4,0x60);	// Reset T1 & T2
	alOut(4,0x80);	// Reset IRQ
	status1 = readstat();
	alOut(2,0xff);	// Set timer 1
	alOut(4,0x21);	// Start timer 1
	SDL_Delay(100);			// Timer 1 overflows after 80 usec

	status2 = readstat();
	alOut(4,0x60);
	alOut(4,0x80);

	if (((status1 & 0xe0) == 0x00) && ((status2 & 0xe0) == 0xc0))
	{
		for (i = 1;i <= 0xf5;i++)	// Zero all the registers
			alOut(i,0);

		alOut(1,0x20);	// Set WSE=1
		alOut(8,0);		// Set CSM=0 & SEL=0

		return(true);
	}
	else
		return(false);
}

////////////////////////////////////////////////////////////////////////////
//
//	SDL_ShutDevice() - turns off whatever device was being used for sound fx
//
////////////////////////////////////////////////////////////////////////////
static void
SDL_ShutDevice(void)
{
	switch (SoundMode)
	{
	case sdm_PC:
		SDL_ShutPC();
		break;
	case sdm_AdLib:
		SDL_ShutAL();
		break;
	}
	SoundMode = sdm_Off;
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_CleanDevice() - totally shuts down all sound devices
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_CleanDevice(void)
{
	if ((SoundMode == sdm_AdLib) || (MusicMode == smm_AdLib))
		SDL_CleanAL();
}

///////////////////////////////////////////////////////////////////////////
//
//	SDL_StartDevice() - turns on whatever device is to be used for sound fx
//
///////////////////////////////////////////////////////////////////////////
static void
SDL_StartDevice(void)
{
	switch (SoundMode)
	{
	case sdm_AdLib:
		SDL_StartAL();
		break;
	}
	SoundNumber = SoundPriority = 0;
}

//	Public routines

///////////////////////////////////////////////////////////////////////////
//
//	SD_SetSoundMode() - Sets which sound hardware to use for sound effects
//
///////////////////////////////////////////////////////////////////////////
boolean
SD_SetSoundMode(SDMode mode)
{
	boolean	result = false;
	word	tableoffset;

	SD_StopSound();

#ifndef	_MUSE_
	if ((mode == sdm_AdLib) && !AdLibPresent)
		mode = sdm_PC;

	switch (mode)
	{
	case sdm_Off:
		NeedsDigitized = false;
		result = true;
		break;
	case sdm_PC:
		tableoffset = STARTPCSOUNDS;
		NeedsDigitized = false;
		result = true;
		break;
	case sdm_AdLib:
		if (AdLibPresent)
		{
			tableoffset = STARTADLIBSOUNDS;
			NeedsDigitized = false;
			result = true;
		}
		break;
	}
#else
	result = true;
#endif

	if (result && (mode != SoundMode))
	{
		SDL_ShutDevice();
		SoundMode = mode;
#ifndef	_MUSE_
		SoundTable = &audiosegs[tableoffset];
#endif
		SDL_StartDevice();
	}

	SDL_SetTimerSpeed();

	return(result);
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_SetMusicMode() - sets the device to use for background music
//
///////////////////////////////////////////////////////////////////////////
boolean
SD_SetMusicMode(SMMode mode)
{
	boolean	result = false;

	SD_FadeOutMusic();
	while (SD_MusicPlaying())
		;

	switch (mode)
	{
	case smm_Off:
		NeedsMusic = false;
		result = true;
		break;
	case smm_AdLib:
		if (AdLibPresent)
		{
			NeedsMusic = true;
			result = true;
		}
		break;
	}

	if (result)
		MusicMode = mode;

	SDL_SetTimerSpeed();

	return(result);
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_Startup() - starts up the Sound Mgr
//		Detects all additional sound hardware and installs my ISR
//
///////////////////////////////////////////////////////////////////////////
void
SD_Startup(void)
{
	int	i;

	if (SD_Started)
		return;

	ssIsTandy = false;
	ssNoCheck = false;
	gusNoCheck = false;
	alNoCheck = false;
	sbNoCheck = false;
	sbNoProCheck = false;
#ifndef	_MUSE_
	for (i = 1;i < _argc;i++)
	{
		switch (US_CheckParm(_argv[i],ParmStrings))
		{
		case 0:						// No AdLib detection
			alNoCheck = true;
			break;
		case 1:						// No SoundBlaster detection
			sbNoCheck = true;
			break;
		case 2:						// No SoundBlaster Pro detection
			sbNoProCheck = true;
			break;
		case 3:
			ssNoCheck = true;		// No Sound Source detection
			break;
		case 4:						// Tandy Sound Source handling
			ssIsTandy = true;
			break;
		case 5:						// Sound Source present at LPT1
			ssPort = 1;
			ssNoCheck = SoundSourcePresent = true;
			break;
		case 6:                     // Sound Source present at LPT2
			ssPort = 2;
			ssNoCheck = SoundSourcePresent = true;
			break;
		case 7:                     // Sound Source present at LPT3
			ssPort = 3;
			ssNoCheck = SoundSourcePresent = true;
			break;
		case 8:						// don't use the Gravis UltraSound
			gusNoCheck = true;
			break;
		}
	}
#endif

	SoundUserHook = 0;

	LocalTime = TimeCount = alTimeCount = 0;

	SD_SetSoundMode(sdm_Off);
	SD_SetMusicMode(smm_Off);

	if (!ssNoCheck)
		SoundSourcePresent = SDL_DetectSoundSource();

	if (!gusNoCheck && GUS_Startup())
	{
		// it stands in for both the AdLib and the Sound Blaster
		GUSPresent = true;
		AdLibPresent = true;
		SoundBlasterPresent = true;
	}
	else if (!alNoCheck)
	{
		AdLibPresent = SDL_DetectAdLib();
		if (AdLibPresent && !sbNoCheck)
		{
			int port = -1;
			char *env = getenv("BLASTER");
			if (env)
			{
				long temp;
				while (*env)
				{
					while (isspace(*env))
						env++;

					switch (toupper(*env))
					{
					case 'A':
						temp = strtol(env + 1,&env,16);
						if
						(
							(temp >= 0x210)
						&&	(temp <= 0x260)
						&&	(!(temp & 0x00f))
						)
							port = (temp - 0x200) >> 4;
						else
							Quit("SD_Startup: Unsupported address value in BLASTER");
						break;
					case 'I':
						temp = strtol(env + 1,&env,10);
						if
						(
							(temp >= 0)
						&&	(temp <= 10)
						&&	(sbIntVectors[temp] != -1)
						)
						{
							sbInterrupt = temp;
							sbIntVec = sbIntVectors[sbInterrupt];
						}
						else
							Quit("SD_Startup: Unsupported interrupt value in BLASTER");
						break;
					case 'D':
						temp = strtol(env + 1,&env,10);
						if ((temp == 0) || (temp == 1) || (temp == 3))
							SDL_SBSetDMA(temp);
						else
							Quit("SD_Startup: Unsupported DMA value in BLASTER");
						break;
					default:
						while (isspace(*env))
							env++;
						while (*env && !isspace(*env))
							env++;
						break;
					}
				}
			}
			SoundBlasterPresent = SDL_DetectSoundBlaster(port);
		}
	}

	for (i = 0;i < 255;i++)
		pcSoundLookup[i] = i * 60;

	if (SoundBlasterPresent && !GUSPresent)
		SDL_StartSB();

	SDL_SetupDigi();

	SD_Started = true;
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_Default() - Sets up the default behaviour for the Sound Mgr whether
//		the config file was present or not.
//
///////////////////////////////////////////////////////////////////////////
void
SD_Default(boolean gotit,SDMode sd,SMMode sm)
{
	boolean	gotsd,gotsm;

	gotsd = gotsm = gotit;

	if (gotsd)	// Make sure requested sound hardware is available
	{
		switch (sd)
		{
		case sdm_AdLib:
			gotsd = AdLibPresent;
			break;
		}
	}
	if (!gotsd)
	{
		if (AdLibPresent)
			sd = sdm_AdLib;
		else
			sd = sdm_PC;
	}
	if (sd != SoundMode)
		SD_SetSoundMode(sd);


	if (gotsm)	// Make sure requested music hardware is available
	{
		switch (sm)
		{
		case sdm_AdLib:
			gotsm = AdLibPresent;
			break;
		}
	}
	if (!gotsm)
	{
		if (AdLibPresent)
			sm = smm_AdLib;
	}
	if (sm != MusicMode)
		SD_SetMusicMode(sm);
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_Shutdown() - shuts down the Sound Mgr
//		Removes sound ISR and turns off whatever sound hardware was active
//
///////////////////////////////////////////////////////////////////////////
void
SD_Shutdown(void)
{
	if (!SD_Started)
		return;

	SD_MusicOff();
	SD_StopSound();
	SDL_ShutDevice();
	SDL_CleanDevice();

	if (SoundBlasterPresent && !GUSPresent)
		SDL_ShutSB();

	if (SoundSourcePresent)
		SDL_ShutSS();

	{
	DISABLE();

	SDL_SetTimer0(0);

	SDL_RemoveTimer();

	RESTORE();
	}

	GUS_Shutdown();

	SD_Started = false;
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_SetUserHook() - sets the routine that the Sound Mgr calls every 1/70th
//		of a second from its timer 0 ISR
//
///////////////////////////////////////////////////////////////////////////
void
SD_SetUserHook(void (* hook)(void))
{
	SoundUserHook = hook;
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_PositionSound() - Sets up a stereo imaging location for the next
//		sound to be played. Each channel ranges from 0 to 15.
//
///////////////////////////////////////////////////////////////////////////
int		(*SD_PlayHook) (soundnames sound);

///////////////////////////////////////////////////////////////////////////
//
//	SD_SoundHasVolume() - true if the sound would be played digitized on a
//		device whose volume SD_SetPosition() sets (SB Pro mixer, GUS); the
//		AdLib, the PC speaker, a plain SB and the Sound Source play every
//		sound at full volume
//
///////////////////////////////////////////////////////////////////////////
boolean
SD_SoundHasVolume(soundnames sound)
{
	if (DigiMode != sds_SoundBlaster || (!SBProPresent && !GUSPresent))
		return(false);
	return(sound == LASTSOUND || DigiMap[sound] != -1);	// LASTSOUND: SD_PlaySample
}

void
SD_PositionSound(int leftvol,int rightvol)
{
	LeftPosition = leftvol;
	RightPosition = rightvol;
	nextsoundpos = true;
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_PlaySample() - plays a digitized sound from memory (8 bit unsigned,
//		7042 Hz, like the page file's) at the given priority, positioned
//		if SD_PositionSound() was called. Not played (false) without a
//		digitized sound device. The data must stay until it's done.
//
///////////////////////////////////////////////////////////////////////////
boolean
SD_PlaySample(byte *data,longword len,word priority)
{
	boolean	ispos;
	int		lp,rp;
	word	seglen;
	memptr	addr;

	lp = LeftPosition;
	rp = RightPosition;
	LeftPosition = 0;
	RightPosition = 0;
	ispos = nextsoundpos;
	nextsoundpos = false;

	if (DigiMode == sds_Off || !data || !len)
		return(false);
	if (len > 0xffff)
		len = 0xffff;				// DigiLeft is a word

	if ((DigiMode == sds_PC) && (SoundMode == sdm_PC))
	{
		if (priority < SoundPriority)
			return(false);
		SDL_PCStopSound();
	}
	else if (priority < DigiPriority)
		return(false);

	SD_StopDigitized();
	SD_SetPosition(lp,rp);

	DigiRaw = data;
	DigiPage = 0;
	DigiLeft = len;
	DigiLastStart = 1;				// no pages to unlock
	DigiLastEnd = 0;

	seglen = (DigiLeft >= PMPageSize)? PMPageSize : DigiLeft;
	addr = SDL_LoadDigiSegment(DigiPage++);

	DigiPlaying = true;
	DigiLastSegment = false;

	SDL_PlayDigiSegment(addr,seglen);
	DigiLeft -= seglen;
	if (!DigiLeft)
		DigiLastSegment = true;

	SoundPositioned = ispos;
	if ((DigiMode == sds_PC) && (SoundMode == sdm_PC))
	{
		SoundNumber = LASTSOUND;	// not one of the game's
		SoundPriority = priority;
	}
	else
	{
		DigiNumber = LASTSOUND;
		DigiPriority = priority;
	}

	SD_Poll();
	return(true);
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_PlaySound() - plays the specified sound on the appropriate hardware
//
///////////////////////////////////////////////////////////////////////////
boolean
SD_PlaySound(soundnames sound)
{
	boolean		ispos;
	SoundCommon	*s;
	int	lp,rp;

	// an unpositioned sound the game may want to place (wl_net.c: one
	// another player made); the hook plays it positioned, or returns -1
	if (!nextsoundpos && SD_PlayHook)
	{
		int	r = SD_PlayHook (sound);
		if (r >= 0)
			return r;
	}

	lp = LeftPosition;
	rp = RightPosition;
	LeftPosition = 0;
	RightPosition = 0;

	ispos = nextsoundpos;
	nextsoundpos = false;

	if (sound == -1)
		return(false);

	s = (SoundCommon *)SoundTable[sound];
	if ((SoundMode != sdm_Off) && !s)
		Quit("SD_PlaySound() - Uncached sound");

	if ((DigiMode != sds_Off) && (DigiMap[sound] != -1))
	{
		if ((DigiMode == sds_PC) && (SoundMode == sdm_PC))
		{
			if (s->priority < SoundPriority)
				return(false);

			SDL_PCStopSound();

			SD_PlayDigitized(DigiMap[sound],lp,rp);
			SoundPositioned = ispos;
			SoundNumber = sound;
			SoundPriority = s->priority;
		}
		else
		{
			DISABLE();
			if (DigiPriority && !DigiNumber)
			{
				RESTORE();
				Quit("SD_PlaySound: Priority without a sound");
			}
			RESTORE();

			if (s->priority < DigiPriority)
				return(false);

			SD_PlayDigitized(DigiMap[sound],lp,rp);
			SoundPositioned = ispos;
			DigiNumber = sound;
			DigiPriority = s->priority;
		}

		return(true);
	}

	if (SoundMode == sdm_Off)
		return(false);
	if (!s->length)
		Quit("SD_PlaySound() - Zero length sound");
	if (s->priority < SoundPriority)
		return(false);

	switch (SoundMode)
	{
	case sdm_PC:
		SDL_PCPlaySound((void *)s);
		break;
	case sdm_AdLib:
		SDL_ALPlaySound((void *)s);
		break;
	}

	SoundNumber = sound;
	SoundPriority = s->priority;

	return(false);
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_SoundPlaying() - returns the sound number that's playing, or 0 if
//		no sound is playing
//
///////////////////////////////////////////////////////////////////////////
word
SD_SoundPlaying(void)
{
	boolean	result = false;

	switch (SoundMode)
	{
	case sdm_PC:
		result = pcSound? true : false;
		break;
	case sdm_AdLib:
		result = alSound? true : false;
		break;
	}

	if (result)
		return(SoundNumber);
	else
		return(false);
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_StopSound() - if a sound is playing, stops it
//
///////////////////////////////////////////////////////////////////////////
void
SD_StopSound(void)
{
	if (DigiPlaying)
		SD_StopDigitized();

	switch (SoundMode)
	{
	case sdm_PC:
		SDL_PCStopSound();
		break;
	case sdm_AdLib:
		SDL_ALStopSound();
		break;
	}

	SoundPositioned = false;

	SDL_SoundFinished();
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_WaitSoundDone() - waits until the current sound is done playing
//
///////////////////////////////////////////////////////////////////////////
void
SD_WaitSoundDone(void)
{
	while (SD_SoundPlaying())
		;
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_MusicOn() - turns on the sequencer
//
///////////////////////////////////////////////////////////////////////////
void
SD_MusicOn(void)
{
	sqActive = true;
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_MusicOff() - turns off the sequencer and any playing notes
//
///////////////////////////////////////////////////////////////////////////
void
SD_MusicOff(void)
{
	word	i;


	switch (MusicMode)
	{
	case smm_AdLib:
		alFXReg = 0;
		alOut(alEffects,0);
		for (i = 0;i < sqMaxTracks;i++)
			alOut(alFreqH + i + 1,0);
		break;
	}
	sqActive = false;
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_StartMusic() - starts playing the music pointed to
//
///////////////////////////////////////////////////////////////////////////
void
SD_StartMusic(MusicGroup *music)
{
	SD_MusicOff();
	{
	DISABLE();

	if (MusicMode == smm_AdLib)
	{
		sqHackPtr = sqHack = music->values;
		sqHackSeqLen = sqHackLen = music->length;
		sqHackTime = 0;
		alTimeCount = 0;
		SD_MusicOn();
	}

	RESTORE();
	}
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_FadeOutMusic() - starts fading out the music. Call SD_MusicPlaying()
//		to see if the fadeout is complete
//
///////////////////////////////////////////////////////////////////////////
void
SD_FadeOutMusic(void)
{
	switch (MusicMode)
	{
	case smm_AdLib:
		// DEBUG - quick hack to turn the music off
		SD_MusicOff();
		break;
	}
}

///////////////////////////////////////////////////////////////////////////
//
//	SD_MusicPlaying() - returns true if music is currently playing, false if
//		not
//
///////////////////////////////////////////////////////////////////////////
boolean
SD_MusicPlaying(void)
{
	boolean	result;

	switch (MusicMode)
	{
	case smm_AdLib:
		result = false;
		// DEBUG - not written
		break;
	default:
		result = false;
	}

	return(result);
}

/*
=============================================================================

			TIMER 0 SERVICE (was ID_SD_A.ASM)

	One handler on IRQ 0 does what the three assembly ISRs did, according
	to t0Mode (set with the timer rate by SDL_SetTimerSpeed):

	t0_Slow		140 Hz: sound effects every tick, TimeCount every 2nd
	t0_Fast		700 Hz: AdLib music and Sound Source every tick,
				sound effects every 5th, TimeCount every 10th
	t0_Extreme	7000 Hz: a PC speaker sample every tick, and the 700 Hz
				work every 10th

	The original chained to the BIOS timer handler whenever its counts
	added up to a full 18.2 Hz tick. In protected mode we can't call the
	real mode handler from here, so its work is done instead: the BIOS
	tick count (and so the DOS clock) and the floppy motor timeout.

=============================================================================
*/

static	volatile word	count_time,count_fx,extreme;
static	volatile boolean	pcindicate;

// PC speaker bits for each digitized sample value (pcdtab in ID_SD_A.ASM)
#define	PCDBIT(v)	(((v) & 0x80) ? 2 : 0)

static void
SDL_IndicatePC(boolean on)
{
	pcindicate = on;
}

//
//	SDL_Delay() - waits about usec microseconds, by reading the AdLib's
//		status port (as the original did): an ISA port read takes about one
//
static void
SDL_Delay(int usec)
{
	while (usec-- > 0)
		inportb(alFMStatus);
}

//
//	SDL_BIOSTick() - what the BIOS timer interrupt does 18.2 times a second
//
static void
SDL_BIOSTick(void)
{
	longword	ticks;
	byte		motor;

	ticks = _farpeekl(_dos_ds,0x46c) + 1;
	if (ticks >= 0x1800b0)			// midnight
	{
		ticks = 0;
		_farpokeb(_dos_ds,0x470,1);
	}
	_farpokel(_dos_ds,0x46c,ticks);

	motor = _farpeekb(_dos_ds,0x440);
	if (motor)
	{
		_farpokeb(_dos_ds,0x440,--motor);
		if (!motor)					// turn the floppy motors off
		{
			_farpokeb(_dos_ds,0x43f,_farpeekb(_dos_ds,0x43f) & 0xf0);
			outportb(0x3f2,0x0c);
		}
	}
}

//
//	DOFX - the next byte of the PC speaker or AdLib sound effect
//
static void
SDL_DoFX(void)
{
	byte	s;
	word	t;

	if (pcSound)
	{
		s = *pcSound;
		pcSound = pcSound + 1;
		if (pcLastSample != s)
		{
			pcLastSample = s;
			if (s)					// We have a frequency!
			{
				t = pcSoundLookup[s];
				outportb(pcTAccess,0xb6);	// Write to channel 2 (speaker) timer
				outportb(pcTimer,t);		// Low byte
				outportb(pcTimer,t >> 8);	// High byte
				outportb(pcSpeaker,inportb(pcSpeaker) | 3);	// Turn the speaker & gate on
			}
			else					// Time for some silence
				outportb(pcSpeaker,inportb(pcSpeaker) & 0xfc);	// Turn the speaker & gate off
		}

		if (!(--pcLengthLeft))
		{
			pcSound = 0;
			SoundNumber = SoundPriority = 0;
			outportb(pcSpeaker,inportb(pcSpeaker) & 0xfd);	// Turn the speaker off
		}
	}

	if (alSound)
	{
		s = *alSound;
		if (s)
		{
			alOut(alFreqL,s);
			alOut(alFreqH,alBlock);
		}
		else
			alOut(alFreqH,0);
		alSound = alSound + 1;

		if (!(--alLengthLeft))
		{
			alSound = 0;
			SoundNumber = SoundPriority = 0;
			alOut(alFreqH,0);
		}
	}
}

//
//	TIME - TimeCount goes up every other call
//
static void
SDL_DoTime(void)
{
	if (++count_time >= 2)
	{
		LocalTime++;
		TimeCount++;
		count_time = 0;
	}
}

//
//	The AdLib music: the IMF data is pairs of (register, value) and delays
//
static void
SDL_DoMusic(void)
{
	word	w;

	if (sqHackLen)
	{
		while (sqHackTime <= alTimeCount)
		{
			w = sqHackPtr[0];
			sqHackTime = alTimeCount + sqHackPtr[1];
			alOut(w & 0xff,w >> 8);
			sqHackPtr = sqHackPtr + 2;
			sqHackLen -= 4;
			if (!sqHackLen)
				break;
		}
	}

	alTimeCount++;
	if (!sqHackLen)
	{
		sqHackPtr = sqHack;
		sqHackLen = sqHackSeqLen;
		alTimeCount = 0;
		sqHackTime = 0;
	}
}

static void
SDL_t0Service(void)
{
	byte	s;

	HackCount++;

	switch (t0Mode)
	{
	case t0_Extreme:
		if (pcindicate && pcSound)
		{
			s = *pcSound;
			pcSound = pcSound + 1;
			outportb(pcSpeaker,(inportb(pcSpeaker) & 0xfc) | PCDBIT(s));
			if (!(--pcLengthLeft))
			{
				pcSound = 0;
				outportb(pcSpeaker,inportb(pcSpeaker) & 0xfc);
				SDL_DigitizedDone();
			}
		}
		if (++extreme < 10)
		{
			outportb(0x20,0x20);	// Ack the interrupt
			return;
		}
		extreme = 0;
		// and the 700 Hz work

	case t0_Fast:
		if (++count_fx >= 5)
		{
			count_fx = 0;
			SDL_DoFX();
			SDL_DoTime();
		}
		if (sqActive)
			SDL_DoMusic();
		if (ssSample)
			SDL_SSService();
		if (GUSPresent)
			GUS_Service();
		break;

	default:
		SDL_DoTime();
		if (pcSound || alSound)
			SDL_DoFX();
		break;
	}

	TimerCount += TimerDivisor;
	if (TimerCount >= 0x10000)		// a full BIOS tick has gone by
	{
		TimerCount &= 0xffff;
		SDL_BIOSTick();
	}
	outportb(0x20,0x20);	// Ack the interrupt
}

//
//	SDL_InstallTimer() - puts SDL_t0Service on IRQ 0 (once)
//
static void
SDL_InstallTimer(void)
{
	if (t0Installed)
		return;

	_go32_dpmi_get_protected_mode_interrupt_vector(8,&t0OldService);
	t0NewService.pm_offset = (unsigned long)SDL_t0Service;
	t0NewService.pm_selector = _go32_my_cs();
	if (_go32_dpmi_allocate_iret_wrapper(&t0NewService))
		Quit("SDL_InstallTimer: Can't hook the timer interrupt");
	_go32_dpmi_set_protected_mode_interrupt_vector(8,&t0NewService);
	t0Installed = true;
}

//
//	SDL_RemoveTimer() - gives IRQ 0 back
//
static void
SDL_RemoveTimer(void)
{
	if (!t0Installed)
		return;

	_go32_dpmi_set_protected_mode_interrupt_vector(8,&t0OldService);
	_go32_dpmi_free_iret_wrapper(&t0NewService);
	t0Installed = false;
	TimerRate = 0;
}

//
//	SD_EmergencyShutdown() - puts the timer, the speaker and the cards back
//		after a crash, without trusting much
//
void
SD_EmergencyShutdown(void)
{
	disable();
	pcSound = 0;
	alSound = 0;
	ssSample = 0;
	sqActive = false;
	outportb(0x43,0x36);			// timer 0 back to 18.2 Hz
	outportb(0x40,0);
	outportb(0x40,0);
	SDL_RemoveTimer();
	outportb(pcSpeaker,inportb(pcSpeaker) & 0xfc);
	if (sbSamplePlaying)
	{
		sbSamplePlaying = false;
		sbOut(sbWriteCmd,0xd0);
	}
	GUS_Shutdown();
	enable();
	if (AdLibPresent)
		SDL_CleanAL();
}
