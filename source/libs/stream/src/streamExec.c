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

#include "streamInt.h"

// maximum allowed processed block batches. One block may include several submit blocks
#define MAX_STREAM_EXEC_BATCH_NUM         32
#define STREAM_RESULT_DUMP_THRESHOLD      300
#define STREAM_RESULT_DUMP_SIZE_THRESHOLD (1048576 * 1)  // 1MiB result data
#define STREAM_SCAN_HISTORY_TIMESLICE     1000           // 1000 ms
#define MIN_INVOKE_INTERVAL               50             // 50ms
#define FILL_HISTORY_TASK_EXEC_INTERVAL   5000           // 5 sec

static int32_t streamAlignRecalculateStart(SStreamTask* pTask);
static int32_t continueDispatchRecalculateStart(SStreamDataBlock* pBlock, SStreamTask* pTask);
static int32_t streamTransferStateDoPrepare(SStreamTask* pTask);
static int32_t streamTaskExecImpl(SStreamTask* pTask, SStreamQueueItem* pItem, int64_t* totalSize,
                                  int32_t* totalBlocks);

bool streamTaskShouldStop(const SStreamTask* pTask) {
  SStreamTaskState pState = streamTaskGetStatus(pTask);
  return (pState.state == TASK_STATUS__STOP) || (pState.state == TASK_STATUS__DROPPING);
}

bool streamTaskShouldPause(const SStreamTask* pTask) {
  return (streamTaskGetStatus(pTask).state == TASK_STATUS__PAUSE);
}

static int32_t doOutputResultBlockImpl(SStreamTask* pTask, SStreamDataBlock* pBlock) {
  int32_t code = 0;
  int32_t type = pTask->outputInfo.type;
  if (type == TASK_OUTPUT__TABLE) {
    pTask->outputInfo.tbSink.tbSinkFunc(pTask, pTask->outputInfo.tbSink.vnode, pBlock->blocks);
    destroyStreamDataBlock(pBlock);
  } else if (type == TASK_OUTPUT__SMA) {
    pTask->outputInfo.smaSink.smaSink(pTask->outputInfo.smaSink.vnode, pTask->outputInfo.smaSink.smaId, pBlock->blocks);
    destroyStreamDataBlock(pBlock);
  } else {
    if (type != TASK_OUTPUT__FIXED_DISPATCH && type != TASK_OUTPUT__SHUFFLE_DISPATCH &&
        type != TASK_OUTPUT__VTABLE_MAP) {
      stError("s-task:%s invalid stream output type:%d, internal error", pTask->id.idStr, type);
      return TSDB_CODE_STREAM_INTERNAL_ERROR;
    }

    code = streamTaskPutDataIntoOutputQ(pTask, pBlock);
    if (code != TSDB_CODE_SUCCESS) {
      destroyStreamDataBlock(pBlock);
      return code;
    }

    // not handle error, if dispatch failed, try next time.
    // checkpoint trigger will be checked
    code = streamDispatchStreamBlock(pTask);
  }

  return code;
}

static int32_t doDumpResult(SStreamTask* pTask, SStreamQueueItem* pItem, SArray* pRes, int32_t size, int64_t* totalSize,
                            int32_t* totalBlocks) {
  int32_t numOfBlocks = taosArrayGetSize(pRes);
  if (numOfBlocks == 0) {
    taosArrayDestroyEx(pRes, (FDelete)blockDataFreeRes);
    return TSDB_CODE_SUCCESS;
  }

  SStreamDataBlock* pStreamBlocks = NULL;

  int32_t code = createStreamBlockFromResults(pItem, pTask, size, pRes, &pStreamBlocks);
  if (code) {
    stError("s-task:%s failed to create result stream data block, code:%s", pTask->id.idStr, tstrerror(terrno));
    taosArrayDestroyEx(pRes, (FDelete)blockDataFreeRes);
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  stDebug("s-task:%s dump stream result data blocks, num:%d, size:%.2fMiB", pTask->id.idStr, numOfBlocks,
          SIZE_IN_MiB(size));

  code = doOutputResultBlockImpl(pTask, pStreamBlocks);
  if (code != TSDB_CODE_SUCCESS) {  // back pressure and record position
    return code;
  }

  *totalSize += size;
  *totalBlocks += numOfBlocks;

  return code;
}

static int32_t doAppendPullOverBlock(SStreamTask* pTask, int32_t* pNumOfBlocks, SStreamDataBlock* pRetrieveBlock,
                                     SArray* pRes) {
  SSDataBlock block = {0};
  int32_t     num = taosArrayGetSize(pRetrieveBlock->blocks);
  if (num != 1) {
    stError("s-task:%s invalid retrieve block number:%d, ignore", pTask->id.idStr, num);
    return TSDB_CODE_INVALID_PARA;
  }

  void*   p = taosArrayGet(pRetrieveBlock->blocks, 0);
  int32_t code = assignOneDataBlock(&block, p);
  if (code) {
    stError("s-task:%s failed to assign retrieve block, code:%s", pTask->id.idStr, tstrerror(code));
    return code;
  }

  block.info.type = STREAM_PULL_OVER;
  block.info.childId = pTask->info.selfChildId;

  p = taosArrayPush(pRes, &block);
  if (p != NULL) {
    (*pNumOfBlocks) += 1;
    stDebug("s-task:%s(child %d) retrieve res from upstream completed, QID:0x%" PRIx64, pTask->id.idStr,
            pTask->info.selfChildId, pRetrieveBlock->reqId);
  } else {
    code = terrno;
    stError("s-task:%s failed to append pull over block for retrieve data, QID:0x%" PRIx64 " code:%s", pTask->id.idStr,
            pRetrieveBlock->reqId, tstrerror(code));
  }

  return code;
}

static int32_t doAppendRecalBlock(SStreamTask* pTask, int32_t* pNumOfBlocks, SStreamTrigger* pRecalculateBlock,
                                  SArray* pRes) {
  int32_t code = 0;
  SSDataBlock block = {0};

  void* p = taosArrayPush(pRes, pRecalculateBlock->pBlock);
  if (p != NULL) {
    (*pNumOfBlocks) += 1;
    stDebug("s-task:%s(child %d) recalculate from upstream completed, QID:0x%" PRIx64, pTask->id.idStr,
            pTask->info.selfChildId, /*pRecalculateBlock->reqId*/ (int64_t)0);
  } else {
    code = terrno;
    stError("s-task:%s failed to append recalculate block for downstream, QID:0x%" PRIx64" code:%s", pTask->id.idStr,
            /*pRecalculateBlock->reqId*/(int64_t)0, tstrerror(code));
  }

  return code;
}

int32_t streamTaskExecImpl(SStreamTask* pTask, SStreamQueueItem* pItem, int64_t* totalSize, int32_t* totalBlocks) {
  int32_t size = 0;
  int32_t numOfBlocks = 0;
  int32_t code = TSDB_CODE_SUCCESS;
  void*   pExecutor = pTask->exec.pExecutor;
  SArray* pRes = NULL;

  *totalBlocks = 0;
  *totalSize = 0;

  while (1) {
    SSDataBlock* output = NULL;
    uint64_t     ts = 0;

    if (pRes == NULL) {
      pRes = taosArrayInit(4, sizeof(SSDataBlock));
    }

    if (streamTaskShouldStop(pTask) || (pRes == NULL)) {
      taosArrayDestroyEx(pRes, (FDelete)blockDataFreeRes);
      return code;
    }

    if ((code = qExecTask(pExecutor, &output, &ts)) < 0) {
      if (code == TSDB_CODE_QRY_IN_EXEC) {
        qResetTaskInfoCode(pExecutor);
      }

      if (code == TSDB_CODE_OUT_OF_MEMORY || code == TSDB_CODE_INVALID_PARA || code == TSDB_CODE_FILE_CORRUPTED) {
        stFatal("s-task:%s failed to continue execute since %s", pTask->id.idStr, tstrerror(code));
        taosArrayDestroyEx(pRes, (FDelete)blockDataFreeRes);
        return code;
      } else {
        qResetTaskCode(pExecutor);
        continue;
      }
    }

    if (output == NULL) {
      if (pItem != NULL && (pItem->type == STREAM_INPUT__DATA_RETRIEVE)) {
        code = doAppendPullOverBlock(pTask, &numOfBlocks, (SStreamDataBlock*)pItem, pRes);
        if (code) {
          taosArrayDestroyEx(pRes, (FDelete)blockDataFreeRes);
          return code;
        }
      }

      break;
    }

    if (pTask->info.fillHistory == STREAM_RECALCUL_TASK && pTask->info.taskLevel == TASK_LEVEL__AGG) {
      stDebug("s-task:%s exec output type:%d", pTask->id.idStr, output->info.type);
    }

    if (output->info.type == STREAM_RETRIEVE) {
      if (streamBroadcastToUpTasks(pTask, output) < 0) {
        // TODO
      }
      continue;
    } else if (output->info.type == STREAM_CHECKPOINT) {
      continue;  // checkpoint block not dispatch to downstream tasks
    }

    SSDataBlock block = {.info.childId = pTask->info.selfChildId};
    code = assignOneDataBlock(&block, output);
    if (code) {
      stError("s-task:%s failed to build result block due to out of memory", pTask->id.idStr);
      continue;
    }

    size += blockDataGetSize(output) + sizeof(SSDataBlock) + sizeof(SColumnInfoData) * blockDataGetNumOfCols(&block);
    numOfBlocks += 1;

    void* p = taosArrayPush(pRes, &block);
    if (p == NULL) {
      stError("s-task:%s failed to add computing results, the final res may be incorrect", pTask->id.idStr);
    } else {
      stDebug("s-task:%s (child %d) executed and get %d result blocks, size:%.2fMiB", pTask->id.idStr,
              pTask->info.selfChildId, numOfBlocks, SIZE_IN_MiB(size));
    }

    // current output should be dispatched to down stream nodes
    if (numOfBlocks >= STREAM_RESULT_DUMP_THRESHOLD || size >= STREAM_RESULT_DUMP_SIZE_THRESHOLD) {
      code = doDumpResult(pTask, pItem, pRes, size, totalSize, totalBlocks);
      // todo: here we need continue retry to put it into output buffer
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }

      pRes = NULL;
      size = 0;
      numOfBlocks = 0;
    }
  }

  if (numOfBlocks > 0) {
    code = doDumpResult(pTask, pItem, pRes, size, totalSize, totalBlocks);
  } else {
    taosArrayDestroyEx(pRes, (FDelete)blockDataFreeRes);
  }

  return code;
}

