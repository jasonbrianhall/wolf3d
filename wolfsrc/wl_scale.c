// WL_SCALE.C

#include "wl_def.h"
#pragma hdrstop

#define OP_RETF	0xcb

/*
=============================================================================

						  GLOBALS

=============================================================================
*/

t_compscale *scaledirectory[MAXSCALEHEIGHT+1];

int			maxscale,maxscaleshl2;

boolean	insetupscaling;

/*
=============================================================================

						  LOCALS

=============================================================================
*/

void BuildCompScale (int height, memptr *finalspot);

int			stepbytwo;

//===========================================================================

/*
==============
=
= BadScale
=
==============
*/

void BadScale (void)
{
	Quit ("BadScale called!");
}


/*
==========================
=
= SetupScaling
=
==========================
*/

void SetupScaling (int maxscaleheight)
{
	int		i,x,y;
	byte	*dest;

	insetupscaling = true;

	maxscaleheight/=2;			// one scaler every two pixels

	maxscale = maxscaleheight-1;
	maxscaleshl2 = maxscale<<2;

//
// free up old scalers
//
	for (i=1;i<MAXSCALEHEIGHT;i++)
	{
		if (scaledirectory[i])
			MM_FreePtr ((memptr *)&scaledirectory[i]);
		if (i>=stepbytwo)
			i += 2;
	}
	memset (scaledirectory,0,sizeof(scaledirectory));

	MM_SortMem ();

//
// build the scalers
//
	stepbytwo = viewheight/2;	// save space by double stepping

	for (i=1;i<=maxscaleheight;i++)
	{
		BuildCompScale (i*2,(memptr *)&scaledirectory[i]);
		if (i>=stepbytwo)
			i+= 2;
	}

//
// share the scalers that were skipped
//
	for (i=1;i<=maxscaleheight;i++)
	{
		if (i>=stepbytwo)
		{
			scaledirectory[i+1] = scaledirectory[i];
			scaledirectory[i+2] = scaledirectory[i];
			i+=2;
		}
	}
	scaledirectory[0] = scaledirectory[1];

//
// oversize wall drawing finds no scaler (ScalePost quits, as BadScale did)
//
	insetupscaling = false;
}

//===========================================================================

/*
========================
=
= BuildCompScale
=
= Builds a scaler that will scale a 64 tall object to the given height
= (centered vertically on the screen)
=
= height should be even
=
= The compiled version was code: for each source pixel that showed, a load
= of the pixel, then a store for each screen row it covered (from the top
= of the view, clipped to it). This is the same as a table of rows.
=
========================
*/

void BuildCompScale (int height, memptr *finalspot)
{
	t_compscale	*work;
	int			src;
	long		fix,step;
	int			startpix,endpix,toppix;

	MM_GetPtr (finalspot,sizeof(t_compscale));
	work = (t_compscale *)*finalspot;

	step = ((long)height<<16) / 64;
	toppix = (viewheight-height)/2;
	fix = 0;

	for (src=0;src<=64;src++)
	{
		startpix = fix>>16;
		fix += step;
		endpix = fix>>16;

		if (endpix>startpix)
			work->width[src] = endpix-startpix;
		else
			work->width[src] = 0;

		startpix+=toppix;
		endpix+=toppix;

		if (startpix == endpix || endpix < 0 || startpix >= viewheight || src == 64)
		{
			work->startrow[src] = work->endrow[src] = 0;	// no pixels drawn
			continue;
		}

		if (startpix < 0)
			startpix = 0;					// not into the view area
		if (endpix > viewheight)
			endpix = viewheight;			// off the bottom of the view area

		work->startrow[src] = startpix;
		work->endrow[src] = endpix;
	}
}


/*
=======================
=
= ScaleLine
=
= Draws slinewidth columns from slinex on, of the shape column linecmds
= (an offset in the shape) points to, with the scaler comptable
=
= linecmds is a list of posts, three words each:
=	end of segment pixel*2 (0 terminates the list)
=	top of virtual line with segment in proper place (the offset in the
=	shape that pixel 0 of the column would be at)
=	start of segment pixel*2
=
=======================
*/

int			slinex,slinewidth;
word		linecmds;
static	t_compscale	*linescale;
static	byte		*lineshape;

