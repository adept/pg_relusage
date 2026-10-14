/*-------------------------------------------------------------------------
 *
 * pg_relusage is a PostgreSQL extension which allows one to log relations that were
 * used by the current statement.
 *
 * This code is based on Bruce Momjian lectures and auto_explain.
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
#include "tcop/tcopprot.h"
#include "access/xact.h"
#include "utils/memutils.h"
#include "access/parallel.h"
#include "tcop/utility.h"
#include "nodes/parsenodes.h"

#if PG_VERSION_NUM < 100000
#error "pg_relusage requires PostgreSQL 10 or later"
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

static char *pg_relusage_rel_kinds = "riSvmfp";

static bool pg_relusage_enabled = true;

/*
 * How relations used by nested statements (executed inside functions,
 * triggers, foreign key checks etc.) are reported
 */
#define PG_RELUSAGE_NESTED_ALL  0	/* each statement is reported separately */
#define PG_RELUSAGE_NESTED_TOP  1	/* nested statements are not reported */
#define PG_RELUSAGE_NESTED_FOLD 2	/* reported with the top-level statement */

static int	pg_relusage_nested_statements = PG_RELUSAGE_NESTED_ALL;

static const struct config_enum_entry nested_statements_options[] = {
	{"all", PG_RELUSAGE_NESTED_ALL, false},
	{"top", PG_RELUSAGE_NESTED_TOP, false},
	{"fold", PG_RELUSAGE_NESTED_FOLD, false},
	{NULL, 0, false}
};

/* Current nesting level of executor calls (0 when no statement is running) */
static int	nesting_level = 0;

/* Top-level statement that is currently running */
static QueryDesc *current_top = NULL;

/*
 * Relations collected for a top-level statement in "fold" mode. Names are
 * formatted as they are added, so that they could be logged at the end of
 * transaction, when catalog lookups are no longer possible.
 */
typedef struct
{
	QueryDesc  *queryDesc;
	SubTransactionId subxid;	/* subtransaction the statement started in */
	List	   *oids;			/* relations seen so far (reported or not) */
	StringInfoData names;		/* formatted names of reported relations */
} pg_relusage_pending;

/* List of pg_relusage_pending, allocated in TopMemoryContext */
static List *pending = NIL;

static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

void		_PG_init(void);

static void pg_relusage_ExecutorStart(QueryDesc *queryDesc, int eflags);
#if PG_VERSION_NUM >= 180000
static void pg_relusage_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
									uint64 count);
#else
static void pg_relusage_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
									uint64 count, bool execute_once);
#endif
static void pg_relusage_ExecutorFinish(QueryDesc *queryDesc);
static void pg_relusage_ExecutorEnd(QueryDesc *queryDesc);
static void pg_relusage_xact_callback(XactEvent event, void *arg);
static void pg_relusage_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
										 SubTransactionId parentSubid, void *arg);

#if PG_VERSION_NUM >= 140000
#define PG_RELUSAGE_UTILITY_PARAMS PlannedStmt *pstmt, const char *queryString, \
	bool readOnlyTree, ProcessUtilityContext context, ParamListInfo params, \
	QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc
#define PG_RELUSAGE_UTILITY_ARGS pstmt, queryString, readOnlyTree, context, \
	params, queryEnv, dest, qc
#elif PG_VERSION_NUM >= 130000
#define PG_RELUSAGE_UTILITY_PARAMS PlannedStmt *pstmt, const char *queryString, \
	ProcessUtilityContext context, ParamListInfo params, \
	QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc
#define PG_RELUSAGE_UTILITY_ARGS pstmt, queryString, context, params, \
	queryEnv, dest, qc
#else
#define PG_RELUSAGE_UTILITY_PARAMS PlannedStmt *pstmt, const char *queryString, \
	ProcessUtilityContext context, ParamListInfo params, \
	QueryEnvironment *queryEnv, DestReceiver *dest, char *completionTag
#define PG_RELUSAGE_UTILITY_ARGS pstmt, queryString, context, params, \
	queryEnv, dest, completionTag
