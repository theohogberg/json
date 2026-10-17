#!/usr/bin/env python3
"""Deterministic input for `make diff`: strings of every length 0-69 and
whitespace runs of 0-40 bytes, so every 16-byte block boundary a vector
scanner handles is crossed by plain text, escapes and multi-byte UTF-8."""
import json
import random

random.seed(1)
mixed, plain = 'ab "\\/\n\t\x01å中\U0001f600', 'abcdefgh'
doc = [{"k" * (n % 5): "".join(random.choice(plain if n % 2 else mixed)
                               for _ in range(n % 70)), "n": [n, n / 8, None]}
       for n in range(3000)]
out = json.dumps(doc, ensure_ascii=False, indent=1)
print("".join(c + " " * random.randrange(41) if c == "\n" else c for c in out))
