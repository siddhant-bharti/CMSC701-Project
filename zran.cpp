/* zran.c -- example of deflate stream indexing and random access
 * Copyright (C) 2005, 2012, 2018, 2023, 2024 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 * Version 1.5  4 Feb 2024  Mark Adler */

/* Version History:
 1.0  29 May 2005  First version
 1.1  29 Sep 2012  Fix memory reallocation error
 1.2  14 Oct 2018  Handle gzip streams with multiple members
                   Add a header file to facilitate usage in applications
 1.3  18 Feb 2023  Permit raw deflate streams as well as zlib and gzip
                   Permit crossing gzip member boundaries when extracting
                   Support a size_t size when extracting (was an int)
                   Do a binary search over the index for an access point
                   Expose the access point type to enable save and load
 1.4  13 Apr 2023  Add a NOPRIME define to not use inflatePrime()
 1.5   4 Feb 2024  Set returned index to NULL on an index build error
                   Stop decoding once request is satisfied
                   Provide a reusable inflate engine in the index
                   Allocate the dictionaries to reduce memory usage
 */

// Illustrate the use of Z_BLOCK, inflatePrime(), and inflateSetDictionary()
// for random access of a compressed file. A file containing a raw deflate
// stream is provided on the command line. The compressed stream is decoded in
// its entirety, and an index built with access points about every SPAN bytes
// in the uncompressed output. The compressed file is left open, and can then
// be read randomly, having to decompress on the average SPAN/2 uncompressed
// bytes before getting to the desired block of data.
//
// An access point can be created at the start of any deflate block, by saving
// the starting file offset and bit of that block, and the 32K bytes of
// uncompressed data that precede that block. Also the uncompressed offset of
// that block is saved to provide a reference for locating a desired starting
// point in the uncompressed stream. deflate_index_build() decompresses the
// input raw deflate stream a block at a time, and at the end of each block
// decides if enough uncompressed data has gone by to justify the creation of a
// new access point. If so, that point is saved in a data structure that grows
// as needed to accommodate the points.
//
// To use the index, an offset in the uncompressed data is provided, for which
// the latest access point at or preceding that offset is located in the index.
// The input file is positioned to the specified location in the index, and if
// necessary the first few bits of the compressed data is read from the file.
// inflate is initialized with those bits and the 32K of uncompressed data, and
// decompression then proceeds until the desired offset in the file is reached.
// Then decompression continues to read the requested uncompressed data from
// the file.
//
// There is some fair bit of overhead to starting inflation for the random
// access, mainly copying the 32K byte dictionary. If small pieces of the file
// are being accessed, it would make sense to implement a cache to hold some
// lookahead to avoid many calls to deflate_index_extract() for small lengths.
//
// Another way to build an index would be to use inflateCopy(). That would not
// be constrained to have access points at block boundaries, but would require
// more memory per access point, and could not be saved to a file due to the
// use of pointers in the state. The approach here allows for storage of the
// index in a file.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <zlib.h>
#include <iostream>

// Access point.
typedef struct point {
    off_t out;          // offset in uncompressed data
    off_t in;           // offset in compressed file of first full byte
    int bits;           // 0, or number of bits (1-7) from byte at in-1
    unsigned dict;      // number of bytes in window to use as a dictionary
    unsigned char *window;  // preceding 32K (or less) of uncompressed data
} point_t;

// Access point list.
struct deflate_index {
    int have;           // number of access points in list
    int mode;           // -15 for raw, 15 for zlib, or 31 for gzip
    off_t length;       // total length of uncompressed data
    point_t *list;      // allocated list of access points
    z_stream strm;      // re-usable inflate engine for extraction
};

#define WINSIZE 32768U      // sliding window size
#define CHUNK 16384         // file input buffer size

// Function to print an access point in one line
void print_point(point_t *point) {
    // Print an entire access point in one line
    std::cout << "out: " << point->out << ", in: " << point->in << ", bits: " << point->bits << ", dict: " << point->dict << ", window: ";
    for (int i = 0; i < 10; i++) {
        std::cout << (int) point->window[i] << " ";
    }
    std::cout << std::endl;
}

