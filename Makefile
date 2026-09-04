# ELF City -- a 3D file explorer for binaries
CC      ?= gcc
PKGS     = sdl2 SDL2_ttf gl glu capstone
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=c11 -Isrc $(shell pkg-config --cflags $(PKGS))
LDLIBS   = $(shell pkg-config --libs $(PKGS)) -lm

SRC = src/elfload.c src/ehframe.c src/disasm.c src/city.c src/world.c src/text.c src/render.c src/hud.c src/main.c
OBJ = $(SRC:.c=.o)
BIN = elfcity

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDLIBS)

$(OBJ): src/model.h src/world.h src/app.h src/render.h src/text.h src/disasm.h

clean:
	rm -f $(OBJ) $(BIN)

run: $(BIN)
	./$(BIN) $(FILE)

.PHONY: all clean run
