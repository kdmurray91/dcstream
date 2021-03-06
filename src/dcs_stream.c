#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>

#include "dcs_util.h"
#include "dcs_compr.h"
#include "dcs_stream.h"


/*******************************************************************************
*                             Helper declarations                             *
*******************************************************************************/

int dcs_fileext(const char *filename, char *extbuf, size_t extbuflen);
dcs_comp_algo dcs_guess_compression_type(const char *filename);


/*******************************************************************************
*                              Stream Open/Close                              *
*******************************************************************************/

static inline int
_dcs_fillbuf(dcs_stream *stream)
{
    if (stream == NULL || !stream->read) return -1;
    int res = dcs_compr_read(stream->compr, stream->buf, &stream->len, stream->cap);
    if (stream->len < stream->cap) {
        stream->fp_eof = true;
    }
    stream->pos = 0;
    stream->prevous_getc = -1; // ungetc now impossible
    return res;
}

static inline int
_dcs_writebuf(dcs_stream *stream)
{
    if (stream == NULL) return -1;
    int res = dcs_compr_write(stream->compr, stream->buf, stream->len);
    stream->len = 0;
    stream->pos = 0;
    return res;
}

static inline dcs_stream *
_dcs_init(const char *mode)
{
    bool read = true;
    if (mode == NULL) return NULL;
    if (mode[0] == 'r') read = true;
    else if (mode[0] == 'w') read = false;
    else return NULL;

    dcs_stream *stream = malloc(sizeof(*stream));
    if (stream == NULL) return NULL;

    stream->buf = calloc(1, DCS_BUFSIZE);
    if (stream->buf == NULL) {
        dcs_free(stream);
        return NULL;
    }
    stream->len = 0;
    stream->pos = 0;
    stream->cap = DCS_BUFSIZE;
    stream->prevous_getc = -1;
    stream->fp_eof = false;
    stream->read = read;
    return stream;
}


dcs_stream *
dcs_open(const char *file, const char *mode, dcs_comp_algo algo)
{
    if (file == NULL || mode == NULL) return NULL;

    // Guess compression type. If still unknown, bail.
    if (algo == DCS_UNKNOWN) algo = dcs_guess_compression_type(file);
    if (algo == DCS_UNKNOWN) return NULL;

    dcs_stream *stream = _dcs_init(mode);

    dcs_compr *compr = dcs_compr_open(file, mode, algo);
    if (compr == NULL) {
        dcs_free(stream->buf);
        dcs_free(stream);
        return NULL;
    }
    stream->compr = compr;
    return stream;
}


dcs_stream *
dcs_dopen(int fd, const char *mode, dcs_comp_algo algo)
{
    if (mode == NULL || algo == DCS_UNKNOWN) return NULL;

    dcs_stream *stream = _dcs_init(mode);

    dcs_compr *compr = dcs_compr_dopen(fd, mode, algo);
    if (compr == NULL) {
        dcs_free(stream->buf);
        dcs_free(stream);
        return NULL;
    }
    stream->compr = compr;

    return stream;
}

/* Close @dcs, destroying all data strucutres
 */
int
_dcs_close(dcs_stream *stream)
{
    if (stream == NULL) return -1;

    if (!stream->read && stream->pos > 0) {
        _dcs_writebuf(stream);
    }
    int res = dcs_compr_close(stream->compr);

    dcs_free(stream->buf);
    dcs_free(stream);
    return res;
}

int
dcs_setbufsize(dcs_stream *stream, size_t size)
{
    if (stream == NULL || size == 0) return -1;

    // If buffer has been filled, exit
    if (stream->pos != 0 || stream->len != 0 || stream->fp_eof) return -1;

    unsigned char *newbuf = calloc(1, size);
    if (newbuf == NULL) return -1;

    dcs_free(stream->buf);
    stream->buf = newbuf;
    stream->cap = size;

    return 0;
}

int
dcs_flush(dcs_stream *stream)
{
    if (stream == NULL || stream->read) return -1;

    if (stream->pos == 0) return 0;

    int res = dcs_compr_write(stream->compr, stream->buf, stream->len);
    res |= dcs_compr_flush(stream->compr);
    stream->len = 0;
    stream->pos = 0;
    return res;
}


/*******************************************************************************
*                               Read and Write                                *
*******************************************************************************/

// returns <0 on error, 0 on eof, 1 if more data to process
static inline int
dcs_moredata(dcs_stream *stream)
{
    if (stream == NULL || ! stream->read) return -1;
    if (!stream->fp_eof && stream->pos == stream->len) {
        if (_dcs_fillbuf(stream) != 0) return -1;
    }
    return dcs_eof(stream) ? 0 : 1;
}

