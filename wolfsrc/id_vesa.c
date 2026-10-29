// ID_VESA.C -- the widescreen display: a VESA mode of about 16:9
//
// The game draws its pages into main memory (id_vl.c's linear mode); this
// shows one: scrwidth x 200 pixels, which are 1.2 times taller than wide
// as on a 4:3 screen, scaled to the largest rectangle of that shape in the
// mode, black around it. 8 bit modes use the VGA DAC for the palette as
// usual; 15, 16, 24 and 32 bit modes go through a lookup table, and a
// palette change shows the page again. Linear framebuffer, or the banked
// window on cards without one.

#include "id_heads.h"
#include <sys/movedata.h>
#include <math.h>
void VESA_Write (unsigned long off, byte *src, unsigned long n);
#pragma hdrstop

static	boolean	vesaon;
static	int		fbw,fbh,fbpitch,fbbytes;
static	boolean	fbindexed,fblinear;
static	byte	rpos,rlen,gpos,glen,bpos,blen;
static	int		lfbsel = -1;
static	__dpmi_meminfo lfbmap;
static	unsigned long wingran,winsize,winseg;
static	int		curbank = -1;
static	int		dstx,dsty,dstw,dsth;
static	int		srcw;
static	unsigned short *xmap;
static	byte	*ymap;
static	byte	*line;
static	unsigned long lut[256];
static	byte	pal[768];
static	int		modenum;
static	int		wantbits;		// -vesabits: only modes of that depth

char	vesamessage[160];

static unsigned rd16 (byte *p)
{
	return p[0] | (p[1]<<8);
}

static unsigned long rd32 (byte *p)
{
	return p[0] | (p[1]<<8) | ((unsigned long)p[2]<<16) | ((unsigned long)p[3]<<24);
}

static boolean ModeInfo (int mode, byte *mi)
{
	__dpmi_regs r;

	memset (&r,0,sizeof(r));
	r.x.ax = 0x4f01;
	r.x.cx = mode;
	r.x.es = __tb >> 4;
	r.x.di = __tb & 15;
	__dpmi_int (0x10,&r);
	if (r.x.ax != 0x004f)
		return false;
	dosmemget (__tb,256,mi);
	return true;
}


/*
===================
=
= VESA_Start
=
= Finds and sets a mode about 16:9 (or wantw x wanth); returns the page
= width to draw (a multiple of 16), or 0 if there's no such mode
=
===================
*/

