/*
 * SETUP.EXE / IPXSETUP.EXE for the DJGPP Wolfenstein 3-D
 *
 * Blue DOS screens like the setup programs of the time.  It edits the
 * CONFIG file the game reads (CONFIG.WL6, .WL1, .SOD or .SDM, whichever
 * data is in the current directory) and starts co-op games over IPX.
 *
 *   setup            the menus
 *   ipxsetup         straight to the network game menu
 *   ipxsetup -nodes 3 -level 2 ...   no menu: runs the game with them
 *
 * The CONFIG layout is the original's: the high score table, then
 * 16-bit sound, control and view settings.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <process.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>

/* ------------------------------------------------------------------ */
/* the CONFIG file                                                     */

#define MAXSCORES	7

#pragma pack(push,1)
typedef struct
{
	char			name[58];
	int32_t			score;
	uint16_t		completed,episode;
} highscore_t;

typedef struct
{
	highscore_t		scores[MAXSCORES];
	int16_t			sd,sm,sds;
	int16_t			mouseenabled,joystickenabled,joypadenabled;
	int16_t			joystickprogressive,joystickport;
	int16_t			dirscan[4];
	int16_t			buttonscan[8];
	int16_t			buttonmouse[4];
	int16_t			buttonjoy[4];
	int16_t			viewsize,mouseadjustment;
} config_t;
#pragma pack(pop)

static config_t	cfg;
static char		cfgname[16] = "CONFIG.WL6";
static char		gameexe[16] = "WOLF3D.EXE";
static char		dataext[4] = "WL6";
static int		episodes = 6,levels = 10;
static int		haveconfig;
static int		dirty;

static const highscore_t defscores[MAXSCORES] =
{
	{"id software-'92",10000,1,0},
	{"Adrian Carmack",10000,1,0},
	{"John Carmack",10000,1,0},
	{"Kevin Cloud",10000,1,0},
	{"Tom Hall",10000,1,0},
	{"John Romero",10000,1,0},
	{"Jay Wilbur",10000,1,0},
};

static int Exists (const char *name)
{
	return access (name,R_OK) == 0;
}

static void FindGame (void)
{
	static const struct { const char *ext,*exe; int episodes,levels; } games[] =
	{
		{"WL6","WOLF3D.EXE",6,10},
		{"WL1","WOLF3D.EXE",1,10},
		{"SOD","SPEAR.EXE",1,21},
		{"SDM","SPEAR.EXE",1,2},
	};
	char	name[16];
	int		i;

	for (i=0;i<4;i++)
	{
		sprintf (name,"VSWAP.%s",games[i].ext);
		if (Exists (name))
			break;
	}
	if (i == 4)
		i = 0;
	strcpy (dataext,games[i].ext);
	strcpy (gameexe,games[i].exe);
	sprintf (cfgname,"CONFIG.%s",dataext);
	episodes = games[i].episodes;
	levels = games[i].levels;
}

static void DefaultConfig (void)
{
	static const int16_t dirs[4] = {0x48,0x4d,0x50,0x4b};
	static const int16_t buttons[8] = {0x1d,0x38,0x36,0x39,0x02,0x03,0x04,0x05};
	static const int16_t mouse[4] = {0,1,3,-1};
	static const int16_t joy[4] = {0,1,3,2};

	memset (&cfg,0,sizeof(cfg));
	memcpy (cfg.scores,defscores,sizeof(defscores));
	cfg.sd = 2;							// AdLib
	cfg.sm = 1;							// AdLib
	cfg.sds = getenv ("BLASTER") || getenv ("ULTRASND") ? 3 : 0;
	cfg.mouseenabled = 1;
	memcpy (cfg.dirscan,dirs,sizeof(dirs));
	memcpy (cfg.buttonscan,buttons,sizeof(buttons));
	memcpy (cfg.buttonmouse,mouse,sizeof(mouse));
	memcpy (cfg.buttonjoy,joy,sizeof(joy));
	cfg.viewsize = 15;
	cfg.mouseadjustment = 5;
}

