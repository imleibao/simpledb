#include"mndbInt.h"
#include"pager.h"
#include"btree.h"
#include<assert.h>

typedef unsigned int uptr;
typedef unsigned char u8;
typedef unsigned short int u16;

#define Addr(P) ((uptr)(P))

//Used only in this file
typedef struct MemPage MemPage;
typedef struct PageOne PageOne;
typedef struct PageHdr PageHdr;
typedef struct CellHdr CellHdr;
typedef struct Cell Cell;
typedef struct FreeBlk  FreeBlk;
typedef struct OverflowPage OverflowPage;

/*
** All structures on a database page are aligned to 4-byte boundries.
** This routine rounds up a number of bytes to the next multiple of 4.
**
** This might need to change for computer architectures that require
** and 8-byte alignment boundry for structures.
*/
#define ROUNDUP(X)  ((X+3) & ~3)

struct PageOne{
  Pgno freeList;
  int nFree;
};

/*
** Each database page has a header that is an instance of this
** structure.
**
** PageHdr.firstFree is 0 if there is no free space on this page.
** Otherwise, PageHdr.firstFree is the index in MemPage.u.aDisk[] of a 
** FreeBlk structure that describes the first block of free space.  
** All free space is defined by a linked list of FreeBlk structures.
**
** Data is stored in a linked list of Cell structures.  PageHdr.firstCell
** is the index into MemPage.u.aDisk[] of the first cell on the page.  The
** Cells are kept in sorted order.
**
** A Cell contains all information about a database entry and a pointer
** to a child page that contains other entries less than itself.  In
** other words, the i-th Cell contains both Ptr(i) and Key(i).  The
** right-most pointer of the page is contained in PageHdr.rightChild.
*/
struct PageHdr {
  Pgno rightChild;  /* Child page that comes after all cells on this page */
  u16 firstCell;    /* Index in MemPage.u.aDisk[] of the first cell */
  u16 firstFree;    /* Index in MemPage.u.aDisk[] of the first free block */
};

/*
** Entries on a page of the database are called "Cells".  Each Cell
** has a header and data.  This structure defines the header.  The
** key and data (collectively the "payload") follow this header on
** the database page.
**
** A definition of the complete Cell structure is given below.  The
** header for the cell must be defined first in order to do some
** of the sizing #defines that follow.
*/
struct CellHdr {
  Pgno leftChild; /* Child page that comes before this cell */
  u16 nKey;       /* Number of bytes in the key */
  u16 iNext;      /* Index in MemPage.u.aDisk[] of next cell in sorted order */
  u32 nData;      /* Number of bytes of data */
};

#define MIN_CELL_SIZE (sizeof(CellHdr)+4)
/*
** The maximum number of database entries that can be held in a single
** page of the database. 
*/
#define MX_CELL ((MNDB_PAGE_SIZE-sizeof(PageHdr))/MIN_CELL_SIZE)

/*
** The amount of usable space on a single page of the BTree.  This is the
** page size minus the overhead of the page header.
*/
#define USABLE_SPACE  (MNDB_PAGE_SIZE - sizeof(PageHdr))

/*
** The maximum amount of payload (in bytes) that can be stored locally for
** a database entry.  If the entry contains more data than this, the
** extra goes onto overflow pages.
**
** This number is chosen so that at least 4 cells will fit on every page.
*/
#define MX_LOCAL_PAYLOAD ((USABLE_SPACE/4-(sizeof(CellHdr)+sizeof(Pgno)))&~3)


/*
** Data on a database page is stored as a linked list of Cell structures.
** Both the key and the data are stored in aPayload[].  The key always comes
** first.  The aPayload[] field grows as necessary to hold the key and data,
** up to a maximum of MX_LOCAL_PAYLOAD bytes.  If the size of the key and
** data combined exceeds MX_LOCAL_PAYLOAD bytes, then Cell.ovfl is the
** page number of the first overflow page.
**
** Though this structure is fixed in size, the Cell on the database
** page varies in size.  Every cell has a CellHdr and at least 4 bytes
** of payload space.  Additional payload bytes (up to the maximum of
** MX_LOCAL_PAYLOAD) and the Cell.ovfl value are allocated only as
** needed.
*/
struct Cell {
  CellHdr h;                        /* The cell header */
  char aPayload[MX_LOCAL_PAYLOAD];  /* Key and data */
  Pgno ovfl;                        /* The first overflow page */
};

/*
** Free space on a page is remembered using a linked list of the FreeBlk
** structures.  Space on a database page is allocated in increments of
** at least 4 bytes and is always aligned to a 4-byte boundry.  The
** linked list of FreeBlks is always kept in order by address.
** 最少四字节，因为FreeBlk的信息有四个字节U16+U16，且保存在物理磁盘的FreeBlock处
*/
struct FreeBlk {
  u16 iSize;      /* Number of bytes in this block of free space */
  u16 iNext;      /* Index in MemPage.u.aDisk[] of the next free block */
};

/*
** The number of bytes of payload that will fit on a single overflow page.
*/
#define OVERFLOW_SIZE (MNDB_PAGE_SIZE-sizeof(Pgno))


/*
** When the key and data for a single entry in the BTree will not fit in
** the MX_LOCAL_PAYLOAD bytes of space available on the database page,
** then all extra bytes are written to a linked list of overflow pages.
** Each overflow page is an instance of the following structure.
**
** Unused pages in the database are also represented by instances of
** the OverflowPage structure.  The PageOne.freeList field is the
** page number of the first page in a linked list of unused database
** pages.
*/
struct OverflowPage {
  Pgno iNext;
  char aPayload[OVERFLOW_SIZE];
};

/*
** For every page in the database file, an instance of the following structure
** is stored in memory.  The u.aDisk[] array contains the raw bits read from
** the disk.  The rest is auxiliary information that held in memory only. The
** auxiliary info is only valid for regular database pages - it is not
** used for overflow pages and pages on the freelist.
**
** Of particular interest in the auxiliary info is the apCell[] entry.  Each
** apCell[] entry is a pointer to a Cell structure in u.aDisk[].  The cells are
** put in this array so that they can be accessed in constant time, rather
** than in linear time which would be needed if we had to walk the linked 
** list on every access.
**
** Note that apCell[] contains enough space to hold up to two more Cells
** than can possibly fit on one page.  In the steady state, every apCell[]
** points to memory inside u.aDisk[].  But in the middle of an insert
** operation, some apCell[] entries may temporarily point to data space
** outside of u.aDisk[].  This is a transient situation that is quickly
** resolved.  But while it is happening, it is possible for a database
** page to hold as many as two more cells than it might otherwise hold.
** The extra too entries in apCell[] are an allowance for this situation.
**
** The pParent field points back to the parent page.  This allows us to
** walk up the BTree from any leaf to the root.  Care must be taken to
** unref() the parent page pointer when this page is no longer referenced.
** The pageDestructor() routine handles that chore.
*/
struct MemPage {
  union {
    char aDisk[MNDB_PAGE_SIZE];  /* Page data stored on disk */
    PageHdr hdr;                   /* Overlay page header */
  } u; 
  int isInit;                    /* True if auxiliary data is initialized */
  MemPage *pParent;              /* The parent of this page.  NULL for root */
  int nFree;                     /* Number of free bytes in u.aDisk[] */
  int nCell;                     /* Number of entries on this page */
  int isOverfull;                /* Some apCell[] points outside u.aDisk[] */
  Cell *apCell[MX_CELL+2];       /* All data entires in sorted order */
};

/*
** The in-memory image of a disk page has the auxiliary information appended
** to the end.  EXTRA_SIZE is the number of bytes of space needed to hold
** that extra information.
*/
#define EXTRA_SIZE (sizeof(MemPage)-MNDB_PAGE_SIZE)

struct Btree{
  Pager *pPager;
  BtCursor *pCursor;
  PageOne *page1;
  int inTrans;
};

typedef Btree Bt;


/*
** A cursor is a pointer to a particular entry in the BTree.
** The entry is identified by its MemPage and the index in
** MemPage.apCell[] of the entry.
*/
struct BtCursor {
  Btree *pBt;               /* The Btree to which this cursor belongs */
  BtCursor *pNext, *pPrev;  /* Forms a linked list of all cursors */
  Pgno pgnoRoot;            /* The root page of this tree */
  MemPage *pPage;           /* Page that contains the entry */
  int idx;                  /* Index of the entry in pPage->apCell[] */
  u8 bSkipNext;             /* mndbBtreeNext() is no-op if true */
  u8 iMatch;                /* compare result from last mndbBtreeMoveto() */
};

/*
** Compute the total number of bytes that a Cell needs on the main
** database page.  The number returned includes the Cell header,
** local payload storage, and the pointer to overflow pages (if
** applicable).  Additional space allocated on overflow pages
** is NOT included in the value returned from this routine.
*/
static int cellSize(Cell *pCell){
  int n = pCell->h.nKey + pCell->h.nData;
  if( n>MX_LOCAL_PAYLOAD ){
    n = MX_LOCAL_PAYLOAD + sizeof(Pgno);
  }else{
    n = ROUNDUP(n);
  }
  n += sizeof(CellHdr);
  return n;
}

static void defragmentPage(MemPage *pPage){
  int pc, i ,n;
  FreeBlk *pFBlk;
  char newPage[MNDB_PAGE_SIZE];
  pc = sizeof(PageHdr);
  pPage->u.hdr.firstCell = pc;
  memcpy(newPage, pPage->u.aDisk,MNDB_PAGE_SIZE);
  for(i = 0; i < pPage->nCell; ++i){
    Cell *pCell = pPage->apCell[i];
    /* This routine should never be called on an overfull page.  The
    ** following asserts verify that constraint. */
    assert( Addr(pCell) > Addr(pPage) );
    assert( Addr(pCell) < Addr(pPage) + MNDB_PAGE_SIZE );
    
    n = cellSize(pCell);
    pCell->h.iNext = pc + n;
    memcpy(&newPage[pc], pCell, n);
    pPage->apCell[i] = (Cell*)&pPage->u.aDisk[pc];
    pc += n;
  }
  assert( pPage->nFree==MNDB_PAGE_SIZE-pc );
  memcpy(pPage->u.aDisk, newPage, pc);
  if( pPage->nCell>0 ){
    pPage->apCell[pPage->nCell-1]->h.iNext = 0;
  }
  pFBlk = (FreeBlk*)&pPage->u.aDisk[pc];
  pFBlk->iSize = MNDB_PAGE_SIZE - pc;
  pFBlk->iNext = 0;
  pPage->u.hdr.firstFree = pc;
  memset(&pFBlk[1], 0, MNDB_PAGE_SIZE - pc - sizeof(FreeBlk));
}