int VESA_Start (int wantw, int wanth)
{
	byte	info[512],mi[256];
	unsigned short modes[256];
	__dpmi_regs r;
	unsigned long listlin;
	int		ver,nm,i,mw,mh,bpp,model,best,attr,width;
	double	aspect,score,bestscore,mhshown,sc;
	boolean	lfb;

	for (i=1;i<_argc-1;i++)
		if (!stricmp (_argv[i],"-vesabits"))
			wantbits = atoi (_argv[i+1]);

	memset (info,0,sizeof(info));
	memcpy (info,"VBE2",4);
	dosmemput (info,sizeof(info),__tb);
	memset (&r,0,sizeof(r));
	r.x.ax = 0x4f00;
	r.x.es = __tb >> 4;
	r.x.di = __tb & 15;
	__dpmi_int (0x10,&r);
	if (r.x.ax != 0x004f)
	{
		strcpy (vesamessage,"No VESA BIOS: no widescreen.");
		return 0;
	}
	dosmemget (__tb,sizeof(info),info);
	if (memcmp (info,"VESA",4))
	{
		strcpy (vesamessage,"No VESA BIOS: no widescreen.");
		return 0;
	}
	ver = rd16 (info+4);
	listlin = ((rd32 (info+14) >> 16) << 4) + (rd32 (info+14) & 0xffff);
	for (nm=0;nm<255;nm++)
	{
		modes[nm] = _farpeekw (_dos_ds,listlin+nm*2);
		if (modes[nm] == 0xffff)
			break;
	}

	//
	// the widest about 16:9 mode, the fewest pixels to write, 8 bits first
	//
	best = -1;
	bestscore = -1e9;
	for (i=0;i<nm;i++)
	{
		if (!ModeInfo (modes[i],mi))
			continue;
		attr = rd16 (mi);
		if ((attr & 0x19) != 0x19)
			continue;						// supported, color, graphics
		mw = rd16 (mi+18);
		mh = rd16 (mi+20);
		bpp = mi[25];
		model = mi[27];
		if (bpp == 8 ? model != 4 : model != 6)
			continue;
		if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32)
			continue;
		lfb = (attr & 0x80) && ver >= 0x200 && rd32 (mi+40);
		if (!lfb && (mi[2] & 5) != 5)
			continue;						// window A must be writable

		aspect = (double)mw/mh;
		if (wantw)
		{
			if (mw != wantw || mh != wanth)
				continue;
			score = 10000;
		}
		else
		{
			if (aspect < 1.5 || mh < 350)
				continue;					// not wide, or too coarse
			score = 1000 - fabs (aspect - 16.0/9)*2000;		// 16:9 first
			score -= (double)mw*mh/20000;	// then the smallest
		}
		if (wantbits && bpp != wantbits)
			continue;
		if (bpp == 8)
			score += 50;
		else if (bpp == 16 || bpp == 15)
			score += 20;
		else if (bpp == 32)
			score += 10;
		if (lfb)
			score += 5;
		if (score > bestscore)
		{
			bestscore = score;
			best = modes[i];
		}
	}
	if (best < 0 || !ModeInfo (best,mi))
	{
		if (wantw)
			sprintf (vesamessage,"No %dx%d VESA mode: no widescreen.",wantw,wanth);
		else
			strcpy (vesamessage,"No widescreen VESA mode on this card (16:10 or 16:9): no widescreen.");
		return 0;
	}

	attr = rd16 (mi);
	fbw = rd16 (mi+18);
	fbh = rd16 (mi+20);
	bpp = mi[25];
	fbbytes = (bpp+7)/8;
	fbindexed = bpp == 8;
	fbpitch = rd16 (mi+16);
	if (!fbindexed)
	{
		rlen = mi[31]; rpos = mi[32];
		glen = mi[33]; gpos = mi[34];
		blen = mi[35]; bpos = mi[36];
		if (!rlen || !glen || !blen)		// VBE 1.x leaves the masks out
		{
			if (bpp == 15) { rpos = 10; rlen = 5; gpos = 5; glen = 5; bpos = 0; blen = 5; }
			else if (bpp == 16) { rpos = 11; rlen = 5; gpos = 5; glen = 6; bpos = 0; blen = 5; }
			else { rpos = 16; rlen = 8; gpos = 8; glen = 8; bpos = 0; blen = 8; }
		}
	}

	lfb = (attr & 0x80) && ver >= 0x200 && rd32 (mi+40);
	if (lfb)
	{
		if (ver >= 0x300 && rd16 (mi+50))
			fbpitch = rd16 (mi+50);			// LinBytesPerScanLine
		lfbmap.address = rd32 (mi+40);
		lfbmap.size = ((unsigned long)fbpitch*fbh + 0xfff) & ~0xffful;
		if (__dpmi_physical_address_mapping (&lfbmap))
			lfb = false;
		else
		{
			lfbsel = __dpmi_allocate_ldt_descriptors (1);
			if (lfbsel < 0)
				lfb = false;
			else
			{
				__dpmi_set_segment_base_address (lfbsel,lfbmap.address);
				__dpmi_set_segment_limit (lfbsel,(lfbmap.size-1) | 0xfff);
			}
		}
	}
	if (!lfb)
	{
		if ((mi[2] & 5) != 5)
			return 0;
		wingran = (unsigned long)rd16 (mi+4)*1024;
		winsize = (unsigned long)rd16 (mi+6)*1024;
		winseg = rd16 (mi+8);
		if (!wingran || !winsize || !winseg)
			return 0;
	}
	fblinear = lfb;

	memset (&r,0,sizeof(r));
	r.x.ax = 0x4f02;
	r.x.bx = best | (lfb ? 0x4000 : 0);
	__dpmi_int (0x10,&r);
	if (r.x.ax != 0x004f)
	{
		sprintf (vesamessage,"Couldn't set VESA mode %Xh: no widescreen.",best);
		return 0;
	}
	modenum = best;
	vesaon = true;
	curbank = -1;

	//
	// the page: 200 rows shown as 240, as wide as the mode's shape allows
	//
	mhshown = fbh;
	width = (int)(240.0*fbw/mhshown/16 + 0.5)*16;
	if (width < 320)
		width = 320;
	if (width > 512)
		width = 512;
	srcw = width;

	sc = (double)fbw/width < (double)fbh/240 ? (double)fbw/width : (double)fbh/240;
	dstw = (int)(width*sc);
	dsth = (int)(240*sc);
	if (dstw > fbw) dstw = fbw;
	if (dsth > fbh) dsth = fbh;
	dstx = (fbw-dstw)/2;
	dsty = (fbh-dsth)/2;

	xmap = malloc (dstw*sizeof(*xmap));
	ymap = malloc (dsth);
	line = malloc (dstw*fbbytes+4);
	for (i=0;i<dstw;i++)
		xmap[i] = (long)i*width/dstw;
	for (i=0;i<dsth;i++)
		ymap[i] = (long)i*200/dsth;

	sprintf (vesamessage,"Widescreen: VESA mode %Xh, %dx%d, %d bits, %s; a %dx200 view",
		best,fbw,fbh,bpp,lfb ? "linear framebuffer" : "banked",width);

	// black bars
	memset (line,0,dstw*fbbytes);
	{
		byte	*zero = calloc (fbpitch,1);
		for (i=0;i<fbh;i++)
		{
			extern void VESA_Write (unsigned long off, byte *src, unsigned long n);
			VESA_Write ((unsigned long)i*fbpitch,zero,fbpitch);
		}
		free (zero);
	}
	return width;
}


