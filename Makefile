.POSIX:
CC      = cc
WINDRES = windres
CFLAGS  = -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -D_WIN32_WINNT=0x0601 -municode
DFLAGS  = -std=c99 -g  -O0 -Wall -Wextra -Wno-unused-parameter -D_WIN32_WINNT=0x0601 -municode
LDFLAGS = -mwindows -static
LDLIBS  = -lshell32 -luser32 -lgdi32 -ldwmapi -lcomctl32

SHELL       = cmd.exe
.SHELLFLAGS = /c

all: bitdim.exe

bitdim.exe: bitdim.c bitdim.res
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ bitdim.c bitdim.res $(LDLIBS)

bitdim.res: bitdim.rc bitdim.ico bitdim.manifest
	$(WINDRES) -O coff -i bitdim.rc -o bitdim.res

debug: bitdim_debug.exe

bitdim_debug.exe: bitdim.c bitdim.res
	$(CC) $(DFLAGS) $(LDFLAGS) -o $@ bitdim.c bitdim.res $(LDLIBS)

clean:
	rm -f bitdim.exe bitdim_debug.exe bitdim.res
