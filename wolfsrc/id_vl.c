// ID_VL.C
//
// DJGPP version: the same unchained 320x200 VGA mode ("mode X" with four
// pages of video memory) as the original, programmed through the same
// registers. Video memory is reached through the near pointer (VGAMEM),
// the ID_VL_A.ASM routines are C here.
//

#include "id_heads.h"
#pragma hdrstop

//
// SC_INDEX is expected to stay at SC_MAPMASK for proper operation
//

unsigned	bufferofs;
unsigned	displayofs,pelpan;

unsigned	screenseg=SCREENSEG;		// set to 0xa000 for asm convenience

unsigned	linewidth;
unsigned	ylookup[MAXSCANLINES];

boolean		screenfaded;
unsigned	bordercolor;

boolean		fastpalette;				// if true, use outsb to set

byte		palette1[256][3],palette2[256][3];

//===========================================================================


/*
=======================
=
= VL_VideoID
=
= 5 for a VGA (or MCGA with VGA BIOS), else 0. The VGA BIOS answers
= INT 10h AX=1A00h (read display combination code) with AL=1Ah, and a
= VGA's code is 7 (monochrome) or 8 (color).
=
=======================
*/

int VL_VideoID (void)
{
	__dpmi_regs	r;

	memset (&r,0,sizeof(r));
	r.x.ax = 0x1a00;
	__dpmi_int (0x10,&r);
	if (r.h.al == 0x1a && (r.h.bl == 7 || r.h.bl == 8))
		return 5;
	return 0;
}


/*
=======================
=
= VL_Startup	// WOLFENSTEIN HACK
=
=======================
*/

static	char *ParmStrings[] = {"HIDDENCARD",""};

void	VL_Startup (void)
{
	int i,videocard;

	videocard = VL_VideoID ();
	for (i = 1;i < _argc;i++)
		if (US_CheckParm(_argv[i],ParmStrings) == 0)
		{
			videocard = 5;
			break;
		}

	if (videocard != 5)
Quit ("Improper video card!  If you really have a VGA card that I am not \n"
	  "detecting, use the -HIDDENCARD command line parameter!");

}



/*
=======================
=
= VL_Shutdown
=
=======================
*/

void	VL_Shutdown (void)
{
	VL_SetTextMode ();
}


/*
=======================
=
= VL_SetVGAPlaneMode
=
=======================
*/

void	VL_SetVGAPlaneMode (void)
{
	__dpmi_regs	r;

	memset (&r,0,sizeof(r));
	r.x.ax = 0x13;
	__dpmi_int (0x10,&r);
	VL_DePlaneVGA ();
	VGAMAPMASK(15);
	VL_SetLineWidth (40);
}


/*
=======================
=
= VL_SetTextMode
=
=======================
*/

void	VL_SetTextMode (void)
{
	__dpmi_regs	r;

	memset (&r,0,sizeof(r));
	r.x.ax = 3;
	__dpmi_int (0x10,&r);
}

//===========================================================================

/*
=================
=
= VL_ClearVideo
=
= Fill the entire video buffer with a given color
=
=================
*/

void VL_ClearVideo (byte color)
{
	outportb (GC_INDEX,GC_MODE);
	outportb (GC_INDEX+1,inportb(GC_INDEX+1)&0xfc);	// write mode 0 to store directly to video

	VGAMAPMASK(15);						// write through all four planes

	memset (VGAMEM,color,0x10000);		// clearing 4 video bytes/byte
}


/*
=============================================================================

			VGA REGISTER MANAGEMENT ROUTINES

=============================================================================
*/


/*
=================
=
= VL_DePlaneVGA
=
=================
*/

void VL_DePlaneVGA (void)
{

//
// change CPU addressing to non linear mode
//

//
// turn off chain 4 and odd/even
//
	outportb (SC_INDEX,SC_MEMMODE);
	outportb (SC_INDEX+1,(inportb(SC_INDEX+1)&~8)|4);

	outportb (SC_INDEX,SC_MAPMASK);		// leave this set throughought

//
// turn off odd/even and set write mode 0
//
	outportb (GC_INDEX,GC_MODE);
	outportb (GC_INDEX+1,inportb(GC_INDEX+1)&~0x13);

//
// turn off chain
//
	outportb (GC_INDEX,GC_MISCELLANEOUS);
	outportb (GC_INDEX+1,inportb(GC_INDEX+1)&~2);

//
// clear the entire buffer space, because int 10h only did 16 k / plane
//
	VL_ClearVideo (0);

//
// change CRTC scanning from doubleword to byte mode, allowing >64k scans
//
	outportb (CRTC_INDEX,CRTC_UNDERLINE);
	outportb (CRTC_INDEX+1,inportb(CRTC_INDEX+1)&~0x40);

	outportb (CRTC_INDEX,CRTC_MODE);
	outportb (CRTC_INDEX+1,inportb(CRTC_INDEX+1)|0x40);
}