// todo contiuous try to create result blocks
static int32_t handleScanhistoryResultBlocks(SStreamTask* pTask, SArray* pRes, int32_t size) {
  int32_t code = TSDB_CODE_SUCCESS;
  if (taosArrayGetSize(pRes) > 0) {
    SStreamDataBlock* pStreamBlocks = NULL;
    code = createStreamBlockFromResults(NULL, pTask, size, pRes, &pStreamBlocks);
    if (code) {
      stError("s-task:%s failed to build history result blocks", pTask->id.idStr);
      return code;
    }

    code = doOutputResultBlockImpl(pTask, pStreamBlocks);
    if (code != TSDB_CODE_SUCCESS) {  // should not have error code
      stError("s-task:%s dump fill-history results failed, code:%s", pTask->id.idStr, tstrerror(code));
    }
  } else {
    taosArrayDestroyEx(pRes, (FDelete)blockDataFreeRes);
  }
  return code;
}

static void streamScanHistoryDataImpl(SStreamTask* pTask, SArray* pRes, int32_t* pSize, bool* pFinish) {
  int32_t code = TSDB_CODE_SUCCESS;
  void*   exec = pTask->exec.pExecutor;
  int32_t numOfBlocks = 0;

  while (1) {
    if (streamTaskShouldStop(pTask)) {
      break;
    }

    if (pTask->inputq.status == TASK_INPUT_STATUS__BLOCKED) {
      stDebug("s-task:%s level:%d inputQ is blocked, retry in 5s", pTask->id.idStr, pTask->info.taskLevel);
      break;
    }

    SSDataBlock* output = NULL;
    uint64_t     ts = 0;
    code = qExecTask(exec, &output, &ts);
    if (code != TSDB_CODE_TSC_QUERY_KILLED && code != TSDB_CODE_SUCCESS) {  // if out of memory occurs, quit
      stError("s-task:%s scan-history data error occurred code:%s, continue scan-history", pTask->id.idStr,
              tstrerror(code));
      qResetTaskCode(exec);
      continue;
    }

    // the generated results before fill-history task been paused, should be dispatched to sink node
    if (output == NULL) {
      (*pFinish) = qStreamScanhistoryFinished(exec);
      break;
    }

    SSDataBlock block = {0};
    code = assignOneDataBlock(&block, output);
    if (code) {
      stError("s-task:%s failed to build result block due to out of memory", pTask->id.idStr);
    }

    block.info.childId = pTask->info.selfChildId;
    void* p = taosArrayPush(pRes, &block);
    if (p == NULL) {
      stError("s-task:%s failed to add computing results, the final res may be incorrect", pTask->id.idStr);
    }

    (*pSize) +=
        blockDataGetSize(output) + sizeof(SSDataBlock) + sizeof(SColumnInfoData) * blockDataGetNumOfCols(&block);
    numOfBlocks += 1;

    if (numOfBlocks >= STREAM_RESULT_DUMP_THRESHOLD || (*pSize) >= STREAM_RESULT_DUMP_SIZE_THRESHOLD) {
      stDebug("s-task:%s scan exec numOfBlocks:%d, size:%.2fKiB output num-limit:%d, size-limit:%.2fKiB reached",
              pTask->id.idStr, numOfBlocks, SIZE_IN_KiB(*pSize), STREAM_RESULT_DUMP_THRESHOLD,
              SIZE_IN_KiB(STREAM_RESULT_DUMP_SIZE_THRESHOLD));
      break;
    }
  }
}