#endif

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static void pg_relusage_ProcessUtility(PG_RELUSAGE_UTILITY_PARAMS);
static bool pg_relusage_plan_walker(PlanState *ps, void *context);
static bool pg_relusage_check_rel_kinds(char **newval, void **extra, GucSource source);

/* Valid values of pg_class.relkind */
#define PG_RELUSAGE_VALID_REL_KINDS "riStvmcfpI"

static bool
pg_relusage_check_rel_kinds(char **newval, void **extra, GucSource source)
{
	const char *c;

	for (c = *newval; *c; c++)
	{
		if (!strchr(PG_RELUSAGE_VALID_REL_KINDS, *c))
		{
			GUC_check_errdetail("Unknown relation kind '%c'. Valid relation kinds are: %s.",
								*c, PG_RELUSAGE_VALID_REL_KINDS);
			return false;
		}
	}
	return true;
}

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
							   "Relation kinds that pg_relusage will report.",
							   "String of pg_class.relkind codes. Relation is reported if its relkind is in this string.",
							   &pg_relusage_rel_kinds,
							   "riSvmfp",
							   PGC_SUSET,
							   0,
							   pg_relusage_check_rel_kinds,
							   NULL,
							   NULL);

	DefineCustomBoolVariable("pg_relusage.enabled",
							 "Enables logging of relations used by statements.",
							 NULL,
							 &pg_relusage_enabled,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomEnumVariable("pg_relusage.nested_statements",
							 "How relations used by nested statements are reported.",
							 "all: every statement, including the ones executed inside functions, triggers etc., is reported separately; "
							 "top: only top-level statements are reported; "
							 "fold: relations used by nested statements are reported together with the top-level statement, when it finishes.",
							 &pg_relusage_nested_statements,
							 PG_RELUSAGE_NESTED_ALL,
							 nested_statements_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

#if PG_VERSION_NUM >= 150000
	MarkGUCPrefixReserved("pg_relusage");
#else
	EmitWarningsOnPlaceholders("pg_relusage");
#endif

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pg_relusage_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = pg_relusage_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = pg_relusage_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pg_relusage_ExecutorEnd;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = pg_relusage_ProcessUtility;
	RegisterXactCallback(pg_relusage_xact_callback, NULL);
	RegisterSubXactCallback(pg_relusage_subxact_callback, NULL);
}

/* State for the walk over the plan tree */
typedef struct
{
	List	   *rtable;			/* range table of the statement */
	List	   *scanned;		/* relations scanned or modified by the plan */
	List	   *indexes;		/* indexes used by the plan */
	bool		unknown_node;	/* plan contains a node type we do not know
								 * about */
} pg_relusage_walk_ctx;

static void
pg_relusage_add_rti(pg_relusage_walk_ctx *ctx, Index rti)
{
	RangeTblEntry *rte;

	if (rti == 0)
		return;
	rte = rt_fetch(rti, ctx->rtable);
	if (rte->rtekind == RTE_RELATION)
		ctx->scanned = list_append_unique_oid(ctx->scanned, rte->relid);
}

static void
pg_relusage_add_rtis(pg_relusage_walk_ctx *ctx, Bitmapset *rtis)
{
	int			rti = -1;

	while ((rti = bms_next_member(rtis, rti)) >= 0)
		pg_relusage_add_rti(ctx, rti);
}

/*
 * Collect relations and indexes that the plan actually scans or modifies.
 * Walks the initialized plan state tree (including subplans and initplans), so
 * partitions pruned at executor startup are not included.
 */
static bool
pg_relusage_plan_walker(PlanState *ps, void *context)
{
	pg_relusage_walk_ctx *ctx = (pg_relusage_walk_ctx *) context;
	Plan	   *plan = ps->plan;
	ListCell   *lst;

	switch (nodeTag(plan))
	{
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
			/* scanrelid is 0 when a join is pushed down to the remote server */
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
			/* Target relations of INSERT/UPDATE/DELETE/MERGE */
			foreach(lst, ((ModifyTable *) plan)->resultRelations)
				pg_relusage_add_rti(ctx, lfirst_int(lst));
			break;

			/* Nodes that do not scan relations by themselves */
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

/*
 * Partitioned tables are never scanned themselves, only their partitions are.
 * Add partitioned ancestors of all scanned partitions.
 */
static void
pg_relusage_add_partition_ancestors(pg_relusage_walk_ctx *ctx)
{
	List	   *ancestors = NIL;
	ListCell   *lst;

	foreach(lst, ctx->scanned)
	{
		Oid			oid = lfirst_oid(lst);

		if (!get_rel_relispartition(oid))
			continue;
#if PG_VERSION_NUM >= 110000
		ancestors = list_concat_unique_oid(ancestors, get_partition_ancestors(oid));
#else
		while (get_rel_relispartition(oid))
		{
			oid = get_partition_parent(oid);
			ancestors = list_append_unique_oid(ancestors, oid);
		}
#endif
	}

	ctx->scanned = list_concat_unique_oid(ctx->scanned, ancestors);
	list_free(ancestors);
}

/* Append name of the relation to the buffer, if it is of the kind we report */
static void
pg_relusage_append_relation(StringInfo buf, Oid oid)
{
	HeapTuple	tp;
	Form_pg_class reltup;

	/*
	 * Fetch relkind and relname from a single catalog lookup, so that they
	 * are consistent with each other. Relation could have been dropped
	 * concurrently.
	 */
	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(oid));
	if (!HeapTupleIsValid(tp))
		return;
	reltup = (Form_pg_class) GETSTRUCT(tp);

	/*
	 * System catalogs (pg_catalog, pg_toast, information_schema) are never
	 * reported: they cannot be dropped, and queries from tools (psql, ORMs,
	 * etc.) would flood the log with them
	 */
	if (strchr(pg_relusage_rel_kinds, reltup->relkind)
		&& reltup->relnamespace != PG_CATALOG_NAMESPACE
		&& !IsToastNamespace(reltup->relnamespace))
	{
		/*
		 * Temporary schemas are named pg_temp_N, where N varies between
		 * sessions, so we report all of them as pg_temp
		 */
		char	   *nspname = isAnyTempNamespace(reltup->relnamespace)
			? "pg_temp" : get_namespace_name(reltup->relnamespace);

		if (nspname && strcmp(nspname, "information_schema") == 0)
		{
			ReleaseSysCache(tp);
			return;
		}

		/*
		 * Relations in "public" are reported without schema, to keep logs
		 * smaller
		 */
		if (nspname && strcmp(nspname, "public") == 0)
			nspname = NULL;

		if (buf->len > 0)
			appendStringInfoChar(buf, ',');
		/* Schema-qualified, with quotes added where necessary */
		appendStringInfoString(buf, quote_qualified_identifier(nspname, NameStr(reltup->relname)));
	}

	ReleaseSysCache(tp);
}

/*
 * Relations used by the statement are the ones its plan depends on
 * (plannedstmt->relationOids), except for the relations from the range table
 * that the final plan does not scan (e.g. eliminated by join removal or by
 * partition pruning). Views and relations referenced via regclass constants
 * (e.g. sequences used by nextval()) are always reported. Indexes used by the
 * plan are reported as well.
 */
static List *
pg_relusage_collect_relations(QueryDesc *queryDesc)
{
	PlannedStmt *stmt = queryDesc->plannedstmt;
	pg_relusage_walk_ctx ctx = {stmt->rtable, NIL, NIL, false};
	List	   *in_rtable = NIL;	/* relations that the plan is expected to
									 * scan */
	bool		has_partitioned = false;
	List	   *result = NIL;
	ListCell   *lst;

	foreach(lst, stmt->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lst);

		/* Views are expanded by the rewriter and are never scanned */
		if (rte->rtekind != RTE_RELATION || rte->relkind == RELKIND_VIEW)
			continue;
		in_rtable = list_append_unique_oid(in_rtable, rte->relid);
		if (rte->relkind == RELKIND_PARTITIONED_TABLE)
			has_partitioned = true;
	}

	if (queryDesc->planstate)
		pg_relusage_plan_walker(queryDesc->planstate, &ctx);
	if (has_partitioned)
		pg_relusage_add_partition_ancestors(&ctx);

	foreach(lst, stmt->relationOids)
	{
		Oid			oid = lfirst_oid(lst);

		/*
		 * Skip relations eliminated from the plan. If the plan contains node
		 * types we do not know about, be conservative and report everything.
		 */
		if (list_member_oid(in_rtable, oid) && !list_member_oid(ctx.scanned, oid)
			&& !ctx.unknown_node)
			continue;

		/* Oid list is not deduplicated */
		result = list_append_unique_oid(result, oid);
	}

	result = list_concat_unique_oid(result, ctx.indexes);

	list_free(in_rtable);
	list_free(ctx.scanned);
	list_free(ctx.indexes);
	return result;
}

static void
pg_relusage_emit(const char *names)
{
	if (names[0] != '\0')
	{
		ereport(pg_relusage_log_level, (errmsg("relations used: %s", names), errhidestmt(true)));
	}
}

/* Would a message of this level be sent to the server log or to the client? */
static bool
pg_relusage_level_is_interesting(int elevel)
{
#if PG_VERSION_NUM >= 140000
	return message_level_is_interesting(elevel);
#else
	/*
	 * Simplified copy of message_level_is_interesting() from PostgreSQL 14+,
	 * for the levels pg_relusage.log_level allows (DEBUG5 .. LOG)
	 */
	bool		to_server;
	bool		to_client;

	if (elevel == LOG)
		to_server = (log_min_messages == LOG || log_min_messages <= ERROR);
	else if (log_min_messages == LOG)
		to_server = false;
	else
		to_server = (elevel >= log_min_messages);

	to_client = (whereToSendOutput == DestRemote
				 && (elevel >= client_min_messages || elevel == INFO));

	return to_server || to_client;
#endif
}

static pg_relusage_pending *
pg_relusage_find_pending(QueryDesc *queryDesc)
{
	ListCell   *lst;

	foreach(lst, pending)
	{
		pg_relusage_pending *entry = (pg_relusage_pending *) lfirst(lst);

		if (entry->queryDesc == queryDesc)
			return entry;
	}
	return NULL;
}

static void pg_relusage_flush_pending(pg_relusage_pending *entry);

static pg_relusage_pending *
pg_relusage_new_pending(QueryDesc *queryDesc)
{
	MemoryContext oldcxt;
	pg_relusage_pending *entry = pg_relusage_find_pending(queryDesc);

	/*
	 * A statement is started only once, so an entry for the same QueryDesc
	 * must be left over from a statement that never finished, and whose
	 * QueryDesc was freed and its memory reused. Should not happen, as such
	 * entries are flushed when their subtransaction aborts, but log it
	 * separately just in case.
	 */
	if (entry)
		pg_relusage_flush_pending(entry);

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	entry = palloc0(sizeof(pg_relusage_pending));
	entry->queryDesc = queryDesc;
	entry->subxid = GetCurrentSubTransactionId();
	initStringInfo(&entry->names);
	pending = lappend(pending, entry);
	MemoryContextSwitchTo(oldcxt);
	return entry;
}

/* Add relations that were not seen before to the pending entry */
static void
pg_relusage_add_to_pending(pg_relusage_pending *entry, List *oids)
{
	StringInfoData buf;
	ListCell   *lst;
	MemoryContext oldcxt;

	initStringInfo(&buf);
	foreach(lst, oids)
	{
		Oid			oid = lfirst_oid(lst);

		if (list_member_oid(entry->oids, oid))
			continue;
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		entry->oids = lappend_oid(entry->oids, oid);
		MemoryContextSwitchTo(oldcxt);
		pg_relusage_append_relation(&buf, oid);
	}

	if (buf.len > 0)
	{
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		if (entry->names.len > 0)
			appendStringInfoChar(&entry->names, ',');
		appendBinaryStringInfo(&entry->names, buf.data, buf.len);
		MemoryContextSwitchTo(oldcxt);
	}
	pfree(buf.data);
}

/* Log relations collected for a top-level statement and forget about it */
static void
pg_relusage_flush_pending(pg_relusage_pending *entry)
{
	pending = list_delete_ptr(pending, entry);
	pg_relusage_emit(entry->names.data);
	pfree(entry->names.data);
	list_free(entry->oids);
	pfree(entry);
}

/*
 * Should relations used by the statement be reported? Checks everything that
 * is cheap to check, to skip all the work if there is nothing to report, or
 * the message would be discarded. Parallel workers execute parts of the plan
 * of the leader, which reports relations for the whole plan.
 */
static bool
pg_relusage_should_report(void)
{
	if (!pg_relusage_enabled
		|| IsParallelWorker()
		|| pg_relusage_rel_kinds[0] == '\0'
		|| !pg_relusage_level_is_interesting(pg_relusage_log_level))
		return false;

	if (pg_relusage_nested_statements == PG_RELUSAGE_NESTED_TOP && nesting_level > 0)
		return false;

	return true;
}

/*
 * Report relations used by the statement. In "fold" mode, relations used by
 * nested statements are added to the pending entry of the top-level statement,
 * and top-level statements executed by the executor (queryDesc is not NULL)
 * get a pending entry of their own, which is logged when they finish.
 */
static void
pg_relusage_report(List *oids, QueryDesc *queryDesc)
{
	StringInfoData buf;
	ListCell   *lst;

	if (pg_relusage_nested_statements == PG_RELUSAGE_NESTED_FOLD)
	{
		pg_relusage_pending *entry = NULL;

		if (nesting_level > 0)
			entry = pg_relusage_find_pending(current_top);
		else if (queryDesc)
			entry = pg_relusage_new_pending(queryDesc);

		/*
		 * Nested statement could have no pending top-level entry (e.g. if the
		 * top-level statement started before pg_relusage was enabled). Then
		 * it is logged on its own.
		 */
		if (entry)
		{
			pg_relusage_add_to_pending(entry, oids);
			return;
		}
	}

	initStringInfo(&buf);
	foreach(lst, oids)
		pg_relusage_append_relation(&buf, lfirst_oid(lst));
	pg_relusage_emit(buf.data);
	pfree(buf.data);
}

static void
pg_relusage_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	List	   *oids;

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/*
	 * Log only after the executor started successfully (e.g. permission
	 * checks passed). EXPLAIN without ANALYZE does not execute the query, so
	 * nothing is used.
	 */
	if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) || !pg_relusage_should_report())
		return;

	oids = pg_relusage_collect_relations(queryDesc);
	pg_relusage_report(oids, queryDesc);
	list_free(oids);
}

