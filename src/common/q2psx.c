#include "q2psx.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static q2_log_level g_log_level = Q2_LOG_INFO;

const char *q2_result_str(q2_result r)
{
    switch (r) {
    case Q2_OK:              return "ok";
    case Q2_ERR_IO:          return "I/O error";
    case Q2_ERR_NOT_FOUND:   return "not found";
    case Q2_ERR_BAD_FORMAT:  return "bad format";
    case Q2_ERR_UNSUPPORTED: return "unsupported";
    case Q2_ERR_NO_MEMORY:   return "out of memory";
    case Q2_ERR_RANGE:       return "out of range";
    case Q2_ERR_INVALID_ARG: return "invalid argument";
    }
    return "unknown error";
}

void q2_log_set_level(q2_log_level level) { g_log_level = level; }
q2_log_level q2_log_get_level(void)       { return g_log_level; }

void q2_log(q2_log_level level, const char *fmt, ...)
{
    static const char *const tag[] = { "error", "warn ", "info ", "debug", "trace" };
    va_list ap;

    if (level > g_log_level)
        return;

    fprintf(stderr, "[%s] ", tag[level]);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

q2_result q2_buf_alloc(q2_buf *buf, size_t size)
{
    if (!buf)
        return Q2_ERR_INVALID_ARG;

    buf->data = NULL;
    buf->size = 0;

    if (size == 0)
        return Q2_OK;

    /* One extra zero byte so string-ish payloads are always safely terminated. */
    buf->data = (u8 *)calloc(1, size + 1);
    if (!buf->data)
        return Q2_ERR_NO_MEMORY;

    buf->size = size;
    return Q2_OK;
}

void q2_buf_free(q2_buf *buf)
{
    if (!buf)
        return;
    free(buf->data);
    buf->data = NULL;
    buf->size = 0;
}

const u8 *q2_buf_at(const q2_buf *buf, size_t offset, size_t need)
{
    if (!buf || !buf->data)
        return NULL;
    if (offset > buf->size || need > buf->size - offset)
        return NULL;
    return buf->data + offset;
}

void q2_str_copy(char *dst, size_t cap, const char *src)
{
    size_t i;

    if (!dst || !cap)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    /* Stops at cap - 1 whether or not a terminator turns up first, so a
     * fixed-width field with no NUL is read no further than its width. */
    for (i = 0; i + 1 < cap && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}
