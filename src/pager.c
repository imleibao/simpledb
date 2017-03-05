/*
** 2015 Dec
** This is the implemention of the page cache subsystem or "pager"
** The pager is used to access the database disk file. It also implements file
** locking to prevent two process from writing the same database file
** simultaneously,  or one process reading the database while another is writing.
*/

#include "os.h"
#include "mndbInt.h"
#include "pager.h"
#include "assert.h"
#include "string.h"
  
#define MNDB_UNLOCK 0
#define MNDB_READLOCK 1
#define MNDB_WRITELOCK 2

/*
** Each in-memory image of a page begins with the following header. This header is only ** visible to this pager module.
*/
typedef struct PgHdr PgHdr;
struct PgHdr{
  Pager *pPager;
  Pgno pgno;
  PgHdr *pNextHash, *pPrevHash;//hash bucket里面的所有pageHdr
  PgHdr *pNextAll, *pPrevAll;//和pager的pAll相关，是所有page的列表中的节点，且此列表不是循环列表，从表头插入即pAll处插入
  int nRef;
  PgHdr *pNextFree, *pPrevFree;//和pager的pFirst、pLast一样与free list有关，该list非循环列表，
  u8 dirty;
  PgHdr *pDirty; //?Tudo: if it is point to the next dirty page or just the head of the dirty page list.(maybe the head,which has smallest pgno)
  /*MNDB_PAGE_SIZE bytes of page data follow this header*/
  /*Pager.nExtra bytes of local data follow the page data, specified by the parama nEx passed by the open function*/
};
/*
** Convert a pointer to a PgHdr into a pointer to its data
** and back again
*/
#define PGHDR_TO_DATA(P) ((void*)(&(P)[1]))
#define DATA_TO_PGHDR(D) (&((PgHdr*)(D))[-1])
#define PGHDR_TO_EXTRA(P) ((void*)&((char*)(&(P)[1]))[MNDB_PAGE_SIZE])

/*
** How big to make the hash table used for locating in-memory pages by page number
*/
#define N_PG_HASH 2048

/*
** Hash a page number
*/
#define pager_hash(PN) ((PN)&(N_PG_HASH-1))

/*
** A open page cache is an instance of the following structure.
*/
struct Pager{
  char *zFilename;
  char *zDirectory;
  OsFile fd;
  int dbSize;
  int origDbSize; //?
  int nExtra; /* Total number of in-memory pages */
  void (*xDestructor)(void*);
  int nPage; /* Total number of in-memory pages */
  int nRef;
  int mxPage;
  int nHit, nMiss, nOvfl; /* Cache hits, missing, and LRU overflows */    
  u8 state;
  u8 errMask;
  u8 tempFile; //?
  u8 readOnly;
  u8 dirtyFile;               /* True if database file has changed in any way */
  PgHdr *pFirst, *pLast; //List of free pages
  PgHdr *pAll;
  PgHdr *aHash[N_PG_HASH];
};

#define PAGER_ERR_FULL    0X01
#define PAGER_ERR_MEM     0X02
#define PAGER_ERR_LOCK    0X04
#define PAGER_ERR_CORRUPT 0X08
#define PAGER_ERR_DISK    0X10

/* 
** write a 32-bit integer into a page header right before the
** page data. This will overwrite the PgHdr.dirty pointer.
*/
static void store32bits(u32 val, PgHdr *p, int offset){
  unsigned char *ac;
  ac = &((unsigned char*)PGHDR_TO_DATA(p))[offset];
  memcpy(ac, &val, 4); //using native byte order,usually small-endian
}

/*
** Convert the bits in the pPager->errMask into an approprate
** return code.
*/
static int pager_errcode(Pager *pPager){
  int rc = MNDB_OK;
  if( pPager->errMask & PAGER_ERR_LOCK )    rc = MNDB_PROTOCOL;
  if( pPager->errMask & PAGER_ERR_DISK )    rc = MNDB_IOERR;
  if( pPager->errMask & PAGER_ERR_FULL )    rc = MNDB_FULL;
  if( pPager->errMask & PAGER_ERR_MEM )     rc = MNDB_NOMEM;
  if( pPager->errMask & PAGER_ERR_CORRUPT ) rc = MNDB_CORRUPT;
  return rc;
}

