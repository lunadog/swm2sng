This builds on sng2swm code released by Hermit - Mihaly Horvath and Conrad/Samar as part of Sid Wizard - https://csdb.dk/release/?id=261127

It attempts to generate working Goattracker files from Sid-Wizard files.

Build instructions: g++ -std=c++17 -O2 -s swm2sng.cpp -o swm2sng

Usage instructions: swm2sng <swm file> <optional - name of sng file to output>

Due to the differences in the way these programs use filters and chords, there are likely to be a number of bugs - please test and report any unexpected effects.

This program was vibe coded using Claude Sonnet 4.6
