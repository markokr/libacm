
AUTOMAKE_OPTIONS = 1.6 foreign
ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src #plugins

EXTRA_DIST = autogen.sh

#
# html
#

README.html: README
	asciidoc -a toc $<

html: README.html
.PHONY: html