/*
** Allocate nByte bytes of space on a page.  nByte must be a 
** multiple of 4.
**
** Return the index into pPage->u.aDisk[] of the first byte of
** the new allocation. Or return 0 if there is not enough free
** space on the page to satisfy the allocation request.
**
** If the page contains nBytes of free space but does not contain
** nBytes of contiguous free space, then this routine automatically
** calls defragementPage() to consolidate all free space before 
** allocating the new chunk.
*/
static int allocateSpace(MemPage *pPage, int nByte){
  FreeBlk *p;
  u16 *pIdx;
  int start;
  int cnt = 0;

  assert( mndbpager_iswriteable(pPage) );
  assert( nByte==ROUNDUP(nByte) );
  if( pPage->nFree<nByte || pPage->isOverfull ) return 0;
  pIdx = &pPage->u.hdr.firstFree;
  p = (FreeBlk*)&pPage->u.aDisk[*pIdx];
  while( p->iSize<nByte ){
    assert( cnt++ < MNDB_PAGE_SIZE/4 );
    if( p->iNext==0 ){
      defragmentPage(pPage);
      pIdx = &pPage->u.hdr.firstFree;
    }else{
      pIdx = &p->iNext;
    }
    p = (FreeBlk*)&pPage->u.aDisk[*pIdx];
  }
  if( p->iSize==nByte ){
    start = *pIdx;
    *pIdx = p->iNext;
  }else{
    FreeBlk *pNew;
    start = *pIdx;
    pNew = (FreeBlk*)&pPage->u.aDisk[start + nByte];
    pNew->iNext = p->iNext;
    pNew->iSize = p->iSize - nByte;
    *pIdx = start + nByte;
  }
  pPage->nFree -= nByte;
  return start;
}

/*
** Return a section of the MemPage.u.aDisk[] to the freelist.
** The first byte of the new free block is pPage->u.aDisk[start]
** and the size of the block is "size" bytes.  Size must be
** a multiple of 4.
**
** Most of the effort here is involved in coalesing adjacent
** free blocks into a single big free block.
*/
static void freeSpace(MemPage *pPage, int start, int size){
  int end = start + size;
  u16 *pIdx, idx;
  FreeBlk *pFBlk;
  FreeBlk *pNew;
  FreeBlk *pNext;

  assert( mndbpager_iswriteable(pPage) );
  assert( size == ROUNDUP(size) );
  assert( start == ROUNDUP(start) );
  pIdx = &pPage->u.hdr.firstFree;
  idx = *pIdx;
  while( idx!=0 && idx<start ){
    pFBlk = (FreeBlk*)&pPage->u.aDisk[idx];
    if( idx + pFBlk->iSize == start ){
      pFBlk->iSize += size;
      if( idx + pFBlk->iSize == pFBlk->iNext ){
        pNext = (FreeBlk*)&pPage->u.aDisk[pFBlk->iNext];
        pFBlk->iSize += pNext->iSize;
        pFBlk->iNext = pNext->iNext;
      }
      pPage->nFree += size;
      return;
    }
    pIdx = &pFBlk->iNext;
    idx = *pIdx;
  }
  pNew = (FreeBlk*)&pPage->u.aDisk[start];
  if( idx != end ){
    pNew->iSize = size;
    pNew->iNext = idx;
  }else{
    pNext = (FreeBlk*)&pPage->u.aDisk[idx];
    pNew->iSize = size + pNext->iSize;
    pNew->iNext = pNext->iNext;
  }
  *pIdx = start;
  pPage->nFree += size;
}



/*
** Initialize the auxiliary information for a disk block.
**
** The pParent parameter must be a pointer to the MemPage which
** is the parent of the page being initialized.  The root of the
** BTree (usually page 2) has no parent and so for that page, 
** pParent==NULL.
**
** Return MNDB_OK on success.  If we see that the page does
** not contained a well-formed database page, then return 
** MNDB_CORRUPT.  Note that a return of MNDB_OK does not
** guarantee that the page is well-formed.  It only shows that
** we failed to detect any corruption.
*/
static int initPage(MemPage *pPage, Pgno pgnoThis, MemPage *pParent){
  int idx;           /* An index into pPage->u.aDisk[] */
  Cell *pCell;       /* A pointer to a Cell in pPage->u.aDisk[] */
  FreeBlk *pFBlk;    /* A pointer to a free block in pPage->u.aDisk[] */
  int sz;            /* The size of a Cell in bytes */
  int freeSpace;     /* Amount of free space on the page */

  if( pPage->pParent ){
    assert( pPage->pParent==pParent );
    return MNDB_OK;
  }
  if( pParent ){
    pPage->pParent = pParent;
    mndbpager_ref(pParent);
  }
  if( pPage->isInit ) return MNDB_OK;
  pPage->isInit = 1;
  pPage->nCell = 0;
  freeSpace = USABLE_SPACE;
  idx = pPage->u.hdr.firstCell;
  while( idx!=0 ){
    if( idx>MNDB_PAGE_SIZE-MIN_CELL_SIZE ) goto page_format_error;
    if( idx<sizeof(PageHdr) ) goto page_format_error;
    if( idx!=ROUNDUP(idx) ) goto page_format_error;
    pCell = (Cell*)&pPage->u.aDisk[idx];
    sz = cellSize(pCell);
    if( idx+sz > MNDB_PAGE_SIZE ) goto page_format_error;
    freeSpace -= sz;
    pPage->apCell[pPage->nCell++] = pCell;
    idx = pCell->h.iNext;
  }
  pPage->nFree = 0;
  idx = pPage->u.hdr.firstFree;
  while( idx!=0 ){
    if( idx>MNDB_PAGE_SIZE-sizeof(FreeBlk) ) goto page_format_error;
    if( idx<sizeof(PageHdr) ) goto page_format_error;
    pFBlk = (FreeBlk*)&pPage->u.aDisk[idx];
    pPage->nFree += pFBlk->iSize;
    if( pFBlk->iNext>0 && pFBlk->iNext <= idx ) goto page_format_error;
    idx = pFBlk->iNext;
  }
  if( pPage->nCell==0 && pPage->nFree==0 ){
    /* As a special case, an uninitialized root page appears to be
    ** an empty database */
    return MNDB_OK;
  }
  if( pPage->nFree!=freeSpace ) goto page_format_error;
  return MNDB_OK;

page_format_error:
  return MNDB_CORRUPT;
}

/*
** Set up a raw page so that it looks like a database page holding
** no entries.
*/
static void zeroPage(MemPage *pPage){
  PageHdr *pHdr;
  FreeBlk *pFBlk;
  assert( mndbpager_iswriteable(pPage) );
  memset(pPage, 0, MNDB_PAGE_SIZE);
  pHdr = &pPage->u.hdr;
  pHdr->firstCell = 0;
  pHdr->firstFree = sizeof(*pHdr);
  pFBlk = (FreeBlk*)&pHdr[1];
  pFBlk->iNext = 0;
  pFBlk->iSize = MNDB_PAGE_SIZE - sizeof(*pHdr);
  pPage->nFree = pFBlk->iSize;
  pPage->nCell = 0;
  pPage->isOverfull = 0;
}

/*
** This routine is called when the reference count for a page
** reaches zero.  We need to unref the pParent pointer when that
** happens.
*/
static void pageDestructor(void *pData){
  MemPage *pPage = (MemPage*)pData;
  if( pPage->pParent ){
    MemPage *pParent = pPage->pParent;
    pPage->pParent = 0;
    mndbpager_unref(pParent);
  }
}

/*
** Open a new database.
**
** Actually, this routine just sets up the internal data structures
** for accessing the database.  We do not open the database file 
** until the first page is loaded.
*/
int mndbBtreeOpen(
  const char *zFilename,    /* Name of the file containing the BTree database */
  int nCache,               /* How many pages in the page cache */
  Btree **ppBtree           /* Pointer to new Btree object written here */
){
  Btree *pBt;
  int rc;

  pBt = mndbMalloc( sizeof(*pBt) );
  if( pBt==0 ){
    *ppBtree = 0;
    return MNDB_NOMEM;
  }
  if( nCache<10 ) nCache = 10;
  rc = mndbpager_open(&pBt->pPager, zFilename, nCache, EXTRA_SIZE);
  if( rc!=MNDB_OK ){
    if( pBt->pPager ) mndbpager_close(pBt->pPager);
    mndbFree(pBt);
    *ppBtree = 0;
    return rc;
  }
  mndbpager_set_destructor(pBt->pPager, pageDestructor);
  pBt->pCursor = 0;
  pBt->page1 = 0;
  *ppBtree = pBt;
  return MNDB_OK;
}

/*
** Close an open database and invalidate all cursors.
*/
int mndbBtreeClose(Btree *pBt){
  while( pBt->pCursor ){
    mndbBtreeCloseCursor(pBt->pCursor);
  }
  mndbpager_close(pBt->pPager);
  mndbFree(pBt);
  return MNDB_OK;
}

/*
** Change the number of pages in the cache.
*/
 //int mndbBtreeSetCacheSize(Btree *pBt, int mxPage){
  // mndbpager_set_cachesize(pBt->pPager, mxPage);
  // return MNDB_OK;
  //}

/*
** Get a reference to page1 of the database file.  This will
** also acquire a readlock on that file.
**
** MNDB_OK is returned on success.  If the file is not a
** well-formed database file, then MNDB_CORRUPT is returned.
** MNDB_BUSY is returned if the database is locked.  MNDB_NOMEM
** is returned if we run out of memory.  MNDB_PROTOCOL is returned
** if there is a locking protocol violation.
*/
static int lockBtree(Btree *pBt){
  int rc;
  if( pBt->page1 ) return MNDB_OK;
  rc = mndbpager_get(pBt->pPager, 1, (void**)&pBt->page1);
  return rc;
}

/*
** Create a new database by initializing the first two pages of the
** file.
*/
static int newDatabase(Btree *pBt){
  MemPage *pRoot;
  int rc;
  if( mndbpager_pagecount(pBt->pPager)>1 ) return MNDB_OK;
  rc = mndbpager_write(pBt->page1);
  if( rc ) return rc;
  rc = mndbpager_get(pBt->pPager, 2, (void**)&pRoot);
  if( rc ) return rc;
  rc = mndbpager_write(pRoot);
  if( rc ){
    mndbpager_unref(pRoot);
    return rc;
  }
  zeroPage(pRoot);
  mndbpager_unref(pRoot);
  return MNDB_OK;
}


/*
** Attempt to start a new transaction./ 
**
** A transaction must be started before attempting any changes
** to the database.  None of the following routines will work
** unless a transaction is started first:
**
**      mndbBtreeCreateTable()
**      mndbBtreeClearTable()
**      mndbBtreeDropTable()
**      mndbBtreeInsert()
**      mndbBtreeDelete()
*/
int mndbBtreeBeginTrans(Btree *pBt){
  int rc;
  if( pBt->inTrans ) return MNDB_ERROR;
  if( pBt->page1==0 ){
    rc = lockBtree(pBt);
    if( rc!=MNDB_OK ){
      return rc;
    }
  }
  if( !mndbpager_isreadonly(pBt->pPager) ){
    rc = mndbpager_begin(pBt->page1);
    if( rc!=MNDB_OK ){
      return rc;
    }
    rc = mndbpager_write(pBt->page1);
    if(rc != MNDB_OK){
      return rc;
    }
    rc = newDatabase(pBt);
  }
  pBt->inTrans = 1;
  return rc;
}

/*
** If there are no outstanding cursors and we are not in the middle
** of a transaction but there is a read lock on the database, then
** this routine unrefs the first page of the database file which 
** has the effect of releasing the read lock.
**
** If there are any outstanding cursors, this routine is a no-op.
**
** If there is a transaction in progress, this routine is a no-op.
*/
static void unlockBtreeIfUnused(Btree *pBt){
  if( pBt->inTrans==0 && pBt->pCursor==0 && pBt->page1!=0 ){
    mndbpager_unref(pBt->page1);
    pBt->page1 = 0;
    pBt->inTrans = 0;
  }
}

