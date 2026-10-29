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

boolean		vlinear;
byte		*vmem;
int			scrwidth = 320;
int			vlxoff;
int			vluix;						// vlxoff of the 320 wide screens
unsigned	screenbwide = 80;
unsigned	screensize = 80*208;
static	unsigned	vdisplay;			// the page shown
static	boolean		vdirty;				// drawn on since

int		VESA_Start (int wantw, int wanth);
void	VESA_Stop (void);
void	VESA_Present (byte *page);
boolean	VESA_SetPalette (byte *palette, int first, int count);
void	VESA_GetPalette (byte *palette);
boolean	VESA_Indexed (void);

#define LPIX(ofs)	(vmem + (unsigned long)(ofs)*4)

static byte *UIPixel (int x, int y)
{
	vdirty = true;
	return LPIX(bufferofs+ylookup[y]) + vlxoff + x;
}

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

static	int		widew,wideh;

static boolean VL_WantWidescreen (void)
{
	int		i,want;

	want = false;
	for (i=1;i<_argc;i++)
	{
		if (!stricmp (_argv[i],"-widescreen"))
			want = true;
		if (!stricmp (_argv[i],"-vesa") && i+1 < _argc
		&& sscanf (_argv[i+1],"%dx%d",&widew,&wideh) == 2)
			want = true;
	}
	if (!want)
	{
		FILE *f = fopen ("VIDEO.CFG","r");		// SETUP's choice
		if (f)
		{
			char	line[80];
			while (fgets (line,sizeof(line),f))
			{
				if (!strnicmp (line,"widescreen=1",12))
					want = true;
				if (!strnicmp (line,"vesa=",5))
					sscanf (line+5,"%dx%d",&widew,&wideh);
			}
			fclose (f);
		}
	}
	return want;
}