static SScanhistoryDataInfo buildScanhistoryExecRet(EScanHistoryCode code, int32_t idleTime) {
  return (SScanhistoryDataInfo){code, idleTime};
}

SScanhistoryDataInfo streamScanHistoryData(SStreamTask* pTask, int64_t st) {
  void*       exec = pTask->exec.pExecutor;
  bool        finished = false;
  const char* id = pTask->id.idStr;

  if (pTask->info.taskLevel != TASK_LEVEL__SOURCE) {
    stError("s-task:%s not source scan-history task, not exec, quit", pTask->id.idStr);
    return buildScanhistoryExecRet(TASK_SCANHISTORY_QUIT, 0);
  }

  if ((!pTask->hTaskInfo.operatorOpen) || (pTask->info.fillHistory == STREAM_RECALCUL_TASK)) {
    int32_t code = qSetStreamOpOpen(exec);
    pTask->hTaskInfo.operatorOpen = true;
  }

  while (1) {
    if (streamTaskShouldPause(pTask)) {
      stDebug("s-task:%s paused from the scan-history task", id);
      // quit from step1, not continue to handle the step2
      return buildScanhistoryExecRet(TASK_SCANHISTORY_QUIT, 0);
    }

    // output queue is full, idle for 5 sec.
    if (streamQueueIsFull(pTask->outputq.queue)) {
      stWarn("s-task:%s outputQ is full, idle for 1sec and retry", id);
      return buildScanhistoryExecRet(TASK_SCANHISTORY_REXEC, STREAM_SCAN_HISTORY_TIMESLICE);
    }

    if (pTask->inputq.status == TASK_INPUT_STATUS__BLOCKED) {
      stWarn("s-task:%s downstream task inputQ blocked, idle for 5sec and retry", id);
      return buildScanhistoryExecRet(TASK_SCANHISTORY_REXEC, FILL_HISTORY_TASK_EXEC_INTERVAL);
    }

    SArray* pRes = taosArrayInit(0, sizeof(SSDataBlock));
    if (pRes == NULL) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      stError("s-task:%s scan-history prepare result block failed, code:%s, retry later", id, tstrerror(terrno));
      continue;
    }

    int32_t size = 0;
    streamScanHistoryDataImpl(pTask, pRes, &size, &finished);

    if (streamTaskShouldStop(pTask)) {
      taosArrayDestroyEx(pRes, (FDelete)blockDataFreeRes);
      return buildScanhistoryExecRet(TASK_SCANHISTORY_QUIT, 0);
    }

    // dispatch the generated results, todo fix error
    int32_t code = handleScanhistoryResultBlocks(pTask, pRes, size);
    if (code) {
      stError("s-task:%s failed to handle scan result block, code:%s", pTask->id.idStr, tstrerror(code));
    }

    if (finished) {
      return buildScanhistoryExecRet(TASK_SCANHISTORY_CONT, 0);
    }

    int64_t el = taosGetTimestampMs() - st;
    if (el >= STREAM_SCAN_HISTORY_TIMESLICE && (pTask->info.fillHistory == 1)) {
      stDebug("s-task:%s fill-history:%d time slice exhausted, elapsed time:%.2fs, retry in 100ms", id,
              pTask->info.fillHistory, el / 1000.0);
      return buildScanhistoryExecRet(TASK_SCANHISTORY_REXEC, 100);
    }
  }
}

int32_t streamTransferStateDoPrepare(SStreamTask* pTask) {
  SStreamMeta* pMeta = pTask->pMeta;
  const char*  id = pTask->id.idStr;

  SStreamTask* pStreamTask = NULL;
  int32_t code = streamMetaAcquireTask(pMeta, pTask->streamTaskId.streamId, pTask->streamTaskId.taskId, &pStreamTask);
  if (pStreamTask == NULL || code != TSDB_CODE_SUCCESS) {
    stError(
        "s-task:%s failed to find related stream task:0x%x, may have been destroyed or closed, destroy related "
        "fill-history task",
        id, (int32_t)pTask->streamTaskId.taskId);

    // 1. free it and remove fill-history task from disk meta-store
    // todo: this function should never be failed.
    code = streamBuildAndSendDropTaskMsg(pTask->pMsgCb, pMeta->vgId, &pTask->id, 0);

    // 2. save to disk
    streamMetaWLock(pMeta);
    if (streamMetaCommit(pMeta) < 0) {
      // persist to disk
    }
    streamMetaWUnLock(pMeta);
    return TSDB_CODE_STREAM_TASK_NOT_EXIST;
  } else {
    double el = (taosGetTimestampMs() - pTask->execInfo.step2Start) / 1000.;
    stDebug(
        "s-task:%s fill-history task end, status:%s, scan wal elapsed time:%.2fSec, update related stream task:%s "
        "info, prepare transfer exec state",
        id, streamTaskGetStatus(pTask).name, el, pStreamTask->id.idStr);
  }

  ETaskStatus  status = streamTaskGetStatus(pStreamTask).state;
  STimeWindow* pTimeWindow = &pStreamTask->dataRange.window;

  // It must be halted for a source stream task, since when the related scan-history-data task start scan the history
  // for the step 2.
  if (pStreamTask->info.taskLevel == TASK_LEVEL__SOURCE) {
    if (!(status == TASK_STATUS__HALT || status == TASK_STATUS__DROPPING || status == TASK_STATUS__STOP)) {
      stError("s-task:%s invalid task status:%d", id, status);
      return TSDB_CODE_STREAM_INTERNAL_ERROR;
    }
  } else {
    if (!(status == TASK_STATUS__READY || status == TASK_STATUS__PAUSE || status == TASK_STATUS__DROPPING ||
          status == TASK_STATUS__STOP)) {
      stError("s-task:%s invalid task status:%d", id, status);
      return TSDB_CODE_STREAM_INTERNAL_ERROR;
    }
    code = streamTaskHandleEvent(pStreamTask->status.pSM, TASK_EVENT_HALT);
    if (code != TSDB_CODE_SUCCESS) {
      stError("s-task:%s halt stream task:%s failed, code:%s not transfer state to stream task", id,
              pStreamTask->id.idStr, tstrerror(code));
      streamMetaReleaseTask(pMeta, pStreamTask);
      return code;
    } else {
      stDebug("s-task:%s halt by related fill-history task:%s", pStreamTask->id.idStr, id);
    }
  }

  // In case of sink tasks, no need to halt them.
  // In case of source tasks and agg tasks, we should HALT them, and wait for them to be idle. And then, it's safe to
  // start the task state transfer procedure.
  SStreamTaskState pState = streamTaskGetStatus(pStreamTask);
  status = pState.state;
  char* p = pState.name;
  if (status == TASK_STATUS__STOP || status == TASK_STATUS__DROPPING) {
    stError("s-task:%s failed to transfer state from fill-history task:%s, status:%s", id, pStreamTask->id.idStr, p);
    streamMetaReleaseTask(pMeta, pStreamTask);
    return TSDB_CODE_STREAM_TASK_IVLD_STATUS;
  }

  // 1. expand the query time window for stream task of WAL scanner
  if (pStreamTask->info.taskLevel == TASK_LEVEL__SOURCE) {
    // update the scan data range for source task.
    stDebug("s-task:%s level:%d stream task window %" PRId64 " - %" PRId64 " update to %" PRId64 " - %" PRId64
            ", status:%s, sched-status:%d",
            pStreamTask->id.idStr, TASK_LEVEL__SOURCE, pTimeWindow->skey, pTimeWindow->ekey, INT64_MIN,
            pTimeWindow->ekey, p, pStreamTask->status.schedStatus);

    code = streamTaskResetTimewindowFilter(pStreamTask);
  } else {
    stDebug("s-task:%s no need to update/reset filter time window for non-source tasks", pStreamTask->id.idStr);
  }

  // NOTE: transfer the ownership of executor state before handle the checkpoint block during stream exec
  // 2. send msg to mnode to launch a checkpoint to keep the state for current stream
  code = streamTaskSendCheckpointReq(pStreamTask);

  // 3. the default task status should be ready or something, not halt.
  // status to the value that will be kept in disk

  // 4. open the inputQ for all upstream tasks
  streamTaskOpenAllUpstreamInput(pStreamTask);

  streamMetaReleaseTask(pMeta, pStreamTask);
  return code;
}

