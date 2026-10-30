// WL_NET.C -- IPX co-op for the DJGPP port
//
// Lockstep, like Doom: every machine runs the whole game and only the
// players' controls go over the wire.  The game advances in NETTICS-tic
// frames; the controls a machine samples at frame f are used at frame
// f+NETDELAY on every machine, so normally nobody waits.  Each packet
// carries the last NETBACKUP frames of controls, so a lost packet is
// covered by the next one, and a stalled machine resends its last one.
//
// Everything that differs between players lives in a netplayer_t and is
// swapped into the usual globals (gamestate.health, player, plux...) by
// NetSetContext, so the single player code runs unchanged for each of them.

#include "wl_def.h"
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#pragma hdrstop

/*
=============================================================================

						 GLOBALS

=============================================================================
*/

boolean	netgame;
int		numplayers = 1;
int		localplayer;
int		netcontext;
objtype	*playerobjs[MAXPLAYERS];

extern	long	playerxmove,playerymove;
extern	int		gotgatgun;
extern	objtype	*LastAttacker;
extern	boolean	buttonstate[NUMBUTTONS];
extern	int		mouseadjustment;
extern	int		objcount;
extern	statetype	s_player;
int		kbhit (void);
int		getch (void);

void	PollKeyboardButtons (void);
void	PollMouseButtons (void);
void	PollJoystickButtons (void);
void	PollKeyboardMove (void);
void	PollMouseMove (void);
void	PollJoystickMove (void);
void	ClearSplitVWB (void);
int		US_RndIndex (void);

#define NETTICS		2			// tics per game frame (35 frames a second)
#define NETDELAY	2			// frames between sampling and using controls
#define NETBACKUP	8			// frames of controls in every packet
#define NETRING		64			// must be a power of two, > 2*NETBACKUP
#define RESPAWNTICS	(70*3)

#define NETSCALE	207			// projection of a full size (304 wide) view:
#define NETSHOOT	30			// shootdelta for it
#define NETHALFVIEW	152

#define NETFOCAL	(0x5700l)
#define NETACTORSIZE	0x4000

typedef struct
{
	byte	buttons;
	short	x,y;
} __attribute__((packed)) netcmd_t;

typedef struct
{
	objtype	*ob;

	// the per player part of gamestate
	long	oldscore,score,nextextra;
	int		lives,health,ammo,keys;
	weapontype	bestweapon,weapon,chosenweapon;
	int		faceframe,attackframe,attackcount,weaponframe;

	// wl_agent.c / wl_play.c
	long	thrustspeed;
	unsigned plux,pluy;
	int		anglefrac,gotgatgun,facecount;
	long	playerxmove,playerymove;
	objtype	*lastattacker;
	int		controlx,controly;
	boolean	buttonstate[NUMBUTTONS],buttonheld[NUMBUTTONS];

	// not swapped
	boolean	dead,removed;
	int		respawn;
	long	spawnx,spawny;
	int		spawnangle;
	long	anim;
	int		moving;				// frames left of the walking animation
	int		pain;				// frames left of the pain frame

	boolean	gone;				// left the game: no controls after lastframe
	long	lastframe;

	long	cmdframe[NETRING];
	netcmd_t cmds[NETRING];
	long	ack;				// frames it has finished (this level)
	byte	addr[10];			// IPX network + node
} netplayer_t;

static	netplayer_t	np[MAXPLAYERS];
static	int		hello_ff;			// friendly fire (player 1's choice)
static	boolean	SpawnSpotClear (netplayer_t *p);
static	void	AddChatLine (char *text, int color);

/*
=============================================================================

						 CONTEXT

=============================================================================
*/

static void SaveContext (netplayer_t *p)
{
	p->oldscore = gamestate.oldscore;
	p->score = gamestate.score;
	p->nextextra = gamestate.nextextra;
	p->lives = gamestate.lives;
	p->health = gamestate.health;
	p->ammo = gamestate.ammo;
	p->keys = gamestate.keys;
	p->bestweapon = gamestate.bestweapon;
	p->weapon = gamestate.weapon;
	p->chosenweapon = gamestate.chosenweapon;
	p->faceframe = gamestate.faceframe;
	p->attackframe = gamestate.attackframe;
	p->attackcount = gamestate.attackcount;
	p->weaponframe = gamestate.weaponframe;

	p->thrustspeed = thrustspeed;
	p->plux = plux;
	p->pluy = pluy;
	p->anglefrac = anglefrac;
	p->gotgatgun = gotgatgun;
	p->facecount = facecount;
	p->playerxmove = playerxmove;
	p->playerymove = playerymove;
	p->lastattacker = LastAttacker;
	p->controlx = controlx;
	p->controly = controly;
	memcpy (p->buttonstate,buttonstate,sizeof(buttonstate));
	memcpy (p->buttonheld,buttonheld,sizeof(buttonheld));
}

static void LoadContext (netplayer_t *p)
{
	gamestate.oldscore = p->oldscore;
	gamestate.score = p->score;
	gamestate.nextextra = p->nextextra;
	gamestate.lives = p->lives;
	gamestate.health = p->health;
	gamestate.ammo = p->ammo;
	gamestate.keys = p->keys;
	gamestate.bestweapon = p->bestweapon;
	gamestate.weapon = p->weapon;
	gamestate.chosenweapon = p->chosenweapon;
	gamestate.faceframe = p->faceframe;
	gamestate.attackframe = p->attackframe;
	gamestate.attackcount = p->attackcount;
	gamestate.weaponframe = p->weaponframe;

	thrustspeed = p->thrustspeed;
	plux = p->plux;
	pluy = p->pluy;
	anglefrac = p->anglefrac;
	gotgatgun = p->gotgatgun;
	facecount = p->facecount;
	playerxmove = p->playerxmove;
	playerymove = p->playerymove;
	LastAttacker = p->lastattacker;
	controlx = p->controlx;
	controly = p->controly;
	memcpy (buttonstate,p->buttonstate,sizeof(buttonstate));
	memcpy (buttonheld,p->buttonheld,sizeof(buttonheld));

	player = p->ob;
}

void NetSetContext (int p)
{
	if (p == netcontext)
	{
		player = np[p].ob;
		return;
	}
	SaveContext (&np[netcontext]);
	LoadContext (&np[p]);
	netcontext = p;
}

boolean NetIsLocal (void)
{
	return !netgame || netcontext == localplayer;
}

boolean NetPlayerAlive (int p)
{
	return !np[p].dead;
}

boolean NetCurrentDead (void)
{
	return netgame && np[netcontext].dead;
}

boolean NetLocalDead (void)
{
	return netgame && np[localplayer].dead;
}

static int GetHealth (int p)
{
	return p == netcontext ? gamestate.health : np[p].health;
}


/*
=============================================================================

						 GAME LOGIC

=============================================================================
*/

/*
===================
=
= NetTarget
=
= The live player nearest an actor: the one its AI works against
=
===================
*/

int NetTarget (objtype *ob)
{
	int		i,best;
	long	dx,dy,d,bestd;

	best = -1;
	bestd = 0x7fffffffl;
	for (i=0;i<numplayers;i++)
	{
		if (np[i].dead)
			continue;
		dx = labs(ob->x - np[i].ob->x);
		dy = labs(ob->y - np[i].ob->y);
		d = dx>dy ? dx : dy;
		if (d < bestd)
		{
			bestd = d;
			best = i;
		}
	}
	if (best == -1)
	{
		for (i=0;i<numplayers;i++)
			if (!np[i].removed)
				return i;
		return 0;
	}
	return best;
}


/*
===================
=
= NetProject
=
= Where ob is in viewer's view, worked out the same on every machine
= (the real view depends on the local screen size)
=
===================
*/

static boolean NetProject (objtype *viewer, objtype *ob, fixed *nxp, long *offp)
{
	fixed	vcos,vsin,vx,vy,gx,gy,nx,ny;

	vcos = costable[viewer->angle];
	vsin = sintable[viewer->angle];
	vx = viewer->x - FixedByFrac(NETFOCAL,vcos);
	vy = viewer->y + FixedByFrac(NETFOCAL,vsin);

	gx = ob->x - vx;
	gy = ob->y - vy;
	nx = FixedByFrac(gx,vcos) - FixedByFrac(gy,vsin) - NETACTORSIZE;
	ny = FixedByFrac(gy,vcos) + FixedByFrac(gx,vsin);

	*nxp = nx;
	if (nx < MINDIST)
		return false;
	*offp = ny*NETSCALE/nx;
	return true;
}


/*
===================
=
= NetCanSee
=
= Stands in for FL_VISABLE (set by the local renderer) in the game logic
=
===================
*/

boolean NetCanSee (objtype *viewer, objtype *ob)
{
	fixed	nx;
	long	off;

	if (!NetProject (viewer,ob,&nx,&off))
		return false;
	return off > -NETHALFVIEW && off < NETHALFVIEW;
}


/*
===================
=
= NetFindTarget
=
= What the current player's gun or knife would hit: the nearest shootable
= actor in the crosshair with a clear line to it
=
===================
*/