/*
 * Relations used by utility statements that access data without going through
 * the executor: COPY to or from a table, TRUNCATE, LOCK and REFRESH
 * MATERIALIZED VIEW. Maintenance (VACUUM, CLUSTER etc.) and DDL statements are
 * not reported, as they are typically run for all relations by scripts and
 * migrations, which would make every relation look used. Utility statements
 * that execute queries (e.g. COPY (query) TO, CREATE TABLE AS, EXPLAIN
 * ANALYZE) are reported by the executor hooks.
 */
static List *
pg_relusage_utility_relations(Node *parsetree)
{
	List	   *rangevars = NIL;
	List	   *oids = NIL;
	ListCell   *lst;

	switch (nodeTag(parsetree))
	{
		case T_CopyStmt:
			if (((CopyStmt *) parsetree)->relation)
				rangevars = list_make1(((CopyStmt *) parsetree)->relation);
			break;
		case T_TruncateStmt:
			rangevars = list_copy(((TruncateStmt *) parsetree)->relations);
			break;
		case T_LockStmt:
			rangevars = list_copy(((LockStmt *) parsetree)->relations);
			break;
		case T_RefreshMatViewStmt:
			rangevars = list_make1(((RefreshMatViewStmt *) parsetree)->relation);
			break;
		default:
			break;
	}

	foreach(lst, rangevars)
	{
		Oid			oid = RangeVarGetRelid((RangeVar *) lfirst(lst), NoLock, true);

		if (OidIsValid(oid))
			oids = list_append_unique_oid(oids, oid);
	}
	list_free(rangevars);
	return oids;
}

