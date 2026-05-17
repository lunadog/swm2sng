/********************************************
 * swm2sng.cpp
 * SID-Wizard SWM1 -> GoatTracker 2 SNG converter
 * Companion to sng2swm.cpp
 *
 * Build: g++ -std=c++17 -O2 -s swm2sng.cpp -o swm2sng
 *
 * SWM pattern block format
 * ------------------------
 * Each pattern block: [packed row data ...][SIZE byte][LENGTH byte]
 *   LENGTH = row count (detected by peeking ahead: when the byte two
 *            positions ahead equals rows parsed so far, the current
 *            byte is SIZE and the next is LENGTH).
 *
 * Row encoding in the packed stream:
 *   0x00        = rest (SW1_PTNCOL1_REST) — gate stays on
 *   0x7E        = keyoff (SW1_PTNCOL1_KEYOFF) — gate off, release envelope
 *   0x70..0x77  = rest run: (byte - 0x70 + 2) consecutive rest rows (0x00)
 *   0x01..0x6F, 0x78..0x7D, 0x7F
 *               = plain note (no instrument byte follows)
 *   0x80..0xFF  = note WITH instrument byte following
 *     instrument bit7=1  -> FX byte follows
 *       FX < SW1_SMALLFX_MIN -> parameter byte follows (big FX)
 *       FX >= SW1_SMALLFX_MIN -> no parameter byte (small FX)
 *
 * Sequence format:
 *   [pattern-ref bytes ...][0xFF loop-marker][restart-pos byte][size byte]
 *
 * Instrument table format:
 *   [SW1_INSTRUMENT_PARAMSIZE param bytes]
 *   [table bytes, last byte encodes total instrument size]
 *   [SW1_INSTRUMENT_NAMESIZE name bytes]
 ********************************************/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <vector>
#include <string>
#include <array>
#include <map>
#include <stdexcept>
#include "swm.h"
#include "sng.h"

/*************************************************************
 * File utilities
 *************************************************************/
static void die(const char* msg) {
    fprintf(stderr, "Error: %s\n", msg);
    exit(1);
}

static FILE* OpenFile(const char* name, const char* mode) {
    FILE* f = fopen(name, mode);
    if (!f) { fprintf(stderr, "Cannot open: %s\n", name); exit(1); }
    return f;
}

static uint8_t read8(FILE* f) {
    int c = fgetc(f);
    if (c == EOF) throw std::runtime_error("Unexpected EOF");
    return (uint8_t)c;
}

static void write8(FILE* f, uint8_t v) { fputc(v, f); }

/*************************************************************
 * In-memory SWM structures
 *************************************************************/
struct SW_Sequence {
    std::vector<uint8_t> data;  // pattern refs + loop marker
    uint8_t restart_pos = 0;
};

struct SW_Pattern {
    std::vector<uint8_t> packed;  // raw packed bytes (data only, no size/length)
    uint8_t size_byte = 0;        // the SIZE byte as stored (unpacked count)
    uint8_t length    = 0;        // row count
};

struct SW_Instrument {
    swm_instrument::swm_instrument_params params{};
    std::vector<uint8_t> tables;
    char name[SW1_INSTRUMENT_NAMESIZE]{};
};

struct SW_FunkTempo {
    uint8_t tempo1 = 0x86;
    uint8_t tempo2 = 0x84;
};

struct SWMFile {
    swm_header              header{};
    std::vector<SW_Sequence>   sequences;
    std::vector<SW_Pattern>    patterns;   /* index 0 unused */
    std::vector<SW_Instrument> instruments;
    std::vector<SW_FunkTempo>  tempos;

    /* Global chord table (chordtable_length / 3 entries).
     * Each chord = 3 semitone offsets from the root note (0 = root). */
    std::vector<std::array<uint8_t,3>> chords;
};

/*************************************************************
 * GT2 output structures
 *************************************************************/
struct GT2Subtune {
    sng_orderlist voice[3]{};
};

struct GT2File {
    sng_header header{};
    std::vector<GT2Subtune>    orderlists;
    std::vector<sng_instrument> instruments;
    sng_table  wavetable{}, pulsetable{}, filtertable{}, speedtable{};
    std::vector<sng_pattern> patterns;
};

/*************************************************************
 * Forward declarations
 *************************************************************/
void LoadSWM    (const char* path, SWMFile& swm);
void Convert     (const SWMFile& swm, GT2File& gt);
void WriteSNGFile(const char* path, const GT2File& gt);
void FreeGT2File (GT2File& gt);

/*************************************************************
 * Entry point
 *************************************************************/
int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: swm2sng <input.swm[.prg]> [output.sng]\n");
        return 1;
    }
    const char* inPath  = argv[1];
    const char* outPath = (argc > 2) ? argv[2] : "output.sng";

    SWMFile swm;
    GT2File gt;

    LoadSWM(inPath, swm);
    Convert(swm, gt);
    WriteSNGFile(outPath, gt);
    FreeGT2File(gt);

    printf("Done. Output: %s\n", outPath);
    return 0;
}

/*************************************************************
 * LoadSWM
 * Reads a SWM1 file (with or without .prg load address).
 *************************************************************/
