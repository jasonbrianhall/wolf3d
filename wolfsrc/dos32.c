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
= DOS32_MemoryFree
=
= Bytes the DPMI host can still give us (physical memory, as virtual
= memory would be no good to the interrupt handlers anyway)
=
=================
*/

long DOS32_MemoryFree (void)
{
	__dpmi_free_mem_info	info;
	unsigned long			pages;

	if (__dpmi_get_free_memory_information (&info))
		return 4L*1024*1024;

	pages = info.total_number_of_free_pages;
	if (pages == 0xfffffffful)
	{
		if (info.largest_available_free_block_in_bytes == 0xfffffffful)
			return 4L*1024*1024;
		return info.largest_available_free_block_in_bytes;
	}
	if (pages > 0x7ffff)
		pages = 0x7ffff;
	return pages*4096;
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
