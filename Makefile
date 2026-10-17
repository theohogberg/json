CC      ?= cc
CFLAGS  ?= -std=c99 -O3 -Wall -Wextra -pedantic
PREFIX  ?= /usr/local
RUNS    ?= 5000

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

test-%: out/%
	JSON=out/$* DEPTH=$(DEPTH) sh test/test.sh

conformance-%: out/% JSONTestSuite
	JSON=out/$* sh test/conformance.sh

check-%: out/%.asan JSONTestSuite
	$(SANENV) JSON=out/$*.asan DEPTH=$(DEPTH) sh test/test.sh
	$(SANENV) JSON=out/$*.asan sh test/conformance.sh

fuzz-%: out/%.asan
	$(SANENV) python3 test/fuzz.py $(RUNS) out/$*.asan

test:        $(VARIANTS:%=test-%)
conformance: $(VARIANTS:%=conformance-%)
check:       $(VARIANTS:%=check-%)
fuzz:        $(VARIANTS:%=fuzz-%)

# all variants must emit identical output
diff: $(BINS)
	@$(firstword $(BINS)) < test/test.json > out/.ref
	@for b in $(BINS); do $$b < test/test.json | cmp - out/.ref && echo "ok $$b" || exit 1; done

ci: test check fuzz conformance diff

JSONTestSuite:
	git clone --depth 1 https://github.com/nst/JSONTestSuite.git

install: out/json
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 out/json $(DESTDIR)$(PREFIX)/bin/json

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/json

clean:
	rm -rf out

# test-%/check-%/fuzz-% are deliberately not phony: that would stop make
# from matching them against the pattern rules.
.PHONY: all test conformance check fuzz diff ci install uninstall clean FORCE