static void LoadConfig (void)
{
	FILE *f = fopen (cfgname,"rb");

	DefaultConfig ();
	haveconfig = 0;
	if (!f)
		return;
	if (fread (&cfg,1,sizeof(cfg),f) == sizeof(cfg))
		haveconfig = 1;
	else
		DefaultConfig ();
	fclose (f);

	if (cfg.sd < 0 || cfg.sd > 2) cfg.sd = 1;
	if (cfg.sm < 0 || cfg.sm > 1) cfg.sm = 0;
	if (cfg.sds < 0 || cfg.sds > 4) cfg.sds = 0;
	if (cfg.viewsize < 4 || cfg.viewsize > 20) cfg.viewsize = 15;
	if (cfg.mouseadjustment < 0 || cfg.mouseadjustment > 9) cfg.mouseadjustment = 5;
	if (cfg.joystickport < 0 || cfg.joystickport > 1) cfg.joystickport = 0;
}

static int SaveConfig (void)
{
	FILE *f = fopen (cfgname,"wb");

	if (!f)
		return 0;
	fwrite (&cfg,1,sizeof(cfg),f);
	fclose (f);
	dirty = 0;
	haveconfig = 1;
	return 1;
}

/* network game settings, kept in WOLFNET.CFG */
static int		net_nodes = 2,net_episode = 1,net_level = 1,net_skill = 2,net_ff = 1;

static void LoadNet (void)
{
	FILE *f = fopen ("WOLFNET.CFG","r");

	if (!f)
		return;
	fscanf (f,"%d %d %d %d %d",&net_nodes,&net_episode,&net_level,&net_skill,&net_ff);
	fclose (f);
	if (net_nodes < 2 || net_nodes > 4) net_nodes = 2;
	if (net_episode < 1 || net_episode > episodes) net_episode = 1;
	if (net_level < 1 || net_level > levels) net_level = 1;
	if (net_skill < 0 || net_skill > 3) net_skill = 2;
	net_ff = !!net_ff;
}

