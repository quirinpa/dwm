# dwm - dynamic window manager
# See LICENSE file for copyright and license details.

include config.mk

SRC = drw.c dwm.c util.c
OBJ = ${SRC:.c=.o}

all: dwm dwm-msg dwm-bar mods/dwm-jarvis/dwm-jarvis.so mods/dwm-ren/dwm-ren.so mods/ren-dwm/ren-dwm.so

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: config.h config.mk dwm.h dwm-types.h

config.h:
	cp config.def.h $@

dwm: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

mods/dwm-jarvis/dwm-jarvis.so: mods/dwm-jarvis/dwm-jarvis.c mods/dwm_ctl.h dwm-types.h
	${CC} ${CFLAGS} -fPIC -shared -o $@ mods/dwm-jarvis/dwm-jarvis.c -L${X11LIB} -lX11 -L$(HOME)/.local/lib -ljarvis -lxylem -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,$(HOME)/.local/lib

mods/dwm-ren/dwm-ren.so: mods/dwm-ren/dwm-ren.c dwm-types.h
	${CC} ${CFLAGS} -fPIC -shared -o $@ mods/dwm-ren/dwm-ren.c -L$(HOME)/.local/lib -lxylem -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,$(HOME)/.local/lib

mods/ren-dwm/ren-dwm.so: mods/ren-dwm/ren-dwm.c mods/dwm_ctl.h dwm-types.h
	${CC} ${CFLAGS} -fPIC -shared -o $@ mods/ren-dwm/ren-dwm.c -L${X11LIB} -lX11 -L$(HOME)/.local/lib -lren -lxylem -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,$(HOME)/.local/lib

dwm-msg: dwm-msg.c
	${CC} ${CFLAGS} -o $@ dwm-msg.c -L${X11LIB} -lX11

dwm-bar: dwm-bar.c
	${CC} ${CFLAGS} -o $@ dwm-bar.c -L${X11LIB} -lX11 -lasound

clean:
	rm -f dwm dwm-msg dwm-bar dwm-vad dwm-whisperd mods/dwm-jarvis/dwm-jarvis.so mods/dwm-ren/dwm-ren.so mods/ren-dwm/ren-dwm.so mods/tts/mod_tts.so ${OBJ} dwm-${VERSION}.tar.gz

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
	@rm -f ${DESTDIR}${PREFIX}/bin/dwm-voice ${DESTDIR}${PREFIX}/bin/dwm-vad ${DESTDIR}${PREFIX}/bin/dwm-jarvis-pi
	install -m 755 scripts/dwm-ctl scripts/dwm-top scripts/dwm-net scripts/dwm-shot scripts/dwm-help scripts/dwm-voice scripts/dwm-cal scripts/dwm-auren scripts/dwm-rec scripts/dwm-jarvis scripts/dwm-session scripts/gnome-control-center ${DESTDIR}${PREFIX}/bin/
	@rm -f ${DESTDIR}${PREFIX}/bin/dwm-act ${DESTDIR}${PREFIX}/bin/dwm-tts ${DESTDIR}${PREFIX}/bin/jarvis_core.py
	@if [ -f $(HOME)/site/external/libjarvis/bin/jarvis ]; then \
		install -m 755 $(HOME)/site/external/libjarvis/bin/jarvis ${DESTDIR}${PREFIX}/bin/jarvis; \
	fi
	@if [ -f $(HOME)/site/external/libjarvis/bin/jarvisd ]; then \
		install -m 755 $(HOME)/site/external/libjarvis/bin/jarvisd ${DESTDIR}${PREFIX}/bin/jarvisd; \
		ln -sf jarvisd ${DESTDIR}${PREFIX}/bin/dwm-whisperd; \
	fi
	mkdir -p $(HOME)/.local/lib/dwm $(HOME)/.local/lib/ren $(HOME)/.config/jarvis
	@rm -f $(HOME)/.local/lib/mod_dwm.so
	install -m 755 mods/dwm-jarvis/dwm-jarvis.so $(HOME)/.local/lib/dwm/dwm-jarvis.so
	install -m 755 mods/dwm-ren/dwm-ren.so $(HOME)/.local/lib/dwm/dwm-ren.so
	install -m 755 mods/ren-dwm/ren-dwm.so $(HOME)/.local/lib/ren/ren-dwm.so
	@if [ -f $(HOME)/site/external/libjarvis/mods/tts/mod_tts.so ]; then \
		install -m 755 $(HOME)/site/external/libjarvis/mods/tts/mod_tts.so $(HOME)/.local/lib/mod_tts.so; \
	fi
	@if [ -f $(HOME)/site/external/libjarvis/data/ontology.json ]; then \
		install -m 644 $(HOME)/site/external/libjarvis/data/ontology.json $(HOME)/.config/jarvis/ontology.json; \
	fi
	mkdir -p $(HOME)/.dwm
	install -m 755 scripts/autostart.sh $(HOME)/.dwm/autostart.sh
	mkdir -p $(HOME)/.config/dunst
	install -m 644 dunstrc $(HOME)/.config/dunst/dunstrc
	install -m 644 Xresources $(HOME)/.Xresources
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	sed "s/VERSION/${VERSION}/g" < dwm.1 > ${DESTDIR}${MANPREFIX}/man1/dwm.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/dwm.1
	@DISPLAY=$${DISPLAY:-:0} xrdb -nocpp -merge $(HOME)/.Xresources 2>/dev/null || true
	@rm -f "/tmp/dwm-auren-xres-$$USER.env" "/tmp/dwm-voice-xres-$$USER.env" 2>/dev/null || true
	@pkill -f "python3 - .*intents\.conf" 2>/dev/null || true
	@rm -f "/tmp/dwm-auren-daemon-$$USER.pid" "/tmp/dwm-auren-$$USER.sock" 2>/dev/null || true
	@if pgrep -x dwm >/dev/null 2>&1; then \
		kill -HUP $$(pgrep -x dwm | head -n 1) 2>/dev/null || true; \
		DISPLAY=$${DISPLAY:-:0} xprop -root -f _DWM_RELOAD 8s -set _DWM_RELOAD 1 2>/dev/null || true; \
	fi

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/dwm\
		${DESTDIR}${MANPREFIX}/man1/dwm.1

.PHONY: all clean dist install uninstall
