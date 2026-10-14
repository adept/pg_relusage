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

#include "access/htup_details.h"
#include "utils/syscache.h"
#include "catalog/pg_class.h"
#include "nodes/pg_list.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "catalog/partition.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "catalog/namespace.h"
#include "catalog/catalog.h"
#include "catalog/pg_namespace.h"
#include "utils/guc.h"
#if PG_VERSION_NUM >= 90600
#include "access/parallel.h"
#endif

PG_MODULE_MAGIC;

static int	pg_relusage_log_level = LOG;

static const struct config_enum_entry log_level_options[] = {
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"debug", DEBUG2, true},
	{"info", INFO, false},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{"log", LOG, false},
	{NULL, 0, false}
};

static char*   pg_relusage_rel_kinds = "riSvmfp";

static ExecutorStart_hook_type prev_ExecutorStart = NULL;

void		_PG_init(void);
void		_PG_fini(void);

static void pg_relusage_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pg_relusage_log_relations(QueryDesc *queryDesc);
static bool pg_relusage_plan_walker(PlanState *ps, void *context);

void
_PG_init(void)
{
  DefineCustomEnumVariable("pg_relusage.log_level",
                           "Log level for pg_relusage.",
                           NULL,
                           &pg_relusage_log_level,
                           LOG,
                           log_level_options,
                           PGC_SUSET,
                           0,
                           NULL,
                           NULL,
                           NULL);

  DefineCustomStringVariable("pg_relusage.rel_kinds",
                             "rel_kinds that pg_relusage will report",
                             "pg_class.relkind of the relation will be checked again this string, and if there is a match, relation will be reported",
                             &pg_relusage_rel_kinds,
                             "riSvmfp",
                             PGC_SUSET,
                             0,
                             NULL,
                             NULL,
                             NULL);

  EmitWarningsOnPlaceholders("pg_relusage");

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

// State for the walk over the plan tree
typedef struct
{
  List *rtable;        // range table of the statement
  List *scanned;       // relations scanned or modified by the plan
  List *indexes;       // indexes used by the plan
  bool unknown_node;   // plan contains a node type we do not know about
} pg_relusage_walk_ctx;

static void
pg_relusage_add_rti(pg_relusage_walk_ctx *ctx, Index rti)
{
  RangeTblEntry *rte;

  if (rti == 0) return;
  rte = rt_fetch(rti, ctx->rtable);
  if (rte->rtekind == RTE_RELATION)
    ctx->scanned = list_append_unique_oid(ctx->scanned, rte->relid);
}

static void
pg_relusage_add_rtis(pg_relusage_walk_ctx *ctx, Bitmapset *rtis)
{
  int rti = -1;

  while ((rti = bms_next_member(rtis, rti)) >= 0)
    pg_relusage_add_rti(ctx, rti);
}

// Collect relations and indexes that the plan actually scans or modifies.
// Walks the initialized plan state tree (including subplans and initplans), so
// partitions pruned at executor startup are not included.
static bool
pg_relusage_plan_walker(PlanState *ps, void *context)
{
  pg_relusage_walk_ctx *ctx = (pg_relusage_walk_ctx *) context;
  Plan *plan = ps->plan;
  ListCell *lst;

  switch (nodeTag(plan)) {
    case T_SeqScan:
    case T_SampleScan:
    case T_BitmapHeapScan:
    case T_TidScan:
#if PG_VERSION_NUM >= 140000
    case T_TidRangeScan:
#endif
      pg_relusage_add_rti(ctx, ((Scan *) plan)->scanrelid);
      break;
    case T_IndexScan:
      pg_relusage_add_rti(ctx, ((IndexScan *) plan)->scan.scanrelid);
      ctx->indexes = list_append_unique_oid(ctx->indexes, ((IndexScan *) plan)->indexid);
      break;
    case T_IndexOnlyScan:
      pg_relusage_add_rti(ctx, ((IndexOnlyScan *) plan)->scan.scanrelid);
      ctx->indexes = list_append_unique_oid(ctx->indexes, ((IndexOnlyScan *) plan)->indexid);
      break;
    case T_BitmapIndexScan:
      ctx->indexes = list_append_unique_oid(ctx->indexes, ((BitmapIndexScan *) plan)->indexid);
      break;
    case T_ForeignScan:
      // scanrelid is 0 when a join is pushed down to the remote server
      pg_relusage_add_rti(ctx, ((ForeignScan *) plan)->scan.scanrelid);
#if PG_VERSION_NUM >= 160000
      pg_relusage_add_rtis(ctx, ((ForeignScan *) plan)->fs_base_relids);
#else
      pg_relusage_add_rtis(ctx, ((ForeignScan *) plan)->fs_relids);
#endif
      break;
    case T_CustomScan:
      pg_relusage_add_rti(ctx, ((CustomScan *) plan)->scan.scanrelid);
      pg_relusage_add_rtis(ctx, ((CustomScan *) plan)->custom_relids);
      break;
    case T_ModifyTable:
      // Target relations of INSERT/UPDATE/DELETE/MERGE
      foreach(lst, ((ModifyTable *) plan)->resultRelations)
        pg_relusage_add_rti(ctx, lfirst_int(lst));
      break;

    // Nodes that do not scan relations by themselves
    case T_Result:
    case T_ProjectSet:
    case T_Append:
    case T_MergeAppend:
    case T_RecursiveUnion:
    case T_BitmapAnd:
    case T_BitmapOr:
    case T_SubqueryScan:
    case T_FunctionScan:
    case T_ValuesScan:
    case T_TableFuncScan:
    case T_CteScan:
    case T_NamedTuplestoreScan:
    case T_WorkTableScan:
    case T_NestLoop:
    case T_MergeJoin:
    case T_HashJoin:
    case T_Material:
#if PG_VERSION_NUM >= 140000
    case T_Memoize:
#endif
    case T_Sort:
#if PG_VERSION_NUM >= 130000
    case T_IncrementalSort:
#endif
    case T_Group:
    case T_Agg:
    case T_WindowAgg:
    case T_Unique:
    case T_Gather:
    case T_GatherMerge:
    case T_Hash:
    case T_SetOp:
    case T_LockRows:
    case T_Limit:
      break;

    default:
      ctx->unknown_node = true;
      break;
  }

  return planstate_tree_walker(ps, pg_relusage_plan_walker, context);
}

// Partitioned tables are never scanned themselves, only their partitions are.
// Add partitioned ancestors of all scanned partitions.
static void
pg_relusage_add_partition_ancestors(pg_relusage_walk_ctx *ctx)
{
  List *ancestors = NIL;
  ListCell *lst;

  foreach(lst, ctx->scanned) {
    Oid oid = lfirst_oid(lst);

    if (!get_rel_relispartition(oid)) continue;
#if PG_VERSION_NUM >= 110000
    ancestors = list_concat_unique_oid(ancestors, get_partition_ancestors(oid));
#else
    while (get_rel_relispartition(oid)) {
      oid = get_partition_parent(oid);
      ancestors = list_append_unique_oid(ancestors, oid);
    }
#endif
  }

  ctx->scanned = list_concat_unique_oid(ctx->scanned, ancestors);
  list_free(ancestors);
}

// Append name of the relation to the buffer, if it is of the kind we report
static void
pg_relusage_append_relation(StringInfo buf, Oid oid)
{
  HeapTuple tp;
  Form_pg_class reltup;

  // Fetch relkind and relname from a single catalog lookup, so that they are
  // consistent with each other. Relation could have been dropped concurrently.
  tp = SearchSysCache1(RELOID, ObjectIdGetDatum(oid));
  if (!HeapTupleIsValid(tp)) return;
  reltup = (Form_pg_class) GETSTRUCT(tp);

  // System catalogs (pg_catalog, pg_toast, information_schema) are never
  // reported: they cannot be dropped, and queries from tools (psql, ORMs, etc.)
  // would flood the log with them
  if (strchr(pg_relusage_rel_kinds, reltup->relkind)
      && reltup->relnamespace != PG_CATALOG_NAMESPACE
      && !IsToastNamespace(reltup->relnamespace)) {
    // Temporary schemas are named pg_temp_N, where N varies between sessions,
    // so we report all of them as pg_temp
    char *nspname = isAnyTempNamespace(reltup->relnamespace)
      ? "pg_temp" : get_namespace_name(reltup->relnamespace);

    if (nspname && strcmp(nspname, "information_schema") == 0) {
      ReleaseSysCache(tp);
      return;
    }

    // Relations in "public" are reported without schema, to keep logs smaller
    if (nspname && strcmp(nspname, "public") == 0)
      nspname = NULL;

    if (buf->len > 0)
      appendStringInfoChar(buf, ',');
    // Schema-qualified, with quotes added where necessary
    appendStringInfoString(buf, quote_qualified_identifier(nspname, NameStr(reltup->relname)));
  }

  ReleaseSysCache(tp);
}

// Relations used by the statement are the ones its plan depends on
// (plannedstmt->relationOids), except for the relations from the range table
// that the final plan does not scan (e.g. eliminated by join removal or by
// partition pruning). Views and relations referenced via regclass constants
// (e.g. sequences used by nextval()) are always reported. Indexes used by the
// plan are reported as well.
static void
pg_relusage_log_relations(QueryDesc *queryDesc)
{
  PlannedStmt *stmt = queryDesc->plannedstmt;
  pg_relusage_walk_ctx ctx = {stmt->rtable, NIL, NIL, false};
  List *in_rtable = NIL;   // relations that the plan is expected to scan
  bool has_partitioned = false;
  List *reported = NIL;
  ListCell *lst;
  StringInfoData buf;

  foreach(lst, stmt->rtable) {
    RangeTblEntry *rte = (RangeTblEntry *) lfirst(lst);

    // Views are expanded by the rewriter and are never scanned
    if (rte->rtekind != RTE_RELATION || rte->relkind == RELKIND_VIEW) continue;
    in_rtable = list_append_unique_oid(in_rtable, rte->relid);
    if (rte->relkind == RELKIND_PARTITIONED_TABLE) has_partitioned = true;
  }

  if (queryDesc->planstate)
    pg_relusage_plan_walker(queryDesc->planstate, &ctx);
  if (has_partitioned)
    pg_relusage_add_partition_ancestors(&ctx);

  initStringInfo(&buf);

  foreach(lst, stmt->relationOids) {
    Oid oid = lfirst_oid(lst);

    // Oid list is not deduplicated
    if (list_member_oid(reported, oid)) continue;

    // Skip relations eliminated from the plan. If the plan contains node types
    // we do not know about, be conservative and report everything.
    if (list_member_oid(in_rtable, oid) && !list_member_oid(ctx.scanned, oid)
        && !ctx.unknown_node)
      continue;

    reported = lappend_oid(reported, oid);
    pg_relusage_append_relation(&buf, oid);
  }

  foreach(lst, ctx.indexes) {
    Oid oid = lfirst_oid(lst);

    if (list_member_oid(reported, oid)) continue;
    reported = lappend_oid(reported, oid);
    pg_relusage_append_relation(&buf, oid);
  }

  if (buf.len > 0) {
    ereport(pg_relusage_log_level, (errmsg("relations used: %s", buf.data), errhidestmt(true)));
  };

  pfree(buf.data);
  list_free(in_rtable);
  list_free(reported);
  list_free(ctx.scanned);
  list_free(ctx.indexes);
}

static void
pg_relusage_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
  if (prev_ExecutorStart)
    prev_ExecutorStart(queryDesc, eflags);
  else
    standard_ExecutorStart(queryDesc, eflags);

  // Log only after the executor started successfully (e.g. permission checks
  // passed). EXPLAIN without ANALYZE does not execute the query, so nothing is used
  if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
    pg_relusage_log_relations(queryDesc);
}