static void SaveNet (void)
{
	FILE *f = fopen ("WOLFNET.CFG","w");

	if (!f)
		return;
	fprintf (f,"%d %d %d %d %d\n",net_nodes,net_episode,net_level,net_skill,net_ff);
	fclose (f);
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */

#define TEXTMEM		0xb8000
#define BLUE		0x1f			// white on blue
#define DIM			0x17			// grey on blue
#define HILITE		0x70			// black on grey
#define TITLE		0x30			// black on cyan
#define YELLOW		0x1e
#define BOX			0x1b			// cyan on blue

static void Put (int x, int y, const char *s, int attr)
{
	unsigned long a = TEXTMEM + (y*80+x)*2;

	for ( ; *s && x < 80 ; s++,x++,a+=2)
		_farpokew (_dos_ds,a,(unsigned char)*s | (attr<<8));
}

static void Fill (int x, int y, int w, int h, int ch, int attr)
{
	int i,j;

	for (j=0;j<h;j++)
		for (i=0;i<w;i++)
			_farpokew (_dos_ds,TEXTMEM+((y+j)*80+x+i)*2,ch | (attr<<8));
}

static void Box (int x, int y, int w, int h, int attr, const char *title)
{
	int i;

	Fill (x,y,w,h,' ',attr);
	for (i=1;i<w-1;i++)
	{
		Fill (x+i,y,1,1,0xcd,attr);
		Fill (x+i,y+h-1,1,1,0xcd,attr);
	}
	for (i=1;i<h-1;i++)
	{
		Fill (x,y+i,1,1,0xba,attr);
		Fill (x+w-1,y+i,1,1,0xba,attr);
	}
	Fill (x,y,1,1,0xc9,attr);
	Fill (x+w-1,y,1,1,0xbb,attr);
	Fill (x,y+h-1,1,1,0xc8,attr);
	Fill (x+w-1,y+h-1,1,1,0xbc,attr);
	if (title)
		Put (x+(w-strlen(title)-2)/2,y," ",attr),
		Put (x+(w-strlen(title))/2,y,title,attr),
		Put (x+(w+strlen(title))/2,y," ",attr);
	// shadow
	for (i=0;i<h;i++)
		if (x+w < 80)
			Fill (x+w,y+1+i < 25 ? y+1+i : 24,1,1,' ',0x08);
	for (i=1;i<=w;i++)
		if (y+h < 25)
			Fill (x+i,y+h,1,1,' ',0x08);
}

static void Cursor (int on)
{
	__dpmi_regs r;

	memset (&r,0,sizeof(r));
	r.h.ah = 1;
	r.x.cx = on ? 0x0607 : 0x2000;
	__dpmi_int (0x10,&r);
}

static void Background (const char *help)
{
	char title[80];

	Fill (0,0,80,25,0xb1,0x19);
	Fill (0,0,80,1,' ',TITLE);
	sprintf (title,"%s Setup",strcmp (dataext,"SOD") && strcmp (dataext,"SDM") ?
		"Wolfenstein 3-D" : "Spear of Destiny");
	Put ((80-strlen(title))/2,0,title,TITLE);
	Put (79-strlen(cfgname)-1,0,cfgname,TITLE);
	Fill (0,24,80,1,' ',TITLE);
	Put (1,24,help,TITLE);
}

/* ------------------------------------------------------------------ */
/* the keyboard (BIOS)                                                 */

static int GetKey (void)			// scan<<8 | ascii
{
	__dpmi_regs r;

	memset (&r,0,sizeof(r));
	r.h.ah = 0x10;
	__dpmi_int (0x16,&r);
	return r.x.ax;
}

static int KeyWaiting (void)
{
	__dpmi_regs r;

	memset (&r,0,sizeof(r));
	r.h.ah = 0x11;
	__dpmi_int (0x16,&r);
	return !(r.x.flags & 0x40);
}

#define K_UP	0x48
#define K_DOWN	0x50
#define K_LEFT	0x4b
#define K_RIGHT	0x4d
#define K_ENTER	0x1c
#define K_ESC	0x01
#define K_PGUP	0x49
#define K_PGDN	0x51
#define K_HOME	0x47
#define K_END	0x4f
#define K_DEL	0x53

static int Scan (int key)
{
	int scan = key >> 8;

	if (scan == 0xe0 || !scan)
		return 0;
	if (scan == 0x9c)				// keypad Enter (extended read)
		return K_ENTER;
	if (scan == 0x48 || scan == 0x50 || scan == 0x4b || scan == 0x4d)
		return scan;
	return scan;
}

/*
 * ReadGameKey: a key for the game's controls, as the scan code the game's
 * keyboard handler sees.  Shift, Ctrl and Alt make no keystroke for the
 * BIOS, so they're read from its shift flags.
 */
static int ReadGameKey (void)
{
	int flags,old,key;

	old = _farpeekb (_dos_ds,0x417) & 0x0f;
	for (;;)
	{
		if (KeyWaiting ())
		{
			key = GetKey ();
			if ((key >> 8) == K_ESC)
				return -1;
			if ((key >> 8) == 0x9c)
				return 0x1c;
			if ((key & 0xff) == 0xe0)
				return key >> 8;		// grey keys: the same scan code
			return key >> 8;
		}
		flags = _farpeekb (_dos_ds,0x417) & 0x0f;
		if (flags & ~old & 1) return 0x36;	// right shift
		if (flags & ~old & 2) return 0x2a;	// left shift
		if (flags & ~old & 4) return 0x1d;	// ctrl
		if (flags & ~old & 8) return 0x38;	// alt
		old = flags;
		__dpmi_yield ();
	}
}

static const char *KeyName (int scan)
{
	static const char *names[0x60] =
	{
		"?","Esc","1","2","3","4","5","6","7","8","9","0","-","=","Backspace","Tab",
		"Q","W","E","R","T","Y","U","I","O","P","[","]","Enter","Ctrl","A","S",
		"D","F","G","H","J","K","L",";","'","`","Left Shift","\\","Z","X","C","V",
		"B","N","M",",",".","/","Right Shift","Keypad *","Alt","Space","Caps Lock","F1","F2","F3","F4","F5",
		"F6","F7","F8","F9","F10","Num Lock","Scroll Lock","Home","Up","Page Up","Keypad -","Left","Keypad 5","Right","Keypad +","End",
		"Down","Page Down","Insert","Delete","?","?","?","F11","F12","?","?","?","?","?","?","?",
	};
	static char buf[8];

	if (scan > 0 && scan < 0x60)
		return names[scan];
	if (scan <= 0)
		return "(none)";
	sprintf (buf,"%02Xh",scan);
	return buf;
}

/* ------------------------------------------------------------------ */
/* forms: a list of fields, Up/Down to pick, Left/Right to change      */

enum {F_CHOICE,F_NUMBER,F_KEY,F_ACTION,F_TEXT};

typedef struct
{
	int			type;
	const char	*label;
	int16_t		*value;			// F_CHOICE / F_NUMBER / F_KEY
	int			min,max;		// F_CHOICE: max = number of choices - 1
	const char	**choices;
	int			action;			// F_ACTION: returned by Form
	int			*ivalue;		// instead of value
} field_t;

static int GetValue (field_t *f)
{
	return f->ivalue ? *f->ivalue : *f->value;
}

static void SetValue (field_t *f, int v)
{
	if (f->ivalue)
		*f->ivalue = v;
	else
		*f->value = v;
	dirty = 1;
}

static void ValueText (field_t *f, char *out)
{
	int v;

	switch (f->type)
	{
	case F_CHOICE:
		v = GetValue (f);
		if (v < f->min || v > f->max)
			v = f->min;
		strcpy (out,f->choices[v-f->min]);
		break;
	case F_NUMBER:
		sprintf (out,"%d",GetValue (f));
		break;
	case F_KEY:
		strcpy (out,KeyName (GetValue (f)));
		break;
	default:
		out[0] = 0;
	}
}

/*
 * Form: returns the action of the F_ACTION picked with Enter, or -1 for Esc
 */
static int Form (const char *title, field_t *fields, int count, int *cursor,
	const char *note)
{
	int		i,w,h,x,y,key,scan,lw,vw,sel,v;
	char	val[40],line[80];
	field_t	*f;

	lw = vw = 0;
	for (i=0;i<count;i++)
	{
		if ((int)strlen (fields[i].label) > lw)
			lw = strlen (fields[i].label);
		if (fields[i].type == F_CHOICE)
		{
			int j;
			for (j=0;j<=fields[i].max-fields[i].min;j++)
				if ((int)strlen (fields[i].choices[j]) > vw)
					vw = strlen (fields[i].choices[j]);
		}
	}
	if (vw < 14)
		vw = 14;
	w = lw + vw + 9;
	if (w < (int)strlen (title)+8)
		w = strlen (title)+8;
	if (note && w < (int)strlen (note)+6)
		w = strlen (note)+6;
	h = count + 4 + (note ? 2 : 0);
	x = (80-w)/2;
	y = (25-h)/2;
	sel = *cursor;
	if (sel >= count)
		sel = 0;

	Background ("\x18\x19 choose   \x1b\x1a change   Enter select   Esc back");
	for (;;)
	{
		Box (x,y,w,h,BOX,title);
		for (i=0;i<count;i++)
		{
			f = &fields[i];
			if (f->type == F_TEXT)
			{
				Put (x+3,y+2+i,f->label,DIM);
				continue;
			}
			ValueText (f,val);
			if (f->type == F_ACTION)
				sprintf (line," %-*s ",lw+vw+2,f->label);
			else
				sprintf (line," %-*s  %-*s ",lw,f->label,vw,val);
			Put (x+2,y+2+i,line,i == sel ? HILITE : (f->type == F_ACTION ? YELLOW : BLUE));
		}
		if (note)
			Put (x+3,y+h-2,note,DIM);

		key = GetKey ();
		scan = Scan (key);
		f = &fields[sel];

		switch (scan)
		{
		case K_ESC:
			*cursor = sel;
			return -1;
		case K_UP:
			do sel = (sel+count-1)%count; while (fields[sel].type == F_TEXT);
			break;
		case K_DOWN:
			do sel = (sel+1)%count; while (fields[sel].type == F_TEXT);
			break;
		case K_HOME:
			for (sel=0;fields[sel].type == F_TEXT;sel++);
			break;
		case K_END:
			for (sel=count-1;fields[sel].type == F_TEXT;sel--);
			break;
		case K_LEFT:
		case K_RIGHT:
			if (f->type == F_CHOICE || f->type == F_NUMBER)
			{
				v = GetValue (f) + (scan == K_RIGHT ? 1 : -1);
				if (v > f->max) v = f->type == F_CHOICE ? f->min : f->max;
				if (v < f->min) v = f->type == F_CHOICE ? f->max : f->min;
				SetValue (f,v);
			}
			break;
		case K_DEL:
			if (f->type == F_KEY)
				SetValue (f,0);
			break;
		case K_ENTER:
			if (f->type == F_ACTION)
			{
				*cursor = sel;
				return f->action;
			}
			if (f->type == F_KEY)
			{
				int k,j;
				Box (x+w/2-16,y+h/2-1,32,3,0x4f,NULL);
				Put (x+w/2-13,y+h/2,"Press a key (Esc cancels)",0x4f);
				k = ReadGameKey ();
				if (k > 0)
				{
					// another control had it: swap
					for (j=0;j<count;j++)
						if (j != sel && fields[j].type == F_KEY && GetValue (&fields[j]) == k)
							SetValue (&fields[j],GetValue (f));
					SetValue (f,k);
				}
				Background ("\x18\x19 choose   \x1b\x1a change   Enter select   Esc back");
			}
			else if (f->type == F_CHOICE || f->type == F_NUMBER)
			{
				v = GetValue (f) + 1;
				if (v > f->max) v = f->min;
				SetValue (f,v);
			}
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* the screens                                                         */

static const char *sfxnames[] = {"Off","PC Speaker","AdLib / SB / UltraSound"};
static const char *musicnames[] = {"Off","AdLib / SB / UltraSound"};
static const char *diginames[] = {"Off","PC Speaker","Sound Source","Sound Blaster / UltraSound"};
static const char *onoff[] = {"Off","On"};
static const char *mbuttons[] = {"(nothing)","Fire","Strafe","Run","Open / Use"};
static const char *joynames[] = {"Off","Port 1","Port 2"};
static const char *skillnames[] =
	{"Can I play, Daddy?","Don't hurt me.","Bring 'em on!","I am Death incarnate!"};

static void SoundScreen (void)
{
	static int cursor;
	char	note[64];
	field_t	f[] =
	{
		{F_CHOICE,"Sound effects",&cfg.sd,0,2,sfxnames},
		{F_CHOICE,"Music",&cfg.sm,0,1,musicnames},
		{F_CHOICE,"Digitized sound",&cfg.sds,0,3,diginames},
	};

	if (getenv ("ULTRASND"))
		sprintf (note,"ULTRASND=%.30s: the UltraSound is used",getenv ("ULTRASND"));
	else
		sprintf (note,"BLASTER=%.40s",getenv ("BLASTER") ? getenv ("BLASTER") : "(not set)");
	while (Form ("Sound",f,3,&cursor,note) != -1)
		;
}

static void KeyboardScreen (void)
{
	static int cursor;
	field_t	f[] =
	{
		{F_KEY,"Move forward",&cfg.dirscan[0]},
		{F_KEY,"Move backward",&cfg.dirscan[2]},
		{F_KEY,"Turn left",&cfg.dirscan[3]},
		{F_KEY,"Turn right",&cfg.dirscan[1]},
		{F_KEY,"Fire",&cfg.buttonscan[0]},
		{F_KEY,"Strafe",&cfg.buttonscan[1]},
		{F_KEY,"Run",&cfg.buttonscan[2]},
		{F_KEY,"Open / Use",&cfg.buttonscan[3]},
		{F_KEY,"Knife",&cfg.buttonscan[4]},
		{F_KEY,"Pistol",&cfg.buttonscan[5]},
		{F_KEY,"Machine gun",&cfg.buttonscan[6]},
		{F_KEY,"Chain gun",&cfg.buttonscan[7]},
	};

	while (Form ("Keyboard",f,12,&cursor,"Enter, then the key. Del clears.") != -1)
		;
}

static void MouseScreen (void)
{
	static int cursor;
	int		joy,i;
	int16_t	mb[3],jb[4];
	field_t	f[] =
	{
		{F_CHOICE,"Mouse",&cfg.mouseenabled,0,1,onoff},
		{F_NUMBER,"Mouse sensitivity",&cfg.mouseadjustment,0,9},
		{F_CHOICE,"Left button",&mb[0],0,4,mbuttons},
		{F_CHOICE,"Right button",&mb[1],0,4,mbuttons},
		{F_CHOICE,"Middle button",&mb[2],0,4,mbuttons},
		{F_TEXT,""},
		{F_CHOICE,"Joystick",0,0,2,joynames,0,&joy},
		{F_CHOICE,"Gravis GamePad",&cfg.joypadenabled,0,1,onoff},
		{F_CHOICE,"Progressive joystick",&cfg.joystickprogressive,0,1,onoff},
		{F_CHOICE,"Joystick button 1",&jb[0],0,4,mbuttons},
		{F_CHOICE,"Joystick button 2",&jb[1],0,4,mbuttons},
		{F_CHOICE,"Joystick button 3",&jb[2],0,4,mbuttons},
		{F_CHOICE,"Joystick button 4",&jb[3],0,4,mbuttons},
	};

	// the game's button values are bt_nobutton (-1), attack, strafe, run, use
	for (i=0;i<3;i++)
		mb[i] = cfg.buttonmouse[i]+1;
	for (i=0;i<4;i++)
		jb[i] = cfg.buttonjoy[i]+1;
	joy = cfg.joystickenabled ? cfg.joystickport+1 : 0;

	while (Form ("Mouse and joystick",f,13,&cursor,NULL) != -1)
		;

	for (i=0;i<3;i++)
		cfg.buttonmouse[i] = mb[i]-1;
	for (i=0;i<4;i++)
		cfg.buttonjoy[i] = jb[i]-1;
	cfg.joystickenabled = joy != 0;
	if (joy)
		cfg.joystickport = joy-1;
}

static void DisplayScreen (void)
{
	static int cursor;
	field_t	f[] =
	{
		{F_NUMBER,"View size (4-20)",&cfg.viewsize,4,20},
	};

	while (Form ("Display",f,1,&cursor,"20 fills the screen: no border") != -1)
		;
}

static void RunGame (const char *const *args)
{
	const char *argv[24];
	int		n = 0,i;

	if (!Exists (gameexe))
	{
		Background ("");
		Box (16,10,48,5,0x4f,"Error");
		Put (19,12,gameexe,0x4f);
		Put (19+strlen (gameexe),12," isn't in this directory",0x4f);
		GetKey ();
		return;
	}

	argv[n++] = gameexe;
	for (i=0;args && args[i] && n < 23;i++)
		argv[n++] = args[i];
	argv[n] = NULL;

	Fill (0,0,80,25,' ',0x07);
	Cursor (1);
	{
		__dpmi_regs r;
		memset (&r,0,sizeof(r));
		r.h.ah = 2;					// cursor home
		__dpmi_int (0x10,&r);
	}
	execv (gameexe,(char *const *)argv);
	printf ("Couldn't run %s\n",gameexe);
	exit (1);
}

static void NetworkScreen (int standalone)
{
	static int cursor;
	char	nodes[4],ep[4],lv[4],sk[4];
	const char *args[16];
	int		n,act;
	field_t	f[] =
	{
		{F_NUMBER,"Players",0,2,4,0,0,&net_nodes},
		{F_NUMBER,"Episode",0,1,6,0,0,&net_episode},
		{F_NUMBER,"Level",0,1,21,0,0,&net_level},
		{F_CHOICE,"Skill",0,0,3,skillnames,0,&net_skill},
		{F_CHOICE,"Friendly fire",0,0,1,onoff,0,&net_ff},
		{F_TEXT,""},
		{F_ACTION,"Start the network game",0,0,0,0,1},
	};

	f[1].max = episodes;
	f[2].max = levels;

	for (;;)
	{
		act = Form ("Network game (IPX)",f,7,&cursor,
			"Everyone runs this; player 1's choices count.");
		if (act == -1)
		{
			SaveNet ();
			return;
		}

		SaveNet ();
		if (dirty && !standalone)
			SaveConfig ();
		n = 0;
		sprintf (nodes,"%d",net_nodes);
		sprintf (ep,"%d",net_episode);
		sprintf (lv,"%d",net_level);
		sprintf (sk,"%d",net_skill);
		args[n++] = "-nodes"; args[n++] = nodes;
		if (episodes > 1)
		{
			args[n++] = "-episode"; args[n++] = ep;
		}
		args[n++] = "-level"; args[n++] = lv;
		args[n++] = "-skill"; args[n++] = sk;
		if (!net_ff)
			args[n++] = "-nofriendlyfire";
		args[n] = NULL;
		RunGame (args);
	}
}

static int Confirm (const char *question)
{
	int key;

	Box (20,10,40,5,0x4f,NULL);
	Put (40-strlen (question)/2,11,question,0x4f);
	Put (31,13,"Y = yes, N = no",0x4f);
	for (;;)
	{
		key = toupper (GetKey () & 0xff);
		if (key == 'Y')
			return 1;
		if (key == 'N' || key == 27)
			return 0;
	}
}

static void Restore (void)
{
	Fill (0,0,80,25,' ',0x07);
	Cursor (1);
}

static void MainScreen (void)
{
	static int cursor;
	int		act;
	field_t	f[] =
	{
		{F_ACTION,"Sound",0,0,0,0,1},
		{F_ACTION,"Keyboard",0,0,0,0,2},
		{F_ACTION,"Mouse and joystick",0,0,0,0,3},
		{F_ACTION,"Display",0,0,0,0,4},
		{F_ACTION,"Network game (IPX co-op)",0,0,0,0,5},
		{F_TEXT,""},
		{F_ACTION,"Save settings and run the game",0,0,0,0,6},
		{F_ACTION,"Save settings and exit",0,0,0,0,7},
		{F_ACTION,"Exit without saving",0,0,0,0,8},
	};

	for (;;)
	{
		act = Form ("Main menu",f,9,&cursor,haveconfig ? NULL : "No CONFIG yet: these are the defaults.");
		switch (act)
		{
		case 1: SoundScreen (); break;
		case 2: KeyboardScreen (); break;
		case 3: MouseScreen (); break;
		case 4: DisplayScreen (); break;
		case 5: NetworkScreen (0); break;
		case 6:
			SaveConfig ();
			RunGame (NULL);
			break;
		case 7:
			SaveConfig ();
			Restore ();
			printf ("Settings saved to %s\n",cfgname);
			return;
		case -1:
			if (dirty && Confirm ("Save the changes?"))
				SaveConfig ();
			Restore ();
			return;
		case 8:
			Restore ();
			return;
		}
	}
}

int main (int argc, char **argv)
{
	const char *base;
	int		ipx,i;

	base = strrchr (argv[0],'/');
	if (!base) base = strrchr (argv[0],'\\');
	base = base ? base+1 : argv[0];
	ipx = !strnicmp (base,"ipxsetup",8);

	FindGame ();
	LoadConfig ();
	LoadNet ();

	for (i=1;i<argc;i++)
		if (!strcmp (argv[i],"/?") || !strcmp (argv[i],"-?") || !stricmp (argv[i],"-h")
		|| !stricmp (argv[i],"--help"))
		{
			printf ("%s: setup for %s (edits %s)\n\n",ipx ? "IPXSETUP" : "SETUP",gameexe,cfgname);
			printf ("  setup               the setup menus\n");
			printf ("  ipxsetup            the network game menu\n");
			printf ("  ipxsetup [options]  runs %s -nodes 2 [options] straight away\n",gameexe);
			printf ("                      (-nodes N, -episode N, -level N, -skill 0-3,\n");
			printf ("                       -nofriendlyfire, and any other game option)\n");
			return 0;
		}

	if (ipx && argc > 1)
	{
		// IPXSETUP.EXE style: the options go to the game
		const char *args[64];
		int n = 0,hasnodes = 0;

		for (i=1;i<argc && n < 60;i++)
		{
			if (!stricmp (argv[i],"-nodes"))
				hasnodes = 1;
			args[n++] = argv[i];
		}
		if (!hasnodes)
		{
			args[n++] = "-nodes";
			args[n++] = "2";
		}
		args[n] = NULL;
		RunGame (args);
		return 1;
	}

	Cursor (0);
	if (ipx)
	{
		NetworkScreen (1);
		Restore ();
	}
	else
		MainScreen ();
	return 0;
}
