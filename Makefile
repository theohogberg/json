CC     ?= cc
PREFIX ?= /usr/local
DEPTH  ?= 1024
RUNS   ?= 5000
RUNS_BENCH ?= 15
SEED   ?=
CTS    = jsonpath-compliance-test-suite

# Windows (MSYS2, MinGW and Git Bash all set OS): same targets, .exe outputs.
# Recipes need a POSIX sh either way; CC defaults to cl, else clang-cl, so a
# Visual Studio developer prompt works as it is. MinGW gcc is not supported.
ifeq ($(OS),Windows_NT)
# From cmd.exe, make falls back to cmd as its shell when no sh.exe is on PATH:
# borrow Git for Windows' (git.exe in Git\cmd, sh.exe in Git\usr\bin), named
# by its 8.3 path because make splits words at the space in "Program Files"
ifneq ($(shell echo 'x'),x)
GITBIN := $(subst \cmd\,\usr\bin,$(shell for %%i in (git.exe) do @echo %%~sdp$$PATH:i))
ifeq ($(wildcard $(subst \,/,$(GITBIN))/sh.exe),)
$(error no sh.exe: run make from Git Bash or MSYS2, or install Git for Windows)
endif
export PATH := $(GITBIN);$(PATH)
SHELL  := $(subst \,/,$(GITBIN))/sh.exe
endif
X      = .exe
# the test scripts' Python would otherwise write cp1252
export PYTHONUTF8 = 1
ifeq ($(origin CC),default)
CC    := $(or $(shell for c in cl clang-cl; do command -v $$c >/dev/null 2>&1 && echo $$c && break; done),cl)
endif
endif

# MSVC (cl, clang-cl) has its own flags; C11 is its oldest C mode
MSVC   = $(filter cl cl.exe clang-cl clang-cl.exe,$(notdir $(CC)))
ifneq ($(MSVC),)
CFLAGS ?= -nologo -std:c11 -O2 -W4
OUT    = -Fe$@ -Fo$(basename $@).obj
# no UBSan in MSVC; -Fd keeps each build's debug info apart under make -j
SAN    = -nologo -std:c11 -Zi -Od -fsanitize=address -DWINDOW=7 -Fd$(basename $@).pdb
else
CFLAGS ?= -std=c99 -O3 -Wall -Wextra -pedantic
OUT    = -o $@
# a 7-byte window makes every token straddle a refill when input is piped
SAN    = -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -DWINDOW=7
endif

DEF    = -DMAXDEPTH=$(DEPTH)
# the tool never frees, so the leak checker would only mask real findings
SANENV = ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

all: out/json$(X)

# make compares mtimes, not flags, and only to the second, so check the flags
# while the makefile is read, before any file is looked at
FLAGS := $(CC) $(CFLAGS) $(DEPTH)
ifneq ($(FLAGS),$(shell cat out/.flags 2>/dev/null))
$(shell mkdir -p out && rm -f out/json$(X) out/json.scalar$(X) out/json.sse2$(X) out/json.asan$(X) && echo '$(FLAGS)' > out/.flags)
endif

out/json$(X): src/json.c src/unicat.h
	$(CC) $(CFLAGS) $(DEF) $(OUT) src/json.c
out/json.scalar$(X): src/json.c src/unicat.h
	$(CC) $(CFLAGS) $(DEF) -DNOSIMD $(OUT) src/json.c
# on x86, SSE2 even where the CPU has AVX2; on ARM, the same NEON build
out/json.sse2$(X): src/json.c src/unicat.h
	$(CC) $(CFLAGS) $(DEF) -DNOAVX2 $(OUT) src/json.c
out/json.asan$(X): src/json.c src/unicat.h
	$(CC) $(SAN) $(DEF) $(OUT) src/json.c

test: out/json$(X)
	JSON=$< DEPTH=$(DEPTH) sh test/test.sh
scalar: out/json.scalar$(X)
	JSON=$< DEPTH=$(DEPTH) sh test/test.sh
conformance: out/json$(X) JSONTestSuite
	JSON=$< sh test/conformance.sh
jsonpath: out/json$(X) $(CTS)
	python3 test/jsonpath.py $< $(CTS)/cts.json
check: out/json.asan$(X) JSONTestSuite $(CTS)
	$(SANENV) JSON=$< DEPTH=$(DEPTH) sh test/test.sh
	$(SANENV) JSON=$< sh test/conformance.sh
	$(SANENV) python3 test/jsonpath.py $< $(CTS)/cts.json
	$(SANENV) python3 test/edges.py $<
fuzz: out/json.asan$(X)
	$(SANENV) python3 test/fuzz.py $(RUNS) $< $(SEED)
diff: out/json$(X) out/json.scalar$(X) out/json.sse2$(X)
	python3 test/corpus.py > out/corpus.json
	out/json$(X) < out/corpus.json > out/corpus.out
	out/json.scalar$(X) < out/corpus.json | cmp - out/corpus.out
	out/json.sse2$(X) < out/corpus.json | cmp - out/corpus.out
	cat out/corpus.json | out/json$(X) | cmp - out/corpus.out

ci: test scalar check fuzz conformance jsonpath diff

# against jq, which must be on PATH; not part of ci
bench: out/json$(X)
	python3 test/bench.py $< $(RUNS_BENCH)

JSONTestSuite:
	git clone --depth 1 https://github.com/nst/JSONTestSuite.git
$(CTS):
	git clone --depth 1 https://github.com/jsonpath-standard/$(CTS).git

install: out/json$(X)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 out/json$(X) $(DESTDIR)$(PREFIX)/bin/json$(X)
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/json$(X)
clean:
	rm -rf out

.PHONY: all test scalar conformance jsonpath check fuzz diff ci bench install uninstall clean