/*
** Unlock the database and clear the in-memory cache.  This routine
** sets the state of the pager back to what it was when it was first
** opened.  Any outstanding pages are invalidated and subsequent attempts
** to access those pages will likely result in a coredump.
*/
static PgHdr* pager_lookup(Pager *pPager, Pgno pgno){
  PgHdr *p = pPager->aHash[pager_hash(pgno)];
  while(p && p->pgno != pgno)
    p = p->pNextHash;
  return p;
}

/*
** When this routine is called, the pager has the journal file open and
** a write lock on the database.  This routine releases the database
** write lock and acquires a read lock in its place.  The journal file
** is deleted and closed.
**
** TODO: Consider keeping the journal file open for temporary databases.
** This might give a performance improvement on windows where opening
** a file is an expensive operation.
*/
static int pager_unwritelock(Pager *pPager){
  // error 
  if(pPager->state < MNDB_WRITELOCK) return MNDB_OK;

  assert(pPager->dirtyFile == 0);
  
  int rc;
  rc = mndbOsReadLock(&pPager->fd);
  if(rc == MNDB_OK){
    pPager->state = MNDB_READLOCK;
  }
  else{
    /* This can only happen if a process does a BEGIN, then forks and the
    ** child process does the COMMIT.  Because of the semantics of unix
    ** file locking, the unlock will fail.
    */
    pPager->state = MNDB_UNLOCK;
  }
  return rc; 
}

/*
** Unlock the database and clear the in-memory cache.  This routine
** sets the state of the pager back to what it was when it was first
** opened.  Any outstanding pages are invalidated and subsequent attempts
** to access those pages will likely result in a coredump.
*/
static void pager_reset(Pager *pPager){
  PgHdr *pPg, *pNext;
  for(pPg = pPager->pAll; pPg; pPg = pNext){
    pNext = pPg->pNextAll;
    mndbFree(pPg);
  }
  
  pPager->pFirst = 0;
  pPager->pLast = 0;
  pPager->pAll = 0;
  memset(pPager->aHash, 0, sizeof(pPager->aHash));
  pPager->nPage = 0;
  //simply report the when lockstate >= write
  //assert(pPager->state >= MNDB_WRITELOCK);
  pager_unwritelock(pPager);
  mndbOsUnlock(&pPager->fd);
  pPager->state = MNDB_UNLOCK;
  pPager->dbSize = -1;
  pPager->nRef = 0;
}

/*
** Open a temporary file.  Write the name of the file into zName
** (zName must be at least MNDB_TEMPNAME_SIZE bytes long.)  Write
** the file descriptor into *fd.  Return MNDB_OK on success or some
** other error code if we fail.
**
** The OS will automatically delete the temporary file when it is
** closed.
*/
static int mndbpager_opentemp(char *zFile, OsFile *fd){
  int cnt = 8;
  int rc;
  do{
    cnt--;
    mndbOsTempFileName(zFile);
  }while( cnt>0 && rc!=MNDB_OK );
  return rc;
}

