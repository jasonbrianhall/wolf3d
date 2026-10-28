// ID_MM.C

/*
=============================================================================

		   ID software memory manager
		   --------------------------

Primary coder: John Carmack

32 bit DJGPP version
--------------------

The original manager carved the 640k of conventional memory (plus EMS and
XMS) into paragraph aligned blocks, moved unlocked blocks around to
compact them and threw purgable ones out when it ran short, writing NULL
into their owner's pointer.

Under DPMI the program has a flat address space with megabytes of memory,
so a block is simply malloc'ed and stays where it is, and purgable blocks
are kept (the game only needs a few megabytes, even for SOD).
Nothing scans conventional memory or probes for EMS/XMS at startup.

The interface is the same, so the rest of the game is unchanged:
MM_GetPtr fills in a pointer, MM_FreePtr frees it and NULLs it,
purge levels and locks are accepted and ignored.

=============================================================================
*/

#include "id_heads.h"
#pragma hdrstop

/*
=============================================================================

						 GLOBAL VARIABLES

=============================================================================
*/

mminfotype	mminfo;
memptr		bufferseg;
boolean		mmerror;

void		(* beforesort) (void);
void		(* aftersort) (void);

/*
=============================================================================

						 LOCAL VARIABLES

=============================================================================
*/

static	boolean		mmstarted;
static	boolean		bombonerror;
static	long		mmallocated;		// bytes handed out, for the debug screens

//
// Each block carries its size in front of it, for MM_ShowMemory / MM_FreePtr
//
typedef struct
{
	long	size;
	long	pad[3];					// keep the data 16 byte aligned
} mmheader;

//==========================================================================

/*
===================
=
= MM_Startup
=
= Allocates bufferseg misc buffer
=
===================
*/

void MM_Startup (void)
{
	long	avail;

	if (mmstarted)
		MM_Shutdown ();

	mmstarted = true;
	bombonerror = true;
	mmallocated = 0;

//
// there is no fixed pool any more: report what the DPMI host has left,
// as everything the game checks against was sized for 640k
//
	avail = DOS32_MemoryFree ();
	if (avail < 1024L*1024)
		avail = 1024L*1024;
	mminfo.nearheap = 0;
	mminfo.farheap = avail;
	mminfo.EMSmem = 0;
	mminfo.XMSmem = 0;
	mminfo.mainmem = avail;

	MM_GetPtr (&bufferseg,BUFFERSIZE);
}

//==========================================================================

/*
====================
=
= MM_Shutdown
=
====================
*/

void MM_Shutdown (void)
{
	if (!mmstarted)
		return;

	MM_FreePtr (&bufferseg);
	mmstarted = false;
}

//==========================================================================

/*
====================
=
= MM_MapEMS
=
= No EMS
=
====================
*/

void MM_MapEMS (void)
{
}

void MML_UseSpace (unsigned segstart, unsigned seglength)
{
}

//==========================================================================

/*
====================
=
= MM_GetPtr
=
= Allocates an unlocked, unpurgable block
=
====================
*/

void MM_GetPtr (memptr *baseptr,unsigned long size)
{
	mmheader	*block;

	block = malloc (sizeof(mmheader) + size);
	if (!block)
	{
		*baseptr = NULL;
		if (bombonerror)
			Quit ("MM_GetPtr: Out of memory!");
		mmerror = true;
		return;
	}

	block->size = size;
	mmallocated += size;
	*baseptr = (memptr)(block+1);
}

//==========================================================================

/*
====================
=
= MM_FreePtr
=
= Deallocates an unlocked, purgable block
=
====================
*/

void MM_FreePtr (memptr *baseptr)
{
	mmheader	*block;

	if (!*baseptr)
		return;

	block = ((mmheader *)*baseptr) - 1;
	mmallocated -= block->size;
	free (block);
	*baseptr = NULL;
}

//==========================================================================

/*
=====================
=
= MM_SetPurge
=
= Sets the purge level for a block (locked blocks cannot be made purgable)
=
=====================
*/

void MM_SetPurge (memptr *baseptr, int purge)
{
}

//==========================================================================

/*
=====================
=
= MM_SetLock
=
= Locks / unlocks the block
=
=====================
*/

void MM_SetLock (memptr *baseptr, boolean locked)
{
}

//==========================================================================

/*
=====================
=
= MM_SortMem
=
= Used to throw out all purgable stuff and compress movable blocks.
= Nothing moves now, but it still stops the current sound effect and calls
= the hooks, as the game expects
=
=====================
*/

void MM_SortMem (void)
{
	SD_StopSound();

	if (beforesort)
		beforesort();

	if (aftersort)
		aftersort();
}

//==========================================================================

/*
=====================
=
= MM_ShowMemory
=
=====================
*/

void MM_ShowMemory (void)
{
	unsigned temp;
	long	used,total;

	temp = bufferofs;
	bufferofs = displayofs;

	total = mmallocated + DOS32_MemoryFree ();
	if (total <= 0)
		total = 1;
	used = mmallocated * 320 / total;

	VW_Bar (0,0,320,8,0);
	VW_Hlin (0,used,0,9);			// medium blue = used

	VW_FadeIn ();
	IN_Ack();

	bufferofs = temp;
}

//==========================================================================

/*
=====================
=
= MM_DumpData
=
=====================
*/

void MM_DumpData (void)
{
	FILE	*dumpfile;

	dumpfile = fopen ("MMDUMP.TXT","w");
	if (!dumpfile)
		Quit ("MM_DumpData: Couldn't open MMDUMP.TXT!");

	fprintf (dumpfile,"Allocated: %ld bytes\nFree: %ld bytes\n",
		mmallocated,DOS32_MemoryFree ());
	fclose (dumpfile);
	Quit ("MMDUMP.TXT created.");
}

//==========================================================================

/*
======================
=
= MM_UnusedMemory
=
= Returns the total free space without purging
=
======================
*/

long MM_UnusedMemory (void)
{
	return DOS32_MemoryFree ();
}

//==========================================================================

/*
======================
=
= MM_TotalFree
=
= Returns the total free space with purging
=
======================
*/

long MM_TotalFree (void)
{
	return DOS32_MemoryFree ();
}

//==========================================================================

/*
=====================
=
= MM_BombOnError
=
=====================
*/

void MM_BombOnError (boolean bomb)
{
	bombonerror = bomb;
}
