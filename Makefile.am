ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src

EXTRA_DIST = autogen.sh clean.sh .clang-format

pkgconfig_DATA = $(PACKAGE).pc

fmt:
	clang-format -i src/*.[ch]