static void
pg_relusage_ProcessUtility(PG_RELUSAGE_UTILITY_PARAMS)
{
	List	   *oids = NIL;

	/*
	 * Resolve relation names before executing the statement, as execution
	 * could modify the parse tree
	 */
	if (pg_relusage_should_report())
		oids = pg_relusage_utility_relations(pstmt->utilityStmt);

	if (prev_ProcessUtility)
		prev_ProcessUtility(PG_RELUSAGE_UTILITY_ARGS);
	else
		standard_ProcessUtility(PG_RELUSAGE_UTILITY_ARGS);

	/* Log only after the statement succeeded */
	if (oids != NIL)
	{
		pg_relusage_report(oids, NULL);
		list_free(oids);
	}
}

/*
 * ExecutorRun and ExecutorFinish track the nesting level of statements, and
 * the top-level statement that is currently running. Nested statements are
 * executed from inside these (triggers and foreign key checks run in ExecutorFinish).
 */
static void
#if PG_VERSION_NUM >= 180000
pg_relusage_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
#else
pg_relusage_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
						bool execute_once)
#endif
{
	QueryDesc  *saved_top = current_top;

	if (nesting_level == 0)
		current_top = queryDesc;
	nesting_level++;
	PG_TRY();
	{
#if PG_VERSION_NUM >= 180000
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
#else
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
#endif
	}
	PG_CATCH();
	{
		nesting_level--;
		current_top = saved_top;
		PG_RE_THROW();
	}
	PG_END_TRY();
	nesting_level--;
	current_top = saved_top;
}