/*
** Create a new page cache and put a pointer to the page cache in *ppPager.
** The file to be cached need not exist.  The file is not locked until
** the first call to sqlitepager_get() and is only held open until the
** last page is released using sqlitepager_unref().
**
** If zFilename is NULL then a randomly-named temporary file is created
** and used as the file to be cached.  The file will be deleted
** automatically when it is closed.
*/
  int mndbpager_open(Pager **ppPager, const char *zFilename, int mxPage, int nExtra){
  Pager *pPager;
  char *zFullPathname;
  int nameLen;
  OsFile fd;
  int rc, i;
  int tempFile;
  int readOnly = 0;
  char zTemp[MNDB_TEMPNAME_SIZE];

  *ppPager = 0;
  if(mndb_malloc_failed){
    return MNDB_NOMEM;
  }
  if(zFilename && zFilename[0]){
    zFullPathname = mndbOsFullPathname(zFilename);
    rc = mndbOsOpenReadWrite(zFullPathname, &fd, &readOnly);
    tempFile = 0;
  }else{
    rc = mndbpager_opentemp(zTemp, &fd);
    zFilename = zTemp;
    zFullPathname = mndbOsFullPathname(zFilename);
    tempFile = 1;
  }
  
  if(mndb_malloc_failed) return MNDB_NOMEM;
  if( rc!=MNDB_OK ){
    mndbFree(zFullPathname);
    return MNDB_CANTOPEN;
  } 
  nameLen = strlen(zFullPathname);
  pPager = mndbMalloc( sizeof(*pPager) + nameLen*2 + 30 ); // 因为filename filedirectory 的空间也存储在此后

  if( pPager==0 ){
    mndbOsClose(&fd);
    mndbFree(zFullPathname);
    return MNDB_NOMEM;
  }

  pPager->zFilename = (char*)&pPager[1];
  pPager->zDirectory = &pPager->zFilename[nameLen+1];
  strcpy(pPager->zFilename, zFullPathname);
  strcpy(pPager->zDirectory, zFullPathname);
  for(i=nameLen; i>0 && pPager->zDirectory[i-1]!='/'; i--){}
  if( i>0 ) pPager->zDirectory[i-1] = 0;
  mndbFree(zFullPathname);

  pPager->fd = fd;
  pPager->nRef = 0;
  pPager->dbSize = -1;
  pPager->nPage = 0;
  pPager->mxPage = mxPage>5 ? mxPage : 10;
  pPager->state = MNDB_UNLOCK;
  pPager->errMask = 0;
  pPager->tempFile = tempFile;
  pPager->readOnly = readOnly;
  pPager->pFirst = 0;
  pPager->pLast = 0;
  pPager->nExtra = nExtra;
  memset(pPager->aHash, 0, sizeof(pPager->aHash));
  *ppPager = pPager;
  return MNDB_OK;
}  
  
/*
** Set the destructor for this pager.  If not NULL, the destructor is called
** when the reference count on each page reaches zero.  The destructor can
** be used to clean up information in the extra segment appended to each page.
**
** The destructor is not called as a result sqlitepager_close().  
** Destructors are only called by sqlitepager_unref().
*/
void mndbpager_set_destructor(Pager *pPager, void (*xDesc)(void*)){
  pPager->xDestructor = xDesc;
}

/*
** Return the total number of pages in the disk file associated with
** pPager.
*/
int mndbpager_pagecount(Pager *pPager){
  off_t n;
  assert( pPager!=0 );
  if( pPager->dbSize>=0 ){
    return pPager->dbSize;
  }
  if( mndbOsFileSize(&pPager->fd, &n)!=MNDB_OK ){
    pPager->errMask |= PAGER_ERR_DISK;
    return 0; 
  }
  n /= MNDB_PAGE_SIZE;
  if( pPager->state!=MNDB_UNLOCK ){
    pPager->dbSize = n;
  }
  return n;
}

/*
** Shutdown the page cache.  Free all memory and close all files.
**
** If a transaction was in progress when this routine is called, that
** transaction is rolled back.  All outstanding pages are invalidated
** and their memory is freed.  Any attempt to use a page associated
** with this page cache after this function returns will likely
** result in a coredump.
**
** Tudo: what if the page is dirty;
*/
int mndbpager_close(Pager *pPager){
  PgHdr *pPg, *pNext;
  switch( pPager->state ){
    case MNDB_WRITELOCK:
    case MNDB_READLOCK: {
      mndbOsUnlock(&pPager->fd);
      break;
    }
    default: {
      /* Do nothing */
      break;
    }
  }
  for(pPg=pPager->pAll; pPg; pPg=pNext){
    pNext = pPg->pNextAll;
    mndbFree(pPg);
  }
  mndbOsClose(&pPager->fd);
  /* Temp files are automatically deleted by the OS
  ** if( pPager->tempFile ){
  **   sqliteOsDelete(pPager->zFilename);
  ** }
  */
	      //  CLR_PAGER(pPager);
  if( pPager->zFilename!=(char*)&pPager[1] ){
    assert( 0 );  /* Cannot happen */
    mndbFree(pPager->zFilename);
    mndbFree(pPager->zDirectory);
  }
  mndbFree(pPager);
  return MNDB_OK;
}

