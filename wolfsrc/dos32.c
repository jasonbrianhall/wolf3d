// DOS32.C
//
// DJGPP specifics that have no home in the original modules: the startup
// flags, the near pointer to video memory, how much memory is left, and
// putting the machine back (timer, keyboard, text mode) if the program
// crashes instead of quitting.

#include "wl_def.h"
#include <crt0.h>
#include <signal.h>
#include <sys/exceptn.h>
#pragma hdrstop

// The keyboard, timer and Sound Blaster interrupt handlers run at
// interrupt time and touch the game's data (and the sound data): lock all
// of the program's memory, so the DPMI host never pages any of it out.
int _crt0_startup_flags = _CRT0_FLAG_LOCK_MEMORY;

void SD_EmergencyShutdown (void);
void IN_EmergencyShutdown (void);

static boolean	dos32started;


/*
=================
=
= DOS32_Mark / DOS32_WriteMarks
=
= -timing: how long each part of the startup took, in STARTUP.TXT
=
=================
*/

static	boolean	timing;
static	int		nummarks;
static	char	*marknames[32];
static	long	marktimes[32];

void DOS32_Mark (char *what)
{
	if (!timing || nummarks == 32)
		return;
	marknames[nummarks] = what;
	marktimes[nummarks] = _farpeekl (_dos_ds,0x46c);	// 18.2 per second
	nummarks++;
}

void DOS32_WriteMarks (void)
{
	FILE	*f;
	int		i;

	if (!timing || !nummarks)
		return;
	f = fopen ("STARTUP.TXT","w");
	if (!f)
		return;
	fprintf (f,"Seconds since main() started:\n");
	for (i=0;i<nummarks;i++)
		fprintf (f,"%7.2f  %s\n",(marktimes[i]-marktimes[0])/18.2065,marknames[i]);
	fclose (f);
	nummarks = 0;
}


/*
=================
=
= DOS32_MemoryFree
=
= What the signon screen's memory gauges and the old 640K checks are
= told. The memory isn't checked at all (that's what made the original
= crawl on big machines): memory comes from malloc as needed, and a
= machine without enough for the game gets "Out of memory" from it.
=
=================
*/

#define REPORTEDMEMORY	(16L*1024*1024)

long DOS32_MemoryFree (void)
{
	return REPORTEDMEMORY;
}


/*
=================
=
= DOS32_Fault
=
= Called for a crash (page fault, divide by zero...) or Ctrl-Break: gives
= back the timer and keyboard interrupts, goes back to text mode, then lets
= DJGPP print its traceback
=
=================
*/

void NetShutdown (void);

static void DOS32_Fault (int sig)
{
	NetShutdown ();
	SD_EmergencyShutdown ();
	IN_EmergencyShutdown ();
	VL_SetTextMode ();

	signal (sig,SIG_DFL);
	raise (sig);
}


/*
=================
=
= DOS32_Startup
=
=================
*/

void DOS32_Startup (void)
{
	{
		int i;
		for (i=1;i<_argc;i++)
			if (!stricmp (_argv[i],"-timing"))
				timing = true;
		DOS32_Mark ("main");
	}
	if (dos32started)
		return;

	if (!__djgpp_nearptr_enable ())
	{
		printf ("This program needs near pointer access to video memory,\n"
			"which this DPMI host does not allow. Run it in plain DOS (with\n"
			"CWSDPMI, which is built in), FreeDOS, Windows 9x or DOSBox.\n");
		exit (1);
	}

	__djgpp_set_ctrl_c (0);			// Ctrl-C is a key, not a signal

	_fmode = O_BINARY;				// creat() and open() as Borland's
									// were used: savegames are binary

	signal (SIGSEGV,DOS32_Fault);
	signal (SIGFPE,DOS32_Fault);
	signal (SIGILL,DOS32_Fault);
	signal (SIGABRT,DOS32_Fault);
	signal (SIGINT,DOS32_Fault);

	dos32started = true;
}


/*
=================
=
= ltoa / ultoa
=
= Borland's number to string conversions (DJGPP has itoa only)
=
=================
*/

char *ultoa (unsigned long value, char *string, int radix)
{
	char	digits[33];
	int		i,d;
	char	*out;

	i = 0;
	do
	{
		d = value % radix;
		digits[i++] = d < 10 ? '0'+d : 'a'+d-10;
		value /= radix;
	} while (value);

	out = string;
	while (i)
		*out++ = digits[--i];
	*out = 0;
	return string;
}

char *ltoa (long value, char *string, int radix)
{
	if (value < 0 && radix == 10)
	{
		string[0] = '-';
		ultoa (-(unsigned long)value,string+1,radix);
		return string;
	}
	return ultoa ((unsigned long)value,string,radix);
}
