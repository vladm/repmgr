/*
 * repmgr_wrapper_funcs.c
 * Copyright (c) 2ndQuadrant, 2010
 *
 * Expose some backend functions in SQL
 */

#include "postgres.h"
#include "fmgr.h"
#include "access/xlog.h"
#include "storage/procarray.h"

PG_MODULE_MAGIC;

Datum last_xlog_replay_timestamp(PG_FUNCTION_ARGS);
Datum oldest_xmin(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(last_xlog_replay_timestamp);

Datum
last_xlog_replay_timestamp(PG_FUNCTION_ARGS)
{
	if (!InRecovery)
		PG_RETURN_NULL();
	else
	    PG_RETURN_TIMESTAMPTZ(GetLatestXLogTime());
}


PG_FUNCTION_INFO_V1(oldest_xmin);

Datum
oldest_xmin(PG_FUNCTION_ARGS)
{
	    PG_RETURN_INT64(GetOldestXmin(false, false));
}
