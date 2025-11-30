CFLAGS := -std=c99 -Os -Wall -Wpedantic
PREFIX := /usr/local
LIBS := -lGL -lglfw -lGLEW -lm

pie: pie.c
	$(CC) $< -o $@ $(CFLAGS) $(LIBS)

install: pie
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp pie $(DESTDIR)$(PREFIX)/bin

clean:
	rm -f pie
