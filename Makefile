CC     ?= cc
CFLAGS ?= -std=c99 -O3 -Wall -Wextra -pedantic
PREFIX ?= /usr/local
DEPTH  ?= 1024
RUNS   ?= 5000
SEED   ?=
CTS    = jsonpath-compliance-test-suite

# Windows (MSYS2, MinGW and Git Bash all set OS): same targets, .exe outputs
ifeq ($(OS),Windows_NT)
X      = .exe
ifeq ($(origin CC),default)
CC     = gcc
endif
endif

DEF    = -DMAXDEPTH=$(DEPTH)
# a 7-byte window makes every token straddle a refill when input is piped
SAN    = -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -DWINDOW=7
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
	$(CC) $(CFLAGS) $(DEF) -o $@ src/json.c
out/json.scalar$(X): src/json.c src/unicat.h
	$(CC) $(CFLAGS) $(DEF) -DNOSIMD -o $@ src/json.c
# on x86, SSE2 even where the CPU has AVX2; on ARM, the same NEON build
out/json.sse2$(X): src/json.c src/unicat.h
	$(CC) $(CFLAGS) $(DEF) -DNOAVX2 -o $@ src/json.c
out/json.asan$(X): src/json.c src/unicat.h
	$(CC) $(SAN) $(DEF) -o $@ src/json.c

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

.PHONY: all test scalar conformance jsonpath check fuzz diff ci install uninstall clean