static int32_t haltCallback(SStreamTask* pTask, void* param) {
  streamTaskOpenAllUpstreamInput(pTask);
  return streamTaskSendCheckpointReq(pTask);
}

int32_t streamTransferStatePrepare(SStreamTask* pTask) {
  int32_t      code = TSDB_CODE_SUCCESS;
  SStreamMeta* pMeta = pTask->pMeta;

  if (pTask->status.appendTranstateBlock != 1) {
    stError("s-task:%s not set appendTransBlock flag, internal error", pTask->id.idStr);
    return TSDB_CODE_STREAM_INTERNAL_ERROR;
  }

  int32_t level = pTask->info.taskLevel;
  if (level == TASK_LEVEL__AGG || level == TASK_LEVEL__SOURCE || level == TASK_LEVEL__MERGE) {  // do transfer task operator states.
    code = streamTransferStateDoPrepare(pTask);
  } else {
    // no state transfer for sink tasks, and drop fill-history task, followed by opening inputQ of sink task.
    SStreamTask* pStreamTask = NULL;
    code = streamMetaAcquireTask(pMeta, pTask->streamTaskId.streamId, pTask->streamTaskId.taskId, &pStreamTask);
    if (pStreamTask != NULL) {
      // halt the related stream sink task
      code = streamTaskHandleEventAsync(pStreamTask->status.pSM, TASK_EVENT_HALT, haltCallback, NULL);
      if (code != TSDB_CODE_SUCCESS) {
        stError("s-task:%s halt stream task:%s failed, code:%s not transfer state to stream task", pTask->id.idStr,
                pStreamTask->id.idStr, tstrerror(code));
        streamMetaReleaseTask(pMeta, pStreamTask);
        return code;
      } else {
        stDebug("s-task:%s sink task halt by related fill-history task:%s", pStreamTask->id.idStr, pTask->id.idStr);
      }
      streamMetaReleaseTask(pMeta, pStreamTask);
    }
  }

  return code;
}

// set input
static int32_t doSetStreamInputBlock(SStreamTask* pTask, const void* pInput, int64_t* pVer, const char* id) {
  void*   pExecutor = pTask->exec.pExecutor;
  int32_t code = 0;

  const SStreamQueueItem* pItem = pInput;
  if (pItem->type == STREAM_INPUT__GET_RES) {
    const SStreamTrigger* pTrigger = (const SStreamTrigger*)pInput;
    code = qSetMultiStreamInput(pExecutor, pTrigger->pBlock, 1, STREAM_INPUT__DATA_BLOCK);
    if (pTask->info.trigger == STREAM_TRIGGER_FORCE_WINDOW_CLOSE) {
      TSKEY k = pTrigger->pBlock->info.window.skey;
      stDebug("s-task:%s set force_window_close as source block, skey:%" PRId64, id, k);
      (*pVer) = k;
    }
  } else if (pItem->type == STREAM_INPUT__DATA_SUBMIT) {
    const SStreamDataSubmit* pSubmit = (const SStreamDataSubmit*)pInput;
    code = qSetMultiStreamInput(pExecutor, &pSubmit->submit, 1, STREAM_INPUT__DATA_SUBMIT);
    stDebug("s-task:%s set submit blocks as source block completed, %p %p len:%d ver:%" PRId64, id, pSubmit,
            pSubmit->submit.msgStr, pSubmit->submit.msgLen, pSubmit->submit.ver);
    if ((*pVer) > pSubmit->submit.ver) {
      stError("s-task:%s invalid recorded ver:%" PRId64 " greater than new block ver:%" PRId64 ", not update", id,
              *pVer, pSubmit->submit.ver);
    } else {
      (*pVer) = pSubmit->submit.ver;
    }
  } else if (pItem->type == STREAM_INPUT__DATA_BLOCK || pItem->type == STREAM_INPUT__DATA_RETRIEVE) {
    const SStreamDataBlock* pBlock = (const SStreamDataBlock*)pInput;

    SArray* pBlockList = pBlock->blocks;
    int32_t numOfBlocks = taosArrayGetSize(pBlockList);
    stDebug("s-task:%s set sdata blocks as input num:%d, ver:%" PRId64, id, numOfBlocks, pBlock->sourceVer);
    code = qSetMultiStreamInput(pExecutor, pBlockList->pData, numOfBlocks, STREAM_INPUT__DATA_BLOCK);

  } else if (pItem->type == STREAM_INPUT__MERGED_SUBMIT) {
    const SStreamMergedSubmit* pMerged = (const SStreamMergedSubmit*)pInput;

    SArray* pBlockList = pMerged->submits;
    int32_t numOfBlocks = taosArrayGetSize(pBlockList);
    stDebug("s-task:%s %p set (merged) submit blocks as a batch, numOfBlocks:%d, ver:%" PRId64, id, pTask, numOfBlocks,
            pMerged->ver);
    code = qSetMultiStreamInput(pExecutor, pBlockList->pData, numOfBlocks, STREAM_INPUT__MERGED_SUBMIT);

    if ((*pVer) > pMerged->ver) {
      stError("s-task:%s invalid recorded ver:%" PRId64 " greater than new block ver:%" PRId64 ", not update", id,
              *pVer, pMerged->ver);
    } else {
      (*pVer) = pMerged->ver;
    }

  } else if (pItem->type == STREAM_INPUT__REF_DATA_BLOCK) {
    const SStreamRefDataBlock* pRefBlock = (const SStreamRefDataBlock*)pInput;
    code = qSetMultiStreamInput(pExecutor, pRefBlock->pBlock, 1, STREAM_INPUT__DATA_BLOCK);

  } else if (pItem->type == STREAM_INPUT__CHECKPOINT || pItem->type == STREAM_INPUT__CHECKPOINT_TRIGGER ||
             pItem->type == STREAM_INPUT__RECALCULATE) {
    const SStreamDataBlock* pCheckpoint = (const SStreamDataBlock*)pInput;
    code = qSetMultiStreamInput(pExecutor, pCheckpoint->blocks, 1, pItem->type);

    if (pItem->type == STREAM_INPUT__RECALCULATE) {
      int32_t t = ((SStreamDataBlock*) pCheckpoint)->type;
      int32_t tId = (int32_t)pTask->hTaskInfo.id.taskId;
      if (t == STREAM_RECALCULATE_START) {
        stDebug("s-task:%s set recalculate block to start related recalculate task:0x%x", id, tId);
      } else {
        stDebug("s-task:%s set recalculate block:%d, task:0x%x", id, t, tId);
      }
    }
  } else {
    stError("s-task:%s invalid input block type:%d, discard", id, pItem->type);
    code = TSDB_CODE_STREAM_INTERNAL_ERROR;
  }

  return code;
}