// Function to print the index
void print_index(struct deflate_index *index) {
    std::cout << "================ index: ================" << std::endl;

    // Print metadata
    std::cout << "mode: " << index->mode << std::endl;
    std::cout << "length: " << index->length << std::endl;
    std::cout << "have: " << index->have << std::endl;

    // Print strm
    std::cout << "strm.avail_in: " << index->strm.avail_in << std::endl;
    std::cout << "strm.avail_out: " << index->strm.avail_out << std::endl;
    std::cout << "strm.data_type: " << index->strm.data_type << std::endl;
    std::cout << "strm.zalloc: " << index->strm.zalloc << std::endl;
    std::cout << "strm.zfree: " << index->strm.zfree << std::endl;
    std::cout << "strm.opaque: " << index->strm.opaque << std::endl;

    // Print the access points
    for (int i = 0; i < index->have; i++) {
        print_point(index->list + i);
    }
    std::cout << "========================================" << std::endl;

}

// See comments in zran.h.
void deflate_index_free(struct deflate_index *index) {
    if (index != NULL) {
        size_t i = index->have;
        while (i)
            free(index->list[--i].window);
        free(index->list);
        inflateEnd(&index->strm);
        free(index);
    }
}

// Save index to file.
int deflate_index_save(FILE *out, struct deflate_index *index) {
    // Write metadata
    if (fwrite(&index->mode, sizeof(index->mode), 1, out) != 1 ||
        fwrite(&index->length, sizeof(index->length), 1, out) != 1 ||
        fwrite(&index->have, sizeof(index->have), 1, out) != 1)
        return Z_ERRNO;

    // Write the access points.
    for (int i = 0; i < index->have; i++) {
        point_t *point = index->list + i;
        if (fwrite(&point->out, sizeof(point->out), 1, out) != 1 ||
            fwrite(&point->in, sizeof(point->in), 1, out) != 1 ||
            fwrite(&point->bits, sizeof(point->bits), 1, out) != 1 ||
            fwrite(&point->dict, sizeof(point->dict), 1, out) != 1 ||
            fwrite(point->window, 1, point->dict, out) != point->dict)
            return Z_ERRNO;
    }

    return 0;
}

// Read index from file.
int deflate_index_load(FILE *in, struct deflate_index **built) {
    struct deflate_index *index = (struct deflate_index*) malloc(sizeof(struct deflate_index));
    if (index == NULL)
        return Z_MEM_ERROR;

    // Read metadata
    if (fread(&index->mode, sizeof(index->mode), 1, in) != 1 ||
        fread(&index->length, sizeof(index->length), 1, in) != 1 ||
        fread(&index->have, sizeof(index->have), 1, in) != 1) {
        free(index);
        return Z_ERRNO;
    }

    // Read the access points.
    index->list = (point_t *) malloc(sizeof(point_t) * index->have);
    if (index->list == NULL) {
        deflate_index_free(index);
        return Z_MEM_ERROR;
    }
    for (int i = 0; i < index->have; i++) {
        point_t *point = index->list + i;
        if (fread(&point->out, sizeof(point->out), 1, in) != 1 ||
            fread(&point->in, sizeof(point->in), 1, in) != 1 ||
            fread(&point->bits, sizeof(point->bits), 1, in) != 1 ||
            fread(&point->dict, sizeof(point->dict), 1, in) != 1) {
            deflate_index_free(index);
            return Z_ERRNO;
        }
        point->window = (unsigned char*) malloc(point->dict);
        if (point->window == NULL) {
            deflate_index_free(index);
            return Z_MEM_ERROR;
        }
        if (fread(point->window, 1, point->dict, in) != point->dict) {
            deflate_index_free(index);
            return Z_ERRNO;
        }
    }

    // Initialize the inflation state.
    index->strm.zalloc = Z_NULL;
    index->strm.zfree = Z_NULL;
    index->strm.opaque = Z_NULL;
    inflateInit2(&index->strm, index->mode);

    // Return the index.
    *built = index;
    return index->have;
}

