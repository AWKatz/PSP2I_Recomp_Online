# SQLite (vendored)

These are `sqlite3.c` and `sqlite3.h` from the official SQLite amalgamation, copied unchanged.

- Version: 3.54.1
- Source: https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
- SHA3-256 of the zip, checked against sqlite.org: `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`
- License: public domain.

The build flags are set in `server/CMakeLists.txt`:
- JSON support is on (the `-DSQLITE_ENABLE_JSON1` flag is kept for clarity; JSON has been built in since 3.38).
- Extension loading is off, so the server makes no `dlopen` calls.
- Foreign keys are on by default.
- Double-quoted strings are not accepted as string literals.
- Thread-safe mode.
