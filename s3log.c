/*
 * file: s3log.c
 * description: structured logging for s3fs / s3mount
 */

#define _POSIX_C_SOURCE 200809L  /* for gettimeofday, strftime, etc. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

#include "s3log.h"

void s3log_timestamp(char *buf, size_t buflen)
{
    struct timeval tv;
    struct tm tm_info;
    gettimeofday(&tv, NULL);
    gmtime_r(&tv.tv_sec, &tm_info);
    strftime(buf, buflen, "%Y-%m-%dT%H:%M:%S", &tm_info);
    size_t len = strlen(buf);
    snprintf(buf + len, buflen - len, ".%03ldZ", (long)(tv.tv_usec / 1000));
}

long s3log_elapsed_ms(const struct timeval *start)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (long)((now.tv_sec  - start->tv_sec)  * 1000L +
                  (now.tv_usec - start->tv_usec) / 1000L);
}

/*
 * Write the [LOG_HEADER] block.
 */
static void write_header(FILE *fp, const struct s3log_config *cfg)
{
    char ts[32];
    char hostname[HOST_NAME_MAX + 1];
    s3log_timestamp(ts, sizeof(ts));
    if (gethostname(hostname, sizeof(hostname)) != 0)
        strncpy(hostname, "unknown", sizeof(hostname));
    fprintf(fp,
            "[LOG_HEADER]\n"
            "timestamp=%s\n"
            "pid=%d\n"
            "hostname=%s\n",
            ts, (int)getpid(), hostname);
    if (cfg->bucket)
        fprintf(fp, "bucket=%s\n", cfg->bucket);
    if (cfg->object_key)
        fprintf(fp, "object_key=%s\n", cfg->object_key);
    if (cfg->mount_point)
        fprintf(fp, "mount_point=%s\n", cfg->mount_point);
    fprintf(fp,
            "local_mode=%d\n"
            "use_http=%d\n"
            "cache_enabled=%d\n",
            cfg->local_mode,
            cfg->use_http,
            !cfg->nocache);
    if (!cfg->nocache) {
        fprintf(fp,
                "cache_blocks=%d\n"
                "cache_block_size_mb=%d\n",
                cfg->cache_size_blocks,
                cfg->cache_block_size_mb);
    }
    fprintf(fp, "nocache_mode=%d\n", cfg->nocache);
    if (cfg->s3_host)
        fprintf(fp, "s3_host=%s\n", cfg->s3_host);
    fprintf(fp, "[END_HEADER]\n\n");
    fprintf(fp,
            "TIMESTAMP\t"
            "OP_TYPE\t"
            "REQ_ID\t"
            "PATH\t"
            "VERSION\t"
            "OFFSET\t"
            "LENGTH\t"
            "ACTUAL\t"
            "DURATION_MS\t"
            "RESULT\t"
            "CACHE_STATUS\t"
            "ERROR\n");
    fflush(fp);
}

/*
* Opens the log file.
*/
int log_open(struct s3log_state *ls, const struct s3log_config *cfg)
{
    memset(ls, 0, sizeof(*ls));
    pthread_mutex_init(&ls->lock, NULL);
    if (!cfg->logfile || cfg->logfile[0] == '\0')
        return 0; 
    ls->fp = fopen(cfg->logfile, "w");
    if (!ls->fp) {
        perror("s3log: could not open log file");
        return -1;
    }
    gettimeofday(&ls->mount_time, NULL);
    write_header(ls->fp, cfg);
    return 0;
}

/*
 * Writes a log entry for an operation.
 */
