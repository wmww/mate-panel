if ENABLE_WAYLAND
all: wlr-layer-shell-unstable-v1-client.h wlr-layer-shell-unstable-v1.c xdg-shell-client.h xdg-shell.c

wlr-layer-shell-unstable-v1-client.h: wlr-layer-shell-unstable-v1.xml
	wayland-scanner -c client-header wlr-layer-shell-unstable-v1.xml wlr-layer-shell-unstable-v1-client.h

wlr-layer-shell-unstable-v1.c: wlr-layer-shell-unstable-v1.xml
	wayland-scanner -c private-code wlr-layer-shell-unstable-v1.xml wlr-layer-shell-unstable-v1.c

xdg-shell-client.h: xdg-shell.xml
	wayland-scanner -c client-header xdg-shell.xml xdg-shell-client.h

xdg-shell.c: xdg-shell.xml
	wayland-scanner -c private-code xdg-shell.xml xdg-shell.c
endif