// Add an access point to the list. If out of memory, deallocate the existing
// list and return NULL. index->mode is temporarily the allocated number of
// access points, until it is time for deflate_index_build() to return. Then
// index->mode is set to the mode of inflation.
static struct deflate_index *add_point(struct deflate_index *index, off_t in,
                                       off_t out, off_t beg,
                                       unsigned char *window) {
    if (index->have == index->mode) {
        // The list is full. Make it bigger.
        index->mode = index->mode ? index->mode << 1 : 8;
        point_t *next = (point_t *)realloc(index->list, sizeof(point_t) * index->mode);
        if (next == NULL) {
            deflate_index_free(index);
            return NULL;
        }
        index->list = next;
    }

    // Fill in the access point and increment how many we have.
    point_t *next = (point_t *)(index->list) + index->have++;
    if (index->have < 0) {
        // Overflowed the int!
        deflate_index_free(index);
        return NULL;
    }
    next->out = out;
    next->in = in;
    next->bits = index->strm.data_type & 7;
    next->dict = out - beg > WINSIZE ? WINSIZE : (unsigned)(out - beg);
    next->window = (unsigned char*) malloc(next->dict);
    if (next->window == NULL) {
        deflate_index_free(index);
        return NULL;
    }
    unsigned recent = WINSIZE - index->strm.avail_out;
    unsigned copy = recent > next->dict ? next->dict : recent;
    memcpy(next->window + next->dict - copy, window + recent - copy, copy);
    copy = next->dict - copy;
    memcpy(next->window, window + WINSIZE - copy, copy);

    // Return the index, which may have been newly allocated or destroyed.
    return index;
}

// Decompression modes. These are the inflateInit2() windowBits parameter.
#define RAW -15
#define ZLIB 15
#define GZIP 31

