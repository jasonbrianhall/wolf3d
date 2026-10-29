// WL_DRAW.C

#include "wl_def.h"
#include <dos.h>
#pragma hdrstop

//#define DEBUGWALLS
//#define DEBUGTICS

/*
=============================================================================

						 LOCAL CONSTANTS

=============================================================================
*/

// the door is the last picture before the sprites
#define DOORWALL	(PMSpriteStart-8)

#define ACTORSIZE	0x4000

/*
=============================================================================

						 GLOBAL VARIABLES

=============================================================================
*/


#ifdef DEBUGWALLS
unsigned screenloc[3]= {0,0,0};
#else
unsigned screenloc[3]= {PAGE1START,PAGE2START,PAGE3START};
#endif
unsigned freelatch = FREESTART;

long 	lasttimecount;
long 	frameon;

unsigned	wallheight[MAXVIEWWIDTH];

fixed	tileglobal	= TILEGLOBAL;
fixed	mindist		= MINDIST;


//
// math tables
//
int			pixelangle[MAXVIEWWIDTH];
long		finetangent[FINEANGLES/4];
fixed 		sintable[ANGLES+ANGLES/4],*costable = sintable+(ANGLES/4);

//
// refresh variables
//
fixed	viewx,viewy;			// the focal point
int		viewangle;
fixed	viewsin,viewcos;



fixed	FixedByFrac (fixed a, fixed b);
void	TransformActor (objtype *ob);
void	BuildTables (void);
void	ClearScreen (void);
int		CalcRotate (objtype *ob);
void	DrawScaleds (void);
void	CalcTics (void);
void	FixOfs (void);
void	ThreeDRefresh (void);



//
// wall optimization variables
//
int		lastside;		// true for vertical
long	lastintercept;
int		lasttilehit;


//
// ray tracing variables
//
int			focaltx,focalty,viewtx,viewty;

int			midangle,angle;
word		xpartial,ypartial;			// 16 bits, as in the asm
word		xpartialup,xpartialdown,ypartialup,ypartialdown;
unsigned	xinttile,yinttile;

unsigned	tilehit;
unsigned	pixx;

int		xtile,ytile;
int		xtilestep,ytilestep;
long	xintercept,yintercept;
long	xstep,ystep;

int		horizwall[MAXWALLTILES],vertwall[MAXWALLTILES];


/*
=============================================================================

						 LOCAL VARIABLES

=============================================================================
*/


void AsmRefresh (void);			// was WL_DR_A.ASM

/*
============================================================================

			   3 - D  DEFINITIONS

============================================================================
*/


//==========================================================================


/*
========================
=
= FixedByFrac
=
= multiply a 16/16 bit, 2's complement fixed point number by a 16 bit
= fraction, passed as a signed magnitude 32 bit number
=
========================
*/

fixed FixedByFrac (fixed a, fixed b)
{
	unsigned long	ua;
	unsigned long	result;
	int				negative;

	negative = (b & 0x80000000ul) != 0;	// sign of result = sign of fraction
	if (a < 0)
	{
		ua = -(unsigned long)a;
		negative ^= 1;					// toggle sign of result
	}
	else
		ua = a;

//
// multiply the unsigned value by the 16 bit fraction, keep 32 bits
//
	result = (unsigned long)(((unsigned long long)ua * (b & 0xffff)) >> 16);

	if (negative)
		result = -result;
	return (fixed)result;
}


//==========================================================================

/*
========================
=
= TransformActor
=
= Takes paramaters:
=   gx,gy		: globalx/globaly of point
=
= globals:
=   viewx,viewy		: point of view
=   viewcos,viewsin	: sin/cos of viewangle
=   scale		: conversion from global value to screen value
=
= sets:
=   screenx,transx,transy,screenheight: projected edge location and size
=
========================
*/


//
// transform actor
//
void TransformActor (objtype *ob)
{
	int ratio;
	fixed gx,gy,gxt,gyt,nx,ny;
	long	temp;

//
// translate point to view centered coordinates
//
	gx = ob->x-viewx;
	gy = ob->y-viewy;

//
// calculate newx
//
	gxt = FixedByFrac(gx,viewcos);
	gyt = FixedByFrac(gy,viewsin);
	nx = gxt-gyt-ACTORSIZE;		// fudge the shape forward a bit, because
								// the midpoint could put parts of the shape
								// into an adjacent wall

//
// calculate newy
//
	gxt = FixedByFrac(gx,viewsin);
	gyt = FixedByFrac(gy,viewcos);
	ny = gyt+gxt;

//
// calculate perspective ratio
//
	ob->transx = nx;
	ob->transy = ny;

	if (nx<mindist)			// too close, don't overflow the divide
	{
	  ob->viewheight = 0;
	  return;
	}

	ob->viewx = centerx + ny*scale/nx;	// DEBUG: use assembly divide

//
// calculate height (heightnumerator/(nx>>8))
//
	ob->viewheight = (word)(heightnumerator/(short)(nx>>8));
}