static void
pg_relusage_ExecutorFinish(QueryDesc *queryDesc)
{
	QueryDesc  *saved_top = current_top;

	if (nesting_level == 0)
		current_top = queryDesc;
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_CATCH();
	{
		nesting_level--;
		current_top = saved_top;
		PG_RE_THROW();
	}
	PG_END_TRY();
	nesting_level--;
	current_top = saved_top;
}

static void
pg_relusage_ExecutorEnd(QueryDesc *queryDesc)
{
	/* In "fold" mode, top-level statement is logged when it is finished */
	pg_relusage_pending *entry = pg_relusage_find_pending(queryDesc);

	if (entry)
		pg_relusage_flush_pending(entry);

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * Top-level statements that failed never reach ExecutorEnd. Log relations they
 * (and their nested statements) used before failing at the end of transaction.
 */
static void
pg_relusage_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_ABORT:
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PREPARE:
			while (pending != NIL)
				pg_relusage_flush_pending((pg_relusage_pending *) linitial(pending));
			break;
		default:
			break;
	}
}

/*
 * Statements that failed inside a subtransaction (e.g. in a PL/pgSQL block
 * with an exception handler) never reach ExecutorEnd, and their QueryDescs are
 * freed. Log relations they used when the subtransaction aborts, and pass
 * entries of the committed subtransaction to its parent.
 */
static void
pg_relusage_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
							 SubTransactionId parentSubid, void *arg)
{
	ListCell   *lst;

	switch (event)
	{
		case SUBXACT_EVENT_ABORT_SUB:
			for (;;)
			{
				pg_relusage_pending *found = NULL;

				foreach(lst, pending)
				{
					pg_relusage_pending *entry = (pg_relusage_pending *) lfirst(lst);

					if (entry->subxid == mySubid)
					{
						found = entry;
						break;
					}
				}
				if (!found)
					break;
				pg_relusage_flush_pending(found);
			}
			break;
		case SUBXACT_EVENT_COMMIT_SUB:
			foreach(lst, pending)
			{
				pg_relusage_pending *entry = (pg_relusage_pending *) lfirst(lst);

				if (entry->subxid == mySubid)
					entry->subxid = parentSubid;
			}
			break;
		default:
			break;
	}
}