objtype *NetFindTarget (objtype *shooter, boolean knife, long *dist)
{
	objtype	*check,*closest;
	fixed	nx;
	long	off,best;

	closest = NULL;
	best = 0x7fffffffl;
	for (check = playerobjs[0] ; check ; check = check->next)
	{
		if (check->obclass == playerobj)
		{
			int i;
			if (!hello_ff || check == shooter)
				continue;
			for (i=0;i<numplayers;i++)
				if (np[i].ob == check)
					break;
			if (i == numplayers || np[i].dead)
				continue;
		}
		else if (!(check->flags & FL_SHOOTABLE))
			continue;
		if (!NetProject (shooter,check,&nx,&off))
			continue;
		if (off <= -NETSHOOT || off >= NETSHOOT)
			continue;
		if (nx >= best)
			continue;
		if (!CheckLine (check))
			continue;
		best = nx;
		closest = check;
	}
	*dist = best;
	return closest;
}


/*
===================
=
= NetKillPlayer
=
= The current player ran out of health: lie down until respawn
=
===================
*/

void T_NetDead (objtype *ob);
statetype s_netdead = {false,0,0,T_NetDead,NULL,NULL};

void NetKillPlayer (objtype *attacker)
{
	netplayer_t *p = &np[netcontext];

	p->dead = true;
	p->respawn = 0;
	player->state = &s_netdead;
	player->ticcount = 0;
	gamestate.attackframe = gamestate.weaponframe = 0;
	if (netcontext == localplayer)
		SD_PlaySound (PLAYERDEATHSND);
	{
		char str[40];
		sprintf (str,"Player %d died",netcontext+1);
		AddChatLine (str,7);
	}
}

/*
-----------------------------------------------------------------------------

 THE RESPAWN SOUND

 Doom's teleport sound (DSTELEPT) when the player has Doom: it's read from
 their own DOOM2.WAD / DOOM.WAD / ... in the current directory, or the one
 -doomwad <file> names. Nothing of Doom's is in this program. Without one,
 an effect of our own is made: a rising shimmer over a burst of noise.
 Either is a digitized sound; with no digitized sound device the extra
 life sound is played instead.

-----------------------------------------------------------------------------
*/

#define RSRATE		7042		// the game's digitized sounds
#define RSPRIORITY	40

static	boolean	rstried;
static	byte	*rsdata;
static	long	rslen;

static long GetLong (byte *p)
{
	return p[0] | (p[1]<<8) | ((long)p[2]<<16) | ((long)p[3]<<24);
}

// DSTELEPT from a Doom WAD, resampled to RSRATE; false if there isn't one
static boolean LoadDoomTeleport (char *name)
{
	FILE	*f;
	byte	hdr[12],ent[16],*lump,*out;
	long	numlumps,dirofs,i,pos,size,rate,count,outlen,j,frac;

	if (!(f = fopen (name,"rb")))
		return false;
	if (fread (hdr,1,12,f) != 12 || (memcmp (hdr,"IWAD",4) && memcmp (hdr,"PWAD",4)))
	{
		fclose (f);
		return false;
	}
	numlumps = GetLong (hdr+4);
	dirofs = GetLong (hdr+8);
	if (numlumps <= 0 || numlumps > 65536 || fseek (f,dirofs,SEEK_SET))
	{
		fclose (f);
		return false;
	}
	pos = size = -1;
	for (i=0;i<numlumps;i++)
	{
		if (fread (ent,1,16,f) != 16)
			break;
		if (!strncmp ((char *)ent+8,"DSTELEPT",8))
		{
			pos = GetLong (ent);
			size = GetLong (ent+4);			// the last one wins, as in Doom
		}
	}
	if (pos < 0 || size < 8 || size > 0x40000 || !(lump = malloc (size)))
	{
		fclose (f);
		return false;
	}
	if (fseek (f,pos,SEEK_SET) || fread (lump,1,size,f) != size)
	{
		free (lump);
		fclose (f);
		return false;
	}
	fclose (f);

	// format 3: word 3, word rate, long count, count 8 bit unsigned samples,
	// the first and last 16 of them padding
	rate = lump[2] | (lump[3]<<8);
	count = GetLong (lump+4);
	if ((lump[0] | (lump[1]<<8)) != 3 || rate < 4000 || rate > 48000
	|| count <= 32 || count > size-8)		// (no overflow with 32 bit longs)
	{
		free (lump);
		return false;
	}
	count -= 32;
	outlen = count * RSRATE / rate;
	if (outlen < 1 || !(out = malloc (outlen)))
	{
		free (lump);
		return false;
	}
	for (i=0;i<outlen;i++)
	{
		pos = i * rate / RSRATE;			// linear interpolation
		frac = (i * rate % RSRATE) * 256 / RSRATE;
		j = pos+1 < count ? pos+1 : pos;
		out[i] = (lump[8+16+pos]*(256-frac) + lump[8+16+j]*frac) >> 8;
	}
	free (lump);
	rsdata = out;
	rslen = outlen;
	return true;
}

// our own: 0.6 s, a tone sweeping up with a fast warble, over noise that
// dies away, faded in and out
static void MakeTeleportSound (void)
{
	long	i,n;
	unsigned long seed = 0x1234567;		// not the game's random numbers
	double	t,phase,freq,env,noise,lp,v;

	n = RSRATE * 6 / 10;
	if (!(rsdata = malloc (n)))
		return;
	phase = lp = 0;
	for (i=0;i<n;i++)
	{
		t = (double)i / n;
		freq = 220 + 1500*t*t + 90*sin (2*3.14159265*38*t);
		phase += 2*3.14159265*freq / RSRATE;
		seed = seed*1103515245 + 12345;
		noise = ((seed >> 16) & 0xffff) / 32768.0 - 1;
		lp += (noise - lp) * (0.15 + 0.6*t);		// brighter as it rises
		env = (t < 0.08 ? t/0.08 : 1) * (1-t) * (1-t);
		v = (0.55*sin (phase) + 0.45*sin (phase*2.01)*t + 0.7*lp*(1-t)) * env;
		v = 128 + v*170;
		rsdata[i] = v < 0 ? 0 : v > 255 ? 255 : (byte)v;
	}
	rslen = n;
}

static void LoadRespawnSound (void)
{
	static char *wads[] = {"DOOM2.WAD","DOOM.WAD","PLUTONIA.WAD","TNT.WAD",
		"DOOMU.WAD","FREEDOOM2.WAD","FREEDOOM1.WAD","DOOM1.WAD",NULL};
	int		i;

	rstried = true;
	for (i=1;i<_argc-1;i++)
		if (!stricmp (_argv[i],"-doomwad") && LoadDoomTeleport (_argv[i+1]))
			return;
	for (i=0;wads[i];i++)
		if (LoadDoomTeleport (wads[i]))
			return;
	MakeTeleportSound ();
}

// from Respawn: everyone hears it, from where the player comes back
static void RespawnSound (objtype *ob)
{
	if (!rstried)
		LoadRespawnSound ();

	if (netcontext != localplayer)
	{
		SetSoundLoc (ob->x,ob->y);
		if (soundinaudible)
			return;
		SD_PositionSound (leftchannel,rightchannel);
	}
	if (DigiMode == sds_Off)
	{
		// nothing digitized: the next best thing
		if (netcontext != localplayer)
			PlaySoundLocGlobal (BONUS1UPSND,ob->x,ob->y);
		else
			SD_PlaySound (BONUS1UPSND);
	}
	else if (SD_PlaySample (rsdata,rslen,RSPRIORITY) && netcontext != localplayer)
	{
		globalsoundx = ob->x;			// UpdateSoundLoc keeps it placed
		globalsoundy = ob->y;
	}
}

static void Respawn (netplayer_t *p)
{
	objtype	*ob = p->ob;

	p->dead = false;
	gamestate.health = 100;
	if (gamestate.ammo < STARTAMMO)
		gamestate.ammo = STARTAMMO;
	gamestate.weapon = gamestate.chosenweapon;
	gamestate.attackframe = gamestate.weaponframe = 0;

	ob->x = p->spawnx;
	ob->y = p->spawny;
	ob->angle = p->spawnangle;
	ob->state = &s_player;
	ob->ticcount = 0;
	Thrust (0,0);				// tile, area
	plux = ob->x >> UNSIGNEDSHIFT;
	pluy = ob->y >> UNSIGNEDSHIFT;
	ConnectAreas ();
	RespawnSound (ob);

	DrawHealth ();
	DrawFace ();
	DrawAmmo ();
	DrawWeapon ();
}

void T_NetDead (objtype *ob)
{
	netplayer_t *p = &np[netcontext];

	if (p->removed)
		return;
	p->respawn += tics;
	if (p->respawn >= RESPAWNTICS && SpawnSpotClear (p))
		Respawn (p);
}


/*
===================
=
= NetPickups
=
= What DrawScaleds does with bonus items, for any player
=
===================
*/

