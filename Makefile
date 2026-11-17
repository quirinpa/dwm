# dwm - dynamic window manager
# See LICENSE file for copyright and license details.

include config.mk

SRC = drw.c dwm.c util.c
OBJ = ${SRC:.c=.o}

all: dwm dwm-msg dwm-bar

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: config.h config.mk

config.h:
	cp config.def.h $@

dwm: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

dwm-msg: dwm-msg.c
	${CC} ${CFLAGS} -o $@ dwm-msg.c -L${X11LIB} -lX11

dwm-bar: dwm-bar.c
	${CC} ${CFLAGS} -o $@ dwm-bar.c -L${X11LIB} -lX11 -lasound

clean:
	rm -f dwm dwm-msg dwm-bar ${OBJ} dwm-${VERSION}.tar.gz

dist: clean
	mkdir -p dwm-${VERSION}
	cp -R LICENSE Makefile README config.def.h config.mk\
		dwm.1 drw.h util.h ${SRC} dwm.png transient.c dwm-${VERSION}
	tar -cf dwm-${VERSION}.tar dwm-${VERSION}
	gzip dwm-${VERSION}.tar
	rm -rf dwm-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	install -m 755 dwm dwm-msg dwm-bar ${DESTDIR}${PREFIX}/bin/
	install -m 755 scripts/dwm-ctl scripts/dwm-net scripts/dwm-shot scripts/dwm-help scripts/dwm-voice scripts/dwm-cal scripts/dwm-auren scripts/dwm-session scripts/gnome-control-center ${DESTDIR}${PREFIX}/bin/
	mkdir -p $(HOME)/.dwm
	install -m 755 scripts/autostart.sh $(HOME)/.dwm/autostart.sh
	mkdir -p $(HOME)/.config/dwm
	[ -f $(HOME)/.config/dwm/intents.conf ] || install -m 644 intents.conf $(HOME)/.config/dwm/intents.conf
	mkdir -p $(HOME)/.config/dunst
	install -m 644 dunstrc $(HOME)/.config/dunst/dunstrc
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	sed "s/VERSION/${VERSION}/g" < dwm.1 > ${DESTDIR}${MANPREFIX}/man1/dwm.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/dwm.1
	@if pgrep -x dwm >/dev/null 2>&1; then \
		kill -HUP $$(pgrep -x dwm | head -n 1) 2>/dev/null || true; \
		DISPLAY=$${DISPLAY:-:0} xprop -root -f _DWM_RELOAD 8s -set _DWM_RELOAD 1 2>/dev/null || true; \
	fi

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/dwm\
		${DESTDIR}${MANPREFIX}/man1/dwm.1

.PHONY: all clean dist install uninstall
