/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tdbInt.h"

int32_t tdbOpen(const char *dbname, int32_t szPage, int32_t pages, TDB **ppDb, int8_t rollback,
                int32_t encryptAlgorithm, char *encryptKey) {
  TDB *pDb;
  int  dsize;
  int  zsize;
  int  tsize;
  u8  *pPtr;
  int  ret;

  *ppDb = NULL;

  dsize = strlen(dbname);
  zsize = sizeof(*pDb) + dsize * 2 + strlen(TDB_JOURNAL_NAME) + 3;

  pPtr = (uint8_t *)tdbOsCalloc(1, zsize);
  if (pPtr == NULL) {
    return terrno;
  }

  pDb = (TDB *)pPtr;
  pPtr += sizeof(*pDb);
  // pDb->rootDir
  pDb->dbName = pPtr;
  memcpy(pDb->dbName, dbname, dsize);
  pDb->dbName[dsize] = '\0';
  pPtr = pPtr + dsize + 1;
  // pDb->jfname
  pDb->jnName = pPtr;
  memcpy(pDb->jnName, dbname, dsize);
  pDb->jnName[dsize] = '/';
  memcpy(pDb->jnName + dsize + 1, TDB_JOURNAL_NAME, strlen(TDB_JOURNAL_NAME));
  pDb->jnName[dsize + 1 + strlen(TDB_JOURNAL_NAME)] = '\0';

  pDb->jfd = -1;

  pDb->encryptAlgorithm = encryptAlgorithm;
  if (encryptKey != NULL) {
    tstrncpy(pDb->encryptKey, encryptKey, ENCRYPT_KEY_LEN + 1);
  }

  ret = tdbPCacheOpen(szPage, pages, &(pDb->pCache));
  if (ret < 0) {
    return ret;
  }

  pDb->nPgrHash = 8;
  tsize = sizeof(SPager *) * pDb->nPgrHash;
  pDb->pgrHash = tdbOsMalloc(tsize);
  if (pDb->pgrHash == NULL) {
    return terrno;
  }
  memset(pDb->pgrHash, 0, tsize);

  ret = taosMulModeMkDir(dbname, 0755, false);
  if (ret < 0) {
    return terrno;
  }

#ifdef USE_MAINDB
  // open main db
  ret = tdbTbOpen(TDB_MAINDB_NAME, -1, sizeof(SBtInfo), NULL, pDb, &pDb->pMainDb, rollback);
  if (ret < 0) {
    return ret;
  }

  ret = tdbTbOpen(TDB_FREEDB_NAME, sizeof(SPgno), 0, NULL, pDb, &pDb->pFreeDb, rollback);
  if (ret < 0) {
    return ret;
  }

  // if the underlying data structure is a btree, it will do re-balance when there are too many pages,
  // the re-balance process include allocate and deallocate of free pages, this is ok for other tables,
  // but not for the free page table, because it is for free pages management, allocate/deallocate of
  // free pages during re-balance result in data corruption. so convert it to a linked list based stack
  // to avoid re-balance.
  int tdbTbBtreeToStack(TTB *pTb);
  ret = tdbTbBtreeToStack(pDb->pFreeDb);
  if (ret < 0) {
    return ret;
  }
#endif

  *ppDb = pDb;
  return 0;
}

void tdbClose(TDB *pDb) {
  SPager *pPager;

  if (pDb) {
#ifdef USE_MAINDB
    if (pDb->pMainDb) tdbTbClose(pDb->pMainDb);
    if (pDb->pFreeDb) tdbTbClose(pDb->pFreeDb);
#endif

    for (pPager = pDb->pgrList; pPager; pPager = pDb->pgrList) {
      pDb->pgrList = pPager->pNext;
      tdbPagerClose(pPager);
    }

    tdbPCacheClose(pDb->pCache);
    tdbOsFree(pDb->pgrHash);
    tdbOsFree(pDb);
  }

  return;
}

int32_t tdbAlter(TDB *pDb, int pages) { return tdbPCacheAlter(pDb->pCache, pages); }

int32_t tdbBegin(TDB *pDb, TXN **ppTxn, void *(*xMalloc)(void *, size_t), void (*xFree)(void *, void *), void *xArg,
                 int flags) {
  SPager *pPager;
  int     ret;
  int64_t txnId = ++pDb->txnId;
  if (txnId == INT64_MAX) {
    pDb->txnId = 0;
  }

  TXN *pTxn = tdbOsCalloc(1, sizeof(*pTxn));
  if (!pTxn) {
    return terrno;
  }

  ret = tdbTxnOpen(pTxn, txnId, xMalloc, xFree, xArg, flags);
  if (ret < 0) {
    tdbOsFree(pTxn);
    return ret;
  }

  for (pPager = pDb->pgrList; pPager; pPager = pPager->pNext) {
    ret = tdbPagerBegin(pPager, pTxn);
    if (ret < 0) {
      tdbError("failed to begin pager since %s. dbName:%s, txnId:%" PRId64, tstrerror(terrno), pDb->dbName,
               pTxn->txnId);
      tdbTxnClose(pTxn);
      return ret;
    }
  }

  *ppTxn = pTxn;

  return 0;
}

