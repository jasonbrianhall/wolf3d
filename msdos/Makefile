# Wolfenstein 3-D for MS-DOS / FreeDOS (DJGPP, 32-bit DPMI)
#
#   make            wolf3d.exe with CWSDPMI embedded (runs on its own)
#   make msdos_original
#                   wolf3d.exe that needs CWSDPMI.EXE next to it (or any
#                   other DPMI host: Windows 9x, HDPMI32, ...)
#   make setup      setup.exe and ipxsetup.exe (the same program; any data)
#   make run        run it in DOSBox with the game data (DATA=dir)
#   make clean
#
#   VERSION=wl6     (default) Wolfenstein 3-D 1.4, GT / Activision / id
#                   release: the .WL6 files sold on Steam and GOG
#   VERSION=wl6apo  Wolfenstein 3-D 1.4, Apogee registered release (.WL6)
#   VERSION=wl1     Wolfenstein 3-D 1.4, Apogee shareware (.WL1)
#   VERSION=sod     Spear of Destiny (.SOD), makes spear.exe
#   VERSION=sdm     Spear of Destiny demo (.SDM), makes spear.exe
#
# The compiler: a local DJGPP cross compiler (i586-pc-msdosdjgpp-gcc on the
# PATH) when there is one, otherwise the djfdyuruiry/djgpp Docker image.
# Force either with DOCKER=1 / DOCKER=0.
#
# The id source (../wolfsrc) is compiled as 32-bit protected mode C: the
# memory manager is malloc and the page file (VSWAP) is read into memory
# at startup, so there's no conventional / EMS / XMS memory to scan or
# juggle. The assembly (raycaster, compiled scalers, sound ISRs, Huffman
# expander...) is C now; the hardware is driven as before: unchained VGA
# ("mode X"), the keyboard on IRQ 1, the PIT on IRQ 0 for timing, PC
# speaker, AdLib and Sound Blaster (BLASTER=A220 I5 D1).
#
# Runs on a 386 or later with a 387 (or a 486DX; without one, put
# EMU387.DXE from DJGPP's v2misc next to it), 4 MB of RAM or more. Put the
# data files in the same directory.
VERSION ?= wl6

SRC     := ../wolfsrc
BUILD   := build/$(VERSION)

ifeq ($(VERSION),wl6)
VERDEF  = -DVER_WL6
EXENAME = wolf3d
endif
ifeq ($(VERSION),wl6apo)
VERDEF  = -DVER_WL6APO
EXENAME = wolf3d
endif
ifeq ($(VERSION),wl1)
VERDEF  = -DVER_WL1
EXENAME = wolf3d
endif
ifeq ($(VERSION),sod)
VERDEF  = -DVER_SOD
EXENAME = spear
endif
ifeq ($(VERSION),sdm)
VERDEF  = -DVER_SDM
EXENAME = spear
endif
ifndef VERDEF
$(error VERSION must be wl6, wl6apo, wl1, sod or sdm)
endif

DOS_TARGET      = $(EXENAME).exe
DOS_COFF        = $(EXENAME)
COFF_TARGET     = $(BUILD)/$(EXENAME).coff
EMBEDDED_TARGET = embedded.exe

DJGPP_IMAGE = djfdyuruiry/djgpp
CSDPMI_URL  = http://na.mirror.garr.it/mirrors/djgpp/current/v2misc/csdpmi7b.zip

USER_ID  = $(shell id -u)
GROUP_ID = $(shell id -g)

CROSS ?= i586-pc-msdosdjgpp-
ifeq ($(origin DOCKER),undefined)
DOCKER := $(if $(shell command -v $(CROSS)gcc 2>/dev/null),0,1)
endif

ifeq ($(DOCKER),1)
# The whole repository is mounted, so ../wolfsrc is reachable.
RUN      = docker run --rm -v $(abspath ..):/src:z -w /src/msdos -u $(USER_ID):$(GROUP_ID) $(DJGPP_IMAGE) /bin/sh -c
CC_DOS   = gcc
EXE2COFF = exe2coff
else
RUN      = /bin/sh -c
CC_DOS   = $(CROSS)gcc
EXE2COFF = $(shell command -v $(CROSS)exe2coff 2>/dev/null || echo $(dir $(shell command -v $(CROSS)gcc))../i586-pc-msdosdjgpp/bin/exe2coff)
endif

# A 386 with a 387, tuned for a Pentium. The code is 1992's: gnu89, old
# style common globals (-fcommon), and floats rounded to their declared
# size as Borland's compiler did (-ffloat-store), so the trig tables (and
# the demos that depend on them) come out the same.
ARCH     = -march=i386 -mtune=i586
CFLAGS   = -O2 $(ARCH) -std=gnu89 -fcommon -ffloat-store -fno-strict-aliasing -w $(VERDEF) -I$(SRC)
LDFLAGS  = -s

WOLF_SRC := id_ca id_in id_mm id_pm id_sd id_us_1 id_us_a id_vh id_vl \
            wl_act1 wl_act2 wl_agent wl_debug wl_draw wl_game wl_inter wl_main \
            wl_menu wl_net wl_play wl_scale wl_state \
            dos32 gamepal signon