//==========================================================================

/*
========================
=
= TransformTile
=
= Takes paramaters:
=   tx,ty		: tile the object is centered in
=
= globals:
=   viewx,viewy		: point of view
=   viewcos,viewsin	: sin/cos of viewangle
=   scale		: conversion from global value to screen value
=
= sets:
=   screenx,transx,transy,screenheight: projected edge location and size
=
= Returns true if the tile is withing getting distance
=
========================
*/

boolean TransformTile (int tx, int ty, int *dispx, int *dispheight)
{
	int ratio;
	fixed gx,gy,gxt,gyt,nx,ny;
	long	temp;

//
// translate point to view centered coordinates
//
	gx = ((long)tx<<TILESHIFT)+0x8000-viewx;
	gy = ((long)ty<<TILESHIFT)+0x8000-viewy;

//
// calculate newx
//
	gxt = FixedByFrac(gx,viewcos);
	gyt = FixedByFrac(gy,viewsin);
	nx = gxt-gyt-0x2000;		// 0x2000 is size of object

//
// calculate newy
//
	gxt = FixedByFrac(gx,viewsin);
	gyt = FixedByFrac(gy,viewcos);
	ny = gyt+gxt;


//
// calculate perspective ratio
//
	if (nx<mindist)			// too close, don't overflow the divide
	{
		*dispheight = 0;
		return false;
	}

	*dispx = centerx + ny*scale/nx;	// DEBUG: use assembly divide

//
// calculate height (heightnumerator/(nx>>8))
//
	*dispheight = (short)(heightnumerator/(short)(nx>>8));

//
// see if it should be grabbed
//
	if (nx<TILEGLOBAL && ny>-TILEGLOBAL/2 && ny<TILEGLOBAL/2)
		return true;
	else
		return false;
}

//==========================================================================

/*
====================
=
= CalcHeight
=
= Calculates the height of xintercept,yintercept from viewx,viewy
=
====================
*/

int	CalcHeight (void)
{
	int	transheight;
	int ratio;
	fixed gxt,gyt,nx,ny;
	long	gx,gy;

	gx = xintercept-viewx;
	gxt = FixedByFrac(gx,viewcos);

	gy = yintercept-viewy;
	gyt = FixedByFrac(gy,viewsin);

	nx = gxt-gyt;

  //
  // calculate perspective ratio (heightnumerator/(nx>>8))
  //
	if (nx<mindist)
		nx=mindist;			// don't let divide overflow

	return (short)(heightnumerator/(short)(nx>>8));
}


//==========================================================================

/*
===================
=
= ScalePost
=
===================
*/

byte		*postsource;
unsigned	posttexture;
unsigned	postx;
unsigned	postwidth;

//
// ScalePost draws postwidth columns of wall from postx on, all the same
// texture column (posttexture in the page postsource), at the height in
// wallheight[postx]. A VGA byte holds four columns (one per plane), so it's
// drawn once for each byte the post touches, with those columns' planes
// enabled.
//
void ScalePost (void)		// VGA version
{
	int			height,x,lastx,y,src,end;
	byte		*source,*dest,*screen,mask,color;
	t_compscale	*comptable;

	height = (wallheight[postx] & 0xfff8)>>3;
	if (height > maxscale)
		height = maxscale;
	comptable = scaledirectory[height];
	if (!comptable)
		Quit ("BadScale called!");

	source = postsource + posttexture;

	x = postx;
	lastx = postx+postwidth-1;
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

		for (src=0;src<64;src++)
		{
			y = comptable->startrow[src];
			end = comptable->endrow[src];
			if (y >= end)
				continue;
			color = source[src];
			screen = dest + y*SCREENBWIDE;
			for (;y<end;y++,screen+=SCREENBWIDE)
				*screen = color;
		}
	}
}

void  FarScalePost (void)				// just so other files can call
{
	ScalePost ();
}


/*
====================
=
= HitVertWall
=
= tilehit bit 7 is 0, because it's not a door tile
= if bit 6 is 1 and the adjacent tile is a door tile, use door side pic
=
====================
*/

