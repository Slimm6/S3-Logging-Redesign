/*
 * file:        s3log.h
 * description: structured logging for s3fs / s3mount
 *
 *   - Header written once at mount
 *   - one line per operation
 *   - Statistics footer written at unmount
 */
#ifndef __S3LOG_H__
#define __S3LOG_H__

#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>
#include <pthread.h>


#define S3LOG_OP_INIT     "INIT"
#define S3LOG_OP_READ     "READ"
#define S3LOG_OP_READLINK "READLINK"
#define S3LOG_OP_GETATTR  "GETATTR"
#define S3LOG_OP_READDIR  "READDIR"
#define S3LOG_OK      "OK"
#define S3LOG_ERROR   "ERROR"
#define S3LOG_TIMEOUT "TIMEOUT"
#define S3LOG_CACHE_HIT     "HIT"
#define S3LOG_CACHE_MISS    "MISS"
#define S3LOG_CACHE_PARTIAL "PARTIAL"
#define S3LOG_CACHE_NA      "N/A"
#define S3LOG_NO_VERSION  (-1)
#define S3LOG_NO_OFFSET   ((off_t)-1)
#define S3LOG_NO_SIZE     ((size_t)-1)

pthread_mutex_t lock;

/* 
 * log state of the current mount, including the log file pointer and cumulative statistics.
 */
struct s3log_state {
    FILE           *fp;             /* NULL when logging is disabled    */
    uint64_t        req_id;
    struct timeval  mount_time;
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
    double          sum_duration_ms;
    uint64_t        s3_retries;
};

/*
/* The configuration of the logfile
*/
struct s3log_config {
    const char *logfile;
    const char *bucket;
    const char *object_key;
    const char *mount_point;
    const char *s3_host;
    int local_mode;
    int use_http;
    int nocache;
    int cache_size_blocks; 
    int cache_block_size_mb;
};


/*
 * log_open - open the log file and write the [LOG_HEADER] block.
 */
int log_open(struct s3log_state *ls, const struct s3log_config *cfg);

/*
 * log_operation - append one tab-separated row to the log.
 * This function does nothing if logging is disabled (ls->fp is NULL).
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
 */
void log_close(struct s3log_state *ls);

/*
 * s3log_timestamp - write an ISO-8601 timestamp with millisecond
 */
void s3log_timestamp(char *buf, size_t buflen);

/*
 * s3log_elapsed_ms - return milliseconds elapsed since *start.
 */
long s3log_elapsed_ms(const struct timeval *start);

#endif /* __S3LOG_H__ */
