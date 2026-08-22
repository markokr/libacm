ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src #plugins

EXTRA_DIST = autogen.sh clean.sh

pkgconfig_DATA = $(PACKAGE).pc

#
# html
#

README.html: README
	asciidoc -a toc $<

html: README.html
.PHONY: html

