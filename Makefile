.PHONY: clean

CFLAGS = `sdl2-config --cflags` -fPIC -O2
LDFLAGS = `sdl2-config --libs` -shared

SRC = moreram.c
OBJ = $(SRC:.c=.o)

TARGET = moreram.so

all: $(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDFLAGS)
