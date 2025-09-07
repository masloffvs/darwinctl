SHELL := /bin/sh

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra

PREFIX      ?= /usr/local
BIN_DIR     := $(PREFIX)/bin
BIN_PATH    := $(BIN_DIR)/darwinctl

LABEL       := com.darwinctl.core
LAUNCHD_DIR := /Library/LaunchDaemons
PLIST_PATH  := $(LAUNCHD_DIR)/$(LABEL).plist
PLIST_TPL   := com.darwinctl.core.plist.in

LOGIN   := $(shell sh -c 'if [ -n "$$SUDO_USER" ]; then echo "$$SUDO_USER"; else id -un; fi')
HOME_DIR := /Users/$(LOGIN)

DAEMON_PATH := /usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
SUDO := $(if $(filter 0,$(shell id -u)),,sudo)

.PHONY: all build install plist validate enable disable reload logs uninstall clean

all: build

build:
	$(CC) $(CFLAGS) -o darwinctl darwinctl.c

plist: $(PLIST_TPL)
	@echo ">> Generating $(PLIST_PATH) for user '$(LOGIN)' (HOME=$(HOME_DIR))"
	$(SUDO) install -m 0755 -d $(LAUNCHD_DIR)
	sed -e 's,@BIN@,$(BIN_PATH),g' \
		-e 's,@LABEL@,$(LABEL),g' \
		-e 's,@LOGIN@,$(LOGIN),g' \
		-e 's,@HOME@,$(HOME_DIR),g' \
		-e 's,@PATH@,$(DAEMON_PATH),g' \
		$(PLIST_TPL) | $(SUDO) tee "$(PLIST_PATH)" >/dev/null
	$(SUDO) chown root:wheel "$(PLIST_PATH)"
	$(SUDO) chmod 0644      "$(PLIST_PATH)"

validate: plist
	@/usr/bin/plutil -lint "$(PLIST_PATH)"

install: build
	@echo ">> Installing binary to $(BIN_PATH)"
	$(SUDO) install -m 0755 -d "$(BIN_DIR)"
	$(SUDO) install -m 0755 darwinctl "$(BIN_PATH)"
	$(MAKE) plist validate
	@echo ">> Done. Run:  make enable"

enable: validate
	@echo ">> bootstrap + kickstart"
	-$(SUDO) launchctl bootstrap system "$(PLIST_PATH)"
	-$(SUDO) launchctl enable system/$(LABEL)
	-$(SUDO) launchctl kickstart -k system/$(LABEL)
	-$(SUDO) launchctl print system/$(LABEL) | sed -n '1,80p'

disable:
	-$(SUDO) launchctl bootout system "$(PLIST_PATH)"

reload:
	$(MAKE) disable
	$(MAKE) enable

logs:
	@echo ">> Daemon stdout/stderr:"
	-$(SUDO) tail -n 200 /var/log/darwinctl-core.out
	-$(SUDO) tail -n 200 /var/log/darwinctl-core.err
	@echo ">> darwinctl app log:"
	- tail -n 200 "$(HOME_DIR)/Library/Logs/darwinctl.log"

uninstall:
	$(MAKE) disable
	-$(SUDO) rm -f "$(PLIST_PATH)"
	-$(SUDO) rm -f "$(BIN_PATH)"

clean:
	-rm -f darwinctl
