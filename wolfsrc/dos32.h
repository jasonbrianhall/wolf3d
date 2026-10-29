// DOS32.H
//
// What the 16-bit Borland C++ 3.0 source needs to build as a 32-bit DJGPP
// (DPMI) program. Memory is flat, so far/huge/_seg pointers are plain
// pointers, the far heap is malloc, and video memory is reached through
// DJGPP's near pointer (the whole of conventional memory, mapped into our
// data segment, see VGAMEM).

#ifndef __DOS32_H__
#define __DOS32_H__

#include <dos.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <sys/nearptr.h>
#include <sys/farptr.h>
#include <sys/movedata.h>
#include <string.h>
#include <stdlib.h>

#define far
#define near
#define huge
#define _seg

#define _fmemcpy	memcpy
#define _fmemset	memset
#define _fmemcmp	memcmp
#define _fstrcpy	strcpy
#define _fstrcat	strcat
#define _fstrlen	strlen
#define _fstrcmp	strcmp
#define _fstricmp	stricmp
#define _fstrnicmp	strnicmp

#define farmalloc	malloc
#define farfree		free

// Borland's command line globals
#define _argc		__crt0_argc
#define _argv		__crt0_argv
extern int __crt0_argc;
extern char **__crt0_argv;

// The VGA's A000:0000 window, through the near pointer. Read it every time:
// the base can move when the DPMI host grows our memory.
#define VGAMEM		((byte *)(0xa0000 + __djgpp_conventional_base))
#define VGAPTR(ofs)	(VGAMEM + (unsigned)(ofs))

// pushf / cli ... popf
#define DISABLE()	int __intson = disable()
#define RESTORE()	do { if (__intson) enable(); } while (0)

// The PIT runs at 1193182 Hz; Wolf used 1192030 (as id's code does)
#define PIT_HZ		1192030L

char *ltoa (long value, char *string, int radix);		// Borland's
char *ultoa (unsigned long value, char *string, int radix);

void DOS32_Startup (void);		// near pointers on, or Quit
void DOS32_Mark (char *what);		// -timing: STARTUP.TXT
void DOS32_WriteMarks (void);
long DOS32_MemoryFree (void);	// bytes the DPMI host can still give us

#endif