void HitVertWall (void)
{
	int			wallpic;
	unsigned	texture;

	texture = (yintercept>>4)&0xfc0;
	if (xtilestep == -1)
	{
		texture = 0xfc0-texture;
		xintercept += TILEGLOBAL;
	}
	wallheight[pixx] = CalcHeight();

	if (lastside==1 && lastintercept == xtile && lasttilehit == tilehit)
	{
		// in the same wall type as last time, so check for optimized draw
		if (texture == posttexture)
		{
		// wide scale
			postwidth++;
			wallheight[pixx] = wallheight[pixx-1];
			return;
		}
		else
		{
			ScalePost ();
			posttexture = texture;
			postwidth = 1;
			postx = pixx;
		}
	}
	else
	{
	// new wall
		if (lastside != -1)				// if not the first scaled post
			ScalePost ();

		lastside = true;
		lastintercept = xtile;

		lasttilehit = tilehit;
		postx = pixx;
		postwidth = 1;

		if (tilehit & 0x40)
		{								// check for adjacent doors
			ytile = yintercept>>TILESHIFT;
			if ( tilemap[xtile-xtilestep][ytile]&0x80 )
				wallpic = DOORWALL+3;
			else
				wallpic = vertwall[tilehit & ~0x40];
		}
		else
			wallpic = vertwall[tilehit];

		postsource = PM_GetPage(wallpic);
		posttexture = texture;

	}
}


/*
====================
=
= HitHorizWall
=
= tilehit bit 7 is 0, because it's not a door tile
= if bit 6 is 1 and the adjacent tile is a door tile, use door side pic
=
====================
*/

void HitHorizWall (void)
{
	int			wallpic;
	unsigned	texture;

	texture = (xintercept>>4)&0xfc0;
	if (ytilestep == -1)
		yintercept += TILEGLOBAL;
	else
		texture = 0xfc0-texture;
	wallheight[pixx] = CalcHeight();

	if (lastside==0 && lastintercept == ytile && lasttilehit == tilehit)
	{
		// in the same wall type as last time, so check for optimized draw
		if (texture == posttexture)
		{
		// wide scale
			postwidth++;
			wallheight[pixx] = wallheight[pixx-1];
			return;
		}
		else
		{
			ScalePost ();
			posttexture = texture;
			postwidth = 1;
			postx = pixx;
		}
	}
	else
	{
	// new wall
		if (lastside != -1)				// if not the first scaled post
			ScalePost ();

		lastside = 0;
		lastintercept = ytile;

		lasttilehit = tilehit;
		postx = pixx;
		postwidth = 1;

		if (tilehit & 0x40)
		{								// check for adjacent doors
			xtile = xintercept>>TILESHIFT;
			if ( tilemap[xtile][ytile-ytilestep]&0x80 )
				wallpic = DOORWALL+2;
			else
				wallpic = horizwall[tilehit & ~0x40];
		}
		else
			wallpic = horizwall[tilehit];

		postsource = PM_GetPage(wallpic);
		posttexture = texture;
	}

}

//==========================================================================

/*
====================
=
= HitHorizDoor
=
====================
*/

void HitHorizDoor (void)
{
	unsigned	texture,doorpage,doornum;

	doornum = tilehit&0x7f;
	texture = ( (xintercept-doorposition[doornum]) >> 4) &0xfc0;

	wallheight[pixx] = CalcHeight();

	if (lasttilehit == tilehit)
	{
	// in the same door as last time, so check for optimized draw
		if (texture == posttexture)
		{
		// wide scale
			postwidth++;
			wallheight[pixx] = wallheight[pixx-1];
			return;
		}
		else
		{
			ScalePost ();
			posttexture = texture;
			postwidth = 1;
			postx = pixx;
		}
	}
	else
	{
		if (lastside != -1)				// if not the first scaled post
			ScalePost ();			// draw last post
	// first pixel in this door
		lastside = 2;
		lasttilehit = tilehit;
		postx = pixx;
		postwidth = 1;

		switch (doorobjlist[doornum].lock)
		{
		case dr_normal:
			doorpage = DOORWALL;
			break;
		case dr_lock1:
		case dr_lock2:
		case dr_lock3:
		case dr_lock4:
			doorpage = DOORWALL+6;
			break;
		case dr_elevator:
			doorpage = DOORWALL+4;
			break;
		}

		postsource = PM_GetPage(doorpage);
		posttexture = texture;
	}
}

//==========================================================================

/*
====================
=
= HitVertDoor
=
====================
*/

void HitVertDoor (void)
{
	unsigned	texture,doorpage,doornum;

	doornum = tilehit&0x7f;
	texture = ( (yintercept-doorposition[doornum]) >> 4) &0xfc0;

	wallheight[pixx] = CalcHeight();

	if (lasttilehit == tilehit)
	{
	// in the same door as last time, so check for optimized draw
		if (texture == posttexture)
		{
		// wide scale
			postwidth++;
			wallheight[pixx] = wallheight[pixx-1];
			return;
		}
		else
		{
			ScalePost ();
			posttexture = texture;
			postwidth = 1;
			postx = pixx;
		}
	}
	else
	{
		if (lastside != -1)				// if not the first scaled post
			ScalePost ();			// draw last post
	// first pixel in this door
		lastside = 2;
		lasttilehit = tilehit;
		postx = pixx;
		postwidth = 1;

		switch (doorobjlist[doornum].lock)
		{
		case dr_normal:
			doorpage = DOORWALL;
			break;
		case dr_lock1:
		case dr_lock2:
		case dr_lock3:
		case dr_lock4:
			doorpage = DOORWALL+6;
			break;
		case dr_elevator:
			doorpage = DOORWALL+4;
			break;
		}

		postsource = PM_GetPage(doorpage+1);
		posttexture = texture;
	}
}