void log_operation(struct s3log_state *ls,
                   const char *op_type,
                   const char *path,
                   int version,
                   off_t offset,
                   size_t len,
                   size_t actual,
                   long duration_ms,
                   const char *result,
                   const char *cache_status,
                   const char *error)
{
    char ts[32];
    s3log_timestamp(ts, sizeof(ts));
    pthread_mutex_lock(&ls->lock);
    if (!ls || !ls->fp)
        return;
    ls->n_ops++;
    if (strcmp(op_type, S3LOG_OP_READ) == 0) {
        ls->n_reads++;
        if (actual != S3LOG_NO_SIZE)
            ls->bytes_read += actual;
    } else if (strcmp(op_type, S3LOG_OP_GETATTR) == 0) {
        ls->n_getattr++;
    } else if (strcmp(op_type, S3LOG_OP_READDIR) == 0) {
        ls->n_readdir++;
    } else if (strcmp(op_type, S3LOG_OP_READLINK) == 0) {
        ls->n_readlink++;
    }
    if (result && strcmp(result, S3LOG_OK) != 0)
        ls->n_errors++;
    if (cache_status) {
        if (strcmp(cache_status, S3LOG_CACHE_HIT) == 0) {
            ls->cache_hits++;
            if (actual != S3LOG_NO_SIZE)
                ls->bytes_cached += actual;
        } else if (strcmp(cache_status, S3LOG_CACHE_MISS) == 0) {
            ls->cache_misses++;
        }
    }
    if (error && strstr(error, "retry"))
        ls->s3_retries++;
    ls->sum_duration_ms += (double)duration_ms;
    if (duration_ms > ls->max_duration_ms)
        ls->max_duration_ms = duration_ms;
    fprintf(ls->fp, "%s\t", ts);
    fprintf(ls->fp, "%s\t", op_type ? op_type : "-");
    fprintf(ls->fp, "0x%06" PRIx64 "\t", ls->req_id++);
    fprintf(ls->fp, "%s\t", (path && path[0]) ? path : "-");
    if (version == S3LOG_NO_VERSION)
        fprintf(ls->fp, "-\t");
    else
        fprintf(ls->fp, "%d\t", version);
    if (offset == S3LOG_NO_OFFSET)
        fprintf(ls->fp, "-\t");
    else
        fprintf(ls->fp, "%lld\t", (long long)offset);
    if (len == S3LOG_NO_SIZE)
        fprintf(ls->fp, "-\t");
    else
        fprintf(ls->fp, "%zu\t", len);
    if (actual == S3LOG_NO_SIZE)
        fprintf(ls->fp, "-\t");
    else
        fprintf(ls->fp, "%zu\t", actual);
    fprintf(ls->fp, "%ld\t", duration_ms);
    fprintf(ls->fp, "%s\t", result ? result : "-");
    fprintf(ls->fp, "%s\t", cache_status ? cache_status : S3LOG_CACHE_NA);
    fprintf(ls->fp, "%s\n", (error && error[0]) ? error : "");
    fflush(ls->fp);
    pthread_mutex_unlock(&ls->lock);
}

/*
* Writes the statistics footer.
*/
static void write_statistics(FILE *fp, const struct s3log_state *ls)
{
    long duration_sec = s3log_elapsed_ms(&ls->mount_time) / 1000L;
    uint64_t total_cache = ls->cache_hits + ls->cache_misses;
    int hit_pct = (total_cache > 0)
                  ? (int)((ls->cache_hits * 100) / total_cache)
                  : 0;
    double avg_ms = (ls->n_ops > 0)
                    ? ls->sum_duration_ms / (double)ls->n_ops
                    : 0.0;
    fprintf(fp,
            "\n[STATISTICS]\n"
            "mount_duration_seconds=%ld\n"
            "total_operations=%" PRIu64 "\n"
            "total_reads=%" PRIu64 "\n"
            "total_getattr=%" PRIu64 "\n"
            "total_readdir=%" PRIu64 "\n"
            "total_readlink=%" PRIu64 "\n"
            "total_errors=%" PRIu64 "\n"
            "bytes_read=%" PRIu64 "\n"
            "bytes_cached=%" PRIu64 "\n"
            "cache_hits=%" PRIu64 "\n"
            "cache_misses=%" PRIu64 "\n"
            "cache_hit_rate=%d%%\n"
            "avg_read_duration_ms=%.1f\n"
            "max_read_duration_ms=%ld\n"
            "s3_retries_total=%" PRIu64 "\n"
            "[END_STATISTICS]\n",
            duration_sec,
            ls->n_ops,
            ls->n_reads,
            ls->n_getattr,
            ls->n_readdir,
            ls->n_readlink,
            ls->n_errors,
            ls->bytes_read,
            ls->bytes_cached,
            ls->cache_hits,
            ls->cache_misses,
            hit_pct,
            avg_ms,
            ls->max_duration_ms,
            ls->s3_retries);
}

void log_close(struct s3log_state *ls)
{
    if (!ls || !ls->fp)
        return;
    write_statistics(ls->fp, ls);
    fclose(ls->fp);
    ls->fp = NULL;
    pthread_mutex_destroy(&ls->lock);
}