/*
** Return the page number for the given page data.
*/
Pgno mndbpager_pagenumber(void *pData){
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  return pPg->pgno;
}

/* 
** Increment the refrence to the given page. If the page
** is in the free page list, then remove it from the list
** 
** ?Tudo: is free list is a cycle; yesA
*/
#define page_ref(P) ((P)->nRef == 0?_page_ref(P):(P)->nRef++);
static void _page_ref(PgHdr *pPg){
  if(pPg->nRef == 0){
    if(pPg->pPrevFree){
      pPg->pPrevFree->pNextFree = pPg->pNextFree;
    }
    else{
      pPg->pPager->pFirst = pPg->pNextFree;
    }
    if(pPg->pNextFree){
      pPg->pNextFree->pPrevFree = pPg->pPrevFree;
    }
    else{
      pPg->pPager->pLast = pPg->pPrevFree;
    }
    pPg->pPager->nRef++; 
  }
  ++pPg->nRef;
  //test:REFINFO(pPg);
}

/*
** Increment the reference count for a page
*/
int mndbpager_ref(void *pData){
  PgHdr *p = DATA_TO_PGHDR(pData);
  page_ref(p);
  return MNDB_OK;
}

//只有commit被调用或者回收一页的时候才会调用（懒惰写）；
static int pager_write_pagelist(PgHdr *pList){
  Pager *pPager;
  int rc;
2
  if(pList == 0) return MNDB_OK;
  pPager = pList->pPager;
  while(pList){
    assert(pList->dirty);
    mndbOsSeek(&pPager->fd, (pList->pgno-1)*(off_t)MNDB_PAGE_SIZE);
    //test
    //TRACE2("STORE %d\n", pList->pgno);
    rc = mndbOsWrite(&pPager->fd, PGHDR_TO_DATA(pList), MNDB_PAGE_SIZE);
    if(rc) return rc; //some one failed
    pList->dirty = 0;
    pList = pList->pDirty;
  }
  pPager->dirtyFile = 0;
  return MNDB_OK;
}

/*
** Collect every dirty pages into a dirty list and return a pointer to the head
** of the list
*/
static PgHdr* pager_get_all_dirty_pages(Pager *pPager){
  PgHdr *pList, *p;
  pList = 0;
  for(p = pPager->pAll; p; p = p->pNextAll){
    if(p->dirty){
      p->pDirty = pList;
      pList = p;
    }
  }
  return pList;
}