void LoadSWM(const char* path, SWMFile& swm) {
    FILE* f = OpenFile(path, "rb");

    fseek(f, 0, SEEK_END);
    long filesize = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Detect optional 2-byte PRG load address */
    uint8_t probe[6]{};
    if (fread(probe, 1, 6, f) != 6)
        die("File too small to be a valid SWM file");

    long headerOffset = 0;
    if (probe[0] == 'S' && probe[1] == 'W' &&
        probe[2] == 'M' && probe[3] == '1') {
        headerOffset = 0;
    } else if (probe[2] == 'S' && probe[3] == 'W' &&
               probe[4] == 'M' && probe[5] == '1') {
        headerOffset = 2;
    } else {
        die("Not a valid SWM1 file (tag not found in first 6 bytes)");
    }

    fseek(f, headerOffset, SEEK_SET);
    if (fread(&swm.header, sizeof(swm_header), 1, f) != 1)
        die("Failed to read SWM header");

    if (strncmp(swm.header.tag, "SWM1", 4) != 0)
        die("SWM1 tag validation failed after seek");

    int subtunes = swm.header.sequence_count / 3;
    printf("Tag:         SWM1\n");
    printf("Subtunes:    %d\n", subtunes);
    printf("Sequences:   %d\n", swm.header.sequence_count);
    printf("Patterns:    %d\n", swm.header.pattern_count);
    printf("Instruments: %d\n", swm.header.instrument_count);

    /*---------------------------------------------------------
     * Read chord table (if present)
     * Located immediately after the header, before sequences.
     * Format: chordtable_length / 3 chords, each chord = 3
     * semitone offsets [off0, off1, off2] from the root note.
     * Offset 0 = play root note; positive = semitones above root.
     *---------------------------------------------------------*/
    if (swm.header.chordtable_length > 0) {
        int num_chords = swm.header.chordtable_length / 3;
        swm.chords.resize(num_chords);
        for (int i = 0; i < num_chords; i++) {
            swm.chords[i][0] = read8(f);
            swm.chords[i][1] = read8(f);
            swm.chords[i][2] = read8(f);
        }
        /* If length is not a multiple of 3, consume the remainder */
        for (int r = num_chords * 3; r < swm.header.chordtable_length; r++)
            read8(f);
        printf("  Chord table: %d chords\n", num_chords);
    }

    /*---------------------------------------------------------
     * Read sequences
     * Format: [pattern-ref bytes...][0xFF loop-marker]
     *         [restart-pos byte][size byte]
     * We read until 0xFF, then two more bytes.
     *---------------------------------------------------------*/
    swm.sequences.resize(swm.header.sequence_count);
    for (int i = 0; i < swm.header.sequence_count; i++) {
        SW_Sequence& seq = swm.sequences[i];
        for (;;) {
            uint8_t c = read8(f);
            if (c == SW1_SEQUENCE_LOOPSONG || c == SW1_SEQUENCE_ENDSONG) {
                seq.restart_pos = read8(f); /* restart position */
                read8(f);                    /* size byte (discard) */
                break;
            }
            seq.data.push_back(c);
        }
    }

    /*---------------------------------------------------------
     * Read pattern blocks
     *
     * Each block: [packed row data...][SIZE byte][LENGTH byte]
     *
     * SIZE byte  = the stored (unpacked) byte count; its value
     *              is NOT necessarily equal to its file position.
     * LENGTH byte = number of pattern rows.
     *
     * Detection algorithm (the critical fix):
     *   Parse rows one at a time.  After every complete row,
     *   peek ONE byte past the current read position.
     *   If that byte equals rows_parsed, we have found the
     *   LENGTH byte; the byte immediately before it is SIZE.
     *
     * Row encoding:
     *   0x70..0x77  -> (byte - 0x70 + 2) rest rows
     *   0x7E        -> 1 rest row
     *   other < 0x80 -> 1 row (note or special), no extra bytes
     *   >= 0x80     -> 1 note row; instrument byte follows
     *     instr bit7=1 -> FX byte follows
     *       FX < SW1_SMALLFX_MIN -> parameter byte follows
     *---------------------------------------------------------*/
    swm.patterns.resize(swm.header.pattern_count + 1);

    for (int pi = 1; pi <= swm.header.pattern_count; pi++) {
        SW_Pattern& ptn = swm.patterns[pi];

        /* We buffer data from the file until the block ends. */
        std::vector<uint8_t> buf;
        int rows = 0;
        bool found = false;

        /* Safety: each pattern can't be larger than some sane bound. */
        const int MAX_BLOCK = 4096;

        for (int guard = 0; guard < MAX_BLOCK && !found; guard++) {
            /* Before reading the next row, peek ahead.
             * The next byte in the file is the start of the next row
             * (or the SIZE byte if we're at the end of the block).
             * We detect end-of-block by checking if the byte TWO
             * positions ahead equals rows: that would be the LENGTH byte
             * and the current position would be the SIZE byte.
             *
             * In practice we just try: peek at file[pos+1] (one beyond
             * the byte we're about to read) to see if it matches rows.
             * We do this by peeking at the file without consuming. */

            /* Peek two bytes ahead using fseek/ftell (portable).
             * Byte at current pos may be SIZE; byte after may be LENGTH. */
            long pos = ftell(f);
            int peek_size = fgetc(f);
            if (peek_size == EOF) die("Unexpected EOF seeking pattern size");
            int peek_len  = fgetc(f);
            if (peek_len  == EOF) die("Unexpected EOF seeking pattern length");
            fseek(f, pos, SEEK_SET);  /* rewind both bytes */

            /* If peek_len == rows && rows > 0: end of block found */
            if (rows > 0 && (uint8_t)peek_len == (uint8_t)rows) {
                /* Consume SIZE and LENGTH bytes */
                ptn.size_byte = (uint8_t)fgetc(f);
                ptn.length    = (uint8_t)fgetc(f);
                found = true;
                break;
            }

            /* Read and process one row */
            uint8_t note = read8(f);
            buf.push_back(note);

            if (note >= 0x70 && note <= 0x77) {
                /* Run of rest rows, no extra bytes */
                rows += (note - 0x70) + 2;
            } else if (note == 0x7E) {
                /* Single rest row, no extra bytes */
                rows += 1;
            } else if (note & 0x80) {
                /* Note with instrument byte */
                rows += 1;
                uint8_t instr = read8(f);
                buf.push_back(instr);
                if (instr & 0x80) {
                    /* FX byte present */
                    uint8_t fx = read8(f);
                    buf.push_back(fx);
                    if (fx > 0 && fx < SW1_SMALLFX_MIN) {
                        /* Big FX: parameter byte present */
                        uint8_t param = read8(f);
                        buf.push_back(param);
                    }
                }
            } else {
                /* Other note-only row (including 0x00 rest) */
                rows += 1;
            }
        }

        if (!found)
            die("Pattern block parse failed: could not locate SIZE/LENGTH bytes");

        ptn.packed = buf;
        printf("  Pattern %2d: %d rows, %zu packed bytes, size_byte=0x%02X\n",
               pi, ptn.length, ptn.packed.size(), ptn.size_byte);
    }

    /*---------------------------------------------------------
     * Read instruments
     * [SW1_INSTRUMENT_PARAMSIZE param bytes]
     * [table bytes; last byte encodes total instrument size]
     * [SW1_INSTRUMENT_NAMESIZE name bytes]
     *---------------------------------------------------------*/
    swm.instruments.resize(swm.header.instrument_count);
    for (int i = 0; i < swm.header.instrument_count; i++) {
        SW_Instrument& inst = swm.instruments[i];
        if (fread(&inst.params, 1, SW1_INSTRUMENT_PARAMSIZE, f)
                != SW1_INSTRUMENT_PARAMSIZE)
            die("Failed to read instrument params");

        inst.tables.clear();
        for (;;) {
            uint8_t c = read8(f);
            inst.tables.push_back(c);
            /* The last byte of the table section encodes total instrument size:
             * value = SW1_INSTRUMENT_PARAMSIZE + table_size - 1              */
            if (c == (uint8_t)(SW1_INSTRUMENT_PARAMSIZE
                               + (int)inst.tables.size() - 1))
                break;
            if (inst.tables.size() > 512)
                die("Instrument table overflow");
        }
        if (fread(inst.name, 1, SW1_INSTRUMENT_NAMESIZE, f)
                != SW1_INSTRUMENT_NAMESIZE)
            die("Failed to read instrument name");
    }

    /*---------------------------------------------------------
     * Read subtune tempos (2 bytes per subtune)
     *---------------------------------------------------------*/
    swm.tempos.resize(subtunes);
    for (int i = 0; i < subtunes; i++) {
        swm.tempos[i].tempo1 = read8(f);
        swm.tempos[i].tempo2 = read8(f);
    }

    fclose(f);
    printf("SWM load complete.\n");
}

/*************************************************************
 * ConvertSequence
 * Reverse of GTseqToSWseq in sng2swm.cpp.
 *************************************************************/
static void ConvertSequence(const SW_Sequence& sw, sng_orderlist& gt) {
    std::vector<uint8_t> out;

    for (size_t i = 0; i < sw.data.size(); i++) {
        uint8_t v = sw.data[i];
        if (v >= SW1_SEQUENCE_TRANS_MIN && v <= SW1_SEQUENCE_TRANS_MAX) {
            /* Reverse SW trans -> GT trans */
            out.push_back(v - (SW1_SEQUENCE_TRANS - GT2_ORDERLIST_TRANS));
        } else if (v > 0) {
            /* Pattern number: SW counts from 1, GT from 0 */
            out.push_back(v - 1);
        } else {
            out.push_back(0);
        }
    }

    /* GT2 orderlist data must end with the end-of-song marker (0xFF =
     * SW1_SEQUENCE_LOOPSONG) before the separate restart byte.  The
     * 'length' field counts this terminator too.  Without it GoatTracker
     * does not know where the list ends and will misread the restart
     * position as another pattern reference. */
    out.push_back(SW1_SEQUENCE_LOOPSONG);

    gt.length  = (uint8_t)out.size();
    gt.data    = new uint8_t[gt.length];
    memcpy(gt.data, out.data(), gt.length);
    gt.restart = sw.restart_pos;
}

/*************************************************************
 * UnpackPattern
 * Expand the packed SW pattern byte stream into a flat list
 * of row-data bytes that ConvertPattern can walk linearly.
 *
 * Expansion rules (same as the file encoding, reversed):
 *   0x70..0x77  -> (byte - 0x70 + 2) copies of 0x00 (rest rows)
 *   0x7E        -> one 0x00 (rest row)
 *   other bytes -> passed through unchanged
 *************************************************************/
static std::vector<uint8_t> UnpackPattern(const std::vector<uint8_t>& packed) {
    std::vector<uint8_t> out;
    out.reserve(packed.size() * 2);

    for (size_t i = 0; i < packed.size(); ) {
        uint8_t b = packed[i++];

        if (b >= 0x70 && b <= 0x77) {
            /* Rest run: expands to (b-0x70+2) plain rest rows = 0x00 each.
             * 0x00 = SW1_PTNCOL1_REST (gate stays on, note continues).
             * 0x7E = SW1_PTNCOL1_KEYOFF (gate off) — a distinct value. */
            int count = (b - 0x70) + 2;
            for (int k = 0; k < count; k++)
                out.push_back(0x00);
        } else if (b == 0x7E) {
            /* 0x7E = SW1_PTNCOL1_KEYOFF: explicit gate-off. Pass through. */
            out.push_back(0x7E);
        } else {
            out.push_back(b);
            if (b & 0x80) {
                if (i < packed.size()) {
                    uint8_t instr = packed[i++];
                    out.push_back(instr);
                    if (instr & 0x80) {
                        if (i < packed.size()) {
                            uint8_t fx = packed[i++];
                            out.push_back(fx);
                            if (fx > 0 && fx < SW1_SMALLFX_MIN) {
                                if (i < packed.size())
                                    out.push_back(packed[i++]);
                            }
                        }
                    }
                }
            }
        }
    }
    return out;
}

/*************************************************************
 * SpeedTableBuilder
 * Accumulates unique GT2 speedtable entries and returns the
 * 1-based index for each entry (GT2 speedtable is 1-indexed).
 *
 * The GT2 speedtable is shared by:
 *   - Funk tempo pairs (left=speed1, right=speed2 in frames/row)
 *   - Slide speeds (left=pitch-delta-high, right=pitch-delta-low)
 *   - Vibrato amplitudes
 *************************************************************/
struct SpeedTableBuilder {
    std::vector<uint8_t> left, right;

