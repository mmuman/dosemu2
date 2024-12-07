# Makefile for DOSEMU
#

all: default

srcdir=.
top_builddir=.
ifeq ($(filter deb rpm flatpak-% %/configure configure,$(MAKECMDGOALS)),)
  -include Makefile.conf
endif
REALTOPDIR ?= $(abspath $(srcdir))

$(REALTOPDIR)/configure configure: $(REALTOPDIR)/configure.ac
	cd $(@D) && autoreconf --install -v -I m4

Makefile.conf config.status etc/dosemu.desktop: $(REALTOPDIR)/configure
ifeq ($(findstring $(MAKECMDGOALS), clean realclean pristine distclean),)
	@echo "Running configure ..."
	$<
else
	$< || true
endif

install: changelog

default install: config.status src/include/config.hh etc/dosemu.desktop
	@$(MAKE) -C man $@
	@$(MAKE) -C src $@

clean realclean:
	@$(MAKE) -C man $@
	@$(MAKE) -C src $@

uninstall:
	@$(MAKE) -C src uninstall

docs:
	@$(MAKE) -C src/doc all
	@$(MAKE) -C src/doc install

docsclean:
	@$(MAKE) -C src/doc clean

GIT_REV := $(shell $(REALTOPDIR)/git-rev.sh $(REALTOPDIR) $(top_builddir))
.LOW_RESOLUTION_TIME: $(GIT_REV)

$(PACKETNAME).tar.gz: $(GIT_REV) changelog
	rm -f $(PACKETNAME).tar.gz
	(cd $(REALTOPDIR); git archive -o $(abs_top_builddir)/$(PACKETNAME).tar --prefix=$(PACKETNAME)/ HEAD)
	tar rf $(PACKETNAME).tar --transform 's,^,$(PACKETNAME)/,' --add-file=changelog; \
	if [ -f "$(fdtarball)" ]; then \
		tar -Prf $(PACKETNAME).tar --transform 's,^$(dir $(fdtarball)),$(PACKETNAME)/,' --add-file=$(fdtarball); \
	fi
	gzip $(PACKETNAME).tar

dist: $(PACKETNAME).tar.gz

rpm: dosemu2.spec
	tito build --test --rpm

deb:
	debuild -i -us -uc -b

changelog:
	if [ -d $(top_srcdir)/.git -o -f $(top_srcdir)/.git ]; then \
		git --git-dir=$(top_srcdir)/.git log >$@ ; \
	else \
		echo "Unofficial build by `whoami`, `date`" >$@ ; \
	fi

log: changelog

tests:
	python3 test/test_dos.py PPDOSGITTestCase

pristine distclean mrproper:  Makefile.conf docsclean
	@$(MAKE) -C src pristine
	rm -f Makefile.conf
	rm -f $(PACKETNAME).tar.gz
	rm -f ChangeLog
	rm -f `find . -name config.cache`
	rm -f `find . -name config.status`
	rm -f `find . -name config.log`
	rm -f `find . -name aclocal.m4`
	rm -f `find . -name configure`
	rm -f `find . -name Makefile.conf`
	rm -rf `find . -name autom4te*.cache`
	rm -f debian/$(PACKAGE_NAME).*
	rm -rf debian/$(PACKAGE_NAME)
	rm -f debian/*-stamp
	rm -f debian/files
	rm -f src/include/config.hh
	rm -f src/include/stamp-h1
	rm -f src/include/config.hh.in
	rm -f src/include/version.hh
	rm -f `find . -name '*~'`
	rm -f `find . -name '*[\.]o'`
	rm -f `find src -type f -name '*.d'`
	rm -f `find . -name '*[\.]orig'`
	rm -f `find . -name '*[\.]rej'`
	rm -f gen*.log
	rm -f config.sub config.guess
	rm -rf 2.*
	rm -rf autom4te.cache
	$(REALTOPDIR)/scripts/mkpluginhooks clean

tar: distclean
	VERSION=`cat VERSION` && cd .. && tar czvf dosemu-$$VERSION.tgz dosemu-$$VERSION

flatpak-build:
	flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
	flatpak-builder --ccache --force-clean --user --repo=repo \
	  --install-deps-from=flathub \
	  --install builddir io.github.dosemu2.dosemu2.yml

flatpak-run:
	flatpak run io.github.dosemu2.dosemu2