static void ScaleLine (void)
{
	int		x,lastx,y,end,src,startsrc,endsrc;
	word	*cmd,top;
	byte	*dest,*screen,mask,color;

	x = slinex;
	lastx = slinex+slinewidth-1;
	while (x <= lastx)
	{
		mask = 0;
		do
		{
			mask |= 1<<(x&3);
			x++;
		} while (x <= lastx && (x&3));

		VGAMAPMASK(mask);
		dest = VGAPTR(bufferofs+((x-1)>>2));

		cmd = (word *)(lineshape+linecmds);
		while ((endsrc = cmd[0]/2) != 0)
		{
			top = cmd[1];
			startsrc = cmd[2]/2;
			cmd += 3;

			for (src=startsrc;src<endsrc;src++)
			{
				y = linescale->startrow[src];
				end = linescale->endrow[src];
				if (y >= end)
					continue;
				color = lineshape[(word)(top+src)];
				screen = dest + y*SCREENBWIDE;
				for (;y<end;y++,screen+=SCREENBWIDE)
					*screen = color;
			}
		}
	}
}


/*
=======================
=
= ScaleShape
=
= Draws a compiled shape at [scale] pixels high
=
= each vertical line of the shape has a pointer to segment data:
= 	end of segment pixel*2 (0 terminates line) used to patch rtl in scaler
= 	top of virtual line with segment in proper place
=	start of segment pixel*2, used to jsl into compiled scaler
=	<repeat>
=
= Setup for call
= --------------
= GC_MODE			read mode 1, write mode 2
= GC_COLORDONTCARE  set to 0, so all reads from video memory return 0xff
= GC_INDEX			pointing at GC_BITMASK
=
=======================
*/

static	long		longtemp;

void ScaleShape (int xcenter, int shapenum, unsigned height)
{
	t_compshape	*shape;
	t_compscale *comptable;
	unsigned	scale,srcx,stopx,tempx;
	int			t;
	word		*cmdptr;
	boolean		leftvis,rightvis;


	shape = PM_GetSpritePage (shapenum);

	scale = height>>3;						// low three bits are fractional
	if (!scale || scale>maxscale)
		return;								// too close or far away
	comptable = scaledirectory[scale];

	linescale = comptable;
	lineshape = (byte *)shape;

//
// scale to the left (from pixel 31 to shape->leftpix)
//
	srcx = 32;
	slinex = xcenter;
	stopx = shape->leftpix;
	cmdptr = &shape->dataofs[31-stopx];

	while ( --srcx >=stopx && slinex>0)
	{
		linecmds = *cmdptr--;
		if ( !(slinewidth = comptable->width[srcx]) )
			continue;

		if (slinewidth == 1)
		{
			slinex--;
			if (slinex<viewwidth)
			{
				if (wallheight[slinex] >= height)
					continue;		// obscured by closer wall
				ScaleLine ();
			}
			continue;
		}

		//
		// handle multi pixel lines
		//
		if (slinex>viewwidth)
		{
			slinex -= slinewidth;
			slinewidth = viewwidth-slinex;
			if (slinewidth<1)
				continue;		// still off the right side
		}
		else
		{
			if (slinewidth>slinex)
				slinewidth = slinex;
			slinex -= slinewidth;
		}


		leftvis = (wallheight[slinex] < height);
		rightvis = (wallheight[slinex+slinewidth-1] < height);

		if (leftvis)
		{
			if (rightvis)
				ScaleLine ();
			else
			{
				while (wallheight[slinex+slinewidth-1] >= height)
					slinewidth--;
				ScaleLine ();
			}
		}
		else
		{
			if (!rightvis)
				continue;		// totally obscured

			while (wallheight[slinex] >= height)
			{
				slinex++;
				slinewidth--;
			}
			ScaleLine ();
			break;			// the rest of the shape is gone
		}
	}


//
// scale to the right
//
	slinex = xcenter;
	stopx = shape->rightpix;
	if (shape->leftpix<31)
	{
		srcx = 31;
		cmdptr = &shape->dataofs[32-shape->leftpix];
	}
	else
	{
		srcx = shape->leftpix-1;
		cmdptr = &shape->dataofs[0];
	}
	slinewidth = 0;

	while ( ++srcx <= stopx && (slinex+=slinewidth)<viewwidth)
	{
		linecmds = *cmdptr++;
		if ( !(slinewidth = comptable->width[srcx]) )
			continue;

		if (slinewidth == 1)
		{
			if (slinex>=0 && wallheight[slinex] < height)
			{
				ScaleLine ();
			}
			continue;
		}

		//
		// handle multi pixel lines
		//
		if (slinex<0)
		{
			if (slinewidth <= -slinex)
				continue;		// still off the left edge

			slinewidth += slinex;
			slinex = 0;
		}
		else
		{
			if (slinex + slinewidth > viewwidth)
				slinewidth = viewwidth-slinex;
		}


		leftvis = (wallheight[slinex] < height);
		rightvis = (wallheight[slinex+slinewidth-1] < height);

		if (leftvis)
		{
			if (rightvis)
			{
				ScaleLine ();
			}
			else
			{
				while (wallheight[slinex+slinewidth-1] >= height)
					slinewidth--;
				ScaleLine ();
				break;			// the rest of the shape is gone
			}
		}
		else
		{
			if (rightvis)
			{
				while (wallheight[slinex] >= height)
				{
					slinex++;
					slinewidth--;
				}
				ScaleLine ();
			}
			else
				continue;		// totally obscured
		}
	}
}



