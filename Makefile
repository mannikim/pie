# SPDX-License-Identifier: GPL-3.0-or-later
# copyright 2025-2026 mannikim <mannikim[at]proton[dot]me>
# this file is part of pie
# see LICENSE file for the license text

CFLAGS := -std=c99 -Os -Wall -Wpedantic -Wextra
PREFIX := /usr/local
LIBS := -lGL -lglfw -lGLEW -lm

all: pie pcp

pie: pie.c common.h
	$(CC) $< -o $@ $(CFLAGS) $(LIBS)

pcp: pcp.c common.h
	$(CC) $< -o $@ $(CFLAGS) $(LIBS)

install: pie pcp pie-cp
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp pie $(DESTDIR)$(PREFIX)/bin
	cp pcp $(DESTDIR)$(PREFIX)/bin
	cp pie-cp $(DESTDIR)$(PREFIX)/bin

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/pie
	rm -f $(DESTDIR)$(PREFIX)/bin/pcp

clean:
	rm -f pie

.PHONY: all clean install uninstall