/*
** Commit the transaction currently in progress.
**
** This will release the write lock on the database file.  If there
** are no active cursors, it also releases the read lock.
*/
int mndbBtreeCommit(Btree *pBt){
  int rc;
  if( pBt->inTrans==0 ) return MNDB_ERROR;
  rc = mndbpager_commit(pBt->pPager);
  pBt->inTrans = 0;
  unlockBtreeIfUnused(pBt);
  return rc;
}


/*
** Create a new cursor for the BTree whose root is on the page
** iTable.  The act of acquiring a cursor gets a read lock on 
** the database file.
*/
int mndbBtreeCursor(Btree *pBt, int iTable, BtCursor **ppCur){
  int rc;
  BtCursor *pCur;
  if( pBt->page1==0 ){
    rc = lockBtree(pBt);
    if( rc!=MNDB_OK ){
      *ppCur = 0;
      return rc;
    }
  }
  pCur = mndbMalloc( sizeof(*pCur) );
  if( pCur==0 ){
    rc = MNDB_NOMEM;
    goto create_cursor_exception;
  }
  pCur->pgnoRoot = (Pgno)iTable;
  rc = mndbpager_get(pBt->pPager, pCur->pgnoRoot, (void**)&pCur->pPage);
  if( rc!=MNDB_OK ){
    goto create_cursor_exception;
  }
  rc = initPage(pCur->pPage, pCur->pgnoRoot, 0);
  if( rc!=MNDB_OK ){
    goto create_cursor_exception;
  }
  pCur->pBt = pBt;
  pCur->idx = 0;
  pCur->pNext = pBt->pCursor;
  if( pCur->pNext ){
    pCur->pNext->pPrev = pCur;
  }
  pCur->pPrev = 0;
  pBt->pCursor = pCur;
  *ppCur = pCur;
  return MNDB_OK;

create_cursor_exception:
  *ppCur = 0;
  if( pCur ){
    if( pCur->pPage ) mndbpager_unref(pCur->pPage);
    mndbFree(pCur);
  }
  unlockBtreeIfUnused(pBt);
  return rc;
}

/*
** Close a cursor.  The read lock on the database file is released
** when the last cursor is closed.
*/
int mndbBtreeCloseCursor(BtCursor *pCur){
  Btree *pBt = pCur->pBt;
  if( pCur->pPrev ){
    pCur->pPrev->pNext = pCur->pNext;
  }else{
    pBt->pCursor = pCur->pNext;
  }
  if( pCur->pNext ){
    pCur->pNext->pPrev = pCur->pPrev;
  }
  mndbpager_unref(pCur->pPage);
  unlockBtreeIfUnused(pBt);
  mndbFree(pCur);
  return MNDB_OK;
}

/*
** Make a temporary cursor by filling in the fields of pTempCur.
** The temporary cursor is not on the cursor list for the Btree.
*/
static void getTempCursor(BtCursor *pCur, BtCursor *pTempCur){
  memcpy(pTempCur, pCur, sizeof(*pCur));
  pTempCur->pNext = 0;
  pTempCur->pPrev = 0;
  mndbpager_ref(pTempCur->pPage);
}

/*
** Delete a temporary cursor such as was made by the CreateTemporaryCursor()
** function above.
*/
static void releaseTempCursor(BtCursor *pCur){
  mndbpager_unref(pCur->pPage);
}

/*
** Set *pSize to the number of bytes of key in the entry the
** cursor currently points to.  Always return MNDB_OK.
** Failure is not possible.  If the cursor is not currently
** pointing to an entry (which can happen, for example, if
** the database is empty) then *pSize is set to 0.
*/
int mndbBtreeKeySize(BtCursor *pCur, int *pSize){
  Cell *pCell;
  MemPage *pPage;

  pPage = pCur->pPage;
  assert( pPage!=0 );
  if( pCur->idx >= pPage->nCell ){
    *pSize = 0;
  }else{
    pCell = pPage->apCell[pCur->idx];
    *pSize = pCell->h.nKey;
  }
  return MNDB_OK;
}

/*
** Read payload information from the entry that the pCur cursor is
** pointing to.  Begin reading the payload at "offset" and read
** a total of "amt" bytes.  Put the result in zBuf.
**
** This routine does not make a distinction between key and data.
** It just reads bytes from the payload area.
*/
static int getPayload(BtCursor *pCur, int offset, int amt, char *zBuf){
  char *aPayload;
  Pgno nextPage;
  int rc;
  assert( pCur!=0 && pCur->pPage!=0 );
  assert( pCur->idx>=0 && pCur->idx<pCur->pPage->nCell );
  aPayload = pCur->pPage->apCell[pCur->idx]->aPayload;
  if( offset<MX_LOCAL_PAYLOAD ){
    int a = amt;
    if( a+offset>MX_LOCAL_PAYLOAD ){
      a = MX_LOCAL_PAYLOAD - offset;
    }
    memcpy(zBuf, &aPayload[offset], a);
    if( a==amt ){
      return MNDB_OK;
    }
    offset = 0;
    zBuf += a;
    amt -= a;
  }else{
    offset -= MX_LOCAL_PAYLOAD;
  }
  if( amt>0 ){
    nextPage = pCur->pPage->apCell[pCur->idx]->ovfl;
  }
  while( amt>0 && nextPage ){
    OverflowPage *pOvfl;
    rc = mndbpager_get(pCur->pBt->pPager, nextPage, (void**)&pOvfl);
    if( rc!=0 ){
      return rc;
    }
    nextPage = pOvfl->iNext;
    if( offset<OVERFLOW_SIZE ){
      int a = amt;
      if( a + offset > OVERFLOW_SIZE ){
        a = OVERFLOW_SIZE - offset;
      }
      memcpy(zBuf, &pOvfl->aPayload[offset], a);
      offset = 0;
      amt -= a;
      zBuf += a;
    }else{
      offset -= OVERFLOW_SIZE;
    }
    mndbpager_unref(pOvfl);
  }
  return amt==0 ? MNDB_OK : MNDB_CORRUPT;
}

/*
** Read part of the key associated with cursor pCur.  A maximum
** of "amt" bytes will be transfered into zBuf[].  The transfer
** begins at "offset".  The number of bytes actually read is
** returned.  The amount returned will be smaller than the
** amount requested if there are not enough bytes in the key
** to satisfy the request.
*/
int mndbBtreeKey(BtCursor *pCur, int offset, int amt, char *zBuf){
  Cell *pCell;
  MemPage *pPage;

  if( amt<0 ) return 0;
  if( offset<0 ) return 0; 
  if( amt==0 ) return 0;
  pPage = pCur->pPage;
  assert( pPage!=0 );
  if( pCur->idx >= pPage->nCell ){
    return 0;
  }
  pCell = pPage->apCell[pCur->idx];
  if( amt+offset > pCell->h.nKey ){
    amt = pCell->h.nKey - offset;
    if( amt<=0 ){
      return 0;
    }
  }
  getPayload(pCur, offset, amt, zBuf);
  return amt;
}

/*
** Set *pSize to the number of bytes of data in the entry the
** cursor currently points to.  Always return MNDB_OK.
** Failure is not possible.  If the cursor is not currently
** pointing to an entry (which can happen, for example, if
** the database is empty) then *pSize is set to 0.
*/
int mndbBtreeDataSize(BtCursor *pCur, int *pSize){
  Cell *pCell;
  MemPage *pPage;

  pPage = pCur->pPage;
  assert( pPage!=0 );
  if( pCur->idx >= pPage->nCell ){
    *pSize = 0;
  }else{
    pCell = pPage->apCell[pCur->idx];
    *pSize = pCell->h.nData;
  }
  return MNDB_OK;
}

/*
** Read part of the data associated with cursor pCur.  A maximum
** of "amt" bytes will be transfered into zBuf[].  The transfer
** begins at "offset".  The number of bytes actually read is
** returned.  The amount returned will be smaller than the
** amount requested if there are not enough bytes in the data
** to satisfy the request.
*/
int mndbBtreeData(BtCursor *pCur, int offset, int amt, char *zBuf){
  Cell *pCell;
  MemPage *pPage;

  if( amt<0 ) return 0;
  if( offset<0 ) return 0;
  if( amt==0 ) return 0;
  pPage = pCur->pPage;
  assert( pPage!=0 );
  if( pCur->idx >= pPage->nCell ){
    return 0;
  }
  pCell = pPage->apCell[pCur->idx];
  if( amt+offset > pCell->h.nData ){
    amt = pCell->h.nData - offset;
    if( amt<=0 ){
      return 0;
    }
  }
  getPayload(pCur, offset + pCell->h.nKey, amt, zBuf);
  return amt;
}

/*
** Compare the key for the entry that pCur points to against the 
** given key (pKey,nKeyOrig).  Put the comparison result in *pResult.
** The result is negative if pCur<pKey, zero if they are equal and
** positive if pCur>pKey.
**
** MNDB_OK is returned on success.  If part of the cursor key
** is on overflow pages and we are unable to access those overflow
** pages, then some other value might be returned to indicate the
** reason for the error.
*/
static int compareKey(
  BtCursor *pCur,      /* Points to the entry against which we are comparing */
  const char *pKey,    /* The comparison key */
  int nKeyOrig,        /* Number of bytes in the comparison key */
  int *pResult         /* Write the comparison results here */
){
  Pgno nextPage;
  int nKey = nKeyOrig;
  int n, c, rc;
  Cell *pCell;

  assert( pCur->pPage );
  assert( pCur->idx>=0 && pCur->idx<pCur->pPage->nCell );
  pCell = pCur->pPage->apCell[pCur->idx];
  if( nKey > pCell->h.nKey ){
    nKey = pCell->h.nKey;
  }
  n = nKey;
  if( n>MX_LOCAL_PAYLOAD ){
    n = MX_LOCAL_PAYLOAD;
  }
  c = memcmp(pCell->aPayload, pKey, n);
  if( c!=0 ){
    *pResult = c;
    return MNDB_OK;
  }
  pKey += n;
  nKey -= n;
  nextPage = pCell->ovfl;
  while( nKey>0 ){
    OverflowPage *pOvfl;
    if( nextPage==0 ){
      return MNDB_CORRUPT;
    }
    rc = mndbpager_get(pCur->pBt->pPager, nextPage, (void**)&pOvfl);
    if( rc ){
      return rc;
    }
    nextPage = pOvfl->iNext;
    n = nKey;
    if( n>OVERFLOW_SIZE ){
      n = OVERFLOW_SIZE;
    }
    c = memcmp(pOvfl->aPayload, pKey, n);
    mndbpager_unref(pOvfl);
    if( c!=0 ){
      *pResult = c;
      return MNDB_OK;
    }
    nKey -= n;
    pKey += n;
  }
  c = pCell->h.nKey - nKeyOrig;
  *pResult = c;
  return MNDB_OK;
}

