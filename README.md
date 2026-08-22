# libacm

## Intro

Library for InterPlay ACM Audio format.
Includes command line tool.

The format was used for games Fallout, Fallout 2 and Baldur's Gate series.
It is less efficient than the more advanced MP3, Vorbis or AAC algorithms,
but it's main feature is very fast decoding speed. 

### FFmpeg

This decoder was merged into FFmpeg 3.0.  Now ACM files can be played
via ffmpeg commands or using libavcodec.

## Installation

    ./configure [--prefix=...] [--enable-shared]
    make
    make install

## Command line tools usage

    $ acmtool -h
    acmtool - libacm version 1.0
    Decode: acmtool -d [-q][-m|-s] [-r|-n] -o outfile infile
            acmtool -d [-q][-m|-s] [-r|-n] infile [infile ...]
    Other:  acmtool -i ACMFILE [ACMFILE ...]
            acmtool -M|-S ACMFILE [ACMFILE ...]
    Commands:
      -d     decode audio into WAV files
      -p     play audio
      -i     show info about ACM files
      -M     modify ACM header to have 1 channel
      -S     modify ACM header to have 2 channels
    Switches:
      -m     force mono wav
      -s     force stereo wav
      -r     raw output - no wav header
      -q     be quiet
      -n     no output - for benchmarking
      -o FN  output to file, can be used if single source file

The mono/stereo options are necessary because for some ACM files
the number of channels in header in wrong.  Usually those are
game samples, which contain mono audio but are tagger as stereo.

## Credits

All the hard work of reverse engineering the format was done
by ABel from TeamX. I simply re-implemented it in C.