// See comments in zran.h.
int deflate_index_build(FILE *in, off_t span, struct deflate_index **built) {
    // If this returns with an error, any attempt to use the index will cleanly
    // return an error.
    *built = NULL;

    // Create and initialize the index list.
    struct deflate_index *index = (struct deflate_index*) malloc(sizeof(struct deflate_index));
    if (index == NULL)
        return Z_MEM_ERROR;
    index->have = 0;
    index->mode = 0;            // entries in index->list allocation
    index->list = NULL;
    index->strm.state = Z_NULL; // so inflateEnd() can work

    // Set up the inflation state.
    index->strm.avail_in = 0;
    index->strm.avail_out = 0;
    unsigned char buf[CHUNK];   // input buffer
    unsigned char win[WINSIZE] = {0};   // output sliding window
    off_t totin = 0;            // total bytes read from input
    off_t totout = 0;           // total bytes uncompressed
    off_t beg = 0;              // starting offset of last history reset
    int mode = 0;               // mode: RAW, ZLIB, or GZIP (0 => not set yet)

    // Decompress from in, generating access points along the way.
    int ret;                    // the return value from zlib, or Z_ERRNO
    off_t last;                 // last access point uncompressed offset
    do {
        // Assure available input, at least until reaching EOF.
        if (index->strm.avail_in == 0) {
            index->strm.avail_in = fread(buf, 1, sizeof(buf), in);
            totin += index->strm.avail_in;
            index->strm.next_in = buf;
            if (index->strm.avail_in < sizeof(buf) && ferror(in)) {
                ret = Z_ERRNO;
                break;
            }

            if (mode == 0) {
                // At the start of the input -- determine the type. Assume raw
                // if it is neither zlib nor gzip. This could in theory result
                // in a false positive for zlib, but in practice the fill bits
                // after a stored block are always zeros, so a raw stream won't
                // start with an 8 in the low nybble.
                mode = index->strm.avail_in == 0 ? RAW :    // will fail
                       (index->strm.next_in[0] & 0xf) == 8 ? ZLIB :
                       index->strm.next_in[0] == 0x1f ? GZIP :
                       /* else */ RAW;
                index->strm.zalloc = Z_NULL;
                index->strm.zfree = Z_NULL;
                index->strm.opaque = Z_NULL;
                ret = inflateInit2(&index->strm, mode);
                if (ret != Z_OK)
                    break;
            }
        }

        // Assure available output. This rotates the output through, for use as
        // a sliding window on the uncompressed data.
        if (index->strm.avail_out == 0) {
            index->strm.avail_out = sizeof(win);
            index->strm.next_out = win;
        }

        if (mode == RAW && index->have == 0)
            // We skip the inflate() call at the start of raw deflate data in
            // order generate an access point there. Set data_type to imitate
            // the end of a header.
            index->strm.data_type = 0x80;
        else {
            // Inflate and update the number of uncompressed bytes.
            unsigned before = index->strm.avail_out;
            ret = inflate(&index->strm, Z_BLOCK);
            totout += before - index->strm.avail_out;
        }

        if ((index->strm.data_type & 0xc0) == 0x80 &&
            (index->have == 0 || totout - last >= span)) {
            // We are at the end of a header or a non-last deflate block, so we
            // can add an access point here. Furthermore, we are either at the
            // very start for the first access point, or there has been span or
            // more uncompressed bytes since the last access point, so we want
            // to add an access point here.
            index = add_point(index, totin - index->strm.avail_in, totout, beg,
                              win);
            if (index == NULL) {
                ret = Z_MEM_ERROR;
                break;
            }
            last = totout;
        }

        if (ret == Z_STREAM_END && mode == GZIP &&
            (index->strm.avail_in || ungetc(getc(in), in) != EOF)) {
            // There is more input after the end of a gzip member. Reset the
            // inflate state to read another gzip member. On success, this will
            // set ret to Z_OK to continue decompressing.
            ret = inflateReset2(&index->strm, GZIP);
            beg = totout;           // reset history
        }

        // Keep going until Z_STREAM_END or error. If the compressed data ends
        // prematurely without a file read error, Z_BUF_ERROR is returned.
    } while (ret == Z_OK);

    if (ret != Z_STREAM_END) {
        // An error was encountered. Discard the index and return a negative
        // error code.
        deflate_index_free(index);
        return ret == Z_NEED_DICT ? Z_DATA_ERROR : ret;
    }

    // Return the index.
    index->mode = mode;
    index->length = totout;
    *built = index;
    return index->have;
}

#define INFLATEPRIME inflatePrime