# Spear has no help or end texts (wl_text.c wasn't in its project either)
ifeq ($(filter sod sdm,$(VERSION)),)
WOLF_SRC += wl_text
endif
C_SRCS   := $(addprefix $(SRC)/,$(addsuffix .c,$(WOLF_SRC)))
HDRS     := $(wildcard $(SRC)/*.h)

all: msdos_embedded setup

pull-djgpp:
ifeq ($(DOCKER),1)
	docker pull $(DJGPP_IMAGE)
endif

get-csdpmi:
	@if [ ! -d "csdpmi" ]; then \
		wget $(CSDPMI_URL); \
		unzip -o csdpmi7b.zip -d csdpmi; \
	fi

# One compiler run, then link (no make inside the container).
$(COFF_TARGET): $(C_SRCS) $(HDRS) Makefile | pull-djgpp
	$(RUN) "mkdir -p $(BUILD) && \
		cd $(BUILD) && $(CC_DOS) $(CFLAGS:-I%=-I../../%) -c $(addprefix ../../,$(C_SRCS)) && cd ../.. && \
		$(CC_DOS) $(LDFLAGS) $(BUILD)/*.o -lm -o $(BUILD)/$(DOS_TARGET) && \
		$(EXE2COFF) $(BUILD)/$(DOS_TARGET) && mv $(BUILD)/$(DOS_COFF) $(COFF_TARGET)"

# Needs CWSDPMI.EXE (or another DPMI host) at run time.
msdos_original: $(COFF_TARGET) get-csdpmi
	cp $(BUILD)/$(DOS_TARGET) $(DOS_TARGET)
	cp csdpmi/bin/CWSDPMI.EXE .

# CWSDPMI's stub in front: one file that runs on plain DOS.
msdos_embedded: $(COFF_TARGET) get-csdpmi
	@if [ ! -f "csdpmi/bin/CWSDSTUB.EXE" ]; then \
		echo "ERROR: CWSDSTUB.EXE not found in csdpmi/bin directory!"; \
		echo "Make sure you have the complete CSDPMI package."; \
		exit 1; \
	fi
	cat csdpmi/bin/CWSDSTUB.EXE $(COFF_TARGET) > $(EMBEDDED_TARGET)
	mv $(EMBEDDED_TARGET) $(DOS_TARGET)
	chmod +x $(DOS_TARGET)
	@echo "Created $(DOS_TARGET) ($(VERSION)) with embedded DPMI extender"

msdos: msdos_embedded

# SETUP.EXE: settings and the IPX co-op menu, for every VERSION. As
# IPXSETUP.EXE it starts in the network menu.
SETUP_COFF = build/setup.coff
$(SETUP_COFF): setup.c Makefile | pull-djgpp
	$(RUN) "mkdir -p build/setup && \
		$(CC_DOS) -O2 $(ARCH) -std=gnu99 -Wall -Wno-unused $(LDFLAGS) setup.c -o build/setup/setup.exe && \
		$(EXE2COFF) build/setup/setup.exe && mv build/setup/setup $(SETUP_COFF)"

setup: $(SETUP_COFF) get-csdpmi
	@if [ -f "csdpmi/bin/CWSDSTUB.EXE" ]; then \
		cat csdpmi/bin/CWSDSTUB.EXE $(SETUP_COFF) > setup.exe; \
	else \
		cp build/setup/setup.exe setup.exe; \
		echo "No CWSDSTUB.EXE: setup.exe needs CWSDPMI.EXE next to it"; \
	fi
	cp setup.exe ipxsetup.exe
	chmod +x setup.exe ipxsetup.exe

# DOSBox with a Sound Blaster 16 and the data copied in.
DATA ?= .
DATAEXT = $(if $(filter wl6apo,$(VERSION)),wl6,$(VERSION))
run: msdos_embedded
	@ls $(DATA)/*.$(DATAEXT) $(DATA)/*.$(shell echo $(DATAEXT) | tr a-z A-Z) 2>/dev/null | grep -q . || \
		{ echo "No .$(DATAEXT) data files in $(DATA): make run DATA=path/to/data"; exit 1; }
	@mkdir -p build/dosc
	cp $(DOS_TARGET) build/dosc/
	for f in $(DATA)/*.$(DATAEXT) $(DATA)/*.$(shell echo $(DATAEXT) | tr a-z A-Z); do \
		[ -f "$$f" ] && cp "$$f" "build/dosc/$$(basename "$$f" | tr a-z A-Z)"; done; true
	printf '[cpu]\ncore=dynamic\ncputype=auto\ncycles=max\n[dosbox]\nmachine=svga_s3\nmemsize=16\n[sblaster]\nsbtype=sb16\nsbbase=220\nirq=7\ndma=1\nhdma=5\noplmode=auto\n[autoexec]\nset BLASTER=A220 I7 D1 H5 T6\n' > build/dosbox.conf
	dosbox -conf build/dosbox.conf -c "mount c build/dosc" -c "c:" -c "$(DOS_TARGET) $(ARGS)"

clean:
	rm -rf build $(DOS_TARGET) wolf3d.exe spear.exe setup.exe ipxsetup.exe $(EMBEDDED_TARGET) CWSDPMI.EXE || true
	rm -rf csdpmi csdpmi7b.zip csdpmi7b.zip.* || true

.PHONY: all pull-djgpp get-csdpmi msdos msdos_original msdos_embedded setup run clean