static void NetPickups (objtype *ob)
{
	statobj_t	*statptr;
	fixed	vcos,vsin,vx,vy,gx,gy,nx,ny;
	int		oldkeys,i;

	vcos = costable[ob->angle];
	vsin = sintable[ob->angle];
	vx = ob->x - FixedByFrac(NETFOCAL,vcos);
	vy = ob->y + FixedByFrac(NETFOCAL,vsin);

	for (statptr = &statobjlist[0] ; statptr != laststatobj ; statptr++)
	{
		if (statptr->shapenum == -1 || !(statptr->flags & FL_BONUS))
			continue;
		if (abs(statptr->tilex - ob->tilex) > 1 || abs(statptr->tiley - ob->tiley) > 1)
			continue;

		gx = ((long)statptr->tilex<<TILESHIFT)+0x8000-vx;
		gy = ((long)statptr->tiley<<TILESHIFT)+0x8000-vy;
		nx = FixedByFrac(gx,vcos) - FixedByFrac(gy,vsin) - 0x2000;
		ny = FixedByFrac(gy,vcos) + FixedByFrac(gx,vsin);
		if (nx < MINDIST || nx >= TILEGLOBAL || ny <= -TILEGLOBAL/2 || ny >= TILEGLOBAL/2)
			continue;

		oldkeys = gamestate.keys;
		GetBonus (statptr);
		if (gamestate.keys != oldkeys)
		{
			// keys are for everybody
			for (i=0;i<numplayers;i++)
				if (i != netcontext)
					np[i].keys |= gamestate.keys;

			// and show up on our status bar, whoever picked them up
			// (DrawKeys draws only for the local player's context)
			if (netcontext != localplayer)
			{
				i = netcontext;
				NetSetContext (localplayer);
				DrawKeys ();
				NetSetContext (i);
			}
		}
	}
}


/*
===================
=
= NetPlaySound
=
= SD_PlayHook: the game plays a player's own sounds (shots, pickups, the
= locked door, a pushwall...) unpositioned, which is right for the local
= player but put everybody else's at full volume in our ears, however
= far away they were. Those are played from where that player is, so
= they get quieter with distance (SetSoundLoc) like the actors' sounds.
= Anything else (the bosses' voices, the menus) is left alone: -1.
=
===================
*/

static int NetPlaySound (soundnames s)
{
	if (!netgame || netcontext == localplayer || !player || obj != player)
		return -1;
	PlaySoundLocGlobal (s,player->x,player->y);
	return 0;
}


/*
===================
=
= NetDamagePlayer
=
= Friendly fire: the current player hit another one
=
===================
*/

void NetDamagePlayer (objtype *target, int damage, objtype *attacker)
{
	int	i,shooter;

	for (i=0;i<numplayers;i++)
		if (np[i].ob == target)
			break;
	if (i == numplayers || np[i].dead)
		return;
	shooter = netcontext;
	NetSetContext (i);
	TakeDamage (damage,attacker);
	NetSetContext (shooter);
}

void NetPlayerHurt (void)
{
	np[netcontext].pain = 6;
}


/*
===================
=
= NetPlayerBlocked
=
= Players are solid to each other, like actors: a move that ends up
= within MINACTORDIST of another live player is refused, unless they
= already overlapped (respawned on each other) and it takes them apart
=
===================
*/

long	netoldx,netoldy;		// set by ClipMove: where the mover started

boolean NetPlayerBlocked (objtype *ob)
{
	int		i;
	objtype	*other;
	long	dx,dy,olddx,olddy,newd,oldd;

	for (i=0;i<numplayers;i++)
	{
		other = np[i].ob;
		if (other == ob || np[i].dead)
			continue;
		dx = labs (ob->x - other->x);
		dy = labs (ob->y - other->y);
		if (dx >= MINACTORDIST || dy >= MINACTORDIST)
			continue;
		olddx = labs (netoldx - other->x);
		olddy = labs (netoldy - other->y);
		newd = dx > dy ? dx : dy;
		oldd = olddx > olddy ? olddx : olddy;
		if (oldd < MINACTORDIST && newd > oldd)
			continue;			// getting out of an overlap
		return true;
	}
	return false;
}

static boolean SpawnSpotClear (netplayer_t *p)
{
	int i;

	for (i=0;i<numplayers;i++)
		if (&np[i] != p && !np[i].dead
		&& labs (np[i].ob->x - p->spawnx) < MINACTORDIST
		&& labs (np[i].ob->y - p->spawny) < MINACTORDIST)
			return false;
	return true;
}


/*
=============================================================================

						 HOW THE OTHERS LOOK

The other players are drawn with BJ from PLAYERS.WL6 when it's there,
otherwise with the SS sprites (eight directions, a gun, walking,
shooting, pain and death frames). Either way they're recolored (BJ's
vest, the SS's uniform) so each player has a color and nobody takes
them for an enemy. The copies are made when first needed.

=============================================================================
*/

#define NUMSSFRAMES	(SPR_SS_SHOOT3-SPR_SS_S_1+1)

static	byte	*netshapes[MAXPLAYERS][NUMSSFRAMES];
static	byte	netcolors[MAXPLAYERS][256];
static	boolean	netcolorsmade;

/*
-----------------------------------------------------------------------------

 PLAYERS.WL6 (PLAYERS.<the data's extension>), made by msdos/mkplayers.py
 from a sprite sheet: BJ in 8 directions standing, walking, aiming and
 firing, plus pain and death frames. When it's there the other players
 are drawn with it instead of the SS; its team[] colors (the vest) are
 the ones recolored per player.

	char	magic[4]	"WLPS"
	word	version		1
	word	numframes
	byte	team[256]
	long	offset[numframes], word length[numframes]
	...		frames, t_compshape like VSWAP's sprites

 The scaler doesn't check what it reads, so every frame is checked here.

-----------------------------------------------------------------------------
*/

#define PF_STAND	0			// + direction
#define PF_WALK		8			// + walk frame*8 + direction
#define PF_AIM		40			// + direction
#define PF_FIRE		48			// + direction
#define PF_PAIN		56
#define PF_DIE1		57			// 57-60
#define PF_DEAD		61
#define PF_NUM		62

#define PFFRAME		0x3c0		// frame numbers above every sprite's: the file's

static	boolean	pftried,pfloaded;
static	byte	*pfdata;
static	byte	*pfframe[PF_NUM];
static	word	pflength[PF_NUM];
static	byte	pfteam[256];
static	byte	*pfshapes[MAXPLAYERS][PF_NUM];

static word GetWord (byte *p)
{
	return p[0] | (p[1]<<8);
}

static boolean CheckFrame (byte *f, unsigned len)
{
	unsigned	left,right,col,ofs,end,top,start;

	if (len < 6)
		return false;
	left = GetWord (f);
	right = GetWord (f+2);
	if (left > right || right > 63 || 4+2*(right-left+1) > len)
		return false;
	for (col=0 ; col <= right-left ; col++)
	{
		ofs = GetWord (f+4+col*2);
		for (;;)
		{
			if (ofs+2 > len)
				return false;
			end = GetWord (f+ofs);
			if (!end)
				break;
			if (ofs+6 > len)
				return false;
			top = GetWord (f+ofs+2);
			start = GetWord (f+ofs+4);
			if ((end|start)&1 || start >= end || end > 128
			|| (word)(top+start/2) >= len || (word)(top+end/2) > len
			|| (word)(top+start/2) > (word)(top+end/2))
				return false;
			ofs += 6;
		}
	}
	return true;
}

static void LoadPlayerSprites (void)
{
	char	fname[16];
	FILE	*f;
	long	size,ofs;
	unsigned len;
	int		i,n;

	pftried = true;
	strcpy (fname,"PLAYERS.");
	strcat (fname,extension);
	f = fopen (fname,"rb");
	if (!f)
		return;
	fseek (f,0,SEEK_END);
	size = ftell (f);
	fseek (f,0,SEEK_SET);
	if (size < 8+256 || size > 0x100000 || !(pfdata = malloc (size)))
	{
		fclose (f);
		return;
	}
	if (fread (pfdata,1,size,f) != size)
		size = 0;
	fclose (f);

	n = size ? GetWord (pfdata+6) : 0;
	if (memcmp (pfdata,"WLPS",4) || GetWord (pfdata+4) != 1 || n < PF_NUM
	|| 8+256+n*6 > size)
		goto bad;
	memcpy (pfteam,pfdata+8,256);
	for (i=0;i<PF_NUM;i++)
	{
		byte *t = pfdata+8+256+i*4;
		ofs = t[0] | (t[1]<<8) | ((long)t[2]<<16) | ((long)t[3]<<24);
		len = GetWord (pfdata+8+256+n*4+i*2);
		if (ofs < 0 || ofs+len > size || !CheckFrame (pfdata+ofs,len))
			goto bad;
		pfframe[i] = pfdata+ofs;
		pflength[i] = len;
	}
	pfloaded = true;
	return;

bad:
	free (pfdata);
	pfdata = NULL;
	{
		char str[40];
		sprintf (str,"%s is damaged: SS used",fname);
		AddChatLine (str,7);
	}
}

