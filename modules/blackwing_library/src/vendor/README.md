# Vendored

`sqlite3.c`, `sqlite3.h`: sqlite 3.54.1, the amalgamation, public domain, from
https://sqlite.org/2026/sqlite-amalgamation-3540100.zip (SHA3-256 of the zip
92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30, as sqlite.org lists it),
unmodified. `shell.c` and `sqlite3ext.h` are left out.

It is C and not C++, so it is compiled as C: `src/sqlite.c` sets the options and includes
it, and kormake builds a module's `.c` files with the toolchain's C compiler. The options:
multi-thread mode (each connection on one thread at a time), WAL with `synchronous=NORMAL`,
FTS5, no double-quoted strings, no extension loading, no shared cache, no deprecated
interfaces, and no automatic initialisation (the module calls `sqlite3_initialize`).

Upgrading is replacing the two files and the version and hash above, then running the
module's tests (`kormake test blackwing_library`).
