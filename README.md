This builds on sng2swm code released by Hermit - Mihaly Horvath and Conrad/Samar as part of Sid Wizard - https://csdb.dk/release/?id=261127

It uses sng.h and swm.h that were already released as part of Sid Wizard.

It attempts to generate working Goattracker files from Sid-Wizard files.

Build instructions: g++ -std=c++17 -O2 -s swm2sng.cpp -o swm2sng

Usage: swm2sng [--no-filter-clone] <input.swm> [output.sng]

  --no-filter-clone  Use simple per-voice filter mask without instrument
                     cloning.  Faster but less accurate filter routing.


Due to the differences in the way these programs use filters and chords, there are likely to be a number of bugs - please test and report any unexpected effects.

This program was coded using Claude Sonnet 4.6

Authored by James Stone/RS-232.
Thanks to Hermit for invaluable input on the development and testing.

This software released under the "unlicense".