static void MakeColors (void)
{
	int		p,c,i,r,g,b,nr,ng,nb,best,bestd,d,dr,dg,db;
	byte	*pal = gamepal;

	for (p=0;p<MAXPLAYERS;p++)
		for (c=0;c<256;c++)
		{
			netcolors[p][c] = c;
			r = pal[c*3]; g = pal[c*3+1]; b = pal[c*3+2];
			if (pfloaded)
			{
				// BJ's gold vest
				if (!pfteam[c])
					continue;
				switch (p)
				{
				case 0: nr = b; ng = (r+g)/2; nb = b; break;		// green
				case 1: nr = (r+g)/2; ng = b; nb = b; break;		// red
				case 2: continue;									// gold
				default: nr = ng = nb = (r+g)/2; break;				// white
				}
			}
			else
			{
				if (b < r+4 || b < g+4)
					continue;			// only the blue of the uniform
				switch (p)
				{
				case 0: nr = r; ng = b; nb = g; break;			// green
				case 1: nr = b; ng = g; nb = r; break;			// red
				case 2: nr = b; ng = b*3/4; nb = r/2; break;	// gold
				default: nr = ng = nb = (r+g+b*2)/3; break;		// white
				}
			}
			best = c;
			bestd = 0x7fffffff;
			for (i=0;i<255;i++)
			{
				if (pfloaded && pfteam[i] && p != 2)
					continue;			// not back to gold
				dr = pal[i*3]-nr; dg = pal[i*3+1]-ng; db = pal[i*3+2]-nb;
				d = dr*dr*3 + dg*dg*4 + db*db*2;
				if (d < bestd)
				{
					bestd = d;
					best = i;
				}
			}
			netcolors[p][c] = best;
		}
	netcolorsmade = true;
}

// a copy of a shape with exactly the pixels the scaler reads recolored
static byte *Recolor (byte *orig, unsigned length, int p)
{
	int		col,src,end,start;
	byte	*copy;
	word	*cmd,top;
	t_compshape *shape;

	copy = malloc (length);
	if (!copy)
		return NULL;
	memcpy (copy,orig,length);

	shape = (t_compshape *)orig;
	for (col=0 ; col <= shape->rightpix-shape->leftpix ; col++)
	{
		cmd = (word *)(orig + shape->dataofs[col]);
		while ((end = cmd[0]/2) != 0)
		{
			top = cmd[1];
			start = cmd[2]/2;
			for (src=start;src<end;src++)
				copy[(word)(top+src)] = netcolors[p][orig[(word)(top+src)]];
			cmd += 3;
		}
	}
	return copy;
}

memptr NetShape (int shapenum)
{
	int		p,frame,page;
	byte	*copy;

	p = (shapenum >> 10) & 3;
	frame = shapenum & 0x3ff;

	if (!netcolorsmade)
		MakeColors ();

	if (frame >= PFFRAME)
	{
		frame -= PFFRAME;
		if (!pfshapes[p][frame])
			pfshapes[p][frame] = Recolor (pfframe[frame],pflength[frame],p);
		return pfshapes[p][frame] ? pfshapes[p][frame] : pfframe[frame];
	}

	if (frame < SPR_SS_S_1 || frame > SPR_SS_SHOOT3)
		return PM_GetSpritePage (frame);

	copy = netshapes[p][frame-SPR_SS_S_1];
	if (copy)
		return copy;

	page = PMSpriteStart + frame;
	copy = Recolor (PM_GetPage (page),PMPages[page].length,p);
	if (!copy)
		return PM_GetPage (page);
	netshapes[p][frame-SPR_SS_S_1] = copy;
	return copy;
}


/*
===================
=
= NetPlayerShape
=
= Which frame of another player the local view shows (0 = none); called
= after TransformActor, so ob->viewx is set
=
===================
*/

int NetPlayerShape (objtype *ob)
{
	int			i,angle,viewangle,frame,wf;
	netplayer_t	*p;

	for (i=0;i<numplayers;i++)
		if (np[i].ob == ob)
			break;
	if (i == numplayers || np[i].removed)
		return 0;
	p = &np[i];

	if (!pftried)
		LoadPlayerSprites ();

	if (p->dead)
	{
		if (pfloaded)
			frame = p->respawn < 24 ? PFFRAME+PF_DIE1+p->respawn/6 : PFFRAME+PF_DEAD;
		else if (p->respawn < 8)
			frame = SPR_SS_DIE_1;
		else if (p->respawn < 16)
			frame = SPR_SS_DIE_2;
		else if (p->respawn < 24)
			frame = SPR_SS_DIE_3;
		else
			frame = SPR_SS_DEAD;
		return NETSHAPEFLAG | (i<<10) | frame;
	}

	// which way it faces from here
	viewangle = player->angle + (centerx - ob->viewx)/8;
	angle = (viewangle-180) - ob->angle + ANGLES/16;
	while (angle >= ANGLES)
		angle -= ANGLES;
	while (angle < 0)
		angle += ANGLES;
	angle /= ANGLES/8;

	wf = i == netcontext ? gamestate.weaponframe : p->weaponframe;

	if (pfloaded)
	{
		// BJ aims and fires in every direction, not only at us
		if (p->pain)
			frame = PF_PAIN;
		else if (wf)
			frame = (wf == 2 ? PF_FIRE : PF_AIM) + angle;
		else if (p->moving)
			frame = PF_WALK + ((p->anim >> 14) & 3)*8 + angle;
		else
			frame = PF_STAND + angle;
		return NETSHAPEFLAG | (i<<10) | (PFFRAME+frame);
	}

	if (p->pain)
		frame = SPR_SS_PAIN_1;
	else if (wf && (angle == 0 || angle == 1 || angle == 7))
		frame = wf == 1 ? SPR_SS_SHOOT1 : wf == 2 ? SPR_SS_SHOOT2 : SPR_SS_SHOOT3;
	else if (p->moving)
		frame = SPR_SS_W1_1 + ((p->anim >> 14) & 3)*8 + angle;
	else
		frame = SPR_SS_S_1 + angle;

	return NETSHAPEFLAG | (i<<10) | frame;
}


/*
=============================================================================

						 IPX

=============================================================================
*/

#define IPX_SOCKET	0x7135
#define PKTMAX		200
#define NUMRECV		16
#define NUMSEND		MAXPLAYERS
#define ECBSIZE		48
#define HDRSIZE		30
#define SLOTSIZE	(ECBSIZE+HDRSIZE+PKTMAX)

static	boolean	ipxup;
static	word	ipxseg,ipxoff;
static	int		dossel;
static	word	dosseg;
static	unsigned long doslin;
static	word	sockw;				// socket, network byte order
static	byte	myaddr[10];

static unsigned long SlotLin (int slot)
{
	return doslin + (unsigned long)slot*SLOTSIZE;
}

static void IPXCall (__dpmi_regs *r)
{
	r->x.cs = ipxseg;
	r->x.ip = ipxoff;
	r->x.ss = r->x.sp = 0;
	r->x.flags = 0;
	__dpmi_simulate_real_mode_procedure_retf (r);
}

static void IPXListen (int slot)
{
	unsigned long l = SlotLin (slot);
	word	off = (word)(l - (unsigned long)dosseg*16);
	__dpmi_regs r;
	int		i;

	for (i=0;i<ECBSIZE;i++)
		_farpokeb (_dos_ds,l+i,0);
	_farpokew (_dos_ds,l+10,sockw);
	_farpokew (_dos_ds,l+34,1);						// one fragment
	_farpokew (_dos_ds,l+36,off+ECBSIZE);
	_farpokew (_dos_ds,l+38,dosseg);
	_farpokew (_dos_ds,l+40,HDRSIZE+PKTMAX);

	memset (&r,0,sizeof(r));
	r.x.bx = 4;
	r.x.es = dosseg;
	r.x.si = off;
	IPXCall (&r);
}

static void IPXSend (int slot, byte *addr, void *data, int len)
{
	unsigned long l = SlotLin (NUMRECV+slot);
	word	off = (word)(l - (unsigned long)dosseg*16);
	unsigned long h = l+ECBSIZE;
	__dpmi_regs r;
	long	start;
	int		i;

	start = _farpeekl (_dos_ds,0x46c);
	while (_farpeekb (_dos_ds,l+8))					// still going out
	{
		memset (&r,0,sizeof(r));
		r.x.bx = 0x0a;
		IPXCall (&r);
		if (_farpeekl (_dos_ds,0x46c) - start > 3)
			return;
	}

	for (i=0;i<ECBSIZE;i++)
		_farpokeb (_dos_ds,l+i,0);
	_farpokew (_dos_ds,l+10,sockw);
	for (i=0;i<6;i++)
		_farpokeb (_dos_ds,l+28+i,addr[4+i]);		// immediate address
	_farpokew (_dos_ds,l+34,1);
	_farpokew (_dos_ds,l+36,off+ECBSIZE);
	_farpokew (_dos_ds,l+38,dosseg);
	_farpokew (_dos_ds,l+40,HDRSIZE+len);

	_farpokew (_dos_ds,h+0,0xffff);					// checksum
	_farpokew (_dos_ds,h+2,0);
	_farpokeb (_dos_ds,h+4,0);
	_farpokeb (_dos_ds,h+5,4);						// IPX packet
	for (i=0;i<10;i++)
		_farpokeb (_dos_ds,h+6+i,addr[i]);
	_farpokew (_dos_ds,h+16,sockw);
	dosmemput (data,len,h+HDRSIZE);

	memset (&r,0,sizeof(r));
	r.x.bx = 3;
	r.x.es = dosseg;
	r.x.si = off;
	IPXCall (&r);
}