/*
** Move the cursor down to a new child page.
*/
static int moveToChild(BtCursor *pCur, int newPgno){
  int rc;
  MemPage *pNewPage;

  rc = mndbpager_get(pCur->pBt->pPager, newPgno, (void**)&pNewPage);
  if( rc ) return rc;
  rc = initPage(pNewPage, newPgno, pCur->pPage);
  if( rc ) return rc;
  mndbpager_unref(pCur->pPage);
  pCur->pPage = pNewPage;
  pCur->idx = 0;
  return MNDB_OK;
}

/*
** Move the cursor up to the parent page.
**
** pCur->idx is set to the cell index that contains the pointer
** to the page we are coming from.  If we are coming from the
** right-most child page then pCur->idx is set to one more than
** the largest cell index.
*/
static int moveToParent(BtCursor *pCur){
  Pgno oldPgno;
  MemPage *pParent;
  int i;
  pParent = pCur->pPage->pParent;
  if( pParent==0 ) return MNDB_INTERNAL;
  oldPgno = mndbpager_pagenumber(pCur->pPage);
  mndbpager_ref(pParent);
  mndbpager_unref(pCur->pPage);
  pCur->pPage = pParent;
  pCur->idx = pParent->nCell;
  for(i=0; i<pParent->nCell; i++){
    if( pParent->apCell[i]->h.leftChild==oldPgno ){
      pCur->idx = i;
      break;
    }
  }
  return MNDB_OK;
}

/*
** Move the cursor to the root page
*/
static int moveToRoot(BtCursor *pCur){
  MemPage *pNew;
  int rc;

  rc = mndbpager_get(pCur->pBt->pPager, pCur->pgnoRoot, (void**)&pNew);
  if( rc ) return rc;
  rc = initPage(pNew, pCur->pgnoRoot, 0);
  if( rc ) return rc;
  mndbpager_unref(pCur->pPage);
  pCur->pPage = pNew;
  pCur->idx = 0;
  return MNDB_OK;
}

/*
** Move the cursor down to the left-most leaf entry beneath the
** entry to which it is currently pointing.
*/
static int moveToLeftmost(BtCursor *pCur){
  Pgno pgno;
  int rc;

  while( (pgno = pCur->pPage->apCell[pCur->idx]->h.leftChild)!=0 ){
    rc = moveToChild(pCur, pgno);
    if( rc ) return rc;
  }
  return MNDB_OK;
}

/* Move the cursor to the first entry in the table.  Return MNDB_OK
** on success.  Set *pRes to 0 if the cursor actually points to something
** or set *pRes to 1 if the table is empty and there is no first element.
*/
int mndbBtreeFirst(BtCursor *pCur, int *pRes){
  int rc;
  rc = moveToRoot(pCur);
  if( rc ) return rc;
  if( pCur->pPage->nCell==0 ){
    *pRes = 1;
    return MNDB_OK;
  }
  *pRes = 0;
  rc = moveToLeftmost(pCur);
  return rc;
}

/* Move the cursor so that it points to an entry near pKey.
** Return a success code.
**
** If an exact match is not found, then the cursor is always
** left pointing at a leaf page which would hold the entry if it
** were present.  The cursor might point to an entry that comes
** before or after the key.
**
** The result of comparing the key with the entry to which the
** cursor is left pointing is stored in pCur->iMatch.  The same
** value is also written to *pRes if pRes!=NULL.  The meaning of
** this value is as follows:
**
**     *pRes<0      The cursor is left pointing at an entry that
**                  is smaller than pKey.
**
**     *pRes==0     The cursor is left pointing at an entry that
**                  exactly matches pKey.
**
**     *pRes>0      The cursor is left pointing at an entry that
**                  is larger than pKey.
*/
int mndbBtreeMoveto(BtCursor *pCur, const void *pKey, int nKey, int *pRes){
  int rc;
  pCur->bSkipNext = 0;
  rc = moveToRoot(pCur);
  if( rc ) return rc;
  for(;;){
    int lwr, upr;
    Pgno chldPg;
    MemPage *pPage = pCur->pPage;
    int c = -1;
    lwr = 0;
    upr = pPage->nCell-1;
    while( lwr<=upr ){
      pCur->idx = (lwr+upr)/2;
      rc = compareKey(pCur, pKey, nKey, &c);
      if( rc ) return rc;
      if( c==0 ){
        pCur->iMatch = c;
        if( pRes ) *pRes = 0;
        return MNDB_OK;
      }
      if( c<0 ){
        lwr = pCur->idx+1;
      }else{
        upr = pCur->idx-1;
      }
    }
    assert( lwr==upr+1 );
    if( lwr>=pPage->nCell ){
      chldPg = pPage->u.hdr.rightChild;
    }else{
      chldPg = pPage->apCell[lwr]->h.leftChild;
    }
    if( chldPg==0 ){
      pCur->iMatch = c;
      if( pRes ) *pRes = c;
      return MNDB_OK;
    }
    rc = moveToChild(pCur, chldPg);
    if( rc ) return rc;
  }
  /* NOT REACHED */
}

/*
** Advance the cursor to the next entry in the database.  If
** successful and pRes!=NULL then set *pRes=0.  If the cursor
** was already pointing to the last entry in the database before
** this routine was called, then set *pRes=1 if pRes!=NULL.
*/
int mndbBtreeNext(BtCursor *pCur, int *pRes){
  int rc;
  if( pCur->bSkipNext ){
    pCur->bSkipNext = 0;
    if( pRes ) *pRes = 0;
    return MNDB_OK;
  }
  pCur->idx++;
  if( pCur->idx>=pCur->pPage->nCell ){
    if( pCur->pPage->u.hdr.rightChild ){
      rc = moveToChild(pCur, pCur->pPage->u.hdr.rightChild);
      if( rc ) return rc;
      rc = moveToLeftmost(pCur);
      if( rc ) return rc;
      if( pRes ) *pRes = 0;
      return MNDB_OK;
    }
    do{
      if( pCur->pPage->pParent==0 ){
        if( pRes ) *pRes = 1;
        return MNDB_OK;
      }
      rc = moveToParent(pCur);
      if( rc ) return rc;
    }while( pCur->idx>=pCur->pPage->nCell );
    if( pRes ) *pRes = 0;
    return MNDB_OK;
  }
  rc = moveToLeftmost(pCur);
  if( rc ) return rc;
  if( pRes ) *pRes = 0;
  return MNDB_OK;
}

/*
** Allocate a new page from the database file.
**
** The new page is marked as dirty.  (In other words, mndbpager_write()
** has already been called on the new page.)  The new page has also
** been referenced and the calling routine is responsible for calling
** mndbpager_unref() on the new page when it is done.
**
** MNDB_OK is returned on success.  Any other return value indicates
** an error.  *ppPage and *pPgno are undefined in the event of an error.
** Do not invoke mndbpager_unref() on *ppPage if an error is returned.
*/
static int allocatePage(Btree *pBt, MemPage **ppPage, Pgno *pPgno){
  PageOne *pPage1 = pBt->page1;
  int rc;
  if( pPage1->freeList ){
    OverflowPage *pOvfl;
    rc = mndbpager_write(pPage1);
    if( rc ) return rc;
    *pPgno = pPage1->freeList;
    rc = mndbpager_get(pBt->pPager, pPage1->freeList, (void**)&pOvfl);
    if( rc ) return rc;
    rc = mndbpager_write(pOvfl);
    if( rc ){
      mndbpager_unref(pOvfl);
      return rc;
    }
    pPage1->freeList = pOvfl->iNext;
    pPage1->nFree--;
    *ppPage = (MemPage*)pOvfl;
  }else{
    *pPgno = mndbpager_pagecount(pBt->pPager) + 1;
    rc = mndbpager_get(pBt->pPager, *pPgno, (void**)ppPage);
    if( rc ) return rc;
    rc = mndbpager_write(*ppPage);
  }
  return rc;
}

/*
** Add a page of the database file to the freelist.  Either pgno or
** pPage but not both may be 0. 
**
** mndbpager_unref() is NOT called for pPage.
*/
static int freePage(Btree *pBt, void *pPage, Pgno pgno){
  PageOne *pPage1 = pBt->page1;
  OverflowPage *pOvfl = (OverflowPage*)pPage;
  int rc;
  int needUnref = 0;
  MemPage *pMemPage;

  if( pgno==0 ){
    assert( pOvfl!=0 );
    pgno = mndbpager_pagenumber(pOvfl);
  }
  assert( pgno>2 );
  rc = mndbpager_write(pPage1);
  if( rc ){
    return rc;
  }
  if( pOvfl==0 ){
    assert( pgno>0 );
    rc = mndbpager_get(pBt->pPager, pgno, (void**)&pOvfl);
    if( rc ) return rc;
    needUnref = 1;
  }
  rc = mndbpager_write(pOvfl);
  if( rc ){
    if( needUnref ) mndbpager_unref(pOvfl);
    return rc;
  }
  pOvfl->iNext = pPage1->freeList;
  pPage1->freeList = pgno;
  pPage1->nFree++;
  memset(pOvfl->aPayload, 0, OVERFLOW_SIZE);
  pMemPage = (MemPage*)pPage;
  pMemPage->isInit = 0;
  if( pMemPage->pParent ){
    mndbpager_unref(pMemPage->pParent);
    pMemPage->pParent = 0;
  }
  if( needUnref ) rc = mndbpager_unref(pOvfl);
  return rc;
}

/*
** Erase all the data out of a cell.  This involves returning overflow
** pages back the freelist.
*/
static int clearCell(Btree *pBt, Cell *pCell){
  Pager *pPager = pBt->pPager;
  OverflowPage *pOvfl;
  Pgno ovfl, nextOvfl;
  int rc;

  if( pCell->h.nKey + pCell->h.nData <= MX_LOCAL_PAYLOAD ){
    return MNDB_OK;
  }
  ovfl = pCell->ovfl;
  pCell->ovfl = 0;
  while( ovfl ){
    rc = mndbpager_get(pPager, ovfl, (void**)&pOvfl);
    if( rc ) return rc;
    nextOvfl = pOvfl->iNext;
    rc = freePage(pBt, pOvfl, ovfl);
    if( rc ) return rc;
    mndbpager_unref(pOvfl);
    ovfl = nextOvfl;
  }
  return MNDB_OK;
}

/*
** Create a new cell from key and data.  Overflow pages are allocated as
** necessary and linked to this cell.  
*/
static int fillInCell(
  Btree *pBt,              /* The whole Btree.  Needed to allocate pages */
  Cell *pCell,             /* Populate this Cell structure */
  const void *pKey, int nKey,    /* The key */
  const void *pData,int nData    /* The data */
){
  OverflowPage *pOvfl, *pPrior;
  Pgno *pNext;
  int spaceLeft;
  int n, rc;
  int nPayload;
  const char *pPayload;
  char *pSpace;

  pCell->h.leftChild = 0;
  pCell->h.nKey = nKey;
  pCell->h.nData = nData;
  pCell->h.iNext = 0;

  pNext = &pCell->ovfl;
  pSpace = pCell->aPayload;
  spaceLeft = MX_LOCAL_PAYLOAD;
  pPayload = pKey;
  pKey = 0;
  nPayload = nKey;
  pPrior = 0;
  while( nPayload>0 ){
    if( spaceLeft==0 ){
      rc = allocatePage(pBt, (MemPage**)&pOvfl, pNext);
      if( rc ){
        *pNext = 0;
      }
      if( pPrior ) mndbpager_unref(pPrior);
      if( rc ){
        clearCell(pBt, pCell);
        return rc;
      }
      pPrior = pOvfl;
      spaceLeft = OVERFLOW_SIZE;
      pSpace = pOvfl->aPayload;
      pNext = &pOvfl->iNext;
    }
    n = nPayload;
    if( n>spaceLeft ) n = spaceLeft;
    memcpy(pSpace, pPayload, n);
    nPayload -= n;
    if( nPayload==0 && pData ){
      pPayload = pData;
      nPayload = nData;
      pData = 0;
    }else{
      pPayload += n;
    }
    spaceLeft -= n;
    pSpace += n;
  }
  *pNext = 0;
  if( pPrior ){
    mndbpager_unref(pPrior);
  }
  return MNDB_OK;
}

