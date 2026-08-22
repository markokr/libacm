# NEWS

## Version 1.5

* decode: limit total samples
* decode: fix seeking with WAVC
* libacm: shared lib setup, .pc file
* acmtool: various fixes

## Version 1.4

* decode: Backport ffmpeg fixes
* decode: Remove precalculated tables
* decode: Use `force_chans = 0` for "always trust header" in `acm_open_decoder`
* decode: Make libacm.h usable from C++
* decode: Document parts of API in libacm.h
* decode: Check for alloc failure
* decode: Avoid invalid alloc sizes
* autogen: pass on failures from autoreconf

## Version 1.3

* acmtool: Fix crash with libao 1.x
* plugins: drop, they do not work with any modern player.

## Version 1.2

* decoder: Fix error handling in `load_bits()`   (Brian "Moses" Hall)
* acmtool: Show time instead number of samples.
* plugin-xmms2: New plugin for XMMS2
* plugin-audacious: Support Audacious up to 2.4
* plugin-gstreamer: Fix End-Of-Stream handling
* plugin-xmms: Drop it.  XMMS is obsolete.

## Version 1.1

* acmtool: play files with libao: `acmtool -p FILE`
* decoder: support WAVC files, which are ACM files with additional header.
* decoder: take mono ACM as stereo, as there are lot of badly tagged files.
           Trust channel count on WAVC files, those seem to be correct,
           and WAVC is mostly used for samples.
* decoder: avoid divide-by-zero in bitrate calculation.
* plugin-audacious: Keep up with API changes in Audacious 2.x branch.
           It seems to be still compatible with 1.x, but I have not tested.

## Version 1.0

* decoder: excpect EOF in more places - otherwise libacm
  complained about "unexpected eof".
* decoder: on reading assume there is additional single zero byte
  at the end of all files.  Otherwise last block on some files was
  skipped because last few bits could not be read.

Thanks to above 2 changes libacm now decodes all Fallout 1/2
files in full length and without complaining.

* License plugins under LGPL.
* New separate plugin for Audacious 1.5+
* Remove plugin for Beep Media Player
* Rewrite plugin-gstreamer.c for GStreamer "Hell On Earth" 0.10
* `acmtool -d -o -` writes to stdout.

## Version 0.9.2

* License core libacm code under minimal BSD/ISC license.
  Plugins continue to be under GPL.
* Fix acmtool crash.  (ahalda@cs.mcgill.ca)

## Version 0.9.1 (unreleased)

* Use autoconf/automake for build system
* Plugin for GStreamer 0.8.  (alsasink seems to be buggy for 22050 Hz
  audio.  If you hear any cracks, try to use osssink.)
* Small cleanups.

## Version 0.9

* First public release.
* Includes: decoder, acmtool, plugins for XMMS, BEEP and Winamp