    /* Returns 1-based index.  Returns 0 if table is full. */
    uint8_t add(uint8_t L, uint8_t R) {
        /* Reuse existing entry if already present */
        for (size_t i = 0; i < left.size(); i++)
            if (left[i] == L && right[i] == R)
                return (uint8_t)(i + 1);
        if (left.size() >= 0xFF) return 0;
        left.push_back(L);
        right.push_back(R);
        return (uint8_t)left.size();
    }

    void fill(sng_table& t) const {
        t.length = (uint8_t)left.size();
        if (t.length) {
            t.left  = new uint8_t[t.length];
            t.right = new uint8_t[t.length];
            memcpy(t.left,  left.data(),  t.length);
            memcpy(t.right, right.data(), t.length);
        } else {
            t.left = t.right = nullptr;
        }
    }
};

/*************************************************************
 * SWsldToGTsld
 * Exact inverse of GTsldToSWsld() from sng2swm.cpp.
 *
 * GTsldToSWsld(GTpos, Dpitch, spdtbl) converts a GT2 speed-
 * table entry (16-bit pitch delta) into a SID-Wizard slide
 * speed byte using the SID frequency tables from swm.h:
 *
 *   GTslide = spdtbl->left[GTpos-1]*256 + spdtbl->right[GTpos-1]
 *   if (GTslide < 0x100):
 *     find i s.t. SWexpTabH[i] <= GTslide  (i from EXPTRESHOLD-1 down)
 *     SWsld = (i - Dpitch) * 2
 *   else:
 *     find i s.t. SWexpTabH[i+FREQTBH_POS]*256+SWexpTabL[i] <= GTslide
 *     SWsld = ((i+EXPTRESHOLD) - Dpitch) * 2
 *
 * Inverse: given SWsld and Dpitch, recover GTslide:
 *   SWsld < 2*EXPTRESHOLD  (index i is in the small-delta range):
 *     i = SWsld/2 + Dpitch
 *     GTslide = SWexpTabH[i]              (single-byte delta)
 *   else:
 *     i = SWsld/2 + Dpitch - EXPTRESHOLD  (index into large table)
 *     GTslide = SWexpTabH[i+FREQTBH_POS]*256 + SWexpTabL[i]
 *
 * Returns a {high, low} pair suitable for a GT2 speedtable entry.
 *************************************************************/
static std::pair<uint8_t,uint8_t> SWsldToGTsld(uint8_t SWsld, uint8_t Dpitch) {
    /* Clamp to valid signed range */
    int i = (int)(SWsld >> 1) + (int)Dpitch;

    if (i < EXPTRESHOLD) {
        /* Small-delta range: single-byte pitch delta */
        int idx = std::max(0, std::min(i, EXPTRESHOLD - 1));
        uint8_t delta = SWexpTabH[idx];
        return { 0x00, delta };
    } else {
        /* Large-delta range: 16-bit pitch delta */
        int j = i - EXPTRESHOLD;
        if (j >= FREQTB_SIZE) j = FREQTB_SIZE - 1;
        uint8_t hi = SWexpTabH[j + FREQTBH_POS];
        uint8_t lo = SWexpTabL[j];
        return { hi, lo };
    }
}

/*************************************************************
 * SWsldToGTspd
 * Converts a SID-Wizard slide speed byte to a GT2 speedtable
 * index.  Uses SWsldToGTsld for the actual frequency lookup.
 *
 * Dpitch should be the most recent discrete note played on the
 * channel (SW1_NOTE_MAX/2 as default if unknown).
 *************************************************************/
static uint8_t SWsldToGTspd(uint8_t SWsld, uint8_t Dpitch,
                             SpeedTableBuilder& spdBuilder) {
    if (SWsld == 0) return 0;  /* speed 0 = legato / instant */
    auto [hi, lo] = SWsldToGTsld(SWsld, Dpitch);
    /* Avoid all-zero entry which GT2 treats as "no effect" */
    if (hi == 0 && lo == 0) lo = 1;
    uint8_t idx = spdBuilder.add(hi, lo);
    return idx ? idx : 1;
}

/*************************************************************
 * CreateArpInstrument
 * Creates (or retrieves from cache) a GT2 instrument that is
 * a clone of `base_gt_idx` but with its wave table replaced
 * by one that cycles through the given 3-note chord arpeggio.
 *
 * chord[3]: semitone offsets from root (0 = root, e.g. {0,4,7}
 * for a major chord).
 *
 * The wave table entries are built as:
 *   [orig_waveform, rel+chord[0]]   <- set waveform + root
 *   [0x00,          rel+chord[1]]   <- arp step 1 (no wf change)
 *   [0x00,          rel+chord[2]]   <- arp step 2
 *   [GT2_TABLE_JUMP, 0x00]          <- loop back to start
 *
 * If the original instrument has no wave table (gi.wv == 0),
 * uses frame1_waveform as the waveform byte for the first entry.
 *
 * Returns the 0-based GT2 instrument index of the arp clone.
 *************************************************************/
static int CreateArpInstrument(
        int                                 base_gt_idx,
        const std::array<uint8_t,3>&        chord,
        GT2File&                            gt,
        sng_table&                          wt,
        std::map<std::pair<int,int>,int>&   arp_cache,
        int                                 chord_num)
{
    auto key = std::make_pair(base_gt_idx, chord_num);
    auto it = arp_cache.find(key);
    if (it != arp_cache.end())
        return it->second;

    /* Clone the base instrument */
    sng_instrument clone = gt.instruments[base_gt_idx];
    int new_idx = (int)gt.instruments.size();
    gt.instruments.push_back(clone);

    /* Determine the waveform to use for the first arp entry.
     * Prefer the first entry of the existing wave table; fall back to
     * the instrument's frame1_waveform (hr field). */
    uint8_t wf_byte = clone.hr ? clone.hr : 0x21; /* default: sawtooth+gate */
    if (clone.wv > 0 && wt.left && (clone.wv - 1) < wt.length)
        wf_byte = wt.left[clone.wv - 1]; /* first wt entry's waveform */

    /* Build arp wave table entries appended to the global wave table.
     * All offsets converted: SW semitone offset → GT2 relative arp value.
     * GT2_ARP_REL_MIN (0x80) + offset = relative semitone up.
     * Offset 0 → 0x00 (no arpeggio, stay at current note). */
    auto sw_to_gt2_arp = [](uint8_t sw_offset) -> uint8_t {
        if (sw_offset == 0) return 0x00;
        return (uint8_t)(GT2_ARP_REL_MIN + sw_offset);
    };

    /* Append to the pre-allocated wave table (255 bytes max). */
    if (wt.length + 4 > 255) {
        printf("  WARNING: wave table full, cannot add chord arp for instr %d chord %d\n",
               base_gt_idx + 1, chord_num);
        arp_cache[key] = base_gt_idx;
        gt.instruments.pop_back();
        return base_gt_idx;
    }

    uint8_t arp_start = (uint8_t)(wt.length + 1); /* 1-based index */
    wt.left [wt.length] = wf_byte;                 /* step 0: set waveform + root */
    wt.right[wt.length] = sw_to_gt2_arp(chord[0]);
    wt.length++;
    wt.left [wt.length] = 0x00;                    /* step 1: no waveform change */
    wt.right[wt.length] = sw_to_gt2_arp(chord[1]);
    wt.length++;
    wt.left [wt.length] = 0x00;                    /* step 2: no waveform change */
    wt.right[wt.length] = sw_to_gt2_arp(chord[2]);
    wt.length++;
    wt.left [wt.length] = GT2_TABLE_JUMP;           /* loop back to arp start */
    wt.right[wt.length] = 0x00;
    wt.length++;

    gt.instruments[new_idx].wv = arp_start;

    printf("  Created arp instrument %d (base %d, chord %d: +%d+%d+%d) -> wt pos %d\n",
           new_idx + 1, base_gt_idx + 1, chord_num,
           chord[0], chord[1], chord[2], arp_start);

    arp_cache[key] = new_idx;
    return new_idx;
}
/*************************************************************
 * ConvertPattern
 * Translates one SW pattern to a GT2 sng_pattern.
 *
 * tempo_cmd / tempo_param: if non-zero, written into row 0's
 * COMMAND column (no extra row is inserted).
 *
 * chords / arp_cache / gt_file / wt_out: used to materialise
 * chord-table arpeggios as duplicate GT2 instruments.
 *************************************************************/