/*
** Acquire a page.
**
** A read lock on the disk file is obtained when the first page is acquired. 
** This read lock is dropped when the last page is released.
**
** A _get works for any page number greater than 0.  If the database
** file is smaller than the requested page, then no actual disk
** read occurs and the memory image of the page is initialized to
** all zeros.  The extra data appended to a page is always initialized
** to zeros the first time a page is loaded into memory.
**
** The acquisition might fail for several reasons.  In all cases,
** an appropriate error code is returned and *ppPage is set to NULL.
**
** See also sqlitepager_lookup().  Both this routine and _lookup() attempt
** to find a page in the in-memory cache first.  If the page is not already
** in memory, this routine goes to disk to read it in whereas _lookup()
** just returns 0.  This routine acquires a read-lock the first time it
** has to go to disk, and could also playback an old journal if necessary.
** Since _lookup() never goes to disk, it never has to deal with locks
** or journal files.
*/
int mndbpager_get(Pager *pPager, Pgno pgno, void **ppPage){
  PgHdr *pPg;
  int rc;

  /* Make sure we have not hit any critical errors.
  */ 
  assert( pPager!=0 );
  assert( pgno!=0 );
  *ppPage = 0;
  if( pPager->errMask & ~(PAGER_ERR_FULL) ){
     return pager_errcode(pPager);
   }

  /* If this is the first page accessed, then get a read lock
  ** on the database file.
  */
  if( pPager->nRef==0 ){
    rc = mndbOsReadLock(&pPager->fd);
    if( rc!=MNDB_OK ){
      return rc;
    }
    pPager->state = MNDB_READLOCK;


    pPg = 0;
  }else{
    /* Search for page in cache */
    pPg = pager_lookup(pPager, pgno);
  }
  if( pPg==0 ){
    /* The requested page is not in the page cache. */
    int h;
    pPager->nMiss++;
    if( pPager->nPage < pPager->mxPage || pPager->pFirst==0 ){
      /* Create a new page */
      pPg = mndbMallocRaw( sizeof(*pPg) + MNDB_PAGE_SIZE 
			   + sizeof(u32)+ pPager->nExtra );//?u32 是什么
      if( pPg==0 ){
        pager_unwritelock(pPager);
        pPager->errMask |= PAGER_ERR_MEM;
        return MNDB_NOMEM;
      }
      memset(pPg, 0, sizeof(*pPg));
      pPg->pPager = pPager;
      pPg->pNextAll = pPager->pAll;
      if( pPager->pAll ){
        pPager->pAll->pPrevAll = pPg;
      }
      pPg->pPrevAll = 0;
      pPager->pAll = pPg;
      pPager->nPage++;
    }else{
      /* Find a page to recycle.  Try to locate a page that does not
      ** require us to do an fsync() on the journal.
      */
      
      //?Tudo:what if there is no free pages;
      pPg = pPager->pFirst;
 
      while(pPg && pPg->dirty){
	pPg = pPg->pNextFree;
      }
      /* If we could not find a page that does not require an fsync()
      ** on the journal file then fsync the journal file.  This is a
      ** very slow operation, so we work hard to avoid it.  But sometimes
      ** it can't be helped.
      */
      /*
      ** Tudo : If No free page, recycle the page by LRU.
      */
      /* Write the page to the database file if it is dirty.
      */
      if(pPg==0 || pPg->dirty){
        pPg->pDirty = 0;
	assert( pPg->nRef==0 );
        rc = pager_write_pagelist( pPg );// 打开一次磁盘上的文件，尽可能多的写入；??
	pPg = pPager->pFirst;
      }
      assert(pPg->ref == 0);
      assert(pPg->dirty == 0);
        
      /* Unlink the old page from the free list and the hash table
      */

      if( pPg->pPrevFree ){
        pPg->pPrevFree->pNextFree = pPg->pNextFree;
      }else{
        assert( pPager->pFirst==pPg );
        pPager->pFirst = pPg->pNextFree;
      }
      if( pPg->pNextFree ){
        pPg->pNextFree->pPrevFree = pPg->pPrevFree;
      }else{
        assert( pPager->pLast==pPg );
        pPager->pLast = pPg->pPrevFree;
      }
      //remove from the free page table
      pPg->pNextFree = pPg->pPrevFree = 0;
      if( pPg->pNextHash ){
        pPg->pNextHash->pPrevHash = pPg->pPrevHash;
      }
      if( pPg->pPrevHash ){
        pPg->pPrevHash->pNextHash = pPg->pNextHash;
      }else{
        h = pager_hash(pPg->pgno);
        assert( pPager->aHash[h]==pPg );
        pPager->aHash[h] = pPg->pNextHash;
      }
      pPg->pNextHash = pPg->pPrevHash = 0;
      pPager->nOvfl++;
    }
    pPg->pgno = pgno;
    pPg->dirty = 0;
    pPg->nRef = 1;
    //test    REFINFO(pPg);
    pPager->nRef++;
    h = pager_hash(pgno);
    pPg->pNextHash = pPager->aHash[h];
    pPager->aHash[h] = pPg;
    if( pPg->pNextHash ){
      assert( pPg->pNextHash->pPrevHash==0 );
      pPg->pNextHash->pPrevHash = pPg;
    }
    if(pPager->nExtra>0){
      memset(PGHDR_TO_EXTRA(pPg),0,pPager->nExtra);
    }
    if( pPager->dbSize<0 ) mndbpager_pagecount(pPager);
    //!!
    if( pPager->errMask!=0 ){
      mndbpager_unref(PGHDR_TO_DATA(pPg));
      rc = pager_errcode(pPager);
      return rc;
    }
    //!!

    if( pPager->dbSize<(int)pgno ){
      memset(PGHDR_TO_DATA(pPg), 0, MNDB_PAGE_SIZE);
    }else{
      int rc;
      mndbOsSeek(&pPager->fd, (pgno-1)*(off_t)MNDB_PAGE_SIZE);
      rc = mndbOsRead(&pPager->fd, PGHDR_TO_DATA(pPg), MNDB_PAGE_SIZE);
      //!TRACE2("FETCH %d\n", pPg->pgno);
 
      if( rc!=MNDB_OK ){
        off_t fileSize;
        if( mndbOsFileSize(&pPager->fd,&fileSize)!=MNDB_OK
               || fileSize>=pgno*MNDB_PAGE_SIZE ){
          mndbpager_unref(PGHDR_TO_DATA(pPg));
          return rc;
        }else{
          memset(PGHDR_TO_DATA(pPg), 0, MNDB_PAGE_SIZE);
        }
      }
    }
  }else{
    /* The requested page is in the page cache. */
    pPager->nHit++;
    page_ref(pPg);
  }
  *ppPage =PGHDR_TO_DATA(pPg);
  return MNDB_OK;
}
/*
** Acquire a page if it is already in the in-memory cache.  Do
** not read the page from disk.  Return a pointer to the page,
** or 0 if the page is not in cache.
**
** See also sqlitepager_get().  The difference between this routine
** and sqlitepager_get() is that _get() will go to the disk and read
** in the page if the page is not already in cache.  This routine
** returns NULL if the page is not in cache or if a disk I/O error 
** has ever happened.
*/
void *mndbpager_lookup(Pager *pPager, Pgno pgno){
  PgHdr *pPg;

  assert( pPager!=0 );
  assert( pgno!=0 );
  if( pPager->errMask & ~(PAGER_ERR_FULL) ){
    return 0;
  }
  /* if( pPager->nRef==0 ){
  **  return 0;
  ** }
  */
  pPg = pager_lookup(pPager, pgno);
  if( pPg==0 ) return 0;
  page_ref(pPg);
  return PGHDR_TO_DATA(pPg);
}