void streamProcessTransstateBlock(SStreamTask* pTask, SStreamDataBlock* pBlock) {
  const char* id = pTask->id.idStr;
  int32_t     code = TSDB_CODE_SUCCESS;
  int32_t     level = pTask->info.taskLevel;

  // dispatch the tran-state block to downstream task immediately
  int32_t type = pTask->outputInfo.type;

  if (level == TASK_LEVEL__AGG || level == TASK_LEVEL__SINK || level == TASK_LEVEL__MERGE) {
    int32_t remain = streamAlignTransferState(pTask);
    if (remain > 0) {
      streamFreeQitem((SStreamQueueItem*)pBlock);
      stDebug("s-task:%s receive upstream trans-state msg, not sent remain:%d", id, remain);
      return;
    }
  }

  // transfer the ownership of executor state
  if (type == TASK_OUTPUT__FIXED_DISPATCH || type == TASK_OUTPUT__SHUFFLE_DISPATCH || type == TASK_OUTPUT__VTABLE_MAP) {
    if (level == TASK_LEVEL__SOURCE) {
      stDebug("s-task:%s add transfer-state block into outputQ", id);
    } else {
      stDebug("s-task:%s all upstream tasks send transfer-state block, add transfer-state block into outputQ", id);
    }

    // agg task should dispatch trans-state msg to sink task, to flush all data to sink task.
    if (level == TASK_LEVEL__AGG || level == TASK_LEVEL__SOURCE || level == TASK_LEVEL__MERGE) {
      pBlock->srcVgId = pTask->pMeta->vgId;
      code = taosWriteQitem(pTask->outputq.queue->pQueue, pBlock);
      if (code == 0) {
        code = streamDispatchStreamBlock(pTask);
        if (code) {
          stError("s-task:%s failed to dispatch stream block, code:%s", id, tstrerror(code));
        }
      } else {  // todo put into queue failed, retry
        streamFreeQitem((SStreamQueueItem*)pBlock);
      }
    } else {  // level == TASK_LEVEL__SINK
      streamFreeQitem((SStreamQueueItem*)pBlock);
    }
  } else {  // non-dispatch task, do task state transfer directly
    streamFreeQitem((SStreamQueueItem*)pBlock);
    stDebug("s-task:%s non-dispatch task, level:%d start to transfer state directly", id, level);

    code = streamTransferStatePrepare(pTask);
    if (code != TSDB_CODE_SUCCESS) {
      stError("s-task:%s failed to prepare transfer state, code:%s", id, tstrerror(code));
      int8_t status = streamTaskSetSchedStatusInactive(pTask);  // let's ignore this return status
    }
  }
}

// static void streamTaskSetIdleInfo(SStreamTask* pTask, int32_t idleTime) { pTask->status.schedIdleTime = idleTime; }
static void setLastExecTs(SStreamTask* pTask, int64_t ts) { pTask->status.lastExecTs = ts; }

static void doRecordThroughput(STaskExecStatisInfo* pInfo, int64_t totalBlocks, int64_t totalSize, int64_t blockSize,
                               double st, const char* id) {
  double el = (taosGetTimestampMs() - st) / 1000.0;

  stDebug("s-task:%s batch of input blocks exec end, elapsed time:%.2fs, result size:%.2fMiB, numOfBlocks:%" PRId64, id,
          el, SIZE_IN_MiB(totalSize), totalBlocks);

  pInfo->outputDataBlocks += totalBlocks;
  pInfo->outputDataSize += totalSize;
  if (fabs(el - 0.0) <= DBL_EPSILON) {
    pInfo->procsThroughput = 0;
    pInfo->outputThroughput = 0;
  } else {
    pInfo->outputThroughput = (totalSize / el);
    pInfo->procsThroughput = (blockSize / el);
  }
}

static int32_t doStreamTaskExecImpl(SStreamTask* pTask, SStreamQueueItem* pBlock, int32_t num) {
  const char*      id = pTask->id.idStr;
  int32_t          blockSize = 0;
  int64_t          st = taosGetTimestampMs();
  SCheckpointInfo* pInfo = &pTask->chkInfo;
  int64_t          ver = pInfo->processedVer;
  int64_t          totalSize = 0;
  int32_t          totalBlocks = 0;
  int32_t          code = 0;

  stDebug("s-task:%s start to process batch blocks, num:%d, type:%s", id, num, streamQueueItemGetTypeStr(pBlock->type));
  code = doSetStreamInputBlock(pTask, pBlock, &ver, id);
  if (code) {
    stError("s-task:%s failed to set input block, not exec for these blocks", id);
    return code;
  }

  code = streamTaskExecImpl(pTask, pBlock, &totalSize, &totalBlocks);
  if (code) {
    return code;
  }

  doRecordThroughput(&pTask->execInfo, totalBlocks, totalSize, blockSize, st, pTask->id.idStr);

  // update the currentVer if processing the submitted blocks.
  if (!(pInfo->checkpointVer <= pInfo->nextProcessVer && ver >= pInfo->checkpointVer)) {
    stError("s-task:%s invalid info, checkpointVer:%" PRId64 ", nextProcessVer:%" PRId64 " currentVer:%" PRId64, id,
            pInfo->checkpointVer, pInfo->nextProcessVer, ver);
    return code;
  }

  if (ver != pInfo->processedVer) {
    stDebug("s-task:%s update processedVer(unsaved) from %" PRId64 " to %" PRId64 " nextProcessVer:%" PRId64
            " ckpt:%" PRId64,
            id, pInfo->processedVer, ver, pInfo->nextProcessVer, pInfo->checkpointVer);
    pInfo->processedVer = ver;
  }

  return code;
}

