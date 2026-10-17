CC      ?= cc
CFLAGS  ?= -std=c99 -O3 -Wall -Wextra -pedantic
PREFIX  ?= /usr/local
RUNS    ?= 5000
# fuzz seed: empty picks one and prints it; set it to replay a run
SEED    ?=

# Recursion cap: ~64 bytes of stack per level. Deeper is generated, not data.
DEPTH   ?= 1024

# The tool never frees, so the leak checker would only mask real findings.
SANENV   = ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
SAN      = -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer

# every src/<name>.c is a variant
VARIANTS = $(basename $(notdir $(wildcard src/*.c)))
BINS     = $(VARIANTS:%=out/%)

all: $(BINS)

# Make compares timestamps, not flags; record them so DEPTH/CFLAGS changes rebuild.
out/.flags: FORCE
	@mkdir -p out
	@echo '$(CC) $(CFLAGS) $(DEPTH)' | cmp -s - $@ || echo '$(CC) $(CFLAGS) $(DEPTH)' > $@
FORCE:

out/%.asan: src/%.c out/.flags
	$(CC) $(SAN) -DMAXDEPTH=$(DEPTH) -o $@ $<

out/%: src/%.c out/.flags
	$(CC) $(CFLAGS) -DMAXDEPTH=$(DEPTH) -o $@ $<

# only one of the SIMD/scalar paths runs on a given machine; test both
out/%.scalar: src/%.c out/.flags
	$(CC) $(CFLAGS) -DNOSIMD -DMAXDEPTH=$(DEPTH) -o $@ $<

test-%: out/%
	JSON=out/$* DEPTH=$(DEPTH) sh test/test.sh

scalar-%: out/%.scalar
	JSON=out/$*.scalar DEPTH=$(DEPTH) sh test/test.sh

conformance-%: out/% JSONTestSuite
	JSON=out/$* sh test/conformance.sh

check-%: out/%.asan JSONTestSuite
	$(SANENV) JSON=out/$*.asan DEPTH=$(DEPTH) sh test/test.sh
	$(SANENV) JSON=out/$*.asan sh test/conformance.sh

fuzz-%: out/%.asan
	$(SANENV) python3 test/fuzz.py $(RUNS) out/$*.asan $(SEED)

test:        $(VARIANTS:%=test-%)
scalar:      $(VARIANTS:%=scalar-%)
conformance: $(VARIANTS:%=conformance-%)
check:       $(VARIANTS:%=check-%)
fuzz:        $(VARIANTS:%=fuzz-%)

# every variant, SIMD or scalar, must emit identical output
diff: $(BINS) $(BINS:=.scalar)
	@for f in test/test.json test/test2.json; do \
		$(firstword $(BINS)) < $$f > out/.ref; \
		for b in $^; do $$b < $$f | cmp -s - out/.ref || { echo "DIFF $$b $$f"; exit 1; }; done; \
	done; echo "ok $^"

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

# out/x.asan and out/x.scalar are only ever built as prerequisites, which
# makes them intermediate: make would delete them after every run and
# rebuild them the next time. Keep them.
.SECONDARY:

# test-%/scalar-%/check-%/fuzz-% are deliberately not phony: that would stop make
# from matching them against the pattern rules.
.PHONY: all test scalar conformance check fuzz diff ci install uninstall clean FORCE
