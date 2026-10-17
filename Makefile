CC     ?= cc
CFLAGS ?= -std=c99 -O3 -Wall -Wextra -pedantic
PREFIX ?= /usr/local
DEPTH  ?= 1024
RUNS   ?= 5000
SEED   ?=
CTS    = jsonpath-compliance-test-suite

DEF    = -DMAXDEPTH=$(DEPTH)
# a 7-byte window makes every token straddle a refill when input is piped
SAN    = -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -DWINDOW=7
# the tool never frees, so the leak checker would only mask real findings
SANENV = ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

all: out/json

# make compares mtimes, not flags, and only to the second, so check the flags
# while the makefile is read, before any file is looked at
FLAGS := $(CC) $(CFLAGS) $(DEPTH)
ifneq ($(FLAGS),$(shell cat out/.flags 2>/dev/null))
$(shell mkdir -p out && rm -f out/json out/json.scalar out/json.sse2 out/json.asan && echo '$(FLAGS)' > out/.flags)
endif

out/json: src/json.c src/unicat.h
	$(CC) $(CFLAGS) $(DEF) -o $@ src/json.c
out/json.scalar: src/json.c src/unicat.h
	$(CC) $(CFLAGS) $(DEF) -DNOSIMD -o $@ src/json.c
# on x86, SSE2 even where the CPU has AVX2; on ARM, the same NEON build
out/json.sse2: src/json.c src/unicat.h
	$(CC) $(CFLAGS) $(DEF) -DNOAVX2 -o $@ src/json.c
out/json.asan: src/json.c src/unicat.h
	$(CC) $(SAN) $(DEF) -o $@ src/json.c

test: out/json
	JSON=$< DEPTH=$(DEPTH) sh test/test.sh
scalar: out/json.scalar
	JSON=$< DEPTH=$(DEPTH) sh test/test.sh
conformance: out/json JSONTestSuite
	JSON=$< sh test/conformance.sh
jsonpath: out/json $(CTS)
	python3 test/jsonpath.py $< $(CTS)/cts.json
check: out/json.asan JSONTestSuite $(CTS)
	$(SANENV) JSON=$< DEPTH=$(DEPTH) sh test/test.sh
	$(SANENV) JSON=$< sh test/conformance.sh
	$(SANENV) python3 test/jsonpath.py $< $(CTS)/cts.json
	$(SANENV) python3 test/edges.py $<
fuzz: out/json.asan
	$(SANENV) python3 test/fuzz.py $(RUNS) $< $(SEED)
diff: out/json out/json.scalar out/json.sse2
	python3 test/corpus.py > out/corpus.json
	out/json < out/corpus.json > out/corpus.out
	out/json.scalar < out/corpus.json | cmp - out/corpus.out
	out/json.sse2 < out/corpus.json | cmp - out/corpus.out
	cat out/corpus.json | out/json | cmp - out/corpus.out

ci: test scalar check fuzz conformance jsonpath diff

JSONTestSuite:
	git clone --depth 1 https://github.com/nst/JSONTestSuite.git
$(CTS):
	git clone --depth 1 https://github.com/jsonpath-standard/$(CTS).git

install: out/json
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 out/json $(DESTDIR)$(PREFIX)/bin/json
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/json
clean:
	rm -rf out

.PHONY: all test scalar conformance jsonpath check fuzz diff ci install uninstall clean
