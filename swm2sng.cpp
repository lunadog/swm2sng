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
 *   LENGTH = row count.  We detect the terminator by reading the whole
 *   pattern block into memory and using a backtracking parser: at each
 *   position we check if [SIZE][LENGTH] is a plausible terminator
 *   (LENGTH==rows_parsed, delta=SIZE-consumed in [0..64]), and if so
 *   recurse to verify the remaining patterns also parse cleanly.
 *   This handles files where the first valid terminator candidate is a
 *   false positive — the backtracker tries the next candidate instead.
 *
 * Row encoding in the packed stream:
 *   Terminology (per SID-Wizard author):
 *     0x00 = 'tick' / empty row — note continues uninterrupted.
 *     0x7E = 'rest' / gate-off — the '---' sign in C64 music editors.
 *   The word 'rest' in comments here always means the gate-off (0x7E),
 *   not the empty tick row (0x00).
 *
 *   0x00        = empty row / tick (SW1_PTNCOL1_REST) — note continues
 *   0x7E        = gate-off / rest (SW1_PTNCOL1_KEYOFF) — release envelope
 *   0x70..0x77  = empty-row run: (byte - 0x70 + 2) consecutive empty rows
 *   0x01..0x6F, 0x78..0x7D, 0x7F
 *               = plain note (no instrument byte follows)
 *   0x80..0xFF  = note WITH instrument byte following
 *     instrument bit7=1  -> FX byte follows
 *       FX < SW1_SMALLFX_MIN -> parameter byte follows (big FX)
 *       FX >= SW1_SMALLFX_MIN -> no parameter byte (small FX)
 *
 * Sequence format (from SWM-spec.src):
 *   0xFF (loop/jump):    [refs...][0xFF][restart-pos][size]
 *   0xFE (end-of-tune):  [refs...][0xFE][size]  (no restart byte)
 *
 * Music data order (from SWM-spec.src):
 *   sequences → patterns → instruments → chord table → tempo table → subtune tempos
 *
 * Instrument table format:
 *   [SW1_INSTRUMENT_PARAMSIZE param bytes]
 *   [table bytes, last byte encodes total instrument size]
 *   [SW1_INSTRUMENT_NAMESIZE name bytes]
 *
 * Chord / arpeggio implementation
 * --------------------------------
 * SID-Wizard has a global chord table (arpeggios shared across instruments).
 * GT2 has no direct equivalent.  Per the SID-Wizard author's suggestion, we
 * implement chords by:
 *   1. Appending one arp sequence per unique chord to the GT2 wave table
 *      (3 entries of [0x00, GT2_ARP_REL_MIN+offset] + jump-to-self).
 *   2. Using GT2_COMMAND_WAVETBL in the pattern row to redirect the wave
 *      table pointer to that arp sequence.
 *   The instrument itself is unchanged — no cloning is required.
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
#include <set>
#include "sng.h"
#include <functional>

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
    std::vector<std::vector<uint8_t>> chords;
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
void Convert     (const SWMFile& swm, GT2File& gt, bool filterClone);
void WriteSNGFile(const char* path, const GT2File& gt);
void FreeGT2File (GT2File& gt);

/*************************************************************
 * Entry point
 *************************************************************/
int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: swm2sng [--no-filter-clone] <input.swm[.prg]> [output.sng]\n");
        printf("  --no-filter-clone  Use simple per-voice filter mask without instrument\n");
        printf("                     cloning.  Faster but less accurate filter routing.\n");
        return 1;
    }

    bool filterClone = true;
    int  firstArg    = 1;
    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--no-filter-clone") == 0) {
            filterClone = false;
            firstArg = a + 1;
        }
    }
    if (firstArg >= argc) {
        printf("Error: no input file specified.\n");
        return 1;
    }

    const char* inPath  = argv[firstArg];
    const char* outPath = (firstArg + 1 < argc) ? argv[firstArg + 1] : "output.sng";

    SWMFile swm;
    GT2File gt;

    LoadSWM(inPath, swm);
    Convert(swm, gt, filterClone);
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
     * Read sequences
     *
     * Per SWM-spec.src, the music data order is:
     *   sequences → patterns → instruments → chord table → tempo table → subtune tempos
     * Sequences start immediately after the 64-byte header (no chord table here).
     *
     * From SWM-spec.src and the SID-Wizard exporter source:
     *
     *   0xFF  Loop/jump — [refs...][0xFF][restart-pos][size]
     *            restart-pos: sequence entry to loop back to
     *            size:        total byte count of this sequence
     *                         (used by the player reading backwards; discarded here)
     *
     *   0xFE  End of tune, stop playback — [refs...][0xFE][size]
     *            size: byte count of this sequence (no restart byte for 0xFE)
     *
     * The spec example:
     *   1,1,1,FE,(4)         => 3 refs + FE + size=4        (3+1=4)
     *   2,2,2,FF,01,(5)      => 3 refs + FF + restart + size (3+2=5)
     *   3,4,5,6,7,FF,02,(7)  => 5 refs + FF + restart + size (5+2=7)
     *---------------------------------------------------------*/
    swm.sequences.resize(swm.header.sequence_count);
    for (int i = 0; i < swm.header.sequence_count; i++) {
        SW_Sequence& seq = swm.sequences[i];
        for (;;) {
            uint8_t c = read8(f);
            if (c == SW1_SEQUENCE_LOOPSONG) {   /* 0xFF: loop/jump */
                seq.restart_pos = read8(f);     /* restart-position byte */
                read8(f);                       /* size byte — discard */
                break;
            }
            if (c == SW1_SEQUENCE_ENDSONG) {    /* 0xFE: end of tune */
                read8(f);                       /* size byte — discard (no restart) */
                break;
            }
            seq.data.push_back(c);
        }
        printf("  Sequence %d: %zu entries, restart=%d\n",
               i + 1, seq.data.size(), seq.restart_pos);
    }

    /* Save position immediately after sequences = start of pattern block.
     * The backwards instrument walk below uses fseek and corrupts the file
     * position; we must restore it before starting pattern parsing. */
    long pattern_block_start = ftell(f);

    /*---------------------------------------------------------
     * Compute instrument block start before parsing patterns.
     *
     * Per packdepack.inc and Hermit's guidance, the SWM pattern
     * SIZE byte encodes the UNCOMPRESSED pattern size, not the
     * packed byte count, so it cannot be used as a back-pointer.
     * The only reliable parse direction is FORWARD from the pattern
     * block start with the instrument block start as a hard stop.
     *
     * The instrument block start is derived by parsing instruments
     * BACKWARDS from the known end of the file:
     *   file_end - subtune_tempos - tempo_table - chord_table
     *            = end of instrument block
     * Then each instrument (params + table + name) is stepped back
     * using the size-marker byte stored just before the name.
     *
     * Instrument layout (forward):
     *   [16 params][variable table][size_marker][8-byte name]
     * The size_marker value satisfies: total_instr_bytes = size_marker + 9.
     *---------------------------------------------------------*/
    long instr_block_start;
    std::vector<int> instr_sm; /* size_marker values from backwards walk */
    {
        int subtune_tempo_bytes = subtunes * 2;
        long instr_block_end = filesize
                               - subtune_tempo_bytes
                               - swm.header.tempotable_length
                               - swm.header.chordtable_length;

        /* Walk instruments backwards from instr_block_end.
         * Store each sm value so forward reading can use exact table lengths. */
        instr_sm.resize(swm.header.instrument_count, 0);
        long pos_back = instr_block_end;
        for (int i = swm.header.instrument_count - 1; i >= 0; i--) {
            /* size_marker is 1 byte before the 8-byte name */
            long name_end_abs  = pos_back;
            long size_marker_abs = name_end_abs - 8 - 1; /* -8 name, -1 marker */
            if (size_marker_abs < headerOffset)
                die("Instrument block underflows file header");
            fseek(f, size_marker_abs, SEEK_SET);
            int sm = fgetc(f);
            if (sm == EOF) die("Unexpected EOF reading instrument size marker");
            instr_sm[i] = sm;
            long instr_total = (long)sm + 9; /* params(16) + table(sm-15) + name(8) */
            pos_back = name_end_abs - instr_total;
        }
        instr_block_start = pos_back;
        printf("  Instrument block: [%ld..%ld]\n", instr_block_start, instr_block_end - 1);
    }

    /*---------------------------------------------------------
     * Read pattern blocks
     *
     * Each block: [packed row data...][SIZE byte][LENGTH byte]
     *
     * SIZE byte  = the stored (unpacked) byte count (NOT the
     *              packed byte count; used internally by SID-Wizard
     *              to depack into fixed-size uncompressed buffers).
     * LENGTH byte = number of pattern rows.
     *
     * Detection algorithm:
     *   Parse rows forward one at a time.  After each complete row,
     *   peek at the two bytes at the current file position:
     *     peek_size = data[pos]
     *     peek_len  = data[pos+1]
     *   If peek_len == rows_parsed (and rows > 0), this is likely
     *   the [SIZE][LENGTH] terminator.  To reject false positives
     *   where an instrument byte accidentally equals rows_parsed,
     *   also require:
     *     0 <= (peek_size - bytes_consumed) <= 32
     *   which is true for all real terminators observed across
     *   multiple SWM files (delta ranges from 0 to ~27).
     *   Additionally, pos+2 must not exceed instr_block_start
     *   (the hard stop computed above).
     *
     * Row encoding:
     *   0x70..0x77  -> (byte - 0x70 + 2) rest rows
     *   0x7E        -> 1 rest row
     *   other < 0x80 -> 1 row (note or special), no extra bytes
     *   >= 0x80     -> 1 note row; instrument byte follows
     *     instr bit7=1 -> FX byte follows; FX below SW1_SMALLFX_MIN means parameter byte follows
     *---------------------------------------------------------*/
    swm.patterns.resize(swm.header.pattern_count + 1);

    /* Restore file position to start of pattern block (the backwards instrument
     * walk used fseek internally, leaving the file pointer in the wrong place). */
    fseek(f, pattern_block_start, SEEK_SET);

    /* Read the whole pattern block into memory for backtracking parse.
     * Backtracking is needed because some patterns have multiple plausible
     * [SIZE][ROWS] terminators within delta<=64, and the first one is not
     * always correct.  Working in memory avoids repeated fseek overhead. */
    size_t ptn_block_len = (size_t)(instr_block_start - pattern_block_start);
    std::vector<uint8_t> pblk(ptn_block_len);
    if (fread(pblk.data(), 1, ptn_block_len, f) != ptn_block_len)
        die("Unexpected EOF reading pattern block");

    /* Returns the packed bytes consumed (excluding the trailing SIZE+ROWS bytes)
     * and the row count if a valid terminator was found at pblk[pos], else -1. */
    auto tryTerminator = [&](size_t pos, size_t start, int rows) -> int {
        if (pos + 1 >= ptn_block_len) return -1;
        uint8_t peek_size = pblk[pos];
        uint8_t peek_len  = pblk[pos + 1];
        if (rows <= 0 || peek_len != (uint8_t)rows) return -1;
        long consumed = (long)(pos - start);
        long delta    = (long)peek_size - consumed;
        /* Delta = SIZE - packed_bytes = uncompressed_savings.
         * No hard upper limit — the backtracker verifies the full block parses
         * cleanly, so false positives are rejected without a delta cap.
         * Only reject negative delta (SIZE < packed, impossible). */
        if (delta < 0) return -1;
        return (int)rows;
    };

    /* Recursive backtracking parser.
     * Returns true and fills offsets[] with end-of-pattern positions (0-based
     * index into pblk, pointing just past the SIZE+ROWS bytes) if all
     * pattern_count patterns fit exactly in [pos .. ptn_block_len). */
    int n_ptns = swm.header.pattern_count;
    std::vector<size_t> ptn_ends(n_ptns, 0);

    std::function<bool(int, size_t)> backtrack = [&](int pi, size_t pos) -> bool {
        if (pi == n_ptns) return pos == ptn_block_len;
        size_t start = pos;
        int rows = 0;
        const size_t MAX_SCAN = 65536;
        for (size_t guard = 0; guard < MAX_SCAN && pos < ptn_block_len; guard++) {
            /* Try terminator at current position */
            if (tryTerminator(pos, start, rows) >= 0) {
                size_t end = pos + 2;
                ptn_ends[pi] = end;
                if (backtrack(pi + 1, end)) return true;
                /* That candidate didn't lead to a complete parse; continue */
            }
            /* Consume one row byte */
            uint8_t b = pblk[pos++];
            if (b >= 0x70 && b <= 0x77) {
                rows += (b - 0x70) + 2;
            } else if (b == 0x7E || b == 0x00) {
                rows += 1;
            } else if (b & 0x80) {
                rows += 1;
                if (pos >= ptn_block_len) break;
                uint8_t instr = pblk[pos++];
                if (instr & 0x80) {
                    if (pos >= ptn_block_len) break;
                    uint8_t fx = pblk[pos++];
                    if (fx > 0 && fx < SW1_SMALLFX_MIN) {
                        if (pos >= ptn_block_len) break;
                        pos++; /* skip fxval */
                    }
                }
            } else {
                rows += 1; /* 0x01..0x6F: note without instrument */
            }
        }
        return false;
    };

    if (!backtrack(0, 0))
        die("Pattern block parse failed: could not locate SIZE/LENGTH bytes");

    /* Re-parse each pattern from pblk using the confirmed end positions */
    size_t blk_pos = 0;
    for (int pi = 1; pi <= n_ptns; pi++) {
        SW_Pattern& ptn = swm.patterns[pi];
        size_t end = ptn_ends[pi - 1];
        /* end-2 = SIZE byte, end-1 = ROWS byte */
        ptn.size_byte = pblk[end - 2];
        ptn.length    = pblk[end - 1];
        ptn.packed.assign(pblk.begin() + blk_pos, pblk.begin() + end - 2);
        printf("  Pattern %2d: %d rows, %zu packed bytes, size_byte=0x%02X\n",
               pi, ptn.length, ptn.packed.size(), ptn.size_byte);
        blk_pos = end;
    }

    /*---------------------------------------------------------
     * Read instruments
     * Seek to the pre-computed instrument block start for robustness
     * (insulates against any residual off-by-one in pattern parsing).
     * [SW1_INSTRUMENT_PARAMSIZE param bytes]
     * [table bytes; last byte encodes total instrument size]
     * [SW1_INSTRUMENT_NAMESIZE name bytes]
     * Table length is computed from the sm value gathered during the backwards
     * walk: table_len = sm - 15.  The in-band marker check (b == 16+size-1) is
     * unreliable because table data can accidentally match the check value.
     *---------------------------------------------------------*/
    fseek(f, instr_block_start, SEEK_SET);
    swm.instruments.resize(swm.header.instrument_count);
    for (int i = 0; i < swm.header.instrument_count; i++) {
        SW_Instrument& inst = swm.instruments[i];
        if (fread(&inst.params, 1, SW1_INSTRUMENT_PARAMSIZE, f)
                != SW1_INSTRUMENT_PARAMSIZE)
            die("Failed to read instrument params");

        int sm = instr_sm[i];
        int table_len = sm - 15; /* exact table byte count including size-marker byte */
        if (table_len < 1 || table_len > 512)
            die("Instrument table length out of range");
        inst.tables.resize(table_len);
        if ((int)fread(inst.tables.data(), 1, table_len, f) != table_len)
            die("Failed to read instrument table");

        if (fread(inst.name, 1, SW1_INSTRUMENT_NAMESIZE, f)
                != SW1_INSTRUMENT_NAMESIZE)
            die("Failed to read instrument name");
    }

    /*---------------------------------------------------------
     * Read chord table (if present)
     * Per SWM-spec.src, chord table comes after instruments.
     * Format: variable-length semitone-offset entries, each entry
     * terminated by 0x7E or 0x7F.  Total byte count is given by
     * header.chordtable_length.
     * Chord index in patterns: instrument-column byte 0x70..0x7F,
     * lower nibble = chord index (0..15).
     *---------------------------------------------------------*/
    if (swm.header.chordtable_length > 0) {
        int remaining = swm.header.chordtable_length;
        while (remaining > 0) {
            std::vector<uint8_t> entry;
            while (remaining > 0) {
                uint8_t b = read8(f); remaining--;
                if (b == 0x7E || b == 0x7F) break; /* chord terminator */
                entry.push_back(b);
            }
            swm.chords.push_back(entry);
        }
        printf("  Chord table: %zu chords\n", swm.chords.size());
    }

    /*---------------------------------------------------------
     * Read tempo table (if present)
     * Comes after chord table; total byte count in header.
     * We skip it — tempo is handled via the subtune tempos below.
     *---------------------------------------------------------*/
    for (int i = 0; i < swm.header.tempotable_length; i++)
        read8(f);

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
            /* Empty-row run: expands to (b-0x70+2) empty rows = 0x00 each.
             * 0x00 = SW1_PTNCOL1_REST ('tick', note continues uninterrupted).
             * 0x7E = SW1_PTNCOL1_KEYOFF ('rest'/gate-off) — a distinct value. */
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
 * Hermit's vibrato / slide lookup tables (from SWMconvert.c 2026)
 * These replace the original sng2swm-derived approach and produce
 * better-sounding output, especially for vibrato rates and speeds.
 *************************************************************/