//===========================================================================

/*
====================
=
= VL_SetLineWidth
=
= Line witdh is in WORDS, 40 words is normal width for vgaplanegr
=
====================
*/

void VL_SetLineWidth (unsigned width)
{
	int i,offset;

//
// set wide virtual screen
//
	outportw (CRTC_INDEX,CRTC_OFFSET+width*256);

//
// set up lookup tables
//
	linewidth = width*2;

	offset = 0;

	for (i=0;i<MAXSCANLINES;i++)
	{
		ylookup[i]=offset;
		offset += linewidth;
	}
}

/*
====================
=
= VL_SetSplitScreen
=
====================
*/

void VL_SetSplitScreen (int linenum)
{
	VL_WaitVBL (1);
	linenum=linenum*2-1;
	outportb (CRTC_INDEX,CRTC_LINECOMPARE);
	outportb (CRTC_INDEX+1,linenum % 256);
	outportb (CRTC_INDEX,CRTC_OVERFLOW);
	outportb (CRTC_INDEX+1, 1+16*(linenum/256));
	outportb (CRTC_INDEX,CRTC_MAXSCANLINE);
	outportb (CRTC_INDEX+1,inportb(CRTC_INDEX+1) & (255-64));
}


/*
=============================================================================

						RETRACE / CRTC (was ID_VL_A.ASM)

=============================================================================
*/

/*
====================
=
= VL_WaitVBL
=
= Waits for the start of the next vertical retrace, vbls times
=
====================
*/

void VL_WaitVBL (int vbls)
{
	long	n;

	while (vbls-- > 0)
	{
		for (n=0 ; n<200000L && (inportb (STATUS_REGISTER_1) & 8) ; n++)
			;
		for (n=0 ; n<200000L && !(inportb (STATUS_REGISTER_1) & 8) ; n++)
			;
	}
}


/*
====================
=
= VL_SetCRTC
=
= Sets the display start address, during the display (the CRTC latches it
= at the start of the retrace)
=
====================
*/

void VL_SetCRTC (int crtc)
{
	DISABLE();

	while (inportb (STATUS_REGISTER_1) & 1)	// 1 = display is disabled (HBL / VBL)
		;

	outportb (CRTC_INDEX,CRTC_STARTHIGH);
	outportb (CRTC_INDEX+1,(crtc>>8)&0xff);
	outportb (CRTC_INDEX,CRTC_STARTLOW);
	outportb (CRTC_INDEX+1,crtc&0xff);

	RESTORE();
}


/*
====================
=
= VL_SetScreen
=
= Sets the display start address and the pel panning so both take effect
= on the same frame: the CRTC latches the start address when the vertical
= retrace begins, so it's written during the display, and the panning is
= written once the retrace has started.
=
= id's code hunted for a horizontal blank instead, and gave up only when
= TimeCount moved on. That needs several port reads inside one horizontal
= blank, which DOSBox and QEMU manage but a real VGA (slower port reads)
= often doesn't, and before the timer interrupt is installed (the signon
= screen) TimeCount doesn't move: the signon took many seconds on real
= hardware. Every wait here is bounded by a count, not by the timer.
=
====================
*/

#define VGAWAITMAX	200000L			// port reads: well over a frame

void VL_SetScreen (int crtc, int pel)
{
	long	n;

	for (n=0 ; n<VGAWAITMAX && (inportb (STATUS_REGISTER_1) & 8) ; n++)
		;							// out of any retrace in progress

	{
	DISABLE();
	outportb (CRTC_INDEX,CRTC_STARTHIGH);
	outportb (CRTC_INDEX+1,(crtc>>8)&0xff);
	outportb (CRTC_INDEX,CRTC_STARTLOW);
	outportb (CRTC_INDEX+1,crtc&0xff);
	RESTORE();
	}

	for (n=0 ; n<VGAWAITMAX && !(inportb (STATUS_REGISTER_1) & 8) ; n++)
		;							// the new address is in use from here

	inportb (STATUS_REGISTER_1);	// attribute controller to its index
	outportb (ATR_INDEX,ATR_PELPAN | 0x20);
	outportb (ATR_INDEX,pel);
}