// See comments in zran.h.
ptrdiff_t deflate_index_extract(FILE *in, struct deflate_index *index,
                                off_t offset, unsigned char *buf, size_t len) {
    // Do a quick sanity check on the index.
    if (index == NULL || index->have < 1 || index->list[0].out != 0) {
        std::cout << "zran: index is not ready" << std::endl;
        return Z_STREAM_ERROR;
    }

    // If nothing to extract, return zero bytes extracted.
    if (len == 0 || offset < 0 || offset >= index->length) {
        std::cout << "zran: nothing to extract" << std::endl;
        return 0;
    }

    // Find the access point closest to but not after offset.
    int lo = -1, hi = index->have;
    point_t *point = index->list;
    while (hi - lo > 1) {
        int mid = (lo + hi) >> 1;
        if (offset < point[mid].out)
            hi = mid;
        else
            lo = mid;
    }
    point += lo;

    // Initialize the input file and prime the inflate engine to start there.
    int ret = fseeko(in, point->in - (point->bits ? 1 : 0), SEEK_SET);
    if (ret == -1) {
        std::cout << "zran: seek error" << std::endl;
        return Z_ERRNO;
    }
    int ch = 0;
    if (point->bits && (ch = getc(in)) == EOF)
        return ferror(in) ? Z_ERRNO : Z_BUF_ERROR;
    index->strm.avail_in = 0;
    ret = inflateReset2(&index->strm, RAW);
    if (ret != Z_OK) {
        std::cout << "zran: inflateReset2 error" << std::endl;
        return ret;
    }
    if (point->bits)
        INFLATEPRIME(&index->strm, point->bits, ch >> (8 - point->bits));
    inflateSetDictionary(&index->strm, point->window, point->dict);

    // Skip uncompressed bytes until offset reached, then satisfy request.
    unsigned char input[CHUNK];
    unsigned char discard[WINSIZE];
    offset -= point->out;       // number of bytes to skip to get to offset
    size_t left = len;          // number of bytes left to read after offset
    do {
        if (offset) {
            // Discard up to offset uncompressed bytes.
            index->strm.avail_out = offset < WINSIZE ? (unsigned)offset :
                                                       WINSIZE;
            index->strm.next_out = discard;
        }
        else {
            // Uncompress up to left bytes into buf.
            index->strm.avail_out = left < UINT_MAX ? (unsigned)left :
                                                      UINT_MAX;
            index->strm.next_out = buf + len - left;
        }

        // Uncompress, setting got to the number of bytes uncompressed.
        if (index->strm.avail_in == 0) {
            // Assure available input.
            index->strm.avail_in = fread(input, 1, CHUNK, in);
            if (index->strm.avail_in < CHUNK && ferror(in)) {
                ret = Z_ERRNO;
                break;
            }
            index->strm.next_in = input;
        }
        unsigned got = index->strm.avail_out;
        ret = inflate(&index->strm, Z_NO_FLUSH);
        got -= index->strm.avail_out;

        // Update the appropriate count.
        if (offset)
            offset -= got;
        else {
            left -= got;
            if (left == 0)
                // Request satisfied.
                break;
        }

        // If we're at the end of a gzip member and there's more to read,
        // continue to the next gzip member.
        if (ret == Z_STREAM_END && index->mode == GZIP) {
            // Discard the gzip trailer.
            unsigned drop = 8;              // length of gzip trailer
            if (index->strm.avail_in >= drop) {
                index->strm.avail_in -= drop;
                index->strm.next_in += drop;
            }
            else {
                // Read and discard the remainder of the gzip trailer.
                drop -= index->strm.avail_in;
                index->strm.avail_in = 0;
                do {
                    if (getc(in) == EOF) {
                        // The input does not have a complete trailer.
                        std::cout << "zran: unexpected EOF" << std::endl;
                        return ferror(in) ? Z_ERRNO : Z_BUF_ERROR;
                    }
                } while (--drop);
            }

            if (index->strm.avail_in || ungetc(getc(in), in) != EOF) {
                // There's more after the gzip trailer. Use inflate to skip the
                // gzip header and resume the raw inflate there.
                inflateReset2(&index->strm, GZIP);
                do {
                    if (index->strm.avail_in == 0) {
                        index->strm.avail_in = fread(input, 1, CHUNK, in);
                        if (index->strm.avail_in < CHUNK && ferror(in)) {
                            ret = Z_ERRNO;
                            break;
                        }
                        index->strm.next_in = input;
                    }
                    index->strm.avail_out = WINSIZE;
                    index->strm.next_out = discard;
                    ret = inflate(&index->strm, Z_BLOCK);  // stop after header
                } while (ret == Z_OK && (index->strm.data_type & 0x80) == 0);
                if (ret != Z_OK)
                    break;
                inflateReset2(&index->strm, RAW);
            }
        }

        // Continue until we have the requested data, the deflate data has
        // ended, or an error is encountered.
    } while (ret == Z_OK);

    // Return the number of uncompressed bytes read into buf, or the error.
    // return ret == Z_OK || ret == Z_STREAM_END ? len - left : ret;
    
    if (ret == Z_OK || ret == Z_STREAM_END) {
        return len - left;
    } else {
        std::cout << "zran: inflate error" << std::endl;
        return ret;
    }

}

#define SPAN 1048576L       // desired distance between access points
#define LEN 16384           // number of bytes to extract

