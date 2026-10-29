// WL_MAP.C -- an automap, like Doom's: Tab cycles map / map over the
// view / off, + and - zoom. It shows the walls next to floor you have
// seen (the raycaster marks it in spotvis), doors, the elevator, and the
// players as arrows. North is up and the map follows you.

#include "wl_def.h"
#pragma hdrstop

int		automapmode;			// 0 off, 1 map, 2 map over the 3D view

static	byte	seen[MAPSIZE][MAPSIZE];
static	int		mapscale = 6;	// pixels per tile
static	int		ox,oy;			// screen position of the view's centre
static	long	cx,cy;			// the map point there (global coordinates)

#define WALLCOLOR		12		// light red
#define DOORCOLOR		14		// yellow
#define ELEVCOLOR		11		// light cyan
#define PLAYERCOLOR		15		// white
#define BACKCOLOR		0

static int othercolors[MAXPLAYERS] = {10,12,14,13};

void AM_Clear (void)
{
	memset (seen,0,sizeof(seen));
}

/*
===================
=
= AM_Mark
=
= After the walls are drawn: remember the floor that was in view
=
===================
*/

void AM_Mark (void)
{
	byte	*s = &spotvis[0][0],*d = &seen[0][0];
	int		i;

	for (i=0;i<MAPSIZE*MAPSIZE;i++)
		d[i] |= s[i];
}

/*
===================
=
= AM_Keys
=
= Tab, + and -; returns true if it used the key
=
===================
*/

boolean AM_Keys (int scan)
{
	switch (scan)
	{
	case sc_Tab:
		automapmode = (automapmode+1)%3;
		return true;
	case 0x0d:					// = / +
	case 0x4e:					// keypad +
		if (automapmode && mapscale < 16)
			mapscale += 2;
		return automapmode != 0;
	case 0x0c:					// -
	case 0x4a:					// keypad -
		if (automapmode && mapscale > 2)
			mapscale -= 2;
		return automapmode != 0;
	}
	return false;
}


//
// drawing, in the view's coordinates (bufferofs points at its corner)
//

static int SX (long gx)
{
	return ox + (int)(((gx - cx) * mapscale) >> TILESHIFT);
}

static int SY (long gy)
{
	return oy + (int)(((gy - cy) * mapscale) >> TILESHIFT);
}

static void HLine (int x1, int x2, int y, int color)
{
	if (y < 0 || y >= viewheight)
		return;
	if (x1 < 0) x1 = 0;
	if (x2 >= viewwidth) x2 = viewwidth-1;
	if (x1 > x2)
		return;
	VL_Hlin (x1,y,x2-x1+1,color);
}

static void VLine (int y1, int y2, int x, int color)
{
	if (x < 0 || x >= viewwidth)
		return;
	if (y1 < 0) y1 = 0;
	if (y2 >= viewheight) y2 = viewheight-1;
	if (y1 > y2)
		return;
	VL_Vlin (x,y1,y2-y1+1,color);
}

static void Line (int x1, int y1, int x2, int y2, int color)
{
	int	dx,dy,sx,sy,err,e2;

	dx = abs (x2-x1);
	dy = -abs (y2-y1);
	sx = x1 < x2 ? 1 : -1;
	sy = y1 < y2 ? 1 : -1;
	err = dx+dy;
	for (;;)
	{
		if (x1 >= 0 && x1 < viewwidth && y1 >= 0 && y1 < viewheight)
			VL_Plot (x1,y1,color);
		if (x1 == x2 && y1 == y2)
			break;
		e2 = 2*err;
		if (e2 >= dy) { err += dy; x1 += sx; }
		if (e2 <= dx) { err += dx; y1 += sy; }
	}
}

static boolean Floor (int x, int y)
{
	if (x < 0 || y < 0 || x >= MAPSIZE || y >= MAPSIZE)
		return false;
	return seen[x][y] && (!tilemap[x][y] || (tilemap[x][y] & 0x80));
}

