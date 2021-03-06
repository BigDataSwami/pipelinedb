/* Copyright (c) 2013-2015 PipelineDB */
/*-------------------------------------------------------------------------
 *
 * worker.c
 *
 *	  Worker process functionality
 *
 * src/backend/pipeline/worker.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include <time.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pipeline_query.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "pipeline/combiner.h"
#include "pipeline/combinerReceiver.h"
#include "pipeline/cqproc.h"
#include "pipeline/tuplebuf.h"
#include "pipeline/worker.h"
#include "tcop/dest.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "storage/proc.h"
#include "pgstat.h"
#include "utils/timestamp.h"

#define LONG_RUNNING_XACT_DURATION 5000 /* 5s */
#define WAIT_SLEEP_MS 5

/*
 * We keep some resources across transactions, so we attach everything to a
 * long-lived ResourceOwner, which prevents the below commit from thinking that
 * there are reference leaks
 */
static void
start_executor(QueryDesc *queryDesc, MemoryContext context, ResourceOwner owner)
{
	MemoryContext old;
	ResourceOwner save;

	StartTransactionCommand();

	old = MemoryContextSwitchTo(context);

	save = CurrentResourceOwner;
	CurrentResourceOwner = owner;

	queryDesc->snapshot = GetTransactionSnapshot();
	queryDesc->snapshot->copied = true;

	RegisterSnapshotOnOwner(queryDesc->snapshot, owner);

	ExecutorStart(queryDesc, 0);

	queryDesc->snapshot->active_count++;
	UnregisterSnapshotFromOwner(queryDesc->snapshot, owner);
	UnregisterSnapshotFromOwner(queryDesc->estate->es_snapshot, owner);

	CurrentResourceOwner = TopTransactionResourceOwner;

	MemoryContextSwitchTo(old);

	CommitTransactionCommand();

	CurrentResourceOwner = save;
}

static void
set_snapshot(EState *estate, ResourceOwner owner)
{
	estate->es_snapshot = GetTransactionSnapshot();
	estate->es_snapshot->active_count++;
	estate->es_snapshot->copied = true;
	RegisterSnapshotOnOwner(estate->es_snapshot, owner);
	PushActiveSnapshot(estate->es_snapshot);
}

static void
unset_snapshot(EState *estate, ResourceOwner owner)
{
	PopActiveSnapshot();
	UnregisterSnapshotFromOwner(estate->es_snapshot, owner);
}

/*
 * ContinuousQueryWorkerStartup
 *
 * Launches a CQ worker, which continuously generates partial query results to send
 * back to the combiner process.
 */