/*
=============================================================================

						PALETTE OPS

		To avoid snow, do a WaitVBL BEFORE calling these

=============================================================================
*/


/*
=================
=
= VL_FillPalette
=
=================
*/

void VL_FillPalette (int red, int green, int blue)
{
	int	i;

	outportb (PEL_WRITE_ADR,0);
	for (i=0;i<256;i++)
	{
		outportb (PEL_DATA,red);
		outportb (PEL_DATA,green);
		outportb (PEL_DATA,blue);
	}
}

//===========================================================================

/*
=================
=
= VL_SetColor
=
=================
*/

void VL_SetColor	(int color, int red, int green, int blue)
{
	outportb (PEL_WRITE_ADR,color);
	outportb (PEL_DATA,red);
	outportb (PEL_DATA,green);
	outportb (PEL_DATA,blue);
}

//===========================================================================

/*
=================
=
= VL_GetColor
=
=================
*/

void VL_GetColor	(int color, int *red, int *green, int *blue)
{
	outportb (PEL_READ_ADR,color);
	*red = inportb (PEL_DATA);
	*green = inportb (PEL_DATA);
	*blue = inportb (PEL_DATA);
}

//===========================================================================

/*
=================
=
= VL_SetPalette
=
= If fast palette setting has been tested for, it is used
= (some cards don't like outsb palette setting)
=
=================
*/

void VL_SetPalette (byte *palette)
{
	int	i;

	outportb (PEL_WRITE_ADR,0);

	if (fastpalette)
	{
		outportsb (PEL_DATA,palette,768);
		return;
	}

	for (i=0;i<768;i++)
		outportb (PEL_DATA,*palette++);
}


//===========================================================================

/*
=================
=
= VL_GetPalette
=
= This does not use the port string instructions,
= due to some incompatabilities
=
=================
*/

void VL_GetPalette (byte *palette)
{
	int	i;

	outportb (PEL_READ_ADR,0);
	for (i=0;i<768;i++)
		*palette++ = inportb(PEL_DATA);
}


//===========================================================================

/*
=================
=
= VL_FadeOut
=
= Fades the current palette to the given color in the given number of steps
=
=================
*/

void VL_FadeOut (int start, int end, int red, int green, int blue, int steps)
{
	int		i,j,orig,delta;
	byte	*origptr, *newptr;

	VL_WaitVBL(1);
	VL_GetPalette (&palette1[0][0]);
	memcpy (palette2,palette1,768);

//
// fade through intermediate frames
//
	for (i=0;i<steps;i++)
	{
		origptr = &palette1[start][0];
		newptr = &palette2[start][0];
		for (j=start;j<=end;j++)
		{
			orig = *origptr++;
			delta = red-orig;
			*newptr++ = orig + delta * i / steps;
			orig = *origptr++;
			delta = green-orig;
			*newptr++ = orig + delta * i / steps;
			orig = *origptr++;
			delta = blue-orig;
			*newptr++ = orig + delta * i / steps;
		}

		VL_WaitVBL(1);
		VL_SetPalette (&palette2[0][0]);
	}

//
// final color
//
	VL_FillPalette (red,green,blue);

	screenfaded = true;
}


/*
=================
=
= VL_FadeIn
=
=================
*/

void VL_FadeIn (int start, int end, byte *palette, int steps)
{
	int		i,j,delta;

	VL_WaitVBL(1);
	VL_GetPalette (&palette1[0][0]);
	memcpy (&palette2[0][0],&palette1[0][0],sizeof(palette1));

	start *= 3;
	end = end*3+2;

//
// fade through intermediate frames
//
	for (i=0;i<steps;i++)
	{
		for (j=start;j<=end;j++)
		{
			delta = palette[j]-((byte *)palette1)[j];
			((byte *)palette2)[j] = ((byte *)palette1)[j] + delta * i / steps;
		}

		VL_WaitVBL(1);
		VL_SetPalette (&palette2[0][0]);
	}

//
// final color
//
	VL_SetPalette (palette);
	screenfaded = false;
}



/*
=================
=
= VL_TestPaletteSet
=
= Sets the palette with outsb, then reads it in and compares
= If it compares ok, fastpalette is set to true.
=
=================
*/