ssize_t
dcs_read(dcs_stream *stream, void *dest, size_t size)
{
    if (stream == NULL || dest == NULL || ! stream->read) return -1;

    size_t read = 0;
    unsigned char *bdest = dest;
    while (read < size && dcs_moredata(stream)) {
        size_t tocpy = dcs_size_min(stream->len - stream->pos, size - read);
        memcpy(bdest + read, stream->buf + stream->pos, tocpy);
        stream->pos += tocpy;
        read += tocpy;
    }
    stream->prevous_getc = -1;
    return read;
}

ssize_t
dcs_write(dcs_stream *stream, const void *src, size_t size)
{
    if (stream == NULL || src == NULL || stream->read) return -1;

    size_t wrote = 0;
    int res = 0;
    const unsigned char *bsrc = src;
    while (wrote < size) {
        size_t tocpy = dcs_size_min(stream->cap - stream->pos, size - wrote);
        memcpy(stream->buf + stream->pos, bsrc + wrote, tocpy);
        stream->pos += tocpy;
        stream->len = stream->pos;
        wrote += tocpy;
        if (stream->pos == stream->cap) {
            res = _dcs_writebuf(stream);
            if (res != 0) {
                return -1;
            }
        }
    }
    return wrote;
}


int
dcs_getc(dcs_stream *stream)
{
    if (stream == NULL || !stream->read) return -1;
    if (dcs_moredata(stream) != 1) return -1;

    int chr = stream->buf[stream->pos++];
    stream->prevous_getc = chr;
    return chr;
}

int
dcs_ungetc(dcs_stream *stream)
{
    if (stream == NULL || !stream->read) return -1;
    if (stream->prevous_getc < 0) return -1;
    if (stream->buf[stream->pos - 1] == stream->prevous_getc) {
        stream->pos--;
        stream->prevous_getc = -1;
        return 0;
    }
    return -1;
}

ssize_t
dcs_getuntil(dcs_stream *stream, char **dest, size_t *size, char delim)
{
    if (stream == NULL || !stream->read || dest == NULL || size == NULL) return -1;

    char *out = *dest;
    size_t outsize = *size;
    size_t outpos = 0;
    bool found = false;
    while (!found && dcs_moredata(stream)) {
        // Find delimiter, or end of buffer
        const char *buffer_start = (char *)stream->buf + stream->pos;
        const size_t buffer_len = stream->len - stream->pos;
        const char *delim_pos = memchr(buffer_start, delim, buffer_len);
        size_t bytes_to_delim = 0;

        if (delim_pos != NULL) {
            bytes_to_delim = delim_pos - buffer_start + 1;
            found = true;
        } else {
            bytes_to_delim = buffer_len;
        }

        // Resize buffer if required
        while (out == NULL || bytes_to_delim > outsize - outpos) {
            if (outsize < 64) outsize = 64;
            else outsize <<= 1;

            out = realloc(out, outsize);
            if (out == NULL) return -1;

            *dest = out;
            *size = outsize;
        }

        memcpy(out + outpos, buffer_start, bytes_to_delim);
        stream->pos += bytes_to_delim;
        outpos += bytes_to_delim;
        out[outpos] = '\0';
    }
    *dest = out;
    *size = outsize;
    return outpos;
}


/*******************************************************************************
*                                   Helpers                                   *
*******************************************************************************/

int
dcs_fileext(const char *filename, char *extbuf, size_t extbuflen)
{
    if (filename == NULL || extbuf == NULL || extbuflen == 0) return -1;

    char *lastdot = strrchr(filename, '.');
    if (lastdot == NULL) {
        // No extension
        strncpy(extbuf, "", extbuflen);
    } else {
        strncpy(extbuf, lastdot, extbuflen);
    }
    return 0;
}

dcs_comp_algo
dcs_guess_compression_type(const char *filename)
{
    int res = 0;

    // If file is stdin, we only support plain file IO
    if (strcmp(filename, "-") == 0 || strcmp(filename, "/dev/stdin") == 0) {
        return DCS_PLAIN;
    }

    // Stat file
    struct stat statres;
    res = stat(filename, &statres);
    if (res == 0) {
        // What, someone gave us a directory?
        if (S_ISDIR(statres.st_mode)) {
            return DCS_UNKNOWN;
        }

        // If file is a stream or socket, we only support plain IO
        if (S_ISFIFO(statres.st_mode) || S_ISSOCK(statres.st_mode)) {
            return DCS_PLAIN;
        }
    }

    // Get the file extension
    char extbuf[4096] = "";
    res = dcs_fileext(filename, extbuf, 4096);
    if (res != 0) return DCS_UNKNOWN;

    // Guess the file type from the extension. Yes, I'm that lazy right now.
    if (strcmp(extbuf, ".gz") == 0) {
        return DCS_GZIP;
    } else if (strcmp(extbuf, ".bz2") == 0) {
        return DCS_BZIP2;
    } else if (strcmp(extbuf, ".zst") == 0) {
        return DCS_ZSTD;
    } else {
        return DCS_PLAIN;
    }
}
