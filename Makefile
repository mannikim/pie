# SPDX-License-Identifier: GPL-3.0-or-later
# copyright 2025-2026 mannikim <mannikim[at]proton[dot]me>
# this file is part of pie
# see LICENSE file for the license text

CFLAGS := -std=c99 -Os -Wall -Wpedantic -Wextra
PREFIX := /usr/local
LIBS := -lGL -lglfw -lGLEW -lm

all: pie pcp piec

pie: pie.c common.h
	$(CC) $< -o $@ $(CFLAGS) $(LIBS)

pcp: pcp.c common.h
	$(CC) $< -o $@ $(CFLAGS) $(LIBS)

piec: piec.c
	$(CC) $< -o $@ $(CFLAGS)

install: pie pcp pie-cp piec
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp pie $(DESTDIR)$(PREFIX)/bin
	cp pcp $(DESTDIR)$(PREFIX)/bin
	cp piec $(DESTDIR)$(PREFIX)/bin
	cp pie-cp $(DESTDIR)$(PREFIX)/bin

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/pie
	rm -f $(DESTDIR)$(PREFIX)/bin/pcp
	rm -f $(DESTDIR)$(PREFIX)/bin/piec
	rm -f $(DESTDIR)$(PREFIX)/bin/pie-cp

clean:
	rm -f pie pcp piec

.PHONY: all clean install uninstall
