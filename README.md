# S3-Backup Logging System Redesign

**s3-backup** performs a full or incremental backup of a directory hierarchy to a single S3 object; **s3mount** mounts a backup as a read-only FUSE file system.

***NOTE***

All information about the Logging System can be found in at the end of the README. The makefile has been adjusted to include everything necessary to use the log. The logging system expands upon the log, making it so it is no longer just the header of a file that gets printed on read.

## Usage

environment variables: S3\_HOSTNAME, S3\_ACCESS\_KEY\_ID, S3\_SECRET\_ACCESS\_KEY 
(also available as command line options)

s3-backup --bucket BUCKET [--incremental OBJECT] \[--max #\] OBJECT /path

s3mount -o bucket=BUCKET[,http] OBJECT /mountpoint

## Description

S3-backup stores a snapshot of a file system as a single S3 object, using a simplified log-structured file system.
It supports incremental backups by chaining a sequence of these objects.
Although it does not support point-in-time snapshots, the incremental model allows creation of a *fuzzy snapshot* by following a full backup with an incremental one - inconsistencies will be bounded by the time it takes to traverse the local file system during the incremental backup.
It is coupled with a FUSE client that aggressively caches data and directories, flags files as cacheable to the kernel, stores symbolic links, and preserves owners, timestamps, and permissions.

Since it was created as a backup tool, it does not traverse mount points, but instead behaves like the `-xdev` flag to `find` - empty directories are stored for any encountered mount points.

Additional features:

- S3 hostname, access key and secret key can be provided by parameters as well as environment variables
- the `local` option to each program forces object names to be interpreted as local file paths, mostly for debugging purposes
- the `--max #` indicates that backup should stop after a certain amount of data (in bytes, M and G prefixes allowed) is written, allowing large full backups to be created as a chain of somewhat smaller ones. (files will not be broken across backups, so the limit is a soft one)
- `--exclude` lets you exclude directories or files matching a pattern, e.g excluding `.ssh` so you don't archive anyone's SSH keys
- the `log=logfile.log` implementation has been revamped, giving access to the mounts specifics, debugging information, statistics of the your activity upon unmount. More information can be found in the log section of the Design / Implementation

## Requirements

To build it on Ubuntu you'll need the following (I think - I haven't tested on a clean system):

- libs3 - a simple C library for S3. You'll need to grab it from https://github.com/bji/libs3 and build it. (make; make install will put it in /usr/local)
**warning:** you need to apply the included patch to libs3 to make it 64-bit safe, at least until they accept my bug report.
- libcurl, libxml2, libssl-dev - for building libs3
- libavl-dev, libuuid-dev

[edit - Makefile now checks for requirements, also the `libs3` target fetches, patches and builds libs3]
On Alpine you'll need argp-standalone as well. (I use Alpine and alpinewall for gateway machines on a couple of my networks, so it was my first use case)

## Design / implementation

S3 is an inode-less file system, vaguely like the original CD-ROM format but with 512-byte sectors, user IDs, and long file names. Describing it from the inside out:

### Offsets
Objects are divided into 512-byte sectors, and sectors are addressed by an 8-byte address:

| object# : 16 | sector offset : 48 |
|------|----------|

### Directory entries

Variable-sized directory entries look like this:

| mode : 16 | uid : 16 | gid : 16 | ctime : 32 | offset : 64 | size : 64 | namelen : 8 | name |
|---------|-------|-------|---------|---------|--------|----|---|

Note that names are not null-terminated; `printf("%.*s", de->namelen, de->name)` is your friend. There's a helper function `next_de(struct s3_dirent*)` to iterate through a directory.

### Object header

All fields (except versions) are 4 bytes:
|magic | version | flags | len | nversions | <versions> |
|----|----|----|----|----|----|

`len` is the length of the header in sectors. (although technically I'm not sure it's needed)

Versions are ordered newest (this object) to oldest, and look like this:

|   uuid | namelen : 16 | name |
|----|----|-----|

again with no null termination, and a iterator function `next_version`. For constructing offsets they're numbered in reverse, with the oldest version numbered 0.

### Object trailer

The last sector of the object combines a directory (starting at offset 0) and file system statistics (at the end of the sector)

- first entry: this points to the root directory
- second entry: hidden file, directory offsets
- third entry: packed directory contents

The directory offsets table has entries of the form:

|    offset : 64 | nbytes : 32 |
|----|----|

If you add then up, you can figure out the byte offset of each directory copy in the packed directory contents. This lets us load all the directories into memory at the beginning so we don't have to go back to S3 for directory lookups. Among other things, this makes incrementals *way* faster.

## Logging Header

The logging is broken up into 3 parts that are implemented within the s3log.c file. The log header, the action log, and the log footer.
The log header is formatted as the following:

[LOG_HEADER]
- timestamp
- pid
- hostname
- bucket
- object_key
- mount_point
- local_mode
- use_http
- cache_enabled
- cache_size_mb
- cache_block_size_mb
- nocache_mode
- mount_user
- s3_host

[END_HEADER]

Here you can see all the necessary informatuon about the mount for future use. What mode you are in or if you are using http. All the information needed about the cache is also included in the log header. 

## Logging Operations

The log itself has been expanded significantly from the previous iteration. This can be found below:

*TIMESTAMP - OP_TYPE - REQ_ID - PATH - VERSION - OFFSET - LENGTH - ACTUAL - DURATION_MS - RESULT - CACHE_STATUS - ERROR*

This is the core of the redesign, expanding upon the filename, offset, and length which were included in the oiginal rendition. The brand new editions to this are the following:
- Timestamp
With the addition of timestamps, you should be able to better track
- Operation Type
In the original log, you only knew that you read the file. Here, I expanded it to make sure that you can get a better overview of how the system behaves. This includes *INIT, READDIR, GETATTR, READLINK*
*INIT:* The init type showcases the first step after you go into the init of the mount.
*READDIR:* When you read a directory
*GETATTR:* When you get file attributes
*READLINK:* When you read through a symlink.
- Request ID
A hex number that showcases the order of when a request was made.
- Path
Self-explanatory, this is the file path and name.
- Version
Keeps track of which backup version the file is.
- Offset
Byte offset in file, like in the original log implementation
-Length
Bytes requested, like in the original log implementation
- Actual
Bytes successfully read
- Duration
How long the request took to perform.
- Result
The result of the request, did it timeout, error, or succeed?
- Cache
How did the request interact with the cache, was it a hit, a miss, or a mix of the two?
- Error
If there was an error, what error occurred within the system? 

This allows for much more informative logging and understanding of how the system is currently functioning. 

## Logging Footer

The final part of the implementation was the creation of the footer, this gives statistics of the most recent mount and information that may have been missed in more extensive testing. 
The log footer is formatted as the following:

[STATISTICS]
- mount_duration_seconds
- total_operations
- total_reads
- total_getattr
- total_readdir
- total_readlink
- total_errors
- bytes_read
- bytes_cached
- cache_hits
- cache_misses
- cache_hit_rate
- avg_read_duration_ms
- max_read_duration_ms
- s3_retries_total
- s3_retry_max_consecutive
- most_accessed_version
- least_accessed_version

[END_STATISTICS]

This catches all the information you would need to know about the most recent mount. Including operations that were performed and how many errored, how many bytes were read and cached alongside the status of the cache. The duration of reads (average and worst case), the number of retires and statistics about the version of the files. It gives an overview of the run in a condensed way.