/*
** Change the MemPage.pParent pointer on the page whose number is
** given in the second argument so that MemPage.pParent holds the
** pointer in the third argument.
*/
static void reparentPage(Pager *pPager, Pgno pgno, MemPage *pNewParent){
  MemPage *pThis;

  if( pgno==0 ) return;
  assert( pPager!=0 );
  pThis = mndbpager_lookup(pPager, pgno);
  if( pThis && pThis->isInit ){
    if( pThis->pParent!=pNewParent ){
      if( pThis->pParent ) mndbpager_unref(pThis->pParent);
      pThis->pParent = pNewParent;
      if( pNewParent ) mndbpager_ref(pNewParent);
    }
    mndbpager_unref(pThis);
  }
}

/*
** Reparent all children of the given page to be the given page.
** In other words, for every child of pPage, invoke reparentPage()
** to make sure that each child knows that pPage is its parent.
**
** This routine gets called after you memcpy() one page into
** another.
*/
static void reparentChildPages(Pager *pPager, MemPage *pPage){
  int i;
  for(i=0; i<pPage->nCell; i++){
    reparentPage(pPager, pPage->apCell[i]->h.leftChild, pPage);
  }
  reparentPage(pPager, pPage->u.hdr.rightChild, pPage);
}

/*
** Remove the i-th cell from pPage.  This routine effects pPage only.
** The cell content is not freed or deallocated.  It is assumed that
** the cell content has been copied someplace else.  This routine just
** removes the reference to the cell from pPage.
**
** "sz" must be the number of bytes in the cell.
**
** Do not bother maintaining the integrity of the linked list of Cells.
** Only the pPage->apCell[] array is important.  The relinkCellList() 
** routine will be called soon after this routine in order to rebuild 
** the linked list.
*/
static void dropCell(MemPage *pPage, int idx, int sz){
  int j;
  assert( idx>=0 && idx<pPage->nCell );
  assert( sz==cellSize(pPage->apCell[idx]) );
  assert( mndbpager_iswriteable(pPage) );
  freeSpace(pPage, Addr(pPage->apCell[idx]) - Addr(pPage), sz);
  for(j=idx; j<pPage->nCell-1; j++){
    pPage->apCell[j] = pPage->apCell[j+1];
  }
  pPage->nCell--;
}

/*
** Insert a new cell on pPage at cell index "i".  pCell points to the
** content of the cell.
**
** If the cell content will fit on the page, then put it there.  If it
** will not fit, then just make pPage->apCell[i] point to the content
** and set pPage->isOverfull.  
**
** Do not bother maintaining the integrity of the linked list of Cells.
** Only the pPage->apCell[] array is important.  The relinkCellList() 
** routine will be called soon after this routine in order to rebuild 
** the linked list.
*/
static void insertCell(MemPage *pPage, int i, Cell *pCell, int sz){
  int idx, j;
  assert( i>=0 && i<=pPage->nCell );
  assert( sz==cellSize(pCell) );
  assert( mndbpager_iswriteable(pPage) );
  idx = allocateSpace(pPage, sz);
  for(j=pPage->nCell; j>i; j--){
    pPage->apCell[j] = pPage->apCell[j-1];
  }
  pPage->nCell++;
  if( idx<=0 ){
    pPage->isOverfull = 1;
    pPage->apCell[i] = pCell;
  }else{
    memcpy(&pPage->u.aDisk[idx], pCell, sz);
    pPage->apCell[i] = (Cell*)&pPage->u.aDisk[idx];
  }
}

/*
** Rebuild the linked list of cells on a page so that the cells
** occur in the order specified by the pPage->apCell[] array.  
** Invoke this routine once to repair damage after one or more
** invocations of either insertCell() or dropCell().
*/
static void relinkCellList(MemPage *pPage){
  int i;
  u16 *pIdx;
  assert( mndbpager_iswriteable(pPage) );
  pIdx = &pPage->u.hdr.firstCell;
  for(i=0; i<pPage->nCell; i++){
    int idx = Addr(pPage->apCell[i]) - Addr(pPage);
    assert( idx>0 && idx<MNDB_PAGE_SIZE );
    *pIdx = idx;
    pIdx = &pPage->apCell[i]->h.iNext;
  }
  *pIdx = 0;
}

/*
** Make a copy of the contents of pFrom into pTo.  The pFrom->apCell[]
** pointers that point into pFrom->u.aDisk[] must be adjusted to point
** into pTo->u.aDisk[] instead.  But some pFrom->apCell[] entries might
** not point to pFrom->u.aDisk[].  Those are unchanged.
*/
static void copyPage(MemPage *pTo, MemPage *pFrom){
  uptr from, to;
  int i;
  memcpy(pTo->u.aDisk, pFrom->u.aDisk, MNDB_PAGE_SIZE);
  pTo->pParent = 0;
  pTo->isInit = 1;
  pTo->nCell = pFrom->nCell;
  pTo->nFree = pFrom->nFree;
  pTo->isOverfull = pFrom->isOverfull;
  to = Addr(pTo);
  from = Addr(pFrom);
  for(i=0; i<pTo->nCell; i++){
    uptr x = Addr(pFrom->apCell[i]);
    if( x>from && x<from+MNDB_PAGE_SIZE ){
      *((uptr*)&pTo->apCell[i]) = x + to - from;
    }else{
      pTo->apCell[i] = pFrom->apCell[i];
    }
  }
}

