/*
** This header file defines the interface that the minidatabase library
** presents to client programs.
*/
#ifndef _MNDB_H_
#define _MNDN_H_
/*
** Return values for sqlite_exec() and sqlite_step()
*/
#define MNDB_OK           0   /* Successful result */
#define MNDB_ERROR        1   /* SQL error or missing database */
#define MNDB_INTERNAL     2   /* An internal logic error in SQLite */
#define MNDB_PERM         3   /* Access permission denied */
#define MNDB_ABORT        4   /* Callback routine requested an abort */
#define MNDB_BUSY         5   /* The database file is locked */
#define MNDB_LOCKED       6   /* A table in the database is locked */
#define MNDB_NOMEM        7   /* A malloc() failed */
#define MNDB_READONLY     8   /* Attempt to write a readonly database */
#define MNDB_INTERRUPT    9   /* Operation terminated by sqlite_interrupt() */
#define MNDB_IOERR       10   /* Some kind of disk I/O error occurred */
#define MNDB_CORRUPT     11   /* The database disk image is malformed */
#define MNDB_NOTFOUND    12   /* (Internal Only) Table or record not found */
#define MNDB_FULL        13   /* Insertion failed because database is full */
#define MNDB_CANTOPEN    14   /* Unable to open the database file */
#define MNDB_PROTOCOL    15   /* Database lock protocol error */
#define MNDB_EMPTY       16   /* (Internal Only) Database table is empty */
#define MNDB_SCHEMA      17   /* The database schema changed */
#define MNDB_TOOBIG      18   /* Too much data for one row of a table */
#define MNDB_CONSTRAINT  19   /* Abort due to contraint violation */
#define MNDB_MISMATCH    20   /* Data type mismatch */
#define MNDB_MISUSE      21   /* Library used incorrectly */
#define MNDB_NOLFS       22   /* Uses OS features not supported on host */
#define MNDB_AUTH        23   /* Authorization denied */
#define MNDB_FORMAT      24   /* Auxiliary database format error */
#define MNDB_RANGE       25   /* 2nd parameter to sqlite_bind out of range */
#define MNDB_NOTADB      26   /* File opened that is not a database file */
#define MNDB_ROW         100  /* sqlite_step() has another row ready */
#define MNDB_DONE        101  /* sqlite_step() has finished executing */

#endif /*_MNDB_*/