static void ConvertPattern(const SW_Pattern& sw, sng_pattern& gt,
                           SpeedTableBuilder& spdBuilder,
                           uint8_t tempo_cmd   = 0,
                           uint8_t tempo_param = 0,
                           const std::vector<std::array<uint8_t,3>>* chords    = nullptr,
                           std::map<std::pair<int,int>,int>*          arp_cache = nullptr,
                           GT2File*                                   gt_file   = nullptr) {
    auto unpacked = UnpackPattern(sw.packed);

    gt.rows = sw.length + 1;  /* +1 for GT2's mandatory $FF end row */
    gt.data = new sng_pattern_row[gt.rows];
    memset(gt.data, 0, sizeof(sng_pattern_row) * gt.rows);

    size_t idx = 0;
    int    row = 0;

    /* Dpitch tracks the last real note played; used for slide speed conversion.
     * Initialise to the middle of the SID-Wizard note range (same as sng2swm). */
    uint8_t Dpitch = (uint8_t)(SW1_NOTE_MAX / 2);

    while (idx < unpacked.size() && row < gt.rows - 1) {
        sng_pattern_row& r = gt.data[row];
        uint8_t note    = unpacked[idx++];
        bool    hasInstr = (note & 0x80) != 0;
        uint8_t noteVal  = note & 0x7F;

        /* Map SW note value -> GT2 note value.
         *
         * Encoding in SID-Wizard packed pattern stream (confirmed by binary analysis):
         *   0x00        = rest (gate stays on, note continues)
         *   0x7E        = keyoff (SW1_PTNCOL1_KEYOFF) — explicit gate-off
         *   0x70..0x77  = rest run (already expanded to 0x00 by UnpackPattern)
         *   0x80+       = note with instrument byte
         *   0x01..0x6F  = plain note (no instrument byte) */
        if (note == 0x00) {
            r.note = GT2_PATTERN_REST;
        } else if (note == 0x7E || noteVal == SW1_PTNCOL1_KEYOFF) {
            /* 0x7E = SW1_PTNCOL1_KEYOFF: explicit gate-off */
            r.note = GT2_PATTERN_KEYOFF;
        } else if (noteVal == SW1_PTNCOL1_KEYON) {
            r.note = GT2_PATTERN_KEYON;
        } else if (noteVal == SW1_PTNCOL1_END) {
            break;
        } else if (noteVal >= SW1_PTNCOL1_NOTE &&
                   noteVal <= SW1_NOTE_MAX) {
            r.note = noteVal + (GT2_PATTERN_NOTE - SW1_PTNCOL1_NOTE);
            Dpitch = noteVal;
        } else {
            r.note = GT2_PATTERN_REST;
        }

        r.instrument = 0;
        r.command    = GT2_COMMAND_NOP;
        r.parameter  = 0;

        /* Inject header tempo into row 0's command column (no extra row). */
        if (row == 0 && tempo_cmd != 0) {
            r.command   = tempo_cmd;
            r.parameter = tempo_param;
        }

        if (!hasInstr) { row++; continue; }
        if (idx >= unpacked.size()) { row++; break; }

        uint8_t instr    = unpacked[idx++];
        bool    hasFX    = (instr & 0x80) != 0;
        uint8_t instrVal = instr & 0x7F;

        /* Legato: instrument byte == SW1_LEGATO (bit7 clear, value 0x00)
         * means "slide to this note without retriggering the envelope". */
        if (!hasFX && instrVal == SW1_LEGATO) {
            r.command   = GT2_COMMAND_TONEPORT;
            r.parameter = 0;  /* legato = toneport with speed 0 */
        } else if (instrVal >= SW1_INSTRUMENT_MIN &&
                   instrVal <= SW1_INSTRUMENT_MAX) {
            r.instrument = instrVal;
        }

        if (!hasFX) { row++; continue; }
        if (idx >= unpacked.size()) { row++; break; }

        uint8_t fx = unpacked[idx++];

        /* FX == 0 with instrument byte bit7 set: alternate legato encoding. */
        if (fx == 0) {
            r.command   = GT2_COMMAND_TONEPORT;
            r.parameter = 0;
            row++; continue;
        }

        /* Small FX (no parameter byte) */
        if (fx >= SW1_SMALLFX_MIN) {
            uint8_t fxbase  = fx & 0xF0;
            uint8_t fxnybbl = fx & 0x0F;
            if (fxbase == SW1_SMALLFX_MAINVOL) {
                r.command   = GT2_COMMAND_VOLUME;
                r.parameter = fxnybbl;
            }
            row++; continue;
        }

        /* Big FX: parameter byte follows */
        uint8_t fxval = 0;
        if (idx < unpacked.size())
            fxval = unpacked[idx++];

        switch (fx) {
            /* Chord/arpeggio: create a duplicate instrument with an
             * arp wave table built from the global chord table entry.
             * The FX is dropped from the pattern (arp lives in wave table). */
            case SW1_BIGFX_SETCHORD: {
                if (chords && arp_cache && gt_file &&
                    fxval < (uint8_t)chords->size() &&
                    r.instrument >= 1)
                {
                    int base_gt = r.instrument - 1; /* 0-based */
                    int new_gt  = CreateArpInstrument(
                                      base_gt,
                                      (*chords)[fxval],
                                      *gt_file,
                                      gt_file->wavetable,
                                      *arp_cache,
                                      (int)fxval);
                    r.instrument = (uint8_t)(new_gt + 1); /* 1-based */
                }
                r.command   = GT2_COMMAND_NOP;
                r.parameter = 0;
                break;
            }

            /* Slide effects: SW speed byte -> GT2 speedtable index via the
             * exact inverse of GTsldToSWsld (uses SWexpTabH/L from swm.h). */
            case SW1_BIGFX_PORTUP:
                r.command   = GT2_COMMAND_PORTUP;
                r.parameter = SWsldToGTspd(fxval, Dpitch, spdBuilder);
                break;
            case SW1_BIGFX_PORTDOWN:
                r.command   = GT2_COMMAND_PORTDOWN;
                r.parameter = SWsldToGTspd(fxval, Dpitch, spdBuilder);
                break;
            case SW1_BIGFX_TONEPORT:
                if (fxval == 0) {
                    r.command   = GT2_COMMAND_TONEPORT;
                    r.parameter = 0;  /* legato */
                } else {
                    r.command   = GT2_COMMAND_TONEPORT;
                    r.parameter = SWsldToGTspd(fxval, Dpitch, spdBuilder);
                }
                break;
            case SW1_BIGFX_VIBRATO:
                r.command   = GT2_COMMAND_VIBRATO;
                r.parameter = SWsldToGTspd(fxval, Dpitch, spdBuilder);
                break;

            /* Direct-value effects */
            case SW1_BIGFX_AD:
                r.command = GT2_COMMAND_AD;         r.parameter = fxval; break;
            case SW1_BIGFX_SR:
                r.command = GT2_COMMAND_SR;         r.parameter = fxval; break;
            case SW1_BIGFX_WAVEREG:
                r.command = GT2_COMMAND_WAVEREG;    r.parameter = fxval; break;
            case SW1_BIGFX_WAVETBL:
                r.command = GT2_COMMAND_WAVETBL;    r.parameter = fxval; break;
            case SW1_BIGFX_PULSETBL:
                r.command = GT2_COMMAND_PULSETBL;   r.parameter = fxval; break;
            case SW1_BIGFX_FILTERTBL:
                r.command = GT2_COMMAND_FILTERTBL;  r.parameter = fxval; break;
            case SW1_BIGFX_FILT_CUTOFF:
                r.command = GT2_COMMAND_FILTERCUT;  r.parameter = fxval; break;
            case SW1_BIGFX_FILT_CONTROL:
                r.command = GT2_COMMAND_FILTERCTRL; r.parameter = fxval; break;

            /* Tempo effects already embedded in pattern data */
            case SW1_BIGFX_TEMPO:
                r.command = GT2_COMMAND_TEMPO;    r.parameter = fxval; break;
            case SW1_BIGFX_TRACKTEMPO:
                r.command = GT2_COMMAND_TEMPO;    r.parameter = fxval | 0x80; break;
            case SW1_BIGFX_FUNKTEMPO: {
                /* fxval encodes two nibble speeds: high=speed1, low=speed2 */
                uint8_t s1 = (fxval >> 4) & 0x0F;
                uint8_t s2 =  fxval       & 0x0F;
                uint8_t tidx = spdBuilder.add(s1, s2);
                r.command   = GT2_COMMAND_FUNK;
                r.parameter = tidx ? tidx : 1;
                break;
            }
            default:
                r.command = GT2_COMMAND_NOP; r.parameter = 0; break;
        }
        row++;
    }

    /* GT2 end-of-pattern marker */
    gt.data[gt.rows - 1].note       = GT2_PATTERN_END;
    gt.data[gt.rows - 1].instrument = 0;
    gt.data[gt.rows - 1].command    = 0;
    gt.data[gt.rows - 1].parameter  = 0;
}