static boolean IPXStartup (void)
{
	__dpmi_regs r;
	int		seg,i;

	memset (&r,0,sizeof(r));
	r.x.ax = 0x7a00;
	__dpmi_int (0x2f,&r);
	if (r.h.al != 0xff)
		return false;
	ipxseg = r.x.es;
	ipxoff = r.x.di;

	seg = __dpmi_allocate_dos_memory ((SLOTSIZE*(NUMRECV+NUMSEND)+16+15)/16,&dossel);
	if (seg == -1)
		return false;
	dosseg = seg;
	doslin = (unsigned long)seg*16;

	sockw = (IPX_SOCKET>>8) | ((IPX_SOCKET&0xff)<<8);
	memset (&r,0,sizeof(r));
	r.x.bx = 0;
	r.h.al = 0;
	r.x.dx = sockw;
	IPXCall (&r);
	if (r.h.al == 0xff)			// left open by a crash: start over
	{
		memset (&r,0,sizeof(r));
		r.x.bx = 1;
		r.x.dx = sockw;
		IPXCall (&r);
		memset (&r,0,sizeof(r));
		r.x.bx = 0;
		r.x.dx = sockw;
		IPXCall (&r);
	}
	if (r.h.al)
	{
		__dpmi_free_dos_memory (dossel);
		return false;
	}

	// local address
	memset (&r,0,sizeof(r));
	r.x.bx = 9;
	r.x.es = dosseg;
	r.x.si = SLOTSIZE*(NUMRECV+NUMSEND);
	IPXCall (&r);
	dosmemget (doslin+SLOTSIZE*(NUMRECV+NUMSEND),10,myaddr);

	for (i=0;i<NUMSEND;i++)
		_farpokeb (_dos_ds,SlotLin(NUMRECV+i)+8,0);
	for (i=0;i<NUMRECV;i++)
		IPXListen (i);

	ipxup = true;
	return true;
}

void NetShutdown (void)
{
	__dpmi_regs r;

	if (!ipxup)
		return;
	ipxup = false;
	memset (&r,0,sizeof(r));
	r.x.bx = 1;
	r.x.dx = sockw;
	IPXCall (&r);				// cancels the listens
	__dpmi_free_dos_memory (dossel);
}

//
// IPXGet: next received packet, or 0
//
static int IPXGet (void *buf, byte *from)
{
	static int next;
	int		i,slot,len;
	unsigned long l;
	__dpmi_regs r;

	memset (&r,0,sizeof(r));
	r.x.bx = 0x0a;					// relinquish control: lets it work
	IPXCall (&r);

	for (i=0;i<NUMRECV;i++)
	{
		slot = (next+i)%NUMRECV;
		l = SlotLin (slot);
		if (_farpeekb (_dos_ds,l+8))
			continue;				// still listening
		next = slot+1;
		len = 0;
		if (!_farpeekb (_dos_ds,l+9))
		{
			len = ((_farpeekb (_dos_ds,l+ECBSIZE+2)<<8) | _farpeekb (_dos_ds,l+ECBSIZE+3)) - HDRSIZE;
			if (len < 0 || len > PKTMAX)
				len = 0;
			dosmemget (l+ECBSIZE+18,10,from);
			dosmemget (l+ECBSIZE+HDRSIZE,len,buf);
		}
		IPXListen (slot);
		if (len > 0)
			return len;
	}
	return 0;
}


/*
=============================================================================

						 PACKETS

=============================================================================
*/

enum {pk_hello=1,pk_tics,pk_quit,pk_chat};

#define NETVERSION	2

typedef struct
{
	byte	magic[2];
	byte	type,version;
	byte	numnodes,episode,level,skill;
	byte	gotcount,friendlyfire,pad[2];
} __attribute__((packed)) hellopacket_t;

typedef struct
{
	byte	magic[2];
	byte	type,player;
	byte	seq,count,pad[2];
	long	first;				// frame of cmds[0]
	long	ack;				// frames the sender has finished
	long	hashframe;
	unsigned long hash;
	netcmd_t cmds[NETBACKUP];
} __attribute__((packed)) ticpacket_t;

static	byte	broadcast[10] = {0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff};

static	byte	levelseq;			// counts play loops, the same everywhere
static	long	netframe;			// frames finished this level
static	long	maketic;			// next frame to sample controls for
static	byte	oldseq;				// the previous level, for late barrier packets
static	long	oldack;

static	unsigned long hashring[NETRING];
static	long	hashframes[NETRING];
static	long	desyncframe = -1;

static	int		hello_episode,hello_level,hello_skill;
static	FILE	*netlog;			// -netlog: NETLOG.TXT, for chasing desyncs

static long BiosTicks (void)
{
	return _farpeekl (_dos_ds,0x46c);
}

static void SendTics (int type)
{
	ticpacket_t pk;
	netplayer_t *me = &np[localplayer];
	long	f;
	int		i;

	memset (&pk,0,sizeof(pk));
	pk.magic[0] = 'W'; pk.magic[1] = '3';
	pk.type = type;
	pk.player = localplayer;
	pk.seq = levelseq;
	f = maketic - NETBACKUP;
	if (f < NETDELAY)
		f = NETDELAY;
	pk.first = f;
	for (i=0 ; f < maketic ; f++,i++)
		pk.cmds[i] = me->cmds[f & (NETRING-1)];
	pk.count = i;
	pk.ack = netframe;
	pk.hashframe = netframe-1;
	pk.hash = netframe ? hashring[(netframe-1) & (NETRING-1)] : 0;

	for (i=0;i<numplayers;i++)
		if (i != localplayer && !np[i].gone)
			IPXSend (i,np[i].addr,&pk,sizeof(pk)-sizeof(pk.cmds)+pk.count*sizeof(netcmd_t));
}

static void SendHello (byte *to)
{
	hellopacket_t pk;

	memset (&pk,0,sizeof(pk));
	pk.magic[0] = 'W'; pk.magic[1] = '3';
	pk.type = pk_hello;
	pk.version = NETVERSION;
	pk.numnodes = numplayers;
	pk.episode = hello_episode;
	pk.level = hello_level;
	pk.skill = hello_skill;
	pk.friendlyfire = hello_ff;
	pk.gotcount = numplayers;
	IPXSend (0,to,&pk,sizeof(pk));
}

/*
=============================================================================

						 CHAT

T starts a message, Enter sends it to everyone, Esc drops it; while
typing, Alt+1..Alt+0 send the macros from CHATMACS.CFG (SETUP edits
them). Messages show at the top of the view for a few seconds. Chat
doesn't touch the game, so it's sent on its own, twice, not in lockstep.

=============================================================================
*/

#define CHATLEN		60
#define NUMLINES	4
#define LINETICS	(70*6)

typedef struct
{
	byte	magic[2];
	byte	type,player;
	byte	msgid,len,pad[2];
	char	text[CHATLEN+1];
} __attribute__((packed)) chatpacket_t;

static	boolean	chatting;
static	char	chatbuf[CHATLEN+1];
static	int		chatlen;
static	byte	chatid,lastchatid[MAXPLAYERS];
static	boolean	gotchatid[MAXPLAYERS];

static	char	chatlines[NUMLINES][CHATLEN+24];
static	int		chatcolor[NUMLINES];
static	long	chattime[NUMLINES];
static	long	chatclock;

char	chatmacros[10][CHATLEN+1] =
{
	"Need ammo!",
	"Need health!",
	"Follow me!",
	"Wait for me!",
	"I've got the key.",
	"Found the exit!",
	"Watch out, guards!",
	"There's a secret here.",
	"Yes.",
	"No.",
};

static	int		playercolors[MAXPLAYERS] = {10,12,14,15};	// as their uniforms

static void LoadMacros (void)
{
	FILE	*f;
	char	line[128];
	int		i,l;

	f = fopen ("CHATMACS.CFG","r");
	if (!f)
		return;
	for (i=0;i<10 && fgets (line,sizeof(line),f);i++)
	{
		l = strlen (line);
		while (l && (line[l-1] == '\n' || line[l-1] == '\r'))
			line[--l] = 0;
		strncpy (chatmacros[i],line,CHATLEN);
		chatmacros[i][CHATLEN] = 0;
	}
	fclose (f);
}