/*
** This routine redistributes Cells on pPage and up to two siblings
** of pPage so that all pages have about the same amount of free space.
** Usually one sibling on either side of pPage is used in the balancing,
** though both siblings might come from one side if pPage is the first
** or last child of its parent.  If pPage has fewer than two siblings
** (something which can only happen if pPage is the root page or a 
** child of root) then all available siblings participate in the balancing.
**
** The number of siblings of pPage might be increased or decreased by
** one in an effort to keep pages between 66% and 100% full. The root page
** is special and is allowed to be less than 66% full. If pPage is 
** the root page, then the depth of the tree might be increased
** or decreased by one, as necessary, to keep the root page from being
** overfull or empty.
**
** This routine calls relinkCellList() on its input page regardless of
** whether or not it does any real balancing.  Client routines will typically
** invoke insertCell() or dropCell() before calling this routine, so we
** need to call relinkCellList() to clean up the mess that those other
** routines left behind.
**
** pCur is left pointing to the same cell as when this routine was called
** even if that cell gets moved to a different page.  pCur may be NULL.
** Set the pCur parameter to NULL if you do not care about keeping track
** of a cell as that will save this routine the work of keeping track of it.
**
** Note that when this routine is called, some of the Cells on pPage
** might not actually be stored in pPage->u.aDisk[].  This can happen
** if the page is overfull.  Part of the job of this routine is to
** make sure all Cells for pPage once again fit in pPage->u.aDisk[].
**
** In the course of balancing the siblings of pPage, the parent of pPage
** might become overfull or underfull.  If that happens, then this routine
** is called recursively on the parent.
**
** If this routine fails for any reason, it might leave the database
** in a corrupted state.  So if this routine fails, the database should
** be rolled back.
*/
static int balance(Btree *pBt, MemPage *pPage, BtCursor *pCur){
  MemPage *pParent;            /* The parent of pPage */
  MemPage *apOld[3];           /* pPage and up to two siblings */
  Pgno pgnoOld[3];             /* Page numbers for each page in apOld[] */
  MemPage *apNew[4];           /* pPage and up to 3 siblings after balancing */
  Pgno pgnoNew[4];             /* Page numbers for each page in apNew[] */
  int idxDiv[3];               /* Indices of divider cells in pParent */
  Cell *apDiv[3];              /* Divider cells in pParent */
  int nCell;                   /* Number of cells in apCell[] */
  int nOld;                    /* Number of pages in apOld[] */
  int nNew;                    /* Number of pages in apNew[] */
  int nDiv;                    /* Number of cells in apDiv[] */
  int i, j, k;                 /* Loop counters */
  int idx;                     /* Index of pPage in pParent->apCell[] */
  int nxDiv;                   /* Next divider slot in pParent->apCell[] */
  int rc;                      /* The return code */
  int iCur;                    /* apCell[iCur] is the cell of the cursor */
  MemPage *pOldCurPage;        /* The cursor originally points to this page */
  int totalSize;               /* Total bytes for all cells */
  int subtotal;                /* Subtotal of bytes in cells on one page */
  int cntNew[4];               /* Index in apCell[] of cell after i-th page */
  int szNew[4];                /* Combined size of cells place on i-th page */
  MemPage *extraUnref = 0;     /* A page that needs to be unref-ed */
  Pgno pgno;                   /* Page number */
  Cell *apCell[MX_CELL*3+5];   /* All cells from pages being balanceed */
  int szCell[MX_CELL*3+5];     /* Local size of all cells */
  Cell aTemp[2];               /* Temporary holding area for apDiv[] */
  MemPage aOld[3];             /* Temporary copies of pPage and its siblings */

  /* 
  ** Return without doing any work if pPage is neither overfull nor
  ** underfull.
  */
  assert( mndbpager_iswriteable(pPage) );
  if( !pPage->isOverfull && pPage->nFree<MNDB_PAGE_SIZE/2 
        && pPage->nCell>=2){
    relinkCellList(pPage);
    return MNDB_OK;
  }

  /*
  ** Find the parent of the page to be balanceed.
  ** If there is no parent, it means this page is the root page and
  ** special rules apply.
  */
  pParent = pPage->pParent;
  if( pParent==0 ){
    Pgno pgnoChild;
    MemPage *pChild;
    if( pPage->nCell==0 ){
      if( pPage->u.hdr.rightChild ){
        /*
        ** The root page is empty.  Copy the one child page
        ** into the root page and return.  This reduces the depth
        ** of the BTree by one.
        */
        pgnoChild = pPage->u.hdr.rightChild;
        rc = mndbpager_get(pBt->pPager, pgnoChild, (void**)&pChild);
        if( rc ) return rc;
        memcpy(pPage, pChild, MNDB_PAGE_SIZE);
        pPage->isInit = 0;
        rc = initPage(pPage, mndbpager_pagenumber(pPage), 0);
        assert( rc==MNDB_OK );
        reparentChildPages(pBt->pPager, pPage);
        if( pCur && pCur->pPage==pChild ){
          mndbpager_unref(pChild);
          pCur->pPage = pPage;
          mndbpager_ref(pPage);
        }
        freePage(pBt, pChild, pgnoChild);
        mndbpager_unref(pChild);
      }else{
        relinkCellList(pPage);
      }
      return MNDB_OK;
    }
    if( !pPage->isOverfull ){
      /* It is OK for the root page to be less than half full.
      */
      relinkCellList(pPage);
      return MNDB_OK;
    }
    /*
    ** If we get to here, it means the root page is overfull.
    ** When this happens, Create a new child page and copy the
    ** contents of the root into the child.  Then make the root
    ** page an empty page with rightChild pointing to the new
    ** child.  Then fall thru to the code below which will cause
    ** the overfull child page to be split.
    */
    rc = mndbpager_write(pPage);
    if( rc ) return rc;
    rc = allocatePage(pBt, &pChild, &pgnoChild);
    if( rc ) return rc;
    assert( mndbpager_iswriteable(pChild) );
    copyPage(pChild, pPage);
    pChild->pParent = pPage;
    mndbpager_ref(pPage);
    pChild->isOverfull = 1;
    if( pCur && pCur->pPage==pPage ){
      mndbpager_unref(pPage);
      pCur->pPage = pChild;
    }else{
      extraUnref = pChild;
    }
    zeroPage(pPage);
    pPage->u.hdr.rightChild = pgnoChild;
    pParent = pPage;
    pPage = pChild;
  }
  rc = mndbpager_write(pParent);
  if( rc ) return rc;
  
  /*
  ** Find the Cell in the parent page whose h.leftChild points back
  ** to pPage.  The "idx" variable is the index of that cell.  If pPage
  ** is the rightmost child of pParent then set idx to pParent->nCell 
  */
  idx = -1;
  pgno = mndbpager_pagenumber(pPage);
  for(i=0; i<pParent->nCell; i++){
    if( pParent->apCell[i]->h.leftChild==pgno ){
      idx = i;
      break;
    }
  }
  if( idx<0 && pParent->u.hdr.rightChild==pgno ){
    idx = pParent->nCell;
  }
  if( idx<0 ){
    return MNDB_CORRUPT;
  }

  /*
  ** Initialize variables so that it will be safe to jump
  ** directly to balance_cleanup at any moment.
  */
  nOld = nNew = 0;
  mndbpager_ref(pParent);

  /*
  ** Find sibling pages to pPage and the Cells in pParent that divide
  ** the siblings.  An attempt is made to find one sibling on either
  ** side of pPage.  Both siblings are taken from one side, however, if
  ** pPage is either the first or last child of its parent.  If pParent
  ** has 3 or fewer children then all children of pParent are taken.
  */
  if( idx==pParent->nCell ){
    nxDiv = idx - 2;
  }else{
    nxDiv = idx - 1;
  }
  if( nxDiv<0 ) nxDiv = 0;
  nDiv = 0;
  for(i=0, k=nxDiv; i<3; i++, k++){
    if( k<pParent->nCell ){
      idxDiv[i] = k;
      apDiv[i] = pParent->apCell[k];
      nDiv++;
      pgnoOld[i] = apDiv[i]->h.leftChild;
    }else if( k==pParent->nCell ){
      pgnoOld[i] = pParent->u.hdr.rightChild;
    }else{
      break;
    }
    rc = mndbpager_get(pBt->pPager, pgnoOld[i], (void**)&apOld[i]);
    if( rc ) goto balance_cleanup;
    rc = initPage(apOld[i], pgnoOld[i], pParent);
    if( rc ) goto balance_cleanup;
    nOld++;
  }

  /*
  ** Set iCur to be the index in apCell[] of the cell that the cursor
  ** is pointing to.  We will need this later on in order to keep the
  ** cursor pointing at the same cell.  If pCur points to a page that
  ** has no involvement with this rebalancing, then set iCur to a large
  ** number so that the iCur==j tests always fail in the main cell
  ** distribution loop below.
  */
  if( pCur ){
    iCur = 0;
    for(i=0; i<nOld; i++){
      if( pCur->pPage==apOld[i] ){
        iCur += pCur->idx;
        break;
      }
      iCur += apOld[i]->nCell;
      if( i<nOld-1 && pCur->pPage==pParent && pCur->idx==idxDiv[i] ){
        break;
      }
      iCur++;
    }
    pOldCurPage = pCur->pPage;
  }

  /*
  ** Make copies of the content of pPage and its siblings into aOld[].
  ** The rest of this function will use data from the copies rather
  ** that the original pages since the original pages will be in the
  ** process of being overwritten.
  */
  for(i=0; i<nOld; i++){
    copyPage(&aOld[i], apOld[i]);
    rc = freePage(pBt, apOld[i], pgnoOld[i]);
    if( rc ) goto balance_cleanup;
    mndbpager_unref(apOld[i]);
    apOld[i] = &aOld[i];
  }

  /*
  ** Load pointers to all cells on sibling pages and the divider cells
  ** into the local apCell[] array.  Make copies of the divider cells
  ** into aTemp[] and remove the the divider Cells from pParent.
  */
  nCell = 0;
  for(i=0; i<nOld; i++){
    MemPage *pOld = apOld[i];
    for(j=0; j<pOld->nCell; j++){
      apCell[nCell] = pOld->apCell[j];
      szCell[nCell] = cellSize(apCell[nCell]);
      nCell++;
    }
    if( i<nOld-1 ){
      szCell[nCell] = cellSize(apDiv[i]);
      memcpy(&aTemp[i], apDiv[i], szCell[nCell]);
      apCell[nCell] = &aTemp[i];
      dropCell(pParent, nxDiv, szCell[nCell]);
      assert( apCell[nCell]->h.leftChild==pgnoOld[i] );
      apCell[nCell]->h.leftChild = pOld->u.hdr.rightChild;
      nCell++;
    }
  }

  /*
  ** Figure out the number of pages needed to hold all nCell cells.
  ** Store this number in "k".  Also compute szNew[] which is the total
  ** size of all cells on the i-th page and cntNew[] which is the index
  ** in apCell[] of the cell that divides path i from path i+1.  
  ** cntNew[k] should equal nCell.
  **
  ** This little patch of code is critical for keeping the tree
  ** balanced. 
  */
  totalSize = 0;
  for(i=0; i<nCell; i++){
    totalSize += szCell[i];
  }
  for(subtotal=k=i=0; i<nCell; i++){
    subtotal += szCell[i];
    if( subtotal > USABLE_SPACE ){
      szNew[k] = subtotal - szCell[i];
      cntNew[k] = i;
      subtotal = 0;
      k++;
    }
  }
  szNew[k] = subtotal;
  cntNew[k] = nCell;
  k++;
  for(i=k-1; i>0; i--){
    while( szNew[i]<USABLE_SPACE/2 ){
      cntNew[i-1]--;
      assert( cntNew[i-1]>0 );
      szNew[i] += szCell[cntNew[i-1]];
      szNew[i-1] -= szCell[cntNew[i-1]-1];
    }
  }
  assert( cntNew[0]>0 );

  /*
  ** Allocate k new pages
  */
  for(i=0; i<k; i++){
    rc = allocatePage(pBt, &apNew[i], &pgnoNew[i]);
    if( rc ) goto balance_cleanup;
    nNew++;
    zeroPage(apNew[i]);
    apNew[i]->isInit = 1;
  }

  /*
  ** Evenly distribute the data in apCell[] across the new pages.
  ** Insert divider cells into pParent as necessary.
  */
  j = 0;
  for(i=0; i<nNew; i++){
    MemPage *pNew = apNew[i];
    while( j<cntNew[i] ){
      assert( pNew->nFree>=szCell[j] );
      if( pCur && iCur==j ){ pCur->pPage = pNew; pCur->idx = pNew->nCell; }
      insertCell(pNew, pNew->nCell, apCell[j], szCell[j]);
      j++;
    }
    assert( pNew->nCell>0 );
    assert( !pNew->isOverfull );
    relinkCellList(pNew);
    if( i<nNew-1 && j<nCell ){
      pNew->u.hdr.rightChild = apCell[j]->h.leftChild;
      apCell[j]->h.leftChild = pgnoNew[i];
      if( pCur && iCur==j ){ pCur->pPage = pParent; pCur->idx = nxDiv; }
      insertCell(pParent, nxDiv, apCell[j], szCell[j]);
      j++;
      nxDiv++;
    }
  }
  assert( j==nCell );
  apNew[nNew-1]->u.hdr.rightChild = apOld[nOld-1]->u.hdr.rightChild;
  if( nxDiv==pParent->nCell ){
    pParent->u.hdr.rightChild = pgnoNew[nNew-1];
  }else{
    pParent->apCell[nxDiv]->h.leftChild = pgnoNew[nNew-1];
  }
  if( pCur ){
    if( j<=iCur && pCur->pPage==pParent && pCur->idx>idxDiv[nOld-1] ){
      assert( pCur->pPage==pOldCurPage );
      pCur->idx += nNew - nOld;
    }else{
      assert( pOldCurPage!=0 );
      mndbpager_ref(pCur->pPage);
      mndbpager_unref(pOldCurPage);
    }
  }

  /*
  ** Reparent children of all cells.
  */
  for(i=0; i<nNew; i++){
    reparentChildPages(pBt->pPager, apNew[i]);
  }
  reparentChildPages(pBt->pPager, pParent);

  /*
  ** balance the parent page.
  */
  rc = balance(pBt, pParent, pCur);

  /*
  ** Cleanup before returning.
  */
balance_cleanup:
  if( extraUnref ){
    mndbpager_unref(extraUnref);
  }
  for(i=0; i<nOld; i++){
    if( apOld[i]!=&aOld[i] ) mndbpager_unref(apOld[i]);
  }
  for(i=0; i<nNew; i++){
    mndbpager_unref(apNew[i]);
  }
  if( pCur && pCur->pPage==0 ){
    pCur->pPage = pParent;
    pCur->idx = 0;
  }else{
    mndbpager_unref(pParent);
  }
  return rc;
}

/*
** Insert a new record into the BTree.  The key is given by (pKey,nKey)
** and the data is given by (pData,nData).  The cursor is used only to
** define what database the record should be inserted into.  The cursor
** is left pointing at the new record.
*/
int mndbBtreeInsert(
  BtCursor *pCur,                /* Insert data into the table of this cursor */
  const void *pKey, int nKey,    /* The key of the new record */
  const void *pData, int nData   /* The data of the new record */
){
  Cell newCell;
  int rc;
  int loc;
  int szNew;
  MemPage *pPage;
  Btree *pBt = pCur->pBt;

  if( !pCur->pBt->inTrans || nKey+nData==0 ){
    return MNDB_ERROR;  /* Must start a transaction first */
  }
  rc = mndbBtreeMoveto(pCur, pKey, nKey, &loc);
  if( rc ) return rc;
  pPage = pCur->pPage;
  rc = mndbpager_write(pPage);
  if( rc ) return rc;
  rc = fillInCell(pBt, &newCell, pKey, nKey, pData, nData);
  if( rc ) return rc;
  szNew = cellSize(&newCell);
  if( loc==0 ){
    newCell.h.leftChild = pPage->apCell[pCur->idx]->h.leftChild;
    rc = clearCell(pBt, pPage->apCell[pCur->idx]);
    if( rc ) return rc;
    dropCell(pPage, pCur->idx, cellSize(pPage->apCell[pCur->idx]));
  }else if( loc<0 && pPage->nCell>0 ){
    assert( pPage->u.hdr.rightChild==0 );  /* Must be a leaf page */
    pCur->idx++;
  }else{
    assert( pPage->u.hdr.rightChild==0 );  /* Must be a leaf page */
  }
  insertCell(pPage, pCur->idx, &newCell, szNew);
  rc = balance(pCur->pBt, pPage, pCur);
  /* mndbBtreePageDump(pCur->pBt, pCur->pgnoRoot, 1); */
  /* fflush(stdout); */
  return rc;
}