//==========================================================================


/*
====================
=
= HitHorizPWall
=
= A pushable wall in action has been hit
=
====================
*/

void HitHorizPWall (void)
{
	int			wallpic;
	unsigned	texture,offset;

	texture = (xintercept>>4)&0xfc0;
	offset = pwallpos<<10;
	if (ytilestep == -1)
		yintercept += TILEGLOBAL-offset;
	else
	{
		texture = 0xfc0-texture;
		yintercept += offset;
	}

	wallheight[pixx] = CalcHeight();

	if (lasttilehit == tilehit)
	{
		// in the same wall type as last time, so check for optimized draw
		if (texture == posttexture)
		{
		// wide scale
			postwidth++;
			wallheight[pixx] = wallheight[pixx-1];
			return;
		}
		else
		{
			ScalePost ();
			posttexture = texture;
			postwidth = 1;
			postx = pixx;
		}
	}
	else
	{
	// new wall
		if (lastside != -1)				// if not the first scaled post
			ScalePost ();

		lasttilehit = tilehit;
		postx = pixx;
		postwidth = 1;

		wallpic = horizwall[tilehit&63];

		postsource = PM_GetPage(wallpic);
		posttexture = texture;
	}

}


/*
====================
=
= HitVertPWall
=
= A pushable wall in action has been hit
=
====================
*/

void HitVertPWall (void)
{
	int			wallpic;
	unsigned	texture,offset;

	texture = (yintercept>>4)&0xfc0;
	offset = pwallpos<<10;
	if (xtilestep == -1)
	{
		xintercept += TILEGLOBAL-offset;
		texture = 0xfc0-texture;
	}
	else
		xintercept += offset;

	wallheight[pixx] = CalcHeight();

	if (lasttilehit == tilehit)
	{
		// in the same wall type as last time, so check for optimized draw
		if (texture == posttexture)
		{
		// wide scale
			postwidth++;
			wallheight[pixx] = wallheight[pixx-1];
			return;
		}
		else
		{
			ScalePost ();
			posttexture = texture;
			postwidth = 1;
			postx = pixx;
		}
	}
	else
	{
	// new wall
		if (lastside != -1)				// if not the first scaled post
			ScalePost ();

		lasttilehit = tilehit;
		postx = pixx;
		postwidth = 1;

		wallpic = vertwall[tilehit&63];

		postsource = PM_GetPage(wallpic);
		posttexture = texture;
	}

}

//==========================================================================

//==========================================================================

#if 0
/*
=====================
=
= ClearScreen
=
=====================
*/

void ClearScreen (void)
{
 unsigned floor=egaFloor[gamestate.episode*10+mapon],
	  ceiling=egaCeiling[gamestate.episode*10+mapon];

  //
  // clear the screen
  //
asm	mov	dx,GC_INDEX
asm	mov	ax,GC_MODE + 256*2		// read mode 0, write mode 2
asm	out	dx,ax
asm	mov	ax,GC_BITMASK + 255*256
asm	out	dx,ax

asm	mov	dx,40
asm	mov	ax,[viewwidth]
asm	shr	ax,3
asm	sub	dx,ax					// dx = 40-viewwidth/8

asm	mov	bx,[viewwidth]
asm	shr	bx,4					// bl = viewwidth/16
asm	mov	bh,BYTE PTR [viewheight]
asm	shr	bh,1					// half height

asm	mov	ax,[ceiling]
asm	mov	es,[screenseg]
asm	mov	di,[bufferofs]

toploop:
asm	mov	cl,bl
asm	rep	stosw
asm	add	di,dx
asm	dec	bh
asm	jnz	toploop

asm	mov	bh,BYTE PTR [viewheight]
asm	shr	bh,1					// half height
asm	mov	ax,[floor]

bottomloop:
asm	mov	cl,bl
asm	rep	stosw
asm	add	di,dx
asm	dec	bh
asm	jnz	bottomloop


asm	mov	dx,GC_INDEX
asm	mov	ax,GC_MODE + 256*10		// read mode 1, write mode 2
asm	out	dx,ax
asm	mov	al,GC_BITMASK
asm	out	dx,al

}
#endif
//==========================================================================