void
ContinuousQueryWorkerRun(Portal portal, ContinuousViewState *state, QueryDesc *queryDesc, ResourceOwner owner)
{
	EState	   *estate = NULL;
	DestReceiver *dest;
	CmdType		operation;
	MemoryContext oldcontext;
	int timeoutms = state->maxwaitms;
	MemoryContext runcontext;
	CQProcEntry *entry = GetCQProcEntry(MyCQId);
	ResourceOwner cqowner = ResourceOwnerCreate(NULL, "CQResourceOwner");
	bool savereadonly = XactReadOnly;

	cq_stat_initialize(state->viewid, MyProcPid);

	dest = CreateDestReceiver(DestCombiner);
	SetCombinerDestReceiverParams(dest, MyCQId);

	/* workers only need read-only transactions */
	XactReadOnly = true;

	runcontext = AllocSetContextCreate(TopMemoryContext, "CQRunContext",
			ALLOCSET_DEFAULT_MINSIZE,
			ALLOCSET_DEFAULT_INITSIZE,
			ALLOCSET_DEFAULT_MAXSIZE);

	elog(LOG, "\"%s\" worker %d running", queryDesc->plannedstmt->cq_target->relname, MyProcPid);
	MarkWorkerAsRunning(MyCQId, MyWorkerId);
	pgstat_report_activity(STATE_RUNNING, queryDesc->sourceText);

	TupleBufferInitLatch(WorkerTupleBuffer, MyCQId, MyWorkerId, &MyProc->procLatch);

	oldcontext = MemoryContextSwitchTo(runcontext);

retry:
	PG_TRY();
	{
		bool xact_commit = true;
		TimestampTz last_process = GetCurrentTimestamp();
		TimestampTz last_commit = GetCurrentTimestamp();

		start_executor(queryDesc, runcontext, cqowner);

		CurrentResourceOwner = cqowner;

		estate = queryDesc->estate;
		operation = queryDesc->operation;

		/*
		 * Initialize context that lives for the duration of a single iteration
		 * of the main worker loop
		 */
		CQExecutionContext = AllocSetContextCreate(estate->es_query_cxt, "CQExecutionContext",
				ALLOCSET_DEFAULT_MINSIZE,
				ALLOCSET_DEFAULT_INITSIZE,
				ALLOCSET_DEFAULT_MAXSIZE);

		estate->es_lastoid = InvalidOid;

		/*
		 * Startup combiner receiver
		 */
		(*dest->rStartup) (dest, operation, queryDesc->tupDesc);

		for (;;)
		{
			if (!TupleBufferHasUnreadSlots())
			{
				if (TimestampDifferenceExceeds(last_process, GetCurrentTimestamp(), state->emptysleepms))
				{
					/* force stats flush */
					cq_stat_report(true);

					pgstat_report_activity(STATE_IDLE, queryDesc->sourceText);
					TupleBufferWait(WorkerTupleBuffer, MyCQId, MyWorkerId);
					pgstat_report_activity(STATE_RUNNING, queryDesc->sourceText);
				}
				else
					pg_usleep(Min(WAIT_SLEEP_MS, state->emptysleepms) * 1000);
			}

			TupleBufferResetNotify(WorkerTupleBuffer, MyCQId, MyWorkerId);

			if (xact_commit)
				StartTransactionCommand();

			set_snapshot(estate, cqowner);
			CurrentResourceOwner = cqowner;
			MemoryContextSwitchTo(estate->es_query_cxt);

			estate->es_processed = 0;
			estate->es_filtered = 0;

			/*
			 * Run plan on a microbatch
			 */
			ExecutePlan(estate, queryDesc->planstate, operation,
					true, 0, timeoutms, ForwardScanDirection, dest);

			IncrementCQExecutions(1);
			TupleBufferClearPinnedSlots();

			if (state->long_xact)
			{
				if (TimestampDifferenceExceeds(last_commit, GetCurrentTimestamp(), LONG_RUNNING_XACT_DURATION))
					xact_commit = true;
				else
					xact_commit = false;
			}

			unset_snapshot(estate, cqowner);
			if (xact_commit)
			{
				CommitTransactionCommand();
				last_commit = GetCurrentTimestamp();
			}

			MemoryContextResetAndDeleteChildren(CQExecutionContext);
			MemoryContextSwitchTo(runcontext);
			CurrentResourceOwner = cqowner;

			if (estate->es_processed || estate->es_filtered)
			{
				/*
				 * If the CV query is such that the select does not return any tuples
				 * ex: select id where id=99; and id=99 does not exist, then this reset
				 * will fail. What will happen is that the worker will block at the latch for every
				 * allocated slot, TILL a cv returns a non-zero tuple, at which point
				 * the worker will resume a simple sleep for the threshold time.
				 */
				last_process = GetCurrentTimestamp();

				/*
				 * Send stats to the collector
				 */
				cq_stat_report(false);
			}

			/* Has the CQ been deactivated? */
			if (!entry->active)
			{
				if (ActiveSnapshotSet())
					unset_snapshot(estate, cqowner);
				if (IsTransactionState())
					CommitTransactionCommand();
				break;
			}
		}

		CurrentResourceOwner = cqowner;

		/*
		 * The cleanup functions below expect these things to be registered
		 */
		RegisterSnapshotOnOwner(estate->es_snapshot, cqowner);
		RegisterSnapshotOnOwner(queryDesc->snapshot, cqowner);
		RegisterSnapshotOnOwner(queryDesc->crosscheck_snapshot, cqowner);

		/* cleanup */
		ExecutorFinish(queryDesc);
		ExecutorEnd(queryDesc);
		FreeQueryDesc(queryDesc);
	}
	PG_CATCH();
	{
		EmitErrorReport();
		FlushErrorState();

		/* Since the worker is read-only, we can simply commit the transaction. */
		if (ActiveSnapshotSet())
			unset_snapshot(estate, cqowner);
		if (IsTransactionState())
			CommitTransactionCommand();

		TupleBufferUnpinAllPinnedSlots();
		TupleBufferClearReaders();

		/* This resets the es_query_ctx and in turn the CQExecutionContext */
		MemoryContextResetAndDeleteChildren(runcontext);

		IncrementCQErrors(1);

		if (continuous_query_crash_recovery)
			goto retry;
	}
	PG_END_TRY();

	(*dest->rShutdown) (dest);

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(runcontext);

	XactReadOnly = savereadonly;

	/*
	 * Remove proc-level stats
	 */
	cq_stat_report(true);
	cq_stat_send_purge(state->viewid, MyProcPid, CQ_STAT_WORKER);

	CurrentResourceOwner = owner;
}