int mndbpager_unref(void *pData){
  PgHdr *pPg; 
  pPg = DATA_TO_PGHDR(pData);
  assert(pPg->nRef > 0);
  --pPg->nRef;
  //TEST REFINFO(pPg);

  /* When the number of references to a page reach 0, call the
  ** destructor and add the page to the freelist.
  */
  if( pPg->nRef==0 ){
    Pager *pPager;
    pPager = pPg->pPager;
    pPg->pNextFree = 0;
    pPg->pPrevFree = pPager->pLast;
    pPager->pLast = pPg;
    if( pPg->pPrevFree ){
      pPg->pPrevFree->pNextFree = pPg;
    }else{
      pPager->pFirst = pPg;
    }

    if( pPager->xDestructor ){
      pPager->xDestructor(pData);
    }
  
    /* When all pages reach the freelist, drop the read lock from
    ** the database file.
    */
    pPager->nRef--;
    assert( pPager->nRef>=0 );
    if( pPager->nRef==0 ){
      pager_reset(pPager);
    }
  }
  return MNDB_OK;
}


/*
** Acquire a write-lock on the database.  The lock is removed when
** the any of the following happen:
**
**   *  sqlitepager_commit() is called.
**   *  sqlitepager_rollback() is called.
**   *  sqlitepager_close() is called.
**   *  sqlitepager_unref() is called to on every outstanding page.
**
** The parameter to this routine is a pointer to any open page of the
** database file.  Nothing changes about the page - it is used merely
** to acquire a pointer to the Pager structure and as proof that there
** is already a read-lock on the database.
**
** If the database is already write-locked, this routine is a no-op.
*/
int mndbpager_begin(void *pData){
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  Pager *pPager = pPg->pPager;
  int rc = MNDB_OK;
  assert( pPg->nRef>0 );
  assert( pPager->state!=MNDB_UNLOCK );
  if( pPager->state==MNDB_READLOCK ){
    rc = mndbOsWriteLock(&pPager->fd);
    if( rc!=MNDB_OK ){
      return rc;
    }
    pPager->state = MNDB_WRITELOCK;
    pPager->dirtyFile = 0;
    //Test TRACE1("TRANSACTION\n");
  }
  return rc;
}