/*
** Delete the entry that the cursor is pointing to.
**
** The cursor is left pointing at either the next or the previous
** entry.  If the cursor is left pointing to the next entry, then 
** the pCur->bSkipNext flag is set which forces the next call to 
** mndbBtreeNext() to be a no-op.  That way, you can always call
** mndbBtreeNext() after a delete and the cursor will be left
** pointing to the first entry after the deleted entry.
*/
int mndbBtreeDelete(BtCursor *pCur){
  MemPage *pPage = pCur->pPage;
  Cell *pCell;
  int rc;
  Pgno pgnoChild;

  if( !pCur->pBt->inTrans ){
    return MNDB_ERROR;  /* Must start a transaction first */
  }
  if( pCur->idx >= pPage->nCell ){
    return MNDB_ERROR;  /* The cursor is not pointing to anything */
  }
  rc = mndbpager_write(pPage);
  if( rc ) return rc;
  pCell = pPage->apCell[pCur->idx];
  pgnoChild = pCell->h.leftChild;
  clearCell(pCur->pBt, pCell);
  if( pgnoChild ){
    /*
    ** The entry we are about to delete is not a leaf so if we do not
    ** do something we will leave a hole on an internal page.
    ** We have to fill the hole by moving in a cell from a leaf.  The
    ** next Cell after the one to be deleted is guaranteed to exist and
    ** to be a leaf so we can use it.
    */
    BtCursor leafCur;
    Cell *pNext;
    int szNext;
    getTempCursor(pCur, &leafCur);
    rc = mndbBtreeNext(&leafCur, 0);
    if( rc!=MNDB_OK ){
      return MNDB_CORRUPT;
    }
    rc = mndbpager_write(leafCur.pPage);
    if( rc ) return rc;
    dropCell(pPage, pCur->idx, cellSize(pCell));
    pNext = leafCur.pPage->apCell[leafCur.idx];
    szNext = cellSize(pNext);
    pNext->h.leftChild = pgnoChild;
    insertCell(pPage, pCur->idx, pNext, szNext);
    rc = balance(pCur->pBt, pPage, pCur);
    if( rc ) return rc;
    pCur->bSkipNext = 1;
    dropCell(leafCur.pPage, leafCur.idx, szNext);
    rc = balance(pCur->pBt, leafCur.pPage, 0);
    releaseTempCursor(&leafCur);
  }else{
    dropCell(pPage, pCur->idx, cellSize(pCell));
    if( pCur->idx>=pPage->nCell ){
      pCur->idx = pPage->nCell-1;
      if( pCur->idx<0 ){ pCur->idx = 0; }
      pCur->bSkipNext = 0;
    }else{
      pCur->bSkipNext = 1;
    }
    rc = balance(pCur->pBt, pPage, pCur);
  }
  return rc;
}

/*
** Create a new BTree in the same file.  Write into *piTable the index
** of the root page of the new table.
*/
int mndbBtreeCreateTable(Btree *pBt, int *piTable){
  MemPage *pRoot;
  Pgno pgnoRoot;
  int rc;
  if( !pBt->inTrans ){
    return MNDB_ERROR;  /* Must start a transaction first */
  }
  rc = allocatePage(pBt, &pRoot, &pgnoRoot);
  if( rc ) return rc;
  assert( mndbpager_iswriteable(pRoot) );
  zeroPage(pRoot);
  mndbpager_unref(pRoot);
  *piTable = (int)pgnoRoot;
  return MNDB_OK;
}

/*
** Erase the given database page and all its children.  Return
** the page to the freelist.
*/
static int clearDatabasePage(Btree *pBt, Pgno pgno, int freePageFlag){
  MemPage *pPage;
  int rc;
  Cell *pCell;
  int idx;

  rc = mndbpager_get(pBt->pPager, pgno, (void**)&pPage);
  if( rc ) return rc;
  rc = mndbpager_write(pPage);
  if( rc ) return rc;
  idx = pPage->u.hdr.firstCell;
  while( idx>0 ){
    pCell = (Cell*)&pPage->u.aDisk[idx];
    idx = pCell->h.iNext;
    if( pCell->h.leftChild ){
      rc = clearDatabasePage(pBt, pCell->h.leftChild, 1);
      if( rc ) return rc;
    }
    rc = clearCell(pBt, pCell);
    if( rc ) return rc;
  }
  if( pPage->u.hdr.rightChild ){
    rc = clearDatabasePage(pBt, pPage->u.hdr.rightChild, 1);
    if( rc ) return rc;
  }
  if( freePageFlag ){
    rc = freePage(pBt, pPage, pgno);
  }else{
    zeroPage(pPage);
  }
  mndbpager_unref(pPage);
  return rc;
}

/*
** Delete all information from a single table in the database.
*/
int mndbBtreeClearTable(Btree *pBt, int iTable){
  int rc;
  if( !pBt->inTrans ){
    return MNDB_ERROR;  /* Must start a transaction first */
  }
  rc = clearDatabasePage(pBt, (Pgno)iTable, 0);
  return rc;
}

/*
** Erase all information in a table and add the root of the table to
** the freelist.  Except, the root of the principle table (the one on
** page 2) is never added to the freelist.
*/
int mndbBtreeDropTable(Btree *pBt, int iTable){
  int rc;
  MemPage *pPage;
  if( !pBt->inTrans ){
    return MNDB_ERROR;  /* Must start a transaction first */
  }
  rc = mndbpager_get(pBt->pPager, (Pgno)iTable, (void**)&pPage);
  if( rc ) return rc;
  rc = mndbBtreeClearTable(pBt, iTable);
  if( rc ) return rc;
  if( iTable>2 ){
    rc = freePage(pBt, pPage, iTable);
  }else{
    zeroPage(pPage);
  }
  mndbpager_unref(pPage);
  return rc;  
}


/******************************************************************************
** The complete implementation of the BTree subsystem is above this line.
** All the code the follows is for testing and troubleshooting the BTree
** subsystem.  None of the code that follows is used during normal operation.
** All of the following code is omitted unless the library is compiled with
** the -DMNDB_TEST=1 compiler option.
******************************************************************************/
#if 1

/*
** Print a disassembly of the given page on standard output.  This routine
** is used for debugging and testing only.
*/
int mndbBtreePageDump(Btree *pBt, int pgno, int recursive){
  int rc;
  MemPage *pPage;
  int i, j;
  int nFree;
  u16 idx;
  char range[20];
  unsigned char payload[20];
  rc = mndbpager_get(pBt->pPager, (Pgno)pgno, (void**)&pPage);
  if( rc ){
    return rc;
  }
  if( recursive ) printf("PAGE %d:\n", pgno);
  i = 0;
  idx = pPage->u.hdr.firstCell;
  while( idx>0 && idx<=MNDB_PAGE_SIZE-MIN_CELL_SIZE ){
    Cell *pCell = (Cell*)&pPage->u.aDisk[idx];
    int sz = cellSize(pCell);
    sprintf(range,"%d..%d", idx, idx+sz-1);
    sz = pCell->h.nKey + pCell->h.nData;
    if( sz>sizeof(payload)-1 ) sz = sizeof(payload)-1;
    memcpy(payload, pCell->aPayload, sz);
    for(j=0; j<sz; j++){
      if( payload[j]<0x20 || payload[j]>0x7f ) payload[j] = '.';
    }
    payload[sz] = 0;
    printf(
      "cell %2d: i=%-10s chld=%-4d nk=%-4d nd=%-4d payload=%s\n",
      i, range, (int)pCell->h.leftChild, pCell->h.nKey, pCell->h.nData,
      payload
    );
    if( pPage->isInit && pPage->apCell[i]!=pCell ){
      printf("**** apCell[%d] does not match on prior entry ****\n", i);
    }
    i++;
    idx = pCell->h.iNext;
  }
  if( idx!=0 ){
    printf("ERROR: next cell index out of range: %d\n", idx);
  }
  printf("right_child: %d\n", pPage->u.hdr.rightChild);
  nFree = 0;
  i = 0;
  idx = pPage->u.hdr.firstFree;
  while( idx>0 && idx<MNDB_PAGE_SIZE ){
    FreeBlk *p = (FreeBlk*)&pPage->u.aDisk[idx];
    sprintf(range,"%d..%d", idx, idx+p->iSize-1);
    nFree += p->iSize;
    printf("freeblock %2d: i=%-10s size=%-4d total=%d\n",
       i, range, p->iSize, nFree);
    idx = p->iNext;
    i++;
  }
  if( idx!=0 ){
    printf("ERROR: next freeblock index out of range: %d\n", idx);
  }
  if( recursive && pPage->u.hdr.rightChild!=0 ){
    idx = pPage->u.hdr.firstCell;
    while( idx>0 && idx<MNDB_PAGE_SIZE-MIN_CELL_SIZE ){
      Cell *pCell = (Cell*)&pPage->u.aDisk[idx];
      mndbBtreePageDump(pBt, pCell->h.leftChild, 1);
      idx = pCell->h.iNext;
    }
    mndbBtreePageDump(pBt, pPage->u.hdr.rightChild, 1);
  }
  mndbpager_unref(pPage);
  return MNDB_OK;
}

/*
** Fill aResult[] with information about the entry and page that the
** cursor is pointing to.
** 
**   aResult[0] =  The page number
**   aResult[1] =  The entry number
**   aResult[2] =  Total number of entries on this page
**   aResult[3] =  Size of this entry
**   aResult[4] =  Number of free bytes on this page
**   aResult[5] =  Number of free blocks on the page
**   aResult[6] =  Page number of the left child of this entry
**   aResult[7] =  Page number of the right child for the whole page
**
** This routine is used for testing and debugging only.
*/
int mndbBtreeCursorDump(BtCursor *pCur, int *aResult){
  int cnt, idx;
  MemPage *pPage = pCur->pPage;
  aResult[0] = mndbpager_pagenumber(pPage);
  aResult[1] = pCur->idx;
  aResult[2] = pPage->nCell;
  if( pCur->idx>=0 && pCur->idx<pPage->nCell ){
    aResult[3] = cellSize(pPage->apCell[pCur->idx]);
    aResult[6] = pPage->apCell[pCur->idx]->h.leftChild;
  }else{
    aResult[3] = 0;
    aResult[6] = 0;
  }
  aResult[4] = pPage->nFree;
  cnt = 0;
  idx = pPage->u.hdr.firstFree;
  while( idx>0 && idx<MNDB_PAGE_SIZE ){
    cnt++;
    idx = ((FreeBlk*)&pPage->u.aDisk[idx])->iNext;
  }
  aResult[5] = cnt;
  aResult[7] = pPage->u.hdr.rightChild;
  return MNDB_OK;
}