/* Maps SW vibrato period nibble (0..15) -> GT2 speedtable left value.
 * OR'd with SNG_CALC_BIT (0x80) to enable calculated vibrato mode. */
static const uint8_t GTvibRates[16] = {
    0x7F, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x04,
    0x05, 0x06, 0x07, 0x08, 0x0A, 0x0C, 0x10, 0x14
};

/* Maps SW vibrato amplitude nibble (0..15) -> GT2 calculated-vibrato divisor. */
static const uint8_t SNGcalcVibratoAmpDiv[16] = {
    7, 6, 6, 5, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 1, 0
};

/* Maps (slidein >> 3) to GT2 calculated-slide divisor (input < 0x60). */
static const uint8_t SNGcalcSlideDiv[32] = {
    5, 4, 4, 4, 4, 3, 3, 3, 2, 2, 2, 1, 1, 1, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* Same for portamento. */
static const uint8_t SNGcalcPortDiv[32] = {
    4, 3, 3, 3, 3, 2, 2, 1, 1, 1, 1, 1, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* Non-calculated slide/portamento 16-bit speeds for (slidein >> 3) index 0..31. */
static const int SNGslideSpeeds[32] = {
    0x0000, 0x0004, 0x0008, 0x000C, 0x0010, 0x0014, 0x0018, 0x0020,
    0x0030, 0x0050, 0x0070, 0x00B0, 0x00F0, 0x0130, 0x0170, 0x01C0,
    0x0200, 0x0240, 0x0280, 0x02C0, 0x0300, 0x0340, 0x0380, 0x03C0,
    0x0400, 0x0440, 0x0480, 0x04C0, 0x0500, 0x0540, 0x0580, 0x05C0
};
static const int SNGportSpeeds[32] = {
    0x0000, 0x0008, 0x0010, 0x0018, 0x0020, 0x0028, 0x0034, 0x0040,
    0x0050, 0x0068, 0x0088, 0x00C0, 0x0100, 0x0180, 0x0200, 0x0280,
    0x0300, 0x0380, 0x0400, 0x0480, 0x0500, 0x0580, 0x0600, 0x0700,
    0x0800, 0x0900, 0x0A00, 0x0C00, 0x0E00, 0x1000, 0x1200, 0x1400
};

/* Below this threshold: use GT2 calculated slide (left |= 0x80). */
static const uint8_t SWM_SLIDE_CALC_THRESHOLD = 0x60;
/* GT2 calculated-slide/vibrato flag in speedtable left byte. */
static const uint8_t SNG_CALC_BIT = 0x80;
/* Exponential-table base offset for high-range slides.
 * = SWM_FREQTBH_POS/2 (6 in Hermit's 1-offset GTexpTabH) - 1 (to convert to
 * 0-based SWexpTabH index) = 5. */
static const int SWM_EXPLOOKUP_BASE = 5;

/*************************************************************
 * SWsldToGTsld
 * Converts a SID-Wizard slide byte + current note to a GT2
 * speedtable (hi, lo) pair, using Hermit's two-path approach:
 *  - input < 0x60: GT2 calculated slide (left bit7 set), divisor
 *    from SNGcalcSlideDiv.
 *  - input >= 0x60: direct exponential-table lookup (non-calculated),
 *    note-compensated, using SWexpTabH (= Hermit's GTexpTabH - 1 offset).
 * is_portamento selects slightly different divisor/speed tables.
 * Dpitch = current note value (SW 1-based, 1..0x5F).
 *************************************************************/
static std::pair<uint8_t,uint8_t> SWsldToGTsld(uint8_t SWsld, uint8_t Dpitch,
                                                bool is_portamento = false) {
    if (SWsld == 0) return {0x00, 0x00};

    if (SWsld < SWM_SLIDE_CALC_THRESHOLD) {
        /* Calculated mode: GT2 plays back the slide as a real-time delta. */
        int idx = (int)(SWsld >> 3);
        if (idx >= 32) idx = 31;
        uint8_t divisor = is_portamento ? SNGcalcPortDiv[idx]
                                        : SNGcalcSlideDiv[idx];
        return { (uint8_t)(SNG_CALC_BIT), divisor };
    } else {
        /* High-range: use exponential table, note-compensated.
         * Formula (Hermit): GTexpTabH[6 + slidein/2 + (note-1)]
         *   = SWexpTabH[5 + slidein/2 + (note-1)]  (0-based SWexpTabH) */
        int table_idx = SWM_EXPLOOKUP_BASE + (int)(SWsld >> 1)
                        + (int)(Dpitch) - 1;  /* note is 1-based, so -1 */

        if (table_idx < EXPTRESHOLD) {
            int clamped = std::max(0, std::min(table_idx, EXPTRESHOLD - 1));
            uint8_t delta = SWexpTabH[clamped];
            int pitchmod = (int)delta;
            int max_val = is_portamento ? 0x7FFF : 0xFFFF; /* avoid portamento overload */
            if (pitchmod > max_val) pitchmod = max_val;
            return { 0x00, (uint8_t)pitchmod };
        } else {
            int j = table_idx - EXPTRESHOLD;
            if (j >= FREQTB_SIZE) j = FREQTB_SIZE - 1;
            int pitchmod = ((int)SWexpTabH[j + FREQTBH_POS] << 8)
                         | (int)SWexpTabL[j];
            int max_val = is_portamento ? 0x7FFF : 0xFFFF;
            if (pitchmod > max_val) pitchmod = max_val;
            return { (uint8_t)(pitchmod >> 8), (uint8_t)(pitchmod & 0xFF) };
        }
    }
}

/*************************************************************
 * SWsldToGTspd
 * Wrapper: converts SW slide byte + note to a GT2 speedtable
 * index.  Uses SWsldToGTsld for the actual frequency lookup.
 *
 * Dpitch should be the most recent discrete note played on the
 * voice so that the slide speed is note-pitch compensated.
 *************************************************************/
static uint8_t SWsldToGTspd(uint8_t SWsld, uint8_t Dpitch,
                              SpeedTableBuilder& spdtbl,
                              bool is_portamento = false) {
    auto [hi, lo] = SWsldToGTsld(SWsld, Dpitch, is_portamento);
    return spdtbl.add(hi, lo);
}
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
/*************************************************************
 * GetOrCreateArpWtPos
 * Returns (or creates) a GT2 wave table position for the given
 * chord arpeggio.
 *
 * Per the SID-Wizard author's suggestion, chords are implemented
 * by placing arp sequences directly in the GT2 wave table and
 * using GT2_COMMAND_WAVETBL in the pattern to redirect the wave
 * table pointer there.  No instrument cloning is needed.
 *
 * chord[3]: semitone offsets from root (0 = root, e.g. {0,4,7}
 * for a major chord).  Only non-zero entries generate arp steps;
 * the first zero terminates the arp cycle (or all 3 are used).
 *
 * Wave table arp sequence (N steps + loop, all left=0x00 so the
 * waveform set by the instrument's own wave table is preserved):
 *   [0x00, arp_val(chord[0])]  step 0
 *   [0x00, arp_val(chord[1])]  step 1  (if non-zero)
 *   [0x00, arp_val(chord[2])]  step 2  (if non-zero)
 *   [0xFF, self_pos]           loop back to this sequence's start
 *
 * arp_cache maps (chord_num, waveform) → 1-based wave table start position.
 * Returns 0 if the wave table is full.
 *************************************************************/
static uint8_t GetOrCreateArpWtPos(
        int                               chord_num,
        const std::vector<uint8_t>&       chord,
        uint8_t                           waveform,  /* waveform for first arp entry */
        sng_table&                        wt,
        std::map<std::pair<int,int>,uint8_t>& arp_cache)
{
    auto cache_key = std::make_pair(chord_num, (int)waveform);
    auto it = arp_cache.find(cache_key);
    if (it != arp_cache.end())
        return it->second;

    /* Use all chord entries — zeros are root-note steps, not padding */
    int n_steps = (int)chord.size();
    if (n_steps < 1) return 0; /* empty chord — nothing to emit */

    int needed = n_steps + 1; /* steps + jump */
    if (wt.length + needed > 255) {
        printf("  WARNING: wave table full, cannot add chord %d arp\n", chord_num);
        return 0;
    }

    auto sw_to_gt2_arp = [](uint8_t sw_offset) -> uint8_t {
        /* SW chord table offsets are positive semitone counts (0x00..0x5F).
         * In the GT2 wave table right column, 0x01..0x5F are also direct
         * positive semitone offsets.  No constant offset needed. */
        return sw_offset;
    };

    uint8_t arp_start = (uint8_t)(wt.length + 1); /* 1-based */
    for (int s = 0; s < n_steps; s++) {
        /* Set waveform on every step so the gate stays open and the note
         * doesn't fade out between arp steps. */
        wt.left [wt.length] = waveform;
        wt.right[wt.length] = sw_to_gt2_arp(chord[s]);
        wt.length++;
    }
    wt.left [wt.length] = GT2_TABLE_JUMP;
    wt.right[wt.length] = arp_start;
    wt.length++;

    printf("  Chord %d waveform=0x%02X (%zu steps) -> wave table pos %d\n",
           chord_num, waveform, chord.size(), arp_start);

    arp_cache[cache_key] = arp_start;
    return arp_start;
}

/*************************************************************
 * ConvertPattern
 * Translates one SW pattern to a GT2 sng_pattern.
 *
 * tempo_cmd / tempo_param: if non-zero, written into row 0's
 * COMMAND column (no extra row is inserted).
 *
 * chords / arp_cache / gt_file: used to materialise chord-table
 * arpeggios as wave table sequences (no instrument cloning).
 *************************************************************/
/* Forward declaration — defined after ConvertPattern */
static std::pair<uint8_t,uint8_t> SWvibToGTvib(uint8_t SWvibr, uint8_t Dpitch);

static void ConvertPattern(const SW_Pattern& sw, sng_pattern& gt,
                           SpeedTableBuilder& spdBuilder,
                           uint8_t tempo_cmd   = 0,
                           uint8_t tempo_param = 0,
                           const std::vector<std::vector<uint8_t>>* chords      = nullptr,
                           std::map<std::pair<int,int>,uint8_t>*     arp_cache   = nullptr,
                           GT2File*                                   gt_file     = nullptr,
                           const std::vector<int8_t>*                 oct_shifts  = nullptr,
                           uint8_t*                                   inout_instr = nullptr,
                           const std::vector<SW_Instrument>*          sw_instruments = nullptr) {
    auto unpacked = UnpackPattern(sw.packed);

    gt.rows = sw.length + 1;  /* +1 for GT2's mandatory $FF end row */
    gt.data = new sng_pattern_row[gt.rows];
    memset(gt.data, 0, sizeof(sng_pattern_row) * gt.rows);

    size_t idx = 0;
    int    row = 0;

    /* Dpitch tracks the last real note played; used for slide speed conversion.
     * Initialise to the middle of the SID-Wizard note range (same as sng2swm). */
    uint8_t Dpitch    = (uint8_t)(SW1_NOTE_MAX / 2);

    /* cur_instr: the SW instrument number (1-based) currently active on this
     * channel.  Initialised from inout_instr so it carries over across pattern
     * boundaries (GT2 instruments persist until explicitly changed, just as in
     * SID-Wizard).  Written back to inout_instr at the end of the pattern. */
    uint8_t cur_instr = (inout_instr && *inout_instr) ? *inout_instr : 0;

    /* Returns the first waveform byte from the wave section of sw_instr (1-based).
     * Used to set the waveform on the first step of chord arp sequences, so the
     * SID chip plays the correct waveform when an alternate chord is triggered. */
    auto instrWaveform = [&](uint8_t sw_iv) -> uint8_t {
        if (!sw_instruments || sw_iv < 1 ||
                sw_iv > (uint8_t)sw_instruments->size()) return 0x41;
        const SW_Instrument& si2 = (*sw_instruments)[sw_iv - 1];
        uint8_t pulse_off = (si2.params.pulsetb_index >= SW1_INSTRUMENT_PARAMSIZE)
                            ? si2.params.pulsetb_index - SW1_INSTRUMENT_PARAMSIZE
                            : (uint8_t)si2.tables.size();
        for (uint8_t t = 0; t + 1 < pulse_off && t + 1 < (uint8_t)si2.tables.size(); t += 3) {
            uint8_t L = si2.tables[t];
            if (L == SW1_TABLE_END || L == SW1_TABLE_JUMP) break;
            if (L != 0x00) return L;
        }
        return 0x41; /* default: pulse + gate */
    };

    /* Portamento carry-over.
     * In SID-Wizard, a portamento (slide) FX set on one row continues on every
     * following row until cancelled.  In GT2, GT2_COMMAND_TONEPORT must be
     * re-written on every row where the slide should still be active.
     * We track the active portamento command and parameter here and re-emit on
     * every empty/gate-off row while the slide is active.  A new note row
     * without a fresh portamento FX cancels the slide. */
    uint8_t port_cmd   = 0;
    uint8_t port_param = 0;

    while (idx < unpacked.size() && row < gt.rows - 1) {
        sng_pattern_row& r = gt.data[row];
        uint8_t note    = unpacked[idx++];
        bool    hasInstr = (note & 0x80) != 0;
        uint8_t noteVal  = note & 0x7F;

        /* If this row sets an instrument, peek at it NOW — before mapping the note —
         * so that the octave shift for the new instrument applies to the note on
         * this very row (not just the following rows). */
        if (hasInstr && idx < unpacked.size()) {
            uint8_t peek_instr = unpacked[idx] & 0x7F;
            if ((peek_instr >= SW1_INSTRUMENT_MIN &&
                 peek_instr <= SW1_INSTRUMENT_MAX) ||
                (peek_instr >= 0x70 && peek_instr <= 0x7F))
                cur_instr = (peek_instr <= SW1_INSTRUMENT_MAX) ? peek_instr : cur_instr;
        }

        /* Map SW note value -> GT2 note value.
         *
         * SID-Wizard packed pattern stream encoding:
         *   0x00        = empty row / tick (note continues, gate stays on)
         *   0x7E        = gate-off / rest (SW1_PTNCOL1_KEYOFF) — release envelope
         *   0x70..0x77  = empty-row run (already expanded to 0x00 by UnpackPattern)
         *   0x80+       = note with instrument byte
         *   0x01..0x6F  = plain note (no instrument byte) */
        if (note == 0x00) {
            r.note = GT2_PATTERN_REST;
            /* Re-emit active portamento on every continuing empty row.
             * In SID-Wizard a slide FX set once stays active; in GT2 it
             * must be written on every row where the slide continues. */
            if (port_cmd) { r.command = port_cmd; r.parameter = port_param; }
        } else if (note == 0x7E || noteVal == SW1_PTNCOL1_KEYOFF) {
            /* gate-off: cancel any active slide */
            r.note   = GT2_PATTERN_KEYOFF;
            port_cmd = port_param = 0;
        } else if (noteVal == SW1_PTNCOL1_KEYON) {
            r.note = GT2_PATTERN_KEYON;
        } else if (noteVal == SW1_PTNCOL1_END) {
            break;
        } else if (noteVal >= SW1_PTNCOL1_NOTE &&
                   noteVal <= SW1_NOTE_MAX) {
            /* Apply per-instrument octave shift (signed semitone offset stored
             * in params.octave_shift).  In SID-Wizard the player adds this shift
             * to the note's SID frequency; in GT2 we bake it into the note value. */
            int shift = 0;
            if (oct_shifts && cur_instr >= 1 &&
                (size_t)cur_instr < oct_shifts->size())
                shift = (*oct_shifts)[cur_instr];

            int gt2_note = (int)noteVal
                           + (int)(GT2_PATTERN_NOTE - SW1_PTNCOL1_NOTE)
                           + shift;

            /* Clamp to valid GT2 note range (96 notes: GT2_PATTERN_NOTE to +95) */
            if (gt2_note < (int)GT2_PATTERN_NOTE)
                gt2_note = (int)GT2_PATTERN_NOTE;
            else if (gt2_note > (int)GT2_PATTERN_NOTE + 95)
                gt2_note = (int)GT2_PATTERN_NOTE + 95;

            r.note = (uint8_t)gt2_note;
            Dpitch = noteVal;
            /* A new pitched note without a fresh portamento FX cancels the slide */
            port_cmd = port_param = 0;
        } else {
            r.note = GT2_PATTERN_REST;
        }

        r.instrument = 0;
        /* Only reset command/parameter for rows that haven't already had the
         * portamento carry-over written.  For empty rows (note==0x00) the
         * carry-over was set above and must not be overwritten here. */
        if (note != 0x00) {
            r.command   = GT2_COMMAND_NOP;
            r.parameter = 0;
        }

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

        /* Helpers: instrument default values for nibble-only FX. */
        auto instAD = [&](uint8_t iv) -> uint8_t {
            if (!sw_instruments || iv < 1 || iv > sw_instruments->size()) return 0x00;
            return (*sw_instruments)[iv-1].params.ad;
        };
        auto instSR = [&](uint8_t iv) -> uint8_t {
            if (!sw_instruments || iv < 1 || iv > sw_instruments->size()) return 0x00;
            return (*sw_instruments)[iv-1].params.sr;
        };
        auto instVib = [&](uint8_t iv) -> uint8_t {
            if (!sw_instruments || iv < 1 || iv > sw_instruments->size()) return 0x00;
            return (*sw_instruments)[iv-1].params.vibrato;
        };
        /* Filter channel-mask for instrument: reads T-byte of first filter entry. */
        auto instFilterMask = [&](uint8_t iv) -> uint8_t {
            if (!sw_instruments || iv < 1 || iv > sw_instruments->size()) return 0x07;
            const SW_Instrument& si2 = (*sw_instruments)[iv-1];
            uint8_t foff = (si2.params.filtertb_index >= SW1_INSTRUMENT_PARAMSIZE)
                           ? si2.params.filtertb_index - SW1_INSTRUMENT_PARAMSIZE
                           : (uint8_t)si2.tables.size();
            if (foff + 2 < (uint8_t)si2.tables.size()) {
                uint8_t T = si2.tables[foff + 2];
                if (T & 0x80) return T & 0x07;
            }
            return 0x07;
        };
        /* Emit waveform-register: SW nybble -> SID high nibble | gate */
        auto emitWavereg = [&](uint8_t nybble) {
            r.command   = GT2_COMMAND_WAVEREG;
            r.parameter = (uint8_t)((nybble << 4) | 0x01);
        };
        /* Emit vibrato from combined SW vibr byte (high=amp nybble, low=freq nybble). */
        auto emitVibFromSW = [&](uint8_t sw_vibr) {
            if (sw_vibr == 0) { r.command = GT2_COMMAND_NOP; return; }
            auto [GTfreq, GTamp] = SWvibToGTvib(sw_vibr, Dpitch);
            uint8_t tidx = spdBuilder.add(GTfreq, GTamp);
            r.command   = GT2_COMMAND_VIBRATO;
            r.parameter = tidx ? tidx : 1;
        };
        /* Emit set-chord via wave table (1-based chord number). */
        auto emitChord = [&](uint8_t chord_1based) {
            if (!chord_1based) return;
            uint8_t ci = chord_1based - 1;
            if (!chords || !arp_cache || !gt_file) return;
            if (ci >= (uint8_t)chords->size()) return;
            uint8_t wt_pos = GetOrCreateArpWtPos((int)ci, (*chords)[ci],
                                                  instrWaveform(cur_instr),
                                                  gt_file->wavetable, *arp_cache);
            if (wt_pos) { r.command = GT2_COMMAND_WAVETBL; r.parameter = wt_pos; }
        };

        /* ---- Instrument column effects ---- */
        if (instrVal == SW1_LEGATO) {
            /* $3F  Legato: set pitch without retriggering envelope -> instant toneport */
            r.command   = GT2_COMMAND_TONEPORT;
            r.parameter = 0;
        } else if (instrVal >= SW1_SMALLFX_WAVEFORM &&
                   instrVal <  SW1_SMALLFX_SUSTAIN) {
            /* $40..$4F  Set Waveform */
            emitWavereg(instrVal & 0x0F);
        } else if (instrVal >= SW1_SMALLFX_SUSTAIN &&
                   instrVal <  SW1_SMALLFX_RELEASE) {
            /* $50..$5F  Set Sustain */
            r.command   = GT2_COMMAND_SR;
            r.parameter = (uint8_t)(((instrVal & 0x0F) << 4) | (instSR(cur_instr) & 0x0F));
        } else if (instrVal >= SW1_SMALLFX_RELEASE &&
                   instrVal <  SW1_SMALLFX_SETCHORD) {
            /* $60..$6F  Set Release */
            r.command   = GT2_COMMAND_SR;
            r.parameter = (uint8_t)((instSR(cur_instr) & 0xF0) | (instrVal & 0x0F));
        } else if (instrVal >= SW1_SMALLFX_SETCHORD &&
                   instrVal <= (SW1_SMALLFX_SETCHORD | 0x0F)) {
            /* $70..$7F  Set Chord (1-based) */
            emitChord(instrVal & 0x0F);
        } else if (instrVal >= SW1_INSTRUMENT_MIN &&
                   instrVal <= SW1_INSTRUMENT_MAX) {
            /* $01..$3E  Select instrument */
            r.instrument = instrVal;
            cur_instr    = instrVal;
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

        /* ---- Small FX (no parameter byte, all info in the fx byte itself) ---- */
        if (fx >= SW1_SMALLFX_MIN) {
            uint8_t fxbase  = fx & 0xF0;
            uint8_t fxnybbl = fx & 0x0F;
            switch (fxbase) {
                case SW1_SMALLFX_ATTACK:   /* $20..$2F  Attack nybble */
                    r.command   = GT2_COMMAND_AD;
                    r.parameter = (uint8_t)((fxnybbl << 4) | (instAD(cur_instr) & 0x0F));
                    break;
                case SW1_SMALLFX_DECAY:    /* $30..$3F  Decay nybble */
                    r.command   = GT2_COMMAND_AD;
                    r.parameter = (uint8_t)((instAD(cur_instr) & 0xF0) | fxnybbl);
                    break;
                case SW1_SMALLFX_WAVEFORM: /* $40..$4F  Waveform nybble */
                    emitWavereg(fxnybbl);
                    break;
                case SW1_SMALLFX_SUSTAIN:  /* $50..$5F  Sustain nybble */
                    r.command   = GT2_COMMAND_SR;
                    r.parameter = (uint8_t)((fxnybbl << 4) | (instSR(cur_instr) & 0x0F));
                    break;
                case SW1_SMALLFX_RELEASE:  /* $60..$6F  Release nybble */
                    r.command   = GT2_COMMAND_SR;
                    r.parameter = (uint8_t)((instSR(cur_instr) & 0xF0) | fxnybbl);
                    break;
                case SW1_SMALLFX_SETCHORD: /* $70..$7F  Set Chord (1-based) */
                    emitChord(fxnybbl);
                    break;
                case SW1_SMALLFX_VIBAMP: { /* $80..$8F  Vibrato amplitude nybble */
                    uint8_t sw_vibr = (uint8_t)((fxnybbl << 4) |
                                                 (instVib(cur_instr) & 0x0F));
                    emitVibFromSW(sw_vibr);
                    break;
                }
                case SW1_SMALLFX_VIBFREQ: { /* $90..$9F  Vibrato rate nybble */
                    uint8_t sw_vibr = (uint8_t)((instVib(cur_instr) & 0xF0) | fxnybbl);
                    emitVibFromSW(sw_vibr);
                    break;
                }
                case SW1_SMALLFX_MAINVOL:  /* $A0..$AF  Main volume */
                    r.command   = GT2_COMMAND_VOLUME;
                    r.parameter = fxnybbl;
                    break;
                case SW1_SMALLFX_FILTBAND: /* $B0..$BF  Filter cutoff hi-byte nybble
                                            * GT2 $Cxy: x=nybble, y=0 */
                    r.command   = GT2_COMMAND_FILTERCUT;
                    r.parameter = (uint8_t)(fxnybbl << 4);
                    break;
                case SW1_SMALLFX_CHORDSPD: /* $C0..$CF  Chord speed — no GT2 equiv */
                case SW1_SMALLFX_DETUNE:   /* $D0..$DF  Detune      — no GT2 equiv */
                case SW1_SMALLFX_WAVEREGC: /* $E0..$EF  Wave ctrl   — no GT2 equiv */
                    break; /* silently dropped */
                case SW1_SMALLFX_RESONANCE:/* $F0..$FF  Filter resonance nybble
                                            * GT2 $Bxy: x=reso nybble, y=inst channel mask */
                    r.command   = GT2_COMMAND_FILTERCTRL;
                    r.parameter = (uint8_t)((fxnybbl << 4) | instFilterMask(cur_instr));
                    break;
                default:
                    break;
            }
            row++; continue;
        }

        /* Big FX: parameter byte follows */
        uint8_t fxval = 0;
        if (idx < unpacked.size())
            fxval = unpacked[idx++];

        switch (fx) {
            /* Chord/arpeggio: append an arp sequence to the GT2 wave table
             * and emit GT2_COMMAND_WAVETBL to redirect the wave table pointer
             * there.  The instrument is unchanged — no cloning needed. */
            case SW1_BIGFX_SETCHORD: {
                /* fxval is 1-based: fxval=1 means chord 0, fxval=7 means chord 6. */
                if (chords && arp_cache && gt_file &&
                    fxval > 0 && (fxval - 1) < (uint8_t)chords->size())
                {
                    uint8_t chord_idx = fxval - 1;
                    uint8_t wt_pos = GetOrCreateArpWtPos(
                                         (int)chord_idx,
                                         (*chords)[chord_idx],
                                         instrWaveform(cur_instr),
                                         gt_file->wavetable,
                                         *arp_cache);
                    if (wt_pos != 0) {
                        r.command   = GT2_COMMAND_WAVETBL;
                        r.parameter = wt_pos;
                    }
                }
                break;
            }

            /* Slide effects: Hermit's two-path approach (calculated below 0x60,
             * exponential-table lookup at/above 0x60, note-pitch compensated). */
            case SW1_BIGFX_PORTUP:
                r.command   = GT2_COMMAND_PORTUP;
                r.parameter = SWsldToGTspd(fxval, Dpitch, spdBuilder, false);
                port_cmd = r.command; port_param = r.parameter;
                break;
            case SW1_BIGFX_PORTDOWN:
                r.command   = GT2_COMMAND_PORTDOWN;
                r.parameter = SWsldToGTspd(fxval, Dpitch, spdBuilder, false);
                port_cmd = r.command; port_param = r.parameter;
                break;
            case SW1_BIGFX_TONEPORT:
                if (fxval == 0) {
                    r.command   = GT2_COMMAND_TONEPORT;
                    r.parameter = 0;  /* legato */
                    port_cmd = port_param = 0;  /* legato is not a repeating slide */
                } else {
                    r.command   = GT2_COMMAND_TONEPORT;
                    r.parameter = SWsldToGTspd(fxval, Dpitch, spdBuilder, true);
                    port_cmd = r.command; port_param = r.parameter;
                }
                break;
            case SW1_BIGFX_VIBRATO:
                r.command   = GT2_COMMAND_VIBRATO;
                r.parameter = SWsldToGTspd(fxval, Dpitch, spdBuilder, false);
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
            case SW1_BIGFX_FUNKTEMPO:
            case SW1_BIGFX_TRACKFUNKT: {
                /* Both global and per-track funk tempo: GT2 $Exy -> speedtable entry */
                uint8_t s1 = (fxval >> 4) & 0x0F;
                uint8_t s2 =  fxval       & 0x0F;
                uint8_t tidx = spdBuilder.add(s1, s2);
                r.command   = GT2_COMMAND_FUNK;
                r.parameter = tidx ? tidx : 1;
                break;
            }

            /* No GT2 equivalent — silently dropped */
            case SW1_BIGFX_CHORDSPEED:  /* $0C chord speed */
            case SW1_BIGFX_DETUNE:      /* $0D detune up */
            case SW1_BIGFX_PULSEWIDTH:  /* $0E set pulse width */
            case SW1_BIGFX_TEMPOPROG:   /* $12 tempo program */
            case SW1_BIGFX_TRKTEMPOPRG: /* $15 track tempo program */
            case SW1_BIGFX_VIBRATYPE:   /* $16 vibrato type */
            case SW1_BIGFX_FILTERSHIFT: /* $1C filter cutoff shift */
            case SW1_BIGFX_DELAYTRACK:  /* $1D delay track */
            case SW1_BIGFX_DELAYNOTE:   /* $1E delay note */
                break;
        }
        row++;
    }

    /* Persist the active instrument so the next pattern on this voice inherits it. */
    if (inout_instr) *inout_instr = cur_instr;

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
        uint8_t                     filter_abs_start, /* absolute byte from params start */
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
            /* Pure cutoff-set entry (generated by sng2swm for GT2→SW round-trips).
             * A single [0x00 0x00 0x00] entry means "filter enabled on this channel
             * but no cutoff/band control" (per SID-Wizard author); skip the cutoff
             * emit in that case and let the routing entry suffice. */
            if (R != 0x00) {
                fl_left .push_back(0x00);
                fl_right.push_back(R);
            }
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
     * not be a terminator.  Only add FF 00 if we actually emitted something
     * useful; an all-skip filter section (e.g. [0x00 0x00 0x00] with R=0)
     * should leave fl_left unchanged so the caller sets gi.fl = 0. */
    bool emitted_something = (fl_left.size() >= gt2_table_start);
    if (emitted_something && fl_left.back() != 0xFF) {
        fl_left .push_back(0xFF);
        fl_right.push_back(0x00);
    }

    /* Patch any SW_TABLE_JUMP targets.
     * sw_offset_to_gt2 maps SW filter_section byte offsets (0-based within
     * filter_section) to GT2 positions.  The raw R stored is an absolute byte
     * offset from the instrument params start; subtract filter_abs_start first. */
    for (size_t k = gt2_table_start - 1; k < fl_left.size(); k++) {
        if (fl_left[k] == 0xFF && fl_right[k] != 0x00) {
            uint8_t sw_abs   = fl_right[k]; /* absolute from params start */
            int     sw_rel   = (int)sw_abs - (int)filter_abs_start;
            if (sw_rel >= 0 && (size_t)sw_rel < sw_offset_to_gt2.size() &&
                sw_offset_to_gt2[(size_t)sw_rel] != 0) {
                fl_right[k] = sw_offset_to_gt2[(size_t)sw_rel];
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
        const std::vector<uint8_t>&       voice_assignments,  /* per-instrument, 1-based */
        const std::vector<uint8_t>&       filter_voice_mask,  /* per-instrument filter routing */
        const std::vector<std::vector<uint8_t>>* chords_ptr, /* global chord table */
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
        bool wv_terminated = false;
        bool wv_has_data   = false;
        for (uint8_t t = 0; t < pulse_offset && t < (uint8_t)tbl.size(); t += 3) {
            uint8_t L = tbl[t];
            uint8_t R = (t + 1 < (uint8_t)tbl.size()) ? tbl[t+1] : 0;

            if (L == SW1_TABLE_END) {
                /* 0xFF in SW = end of table: emit GT2 stop (FF 00) only if there's data */
                if (wv_has_data) {
                    wv_left.push_back(GT2_TABLE_JUMP);
                    wv_right.push_back(0x00);
                }
                wv_terminated = true;
                break;
            }
            if (L == SW1_TABLE_JUMP) {
                /* 0xFE in SW = loop/jump: R is the absolute byte offset from the
                 * start of the instrument params block (byte 0 of the 16-byte params).
                 * The wave section starts at absolute byte SW1_INSTRUMENT_PARAMSIZE (16).
                 * Convert to a GT2 1-based table position. */
                uint8_t gt_target = (uint8_t)(wv_start + (R - SW1_INSTRUMENT_PARAMSIZE) / 3);
                wv_left.push_back(GT2_TABLE_JUMP);
                wv_right.push_back(gt_target);
                wv_terminated = true;
                break;
            }

            /* R = 0x7F = SW1_TABLEJTOCORD: expand instrument's default chord inline */
            if (R == 0x7F && chords_ptr && !chords_ptr->empty()) {
                uint8_t chord_idx = si.params.default_chord;
                if (chord_idx > 0) chord_idx -= 1;
                if (chord_idx < (uint8_t)chords_ptr->size()) {
                    const auto& chord = (*chords_ptr)[chord_idx];
                    int n_steps = (int)chord.size();
                    if (n_steps < 1) {
                        /* Empty chord — skip silently (no arp entries) */
                    } else {
                        auto to_gt2_arp = [](uint8_t sw_off) -> uint8_t {
                            /* Chord offsets (0x00..0x5F) are direct GT2 arp values */
                            return sw_off;
                        };
                        for (int s = 0; s < n_steps; s++) {
                            /* Set waveform on every step to keep gate open */
                            wv_left.push_back(L);
                            wv_right.push_back(to_gt2_arp(chord[s]));
                        }
                        wv_has_data = true;
                    }
                } /* end if chord_idx < chords_ptr->size() */
                /* The chord must loop continuously: emit a jump back to the start
                 * of this instrument's wave table entries, then stop processing.
                 * Do NOT fall through to the next entry (which would be the 0xFF
                 * stop marker, causing the arp to play only once). */
                wv_left.push_back(GT2_TABLE_JUMP);
                wv_right.push_back(wv_start);
                wv_terminated = true;
                break;
            } /* end if R == 0x7F */

            /* Normal arp conversion */
            if (R >= SW1_ARP_REL_MIN)
                R -= (SW1_ARP_REL_MIN - GT2_ARP_REL_MIN);
            else if (R >= SW1_ARP_ABS_MIN && R <= SW1_ARP_ABS_MAX)
                R -= 1;
            wv_left.push_back(L);
            wv_right.push_back(R);
            wv_has_data = true;
        }
        /* Only add a loop-back if the SW table had no explicit terminator
         * and had real data entries. */
        if (!wv_terminated && wv_has_data) {
            wv_left.push_back(GT2_TABLE_JUMP);
            wv_right.push_back(wv_start);
        }
        gi.wv = wv_has_data ? wv_start : 0;

        /* ---- Pulse table ---- */
        uint8_t pl_start = (uint8_t)(pl_left.size() + 1);
        bool pl_terminated = false;
        bool pl_has_data   = false; /* true once a real (non-stop/jump) entry is added */
        for (uint8_t t = pulse_offset; t < filter_offset && t < (uint8_t)tbl.size(); t += 3) {
            uint8_t L = tbl[t];
            uint8_t R = (t + 1 < (uint8_t)tbl.size()) ? tbl[t+1] : 0;
            if (L == SW1_TABLE_END) {
                if (pl_has_data) { /* only emit stop if there's something to stop after */
                    pl_left.push_back(GT2_TABLE_JUMP); pl_right.push_back(0x00);
                }
                pl_terminated = true; break;
            }
            if (L == SW1_TABLE_JUMP) {
                /* R is the absolute byte offset from the instrument params start.
                 * Pulse section starts at absolute byte SW1_INSTRUMENT_PARAMSIZE + pulse_offset. */
                uint8_t gt_target = (uint8_t)(pl_start + (R - (SW1_INSTRUMENT_PARAMSIZE + pulse_offset)) / 3);
                pl_left.push_back(GT2_TABLE_JUMP); pl_right.push_back(gt_target);
                pl_terminated = true; break;
            }
            pl_left.push_back(L);
            pl_right.push_back(R);
            pl_has_data = true;
        }
        if (!pl_terminated && pl_has_data) {
            pl_left.push_back(GT2_TABLE_JUMP);
            pl_right.push_back(0x00);
        }
        gi.pl = pl_has_data ? pl_start : 0;

        /* ---- Filter table ---- */
        uint8_t fl_start = (uint8_t)(fl_left.size() + 1);

        /* Collect raw SW filter bytes (excludes size marker) */
        std::vector<uint8_t> filter_section;
        for (uint8_t t = filter_offset; t < (uint8_t)tbl.size() - 1; t++)
            filter_section.push_back(tbl[t]);

        bool has_filter = !filter_section.empty();
        /* initial_ctrl (params byte[4]) is the SID filter register initial value.
         * It is stored in ALL instruments as part of SID-Wizard's internal state,
         * even those with no filter table. We must NOT use it alone as evidence of
         * a filter: only a non-empty filter_section means this instrument actually
         * drives the SID filter. */
        uint8_t initial_ctrl = si.params.arpchord_speed; /* byte[4] */

        if (has_filter) {
            /* filter_voice_mask[i] = 0 means this instrument is not the designated
             * filter setter at any orderlist position — skip it entirely (gi.fl stays 0).
             * A non-zero mask means this instrument owns the filter at some positions;
             * use that exact mask (no fallback). */
            uint8_t fmask = (i < filter_voice_mask.size()) ? filter_voice_mask[i] : 0;
            if (fmask != 0) {
                uint8_t filter_abs_start = (uint8_t)(SW1_INSTRUMENT_PARAMSIZE + filter_offset);
                SWFilterToGT2Filter(filter_section, fmask, initial_ctrl,
                                    filter_abs_start, fl_left, fl_right);
            }
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
 * Converts the SID-Wizard vibrato byte to a GT2 speedtable entry.
 * Uses Hermit's approach from SWMconvert.c (2026):
 *   - left  = GTvibRates[SW_period_nibble] | 0x80  (calculated mode)
 *   - right = SNGcalcVibratoAmpDiv[SW_amplitude_nibble]
 * Dpitch is retained in the signature for compatibility but not
 * used — calculated vibrato does not need note compensation.
 * Returns {left, right} for the GT2 speedtable entry.
 *************************************************************/
static std::pair<uint8_t,uint8_t> SWvibToGTvib(uint8_t SWvibr, uint8_t /*Dpitch*/) {
    if (SWvibr == 0) return {0, 0};
    uint8_t amp_nibble  = (SWvibr >> 4) & 0x0F;
    uint8_t freq_nibble =  SWvibr       & 0x0F;
    uint8_t left  = (uint8_t)(GTvibRates[freq_nibble] | SNG_CALC_BIT);
    uint8_t right = SNGcalcVibratoAmpDiv[amp_nibble];
    return {left, right};
}

/*************************************************************
 * ConvertInstrument
 * Converts one SWM instrument to a GT2 sng_instrument.
 * Also registers a vibrato speedtable entry if needed.
 *************************************************************/
static void ConvertInstrument(const SW_Instrument& si,
                              sng_instrument&      gi,
                              SpeedTableBuilder&   spdBuilder) {
    /* ADSR — direct 1:1 mapping.
     * params.ad (byte[3]) = note-start Attack/Decay.
     * params.sr (byte[4]) = note-start Sustain/Release.
     * (params.hr_ad/hr_sr at bytes[1][2] are the hard-restart ADSR values;
     *  GT2 has no separate HR-ADSR so those are not mapped.) */
    gi.ad = si.params.ad;
    gi.sr = si.params.sr;

    /* ---- Frame-1 waveform ----
     * frame1_waveform (params[15]) is the hard-restart waveform applied on
     * every note trigger in SID-Wizard (test-bit + gate = 0x09 is standard).
     * The wframe1 flag controls something else and does NOT gate whether
     * this waveform is applied.  Always copy it to gi.hr; leaving gi.hr=0
     * would suppress the hard restart entirely, making the instrument silent. */
    gi.hr = si.params.frame1_waveform;

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
void Convert(const SWMFile& swm, GT2File& gt, bool filterClone) {
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

    /* GT2 has a single global filter: only ONE instrument's filter table is
     * active at a time, and having multiple filter instruments simultaneously
     * causes them to cancel each other out.
     *
     * Strategy (per user guidance):
     *   1. Find the first filter-bearing instrument that plays on voice 1 (ch1).
     *      If none plays on voice 1, use the first filter-bearing instrument found.
     *   2. That instrument becomes the "primary" filter owner.  Its filter table
     *      in GT2 uses the ALL-channels mask (0x07) so the filter always affects
     *      every voice, matching SID-Wizard's global SID filter.
     *   3. All other filter-bearing instruments get their gi.fl set to 0 (no filter
     *      table in GT2).  In SID-Wizard the filter is a shared hardware resource,
     *      not per-instrument, so this is the correct semantic mapping.
     *
     * voice_assignments[i] is still computed for completeness (pulse tables, etc.) */

    /*---------------------------------------------------------
     * Per-instrument filter voice mask.
     *
     * Hermit (SID-Wizard author): "If all 3 channels control the filter
     * at the same time, the leftmost channel has priority.  But if then
     * any new filter-controlling instrument comes on any channel, it
     * takes over priority."
     *
     * Two modes selected by the filterClone flag:
     *
     * --no-filter-clone (simple):
     *   Each filter instrument gets mask = union of voice bits it plays on.
     *   No cloning.  Fast but routing may be imprecise.
     *
     * Default (row-level clone mode):
     *   Simulate the song row by row, tracking which instrument is playing
     *   on each voice and which has filter priority at every row.
     *   For each unique (priority_instrument, routing_mask) pair, create a
     *   GT2 instrument clone with that exact filter table mask.  Clone GT2
     *   patterns as needed so each pattern row references the right clone.
     *---------------------------------------------------------*/

    /* Helper: does instrument i (0-based) have a filter table? */
    /* Build a set of filter (L,R) parameter pairs that are UNIQUE across all
     * instruments.  If multiple instruments share the same filter params, they
     * were likely tagged as "filtered channel" by sng2swm but don't actually
     * control the filter independently — only uniquely-parameterised instruments
     * are real filter setters.  All-zero params (0x00,0x00) are always excluded. */
    std::map<std::pair<uint8_t,uint8_t>, int> filter_param_count;
    for (int i = 0; i < swm.header.instrument_count; i++) {
        const SW_Instrument& si2 = swm.instruments[i];
        uint8_t foff = (si2.params.filtertb_index >= SW1_INSTRUMENT_PARAMSIZE)
                       ? si2.params.filtertb_index - SW1_INSTRUMENT_PARAMSIZE
                       : (uint8_t)si2.tables.size();
        if (foff >= (uint8_t)(si2.tables.size() - 1)) continue; /* no filter */
        uint8_t L = si2.tables[foff];
        uint8_t R = (foff + 1 < (uint8_t)si2.tables.size()) ? si2.tables[foff + 1] : 0;
        if (L == 0 && R == 0) continue; /* all-zero: skip */
        filter_param_count[{L, R}]++;
    }

    auto instHasFilter = [&](int i) -> bool {
        if (i < 0 || i >= swm.header.instrument_count) return false;
        const SW_Instrument& si2 = swm.instruments[i];
        uint8_t foff = (si2.params.filtertb_index >= SW1_INSTRUMENT_PARAMSIZE)
                       ? si2.params.filtertb_index - SW1_INSTRUMENT_PARAMSIZE
                       : (uint8_t)si2.tables.size();
        if (foff >= (uint8_t)(si2.tables.size() - 1)) return false;
        uint8_t L = si2.tables[foff];
        uint8_t R = (foff + 1 < (uint8_t)si2.tables.size()) ? si2.tables[foff + 1] : 0;
        if (L == 0 && R == 0) return false; /* all-zero = pass-through */
        /* Shared filter params = sng2swm channel-routing artifact, not a real setter */
        return filter_param_count.count({L, R}) && filter_param_count.at({L, R}) == 1;
    };

    /* instHasFilterRaw: true if the instrument has ANY non-empty filter section,
     * regardless of uniqueness.  Used for computing the routing mask — voices
     * whose instruments have a filter section (even pass-through ones) should
     * be routed through the filter set by the priority instrument. */
    auto instHasFilterRaw = [&](int i) -> bool {
        if (i < 0 || i >= swm.header.instrument_count) return false;
        const SW_Instrument& si2 = swm.instruments[i];
        uint8_t foff = (si2.params.filtertb_index >= SW1_INSTRUMENT_PARAMSIZE)
                       ? si2.params.filtertb_index - SW1_INSTRUMENT_PARAMSIZE
                       : (uint8_t)si2.tables.size();
        if (foff >= (uint8_t)(si2.tables.size() - 1)) return false;
        uint8_t L = si2.tables[foff];
        uint8_t R = (foff + 1 < (uint8_t)si2.tables.size()) ? si2.tables[foff + 1] : 0;
        return !(L == 0 && R == 0);
    };

    /* filter_voice_mask[i] = GT2 channel routing byte for instrument i (0-based).
     * 0 = no filter table entry. */
    std::vector<uint8_t> filter_voice_mask(swm.header.instrument_count, 0);

    /* filter_instr_clone[(sw_1based, mask)] = GT2 instrument index (0-based).
     * Populated only in clone mode. */
    std::map<std::pair<uint8_t,uint8_t>, int> filter_instr_clone;

    /* FilterCtx and row_ctx are populated in clone mode and consumed by
     * the post-processing pass below.  Declared here so both sections
     * can see them regardless of which branch runs. */
    struct FilterCtx { uint8_t priority_iv; uint8_t mask; };
    std::map<std::pair<int,int>, FilterCtx> row_ctx;

    if (!filterClone) {
        /* ---- Simple mode: per-voice mask from orderlist scan ---- */
        std::map<int,uint8_t> ivm;
        for (int v = 0; v < 3; v++) {
            uint8_t vbit = (uint8_t)(1u << v);
            for (int si2 = 0; si2 < subtunes; si2++) {
                const sng_orderlist& ol = gt.orderlists[si2].voice[v];
                for (int e = 0; e < ol.length; e++) {
                    uint8_t entry = ol.data[e];
                    if (entry == SW1_SEQUENCE_LOOPSONG) break;
                    if (entry >= GT2_ORDERLIST_TRANS_MIN) continue;
                    int sw_ptn = (int)entry + 1;
                    if (sw_ptn < 1 || sw_ptn > swm.header.pattern_count) continue;
                    const auto& up = UnpackPattern(swm.patterns[sw_ptn].packed);
                    for (size_t ui = 0; ui < up.size(); ) {
                        uint8_t nb = up[ui++];
                        if (nb & 0x80) {
                            if (ui >= up.size()) break;
                            uint8_t ib = up[ui++];
                            uint8_t iv2 = ib & 0x7F;
                            if (iv2 >= SW1_INSTRUMENT_MIN && iv2 <= SW1_INSTRUMENT_MAX)
                                ivm[iv2] |= vbit;
                            if (ib & 0x80 && ui < up.size()) {
                                uint8_t fx = up[ui++];
                                if (fx > 0 && fx < SW1_SMALLFX_MIN && ui < up.size()) ui++;
                            }
                        }
                    }
                }
            }
        }
        for (int i = 0; i < swm.header.instrument_count; i++) {
            if (instHasFilter(i)) {
                /* Voice mask: include voices from ALL raw-filter instruments,
                 * not just unique setters, so the routing covers pass-throughs too. */
                uint8_t vm = 0;
                for (int j = 0; j < swm.header.instrument_count; j++) {
                    if (instHasFilterRaw(j) && ivm.count(j+1))
                        vm |= ivm[j+1];
                }
                if (!vm) vm = 0x07;
                filter_voice_mask[i] = vm;
                printf("    Inst %d '%.8s': filter mask=0x%02X (simple)\n",
                       i+1, swm.instruments[i].name, vm);
            }
        }

    } else {
        /* ---- Clone mode: row-level simulation ---- */

        /* Per-pattern row cache: each row stores the note-trigger info */
        struct RowInfo { uint8_t instr; bool has_note; };
        std::map<int, std::vector<RowInfo>> ptn_row_cache; /* sw_ptn_1based -> rows */

        auto getRows = [&](int sw_ptn) -> const std::vector<RowInfo>& {
            auto it = ptn_row_cache.find(sw_ptn);
            if (it != ptn_row_cache.end()) return it->second;
            std::vector<RowInfo>& rv = ptn_row_cache[sw_ptn];
            if (sw_ptn < 1 || sw_ptn > swm.header.pattern_count) return rv;
            int target_rows = swm.patterns[sw_ptn].length;
            rv.resize(target_rows, {0, false});
            const auto& up = UnpackPattern(swm.patterns[sw_ptn].packed);
            int row = 0;
            for (size_t ui = 0; ui < up.size() && row < target_rows; ) {
                uint8_t nb = up[ui++];
                if (nb >= 0x70 && nb <= 0x77) {
                    row += (nb - 0x70) + 2;
                } else if (nb == 0x7E || nb == 0x00) {
                    row++;
                } else if (nb & 0x80) {
                    if (row < target_rows) {
                        rv[row].has_note = true;
                        if (ui < up.size()) {
                            uint8_t ib = up[ui++];
                            uint8_t iv2 = ib & 0x7F;
                            if (iv2 >= SW1_INSTRUMENT_MIN && iv2 <= SW1_INSTRUMENT_MAX)
                                rv[row].instr = iv2;
                            if (ib & 0x80 && ui < up.size()) {
                                uint8_t fx = up[ui++];
                                if (fx > 0 && fx < SW1_SMALLFX_MIN && ui < up.size()) ui++;
                            }
                        }
                    }
                    row++;
                } else {
                    row++;
                }
            }
            return rv;
        };

        /* Pre-cache all referenced patterns */
        for (int si2 = 0; si2 < subtunes; si2++)
            for (int v = 0; v < 3; v++) {
                const sng_orderlist& ol = gt.orderlists[si2].voice[v];
                for (int e = 0; e < ol.length; e++) {
                    uint8_t entry = ol.data[e];
                    if (entry == SW1_SEQUENCE_LOOPSONG) break;
                    if (entry >= GT2_ORDERLIST_TRANS_MIN) continue;
                    getRows((int)entry + 1);
                }
            }

        /* Row-level filter context:
         * For each (gt_ptn_0based, row), record which (priority_iv_1based, mask)
         * is active.  Only recorded when priority_iv has a filter. */

        for (int si2 = 0; si2 < subtunes; si2++) {
            int n_pos = 0;
            for (int v = 0; v < 3; v++)
                n_pos = std::max(n_pos, (int)gt.orderlists[si2].voice[v].length);

            uint8_t cur_instr[3]  = {0, 0, 0}; /* 1-based, per voice */
            uint8_t priority_iv   = 0;           /* 1-based filter priority */

            for (int pos = 0; pos < n_pos; pos++) {
                int sw_ptns[3] = {0, 0, 0};
                int n_rows = 0;
                for (int v = 0; v < 3; v++) {
                    const sng_orderlist& ol = gt.orderlists[si2].voice[v];
                    if (pos >= ol.length) continue;
                    uint8_t entry = ol.data[pos];
                    if (entry == SW1_SEQUENCE_LOOPSONG) break;
                    if (entry >= GT2_ORDERLIST_TRANS_MIN) continue;
                    sw_ptns[v] = (int)entry + 1;
                    if (sw_ptns[v] >= 1 && sw_ptns[v] <= swm.header.pattern_count)
                        n_rows = std::max(n_rows, (int)swm.patterns[sw_ptns[v]].length);
                }
                if (!n_rows) continue;

                for (int row = 0; row < n_rows; row++) {
                    /* Collect new triggers this row */
                    bool triggered[3] = {false, false, false};
                    for (int v = 0; v < 3; v++) {
                        if (!sw_ptns[v]) continue;
                        const auto& rows = getRows(sw_ptns[v]);
                        if (row >= (int)rows.size()) continue;
                        if (!rows[row].has_note) continue;
                        triggered[v] = true;
                        if (rows[row].instr)
                            cur_instr[v] = rows[row].instr;
                    }

                    /* Routing mask: voices whose cur_instr has ANY filter section
                     * (including pass-through instruments).  This ensures voices
                     * routed through the filter in SID-Wizard are also routed in GT2,
                     * even if those instruments are not themselves priority setters. */
                    uint8_t mask = 0;
                    for (int v = 0; v < 3; v++)
                        if (cur_instr[v] && instHasFilterRaw(cur_instr[v] - 1))
                            mask |= (uint8_t)(1u << v);
                    if (!mask) continue;

                    /* Priority: leftmost new trigger with a UNIQUE filter instrument
                     * (instHasFilter, not raw — only real setters change priority). */
                    for (int v = 0; v < 3; v++) {
                        if (triggered[v] && cur_instr[v] &&
                                instHasFilter(cur_instr[v] - 1)) {
                            priority_iv = cur_instr[v];
                            break;
                        }
                    }
                    if (!priority_iv) continue;

                    /* Record for each voice whose pattern row is driven by priority_iv */
                    for (int v = 0; v < 3; v++) {
                        if (!sw_ptns[v] || cur_instr[v] != priority_iv) continue;
                        auto key = std::make_pair(sw_ptns[v] - 1, row);
                        /* Only update if this row has a new priority trigger,
                         * or if not yet recorded */
                        if (triggered[v] || row_ctx.find(key) == row_ctx.end())
                            row_ctx[key] = {priority_iv, mask};
                    }
                }
            }
        }

        /* Build (iv, mask) -> GT2 instrument index.
         * Most-common mask per iv becomes the base; others become clones. */
        std::map<uint8_t, std::map<uint8_t,int>> iv_mask_freq;
        for (auto& [key, ctx] : row_ctx)
            iv_mask_freq[ctx.priority_iv][ctx.mask]++;

        for (auto& [iv, mfreq] : iv_mask_freq) {
            /* Base mask: use the mask routing the MOST voices (highest popcount).
             * This ensures the base instrument covers the widest filter context;
             * clones handle reduced-voice cases.  Ties broken by highest value. */
            uint8_t base_mask = mfreq.begin()->first;
            int     best_pop  = -1;
            for (auto& [m, f] : mfreq) {
                int pop = __builtin_popcount(m);
                if (pop > best_pop || (pop == best_pop && m > base_mask)) {
                    best_pop  = pop;
                    base_mask = m;
                }
            }

            filter_voice_mask[iv - 1]           = base_mask;
            filter_instr_clone[{iv, base_mask}] = (int)(iv - 1);

            for (auto& [m, f] : mfreq) {
                if (m == base_mask) continue;
                int new_idx = (int)gt.instruments.size();
                gt.instruments.push_back(gt.instruments[iv - 1]);
                filter_instr_clone[{iv, m}] = new_idx;
                printf("  Clone inst %d '%.8s' -> GT2 inst %d for filter mask 0x%02X\n",
                       (int)iv, swm.instruments[iv-1].name, new_idx + 1, m);
            }
            printf("    Inst %d '%.8s': base filter mask=0x%02X\n",
                   (int)iv, swm.instruments[iv-1].name, base_mask);
        }
    } /* end clone mode */

    /*---------------------------------------------------------
     * Post-processing: remap pattern instrument refs to filter clones.
     * Only runs in clone mode when clones were actually created.
     *---------------------------------------------------------*/



    /* Build sw_instr_ext: one SW_Instrument entry per GT2 instrument slot.
     * Original slots (0..N-1) map directly to swm.instruments[i].
     * Clone slots (N..) map back to their source SW instrument using
     * filter_instr_clone, so ReconstructTables uses the correct tables. */
    std::vector<SW_Instrument> sw_instr_ext = swm.instruments;
    {
        std::map<int,int> clone_to_sw; /* gt_idx(0-based) -> sw_idx(0-based) */
        for (auto& [key, gt_idx] : filter_instr_clone)
            clone_to_sw[gt_idx] = (int)(key.first) - 1;
        for (int gi = (int)swm.instruments.size(); gi < (int)gt.instruments.size(); gi++) {
            auto it = clone_to_sw.find(gi);
            sw_instr_ext.push_back(it != clone_to_sw.end()
                ? swm.instruments[it->second]
                : swm.instruments.back());
        }
    }

    /* Pre-allocate wave table with maximum capacity (255 entries) so that
     * GetOrCreateArpWtPos can safely append chord-arp entries to it after
     * ReconstructTables fills the initial instrument wave table data. */
    gt.wavetable.left  = new uint8_t[255]();
    gt.wavetable.right = new uint8_t[255]();
    gt.wavetable.length = 0;

    /* Extend filter_voice_mask and voice_assignments to cover cloned GT2 instruments.
     * filter_instr_clone maps (sw_iv, mask) -> gt_idx; use it to assign each clone's mask. */
    std::vector<uint8_t> filter_voice_mask_ext = filter_voice_mask;
    filter_voice_mask_ext.resize(gt.instruments.size(), 0);
    for (auto& [key, gt_idx] : filter_instr_clone)
        if (gt_idx < (int)filter_voice_mask_ext.size())
            filter_voice_mask_ext[gt_idx] = key.second;

    /* Clones share voice routing with their source; pad with 0 for safety. */
    voice_assignments.resize(gt.instruments.size(), 0);

    ReconstructTables(sw_instr_ext, gt.instruments, voice_assignments,
                      filter_voice_mask_ext,
                      swm.chords.empty() ? nullptr : &swm.chords,
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

        /* Collect the first (orderlist pos 0) SW pattern for each voice.
         * If ANY of them already carries a tempo FX at row 0 in the SW pattern
         * data, the song self-establishes its tempo and no injection is needed.
         * Injecting would re-assert the subtune default every time one of the
         * other voices' pos-0 patterns loops back, causing incorrect speed. */
        auto swPtnHasTempoAtRow0 = [&](int sw_ptn_1based) -> bool {
            if (sw_ptn_1based < 1 ||
                sw_ptn_1based > swm.header.pattern_count) return false;
            const auto& raw = swm.patterns[sw_ptn_1based].packed;
            int row = 0;
            for (size_t bi = 0; bi < raw.size(); ) {
                uint8_t b = raw[bi++];
                if (b >= 0x70 && b <= 0x77) { row += (b-0x70)+2; break; }
                if (b == 0x7E || b == 0x00) { row++; break; }
                if (b & 0x80) {
                    row++;
                    if (bi >= raw.size()) break;
                    uint8_t ib = raw[bi++];
                    if (ib & 0x80) {
                        if (bi >= raw.size()) break;
                        uint8_t fx = raw[bi++];
                        if (fx == SW1_BIGFX_TEMPO || fx == SW1_BIGFX_FUNKTEMPO)
                            return true;
                        if (fx > 0 && fx < SW1_SMALLFX_MIN && bi < raw.size())
                            bi++;  /* skip fxval */
                    }
                    break; /* only care about row 0 */
                }
                break;
            }
            return false;
        };

        bool any_pos0_has_tempo = false;
        for (int v = 0; v < 3; v++) {
            const sng_orderlist& ol = gt.orderlists[si].voice[v];
            for (int e = 0; e < ol.length; e++) {
                uint8_t entry = ol.data[e];
                if (entry < GT2_ORDERLIST_TRANS_MIN) {
                    int sw_ptn = (int)entry + 1; /* GT2 0-based -> SW 1-based */
                    if (swPtnHasTempoAtRow0(sw_ptn))
                        any_pos0_has_tempo = true;
                    break;
                }
            }
        }
        if (any_pos0_has_tempo) {
            printf("    (pos-0 pattern already has tempo FX; skipping injection)\n");
            continue;
        }

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

    /* Per-instrument octave shifts.
     * SID-Wizard stores a signed semitone offset in params.octave_shift (byte[9]).
     * GT2 has no equivalent; we bake the shift directly into each note value.
     * oct_shifts[sw_instr_1based] = shift in semitones (int8_t). */
    std::vector<int8_t> oct_shifts(swm.header.instrument_count + 1, 0);
    for (int i = 0; i < swm.header.instrument_count; i++) {
        int8_t shift = (int8_t)swm.instruments[i].params.octave_shift;
        oct_shifts[i + 1] = shift;  /* SW instruments are 1-based */
        if (shift != 0)
            printf("  Instrument %d: octave_shift=%d semitones (%+d octave%s)\n",
                   i + 1, (int)shift, (int)(shift / 12),
                   std::abs(shift / 12) == 1 ? "" : "s");
    }

    /* Patterns (SWM numbering starts at 1, GT2 from 0) */
    gt.patterns.resize(swm.header.pattern_count);
    std::map<std::pair<int,int>,uint8_t> arp_cache;

    /* Per-voice active instrument state, carried across pattern boundaries.
     * Indexed [subtune * 3 + voice].  Each entry is the SW instrument number
     * (1-based) most recently set on that voice, or 0 if none yet. */
    std::vector<uint8_t> voice_cur_instr(subtunes * 3, 0);

    /* Build a map from GT2 pattern index → which (subtune, voice) plays it first,
     * so we can pass the correct inout_instr pointer to ConvertPattern.
     * A pattern may be played by multiple voices; we thread the state from the
     * orderlist perspective (voice 0 of subtune 0 → voice 1 → voice 2, etc.). */

    /* Simple approach: convert patterns in orderlist play order per voice so the
     * instrument state is threaded correctly.  We still convert each pattern once;
     * if a pattern is shared between voices the last writer wins (acceptable since
     * the octave shift is an instrument property, not a pattern property). */
    std::vector<bool> ptn_converted(swm.header.pattern_count, false);

    /* First pass: convert patterns in orderlist order, threading instrument state. */
    for (int si = 0; si < subtunes; si++) {
        for (int v = 0; v < 3; v++) {
            const sng_orderlist& ol = gt.orderlists[si].voice[v];
            uint8_t* vinstr = &voice_cur_instr[si * 3 + v];
            for (int e = 0; e < ol.length; e++) {
                uint8_t entry = ol.data[e];
                if (entry == SW1_SEQUENCE_LOOPSONG) break;
                if (entry >= GT2_ORDERLIST_TRANS_MIN) continue; /* transpose */
                int ptn_idx = (int)entry; /* 0-based GT2 pattern index */
                if (ptn_idx < 0 || ptn_idx >= swm.header.pattern_count) continue;
                if (!ptn_converted[ptn_idx]) {
                    auto [tcmd, tparam] = ptnTempoInject[ptn_idx];
                    ConvertPattern(swm.patterns[ptn_idx + 1], gt.patterns[ptn_idx],
                                   spdBuilder, tcmd, tparam,
                                   swm.chords.empty() ? nullptr : &swm.chords,
                                   &arp_cache, &gt, &oct_shifts, vinstr,
                                   &swm.instruments);
                    ptn_converted[ptn_idx] = true;
                } else {
                    /* Pattern already converted; still thread the instrument state
                     * by simulating a scan of its rows to find the last instrument set. */
                    auto unpacked = UnpackPattern(swm.patterns[ptn_idx + 1].packed);
                    size_t idx2 = 0;
                    while (idx2 < unpacked.size()) {
                        uint8_t nb = unpacked[idx2++];
                        if (nb & 0x80) {
                            if (idx2 < unpacked.size()) {
                                uint8_t ib = unpacked[idx2++];
                                uint8_t iv = ib & 0x7F;
                                if (iv >= SW1_INSTRUMENT_MIN && iv <= SW1_INSTRUMENT_MAX)
                                    *vinstr = iv;
                                if (ib & 0x80) {
                                    if (idx2 < unpacked.size()) {
                                        uint8_t fx = unpacked[idx2++];
                                        if (fx > 0 && fx < SW1_SMALLFX_MIN && idx2 < unpacked.size())
                                            idx2++;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* Convert any patterns not reached by the orderlists (e.g. unused patterns). */
    for (int i = 0; i < swm.header.pattern_count; i++) {
        if (!ptn_converted[i]) {
            auto [tcmd, tparam] = ptnTempoInject[i];
            ConvertPattern(swm.patterns[i + 1], gt.patterns[i],
                           spdBuilder, tcmd, tparam,
                           swm.chords.empty() ? nullptr : &swm.chords,
                           &arp_cache, &gt, &oct_shifts, nullptr);
        }
    }

    /* Commit speedtable (may have entries from tempo and/or slide effects) */
    spdBuilder.fill(gt.speedtable);

    if (filterClone && !filter_instr_clone.empty()) {
        /* For each (gt_ptn_0based, row) that needs a non-base clone,
         * remap the instrument reference.  Clone the GT2 pattern first
         * if the same pattern is also used with the base instrument. */
        struct RowRemap { int row; int clone_gt_idx; uint8_t sw_iv; };
        std::map<int, std::vector<RowRemap>> ptn_remaps;

        for (auto& [key, ctx] : row_ctx) {
            int gt_ptn  = key.first;
            int row     = key.second;
            uint8_t iv  = ctx.priority_iv;
            uint8_t mask = ctx.mask;
            auto it = filter_instr_clone.find({iv, mask});
            if (it == filter_instr_clone.end()) continue;
            int clone_idx = it->second;
            if (clone_idx == (int)(iv - 1)) continue; /* base — nothing to do */
            if (gt_ptn < 0 || gt_ptn >= (int)gt.patterns.size()) continue;
            ptn_remaps[gt_ptn].push_back({row, clone_idx, iv});
        }

        std::map<std::pair<int,int>, int> ptn_clone_cache;

        for (auto& [gt_ptn, remaps] : ptn_remaps) {
            if (gt_ptn < 0 || gt_ptn >= (int)gt.patterns.size()) continue;

            /* Group remaps by clone index */
            std::map<int, std::vector<int>> clone_to_rows;
            for (auto& rr : remaps)
                clone_to_rows[rr.clone_gt_idx].push_back(rr.row);

            for (auto& [clone_idx, rows_need] : clone_to_rows) {
                uint8_t sw_iv = 0;
                for (auto& rr : remaps)
                    if (rr.clone_gt_idx == clone_idx) { sw_iv = rr.sw_iv; break; }
                uint8_t base_gt1 = (uint8_t)sw_iv; /* 1-based GT2 instrument */
                uint8_t clone_gt1 = (uint8_t)(clone_idx + 1);

                /* Does the pattern have rows using base instrument NOT in rows_need? */
                bool needs_clone_ptn = false;
                {
                    sng_pattern& ptn = gt.patterns[gt_ptn];
                    if (ptn.data) {
                        for (int r = 0; r < ptn.rows && !needs_clone_ptn; r++) {
                            if (ptn.data[r].instrument != base_gt1) continue;
                            bool in_remap = false;
                            for (int rn : rows_need) if (rn == r) { in_remap = true; break; }
                            if (!in_remap) needs_clone_ptn = true;
                        }
                    }
                }

                int target_ptn = gt_ptn;
                if (needs_clone_ptn) {
                    auto pc_key = std::make_pair(gt_ptn, clone_idx);
                    auto pc_it  = ptn_clone_cache.find(pc_key);
                    if (pc_it != ptn_clone_cache.end()) {
                        target_ptn = pc_it->second;
                    } else {
                        sng_pattern ptn_copy = gt.patterns[gt_ptn];
                        /* Deep-copy data array to avoid double-free in FreeGT2File */
                        if (ptn_copy.rows && ptn_copy.data) {
                            ptn_copy.data = new sng_pattern_row[ptn_copy.rows];
                            memcpy(ptn_copy.data, gt.patterns[gt_ptn].data,
                                   ptn_copy.rows * sizeof(sng_pattern_row));
                        }
                        target_ptn = (int)gt.patterns.size();
                        gt.patterns.push_back(ptn_copy);
                        ptn_clone_cache[pc_key] = target_ptn;
                        printf("  Cloned GT2 pattern %d -> %d (filter clone inst %d)\n",
                               gt_ptn + 1, target_ptn + 1, clone_idx + 1);
                    }
                }

                if (target_ptn < 0 || target_ptn >= (int)gt.patterns.size()) continue;
                sng_pattern& ptn = gt.patterns[target_ptn];
                if (!ptn.data || !ptn.rows) continue;

                /* Remap the specific rows */
                for (int rn : rows_need) {
                    if (rn < ptn.rows && ptn.data[rn].instrument == base_gt1)
                        ptn.data[rn].instrument = clone_gt1;
                }

                /* Point relevant orderlist entries to the cloned pattern */
                if (needs_clone_ptn && target_ptn != gt_ptn && target_ptn <= 0xFF) {
                    for (int si2 = 0; si2 < subtunes; si2++)
                        for (int v = 0; v < 3; v++) {
                            sng_orderlist& ol = gt.orderlists[si2].voice[v];
                            for (int e = 0; e < ol.length; e++)
                                if (ol.data[e] == (uint8_t)gt_ptn)
                                    ol.data[e] = (uint8_t)target_ptn;
                        }
                }
            }
        }
    }
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