unsigned vgaCeiling[]=
{
#ifndef SPEAR
 0x1d1d,0x1d1d,0x1d1d,0x1d1d,0x1d1d,0x1d1d,0x1d1d,0x1d1d,0x1d1d,0xbfbf,
 0x4e4e,0x4e4e,0x4e4e,0x1d1d,0x8d8d,0x4e4e,0x1d1d,0x2d2d,0x1d1d,0x8d8d,
 0x1d1d,0x1d1d,0x1d1d,0x1d1d,0x1d1d,0x2d2d,0xdddd,0x1d1d,0x1d1d,0x9898,

 0x1d1d,0x9d9d,0x2d2d,0xdddd,0xdddd,0x9d9d,0x2d2d,0x4d4d,0x1d1d,0xdddd,
 0x7d7d,0x1d1d,0x2d2d,0x2d2d,0xdddd,0xd7d7,0x1d1d,0x1d1d,0x1d1d,0x2d2d,
 0x1d1d,0x1d1d,0x1d1d,0x1d1d,0xdddd,0xdddd,0x7d7d,0xdddd,0xdddd,0xdddd
#else
 0x6f6f,0x4f4f,0x1d1d,0xdede,0xdfdf,0x2e2e,0x7f7f,0x9e9e,0xaeae,0x7f7f,
 0x1d1d,0xdede,0xdfdf,0xdede,0xdfdf,0xdede,0xe1e1,0xdcdc,0x2e2e,0x1d1d,0xdcdc
#endif
};

/*
=====================
=
= VGAClearScreen
=
=====================
*/

void VGAClearScreen (void)
{
 unsigned ceiling=vgaCeiling[gamestate.episode*10+mapon];
 int	y,halfheight;
 byte	*dest;

  //
  // clear the screen
  //
	VGAMAPMASK(15);						// write through all planes

	halfheight = viewheight/2;
	dest = VGAPTR(bufferofs);

	for (y=0;y<halfheight;y++,dest+=SCREENBWIDE)
		memset (dest,ceiling&0xff,viewwidth/4);
	for (y=0;y<halfheight;y++,dest+=SCREENBWIDE)
		memset (dest,0x19,viewwidth/4);
}

//==========================================================================

/*
=====================
=
= CalcRotate
=
=====================
*/

int	CalcRotate (objtype *ob)
{
	int	angle,viewangle;

	// this isn't exactly correct, as it should vary by a trig value,
	// but it is close enough with only eight rotations

	viewangle = player->angle + (centerx - ob->viewx)/8;

	if (ob->obclass == rocketobj || ob->obclass == hrocketobj)
		angle =  (viewangle-180)- ob->angle;
	else
		angle =  (viewangle-180)- dirangle[ob->dir];

	angle+=ANGLES/16;
	while (angle>=ANGLES)
		angle-=ANGLES;
	while (angle<0)
		angle+=ANGLES;

	if (ob->state->rotate == 2)             // 2 rotation pain frame
		return 4*(angle/(ANGLES/2));        // seperated by 3 (art layout...)

	return angle/(ANGLES/8);
}


/*
=====================
=
= DrawScaleds
=
= Draws all objects that are visable
=
=====================
*/

#define MAXVISABLE	50

typedef struct
{
	int	viewx,
		viewheight,
		shapenum;
} visobj_t;

visobj_t	vislist[MAXVISABLE],*visptr,*visstep,*farthest;