void VL_TestPaletteSet (void)
{
	int	i;

	for (i=0;i<768;i++)
		((byte *)palette1)[i] = i;

	fastpalette = true;
	VL_SetPalette (&palette1[0][0]);
	VL_GetPalette (&palette2[0][0]);
	if (memcmp (&palette1[0][0],&palette2[0][0],768))
		fastpalette = false;
}


/*
==================
=
= VL_ColorBorder
=
==================
*/

void VL_ColorBorder (int color)
{
	__dpmi_regs	r;

	memset (&r,0,sizeof(r));
	r.x.ax = 0x1001;
	r.h.bh = color;
	__dpmi_int (0x10,&r);
	bordercolor = color;
}



/*
=============================================================================

							PIXEL OPS

=============================================================================
*/

byte	pixmasks[4] = {1,2,4,8};
byte	leftmasks[4] = {15,14,12,8};
byte	rightmasks[4] = {1,3,7,15};


/*
=================
=
= VL_Plot
=
=================
*/

void VL_Plot (int x, int y, int color)
{
	byte mask;

	mask = pixmasks[x&3];
	VGAMAPMASK(mask);
	*VGAPTR(bufferofs+(ylookup[y]+(x>>2))) = color;
	VGAMAPMASK(15);
}


/*
=================
=
= VL_Hlin
=
=================
*/

void VL_Hlin (unsigned x, unsigned y, unsigned width, unsigned color)
{
	unsigned		xbyte;
	byte			*dest;
	byte			leftmask,rightmask;
	int				midbytes;

	xbyte = x>>2;
	leftmask = leftmasks[x&3];
	rightmask = rightmasks[(x+width-1)&3];
	midbytes = ((x+width+3)>>2) - xbyte - 2;

	dest = VGAPTR(bufferofs+ylookup[y]+xbyte);

	if (midbytes<0)
	{
	// all in one byte
		VGAMAPMASK(leftmask&rightmask);
		*dest = color;
		VGAMAPMASK(15);
		return;
	}

	VGAMAPMASK(leftmask);
	*dest++ = color;

	VGAMAPMASK(15);
	memset (dest,color,midbytes);
	dest+=midbytes;

	VGAMAPMASK(rightmask);
	*dest = color;

	VGAMAPMASK(15);
}


/*
=================
=
= VL_Vlin
=
=================
*/

void VL_Vlin (int x, int y, int height, int color)
{
	byte	*dest,mask;

	mask = pixmasks[x&3];
	VGAMAPMASK(mask);

	dest = VGAPTR(bufferofs+ylookup[y]+(x>>2));

	while (height--)
	{
		*dest = color;
		dest += linewidth;
	}

	VGAMAPMASK(15);
}


/*
=================
=
= VL_Bar
=
=================
*/

void VL_Bar (int x, int y, int width, int height, int color)
{
	byte	*dest;
	byte	leftmask,rightmask;
	int		midbytes,linedelta;

	leftmask = leftmasks[x&3];
	rightmask = rightmasks[(x+width-1)&3];
	midbytes = ((x+width+3)>>2) - (x>>2) - 2;
	linedelta = linewidth-(midbytes+1);

	dest = VGAPTR(bufferofs+ylookup[y]+(x>>2));

	if (midbytes<0)
	{
	// all in one byte
		VGAMAPMASK(leftmask&rightmask);
		while (height--)
		{
			*dest = color;
			dest += linewidth;
		}
		VGAMAPMASK(15);
		return;
	}

	while (height--)
	{
		VGAMAPMASK(leftmask);
		*dest++ = color;

		VGAMAPMASK(15);
		memset (dest,color,midbytes);
		dest+=midbytes;

		VGAMAPMASK(rightmask);
		*dest = color;

		dest+=linedelta;
	}

	VGAMAPMASK(15);
}

/*
============================================================================

							MEMORY OPS

============================================================================
*/

/*
=================
=
= VL_MemToLatch
=
=================
*/

void VL_MemToLatch (byte *source, int width, int height, unsigned dest)
{
	unsigned	count;
	byte	plane,mask;

	count = ((width+3)/4)*height;
	mask = 1;
	for (plane = 0; plane<4 ; plane++)
	{
		VGAMAPMASK(mask);
		mask <<= 1;

		memcpy (VGAPTR(dest),source,count);

		source+= count;
	}
}


//===========================================================================


/*
=================
=
= VL_MemToScreen
=
= Draws a block of data to the screen.
=
=================
*/