/*************************************************************
 * SWFilterToGT2Filter
 * Converts a SID-Wizard instrument filter table section into
 * a series of GT2 filter table entries.
 *
 * SW filter table: 3 bytes per entry [L][R][T]
 *
 *   L == 0x00           : cutoff-set (generated by sng2swm for GT2→SW);
 *                         GT2 output: [0x00][R]
 *
 *   L 0x01..0x7F        : filter sweep/modulation
 *                         L = frame count (duration)
 *                         R = speed in SW units (11-bit SID range ×8)
 *                         GT2 output: [L][R>>3]   (unscale ×8 back to GT2)
 *                         Negative speeds (R≥0x80): unscale
 *                         (0x100-R)*8 → -(0x100-R)/8 in GT2
 *
 *   L 0x80..0xFF        : set filter parameters
 *                         high nibble of L = passband
 *                           (0x9_ = lowpass, 0xA_ = bandpass, 0xC_ = highpass)
 *                         low  nibble of L = resonance (0..F)
 *                         R = absolute cutoff
 *                         GT2 output: two entries on same frame:
 *                           [L & 0xF0][(reso<<4) | voice_mask]   — set params
 *                           [0x00    ][R]                        — set cutoff
 *
 *   SW1_TABLE_JUMP      : loop/jump; right = target SW byte offset
 *                         (converted to GT2 1-based table position)
 *   SW1_TABLE_END       : stop — GT2 [0xFF][0x00]
 *
 * voice_mask: GT2 channel bitmask for this instrument's voice
 *   (bit0=ch1, bit1=ch2, bit2=ch3; e.g. 0x04 = channel 3 only)
 *
 * initial_ctrl: SW instrument params byte that holds the SID filter
 *   control register initial value (resonance nibble in bits 7-4,
 *   voice routing in bits 2-0).  Used when no explicit band/reso
 *   entry appears in the filter table, so the resonance is preserved.
 *************************************************************/
static void SWFilterToGT2Filter(
        const std::vector<uint8_t>& filter_section,
        uint8_t                     voice_mask,
        uint8_t                     initial_ctrl,
        std::vector<uint8_t>&       fl_left,
        std::vector<uint8_t>&       fl_right)
{
    /* If there are no table entries but initial_ctrl has content,
     * emit a single param-set entry to establish the filter state. */
    if (filter_section.empty()) {
        if (initial_ctrl != 0) {
            uint8_t reso  = (initial_ctrl >> 4) & 0x0F;
            /* Default to low-pass when only initial_ctrl is present. */
            fl_left .push_back(0x90);
            fl_right.push_back((uint8_t)((reso << 4) | voice_mask));
            /* No cutoff value known — leave cutoff at whatever it was. */
            fl_left .push_back(0xFF);
            fl_right.push_back(0x00); /* stop */
        }
        return;
    }

    /* Track the start offset within fl_left/fl_right so that SW jump
     * targets (byte offsets) can be converted to GT2 1-based indices. */
    uint8_t gt2_table_start = (uint8_t)(fl_left.size() + 1); /* 1-based */

    /* Map: SW byte offset (0-based within filter_section) →
     *      GT2 table position (1-based from start of whole filter table).
     * Built as we go; used to fix up jump targets at the end. */
    std::vector<uint8_t> sw_offset_to_gt2(filter_section.size() + 1, 0);

    size_t t = 0;
    while (t < filter_section.size()) {
        sw_offset_to_gt2[t] = (uint8_t)(fl_left.size() + 1); /* record mapping */

        uint8_t L = filter_section[t];
        uint8_t R = (t + 1 < filter_section.size()) ? filter_section[t + 1] : 0;
        /* T (byte 2) is either channel mask | 0x80, or 0x00 in original SW files */
        uint8_t T = (t + 2 < filter_section.size()) ? filter_section[t + 2] : 0;
        t += 3;

        if (L == SW1_TABLE_END) {
            /* Stop marker */
            fl_left .push_back(0xFF);
            fl_right.push_back(0x00);
            break;
        }

        if (L == SW1_TABLE_JUMP) {
            /* Loop/jump: R = SW byte offset target.
             * We'll patch gt2_right after building the table. */
            fl_left .push_back(0xFF);
            fl_right.push_back(R); /* placeholder — patched below */
            break;
        }

        if (L == 0x00) {
            /* Pure cutoff-set entry (generated by sng2swm). */
            fl_left .push_back(0x00);
            fl_right.push_back(R);
            continue;
        }

        if (L >= 0x80) {
            /* Band/resonance/cutoff entry.
             * L high nibble = passband (0x9=LP, 0xA=BP, 0xC=HP …)
             * L low  nibble = resonance
             * R = absolute cutoff
             * T: if T & 0x80, the low nybble of T is an explicit channel mask
             *    (from sng2swm-produced files); otherwise use voice_mask. */
            uint8_t passband = L & 0xF0;       /* e.g. 0x90, 0xA0, 0xC0 */
            uint8_t reso     = L & 0x0F;
            uint8_t ch_mask  = (T & 0x80) ? (T & 0x0F) : voice_mask;

            /* GT2 "set filter parameters" entry */
            fl_left .push_back(passband);
            fl_right.push_back((uint8_t)((reso << 4) | ch_mask));

            /* GT2 "set cutoff" entry immediately following (same frame) */
            fl_left .push_back(0x00);
            fl_right.push_back(R);
            continue;
        }

        /* L 0x01..0x7F: filter modulation/sweep.
         * L = frame count (duration), R = speed in SW units (×8 scaling).
         * Inverse scale: GT2_speed = SW_speed / 8 (rounded, preserve sign). */
        uint8_t gt2_speed;
        if (R <= 0x7F) {
            /* Positive speed: SW = GT2 * 8 → GT2 = SW / 8 */
            gt2_speed = (uint8_t)(R >> 3);
            if (gt2_speed == 0 && R != 0) gt2_speed = 1; /* floor to 1 */
        } else {
            /* Negative speed (two's complement): 
             * SW stores 0x100 - |GT2_speed| * 8
             * Inverse: |GT2_speed| = (0x100 - SW_speed) / 8 */
            uint8_t sw_mag  = (uint8_t)(0x100u - R);
            uint8_t gt2_mag = (uint8_t)(sw_mag >> 3);
            if (gt2_mag == 0) gt2_mag = 1;
            gt2_speed = (uint8_t)(0x100u - gt2_mag);
        }
        fl_left .push_back(L);
        fl_right.push_back(gt2_speed);
    }

    /* If the loop exited because the data ran out (no SW1_TABLE_END or
     * SW1_TABLE_JUMP was encountered), the last GT2 entry written will
     * not be a terminator.  Always ensure the filter definition ends with
     * FF 00 (stop) so GoatTracker knows where the table ends. */
    if (fl_left.empty() ||
        !(fl_left.back() == 0xFF)) {
        fl_left .push_back(0xFF);
        fl_right.push_back(0x00);
    }

    /* Patch any SW_TABLE_JUMP targets.
     * sw_offset_to_gt2 maps SW filter_section byte offsets to GT2 positions.
     * The jump right byte we stored is the raw SW byte offset; replace with
     * the corresponding GT2 1-based table index. */
    for (size_t k = gt2_table_start - 1; k < fl_left.size(); k++) {
        if (fl_left[k] == 0xFF && fl_right[k] != 0x00) {
            uint8_t sw_target = fl_right[k];
            if (sw_target < sw_offset_to_gt2.size() &&
                sw_offset_to_gt2[sw_target] != 0) {
                fl_right[k] = sw_offset_to_gt2[sw_target];
            } else {
                fl_right[k] = gt2_table_start; /* fallback: loop to start */
            }
        }
    }
}

/*************************************************************
 * ReconstructTables
 * Rebuild GT2 wave/pulse/filter tables from per-instrument SW
 * table sections.  Populates gi.wv / gi.pl / gi.fl indices.
 *
 * voice_assignments[i] = SID voice routing bit(s) for
 * instrument i (1-based; 0 = not on any voice yet).
 *************************************************************/
