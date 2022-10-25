/*-------------------------------------------------------------------------
 *
 * pg_relusage is a PostgreSQL extension which allows to log relations that were
 * used by the current statement.
 *
 * This code is based on Bruce Momijan lectures and auto_explain.
 *
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *
 * Copyright (c) 2022, Dmitry Astapov <dastapov@gmail.com>
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "executor/executor.h"

#include "utils/lsyscache.h"
#include "nodes/pg_list.h"
#if PG_VERSION_NUM >= 90600
#include "access/parallel.h"
#endif

PG_MODULE_MAGIC;

static int	pg_relusage_log_level = LOG;

static ExecutorStart_hook_type prev_ExecutorStart = NULL;

void		_PG_init(void);
void		_PG_fini(void);

static void pg_relusage_ExecutorStart(QueryDesc *queryDesc, int eflags);

void
_PG_init(void)
{
  //EmitWarningsOnPlaceholders("pg_relusage");

  /* Install hooks only on leader. */
#if PG_VERSION_NUM >= 90600
  if (!IsParallelWorker())
    {
#endif
      prev_ExecutorStart = ExecutorStart_hook;
      ExecutorStart_hook = pg_relusage_ExecutorStart;
#if PG_VERSION_NUM >= 90600
    }
#endif
}

void
_PG_fini(void)
{
#if PG_VERSION_NUM >= 90600
  if (!IsParallelWorker())
    {
#endif
      ExecutorStart_hook = prev_ExecutorStart;
#if PG_VERSION_NUM >= 90600
    }
#endif
}

static void
pg_relusage_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
  ListCell   *lst;
  ListCell   *lst2;
  Oid oid;
  StringInfoData buf;
  bool buf_empty=true;
  char kind;
    
  initStringInfo(&buf);

  foreach(lst, queryDesc -> plannedstmt -> relationOids) {
    bool seen = false;
    oid = lfirst_oid(lst);
    kind = get_rel_relkind(oid);

    // Exclude things with unknown relkind, and ignore indices and TOAST tables
    if (!kind || kind == 'i' || kind == 'I' || kind == 't') continue;
    
    // Oid list is not deduplicated so we check if the same oid was mentioned before
    foreach(lst2, queryDesc -> plannedstmt -> relationOids) {
      if(lst2 == lst)
        break;
      if(oid == lfirst_oid(lst2)) 
        seen = true;
    }
    
    if (!seen) {
      if (buf_empty) {
        buf_empty=false;
      } else {
        appendStringInfoChar(&buf, ',');
      };
      appendStringInfoString(&buf, get_rel_name(oid));
    }
    
  };

  if (!buf_empty) {
    ereport(pg_relusage_log_level, (errmsg("relations used: %s", buf.data), errhidestmt(true)));
  };
  
  pfree(buf.data);
  
  if (prev_ExecutorStart)
    prev_ExecutorStart(queryDesc, eflags);
  else
    standard_ExecutorStart(queryDesc, eflags);
}
