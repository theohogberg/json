CC     ?= cc
CFLAGS ?= -std=c99 -O3 -Wall -Wextra -pedantic
PREFIX ?= /usr/local
DEPTH  ?= 1024
RUNS   ?= 5000
SEED   ?=

DEF    = -DMAXDEPTH=$(DEPTH)
SAN    = -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
# the tool never frees, so the leak checker would only mask real findings
SANENV = ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

all: out/json

# make compares mtimes, not flags, and only to the second, so check the flags
# while the makefile is read, before any file is looked at
FLAGS := $(CC) $(CFLAGS) $(DEPTH)
ifneq ($(FLAGS),$(shell cat out/.flags 2>/dev/null))
$(shell mkdir -p out && rm -f out/json out/json.scalar out/json.asan && echo '$(FLAGS)' > out/.flags)
endif

out/json: src/json.c
	$(CC) $(CFLAGS) $(DEF) -o $@ src/json.c
out/json.scalar: src/json.c
	$(CC) $(CFLAGS) $(DEF) -DNOSIMD -o $@ src/json.c
out/json.asan: src/json.c
	$(CC) $(SAN) $(DEF) -o $@ src/json.c

test: out/json
	JSON=$< DEPTH=$(DEPTH) sh test/test.sh
scalar: out/json.scalar
	JSON=$< DEPTH=$(DEPTH) sh test/test.sh
conformance: out/json JSONTestSuite
	JSON=$< sh test/conformance.sh
check: out/json.asan JSONTestSuite
	$(SANENV) JSON=$< DEPTH=$(DEPTH) sh test/test.sh
	$(SANENV) JSON=$< sh test/conformance.sh
fuzz: out/json.asan
	$(SANENV) python3 test/fuzz.py $(RUNS) $< $(SEED)
diff: out/json out/json.scalar
	python3 test/corpus.py > out/corpus.json
	out/json < out/corpus.json > out/corpus.out
	out/json.scalar < out/corpus.json | cmp - out/corpus.out

ci: test scalar check fuzz conformance diff

JSONTestSuite:
	git clone --depth 1 https://github.com/nst/JSONTestSuite.git

install: out/json
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 out/json $(DESTDIR)$(PREFIX)/bin/json
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/json
clean:
	rm -rf out

.PHONY: all test scalar conformance check fuzz diff ci install uninstall clean