static void AddChatLine (char *text, int color)
{
	int i;

	for (i=0;i<NUMLINES-1;i++)
	{
		strcpy (chatlines[i],chatlines[i+1]);
		chatcolor[i] = chatcolor[i+1];
		chattime[i] = chattime[i+1];
	}
	strncpy (chatlines[NUMLINES-1],text,sizeof(chatlines[0])-1);
	chatlines[NUMLINES-1][sizeof(chatlines[0])-1] = 0;
	chatcolor[NUMLINES-1] = color;
	chattime[NUMLINES-1] = chatclock + LINETICS;
}

static void ChatMessage (int player, char *text)
{
	char line[CHATLEN+24];

	sprintf (line,"Player %d: %s",player+1,text);
	AddChatLine (line,playercolors[player]);
}

static void SendChat (char *text)
{
	chatpacket_t pk;
	int		i,len;

	if (!*text)
		return;
	ChatMessage (localplayer,text);

	memset (&pk,0,sizeof(pk));
	pk.magic[0] = 'W'; pk.magic[1] = '3';
	pk.type = pk_chat;
	pk.player = localplayer;
	pk.msgid = ++chatid;
	strncpy (pk.text,text,CHATLEN);
	pk.len = len = strlen (pk.text);
	for (i=0;i<numplayers;i++)
		if (i != localplayer && !np[i].gone)
		{
			IPXSend (i,np[i].addr,&pk,sizeof(pk)-sizeof(pk.text)+len+1);
			IPXSend (i,np[i].addr,&pk,sizeof(pk)-sizeof(pk.text)+len+1);
		}
}

static void ReceiveChat (byte *buf, int len)
{
	chatpacket_t *pk = (chatpacket_t *)buf;

	if (len < sizeof(chatpacket_t)-sizeof(pk->text)+1 || pk->player >= numplayers
	|| pk->player == localplayer || pk->len > CHATLEN)
		return;
	if (gotchatid[pk->player] && lastchatid[pk->player] == pk->msgid)
		return;					// the second copy
	gotchatid[pk->player] = true;
	lastchatid[pk->player] = pk->msgid;
	pk->text[pk->len] = 0;
	ChatMessage (pk->player,pk->text);
	SD_PlaySound (MOVEGUN2SND);		// the menu's click
}

//
// ChatKeys: from NetCheckKeys; true while the keys are the chat's
//
static boolean ChatKeys (void)
{
	int		scan,ch;

	if (!chatting)
	{
		if (LastScan != sc_T || numplayers < 2)
			return false;
		chatting = true;
		chatlen = 0;
		chatbuf[0] = 0;
		LastScan = sc_None;
		LastASCII = 0;
		return true;
	}

	scan = LastScan;
	ch = LastASCII;
	LastScan = sc_None;
	LastASCII = 0;

	if (scan == sc_Escape)
		chatting = false;
	else if (scan == sc_Return)
	{
		SendChat (chatbuf);
		chatting = false;
	}
	else if (scan == sc_BackSpace)
	{
		if (chatlen)
			chatbuf[--chatlen] = 0;
	}
	else if (Keyboard[sc_Alt] && scan >= sc_1 && scan <= sc_0)
	{
		SendChat (chatmacros[scan-sc_1]);
		chatting = false;
	}
	else if (ch >= ' ' && ch < 127 && chatlen < CHATLEN)
	{
		chatbuf[chatlen++] = ch;
		chatbuf[chatlen] = 0;
	}
	return true;
}

static void DrawText (int x, int y, char *text, int color)
{
	char	str[CHATLEN+30];
	word	w,h;
	int		l;

	strncpy (str,text,sizeof(str)-1);
	str[sizeof(str)-1] = 0;
	l = strlen (str);
	for (;;)					// it doesn't clip: shorten it to fit
	{
		VW_MeasurePropString (str,&w,&h);
		if (x+w+1 < viewwidth || !l)
			break;
		str[--l] = 0;
	}
	SETFONTCOLOR(0,0);
	px = x+1;
	py = y+1;
	VW_DrawPropString (str);
	SETFONTCOLOR(color,0);
	px = x;
	py = y;
	VW_DrawPropString (str);
}

/*
===================
=
= NetDrawChat
=
= From ThreeDRefresh, with bufferofs at the view's corner
=
===================
*/

void NetDrawChat (void)
{
	int		i,y,oldfont;
	char	line[CHATLEN+8];

	chatclock += tics;
	CA_CacheGrChunk (STARTFONT);
	oldfont = fontnumber;
	fontnumber = 0;

	y = 2;
	for (i=0;i<NUMLINES;i++)
		if (chatlines[i][0] && chattime[i] > chatclock)
		{
			DrawText (4,y,chatlines[i],chatcolor[i]);
			y += 10;
		}
	if (chatting)
	{
		sprintf (line,"Say: %s_",chatbuf);
		DrawText (4,y,line,15);
	}

	fontnumber = oldfont;
}

boolean NetChatting (void)
{
	return chatting;
}

static void PlayerLeft (int p, long lastframe)
{
	char str[40];

	if (np[p].gone)
		return;
	np[p].gone = true;
	sprintf (str,"Player %d left the game",p+1);
	AddChatLine (str,7);
	np[p].lastframe = lastframe;
}

//
// NetReceive: take in whatever has arrived
//
static void NetReceive (void)
{
	byte	buf[PKTMAX],from[10];
	ticpacket_t *pk = (ticpacket_t *)buf;
	netplayer_t *p;
	int		len,i;
	long	f;

	if (!ipxup)
		return;

	while ((len = IPXGet (buf,from)) != 0)
	{
		if (len < 4 || buf[0] != 'W' || buf[1] != '3')
			continue;

		if (buf[2] == pk_chat)
		{
			ReceiveChat (buf,len);
			continue;
		}

		if (buf[2] == pk_hello)
		{
			// a straggler still looking for us
			for (i=0;i<numplayers;i++)
				if (i != localplayer && !memcmp (np[i].addr,from,10))
					SendHello (from);
			continue;
		}

		if (len < sizeof(ticpacket_t)-sizeof(pk->cmds) || pk->player >= numplayers
		|| pk->player == localplayer || pk->count > NETBACKUP)
			continue;
		p = &np[pk->player];

		if (pk->seq != levelseq)
		{
			if (pk->seq == oldseq && pk->type == pk_tics)
				SendTics (pk_tics);		// it's waiting for us to finish
			if ((signed char)(pk->seq - levelseq) > 0)
				p->ack = 0x7fffffffl;	// it's ahead: done with this level
			continue;
		}

		if (pk->ack > p->ack)
			p->ack = pk->ack;

		for (i=0;i<pk->count;i++)
		{
			f = pk->first+i;
			p->cmdframe[f & (NETRING-1)] = f;
			p->cmds[f & (NETRING-1)] = pk->cmds[i];
		}

		if (pk->type == pk_quit)
			PlayerLeft (pk->player,pk->first+pk->count-1);

		// consistency check
		f = pk->hashframe;
		if (f >= 0 && f < netframe && f > netframe-NETRING
		&& hashframes[f & (NETRING-1)] == f
		&& hashring[f & (NETRING-1)] != pk->hash
		&& desyncframe == -1)
			desyncframe = f;
	}
}


/*
=============================================================================

						 SETUP

=============================================================================
*/

static int ParmInt (char *name, int def)
{
	int i;

	for (i=1;i<_argc-1;i++)
		if (!stricmp (_argv[i],name))
			return atoi (_argv[i+1]);
	return def;
}

/*
===================
=
= NetStartup
=
= -nodes N [-episode E] [-level L] [-skill S]: waits in text mode until N
= copies of the game have found each other on the IPX network
=
===================
*/

