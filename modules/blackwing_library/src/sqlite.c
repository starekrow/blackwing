/* sqlite, once, as C, with the options the library uses (kormake builds a module's .c files
 * with the toolchain's C compiler). The index is a cache in Blackwing's own data, written
 * by one connection on one thread and read by another on its own: multi-thread mode, WAL,
 * FTS5 for search. Nothing is loaded from outside it. */
#define SQLITE_THREADSAFE 2                /* a connection is used by one thread at a time */
#define SQLITE_DEFAULT_MEMSTATUS 0
#define SQLITE_DEFAULT_WAL_SYNCHRONOUS 1   /* NORMAL: a cache may lose its last commits to a power cut, never its consistency */
#define SQLITE_DQS 0                       /* "x" is a name, never a string */
#define SQLITE_ENABLE_FTS5 1
#define SQLITE_LIKE_DOESNT_MATCH_BLOBS 1
#define SQLITE_OMIT_DEPRECATED 1
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_OMIT_SHARED_CACHE 1
#define SQLITE_OMIT_AUTOINIT 1             /* sqlite3_initialize is called, once, at startup */
#include "vendor/sqlite3.c"