void VL_MemToScreen (byte *source, int width, int height, int x, int y)
{
	byte    *screen,*dest,mask;
	int		plane;

	width>>=2;
	dest = VGAPTR(bufferofs+ylookup[y]+(x>>2) );
	mask = 1 << (x&3);

	for (plane = 0; plane<4; plane++)
	{
		VGAMAPMASK(mask);
		mask <<= 1;
		if (mask == 16)
			mask = 1;

		screen = dest;
		for (y=0;y<height;y++,screen+=linewidth,source+=width)
			memcpy (screen,source,width);
	}
}

//==========================================================================


/*
=================
=
= VL_MaskedToScreen
=
= Masks a block of main memory to the screen.
=
=================
*/

void VL_MaskedToScreen (byte *source, int width, int height, int x, int y)
{
	byte    *screen,*dest,mask;
	int		plane;

	width>>=2;
	dest = VGAPTR(bufferofs+ylookup[y]+(x>>2) );
	mask = 1 << (x&3);

	for (plane = 0; plane<4; plane++)
	{
		VGAMAPMASK(mask);
		mask <<= 1;
		if (mask == 16)
			mask = 1;

		screen = dest;
		for (y=0;y<height;y++,screen+=linewidth,source+=width)
			memcpy (screen,source,width);
	}
}

//==========================================================================

/*
=================
=
= VL_LatchCopy
=
= Copies video memory to video memory in write mode 1: every byte read
= loads the four latches, every byte written stores them. Has to go a
= byte at a time (read, write, read, write...), so no memcpy.
=
=================
*/

static void VL_LatchCopy (volatile byte *src, volatile byte *dest, int count)
{
	while (count--)
		*dest++ = *src++;
}


/*
=================
=
= VL_LatchToScreen
=
=================
*/

void VL_LatchToScreen (unsigned source, int width, int height, int x, int y)
{
	byte	*src,*dest;

	VGAWRITEMODE(1);
	VGAMAPMASK(15);

	src = VGAPTR(source);
	dest = VGAPTR(bufferofs+ylookup[y]+(x>>2));

	while (height--)
	{
		VL_LatchCopy (src,dest,width);
		src += width;
		dest += linewidth;
	}

	VGAWRITEMODE(0);
}


//===========================================================================

/*
=================
=
= VL_ScreenToScreen
=
=================
*/

void VL_ScreenToScreen (unsigned source, unsigned dest,int width, int height)
{
	byte	*src,*dst;

	VGAMAPMASK(15);
	VGAWRITEMODE(1);

	src = VGAPTR(source);
	dst = VGAPTR(dest);

	while (height--)
	{
		VL_LatchCopy (src,dst,width);
		src += linewidth;
		dst += linewidth;
	}

	VGAWRITEMODE(0);
}


/*
=============================================================================

						STRING OUTPUT ROUTINES

=============================================================================
*/




/*
===================
=
= VL_DrawTile8String
=
===================
*/

void VL_DrawTile8String (char *str, char *tile8ptr, int printx, int printy)
{
	int		i,plane;
	word	*dest,*screen,*src;

	dest = (word *)VGAPTR(bufferofs+ylookup[printy]+(printx>>2));

	while (*str)
	{
		src = (word *)(tile8ptr + (*str<<6));
		// each character is 64 bytes

		for (plane=0;plane<4;plane++)
		{
			VGAMAPMASK(1<<plane);
			screen = dest;
			for (i=0;i<8;i++,screen+=linewidth/2)
				*screen = *src++;
		}

		str++;
		printx += 8;
		dest++;
	}
}



/*
===================
=
= VL_DrawLatch8String
=
===================
*/

void VL_DrawLatch8String (char *str, unsigned tile8ptr, int printx, int printy)
{
	int		i;
	unsigned	src,dest;
	byte	*s,*d;

	dest = bufferofs+ylookup[printy]+(printx>>2);

	VGAWRITEMODE(1);
	VGAMAPMASK(15);

	while (*str)
	{
		src = tile8ptr + (*str<<4);		// each character is 16 latch bytes

		s = VGAPTR(src);
		d = VGAPTR(dest);
		for (i=0;i<8;i++,s+=2,d+=linewidth)
			VL_LatchCopy (s,d,2);

		str++;
		printx += 8;
		dest+=2;
	}

	VGAWRITEMODE(0);
}


/*
===================
=
= VL_SizeTile8String
=
===================
*/

void VL_SizeTile8String (char *str, int *width, int *height)
{
	*height = 8;
	*width = 8*strlen(str);
}
