//
//	ID_PM.C
//	Id Engine's Page Manager v1.0
//	Primary coder: Jason Blochowiak
//
//	32 bit DJGPP version: the page file (VSWAP) is read into memory once at
//	startup. The original juggled 4k pages between 640k, EMS and XMS with
//	an LRU scheme; with a flat address space every page is simply there, so
//	PM_GetPage is a table lookup and nothing is ever thrown out.
//

#include "id_heads.h"
#pragma hdrstop

//	Main Mem specific variables
	boolean			MainPresent;

//	EMS specific variables (always absent)
	boolean			EMSPresent;
	word			EMSAvail,EMSPagesAvail;

//	XMS specific variables (always absent)
	boolean			XMSPresent;
	word			XMSAvail,XMSPagesAvail;

//	File specific variables
	char			PageFileName[13] = {"VSWAP."};
	word			ChunksInFile;
	word			PMSpriteStart,PMSoundStart;

//	General usage variables
	boolean			PMStarted;
	word			MainPagesUsed,
					PMNumBlocks;
	long			PMFrameCount;
	PageListStruct	*PMPages;

static	byte		*PMFileData;		// the whole page file
static	long		PMFileSize;

/////////////////////////////////////////////////////////////////////////////
//
//	Main memory code
//
/////////////////////////////////////////////////////////////////////////////

//
//	PM_SetMainMemPurge() - Sets the purge level for all allocated main memory
//		blocks. Nothing is purged in this version.
//
void
PM_SetMainMemPurge(int level)
{
}

//
//	PM_CheckMainMem() - Makes sure that all allocated main memory blocks are
//		still there. They always are.
//
void
PM_CheckMainMem(void)
{
}

/////////////////////////////////////////////////////////////////////////////
//
//	File management code
//
/////////////////////////////////////////////////////////////////////////////

//
//	PML_OpenPageFile() - Reads in the whole page file and sets up the page
//		info
//
static void
PML_OpenPageFile(void)
{
	int				i,handle;
	byte			*p;
	PageListStruct	*page;
	char			error[80];

	handle = open(PageFileName,O_RDONLY | O_BINARY);
	if (handle == -1)
	{
		sprintf(error,"PML_OpenPageFile: Unable to open page file %s",PageFileName);
		Quit(error);
	}

	PMFileSize = filelength(handle);
	if (PMFileSize < 6)
		Quit("PML_OpenPageFile: Page file too short");

	PMFileData = malloc(PMFileSize + PMPageSize);	// slack for reads past the end
	if (!PMFileData)
		Quit("PML_OpenPageFile: Not enough memory for the page file");
	memset(PMFileData + PMFileSize,0,PMPageSize);

	if (read(handle,PMFileData,PMFileSize) != PMFileSize)
		Quit("PML_OpenPageFile: Read failed");
	close(handle);

	// Header variables
	p = PMFileData;
	ChunksInFile = p[0] | (p[1] << 8);
	PMSpriteStart = p[2] | (p[3] << 8);
	PMSoundStart = p[4] | (p[5] << 8);
	p += 6;

	if (6 + ChunksInFile * 6L > PMFileSize)
		Quit("PML_OpenPageFile: Bad page file header");

	// Allocate and clear the page list
	PMNumBlocks = ChunksInFile;
	PMPages = calloc(PMNumBlocks,sizeof(PageListStruct));
	if (!PMPages)
		Quit("PML_OpenPageFile: Not enough memory for the page list");

	// The chunk offsets, then the chunk lengths
	for (i = 0,page = PMPages;i < ChunksInFile;i++,page++,p += 4)
		page->offset = p[0] | (p[1] << 8) | ((longword)p[2] << 16) | ((longword)p[3] << 24);
	for (i = 0,page = PMPages;i < ChunksInFile;i++,page++,p += 2)
		page->length = p[0] | (p[1] << 8);

	for (i = 0,page = PMPages;i < ChunksInFile;i++,page++)
	{
		if (page->offset && page->offset + page->length > PMFileSize)
			Quit("PML_OpenPageFile: Page past the end of the file");
		page->data = page->offset ? PMFileData + page->offset : NULL;
	}
}

//
//  PML_ClosePageFile() - Frees the page file
//
static void
PML_ClosePageFile(void)
{
	free(PMPages);
	PMPages = NULL;
	free(PMFileData);
	PMFileData = NULL;
}

/////////////////////////////////////////////////////////////////////////////
//
//	Allocation, etc., code
//
/////////////////////////////////////////////////////////////////////////////

//
//	PM_GetPageAddress() - Returns the address of a given page
//		Returns nil if the page is sparse
//
memptr
PM_GetPageAddress(int pagenum)
{
	if ((unsigned)pagenum >= ChunksInFile)
		Quit("PM_GetPageAddress: Invalid page request");
	return PMPages[pagenum].data;
}

//
//	PM_GetPage() - Returns the address of the page
//
memptr
PM_GetPage(int pagenum)
{
	PageListStruct	*page;

	if ((unsigned)pagenum >= ChunksInFile)
		Quit("PM_GetPage: Invalid page request");

	page = &PMPages[pagenum];
	page->lastHit = PMFrameCount;
	return page->data;
}

//
//	PM_SetPageLock() - Sets the lock type on a given page
//
void
PM_SetPageLock(int pagenum,PMLockType lock)
{
	if (pagenum < PMSoundStart)
		Quit("PM_SetPageLock: Locking/unlocking non-sound page");

	PMPages[pagenum].locked = lock;
}

//
//	PM_Preload() - Loads as many pages as possible into all types of memory.
//		Everything is in memory already, so this just tells the update
//		routine it's done.
//
void
PM_Preload(boolean (*update)(word current,word total))
{
	update(1,1);
}

/////////////////////////////////////////////////////////////////////////////
//
//	General code
//
/////////////////////////////////////////////////////////////////////////////

//
//	PM_NextFrame() - Increments the frame counter
//
void
PM_NextFrame(void)
{
	int	i;

	if (++PMFrameCount >= LONG_MAX - 4)
	{
		for (i = 0;i < PMNumBlocks;i++)
			PMPages[i].lastHit = 0;
		PMFrameCount = 0;
	}
}

//
//	PM_Reset() - Sets up the page manager
//
void
PM_Reset(void)
{
	int				i;
	PageListStruct	*page;

	XMSPagesAvail = EMSPagesAvail = 0;
	MainPagesUsed = 0;

	for (i = 0,page = PMPages;i < PMNumBlocks;i++,page++)
	{
		page->mainPage = page->data ? i : -1;
		page->emsPage = -1;
		page->xmsPage = -1;
		page->locked = false;
		if (page->data)
			MainPagesUsed++;
	}
}

//
//	PM_Startup() - Start up the Page Mgr
//
void
PM_Startup(void)
{
	if (PMStarted)
		return;

	PML_OpenPageFile();

	MainPresent = true;
	EMSPresent = false;

	PM_Reset();

	// For the memory gauges of the signon screen only: the extended memory
	// DPMI gives us, shown as XMS
	XMSPresent = true;
	{
		long	pages = DOS32_MemoryFree() / PMPageSize;
		XMSPagesAvail = pages > 0xffff ? 0xffff : pages;
	}

	PMStarted = true;
}

//
//	PM_Shutdown() - Shut down the Page Mgr
//
void
PM_Shutdown(void)
{
	if (!PMStarted)
		return;

	PML_ClosePageFile();

	PMStarted = false;
}
