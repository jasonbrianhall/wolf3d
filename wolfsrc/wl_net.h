// WL_NET.H -- IPX co-op (lockstep), MS-DOS / DJGPP

#ifndef __WL_NET_H__
#define __WL_NET_H__

#define MAXPLAYERS	4

extern	boolean	netgame;			// true for a -nodes co-op game
extern	int		numplayers;			// 1 unless netgame
extern	int		localplayer;		// the one this machine controls and views
extern	int		netcontext;			// whose globals are loaded right now
extern	objtype	*playerobjs[MAXPLAYERS];

void	NetStartup (void);			// parses -nodes, finds the other players
void	NetShutdown (void);
void	NetGameLoop (void);			// runs the whole co-op game, never returns
void	NetLevelStart (void);		// after SetupGameLevel: spawn the others
void	NetPlayLoop (void);			// replaces PlayLoop in a netgame

void	NetSetContext (int p);		// load player p's state into the globals
boolean	NetIsLocal (void);			// drawing allowed (current context is local)
boolean	NetPlayerAlive (int p);
int		NetTarget (objtype *ob);	// player an enemy is after
void	NetKillPlayer (objtype *attacker);
boolean	NetCurrentDead (void);
boolean	NetLocalDead (void);
objtype	*NetFindTarget (objtype *shooter, boolean knife, long *dist);
boolean	NetCanSee (objtype *viewer, objtype *ob);
int		NetPlayerShape (objtype *ob);

#endif