void	VL_SetVGAPlaneMode (void)
{
	if (!vlinear && VL_WantWidescreen ())
	{
		int	width = VESA_Start (widew,wideh);

		if (width)
		{
			vlinear = true;
			scrwidth = width;
			screenbwide = width/4;
			vluix = vlxoff = (width-320)/2;
			screensize = screenbwide*208;
			vmem = calloc ((screensize*3+0x10000ul)*4,1);
			if (!vmem)
				Quit ("No memory for the widescreen pages");
			VL_SetLineWidth (screenbwide/2);
			return;
		}
	}
	if (vlinear)
		return;

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
	VESA_Stop ();

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
	if (vlinear)
	{
		memset (vmem,color,(screensize*3+0x10000ul)*4);
		vdirty = true;
		return;
	}

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
	if (vlinear)
	{
		linewidth = width*2;
		for (i=0;i<MAXSCANLINES;i++)
			ylookup[i] = i*linewidth;
		return;
	}


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
	if (vlinear)
		return;

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
	if (vlinear)
	{
		VL_PresentDirty ();
		while (vbls-- > 0)		// a VESA card may not have the VGA's retrace bit
		{
			for (n=0 ; n<20000 && (inportb (STATUS_REGISTER_1) & 8) ; n++)
				;
			for (n=0 ; n<20000 && !(inportb (STATUS_REGISTER_1) & 8) ; n++)
				;
		}
		return;
	}


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
	if (vlinear)
	{
		VL_Present (crtc);
		return;
	}

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
	if (vlinear)
	{
		VL_Present (crtc);
		return;
	}

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
	if (vlinear && !VESA_Indexed ())
	{
		byte	p[768];
		for (i=0;i<256;i++)
		{
			p[i*3] = red;
			p[i*3+1] = green;
			p[i*3+2] = blue;
		}
		VL_SetPalette (p);
		return;
	}


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
	if (vlinear && !VESA_Indexed ())
	{
		byte	p[3];
		p[0] = red; p[1] = green; p[2] = blue;
		if (VESA_SetPalette (p,color,1))
			VL_Present (vdisplay);
		return;
	}

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
	if (vlinear && !VESA_Indexed ())
	{
		byte	p[768];
		VESA_GetPalette (p);
		*red = p[color*3];
		*green = p[color*3+1];
		*blue = p[color*3+2];
		return;
	}

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
	if (vlinear && VESA_SetPalette (palette,0,256))
	{
		VL_Present (vdisplay);		// direct color: show it in the new colors
		return;
	}

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
	if (vlinear && !VESA_Indexed ())
	{
		VESA_GetPalette (palette);
		return;
	}

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
	if (vlinear)
	{
		bordercolor = color;
		return;
	}

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
	if (vlinear)
	{
		*UIPixel (x,y) = color;
		return;
	}

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
	if (vlinear)
	{
		memset (UIPixel (x,y),color,width);
		return;
	}

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
	if (vlinear)
	{
		byte *p = UIPixel (x,y);
		while (height-- > 0)
		{
			*p = color;
			p += scrwidth;
		}
		return;
	}

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
	if (vlinear)
	{
		byte *p = UIPixel (x,y);
		if (x == 0 && width >= 320 && vlxoff == vluix)
			VL_ClearMargins (y,height,color);	// a whole screen: its sides too
		while (height-- > 0)
		{
			memset (p,color,width);
			p += scrwidth;
		}
		return;
	}

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
	if (vlinear)
	{
		// stored as rows of width pixels
		int		w4 = (width+3)/4,p,y,k;
		byte	*d = LPIX(dest);
		count = w4*height;
		for (p=0;p<4;p++)
			for (y=0;y<height;y++)
				for (k=0;k<w4;k++)
					if (p+4*k < width)
						d[y*width+p+4*k] = source[p*count+y*w4+k];
		return;
	}


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
	if (vlinear)
	{
		// the pic is munged: plane by plane, a quarter of the pixels each
		int		w4 = width>>2,p,yy,k;
		byte	*d = UIPixel (x,y);
		if (x == 0 && width >= 320 && vlxoff == vluix)
			VL_ClearMargins (y,height,0);
		for (p=0;p<4;p++)
			for (yy=0;yy<height;yy++)
			{
				byte *row = d + yy*scrwidth + p;
				for (k=0;k<w4;k++)
					row[k*4] = *source++;
			}
		return;
	}

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
	if (vlinear)
	{
		// the pic is munged: plane by plane, a quarter of the pixels each
		int		w4 = width>>2,p,yy,k;
		byte	*d = UIPixel (x,y);
		
		for (p=0;p<4;p++)
			for (yy=0;yy<height;yy++)
			{
				byte *row = d + yy*scrwidth + p;
				for (k=0;k<w4;k++)
					row[k*4] = *source++;
			}
		return;
	}

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
	if (vlinear)
	{
		byte	*s = LPIX(source),*d = UIPixel (x,y);
		while (height-- > 0)
		{
			memcpy (d,s,width*4);
			s += width*4;
			d += scrwidth;
		}
		return;
	}

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
	if (vlinear)
	{
		byte	*s,*d;
		if (width == 80)
			width = screenbwide;	// "the whole width"
		s = LPIX(source);
		d = LPIX(dest);
		vdirty = true;
		while (height-- > 0)
		{
			memmove (d,s,width*4);
			s += scrwidth;
			d += scrwidth;
		}
		return;
	}

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
	if (vlinear)
	{
		int		p,row,k;
		byte	*src,*d;
		for ( ; *str ; str++,printx+=8)
		{
			src = (byte *)tile8ptr + (*str<<6);
			d = UIPixel (printx,printy);
			for (p=0;p<4;p++)
				for (row=0;row<8;row++)
					for (k=0;k<2;k++)
						d[row*scrwidth+p+4*k] = *src++;
		}
		return;
	}

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
	if (vlinear)
	{
		byte	*s2,*d2;
		for ( ; *str ; str++,printx+=8)
		{
			s2 = LPIX(tile8ptr + (*str<<4));
			d2 = UIPixel (printx,printy);
			for (i=0;i<8;i++,s2+=8,d2+=scrwidth)
				memcpy (d2,s2,8);
		}
		return;
	}

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


/*
=============================================================================

						WIDESCREEN

=============================================================================
*/

/*
=================
=
= VL_Present
=
= Shows the page at ofs (widescreen: there's no CRTC start address to set)
=
=================
*/

void VL_Present (unsigned ofs)
{
	if (!vlinear)
		return;
	vdisplay = ofs;
	vdirty = false;
	VESA_Present (LPIX(ofs));
}

byte *VL_UIPixel (int x, int y)
{
	return UIPixel (x,y);
}

byte *VL_Linear (unsigned ofs)
{
	vdirty = true;
	return LPIX(ofs);
}

void VL_PresentDirty (void)
{
	if (vlinear && vdirty)
		VL_Present (vdisplay);
}


/*
=================
=
= VL_ClearMargins
=
= The sides of the screen, left and right of the 320 wide screens, on all
= three pages
=
=================
*/

void VL_ClearMargins (int y, int height, int color)
{
	int		page,row;
	byte	*p;

	if (!vlinear || !vluix)
		return;
	if (y < 0)
	{
		height += y;
		y = 0;
	}
	if (y+height > 200)
		height = 200-y;
	vdirty = true;
	for (page=0;page<3;page++)
		for (row=y;row<y+height;row++)
		{
			p = LPIX(page*screensize + ylookup[row]);
			memset (p,color,vluix);
			memset (p+vluix+320,color,scrwidth-vluix-320);
		}
}
