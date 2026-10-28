// VERSION.H
//
// Which game data this build is for. msdos/Makefile passes one of these
// (make VERSION=wl6 / wl6apo / wl1 / sod / sdm):
//
//   VER_WL6     Wolfenstein 3-D 1.4, GT / Activision / id release (.WL6).
//               This is the data sold today on Steam and GOG. (default)
//   VER_WL6APO  Wolfenstein 3-D 1.4, Apogee registered release (.WL6)
//   VER_WL1     Wolfenstein 3-D 1.4, Apogee shareware (.WL1)
//   VER_SOD     Spear of Destiny (.SOD)
//   VER_SDM     Spear of Destiny demo (.SDM)
//

#if !defined(VER_WL6) && !defined(VER_WL6APO) && !defined(VER_WL1) \
	&& !defined(VER_SOD) && !defined(VER_SDM)
#define VER_WL6
#endif

#ifdef VER_WL6
#define GOODTIMES
#endif

#ifdef VER_WL6APO
#define APOGEE
#endif

#ifdef VER_WL1
#define APOGEE
#define UPLOAD
#endif

#ifdef VER_SOD
#define SPEAR
#endif

#ifdef VER_SDM
#define SPEAR
#define SPEARDEMO
#endif

//#define JAPAN
#define ARTSEXTERN
#define DEMOSEXTERN
//#define MYPROFILE
//#define DEBCHECK
#define CARMACIZED