static int WallColor (int x, int y)
{
	int t;

	if (x < 0 || y < 0 || x >= MAPSIZE || y >= MAPSIZE)
		return 0;
	t = tilemap[x][y];
	if (!t || (t & 0x80))
		return 0;					// floor or door
	if ((t & 63) == ELEVATORTILE)
		return ELEVCOLOR;
	return WALLCOLOR;
}

static void Arrow (objtype *ob, int color)
{
	int		x,y,len,i;
	int		px[3],py[3];
	static int angles[3] = {0,140,220};

	x = SX (ob->x);
	y = SY (ob->y);
	len = mapscale < 4 ? 4 : mapscale;
	for (i=0;i<3;i++)
	{
		int a = ob->angle + angles[i];
		int l = i ? len*2/3 : len;
		if (a >= ANGLES)
			a -= ANGLES;
		px[i] = x + (int)(FixedByFrac ((long)l<<8,costable[a]) >> 8);
		py[i] = y - (int)(FixedByFrac ((long)l<<8,sintable[a]) >> 8);
	}
	Line (px[0],py[0],px[1],py[1],color);
	Line (px[0],py[0],px[2],py[2],color);
	Line (px[1],py[1],x,y,color);
	Line (px[2],py[2],x,y,color);
}


/*
===================
=
= AM_Draw
=
= Called by ThreeDRefresh with bufferofs at the view's corner
=
===================
*/

void AM_Draw (void)
{
	int		x,y,x1,y1,x2,y2,c,i;
	doorobj_t *door;

	if (automapmode == 1)
		VL_Bar (0,0,viewwidth,viewheight,BACKCOLOR);

	ox = viewwidth/2;
	oy = viewheight/2;
	cx = player->x;
	cy = player->y;

	//
	// walls: the sides that face floor you've seen
	//
	for (x=0;x<MAPSIZE;x++)
		for (y=0;y<MAPSIZE;y++)
		{
			if (!Floor (x,y))
				continue;
			x1 = SX ((long)x<<TILESHIFT);
			x2 = SX ((long)(x+1)<<TILESHIFT);
			y1 = SY ((long)y<<TILESHIFT);
			y2 = SY ((long)(y+1)<<TILESHIFT);
			if (x2 < 0 || x1 >= viewwidth || y2 < 0 || y1 >= viewheight)
				continue;
			if ((c = WallColor (x,y-1)) != 0)
				HLine (x1,x2,y1,c);
			if ((c = WallColor (x,y+1)) != 0)
				HLine (x1,x2,y2,c);
			if ((c = WallColor (x-1,y)) != 0)
				VLine (y1,y2,x1,c);
			if ((c = WallColor (x+1,y)) != 0)
				VLine (y1,y2,x2,c);
		}

	//
	// doors next to seen floor, across the middle of their tile
	//
	for (door = &doorobjlist[0] ; door != lastdoorobj ; door++)
	{
		x = door->tilex;
		y = door->tiley;
		if (!Floor (x-1,y) && !Floor (x+1,y) && !Floor (x,y-1) && !Floor (x,y+1))
			continue;
		if (door->vertical)
		{
			x1 = SX (((long)x<<TILESHIFT) + TILEGLOBAL/2);
			VLine (SY ((long)y<<TILESHIFT),SY ((long)(y+1)<<TILESHIFT),x1,DOORCOLOR);
		}
		else
		{
			y1 = SY (((long)y<<TILESHIFT) + TILEGLOBAL/2);
			HLine (SX ((long)x<<TILESHIFT),SX ((long)(x+1)<<TILESHIFT),y1,DOORCOLOR);
		}
	}

	//
	// the players
	//
	if (netgame)
	{
		for (i=0;i<numplayers;i++)
			if (playerobjs[i] != player && NetPlayerAlive (i))
				Arrow (playerobjs[i],othercolors[i]);
	}
	Arrow (player,PLAYERCOLOR);
}
