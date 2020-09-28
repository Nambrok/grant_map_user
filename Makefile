HOST_SYS=root@192.168.1.32
GUEST_SYS=root@dtt-gntdev.localdomain
DIR:=host guest
EXE:=$(addsuffix _main,$(DIR))

all: build_host

define MAKE_DIR
$(MAKE) -C $(1);
endef

host_main: host
	$(MAKE) -C $<

guest_main: guest
	$(MAKE) -C $<

REMOTE_SYS:=$(HOST_IP)

ifdef HOST_SYS
rsync_host:
	rsync -aruv . $(HOST_SYS):grant_map/
build_host: rsync_host
	ssh $(HOST_SYS) "$(MAKE) -C grant_map host_main"
endif

ifdef GUEST_SYS
rsync_guest:
	rsync -aruv . $(GUEST_SYS):grant_map/
build_guest: rsync_guest
	ssh $(GUEST_SYS) "$(MAKE) -C grant_map guest_main"
endif

ifdef HOST_SYS
ifdef GUEST_SYS
remote: build_host build_guest
endif
endif

.PHONY: remote all