void DrawScaleds (void)
{
	int 		i,j,least,numvisable,height;
	memptr		shape;
	byte		*tilespot,*visspot;
	int			shapenum;
	unsigned	spotloc;

	statobj_t	*statptr;
	objtype		*obj;

	visptr = &vislist[0];

//
// place static objects
//
	for (statptr = &statobjlist[0] ; statptr !=laststatobj ; statptr++)
	{
		if ((visptr->shapenum = statptr->shapenum) == -1)
			continue;						// object has been deleted

		if (!*statptr->visspot)
			continue;						// not visable

		if (TransformTile (statptr->tilex,statptr->tiley
			,&visptr->viewx,&visptr->viewheight) && statptr->flags & FL_BONUS)
		{
			if (!netgame)		// netgame: in the game logic (NetPickups)
			{
				GetBonus (statptr);
				continue;
			}
		}

		if (!visptr->viewheight)
			continue;						// to close to the object

		if (visptr < &vislist[MAXVISABLE-1])	// don't let it overflow
			visptr++;
	}

//
// place active objects
//
	for (obj = netgame ? playerobjs[0] : player->next;obj;obj=obj->next)
	{
		if (netgame && obj->obclass == playerobj)
		{
			if (obj == player)
				continue;
			visptr->shapenum = 1;			// picked after TransformActor
		}
		else if (!(visptr->shapenum = obj->state->shapenum))
			continue;						// no shape

		spotloc = (obj->tilex<<6)+obj->tiley;	// optimize: keep in struct?
		visspot = &spotvis[0][0]+spotloc;
		tilespot = &tilemap[0][0]+spotloc;

		//
		// could be in any of the nine surrounding tiles
		//
		if (*visspot
		|| ( *(visspot-1) && !*(tilespot-1) )
		|| ( *(visspot+1) && !*(tilespot+1) )
		|| ( *(visspot-65) && !*(tilespot-65) )
		|| ( *(visspot-64) && !*(tilespot-64) )
		|| ( *(visspot-63) && !*(tilespot-63) )
		|| ( *(visspot+65) && !*(tilespot+65) )
		|| ( *(visspot+64) && !*(tilespot+64) )
		|| ( *(visspot+63) && !*(tilespot+63) ) )
		{
			if (!netgame)
				obj->active = true;
			TransformActor (obj);
			if (!obj->viewheight)
				continue;						// too close or far away

			visptr->viewx = obj->viewx;
			visptr->viewheight = obj->viewheight;
			if (netgame && obj->obclass == playerobj)
			{
				if (!(visptr->shapenum = NetPlayerShape (obj)))
					continue;
			}
			else
			{
				if (visptr->shapenum == -1)
					visptr->shapenum = obj->temp1;	// special shape

				if (obj->state->rotate)
					visptr->shapenum += CalcRotate (obj);
			}

			if (visptr < &vislist[MAXVISABLE-1])	// don't let it overflow
				visptr++;
			obj->flags |= FL_VISABLE;
		}
		else
			obj->flags &= ~FL_VISABLE;
	}

//
// draw from back to front
//
	numvisable = visptr-&vislist[0];

	if (!numvisable)
		return;									// no visable objects

	for (i = 0; i<numvisable; i++)
	{
		least = 32000;
		for (visstep=&vislist[0] ; visstep<visptr ; visstep++)
		{
			height = visstep->viewheight;
			if (height < least)
			{
				least = height;
				farthest = visstep;
			}
		}
		//
		// draw farthest
		//
		ScaleShape(farthest->viewx,farthest->shapenum,farthest->viewheight);

		farthest->viewheight = 32000;
	}

}

//==========================================================================

/*
==============
=
= DrawPlayerWeapon
=
= Draw the player's hands
=
==============
*/

int	weaponscale[NUMWEAPONS] = {SPR_KNIFEREADY,SPR_PISTOLREADY
	,SPR_MACHINEGUNREADY,SPR_CHAINREADY};

void DrawPlayerWeapon (void)
{
	int	shapenum;

	if (NetLocalDead ())
		return;

#ifndef SPEAR
	if (gamestate.victoryflag)
	{
		if (player->state == &s_deathcam && (TimeCount&32) )
			SimpleScaleShape(viewwidth/2,SPR_DEATHCAM,viewheight+1);
		return;
	}
#endif

	if (gamestate.weapon != -1)
	{
		shapenum = weaponscale[gamestate.weapon]+gamestate.weaponframe;
		SimpleScaleShape(viewwidth/2,shapenum,viewheight+1);
	}

	if (demorecord || demoplayback)
		SimpleScaleShape(viewwidth/2,SPR_DEMO,viewheight+1);
}


//==========================================================================


/*
=====================
=
= CalcTics
=
=====================
*/

void CalcTics (void)
{
	long	newtime,oldtimecount;

//
// calculate tics since last refresh for adaptive timing
//
	if (lasttimecount > TimeCount)
		TimeCount = lasttimecount;		// if the game was paused a LONG time

	do
	{
		newtime = TimeCount;
		tics = newtime-lasttimecount;
	} while (!tics);			// make sure at least one tic passes

	lasttimecount = newtime;

#ifdef FILEPROFILE
		strcpy (scratch,"\tTics:");
		itoa (tics,str,10);
		strcat (scratch,str);
		strcat (scratch,"\n");
		write (profilehandle,scratch,strlen(scratch));
#endif

	if (tics>MAXTICS)
	{
		TimeCount -= (tics-MAXTICS);
		tics = MAXTICS;
	}
}


//==========================================================================


/*
========================
=
= FixOfs
=
========================
*/

void	FixOfs (void)
{
	VW_ScreenToScreen (displayofs,bufferofs,viewwidth/8,viewheight);
}


//==========================================================================

/*
====================
=
= AsmRefresh (was WL_DR_A.ASM)
=
= Traces a ray for each column of the view, through the tile map, until it
= hits a wall, door or pushwall, then calls the Hit* routine for it, which
= sets up the post. Steps along both axes at once: vertical walls are at
= the x tile edges (checked against yintercept), horizontal walls at the y
= tile edges (checked against xintercept).
=
= Integer behaviour (sign/magnitude multiplies, 16 bit tile coordinates)
= follows the assembly.
=
====================
*/

