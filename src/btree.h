/*
** Copyright (c) 2001 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public
** License as published by the Free Software Foundation; either
** version 2 of the License, or (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** General Public License for more details.
** 
** You should have received a copy of the GNU General Public
** License along with this library; if not, write to the
** Free Software Foundation, Inc., 59 Temple Place - Suite 330,
** Boston, MA  02111-1307, USA.
**
** Author contact information:
**   drh@hwaci.com
**   http://www.hwaci.com/drh/
**
*************************************************************************
** This header file defines the interface that the mndb B-Tree file
** subsystem.
**
** @(#) $Id: btree.h,v 1.13 2001/09/14 18:54:08 drh Exp $
*/
#ifndef _BTREE_H_
#define _BTREE_H_

typedef struct Btree Btree;
typedef struct BtCursor BtCursor;

int mndbBtreeOpen(const char *zFilename,  int nPg, Btree **ppBtree);
int mndbBtreeClose(Btree*);
//int mndbBtreeSetCacheSize(Btree*, int);

// 写操作时调用tans
int mndbBtreeBeginTrans(Btree*);
int mndbBtreeCommit(Btree*);

int mndbBtreeCreateTable(Btree*, int*);
int mndbBtreeDropTable(Btree*, int);
int mndbBtreeClearTable(Btree*, int);

int mndbBtreeCursor(Btree*, int iTable, BtCursor **ppCur);
int mndbBtreeMoveto(BtCursor*, const void *pKey, int nKey, int *pRes);
int mndbBtreeDelete(BtCursor*);
int mndbBtreeInsert(BtCursor*, const void *pKey, int nKey,
                                 const void *pData, int nData);
int mndbBtreeFirst(BtCursor*, int *pRes);
int mndbBtreeNext(BtCursor*, int *pRes);
int mndbBtreeKeySize(BtCursor*, int *pSize);
int mndbBtreeKey(BtCursor*, int offset, int amt, char *zBuf);
int mndbBtreeDataSize(BtCursor*, int *pSize);
int mndbBtreeData(BtCursor*, int offset, int amt, char *zBuf);
int mndbBtreeCloseCursor(BtCursor*);


#ifdef MNDB_TEST
int mndbBtreePageDump(Btree*, int, int);
int mndbBtreeCursorDump(BtCursor*, int*);
struct Pager *mndbBtreePager(Btree*);
char *mndbBtreeSanityCheck(Btree*, int*, int);
#endif

#endif /* _BTREE_H_ */