/*
** Mark a data page as writeable.  The page is written into the journal 
** if it is not there already.  This routine must be called before making
** changes to a page.
**
** The first time this routine is called, the pager creates a new
** journal and acquires a write lock on the database.  If the write
** lock could not be acquired, this routine returns MNDB_BUSY.  The
** calling routine must check for that return value and be careful not to
** change any page data until this routine returns MNDB_OK.
**
** If the journal file could not be written because the disk is full,
** then this routine returns MNDB_FULL and does an immediate rollback.
** All subsequent write attempts also return MNDB_FULL until there
** is a call to sqlitepager_commit() or sqlitepager_rollback() to
** reset.
*/
int mndbpager_write(void *pData){
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  Pager *pPager = pPg->pPager;
  int rc = MNDB_OK;

  /* Check for errors
  */
  if( pPager->errMask ){ 
    return pager_errcode(pPager);
  }
  if( pPager->readOnly ){
    return MNDB_PERM;
  }

  /* Mark the page as dirty.  If the page has already been written
  ** to the journal then we can return right away.
  */
  pPg->dirty = 1;
  pPager->dirtyFile = 1;

  return MNDB_OK;
}

/*
** Return TRUE if the page given in the argument was previously passed
** to sqlitepager_write().  In other words, return TRUE if it is ok
** to change the content of the page.
*/ 
int mndbpager_iswriteable(void *pData){
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  return pPg->dirty;
}

/*
** Replace the content of a single page with the information in the third
** argument.
*/
int mndbpager_overwrite(Pager *pPager, Pgno pgno, void *pData){
  void *pPage;
  int rc;
  rc = mndbpager_get(pPager, pgno, &pPage);
  if(rc == MNDB_OK){
    rc = mndbpager_write(pPage);
    if(rc == MNDB_OK){
      memcpy(pPager, pData, MNDB_PAGE_SIZE);
    }
    mndbpager_unref(pPage);
  }
  return rc;
}


/*
** Commit all changes to the database and release the write lock.
**
** If the commit fails for any reason, a rollback attempt is made
** and an error code is returned.  If the commit worked, MNDB_OK
** is returned.
*/
int mndbpager_commit(Pager *pPager){
  int rc;
  PgHdr *pPg;

  /*if( pPager->errMask==PAGER_ERR_FULL ){
    rc = sqlitepager_rollback(pPager);
    if( rc==MNDB_OK ){
      rc = MNDB_FULL;
    }
    return rc;
  }
  if( pPager->errMask!=0 ){
    rc = pager_errcode(pPager);
    return rc;
    }*/
  if( pPager->state!=MNDB_WRITELOCK ){
    return MNDB_ERROR;
  }

  //test TRACE1("COMMIT\n");
  if( pPager->dirtyFile!=0 ){
    pPg = pager_get_all_dirty_pages(pPager);
    if( pPg ){
      rc = pager_write_pagelist(pPg);
      if(rc != MNDB_OK)
	return rc;
    }
  }
  rc = pager_unwritelock(pPager);
  pPager->dbSize = -1;
  return rc;
}

/*
** Return TRUE if the database file is opened read-only.  Return FALSE
** if the database is (in theory) writable.
*/
int mndbpager_isreadonly(Pager *pPager){
  return pPager->readOnly;
}

/*
** This routine is used for testing and analysis only.
*/
int *mndbpager_stats(Pager *pPager){
  static int a[9];
  a[0] = pPager->nRef;
  a[1] = pPager->nPage;
  a[2] = pPager->mxPage;
  a[3] = pPager->dbSize;
  a[4] = pPager->state;
  a[5] = pPager->errMask;
  a[6] = pPager->nHit;
  a[7] = pPager->nMiss;
  a[8] = pPager->nOvfl;
  return a;
}

const char* mndbpager_filename(Pager *pPager){
  return pPager->zFilename;
}

