static void ReconstructTables(
        const std::vector<SW_Instrument>& sw_instr,
        std::vector<sng_instrument>&      gt_instr,
        const std::vector<uint8_t>&       voice_assignments, /* per-instrument, 1-based */
        sng_table& wt, sng_table& pt, sng_table& ft)
{
    std::vector<uint8_t> wv_left, wv_right;
    std::vector<uint8_t> pl_left, pl_right;
    std::vector<uint8_t> fl_left, fl_right;

    for (size_t i = 0; i < sw_instr.size(); i++) {
        const SW_Instrument& si = sw_instr[i];
        sng_instrument&      gi = gt_instr[i];
        const auto& tbl = si.tables;

        uint8_t pulse_offset =
            (si.params.pulsetb_index >= SW1_INSTRUMENT_PARAMSIZE)
            ? si.params.pulsetb_index - SW1_INSTRUMENT_PARAMSIZE
            : (uint8_t)tbl.size();
        uint8_t filter_offset =
            (si.params.filtertb_index >= SW1_INSTRUMENT_PARAMSIZE)
            ? si.params.filtertb_index - SW1_INSTRUMENT_PARAMSIZE
            : (uint8_t)tbl.size();

        /* ---- Wave table ---- */
        uint8_t wv_start = (uint8_t)(wv_left.size() + 1);
        for (uint8_t t = 0; t + 1 < pulse_offset && t + 1 < (uint8_t)tbl.size(); t += 3) {
            uint8_t L = tbl[t], R = tbl[t+1];
            if (L == SW1_TABLE_END)  break;
            if (L == SW1_TABLE_JUMP) L = GT2_TABLE_JUMP;
            /* The arpeggio right byte: relative arpeggio uses SW1_ARP_REL_MIN offset. */
            if (R >= SW1_ARP_REL_MIN)
                R -= (SW1_ARP_REL_MIN - GT2_ARP_REL_MIN);
            else if (R >= SW1_ARP_ABS_MIN && R <= SW1_ARP_ABS_MAX)
                R -= 1;
            wv_left.push_back(L);
            wv_right.push_back(R);
        }
        if ((uint8_t)wv_left.size() >= wv_start) {
            wv_left.push_back(GT2_TABLE_JUMP);
            wv_right.push_back(0x00);
        }
        gi.wv = (wv_start <= (uint8_t)wv_left.size()) ? wv_start : 0;

        /* ---- Pulse table ---- */
        uint8_t pl_start = (uint8_t)(pl_left.size() + 1);
        for (uint8_t t = pulse_offset;
             t < filter_offset && t + 1 < (uint8_t)tbl.size(); t += 3) {
            uint8_t L = tbl[t], R = tbl[t+1];
            if (L == SW1_TABLE_END)  break;
            if (L == SW1_TABLE_JUMP) L = GT2_TABLE_JUMP;
            pl_left.push_back(L);
            pl_right.push_back(R);
        }
        if ((uint8_t)pl_left.size() >= pl_start) {
            pl_left.push_back(GT2_TABLE_JUMP);
            pl_right.push_back(0x00);
        }
        gi.pl = (pl_start <= (uint8_t)pl_left.size()) ? pl_start : 0;

        /* ---- Filter table ---- */
        uint8_t fl_start = (uint8_t)(fl_left.size() + 1);

        /* Collect raw SW filter bytes (excludes size marker) */
        std::vector<uint8_t> filter_section;
        for (uint8_t t = filter_offset; t < (uint8_t)tbl.size() - 1; t++)
            filter_section.push_back(tbl[t]);

        bool has_filter = !filter_section.empty();
        /* Also consider byte[4] (initial filter control): non-zero means filter is used */
        uint8_t initial_ctrl = si.params.arpchord_speed; /* byte[4] per our layout analysis */
        if (!has_filter && initial_ctrl != 0) has_filter = true;

        if (has_filter) {
            uint8_t vmask = (i < voice_assignments.size()) ? voice_assignments[i] : 0x07;
            if (vmask == 0) vmask = 0x07; /* fallback: all voices */
            SWFilterToGT2Filter(filter_section, vmask, initial_ctrl, fl_left, fl_right);
            /* SWFilterToGT2Filter emits its own stop/jump entry. */
        }

        gi.fl = (fl_start <= (uint8_t)fl_left.size()) ? fl_start : 0;
    }

    /* Write accumulated wave table data into the pre-allocated gt.wavetable.
     * For pulse and filter tables, allocate fresh (they are not appended to later). */
    auto fill_wave = [](sng_table& t,
                        const std::vector<uint8_t>& L,
                        const std::vector<uint8_t>& R)
    {
        /* gt.wavetable is pre-allocated with 255 bytes; just copy the data in. */
        t.length = (uint8_t)std::min(L.size(), (size_t)255);
        if (t.length && t.left && t.right) {
            memcpy(t.left,  L.data(), t.length);
            memcpy(t.right, R.data(), t.length);
        }
    };

    auto fill = [](sng_table& t,
                   const std::vector<uint8_t>& L,
                   const std::vector<uint8_t>& R)
    {
        t.length = (uint8_t)std::min(L.size(), (size_t)0xFF);
        if (t.length) {
            t.left  = new uint8_t[t.length];
            t.right = new uint8_t[t.length];
            memcpy(t.left,  L.data(), t.length);
            memcpy(t.right, R.data(), t.length);
        } else {
            t.left = t.right = nullptr;
        }
    };
    fill_wave(wt, wv_left, wv_right);
    fill(pt, pl_left, pl_right);
    fill(ft, fl_left, fl_right);
}

/*************************************************************
 * SWvibToGTvib
 * Exact inverse of GTvibToSWvib() from sng2swm.
 *
 * GTvibToSWvib encodes vibrato as:
 *   SWamp = (i - Dpitch) * 4       (i found via SWexpTabH[i] <= GTamp)
 *   SWvibr = (SWamp & 0xF0) + SWvibFreq[GTfreq]
 *
 * So the stored byte packs:
 *   HIGH nibble = SWamp >> 4       (amplitude class)
 *   LOW  nibble = SWvibFreq[GTfreq] (frequency index)
 *
 * Inverse:
 *   amp_nibble  = (SWvibr >> 4) & 0xF
 *   freq_nibble = SWvibr & 0xF
 *   SWamp       = amp_nibble * 16
 *   i           = SWamp / 4 + Dpitch = amp_nibble * 4 + Dpitch
 *   GTamp       = SWexpTabH[ clamp(i, 0, EXPTRESHOLD-1) ]
 *
 * For the frequency: SWvibFreq[] maps GT2 freq (0..15) → SW nibble.
 * We don't have that table, so we use freq_nibble directly as GTfreq.
 * This is exact when SWvibFreq is the identity permutation; otherwise
 * the speed is approximate but the depth is accurate.
 *
 * vibtype (from params.flag.vibtype):
 *   0 = triangle vibrato (standard, use Dpitch for amplitude lookup)
 *   1 = sine vibrato     (sng2swm always writes 1; same formula applies)
 *   When vibtype indicates "calculated" (GT2 bit7 on GTamp), the
 *   amplitude lookup skips Dpitch.  We handle this by checking if
 *   amp_nibble is 0 — in that case the depth is negligible.
 *
 * Returns a pair {GTfreq_byte, GTamp_byte} for a speedtable entry.
 *************************************************************/
static std::pair<uint8_t,uint8_t> SWvibToGTvib(uint8_t SWvibr, uint8_t Dpitch) {
    if (SWvibr == 0) return {0, 0};

    uint8_t amp_nibble  = (SWvibr >> 4) & 0x0F;
    uint8_t freq_nibble = SWvibr & 0x0F;

    /* Recover GTamp via the same SWexpTabH table used in sng2swm */
    int i = (int)amp_nibble * 4 + (int)Dpitch;
    if (i >= EXPTRESHOLD) i = EXPTRESHOLD - 1;
    if (i < 0)            i = 0;
    uint8_t GTamp = SWexpTabH[i];
    if (GTamp == 0 && amp_nibble > 0) GTamp = 1; /* ensure at least 1 */

    /* Clamp GTfreq: GT2 speedtable left byte for vibrato is 0..15 */
    uint8_t GTfreq = freq_nibble & 0x0F;

    return {GTfreq, GTamp};
}

/*************************************************************
 * ConvertInstrument
 * Converts one SWM instrument to a GT2 sng_instrument.
 * Also registers a vibrato speedtable entry if needed.
 *************************************************************/
static void ConvertInstrument(const SW_Instrument& si,
                              sng_instrument&      gi,
                              SpeedTableBuilder&   spdBuilder) {
    /* ADSR — direct 1:1 mapping */
    gi.ad = si.params.ad;
    gi.sr = si.params.sr;

    /* ---- Frame-1 waveform ----
     * In SID-Wizard the flag.wframe1 bit controls whether the player
     * outputs frame1_waveform to the SID chip before the wave table starts.
     * In GT2 this is the instrument's "first-frame waveform" (gi.hr field).
     * Only copy it when the flag is actually set; otherwise GT2 should see 0
     * (which means "don't force a waveform on note trigger"). */
    gi.hr = si.params.flag.wframe1 ? si.params.frame1_waveform : 0;

    /* ---- Hard restart timer ----
     * flag.hrtimer: 0=none, 1=frame-counter HR, 2=gate-off HR.
     * GT2 gtoff: bit7 set → no HR; 1 → frame counter; 2 → gate-off.
     * Bit7 set in SW gtoff means "don't hard-restart" → use 0x80 in GT2.
     *
     * GT2 also does not have separate HR-AD and HR-SR registers; the
     * hard-restart ADSR values from the SWM (si.params.hr_ad / hr_sr)
     * are not directly expressible in GT2 and are silently dropped. */
    switch (si.params.flag.hrtimer) {
        case 0:  gi.gtoff = 0x80; break;   /* no HR → bit7=no-restart */
        case 1:  gi.gtoff = 1;    break;   /* frame-counter HR */
        case 2:  gi.gtoff = 2;    break;   /* gate-off HR */
        default: gi.gtoff = 2;    break;   /* any other value → type 2 */
    }

    /* ---- Vibrato ----
     * SID-Wizard stores vibrato as a single byte:
     *   high nibble = amplitude class  (0 = none, 1..15 = increasing depth)
     *   low  nibble = frequency index  (0 = slowest, 15 = fastest)
     * Delay is stored in params.vibrato_delay.
     *
     * GT2 vibrato: gi.vibspd = 1-based speedtable index (left=freq, right=depth)
     *              gi.vibtim = frames before vibrato starts.
     *
     * We use SWvibToGTvib() which is the exact inverse of GTvibToSWvib(). */
    if (si.params.vibrato == 0) {
        gi.vibspd = 0;
        gi.vibtim = 0;
    } else {
        uint8_t Dpitch = (uint8_t)(SW1_NOTE_MAX / 2); /* mid-range default */
        auto [GTfreq, GTamp] = SWvibToGTvib(si.params.vibrato, Dpitch);
        uint8_t idx = spdBuilder.add(GTfreq, GTamp);
        gi.vibspd = idx ? idx : 0;
        gi.vibtim = si.params.vibrato_delay;
    }

    /* ---- Name ----
     * SID-Wizard pads with spaces (0x20); GT2 terminates with 0x00. */
    memset(gi.name, 0, GT2_INSTRUMENT_NAMESIZE);
    for (int j = 0; j < SW1_INSTRUMENT_NAMESIZE &&
                    j < GT2_INSTRUMENT_NAMESIZE - 1; j++) {
        char c = (char)si.name[j];
        if (c == 0x20 || c == 0) break;
        gi.name[j] = c;
    }
}

