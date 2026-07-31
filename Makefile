CC       ?= cc
CFLAGS   ?= -std=c99 -O3 -Wall -Wextra -pedantic
PREFIX   ?= /usr/local

INCS      = -Iinclude
LDFLAGS  += -Llib

SRC       = src/json.c
OUT       = out
BIN       = $(OUT)/json
ASAN      = $(OUT)/json.asan

SAN       = -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
# the tool deliberately never frees; exit reclaims. turn off the leak checker
# so it does not mask real findings.
SANENV    = ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

all: $(BIN)

# order-only (|) so writing a binary into out/ does not stamp the directory
# newer than the binary and force a rebuild every time.
$(OUT):
	mkdir -p $(OUT)

$(BIN): $(SRC) | $(OUT)
	$(CC) $(CFLAGS) $(INCS) $(LDFLAGS) -o $@ $<

$(ASAN): $(SRC) | $(OUT)
	$(CC) $(SAN) $(INCS) $(LDFLAGS) -o $@ $<

# fast feedback loop
test: $(BIN)
	JSON=$(BIN) ./test.sh

# same tests, memory- and UB-checked
check: $(ASAN) JSONTestSuite
	$(SANENV) JSON=$(ASAN) ./test.sh
	$(SANENV) JSON=$(ASAN) ./conformance.sh

# differential test against a reference parser
fuzz: $(ASAN)
	$(SANENV) python3 fuzz.py 5000 $(ASAN)

# github.com/nst/JSONTestSuite
JSONTestSuite:
	git clone --depth 1 https://github.com/nst/JSONTestSuite.git

conformance: $(BIN) JSONTestSuite
	JSON=$(BIN) ./conformance.sh

# everything, the way CI should run it
ci: test check fuzz conformance

tags: $(SRC)
	ctags -R src include

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/json

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/json

clean:
	rm -rf $(OUT)

.PHONY: all test check fuzz conformance ci install uninstall clean
