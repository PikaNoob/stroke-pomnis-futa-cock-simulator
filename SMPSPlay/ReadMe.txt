"SMPS in C" by Robert A. Showalter  -- robert.a.showalter@gmail.com
and Valley Bell
Release 15/02/2012

Feel free to use and distribute this as you please.  I only ask for credit and would enjoy learning of where it goes.


Features:
- Playback of Sonic 1 and Sonic 2 SMPS files (both music and SFX)
- Playback of Sonic 3 & Knuckles SMPS files (quite complete)
- play e.g. Sonic 1 music and Sonic 3 SFX at the same time
- Easily changable DAC sounds and PSG envelopes
- DAC sound can either be raw PCM or "compressed" DPCM (also know as jman2050 compression)
- PSG envelopes can be in Sonic 1/2 or Sonic 3K format
- support for VGM v1.60 logging (including automatic looping)



Changes I since the version of Rob Jinnai:

Release from 22/12/2011:
- removed Allegro and made a Console application out of it
- PSG note, rest, delay, delay behaviour
- fixed tempo for Sonic 1 SMPS files
- added support for S3K-style PSG envelopes (with 81 and 83 end bytes)
- unknown extentions now default to Sonic 1 SMPS instead of "undefined behaviour" (mostly crashes)
- added VGM logging
- added possibility to pause playback by pressing 'P'
- made DAC sounds and PSG envelopes being loaded dynamically (PSGs can still default to hardcoded envelopes)
- made DAC/FM6 behaviour more like in Sonic 1 (i.e. FM6 can be used until the first DAC sound is played)
- replaced YM2612 and SN76489 sound emulators with more accurate ones

Release from 15/02/2012:
- limited Sonic-1-like DAC/FM6 behvaviour to Sonic 1 SMPS files
- made frequency calculation of DAC sounds adjustable via ini-file
- added loop-detection for SMPS music files (doesn't work always)
- added SFX playback
- added most commands from the S3K driver, including some of the special ones exclusive to Flamewing's S&K driver
- added some commands of newer SMPS 68k drivers



Keys:
- Cursor Up/Down - change song
- Space / Enter - play
- V - enable/disable VGM logging (alternatively you can start the player with the commandline parameter -v)
- A - automatic progessing (plays the next song when a song is finished or played 2 full loops)
- R - reload ini-file, PSG flutter data and DAC sounds


File Naming:
In order to detect the SMPS files correctly, they must follow these extention rules:
Music:
- .smp - Sonic 1
- .rom - Sonic 2, uncompressed*
- .sax - Sonic 2, Saxman compressed
- .s3k - Sonic 3 & Knuckles*
- .trs - newer SMPS 68k (e.g. SMPS/Treasure, Phantasy Star IV)

SFX:
- .sfx - Sonic 1 (and other SMPS 68k SFX)
- .sf2 - Sonic 2*
- .sf3 - Sonic 3 & Knuckles*

* with ROM bank offsets

Unknown extentions (like .bin) fall back to .smp.