//
// partialbystep: multiplies long step (possibly negative) by 16 bit
// unsigned partial, as a sign/magnitude 16.16 multiply (truncates
// towards zero)
//
static long partialbystep (long step, word partial)
{
	unsigned long	mag;
	unsigned long	result;

	if (step < 0)
	{
		mag = -(unsigned long)step;
		result = (unsigned long)(((unsigned long long)mag * partial) >> 16);
		return -(long)result;
	}
	mag = step;
	return (long)(((unsigned long long)mag * partial) >> 16);
}

#define DEG90	900
#define DEG180	1800
#define DEG270	2700
#define DEG360	3600

void AsmRefresh (void)
{
	int		angl;
	short	xt,yt;				// live xtile (BX) and ytile (BP)
	short	xinth,yinth;		// high words of xintercept (CX) and yintercept (DX)
	word	xspot,yspot;
	word	doorpos;
	byte	hit;
	long	newintercept;

	for (pixx=0;pixx<viewwidth;pixx++)
	{
	//
	// Setup to trace a ray through pixx view pixel
	//
		angl = midangle+pixelangle[pixx];	// delta for this pixel

		if (angl < 0)
			angl += FINEANGLES;				// -90 is the same as 270
		else if (angl >= DEG360)
			angl -= FINEANGLES;				// -449 is the same as 89

		if (angl < DEG90)
		{
		// 0-89 degree arc
			xtilestep = 1;
			ytilestep = -1;
			xstep = finetangent[DEG90-1-angl];
			ystep = -finetangent[angl];
			xpartial = xpartialup;
			ypartial = ypartialdown;
		}
		else if (angl < DEG180)
		{
		// 90-179 degree arc
			xtilestep = -1;
			ytilestep = -1;
			xstep = -finetangent[angl-DEG90];
			ystep = -finetangent[DEG180-1-angl];
			xpartial = xpartialdown;
			ypartial = ypartialdown;
		}
		else if (angl < DEG270)
		{
		// 180-269 degree arc
			xtilestep = -1;
			ytilestep = 1;
			xstep = -finetangent[DEG270-1-angl];
			ystep = finetangent[angl-DEG180];
			xpartial = xpartialdown;
			ypartial = ypartialup;
		}
		else
		{
		// 270-359 degree arc
			xtilestep = 1;
			ytilestep = 1;
			xstep = finetangent[angl-DEG270];
			ystep = finetangent[DEG360-1-angl];
			xpartial = xpartialup;
			ypartial = ypartialup;
		}

	//
	// initialise variables for intersection testing
	//
		yintercept = partialbystep (ystep,xpartial) + viewy;
		yinth = yintercept>>16;
		xt = focaltx+xtilestep;
		xspot = (xt<<6)+yinth;

		xintercept = partialbystep (xstep,ypartial) + viewx;
		xinth = xintercept>>16;
		yt = focalty+ytilestep;
		yspot = (xinth<<6)+yt;

	//
	// trace along this angle until we hit a wall
	//

		//-----------
		//
		// check intersections with vertical walls
		//
		//-----------
vertcheck:
		if (ytilestep == -1 ? yinth <= yt : yinth >= yt)
			goto horizentry;
vertentry:
		hit = ((byte *)tilemap)[xspot];
		if (hit)
			goto hitvert;
passvert:
		((byte *)spotvis)[xspot] = 1;
		xt += xtilestep;
		yintercept += ystep;
		yinth = yintercept>>16;
		xspot = (xt<<6)+yinth;
		goto vertcheck;

hitvert:
		tilehit = hit;
		if (hit & 0x80)
			goto vertdoor;
		xintercept = (long)xt<<16;
		xtile = xt;
		ytile = yinth;
		HitVertWall ();
		continue;

		//-----------
		//
		// check intersections with horizontal walls
		//
		//-----------
horizcheck:
		if (xtilestep == -1 ? xinth <= xt : xinth >= xt)
			goto vertentry;
horizentry:
		hit = ((byte *)tilemap)[yspot];
		if (hit)
			goto hithoriz;
passhoriz:
		((byte *)spotvis)[yspot] = 1;
		yt += ytilestep;
		xintercept += xstep;
		xinth = xintercept>>16;
		yspot = (xinth<<6)+yt;
		goto horizcheck;

hithoriz:
		tilehit = hit;
		if (hit & 0x80)
			goto horizdoor;
		xtile = xinth;
		yintercept = (long)yt<<16;
		ytile = yt;
		HitHorizWall ();
		continue;

		//=============
		//
		// hit a special horizontal wall, so find which coordinate a door would be
		// intersected at, and check to see if the door is open past that point
		//
		//=============
horizdoor:
		xtile = xt;					// save off live register variables

		if (hit & 0x40)
		{
		//
		// hit a sliding horizontal wall
		//
			newintercept = xintercept + ((long)((unsigned long)xstep*pwallpos)>>6);
			if ((short)(newintercept>>16) != xinth)
				goto passhoriz;			// no, it hit the side
		//
		// draw the pushable wall at the new height
		//
			xintercept = newintercept;
			yintercept = (long)yt<<16;
			HitHorizPWall ();
			continue;
		}

		newintercept = xintercept + (xstep>>1);	// half a step gets to door position
		if ((short)(newintercept>>16) != xinth)
			goto passhoriz;				// midpoint is outside tile, so it hit the side of the wall before a door
		doorpos = newintercept & 0xffff;
		if (doorpos < (word)doorposition[hit & 0x7f])
			goto passhoriz;				// door is open that far
	//
	// draw the door
	//
		xintercept = ((long)xinth<<16) | doorpos;
		yintercept = ((long)yt<<16) | 0x8000;	// intercept in middle of tile
		HitHorizDoor ();
		continue;

		//=============
		//
		// hit a special vertical wall, so find which coordinate a door would be
		// intersected at, and check to see if the door is open past that point
		//
		//=============
vertdoor:
		xtile = xt;					// save off live register variables

		if (hit & 0x40)
		{
		//
		// hit a sliding vertical wall
		//
			newintercept = yintercept + ((long)((unsigned long)ystep*pwallpos)>>6);
			if ((short)(newintercept>>16) != yinth)
				goto passvert;			// no, it hit the side
		//
		// draw the pushable wall at the new height
		//
			yintercept = newintercept;
			xintercept = (long)xt<<16;
			HitVertPWall ();
			continue;
		}

		newintercept = yintercept + (ystep>>1);	// half a step gets to door position
		if ((short)(newintercept>>16) != yinth)
			goto passvert;				// midpoint is outside tile, so it hit the side of the wall before a door
		doorpos = newintercept & 0xffff;
		if (doorpos < (word)doorposition[hit & 0x7f])
			goto passvert;				// door is open that far
	//
	// draw the door
	//
		yintercept = ((long)yinth<<16) | doorpos;
		xintercept = ((long)xt<<16) | 0x8000;	// intercept in middle of tile
		HitVertDoor ();
	}
}