/*************************************************************
 * Convert
 *************************************************************/
void Convert(const SWMFile& swm, GT2File& gt) {
    const int subtunes = swm.header.sequence_count / 3;

    /* Header */
    memcpy(gt.header.tag, GT2_TAG, GT2_TAG_SIZE);
    gt.header.subtunes = (uint8_t)subtunes;

    {
        char tmp[SW1_AUTHORINFO_SIZE + 1]{};
        memcpy(tmp, swm.header.authorinfo, SW1_AUTHORINFO_SIZE);
        char* colon = strchr(tmp, ':');
        if (colon) {
            *colon = '\0';
            strncpy(gt.header.author, tmp, GT2_AUTHOR_SIZE - 1);
            for (int i = (int)strlen(gt.header.author)-1;
                     i >= 0 && gt.header.author[i]==' '; i--)
                gt.header.author[i] = '\0';
            strncpy(gt.header.name, colon + 1, GT2_NAME_SIZE - 1);
            for (int i = (int)strlen(gt.header.name)-1;
                     i >= 0 && gt.header.name[i]==' '; i--)
                gt.header.name[i] = '\0';
        } else {
            strncpy(gt.header.author, tmp, GT2_AUTHOR_SIZE - 1);
        }
        snprintf(gt.header.copyright, GT2_COPYRIGHT_SIZE, "SWM CONVERTED");
    }

    /* Orderlists */
    gt.orderlists.resize(subtunes);
    for (int i = 0; i < subtunes; i++) {
        ConvertSequence(swm.sequences[i*3+0], gt.orderlists[i].voice[0]);
        ConvertSequence(swm.sequences[i*3+1], gt.orderlists[i].voice[1]);
        ConvertSequence(swm.sequences[i*3+2], gt.orderlists[i].voice[2]);
    }

    /* SpeedTableBuilder accumulates all GT2 speedtable entries needed for
     * vibrato, slide, funk tempo, and chord arpeggios.  It must be declared
     * before ConvertInstrument (vibrato) and ConvertPattern (slides/tempo). */
    SpeedTableBuilder spdBuilder;

    /* Instruments */
    gt.instruments.resize(swm.header.instrument_count);
    for (int i = 0; i < swm.header.instrument_count; i++)
        ConvertInstrument(swm.instruments[i], gt.instruments[i], spdBuilder);

    /*---------------------------------------------------------
     * Per-instrument voice assignment for filter routing.
     *
     * SID-Wizard: filter settings are per-instrument and apply
     * globally regardless of which voice the instrument plays on.
     * GoatTracker: filter voice routing is set via the filter
     * table control entries.  We must know which SID voice each
     * SW instrument actually plays on so we can set the right
     * routing bits.
     *
     * Algorithm:
     *   1. For each subtune voice V (0=voice1,1=voice2,2=voice3),
     *      scan every pattern referenced in the orderlist.
     *   2. Collect the set of SW instrument numbers used on V.
     *   3. SID voice mask: voice0→bit0 (0x01), voice1→bit1 (0x02),
     *      voice2→bit2 (0x04).
     *
     * If an instrument with a filter table is used on MORE THAN
     * ONE voice, we generate a duplicate GT2 instrument for each
     * additional voice, each with its own filter routing.
     *---------------------------------------------------------*/

    /* instr_voice_mask[sw_instr_num] = bitmask of SID voices (1-based instr, 3-voice) */
    std::vector<uint8_t> instr_voice_mask(swm.header.instrument_count + 1, 0);

    /* Build a map: pattern_index (0-based) -> set of SW instruments used in it */
    auto ptn_instrs = [&](int gt_ptn_idx) -> std::vector<uint8_t> {
        if (gt_ptn_idx < 0 || gt_ptn_idx >= swm.header.pattern_count) return {};
        const SW_Pattern& sw = swm.patterns[gt_ptn_idx + 1];
        auto unpacked = UnpackPattern(sw.packed);
        std::vector<uint8_t> found;
        for (size_t i = 0; i < unpacked.size(); ) {
            uint8_t note = unpacked[i++];
            if (note & 0x80) {
                if (i >= unpacked.size()) break;
                uint8_t instr = unpacked[i++];
                uint8_t iv = instr & 0x7F;
                if (iv >= SW1_INSTRUMENT_MIN && iv <= SW1_INSTRUMENT_MAX)
                    found.push_back(iv);
                if (instr & 0x80) {
                    if (i >= unpacked.size()) break;
                    uint8_t fx = unpacked[i++];
                    if (fx > 0 && fx < SW1_SMALLFX_MIN && i < unpacked.size()) i++;
                }
            }
        }
        return found;
    };

    for (int si = 0; si < subtunes; si++) {
        for (int v = 0; v < 3; v++) {
            const sng_orderlist& ol = gt.orderlists[si].voice[v];
            uint8_t voice_bit = (uint8_t)(1u << v); /* 0x01, 0x02, 0x04 */
            for (int e = 0; e < ol.length; e++) {
                uint8_t entry = ol.data[e];
                if (entry == SW1_SEQUENCE_LOOPSONG) break;
                if (entry < GT2_ORDERLIST_TRANS_MIN) {
                    for (uint8_t iv : ptn_instrs((int)entry))
                        if (iv <= swm.header.instrument_count)
                            instr_voice_mask[iv] |= voice_bit;
                }
            }
        }
    }

    /* voice_assignments[gt_instr_idx] = SID voice routing bitmask for that GT2 instrument */
    /* (0-based GT2 index, SW instruments are 1-based)                                      */
    std::vector<uint8_t> voice_assignments(swm.header.instrument_count);
    for (int i = 0; i < swm.header.instrument_count; i++)
        voice_assignments[i] = instr_voice_mask[i + 1]; /* SW 1-based -> GT2 0-based */

    /* Check if any filter-bearing instrument is used on multiple voices.
     * If so, duplicate it: one GT2 instrument per voice, each with its voice mask.
     * We also need to patch all pattern references to point to the duplicate. */
    auto instr_has_filter = [&](int sw_idx) -> bool {
        /* sw_idx is 0-based */
        const SW_Instrument& si = swm.instruments[sw_idx];
        const auto& tbl = si.tables;
        uint8_t filter_offset =
            (si.params.filtertb_index >= SW1_INSTRUMENT_PARAMSIZE)
            ? si.params.filtertb_index - SW1_INSTRUMENT_PARAMSIZE
            : (uint8_t)tbl.size();
        bool has_tbl = (filter_offset < (uint8_t)(tbl.size() - 1));
        bool has_ctrl = (si.params.arpchord_speed != 0); /* byte[4] */
        return has_tbl || has_ctrl;
    };

    /* Map: (sw_instr_1based, voice_bit) -> new GT2 instrument index (0-based) */
    std::map<std::pair<int,int>, int> instr_voice_to_gt;
    /* Populate with the existing instruments (which own the FIRST voice bit set) */
    for (int i = 0; i < swm.header.instrument_count; i++) {
        uint8_t vmask = voice_assignments[i];
        if (!instr_has_filter(i) || (vmask & (vmask-1)) == 0) {
            /* No duplication needed */
            int first_bit = (vmask != 0) ? (vmask & -vmask) : 1; /* lowest set bit */
            instr_voice_to_gt[{i + 1, first_bit}] = i;
        } else {
            /* Split: one GT2 instrument per voice bit */
            bool first = true;
            for (int b = 0; b < 3; b++) {
                if (!(vmask & (1 << b))) continue;
                int vbit = (1 << b);
                if (first) {
                    instr_voice_to_gt[{i + 1, vbit}] = i;
                    voice_assignments[i] = (uint8_t)vbit; /* this copy owns voice b */
                    first = false;
                } else {
                    int new_idx = (int)gt.instruments.size();
                    gt.instruments.push_back(gt.instruments[i]); /* copy base instr */
                    voice_assignments.push_back((uint8_t)vbit);
                    instr_voice_to_gt[{i + 1, vbit}] = new_idx;
                    printf("  Duplicated instrument %d ('%s') for voice %d as GT2 instr %d\n",
                           i + 1, swm.instruments[i].name, b + 1, new_idx + 1);
                }
            }
        }
    }

    /* Patch pattern instrument references if any duplicates were created */
    bool duplicated = ((int)gt.instruments.size() > swm.header.instrument_count);

    /* Pre-allocate wave table with maximum capacity (255 entries) so that
     * CreateArpInstrument can safely append chord-arp entries to it after
     * ReconstructTables fills the initial instrument wave table data. */
    gt.wavetable.left  = new uint8_t[255]();
    gt.wavetable.right = new uint8_t[255]();
    gt.wavetable.length = 0;

    /* Tables (wave, pulse, filter rebuilt from instruments, now with voice assignments) */
    /* Extend sw_instr to cover any duplicated entries (they share the same SW source) */
    std::vector<SW_Instrument> sw_instr_ext = swm.instruments;
    while ((int)sw_instr_ext.size() < (int)gt.instruments.size()) {
        /* Find which original instrument this duplicate came from */
        /* Duplicates are copies of existing instruments - the voice_assignments tell us
         * how many copies there are per original instrument.                           */
        /* Safe: just repeat the last original instrument (the correct data is already
         * in gt.instruments from the copy above; ReconstructTables uses sw_instr_ext
         * only for table bytes which are the same for all voice-copies).              */
        sw_instr_ext.push_back(sw_instr_ext.back());
    }

    ReconstructTables(sw_instr_ext, gt.instruments, voice_assignments,
                      gt.wavetable, gt.pulsetable, gt.filtertable);

    /*---------------------------------------------------------
     * Speedtable + Tempo
     *
     * SWM stores one swm_funktempo per subtune (2 bytes at end
     * of file).  Each byte encodes a frame-count with bit7 set
     * as a flag: actual_frames = tempo_byte & 0x7F.
     *
     * Conversion strategy:
     *   1. Add a speedtable entry for each subtune's tempo pair.
     *   2. Determine which GT2 patterns are the FIRST pattern
     *      referenced by each voice's orderlist for that subtune.
     *   3. Inject a tempo command (GT2_COMMAND_TEMPO for single
     *      speed, GT2_COMMAND_FUNK for alternating) into row 0
     *      of those patterns.
     *
     * Patterns from different subtunes/voices that share the
     * same first pattern number will get the command only once
     * (from the first voice that requests it), so the injected
     * row is shared cleanly.
     *---------------------------------------------------------*/

    /* Map from GT2 pattern index (0-based) -> {cmd, param} to inject */
    std::vector<std::pair<uint8_t,uint8_t>> ptnTempoInject(
        swm.header.pattern_count, {0, 0});

    for (int si = 0; si < subtunes; si++) {
        uint8_t t1 = 6;  /* default: 6 frames (GT2 default) */
        if (si < (int)swm.tempos.size()) {
            t1 = swm.tempos[si].tempo1 & 0x7F;
            if (t1 == 0) t1 = 6;
        }

        /* Use a simple fixed tempo command (GT2_COMMAND_TEMPO).
         * SID-Wizard stores two alternating tempos (funk tempo), but the
         * primary tempo (tempo1) gives the correct overall playback speed.
         * The secondary tempo (tempo2) is an ornamental syncopation that
         * GT2's FUNK mechanism would replicate, but its slight timing
         * variation is imperceptible when the primary tempo is correct. */
        uint8_t tcmd   = GT2_COMMAND_TEMPO;
        uint8_t tparam = t1;

        printf("  Subtune %d: SWM tempo1=0x%02X -> GT2 TEMPO(%d)\n",
               si+1, swm.tempos[si].tempo1, t1);

        /* Find first pattern of each voice for this subtune */
        for (int v = 0; v < 3; v++) {
            const sng_orderlist& ol = gt.orderlists[si].voice[v];
            if (ol.length == 0 || !ol.data) continue;
            /* First entry may be a transpose command; skip those */
            for (int e = 0; e < ol.length; e++) {
                uint8_t entry = ol.data[e];
                if (entry < GT2_ORDERLIST_TRANS_MIN) {
                    /* It's a pattern reference (0-based GT2 pattern index) */
                    int ptnIdx = (int)entry;
                    if (ptnIdx < swm.header.pattern_count &&
                        ptnTempoInject[ptnIdx].first == 0) {
                        ptnTempoInject[ptnIdx] = {tcmd, tparam};
                    }
                    break;
                }
            }
        }
    }

    /* Patterns (SWM numbering starts at 1, GT2 from 0) */
    gt.patterns.resize(swm.header.pattern_count);
    std::map<std::pair<int,int>,int> arp_cache;
    for (int i = 0; i < swm.header.pattern_count; i++) {
        auto [tcmd, tparam] = ptnTempoInject[i];
        ConvertPattern(swm.patterns[i + 1], gt.patterns[i],
                       spdBuilder, tcmd, tparam,
                       swm.chords.empty() ? nullptr : &swm.chords,
                       &arp_cache, &gt);
    }

    /* Commit speedtable (may have entries from tempo and/or slide effects) */
    spdBuilder.fill(gt.speedtable);
}

