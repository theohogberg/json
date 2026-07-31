COMPILER := gcc
CFLAGS := -ansi
SOURCE := $(wildcard src/*.c)
TARGET := out/json

default: build run

build:
	mkdir -p out
	gcc $(CFLAGS) $(SOURCE) -o $(TARGET)

run:
	$(TARGET)

tags:
	ctags -R --c-kinds=+p /usr/include src/

.PHONY: build run tags