/*
** Return the pager associated with a BTree.  This routine is used for
** testing and debugging only.
*/
Pager *mndbBtreePager(Btree *pBt){
  return pBt->pPager;
}

/*
** This structure is passed around through all the sanity checking routines
** in order to keep track of some global state information.
*/
typedef struct SanityCheck SanityCheck;
struct SanityCheck {
  Btree *pBt;    // The tree being checked out
  Pager *pPager; // The associated pager.  Also accessible by pBt->pPager
  int nPage;     // Number of pages in the database
  int *anRef;    // Number of times each page is referenced
  int nTreePage; // Number of BTree pages
  int nByte;     // Number of bytes of data stored on BTree pages
  char *zErrMsg; // An error message.  NULL of no errors seen.
};

/*
** Append a message to the error message string.
*/
static void checkAppendMsg(SanityCheck *pCheck, char *zMsg1, char *zMsg2){
  if( pCheck->zErrMsg ){
    char *zOld = pCheck->zErrMsg;
    pCheck->zErrMsg = 0;
    mndbSetString(&pCheck->zErrMsg, zOld, "\n", zMsg1, zMsg2, 0);
    mndbFree(zOld);
  }else{
    mndbSetString(&pCheck->zErrMsg, zMsg1, zMsg2, 0);
  }
}

/*
** Add 1 to the reference count for page iPage.  If this is the second
** reference to the page, add an error message to pCheck->zErrMsg.
** Return 1 if there are 2 ore more references to the page and 0 if
** if this is the first reference to the page.
**
** Also check that the page number is in bounds.
*/
static int checkRef(SanityCheck *pCheck, int iPage, char *zContext){
  if( iPage==0 ) return 1;
  if( iPage>pCheck->nPage ){
    char zBuf[100];
    sprintf(zBuf, "invalid page number %d", iPage);
    checkAppendMsg(pCheck, zContext, zBuf);
    return 1;
  }
  if( pCheck->anRef[iPage]==1 ){
    char zBuf[100];
    sprintf(zBuf, "2nd reference to page %d", iPage);
    checkAppendMsg(pCheck, zContext, zBuf);
    return 1;
  }
  return  (pCheck->anRef[iPage]++)>1;
}

/*
** Check the integrity of the freelist or of an overflow page list.
** Verify that the number of pages on the list is N.
*/
static void checkList(SanityCheck *pCheck, int iPage, int N, char *zContext){
  char zMsg[100];
  while( N-- ){
    OverflowPage *pOvfl;
    if( iPage<1 ){
      sprintf(zMsg, "%d pages missing from overflow list", N+1);
      checkAppendMsg(pCheck, zContext, zMsg);
      break;
    }
    if( checkRef(pCheck, iPage, zContext) ) break;
    if( mndbpager_get(pCheck->pPager, (Pgno)iPage, (void**)&pOvfl) ){
      sprintf(zMsg, "failed to get page %d", iPage);
      checkAppendMsg(pCheck, zContext, zMsg);
      break;
    }
    iPage = (int)pOvfl->iNext;
    mndbpager_unref(pOvfl);
  }
}

/*
** Do various sanity checks on a single page of a tree.  Return
** the tree depth.  Root pages return 0.  Parents of root pages
** return 1, and so forth.
** 
** These checks are done:
**
**      1.  Make sure that cells and freeblocks do not overlap
**          but combine to completely cover the page.
**      2.  Make sure cell keys are in order.
**      3.  Make sure no key is less than or equal to zLowerBound.
**      4.  Make sure no key is greater than or equal to zUpperBound.
**      5.  Check the integrity of overflow pages.
**      6.  Recursively call checkTreePage on all children.
**      7.  Verify that the depth of all children is the same.
**      8.  Make sure this page is at least 33% full or else it is
**          the root of the tree.
*/
static int checkTreePage(
  SanityCheck *pCheck,  /* Context for the sanity check */
  int iPage,            /* Page number of the page to check */
  MemPage *pParent,     /* Parent page */
  char *zParentContext, /* Parent context */
  char *zLowerBound,    /* All keys should be greater than this, if not NULL */
  char *zUpperBound     /* All keys should be less than this, if not NULL */
){
  MemPage *pPage;
  int i, rc, depth, d2, pgno;
  char *zKey1, *zKey2;
  BtCursor cur;
  char zMsg[100];
  char zContext[100];
  char hit[MNDB_PAGE_SIZE];

  /* Check that the page exists
  */
  if( iPage==0 ) return 0;
  if( checkRef(pCheck, iPage, zParentContext) ) return 0;
  sprintf(zContext, "On tree page %d: ", iPage);
  if( (rc = mndbpager_get(pCheck->pPager, (Pgno)iPage, (void**)&pPage))!=0 ){
    sprintf(zMsg, "unable to get the page. error code=%d", rc);
    checkAppendMsg(pCheck, zContext, zMsg);
    return 0;
  }
  if( (rc = initPage(pPage, (Pgno)iPage, pParent))!=0 ){
    sprintf(zMsg, "initPage() returns error code %d", rc);
    checkAppendMsg(pCheck, zContext, zMsg);
    mndbpager_unref(pPage);
    return 0;
  }

  /* Check out all the cells.
  */
  depth = 0;
  zKey1 = zLowerBound ? mndbStrDup(zLowerBound) : 0;
  cur.pPage = pPage;
  cur.pBt = pCheck->pBt;
  for(i=0; i<pPage->nCell; i++){
    Cell *pCell = pPage->apCell[i];
    int sz;

    /* Check payload overflow pages
    */
    sz = pCell->h.nKey + pCell->h.nData;
    sprintf(zContext, "On page %d cell %d: ", iPage, i);
    if( sz>MX_LOCAL_PAYLOAD ){
      int nPage = (sz - MX_LOCAL_PAYLOAD + OVERFLOW_SIZE - 1)/OVERFLOW_SIZE;
      checkList(pCheck, pCell->ovfl, nPage, zContext);
    }

    /* Check that keys are in the right order
    */
    cur.idx = i;
    zKey2 = mndbMalloc( pCell->h.nKey+1 );
    getPayload(&cur, 0, pCell->h.nKey, zKey2);
    if( zKey1 && strcmp(zKey1,zKey2)>=0 ){
      checkAppendMsg(pCheck, zContext, "Key is out of order");
    }

    /* Check sanity of left child page.
    */
    pgno = (int)pCell->h.leftChild;
    d2 = checkTreePage(pCheck, pgno, pPage, zContext, zKey1, zKey2);
    if( i>0 && d2!=depth ){
      checkAppendMsg(pCheck, zContext, "Child page depth differs");
    }
    depth = d2;
    mndbFree(zKey1);
    zKey1 = zKey2;
  }
  pgno = pPage->u.hdr.rightChild;
  sprintf(zContext, "On page %d at right child: ", iPage);
  checkTreePage(pCheck, pgno, pPage, zContext, zKey1, zUpperBound);
  mndbFree(zKey1);
 
  /* Check for complete coverage of the page
  */
  memset(hit, 0, sizeof(hit));
  memset(hit, 1, sizeof(PageHdr));
  for(i=pPage->u.hdr.firstCell; i>0 && i<MNDB_PAGE_SIZE; ){
    Cell *pCell = (Cell*)&pPage->u.aDisk[i];
    int j;
    for(j=i+cellSize(pCell)-1; j>=i; j--) hit[j]++;
    i = pCell->h.iNext;
  }
  for(i=pPage->u.hdr.firstFree; i>0 && i<MNDB_PAGE_SIZE; ){
    FreeBlk *pFBlk = (FreeBlk*)&pPage->u.aDisk[i];
    int j;
    for(j=i+pFBlk->iSize-1; j>=i; j--) hit[j]++;
    i = pFBlk->iNext;
  }
  for(i=0; i<MNDB_PAGE_SIZE; i++){
    if( hit[i]==0 ){
      sprintf(zMsg, "Unused space at byte %d of page %d", i, iPage);
      checkAppendMsg(pCheck, zMsg, 0);
      break;
    }else if( hit[i]>1 ){
      sprintf(zMsg, "Multiple uses for byte %d of page %d", i, iPage);
      checkAppendMsg(pCheck, zMsg, 0);
      break;
    }
  }

  /* Check that free space is kept to a minimum
  */
#if 0
  if( pParent && pParent->nCell>2 && pPage->nFree>3*MNDB_PAGE_SIZE/4 ){
    sprintf(zMsg, "free space (%d) greater than max (%d)", pPage->nFree,
       MNDB_PAGE_SIZE/3);
    checkAppendMsg(pCheck, zContext, zMsg);
  }
#endif

  /* Update freespace totals.
  */
  pCheck->nTreePage++;
  pCheck->nByte += USABLE_SPACE - pPage->nFree;

  mndbpager_unref(pPage);
  return depth;
}

/*
** This routine does a complete check of the given BTree file.  aRoot[] is
** an array of pages numbers were each page number is the root page of
** a table.  nRoot is the number of entries in aRoot.
**
** If everything checks out, this routine returns NULL.  If something is
** amiss, an error message is written into memory obtained from malloc()
** and a pointer to that error message is returned.  The calling function
** is responsible for freeing the error message when it is done.
*/
char *mndbBtreeSanityCheck(Btree *pBt, int *aRoot, int nRoot){
  int i;
  int nRef;
  SanityCheck sCheck;

  nRef = *mndbpager_stats(pBt->pPager);
  if( lockBtree(pBt)!=MNDB_OK ){
    return mndbStrDup("Unable to acquire a read lock on the database");
  }
  sCheck.pBt = pBt;
  sCheck.pPager = pBt->pPager;
  sCheck.nPage = mndbpager_pagecount(sCheck.pPager);
  sCheck.anRef = mndbMalloc( (sCheck.nPage+1)*sizeof(sCheck.anRef[0]) );
  sCheck.anRef[1] = 1;
  for(i=2; i<=sCheck.nPage; i++){ sCheck.anRef[i] = 0; }
  sCheck.zErrMsg = 0;

  /* Check the integrity of the freelist
  */
  checkList(&sCheck, pBt->page1->freeList, pBt->page1->nFree,"Main freelist: ");

  /* Check all the tables.
  */
  for(i=0; i<nRoot; i++){
    checkTreePage(&sCheck, aRoot[i], 0, "List of tree roots: ", 0, 0);
  }

  /* Make sure every page in the file is referenced
  */
  for(i=1; i<=sCheck.nPage; i++){
    if( sCheck.anRef[i]==0 ){
      char zBuf[100];
      sprintf(zBuf, "Page %d is never used", i);
      checkAppendMsg(&sCheck, zBuf, 0);
    }
  }

  /* Make sure this analysis did not leave any unref() pages
  */
  unlockBtreeIfUnused(pBt);
  if( nRef != *mndbpager_stats(pBt->pPager) ){
    char zBuf[100];
    sprintf(zBuf, 
      "Outstanding page count goes from %d to %d during this analysis",
      nRef, *mndbpager_stats(pBt->pPager)
    );
    checkAppendMsg(&sCheck, zBuf, 0);
  }

  /* Clean  up and report errors.
  */
  mndbFree(sCheck.anRef);
  return sCheck.zErrMsg;
}

#endif /* MNDB_TEST */