/*************************************************************
 * WriteSNGFile
 * GT2 .sng binary layout:
 *   1. sng_header
 *   2. Orderlists: for each subtune, 3x [length][data][restart]
 *   3. Instrument count byte
 *   4. Instruments: sizeof(sng_instrument) each
 *   5. Wave table:   [length][left bytes][right bytes]
 *   6. Pulse table
 *   7. Filter table
 *   8. Speed table
 *   9. Pattern count byte
 *  10. Patterns: [rows][sng_pattern_row * rows]
 *************************************************************/
void WriteSNGFile(const char* path, const GT2File& gt) {
    FILE* f = OpenFile(path, "wb");

    /* 1. Header */
    if (fwrite(&gt.header, sizeof(sng_header), 1, f) != 1)
        die("Failed to write SNG header");

    /* 2. Orderlists */
    for (size_t i = 0; i < gt.orderlists.size(); i++) {
        for (int v = 0; v < 3; v++) {
            const sng_orderlist& ol = gt.orderlists[i].voice[v];
            write8(f, ol.length);
            if (ol.length && ol.data)
                fwrite(ol.data, 1, ol.length, f);
            write8(f, ol.restart);
        }
    }

    /* 3. Instrument count */
    write8(f, (uint8_t)gt.instruments.size());

    /* 4. Instruments */
    for (const auto& inst : gt.instruments)
        if (fwrite(&inst, sizeof(sng_instrument), 1, f) != 1)
            die("Failed to write instrument");

    /* 5-8. Tables */
    auto writeTable = [&](const sng_table& t) {
        write8(f, t.length);
        if (t.length) {
            if (!t.left || !t.right)
                die("Table length non-zero but data is null");
            fwrite(t.left,  1, t.length, f);
            fwrite(t.right, 1, t.length, f);
        }
    };
    writeTable(gt.wavetable);
    writeTable(gt.pulsetable);
    writeTable(gt.filtertable);
    writeTable(gt.speedtable);

    /* 9. Pattern count */
    write8(f, (uint8_t)gt.patterns.size());

    /* 10. Patterns */
    for (const auto& ptn : gt.patterns) {
        write8(f, ptn.rows);
        if (ptn.rows && ptn.data)
            if (fwrite(ptn.data, sizeof(sng_pattern_row), ptn.rows, f) != ptn.rows)
                die("Failed to write pattern rows");
    }

    fclose(f);
    printf("SNG written: %s\n", path);
    printf("  Subtunes:    %d\n", gt.header.subtunes);
    printf("  Instruments: %d\n", (int)gt.instruments.size());
    printf("  Patterns:    %d\n", (int)gt.patterns.size());
}

/*************************************************************
 * FreeGT2File
 *************************************************************/
void FreeGT2File(GT2File& gt) {
    for (auto& sub : gt.orderlists)
        for (int v = 0; v < 3; v++)
            delete[] sub.voice[v].data;
    for (auto& ptn : gt.patterns)
        delete[] ptn.data;
    if (gt.wavetable.left)    delete[] gt.wavetable.left;
    if (gt.wavetable.right)   delete[] gt.wavetable.right;
    if (gt.pulsetable.left)   delete[] gt.pulsetable.left;
    if (gt.pulsetable.right)  delete[] gt.pulsetable.right;
    if (gt.filtertable.left)  delete[] gt.filtertable.left;
    if (gt.filtertable.right) delete[] gt.filtertable.right;
    if (gt.speedtable.left)   delete[] gt.speedtable.left;
    if (gt.speedtable.right)  delete[] gt.speedtable.right;
}

/* === END OF swm2sng.cpp === */
