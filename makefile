# ─────────────────────────────────────────────────────────────────────────────
# Makefile for filedrop
#
# Targets:
#   make              build with the auto-detected / default toolchain
#   make native       force native MinGW  (gcc)
#   make cross        force cross-compile (x86_64-w64-mingw32-gcc)
#   make clean        remove build artefacts
#
# Override anything on the command line, e.g.:
#   make CC=x86_64-w64-mingw32-gcc
#   make OUTDIR=build
# ─────────────────────────────────────────────────────────────────────────────

# ── output ───────────────────────────────────────────────────────────────────
TARGET  := filedrop.exe
SRC     := filedrop.c
OUTDIR  := .

# ── auto-detect toolchain ────────────────────────────────────────────────────
# $(OS) is set to "Windows_NT" by Windows regardless of the shell (cmd /
# PowerShell / Git Bash).  On Windows we just use gcc, which is already a
# native Windows compiler when it comes from MinGW / MSYS2 / winlibs / etc.
# On Linux we search for the mingw-w64 cross-compiler first because the
# plain gcc there targets Linux and cannot produce Windows binaries.
#
# Override at any time:  make CC=x86_64-w64-mingw32-gcc

ifeq ($(CC),cc)   # make sets CC=cc by default; treat that as "not set"
  CC :=
endif

ifndef CC
  ifeq ($(OS),Windows_NT)
    CC := gcc
  else
    # Linux / macOS – prefer the cross-compiler
    ifneq ($(shell command -v x86_64-w64-mingw32-gcc 2>/dev/null),)
      CC := x86_64-w64-mingw32-gcc
    else ifneq ($(shell command -v i686-w64-mingw32-gcc 2>/dev/null),)
      CC := i686-w64-mingw32-gcc
    else ifneq ($(shell command -v gcc 2>/dev/null),)
      CC := gcc
    else
      $(error No compiler found. \
              On Linux: sudo apt install gcc-mingw-w64-x86-64)
    endif
  endif
endif

# ── flags ────────────────────────────────────────────────────────────────────
CFLAGS  := -std=c99 -O2 -Wall -Wextra -D__USE_MINGW_ANSI_STDIO=1
LDFLAGS := -lws2_32

# Strip debug symbols from release builds and keep the binary small
CFLAGS  += -s

# ── rules ─────────────────────────────────────────────────────────────────────
.PHONY: all native cross clean

all: $(OUTDIR)/$(TARGET)

$(OUTDIR)/$(TARGET): $(SRC)
	@echo "  CC   $(SRC)  ->  $(OUTDIR)/$(TARGET)  [$(CC)]"
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  OK   $(OUTDIR)/$(TARGET)"

# ── convenience targets ───────────────────────────────────────────────────────
native:
	$(MAKE) CC=gcc

cross:
	$(MAKE) CC=x86_64-w64-mingw32-gcc

clean:
	@echo "  RM   $(OUTDIR)/$(TARGET)"
	@rm -f $(OUTDIR)/$(TARGET)