void NetStartup (void)
{
	byte	addrs[MAXPLAYERS][10];
	int		settings[MAXPLAYERS][4];
	boolean	done[MAXPLAYERS];
	byte	buf[PKTMAX],from[10];
	hellopacket_t *pk = (hellopacket_t *)buf;
	int		nodes,found,i,j,len,shown,extra;
	long	lastsend;

	nodes = ParmInt ("-nodes",0);
	if (!nodes)
		return;
	if (nodes < 1 || nodes > MAXPLAYERS)
	{
		printf ("-nodes must be 1 to %d\n",MAXPLAYERS);
		exit (1);
	}

	hello_episode = ParmInt ("-episode",1)-1;
	hello_level = ParmInt ("-level",1)-1;
	hello_skill = ParmInt ("-skill",2);
#ifdef SPEAR
	hello_episode = 0;
#endif
	if (hello_episode < 0 || hello_episode > 5) hello_episode = 0;
	if (hello_level < 0 || hello_level > 20) hello_level = 0;
	if (hello_skill < 0 || hello_skill > 3) hello_skill = 2;
	hello_ff = 1;
	for (i=1;i<_argc;i++)
		if (!stricmp (_argv[i],"-nofriendlyfire") || !stricmp (_argv[i],"-noff"))
			hello_ff = 0;

	netgame = true;
	numplayers = nodes;
	SD_PlayHook = NetPlaySound;
	for (i=1;i<_argc;i++)
		if (!stricmp (_argv[i],"-netlog"))
			netlog = fopen ("NETLOG.TXT","w");
	NoWait = true;
	LoadMacros ();

	if (nodes == 1)
	{
		localplayer = 0;
		return;
	}

	if (!IPXStartup ())
	{
		printf ("No IPX driver found (load one, or use DOSBox's ipx=true and IPXNET).\n");
		exit (1);
	}
	atexit (NetShutdown);

	printf ("Looking for %d players on the IPX network (Esc aborts)...\n",nodes);

	memcpy (addrs[0],myaddr,10);
	settings[0][0] = hello_episode;
	settings[0][1] = hello_level;
	settings[0][2] = hello_skill;
	settings[0][3] = hello_ff;
	done[0] = false;
	found = 1;
	shown = 0;
	lastsend = BiosTicks () - 100;
	extra = 0;

	while (1)
	{
		if (BiosTicks () - lastsend >= 9 || BiosTicks () < lastsend)
		{
			hellopacket_t h;

			lastsend = BiosTicks ();
			memset (&h,0,sizeof(h));
			h.magic[0] = 'W'; h.magic[1] = '3';
			h.type = pk_hello;
			h.version = NETVERSION;
			h.numnodes = nodes;
			h.episode = hello_episode;
			h.level = hello_level;
			h.skill = hello_skill;
			h.friendlyfire = hello_ff;
			h.gotcount = found;
			IPXSend (0,broadcast,&h,sizeof(h));
			if (extra && !--extra)
				break;
		}

		while ((len = IPXGet (buf,from)) != 0)
		{
			if (len < 4 || buf[0] != 'W' || buf[1] != '3')
				continue;
			if (!memcmp (from,myaddr,10))
				continue;				// our own broadcast

			for (i=1;i<found;i++)
				if (!memcmp (addrs[i],from,10))
					break;

			if (pk->type == pk_hello)
			{
				if (len < sizeof(hellopacket_t) || pk->version != NETVERSION)
					continue;
				if (pk->numnodes != nodes)
				{
					printf ("Someone started with -nodes %d, ignoring them\n",pk->numnodes);
					continue;
				}
				if (i == found)
				{
					if (found == nodes)
						continue;
					memcpy (addrs[found],from,10);
					done[found] = false;
					found++;
				}
				settings[i][0] = pk->episode;
				settings[i][1] = pk->level;
				settings[i][2] = pk->skill;
				settings[i][3] = pk->friendlyfire;
				if (pk->gotcount == nodes)
					done[i] = true;
			}
			else if (i < found)
				done[i] = true;			// already playing
		}

		if (found != shown)
		{
			printf ("Found %d of %d\n",found,nodes);
			shown = found;
		}

		if (!extra && found == nodes)
		{
			for (i=1;i<found;i++)
				if (!done[i])
					break;
			if (i == found)
			{
				extra = 2;				// tell them a couple more times
				lastsend = BiosTicks () - 100;
			}
		}

		if (kbhit () && getch () == 27)
		{
			printf ("Aborted.\n");
			exit (1);
		}
	}

	//
	// player numbers go by address, so everyone agrees
	//
	for (i=0;i<nodes;i++)
		for (j=i+1;j<nodes;j++)
			if (memcmp (addrs[j],addrs[i],10) < 0)
			{
				byte t[10];
				int  s[4];
				memcpy (t,addrs[i],10); memcpy (addrs[i],addrs[j],10); memcpy (addrs[j],t,10);
				memcpy (s,settings[i],sizeof(s)); memcpy (settings[i],settings[j],sizeof(s)); memcpy (settings[j],s,sizeof(s));
			}

	for (i=0;i<nodes;i++)
	{
		memcpy (np[i].addr,addrs[i],10);
		if (!memcmp (addrs[i],myaddr,10))
			localplayer = i;
	}

	hello_episode = settings[0][0];
	hello_level = settings[0][1];
	hello_skill = settings[0][2];
	hello_ff = settings[0][3];

	printf ("You are player %d of %d: episode %d, level %d, skill %d, friendly fire %s\n",
		localplayer+1,nodes,hello_episode+1,hello_level+1,hello_skill,hello_ff ? "on" : "off");
}


/*
=============================================================================

						 GAME

=============================================================================
*/

/*
===================
=
= NetGameLoop
=
===================
*/

void NetGameLoop (void)
{
	int i;

	NewGame (hello_skill,hello_episode);
	gamestate.mapon = hello_level;
	startgame = false;

	netcontext = localplayer;
	for (i=0;i<numplayers;i++)
	{
		SaveContext (&np[i]);
		np[i].ob = &objlist[i];
	}

	GameLoop ();

	if (desyncframe != -1)
	{
		char str[80];
		sprintf (str,"The games went out of sync at frame %ld",desyncframe);
		Quit (str);
	}
	Quit (NULL);
}


/*
===================
=
= NetLevelStart
=
= SetupGameLevel spawned player 0 at the start spot; put the others on the
= nearest free tiles
=
===================
*/

void NetLevelStart (void)
{
	static	byte	queue[MAPSIZE*MAPSIZE][2];
	static	byte	seen[MAPSIZE][MAPSIZE];
	int		head,tail,x,y,d,i,placed;
	unsigned area;
	objtype	*p0;
	static int dx[4] = {1,0,-1,0}, dy[4] = {0,-1,0,1};

	if (!rstried)
		LoadRespawnSound ();	// now, not in the middle of a fight

	player = np[netcontext].ob;
	p0 = playerobjs[0];

	memset (seen,0,sizeof(seen));
	head = tail = 0;
	queue[tail][0] = p0->tilex;
	queue[tail][1] = p0->tiley;
	tail++;
	seen[p0->tilex][p0->tiley] = 1;
	placed = 1;

	while (head < tail && placed < numplayers)
	{
		x = queue[head][0];
		y = queue[head][1];
		head++;

		if (head > 1)
		{
			NetSetContext (placed);
			SpawnPlayer (x,y,0);
			player->angle = p0->angle;
			placed++;
		}

		for (d=0;d<4;d++)
		{
			int nx = x+dx[d], ny = y+dy[d];
			if (nx < 1 || ny < 1 || nx > MAPSIZE-2 || ny > MAPSIZE-2 || seen[nx][ny])
				continue;
			seen[nx][ny] = 1;
			if (tilemap[nx][ny] || actorat[nx][ny])
				continue;
			area = *(mapsegs[0]+farmapylookup[ny]+nx);
			if (area < AREATILE || area >= AREATILE+NUMAREAS)
				continue;
			queue[tail][0] = nx;
			queue[tail][1] = ny;
			tail++;
		}
	}
	for ( ; placed < numplayers ; placed++)	// no room: stack them up
	{
		NetSetContext (placed);
		SpawnPlayer (p0->tilex,p0->tiley,0);
		player->angle = p0->angle;
	}

	for (i=0;i<numplayers;i++)
	{
		NetSetContext (i);
		if (np[i].dead || gamestate.health <= 0)
		{
			gamestate.health = 100;
			if (gamestate.ammo < STARTAMMO)
				gamestate.ammo = STARTAMMO;
			gamestate.weapon = gamestate.chosenweapon;
		}
		np[i].dead = np[i].removed;
		if (np[i].removed)
			player->state = &s_netdead;
		np[i].respawn = 0;
		np[i].spawnx = player->x;
		np[i].spawny = player->y;
		np[i].spawnangle = player->angle;

		gamestate.keys = 0;
#ifdef SPEAR
		if (gamestate.mapon == 20)
			gamestate.keys |= 1;
#endif
		gamestate.attackframe = gamestate.weaponframe = 0;
		thrustspeed = 0;
		anglefrac = 0;
		facecount = 0;
		gotgatgun = 0;
		controlx = controly = 0;
		memset (buttonstate,0,sizeof(buttonstate));
		memset (buttonheld,0,sizeof(buttonheld));
		plux = player->x >> UNSIGNEDSHIFT;
		pluy = player->y >> UNSIGNEDSHIFT;
	}
	NetSetContext (localplayer);
	ConnectAreas ();
}


/*
===================
=
= SampleControls
=
= The local controls for one frame, as PollControls would read them
=
===================
*/

static void SampleControls (netcmd_t *cmd)
{
	int		i,max,savex,savey;
	byte	bits;
	boolean	savestate[NUMBUTTONS];

	savex = controlx;
	savey = controly;
	memcpy (savestate,buttonstate,sizeof(savestate));
	controlx = controly = 0;
	memset (buttonstate,0,sizeof(buttonstate));

	PollKeyboardButtons ();
	if (mouseenabled)
		PollMouseButtons ();
	if (joystickenabled)
		PollJoystickButtons ();

	PollKeyboardMove ();
	if (mouseenabled)
		PollMouseMove ();
	if (joystickenabled)
		PollJoystickMove ();

	max = 100*tics;
	if (controlx > max) controlx = max;
	if (controlx < -max) controlx = -max;
	if (controly > max) controly = max;
	if (controly < -max) controly = -max;

	bits = 0;
	for (i=NUMBUTTONS-1;i>=0;i--)
	{
		bits <<= 1;
		if (buttonstate[i])
			bits |= 1;
	}
	cmd->buttons = bits;
	cmd->x = controlx;
	cmd->y = controly;
	if (chatting)					// the keys are for the message
		memset (cmd,0,sizeof(*cmd));

	controlx = savex;
	controly = savey;
	memcpy (buttonstate,savestate,sizeof(savestate));
}