/*
=======================
=
= SimpleScaleShape
=
= NO CLIPPING, height in pixels
=
= Draws a compiled shape at [scale] pixels high
=
= each vertical line of the shape has a pointer to segment data:
= 	end of segment pixel*2 (0 terminates line) used to patch rtl in scaler
= 	top of virtual line with segment in proper place
=	start of segment pixel*2, used to jsl into compiled scaler
=	<repeat>
=
= Setup for call
= --------------
= GC_MODE			read mode 1, write mode 2
= GC_COLORDONTCARE  set to 0, so all reads from video memory return 0xff
= GC_INDEX			pointing at GC_BITMASK
=
=======================
*/

void SimpleScaleShape (int xcenter, int shapenum, unsigned height)
{
	t_compshape	*shape;
	t_compscale *comptable;
	unsigned	scale,srcx,stopx,tempx;
	int			t;
	word		*cmdptr;
	boolean		leftvis,rightvis;


	shape = PM_GetSpritePage (shapenum);

	scale = height>>1;
	comptable = scaledirectory[scale];
	if (!comptable)
		Quit ("BadScale called!");

	linescale = comptable;
	lineshape = (byte *)shape;

//
// scale to the left (from pixel 31 to shape->leftpix)
//
	srcx = 32;
	slinex = xcenter;
	stopx = shape->leftpix;
	cmdptr = &shape->dataofs[31-stopx];

	while ( --srcx >=stopx )
	{
		linecmds = *cmdptr--;
		if ( !(slinewidth = comptable->width[srcx]) )
			continue;

		slinex -= slinewidth;
		ScaleLine ();
	}


//
// scale to the right
//
	slinex = xcenter;
	stopx = shape->rightpix;
	if (shape->leftpix<31)
	{
		srcx = 31;
		cmdptr = &shape->dataofs[32-shape->leftpix];
	}
	else
	{
		srcx = shape->leftpix-1;
		cmdptr = &shape->dataofs[0];
	}
	slinewidth = 0;

	while ( ++srcx <= stopx )
	{
		linecmds = *cmdptr++;
		if ( !(slinewidth = comptable->width[srcx]) )
			continue;

		ScaleLine ();
		slinex+=slinewidth;
	}
}




//
// bit mask tables for drawing scaled strips up to eight pixels wide
//
// down here so the STUPID inline assembler doesn't get confused!
//


byte	mapmasks1[4][8] = {
{1 ,3 ,7 ,15,15,15,15,15},
{2 ,6 ,14,14,14,14,14,14},
{4 ,12,12,12,12,12,12,12},
{8 ,8 ,8 ,8 ,8 ,8 ,8 ,8} };

byte	mapmasks2[4][8] = {
{0 ,0 ,0 ,0 ,1 ,3 ,7 ,15},
{0 ,0 ,0 ,1 ,3 ,7 ,15,15},
{0 ,0 ,1 ,3 ,7 ,15,15,15},
{0 ,1 ,3 ,7 ,15,15,15,15} };

byte	mapmasks3[4][8] = {
{0 ,0 ,0 ,0 ,0 ,0 ,0 ,0},
{0 ,0 ,0 ,0 ,0 ,0 ,0 ,1},
{0 ,0 ,0 ,0 ,0 ,0 ,1 ,3},
{0 ,0 ,0 ,0 ,0 ,1 ,3 ,7} };


unsigned	wordmasks[8][8] = {
{0x0080,0x00c0,0x00e0,0x00f0,0x00f8,0x00fc,0x00fe,0x00ff},
{0x0040,0x0060,0x0070,0x0078,0x007c,0x007e,0x007f,0x807f},
{0x0020,0x0030,0x0038,0x003c,0x003e,0x003f,0x803f,0xc03f},
{0x0010,0x0018,0x001c,0x001e,0x001f,0x801f,0xc01f,0xe01f},
{0x0008,0x000c,0x000e,0x000f,0x800f,0xc00f,0xe00f,0xf00f},
{0x0004,0x0006,0x0007,0x8007,0xc007,0xe007,0xf007,0xf807},
{0x0002,0x0003,0x8003,0xc003,0xe003,0xf003,0xf803,0xfc03},
{0x0001,0x8001,0xc001,0xe001,0xf001,0xf801,0xfc01,0xfe01} };