// do nothing after sync executor state to storage backend, untill checkpoint is completed.
static int32_t doHandleChkptBlock(SStreamTask* pTask) {
  int32_t     code = 0;
  const char* id = pTask->id.idStr;

  streamMutexLock(&pTask->lock);
  SStreamTaskState pState = streamTaskGetStatus(pTask);
  streamMutexUnlock(&pTask->lock);

  if (pState.state == TASK_STATUS__CK) {  // todo other thread may change the status
    stDebug("s-task:%s checkpoint block received, set status:%s", id, pState.name);
    code = streamTaskBuildCheckpoint(pTask);  // ignore this error msg, and continue
  } else {                                    // todo refactor
    if (pTask->info.taskLevel == TASK_LEVEL__SOURCE) {
      code = streamTaskSendCheckpointSourceRsp(pTask);
    } else {
      code = streamTaskSendCheckpointReadyMsg(pTask);
    }

    if (code != TSDB_CODE_SUCCESS) {
      // todo: let's retry send rsp to upstream/mnode
      stError("s-task:%s failed to send checkpoint rsp to upstream, checkpointId:%d, code:%s", id, 0,
              tstrerror(code));
    }
  }

//  streamMutexUnlock(&pTask->lock);
  return code;
}

int32_t flushStateDataInExecutor(SStreamTask* pTask, SStreamQueueItem* pCheckpointBlock) {
  const char* id = pTask->id.idStr;

  // 1. transfer the ownership of executor state
  bool dropRelHTask = (streamTaskGetPrevStatus(pTask) == TASK_STATUS__HALT);
  if (dropRelHTask) {
    STaskId*     pHTaskId = &pTask->hTaskInfo.id;
    SStreamTask* pHTask = NULL;
    int32_t      code = streamMetaAcquireTask(pTask->pMeta, pHTaskId->streamId, pHTaskId->taskId, &pHTask);
    if (code == TSDB_CODE_SUCCESS) {  // ignore the error code.
      code = streamTaskReleaseState(pHTask);
      if (code) {
        stError("s-task:%s failed to release query state, code:%s", pHTask->id.idStr, tstrerror(code));
      }

      if (code == TSDB_CODE_SUCCESS) {
        code = streamTaskReloadState(pTask);
        if (code) {
          stError("s-task:%s failed to reload query state, code:%s", pTask->id.idStr, tstrerror(code));
        }
      }

      stDebug("s-task:%s transfer state from fill-history task:%s, status:%s completed", id, pHTask->id.idStr,
              streamTaskGetStatus(pHTask).name);
      // todo execute qExecTask to fetch the reload-generated result, if this is stream is for session window query.
      /*
       * while(1) {
       * qExecTask()
       * }
       * // put into the output queue.
       */
      streamMetaReleaseTask(pTask->pMeta, pHTask);
    } else {
      stError("s-task:%s related fill-history task:0x%x failed to acquire, transfer state failed", id,
              (int32_t)pHTaskId->taskId);
    }
  } else {
    stDebug("s-task:%s no transfer-state needed", id);
  }

  // 2. flush data in executor to K/V store, which should be completed before do checkpoint in the K/V.
  int32_t code = doStreamTaskExecImpl(pTask, pCheckpointBlock, 1);
  if (code) {
    stError("s-task:%s failed to exec stream task before checkpoint, code:%s", id, tstrerror(code));
  }

  return code;
}

/**
 * todo: the batch of blocks should be tuned dynamic, according to the total elapsed time of each batch of blocks, the
 * appropriate batch of blocks should be handled in 5 to 10 sec.
 */