// Demonstrate the use of deflate_index_build() and deflate_index_extract() by
// processing the file provided on the command line, and extracting LEN bytes
// from 2/3rds of the way through the uncompressed output, writing that to
// stdout. An offset can be provided as the second argument, in which case the
// data is extracted from there instead.
int main(int argc, char **argv) {
    // Open the input file.
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "usage: zran file.raw [offset]\n");
        return 1;
    }
    FILE *in = fopen(argv[1], "rb");
    if (in == NULL) {
        fprintf(stderr, "zran: could not open %s for reading\n", argv[1]);
        return 1;
    }

    // Get optional offset.
    off_t offset = -1;
    if (argc > 2) {
        char *end;
        offset = strtoll(argv[2], &end, 0);
        if (*end) {
            fprintf(stderr, "zran: invalid offset %s\n", argv[2]);
            fclose(in);
            return 1;
        }
    }
    fprintf(stderr, "zran: extracting %d bytes at offset %ld\n", LEN, offset);

    // Either build an index or read one from a file
    struct deflate_index *index = NULL;
    int len;

    // Read index from file if provided
    if (argc == 4) {
        fprintf(stderr, "zran: attempting to read index from %s\n", argv[3]);

        FILE *index_file = fopen(argv[3], "rb");
        if (index_file == NULL) {
            fprintf(stderr, "zran: could not open %s for reading\n", argv[3]);
            fclose(in);
            return 1;
        }

        len = deflate_index_load(index_file, &index);
        fclose(index_file);
    } else {
        // Build index.
        len = deflate_index_build(in, SPAN, &index);
    }

    if (len < 0) {
        fclose(in);
        switch (len) {
        case Z_MEM_ERROR:
            fprintf(stderr, "zran: out of memory\n");
            break;
        case Z_BUF_ERROR:
            fprintf(stderr, "zran: %s ended prematurely\n", argv[1]);
            break;
        case Z_DATA_ERROR:
            fprintf(stderr, "zran: compressed data error in %s\n", argv[1]);
            break;
        case Z_ERRNO:
            fprintf(stderr, "zran: read error on %s\n", argv[1]);
            break;
        default:
            fprintf(stderr, "zran: error %d while building index\n", len);
        }
        return 1;
    }
    
    if (argc == 4) {
        fprintf(stderr, "zran: read index with %d access points!\n", len);
        print_index(index);
    } else {
        fprintf(stderr, "zran: built index with %d access points!\n", len);
        print_index(index);

        // Save index to file
        char *filename = (char *) malloc(strlen(argv[1]) + 6);
        if (filename == NULL) {
            fprintf(stderr, "zran: out of memory\n");
            deflate_index_free(index);
            fclose(in);
            return 1;
        }
        strcpy(filename, argv[1]);
        strcat(filename, ".index");

        fprintf(stderr, "zran: attempting to write index to %s\n", filename);

        // Open the index file for writing.
        FILE *idx = fopen(filename, "wb");
        if (idx == NULL) {
            fprintf(stderr, "zran: could not open %s for writing\n", filename);
            deflate_index_free(index);
            fclose(in);
            return 1;
        }

        // Write the index to the file.
        len = deflate_index_save(idx, index);
        if (len != 0) {
            fclose(idx);
            fprintf(stderr, "zran: write error on %s\n", filename);
            deflate_index_free(index);
            fclose(in);
            return 1;
        }
        fprintf(stderr, "zran: wrote index with %d access points to %s\n", index->have, filename);

        // Clean up and exit
        fclose(idx);
        free(filename);
        deflate_index_free(index);
        fclose(in);
        return 0;
    }

    // Use index by reading some bytes from an arbitrary offset.
    unsigned char buf[LEN];
    if (offset == -1)
        offset = ((index->length + 1) << 1) / 3;
    ptrdiff_t got = deflate_index_extract(in, index, offset, buf, LEN);
    if (got < 0)
        fprintf(stderr, "zran: extraction failed: %s error\n",
                got == Z_MEM_ERROR ? "out of memory" : "input corrupted");
    else {
        fwrite(buf, 1, got, stdout);
        fprintf(stderr, "\nzran: extracted %ld bytes at %ld using index read from disk\n", got, offset);
    }

    // Clean up and exit.
    deflate_index_free(index);
    fclose(in);
    return 0;
}