static boolean HaveCmd (int p, long f)
{
	if (f < NETDELAY)
		return true;
	if (np[p].gone && f > np[p].lastframe)
		return true;
	return np[p].cmdframe[f & (NETRING-1)] == f;
}

static void ApplyCmd (int p, long f)
{
	netcmd_t	*cmd,zero;
	byte	bits;
	int		i;

	memset (&zero,0,sizeof(zero));
	if (f < NETDELAY || (np[p].gone && f > np[p].lastframe))
		cmd = &zero;
	else
		cmd = &np[p].cmds[f & (NETRING-1)];

	memcpy (buttonheld,buttonstate,sizeof(buttonstate));
	bits = cmd->buttons;
	for (i=0;i<NUMBUTTONS;i++)
	{
		buttonstate[i] = bits&1;
		bits >>= 1;
	}
	controlx = cmd->x;
	controly = cmd->y;
}


static void ShowMessage (char *str)
{
	ClearSplitVWB ();
	VW_ScreenToScreen (displayofs,bufferofs,80,160);
	WindowH = 160;
	Message (str);
	SETFONTCOLOR(0,15);
}


/*
===================
=
= WaitForFrame
=
= Until everyone's controls for frame f are in
=
===================
*/

static void WaitForFrame (long f)
{
	long	start,lastsend;
	int		i;
	boolean	shown;
	char	str[80];

	start = lastsend = TimeCount;
	shown = false;

	while (1)
	{
		NetReceive ();
		for (i=0;i<numplayers;i++)
			if (!HaveCmd (i,f))
				break;
		if (i == numplayers)
			break;

		if (TimeCount - lastsend >= 7)
		{
			SendTics (pk_tics);
			lastsend = TimeCount;
		}
		if (!shown && TimeCount - start > 70)
		{
			sprintf (str,"Waiting for player %d...\n(Esc leaves the game)",i+1);
			ShowMessage (str);
			shown = true;
		}
		if (shown && Keyboard[sc_Escape])
		{
			SendTics (pk_quit);
			SendTics (pk_quit);
			Quit (NULL);
		}
		if (TimeCount - start > 70*30)
		{
			sprintf (str,"Lost contact with player %d",i+1);
			Quit (str);
		}
	}

	if (shown)
	{
		DrawAllPlayBorder ();
		lasttimecount = TimeCount;
	}
}


/*
===================
=
= ConsistencyHash
=
===================
*/

static unsigned long ConsistencyHash (void)
{
	unsigned long h;
	objtype	*ob;
	int		i;

	h = US_RndIndex () + objcount*31 + gamestate.killcount*977;
	for (i=0;i<numplayers;i++)
		h = h*31 + GetHealth (i);
	for (ob = playerobjs[0] ; ob ; ob = ob->next)
		h = h*31 + ob->x + ob->y*7 + ob->angle*13 + ob->obclass;
	return h;
}


/*
===================
=
= RunFrame
=
===================
*/

static void RunFrame (long f)
{
	int		i;
	long	oldx,oldy;

	for (i=0;i<numplayers;i++)
	{
		NetSetContext (i);
		ApplyCmd (i,f);
		if (np[i].gone && f > np[i].lastframe && !np[i].removed)
		{
			np[i].removed = np[i].dead = true;
			player->state = &s_netdead;
		}
	}

	madenoise = false;

	NetSetContext (0);
	MoveDoors ();
	MovePWalls ();

	for (obj = playerobjs[0] ; obj ; obj = obj->next)
	{
		if (obj->obclass == playerobj)
		{
			for (i=0;i<numplayers;i++)
				if (np[i].ob == obj)
					break;
			if (i == numplayers)
				continue;
			NetSetContext (i);
			oldx = obj->x;
			oldy = obj->y;
			DoActor (obj);
			if (np[i].pain)
				np[i].pain--;
			if (!np[i].dead)
			{
				NetPickups (obj);
				if (obj->x != oldx || obj->y != oldy)
				{
					np[i].anim += labs(obj->x-oldx) + labs(obj->y-oldy);
					np[i].moving = 4;
				}
				else if (np[i].moving)
					np[i].moving--;
			}
		}
		else
		{
			NetSetContext (NetTarget (obj));
			DoActor (obj);
		}
	}

	gamestate.TimeCount += tics;
	NetSetContext (localplayer);

	hashring[f & (NETRING-1)] = ConsistencyHash ();
	hashframes[f & (NETRING-1)] = f;

	if (netlog)
	{
		fprintf (netlog,"%d %ld %08lx rnd %d",levelseq,f,hashring[f & (NETRING-1)],US_RndIndex ());
		for (i=0;i<numplayers;i++)
		{
			netcmd_t *c = &np[i].cmds[f & (NETRING-1)];
			fprintf (netlog," | %lx %lx %d %d c%02x,%d,%d",playerobjs[i]->x,playerobjs[i]->y,
				playerobjs[i]->angle,GetHealth (i),HaveCmd(i,f) && f>=NETDELAY ? c->buttons : 0,
				f>=NETDELAY ? c->x : 0, f>=NETDELAY ? c->y : 0);
		}
		fprintf (netlog,"\n");
	}
}


/*
===================
=
= NetCheckKeys
=
===================
*/

static void NetCheckKeys (void)
{
	if (Paused)
		Paused = false;

	if (screenfaded)
		return;

	if (ChatKeys ())
		return;

	if (AM_Keys (LastScan))
		LastScan = sc_None;

	if (LastScan == sc_Escape || LastScan == sc_F10)
	{
		long lastsend = TimeCount;

		LastScan = 0;
		ShowMessage ("Leave the network game?\nY to leave, N to play on");
		IN_ClearKeysDown ();
		while (1)
		{
			NetReceive ();
			if (TimeCount - lastsend >= 7)
			{
				SendTics (pk_tics);
				lastsend = TimeCount;
			}
			if (Keyboard[sc_Y])
			{
				SendTics (pk_quit);
				SendTics (pk_quit);
				SendTics (pk_quit);
				Quit (NULL);
			}
			if (Keyboard[sc_N] || Keyboard[sc_Escape])
				break;
		}
		IN_ClearKeysDown ();
		DrawAllPlayBorder ();
		lasttimecount = TimeCount;
		if (MousePresent)
			Mouse(MDelta);
	}
}


/*
===================
=
= NetPlayLoop
=
===================
*/

void NetPlayLoop (void)
{
	int		i,f;
	long	start,lastsend;

	levelseq++;
	netframe = 0;
	maketic = NETDELAY;
	for (i=0;i<numplayers;i++)
	{
		for (f=0;f<NETRING;f++)
			np[i].cmdframe[f] = -1;
		np[i].ack = 0;
	}
	for (f=0;f<NETRING;f++)
		hashframes[f] = -1;

	playstate = TimeCount = lasttimecount = 0;
	frameon = 0;
	ClearPaletteShifts ();
	if (MousePresent)
		Mouse(MDelta);

	do
	{
		//
		// one frame every NETTICS tics
		//
		while (TimeCount < lasttimecount+NETTICS)
			NetReceive ();
		lasttimecount += NETTICS;
		if (TimeCount - lasttimecount > 10*NETTICS)
			lasttimecount = TimeCount;		// way behind: don't race to catch up
		tics = NETTICS;

		//
		// our controls for a later frame
		//
		SampleControls (&np[localplayer].cmds[maketic & (NETRING-1)]);
		np[localplayer].cmdframe[maketic & (NETRING-1)] = maketic;
		maketic++;
		SendTics (pk_tics);

		//
		// everyone's for this one
		//
		NetReceive ();
		WaitForFrame (netframe);
		RunFrame (netframe);
		netframe++;

		//
		// show it
		//
		UpdatePaletteShifts ();
		ThreeDRefresh ();

		SD_Poll ();
		UpdateSoundLoc ();

		if (screenfaded)
			VW_FadeIn ();

		NetCheckKeys ();
	} while (!playstate);

	//
	// don't leave while somebody might still need our controls
	//
	start = lastsend = TimeCount;
	SendTics (pk_tics);
	while (TimeCount - start < 70*5)
	{
		NetReceive ();
		for (i=0;i<numplayers;i++)
			if (i != localplayer && !np[i].gone && np[i].ack < netframe)
				break;
		if (i == numplayers)
			break;
		if (TimeCount - lastsend >= 7)
		{
			SendTics (pk_tics);
			lastsend = TimeCount;
		}
	}
	oldseq = levelseq;
	oldack = netframe;

	FinishPaletteShifts ();
}
