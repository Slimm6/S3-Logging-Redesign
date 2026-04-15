/*
 * file:        s3log.h
 * description: structured logging for s3fs / s3mount
 *
 * Log format follows LOG_FORMAT_PROPOSAL.md:
 *   - [LOG_HEADER] block written once at mount
 *   - one tab-separated line per operation
 *   - [STATISTICS] footer written at unmount
 */
#ifndef __S3LOG_H__
#define __S3LOG_H__

#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>

/* ------------------------------------------------------------------ */
/* Operation type strings (passed to log_operation as op_type)         */
/* ------------------------------------------------------------------ */
#define S3LOG_OP_INIT     "INIT"
#define S3LOG_OP_READ     "READ"
#define S3LOG_OP_READLINK "READLINK"
#define S3LOG_OP_GETATTR  "GETATTR"
#define S3LOG_OP_READDIR  "READDIR"
#define S3LOG_OP_SEEK     "SEEK"

/* Result strings */
#define S3LOG_OK      "OK"
#define S3LOG_PARTIAL "PARTIAL"
#define S3LOG_ERROR   "ERROR"
#define S3LOG_TIMEOUT "TIMEOUT"

/* Cache status strings */
#define S3LOG_CACHE_HIT     "HIT"
#define S3LOG_CACHE_MISS    "MISS"
#define S3LOG_CACHE_PARTIAL "PARTIAL"
#define S3LOG_CACHE_NA      "N/A"

/* Sentinel values for fields that don't apply to an operation */
#define S3LOG_NO_VERSION  (-1)
#define S3LOG_NO_OFFSET   ((off_t)-1)
#define S3LOG_NO_SIZE     ((size_t)-1)

/* ------------------------------------------------------------------ */
/* Per-mount logging state                                             */
/* Embed this inside struct state in s3mount.c                        */
/* ------------------------------------------------------------------ */
struct s3log_state {
    FILE           *fp;             /* NULL when logging is disabled    */
    uint64_t        req_id;         /* monotonically increasing counter */
    struct timeval  mount_time;     /* wall time at fs_init             */

    /* cumulative counters – updated by log_operation()                */
    uint64_t        n_ops;
    uint64_t        n_reads;
    uint64_t        n_getattr;
    uint64_t        n_readdir;
    uint64_t        n_readlink;
    uint64_t        n_errors;
    uint64_t        bytes_read;
    uint64_t        bytes_cached;
    uint64_t        cache_hits;
    uint64_t        cache_misses;
    long            max_duration_ms;
    double          sum_duration_ms; /* for average */
    uint64_t        s3_retries;
};

/* ------------------------------------------------------------------ */
/* Configuration passed to log_open()                                  */
/* ------------------------------------------------------------------ */
struct s3log_config {
    const char *logfile;        /* path to log file; NULL → no logging  */
    const char *bucket;
    const char *object_key;
    const char *mount_point;
    const char *s3_host;        /* may be NULL                          */
    int         local_mode;
    int         use_http;
    int         nocache;
    int         cache_size_blocks;      /* CACHE_SIZE constant          */
    int         cache_block_size_mb;    /* CACHE_BLOCK in MiB           */
};

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * log_open - open the log file and write the [LOG_HEADER] block.
 * Call from fs_init() after all config fields are known.
 * Returns 0 on success, -1 if the file could not be opened (logging
 * is then silently disabled – ls->fp is left NULL).
 */
int log_open(struct s3log_state *ls, const struct s3log_config *cfg);

/*
 * log_operation - append one tab-separated row to the log.
 *
 *   op_type      - one of the S3LOG_OP_* strings (or any short literal)
 *   path         - filesystem path visible to the user, or NULL / ""
 *   version      - backup version index; S3LOG_NO_VERSION if not applicable
 *   offset       - byte offset; S3LOG_NO_OFFSET if not applicable
 *   len          - bytes requested; S3LOG_NO_SIZE if not applicable
 *   actual       - bytes actually read/returned; S3LOG_NO_SIZE if N/A
 *   duration_ms  - elapsed time in milliseconds
 *   result       - S3LOG_OK / S3LOG_PARTIAL / S3LOG_ERROR / S3LOG_TIMEOUT
 *   cache_status - S3LOG_CACHE_* or S3LOG_CACHE_NA
 *   error        - error detail string, or NULL / "" if result is OK
 *
 * This function is a no-op when ls->fp is NULL.
 */
void log_operation(struct s3log_state *ls,
                   const char *op_type,
                   const char *path,
                   int         version,
                   off_t       offset,
                   size_t      len,
                   size_t      actual,
                   long        duration_ms,
                   const char *result,
                   const char *cache_status,
                   const char *error);

/*
 * log_close - write the [STATISTICS] footer and close the log file.
 * Call from the FUSE destroy callback or atexit handler.
 * Safe to call even if log_open() was never called or failed.
 */
void log_close(struct s3log_state *ls);

/* ------------------------------------------------------------------ */
/* Utility helpers (also available to callers)                         */
/* ------------------------------------------------------------------ */

/*
 * s3log_timestamp - write an ISO-8601 timestamp with millisecond
 * resolution into buf (must be at least 32 bytes).
 */
void s3log_timestamp(char *buf, size_t buflen);

/*
 * s3log_elapsed_ms - return milliseconds elapsed since *start.
 */
long s3log_elapsed_ms(const struct timeval *start);

#endif /* __S3LOG_H__ */