void VESA_Stop (void)
{
	if (!vesaon)
		return;
	vesaon = false;
	if (lfbsel >= 0)
	{
		__dpmi_free_ldt_descriptor (lfbsel);
		lfbsel = -1;
	}
	if (fblinear)
		__dpmi_free_physical_address_mapping (&lfbmap);
}


//
// VESA_Write: bytes to the screen at byte offset off
//
void VESA_Write (unsigned long off, byte *src, unsigned long n)
{
	unsigned long bank,in,k;
	__dpmi_regs r;

	if (fblinear)
	{
		movedata (_my_ds(),(unsigned)src,lfbsel,off,n);
		return;
	}
	while (n)
	{
		bank = off / wingran;
		in = off % wingran;
		k = winsize - in;
		if (k > n)
			k = n;
		if (bank != curbank)
		{
			memset (&r,0,sizeof(r));
			r.x.ax = 0x4f05;
			r.x.bx = 0;
			r.x.dx = bank;
			__dpmi_int (0x10,&r);
			curbank = bank;
		}
		dosmemput (src,k,winseg*16+in);
		src += k;
		off += k;
		n -= k;
	}
}


/*
===================
=
= VESA_SetPalette
=
= Direct color modes: the 6 bit VGA palette as the mode's pixel values;
= true if the page has to be shown again
=
===================
*/

boolean VESA_SetPalette (byte *palette, int first, int count)
{
	int		i,c,r,g,b;

	memcpy (pal+first*3,palette,count*3);
	if (fbindexed)
		return false;
	for (i=first;i<first+count;i++)
	{
		r = pal[i*3]*255/63;
		g = pal[i*3+1]*255/63;
		b = pal[i*3+2]*255/63;
		lut[i] = ((unsigned long)(r >> (8-rlen)) << rpos)
			| ((unsigned long)(g >> (8-glen)) << gpos)
			| ((unsigned long)(b >> (8-blen)) << bpos);
	}
	return true;
}

void VESA_GetPalette (byte *palette)
{
	memcpy (palette,pal,768);
}

boolean VESA_Indexed (void)
{
	return fbindexed;
}


/*
===================
=
= VESA_Present
=
= Shows srcw x 200 pixels from page, scaled
=
===================
*/

void VESA_Present (byte *page)
{
	int		y,lasty,x;
	byte	*src;

	if (!vesaon)
		return;

	lasty = -1;
	for (y=0;y<dsth;y++)
	{
		if (ymap[y] != lasty)
		{
			lasty = ymap[y];
			src = page + lasty*srcw;
			switch (fbbytes)
			{
			case 1:
				for (x=0;x<dstw;x++)
					line[x] = src[xmap[x]];
				break;
			case 2:
				for (x=0;x<dstw;x++)
					((unsigned short *)line)[x] = lut[src[xmap[x]]];
				break;
			case 3:
				for (x=0;x<dstw;x++)
				{
					unsigned long c = lut[src[xmap[x]]];
					line[x*3] = c;
					line[x*3+1] = c >> 8;
					line[x*3+2] = c >> 16;
				}
				break;
			default:
				for (x=0;x<dstw;x++)
					((unsigned long *)line)[x] = lut[src[xmap[x]]];
				break;
			}
		}
		VESA_Write ((unsigned long)(dsty+y)*fbpitch + (unsigned long)dstx*fbbytes,line,dstw*fbbytes);
	}
}