//==========================================================================


/*
====================
=
= WallRefresh
=
====================
*/

void WallRefresh (void)
{
//
// set up variables for this view
//
	viewangle = player->angle;
	midangle = viewangle*(FINEANGLES/ANGLES);
	viewsin = sintable[viewangle];
	viewcos = costable[viewangle];
	viewx = player->x - FixedByFrac(focallength,viewcos);
	viewy = player->y + FixedByFrac(focallength,viewsin);

	focaltx = viewx>>TILESHIFT;
	focalty = viewy>>TILESHIFT;

	viewtx = player->x >> TILESHIFT;
	viewty = player->y >> TILESHIFT;

	xpartialdown = viewx&(TILEGLOBAL-1);
	xpartialup = TILEGLOBAL-xpartialdown;
	ypartialdown = viewy&(TILEGLOBAL-1);
	ypartialup = TILEGLOBAL-ypartialdown;

	lastside = -1;			// the first pixel is on a new wall
	AsmRefresh ();
	ScalePost ();			// no more optimization on last post
}

//==========================================================================

/*
========================
=
= ThreeDRefresh
=
========================
*/

void	ThreeDRefresh (void)
{
	int tracedir;

// this wouldn't need to be done except for my debugger/video wierdness
	outportb (SC_INDEX,SC_MAPMASK);

//
// clear out the traced array
//
	memset (spotvis,0,sizeof(spotvis));

	bufferofs += screenofs;

//
// follow the walls from there to the right, drawwing as we go
//
	VGAClearScreen ();

	WallRefresh ();

//
// draw all the scaled images
//
	DrawScaleds();			// draw scaled stuff
	DrawPlayerWeapon ();	// draw player's hands

//
// show screen and time last cycle
//
	if (fizzlein)
	{
		FizzleFade(bufferofs,displayofs+screenofs,viewwidth,viewheight,20,false);
		fizzlein = false;

		lasttimecount = TimeCount = 0;		// don't make a big tic count

	}

	bufferofs -= screenofs;
	displayofs = bufferofs;

	{
	DISABLE();
	outportb (CRTC_INDEX,CRTC_STARTHIGH);	// start address high register
	outportb (CRTC_INDEX+1,displayofs>>8);	// set the high byte
	RESTORE();
	}

	bufferofs += SCREENSIZE;
	if (bufferofs > PAGE3START)
		bufferofs = PAGE1START;

	frameon++;
	PM_NextFrame();
}


//===========================================================================