static int32_t doStreamExecTask(SStreamTask* pTask) {
  const char* id = pTask->id.idStr;
  int32_t     code = 0;
  int32_t     vgId = pTask->pMeta->vgId;
  int32_t     taskLevel = pTask->info.taskLevel;
  int32_t     taskType = pTask->info.fillHistory;

  // merge multiple input data if possible in the input queue.
  int64_t st = taosGetTimestampMs();
  stDebug("s-task:%s start to extract data block from inputQ, ts:%" PRId64, id, st);

  while (1) {
    int32_t           blockSize = 0;
    int32_t           numOfBlocks = 0;
    SStreamQueueItem* pInput = NULL;

    if (streamTaskShouldStop(pTask) || (streamTaskGetStatus(pTask).state == TASK_STATUS__UNINIT)) {
      stDebug("s-task:%s stream task is stopped", id);
      return 0;
    }

    if (streamQueueIsFull(pTask->outputq.queue)) {
      stTrace("s-task:%s outputQ is full, idle for 500ms and retry", id);
      streamTaskSetIdleInfo(pTask, 1000);
      return 0;
    }

    if (pTask->inputq.status == TASK_INPUT_STATUS__BLOCKED) {
      stTrace("s-task:%s downstream task inputQ blocked, idle for 1sec and retry", id);
      streamTaskSetIdleInfo(pTask, 1000);
      return 0;
    }

    if (taosGetTimestampMs() - pTask->status.lastExecTs < MIN_INVOKE_INTERVAL) {
      stDebug("s-task:%s invoke exec too fast, idle and retry in 50ms", id);
      streamTaskSetIdleInfo(pTask, MIN_INVOKE_INTERVAL);
      return 0;
    }

    EExtractDataCode ret = streamTaskGetDataFromInputQ(pTask, &pInput, &numOfBlocks, &blockSize);
    if (ret == EXEC_AFTER_IDLE) {
      streamTaskSetIdleInfo(pTask, MIN_INVOKE_INTERVAL);
      return 0;
    } else {
      if (pInput == NULL) {
        return 0;
      }
    }

    pTask->execInfo.inputDataBlocks += numOfBlocks;
    pTask->execInfo.inputDataSize += blockSize;

    // dispatch checkpoint msg to all downstream tasks
    int32_t type = pInput->type;
    if (type == STREAM_INPUT__CHECKPOINT_TRIGGER) {
#if 0
      // Injection error: for automatic kill long trans test
      taosMsleep(50*1000);
#endif
      code = streamProcessCheckpointTriggerBlock(pTask, (SStreamDataBlock*)pInput);
      if (code != 0) {
        stError("s-task:%s failed to process checkpoint-trigger block, code:%s", pTask->id.idStr, tstrerror(code));
      }
      continue;
    }

    if (type == STREAM_INPUT__TRANS_STATE) {
      streamProcessTransstateBlock(pTask, (SStreamDataBlock*)pInput);
      continue;
    }

    if (type == STREAM_INPUT__CHECKPOINT) {
      code = doHandleChkptBlock(pTask);
      streamFreeQitem(pInput);
      return code;
    }

    if (taskLevel == TASK_LEVEL__SINK) {
      if (type != STREAM_INPUT__DATA_BLOCK && type != STREAM_INPUT__RECALCULATE) {
        stError("s-task:%s invalid block type:%d for sink task, discard", id, type);
        continue;
      }

      // here only handle the data block sink operation
      if (type == STREAM_INPUT__DATA_BLOCK) {
        pTask->execInfo.sink.dataSize += blockSize;
        stDebug("s-task:%s sink task start to sink %d blocks, size:%.2fKiB", id, numOfBlocks, SIZE_IN_KiB(blockSize));
        code = doOutputResultBlockImpl(pTask, (SStreamDataBlock*)pInput);
        if (code != TSDB_CODE_SUCCESS) {
          return code;
        }

        double el = (taosGetTimestampMs() - st) / 1000.0;
        pTask->execInfo.procsThroughput = (fabs(el - 0.0) <= DBL_EPSILON) ? 0 : (blockSize / el);
      } else {
        streamFreeQitem((SStreamQueueItem*)pInput);
      }

      continue;
    }

    if (type == STREAM_INPUT__RECALCULATE) {
      if (taskType == STREAM_NORMAL_TASK && taskLevel == TASK_LEVEL__AGG) {
        int32_t remain = streamAlignRecalculateStart(pTask);
        if (remain > 0) {
          streamFreeQitem((SStreamQueueItem*)pInput);
          stDebug("s-task:%s receive upstream recalculate msg, not sent remain:%d", id, remain);
          return code;
        }

        stDebug("s-task:%s all upstream send recalculate msg, continue", id);
      }

      // 1. generate the recalculating snapshot for related recalculate tasks.
      if ((taskType == STREAM_NORMAL_TASK) &&
          ((taskLevel == TASK_LEVEL__AGG) || (taskLevel == TASK_LEVEL__SOURCE && (!pTask->info.hasAggTasks)))) {
        code = doStreamTaskExecImpl(pTask, pInput, numOfBlocks);
      } else if (taskType == STREAM_RECALCUL_TASK && taskLevel == TASK_LEVEL__AGG) {
        // send retrieve to upstream tasks (source tasks, to start to recalculate procedure.
        stDebug("s-task:%s recalculate agg task send retrieve to upstream source tasks", id);
        code = doStreamTaskExecImpl(pTask, pInput, numOfBlocks);
      }
    }

    if (type != STREAM_INPUT__RECALCULATE) {
      code = doStreamTaskExecImpl(pTask, pInput, numOfBlocks);
      streamFreeQitem(pInput);
      if (code) {
        return code;
      }
    }

    // for stream with only 1 task, start related re-calculate stream task directly.
    // We only start the re-calculate agg task here, and do NOT start the source task, for streams with agg-tasks.
    if ((type == STREAM_INPUT__RECALCULATE) && (taskType == STREAM_NORMAL_TASK)) {
      SSDataBlock* pb = taosArrayGet(((SStreamDataBlock*)pInput)->blocks, 0);

      if ((taskLevel == TASK_LEVEL__AGG) || ((taskLevel == TASK_LEVEL__SOURCE) && (!pTask->info.hasAggTasks))) {
        EStreamType blockType = pb->info.type;

        if (pTask->hTaskInfo.id.streamId == 0) {
          stError("s-task:%s related re-calculate stream task is dropping, failed to start re-calculate", id);
          streamFreeQitem(pInput);
          return TSDB_CODE_STREAM_INTERNAL_ERROR;
        }

        if (pTask->info.trigger != STREAM_TRIGGER_CONTINUOUS_WINDOW_CLOSE) {
          stError("s-task:%s invalid trigger model, expect:%d, actually:%d, not exec tasks", id,
                  STREAM_TRIGGER_CONTINUOUS_WINDOW_CLOSE, pTask->info.trigger);
          streamFreeQitem(pInput);
          return TSDB_CODE_STREAM_INTERNAL_ERROR;
        }

        SStreamTask* pHTask = NULL;
        code = streamMetaAcquireTask(pTask->pMeta, pTask->hTaskInfo.id.streamId, pTask->hTaskInfo.id.taskId, &pHTask);
        if (code != 0) {
          stError("s-task:%s failed to acquire related recalculate task:0x%x, not start the recalculation, code:%s", id,
                  (int32_t)pTask->hTaskInfo.id.taskId, tstrerror(code));
          streamFreeQitem(pInput);
          return code;
        }

        if (blockType == STREAM_RECALCULATE_START) {
          // start the related recalculate task to do recalculate
          stDebug("s-task:%s start recalculate task to do recalculate:0x%x", id, pHTask->id.taskId);

          if (taskLevel == TASK_LEVEL__SOURCE) {
            code = streamStartScanHistoryAsync(pHTask, 0);
          } else {  // for agg task, in normal stream queue to execute
            SStreamDataBlock* pRecalBlock = NULL;
            code = streamCreateRecalculateBlock(pTask, &pRecalBlock, STREAM_RECALCULATE_START);
            if (code) {
              stError("s-task:%s failed to generate recalculate block, code:%s", id, tstrerror(code));
            } else {
              code = streamTaskPutDataIntoInputQ(pHTask, (SStreamQueueItem*)pRecalBlock);
              if (code != TSDB_CODE_SUCCESS) {
                stError("s-task:%s failed to put recalculate block into q, code:%s", pTask->id.idStr, tstrerror(code));
              } else {
                stDebug("s-task:%s put recalculate block into inputQ", pHTask->id.idStr);
              }
              code = streamTrySchedExec(pHTask, false);
            }
          }
        }
        streamMetaReleaseTask(pTask->pMeta, pHTask);
      } else if ((taskLevel == TASK_LEVEL__SOURCE) && pTask->info.hasAggTasks) {
        code = continueDispatchRecalculateStart((SStreamDataBlock*)pInput, pTask);
        pInput = NULL;
      }
    }

    if (type == STREAM_INPUT__RECALCULATE) {
      streamFreeQitem(pInput);
    }

    if (code) {
      return code;
    }

    if (taskType == STREAM_RECALCUL_TASK && taskLevel == TASK_LEVEL__AGG && type != STREAM_INPUT__RECALCULATE) {
      bool complete = qStreamScanhistoryFinished(pTask->exec.pExecutor);
      if (complete) {
        stDebug("s-task:%s recalculate agg task complete recalculate procedure", id);
        return 0;
      }
    }

    double el = (taosGetTimestampMs() - st) / 1000.0;
    if (el > 2.0) {  // elapsed more than 5 sec, not occupy the CPU anymore
      stDebug("s-task:%s occupy more than 2.0s, release the exec threads and idle for 500ms", id);
      streamTaskSetIdleInfo(pTask, 500);
      return code;
    }
  }

  
}