int32_t tdbCommit(TDB *pDb, TXN *pTxn) {
  SPager *pPager;
  int     ret;

  for (pPager = pDb->pgrList; pPager; pPager = pPager->pNext) {
    ret = tdbPagerCommit(pPager, pTxn);
    if (ret < 0) {
      tdbError("failed to commit pager since %s. dbName:%s, txnId:%" PRId64, tstrerror(terrno), pDb->dbName,
               pTxn->txnId);
      return ret;
    }
  }

  return 0;
}

int32_t tdbPostCommit(TDB *pDb, TXN *pTxn) {
  SPager *pPager;
  int     ret;

  for (pPager = pDb->pgrList; pPager; pPager = pPager->pNext) {
    ret = tdbPagerPostCommit(pPager, pTxn);
    if (ret < 0) {
      tdbError("failed to commit pager since %s. dbName:%s, txnId:%" PRId64, tstrerror(terrno), pDb->dbName,
               pTxn->txnId);
      return ret;
    }
  }

  tdbTxnClose(pTxn);

  return 0;
}

int32_t tdbPrepareAsyncCommit(TDB *pDb, TXN *pTxn) {
  SPager *pPager;
  int     ret;

  for (pPager = pDb->pgrList; pPager; pPager = pPager->pNext) {
    ret = tdbPagerPrepareAsyncCommit(pPager, pTxn);
    if (ret < 0) {
      tdbError("failed to commit pager since %s. dbName:%s, txnId:%" PRId64, tstrerror(terrno), pDb->dbName,
               pTxn->txnId);
      return ret;
    }
  }

  return 0;
}

void tdbAbort(TDB *pDb, TXN *pTxn) {
  SPager *pPager;
  int     ret;

  for (pPager = pDb->pgrList; pPager; pPager = pPager->pNext) {
    ret = tdbPagerAbort(pPager, pTxn);
    if (ret < 0) {
      tdbError("failed to abort pager since %s. dbName:%s, txnId:%" PRId64, tstrerror(terrno), pDb->dbName,
               pTxn->txnId);
    }
  }

  tdbTxnClose(pTxn);

  return;
}

SPager *tdbEnvGetPager(TDB *pDb, const char *fname) {
  u32      hash;
  SPager **ppPager;

  hash = tdbCstringHash(fname);
  ppPager = &pDb->pgrHash[hash % pDb->nPgrHash];
  tdbTrace("tdbttl getPager1: pager:%p, index:%d, name:%s", *ppPager, hash % pDb->nPgrHash, fname);
  for (; *ppPager && (strcmp(fname, (*ppPager)->dbFileName) != 0); ppPager = &((*ppPager)->pHashNext)) {
  }
  tdbTrace("tdbttl getPager2: pager:%p, index:%d, name:%s", *ppPager, hash % pDb->nPgrHash, fname);
  return *ppPager;
}

void tdbEnvAddPager(TDB *pDb, SPager *pPager) {
  u32      hash;
  SPager **ppPager;

  // rehash if neccessary
  if (pDb->nPager + 1 > pDb->nPgrHash) {
    // TODO
  }

  // add to list
  pPager->pNext = pDb->pgrList;
  pDb->pgrList = pPager;

  // add to hash
  hash = tdbCstringHash(pPager->dbFileName);
  ppPager = &pDb->pgrHash[hash % pDb->nPgrHash];
  tdbTrace("tdbttl addPager1: pager:%p, index:%d, name:%s", *ppPager, hash % pDb->nPgrHash, pPager->dbFileName);
  pPager->pHashNext = *ppPager;
  *ppPager = pPager;

  tdbTrace("tdbttl addPager2: pager:%p, index:%d, name:%s", *ppPager, hash % pDb->nPgrHash, pPager->dbFileName);

  // increase the counter
  pDb->nPager++;
}

void tdbEnvRemovePager(TDB *pDb, SPager *pPager) {
  u32      hash;
  SPager **ppPager;

  // remove from the list
  for (ppPager = &pDb->pgrList; *ppPager && (*ppPager != pPager); ppPager = &((*ppPager)->pNext)) {
  }
  if (*ppPager != pPager) {
    tdbError("tdb/db: invalid pPager: %p, *ppPager: %p", pPager, *ppPager);
    return;
  }
  *ppPager = pPager->pNext;

  // remove from hash
  hash = tdbCstringHash(pPager->dbFileName);
  ppPager = &pDb->pgrHash[hash % pDb->nPgrHash];
  for (; *ppPager && *ppPager != pPager; ppPager = &((*ppPager)->pHashNext)) {
  }
  if (*ppPager != pPager) {
    tdbError("tdb/db: invalid pPager: %p, *ppPager: %p", pPager, *ppPager);
    return;
  }
  *ppPager = pPager->pNext;

  // decrease the counter
  pDb->nPager--;

  // rehash if necessary
  if (pDb->nPgrHash > 8 && pDb->nPager < pDb->nPgrHash / 2) {
    // TODO
  }
}
