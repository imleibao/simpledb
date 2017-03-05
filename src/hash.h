/*
** 2001 September 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This is the header file for the generic hash-table implemenation
** used in SQLite.
**
** $Id: hash.h,v 1.6 2004/01/08 02:17:33 drh Exp $
*/
#ifndef _MNDB_HASH_H_
#define _MNDB_HASH_H_

/* Forward declarations of structures. */
typedef struct Hash Hash;
typedef struct HashElem HashElem;

/* A complete hash table is an instance of the following structure.
** The internals of this structure are intended to be opaque -- client
** code should not attempt to access or modify the fields of this structure
** directly.  Change this structure only by using the routines below.
** However, many of the "procedures" and "functions" for modifying and
** accessing this structure are really macros, so we can't really make
** this structure opaque.
*/
struct Hash {
  char keyClass;          /* MNDB_HASH_INT, _POINTER, _STRING, _BINARY */
  char copyKey;           /* True if copy of key made on insert */
  int count;              /* Number of entries in this table */
  HashElem *first;        /* The first element of the array */
  int htsize;             /* Number of buckets in the hash table */
  struct _ht {            /* the hash table */
    int count;               /* Number of entries with this hash */
    HashElem *chain;         /* Pointer to first entry with this hash */
  } *ht;
};

/* Each element in the hash table is an instance of the following 
** structure.  All elements are stored on a single doubly-linked list.
**
** Again, this structure is intended to be opaque, but it can't really
** be opaque because it is used by macros.
*/
struct HashElem {
  HashElem *next, *prev;   /* Next and previous elements in the table */
  void *data;              /* Data associated with this element */
  void *pKey; int nKey;    /* Key associated with this element */
};

/*
** There are 4 different modes of operation for a hash table:
**
**   MNDB_HASH_INT         nKey is used as the key and pKey is ignored.
**
**   MNDB_HASH_POINTER     pKey is used as the key and nKey is ignored.
**
**   MNDB_HASH_STRING      pKey points to a string that is nKey bytes long
**                           (including the null-terminator, if any).  Case
**                           is ignored in comparisons.
**
**   MNDB_HASH_BINARY      pKey points to binary data nKey bytes long. 
**                           memcmp() is used to compare keys.
**
** A copy of the key is made for MNDB_HASH_STRING and MNDB_HASH_BINARY
** if the copyKey parameter to HashInit is 1.  
*/
#define MNDB_HASH_INT       1
/* #define MNDB_HASH_POINTER   2 // NOT USED */
#define MNDB_HASH_STRING    3
#define MNDB_HASH_BINARY    4

/*
** Access routines.  To delete, insert a NULL pointer.
*/
void mndbHashInit(Hash*, int keytype, int copyKey);
void *mndbHashInsert(Hash*, const void *pKey, int nKey, void *pData);
void *mndbHashFind(const Hash*, const void *pKey, int nKey);
void mndbHashClear(Hash*);

/*
** Macros for looping over all elements of a hash table.  The idiom is
** like this:
**
**   Hash h;
**   HashElem *p;
**   ...
**   for(p=mndbHashFirst(&h); p; p=mndbHashNext(p)){
**     SomeStructure *pData = mndbHashData(p);
**     // do something with pData
**   }
*/
#define mndbHashFirst(H)  ((H)->first)
#define mndbHashNext(E)   ((E)->next)
#define mndbHashData(E)   ((E)->data)
#define mndbHashKey(E)    ((E)->pKey)
#define mndbHashKeysize(E) ((E)->nKey)

/*
** Number of entries in a hash table
*/
#define mndbHashCount(H)  ((H)->count)

#endif /* _MNDB_HASH_H_ */