// the task may be set dropping/stopping, while it is still in the task queue, therefore, the sched-status can not
// be updated by tryExec function, therefore, the schedStatus will always be the TASK_SCHED_STATUS__WAITING.
bool streamTaskIsIdle(const SStreamTask* pTask) {
  ETaskStatus status = streamTaskGetStatus(pTask).state;
  return (pTask->status.schedStatus == TASK_SCHED_STATUS__INACTIVE || status == TASK_STATUS__STOP ||
          status == TASK_STATUS__DROPPING);
}

bool streamTaskReadyToRun(const SStreamTask* pTask, char** pStatus) {
  SStreamTaskState pState = streamTaskGetStatus(pTask);

  ETaskStatus st = pState.state;
  if (pStatus != NULL) {
    *pStatus = pState.name;
  }

  // pause & halt will still run for sink tasks.
  if (streamTaskIsSinkTask(pTask)) {
    return (st == TASK_STATUS__READY || st == TASK_STATUS__SCAN_HISTORY || st == TASK_STATUS__CK ||
            st == TASK_STATUS__PAUSE || st == TASK_STATUS__HALT);
  } else {
    return (st == TASK_STATUS__READY || st == TASK_STATUS__SCAN_HISTORY || st == TASK_STATUS__CK ||
            st == TASK_STATUS__HALT);
  }
}

static bool shouldNotCont(SStreamTask* pTask) {
  int32_t       level = pTask->info.taskLevel;
  SStreamQueue* pQueue = pTask->inputq.queue;
  ETaskStatus   status = streamTaskGetStatus(pTask).state;

  // 1. task should jump out
  bool quit = (status == TASK_STATUS__STOP) || (status == TASK_STATUS__PAUSE) || (status == TASK_STATUS__DROPPING);

  // 2. checkpoint procedure, the source task's checkpoint queue is empty, not read from ordinary queue
  bool emptyCkQueue = (taosQueueItemSize(pQueue->pChkptQueue) == 0);

  // 3. no data in ordinary queue
  bool emptyBlockQueue = (streamQueueGetNumOfItems(pQueue) == 0);

  if (quit) {
    return true;
  } else {
    if (status == TASK_STATUS__CK && level == TASK_LEVEL__SOURCE) {
      // in checkpoint procedure, we only check whether the controller queue is empty or not
      return emptyCkQueue;
    } else { // otherwise, if the block queue is empty, not continue.
      return emptyBlockQueue && emptyCkQueue;
    }
  }
}

int32_t streamResumeTask(SStreamTask* pTask) {
  const char* id = pTask->id.idStr;
  int32_t     level = pTask->info.taskLevel;
  int32_t     code = 0;

  if (pTask->status.schedStatus != TASK_SCHED_STATUS__ACTIVE) {
    stError("s-task:%s invalid sched status:%d, not resume task", pTask->id.idStr, pTask->status.schedStatus);
    return code;
  }

  while (1) {
    code = doStreamExecTask(pTask);
    if (code) {
      stError("s-task:%s failed to exec stream task, code:%s, continue", id, tstrerror(code));
    }

    streamMutexLock(&pTask->lock);

    if (shouldNotCont(pTask)) {
      atomic_store_8(&pTask->status.schedStatus, TASK_SCHED_STATUS__INACTIVE);
      streamTaskClearSchedIdleInfo(pTask);
      streamMutexUnlock(&pTask->lock);

      setLastExecTs(pTask, taosGetTimestampMs());

      char* p = streamTaskGetStatus(pTask).name;
      stDebug("s-task:%s exec completed, status:%s, sched-status:%d, lastExecTs:%" PRId64, id, p,
              pTask->status.schedStatus, pTask->status.lastExecTs);

      return code;
    } else {
      // check if this task needs to be idle for a while
      if (pTask->status.schedIdleTime > 0) {
        streamTaskResumeInFuture(pTask);

        streamMutexUnlock(&pTask->lock);
        setLastExecTs(pTask, taosGetTimestampMs());
        return code;
      }
    }

    streamMutexUnlock(&pTask->lock);
  }

  return code;
}

int32_t streamExecTask(SStreamTask* pTask) {
  // this function may be executed by multi-threads, so status check is required.
  const char* id = pTask->id.idStr;
  int32_t     code = 0;

  int8_t schedStatus = streamTaskSetSchedStatusActive(pTask);
  if (schedStatus == TASK_SCHED_STATUS__WAITING) {
    code = streamResumeTask(pTask);
  } else {
    char* p = streamTaskGetStatus(pTask).name;
    stDebug("s-task:%s already started to exec by other thread, status:%s, sched-status:%d", id, p,
            pTask->status.schedStatus);
  }

  return code;
}

int32_t streamTaskReleaseState(SStreamTask* pTask) {
  stDebug("s-task:%s release exec state", pTask->id.idStr);
  void* pExecutor = pTask->exec.pExecutor;

  int32_t code = TSDB_CODE_SUCCESS;
  if (pExecutor != NULL) {
    code = qStreamOperatorReleaseState(pExecutor);
  }

  return code;
}

int32_t streamTaskReloadState(SStreamTask* pTask) {
  stDebug("s-task:%s reload exec state", pTask->id.idStr);
  void* pExecutor = pTask->exec.pExecutor;

  int32_t code = TSDB_CODE_SUCCESS;
  if (pExecutor != NULL) {
    code = qStreamOperatorReloadState(pExecutor);
  }

  return code;
}

int32_t streamAlignTransferState(SStreamTask* pTask) {
  int32_t numOfUpstream = taosArrayGetSize(pTask->upstreamInfo.pList);
  int32_t old = atomic_val_compare_exchange_32(&pTask->transferStateAlignCnt, 0, numOfUpstream);
  if (old == 0) {
    stDebug("s-task:%s set the transfer state aligncnt %d", pTask->id.idStr, numOfUpstream);
  }

  return atomic_sub_fetch_32(&pTask->transferStateAlignCnt, 1);
}

int32_t streamAlignRecalculateStart(SStreamTask* pTask) {
  int32_t numOfUpstream = taosArrayGetSize(pTask->upstreamInfo.pList);
  int32_t old = atomic_val_compare_exchange_32(&pTask->recalculateAlignCnt, 0, numOfUpstream);
  if (old == 0) {
    stDebug("s-task:%s set start recalculate state aligncnt %d", pTask->id.idStr, numOfUpstream);
  }

  return atomic_sub_fetch_32(&pTask->recalculateAlignCnt, 1);
}

int32_t continueDispatchRecalculateStart(SStreamDataBlock* pBlock, SStreamTask* pTask) {
  pBlock->srcTaskId = pTask->id.taskId;
  pBlock->srcVgId = pTask->pMeta->vgId;

  int32_t code = taosWriteQitem(pTask->outputq.queue->pQueue, pBlock);
  if (code == 0) {
    code = streamDispatchStreamBlock(pTask);
  } else {
    stError("s-task:%s failed to put recalculate start block into outputQ, code:%s", pTask->id.idStr, tstrerror(code));
    streamFreeQitem((SStreamQueueItem*)pBlock);
  }

  return code;
}
