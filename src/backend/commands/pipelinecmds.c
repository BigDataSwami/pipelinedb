/* Copyright (c) 2013-2015 PipelineDB */
/*-------------------------------------------------------------------------
 *
 * pipelinecmds.c
 *	  commands specific to PipelineDB
 *
 * IDENTIFICATION
 *	  src/backend/commands/pipelinecmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "pgstat.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/xact.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/pipelinecmds.h"
#include "commands/tablecmds.h"
#include "commands/view.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/pipeline_query.h"
#include "catalog/pipeline_query_fn.h"
#include "catalog/pipeline_stream_fn.h"
#include "catalog/pipeline_tstate_fn.h"
#include "catalog/toasting.h"
#include "executor/execdesc.h"
#include "executor/tstoreReceiver.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "optimizer/tlist.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "pipeline/cqanalyze.h"
#include "pipeline/cqmatrel.h"
#include "pipeline/cont_analyze.h"
#include "pipeline/cont_plan.h"
#include "pipeline/cqwindow.h"
#include "pipeline/miscutils.h"
#include "pipeline/stream.h"
#include "pipeline/tuplebuf.h"
#include "regex/regex.h"
#include "tcop/dest.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/portal.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#define CQ_MATREL_INDEX_TYPE "btree"
#define DEFAULT_TYPEMOD -1

#define OPTION_FILLFACTOR "fillfactor"

int continuous_view_fillfactor;

static ColumnDef *
make_cv_columndef(char *name, Oid type, Oid typemod)
{
	ColumnDef *result;
	TypeName *typename;

	typename = makeNode(TypeName);
	typename->typeOid = type;
	typename->typemod = typemod;

	result = makeNode(ColumnDef);
	result->colname = name;
	result->inhcount = 0;
	result->is_local = true;
	result->is_not_null = false;
	result->raw_default = NULL;
	result->cooked_default = NULL;
	result->constraints = NIL;
	result->typeName = typename;

	return result;
}

/*
 * has_fillfactor
 *
 * Returns true if a fillfactor option is included in the given WITH options
 */
static bool
has_fillfactor(List *options)
{
	ListCell *lc;

	foreach(lc, options)
	{
		DefElem *de;

		if (!IsA(lfirst(lc), DefElem))
			continue;

		de = (DefElem *) lfirst(lc);
		if (de->defname && pg_strcasecmp(de->defname, OPTION_FILLFACTOR) == 0)
			return true;
	}

	return false;
}

/*
 * make_default_fillfactor
 *
 * Return a fillfactor DefElem with the default fillfactor
 */
static List *
add_default_fillfactor(List *options)
{
	DefElem *ff = makeDefElem(OPTION_FILLFACTOR, (Node *) makeInteger(continuous_view_fillfactor));

	options = lappend(options, (Node *) ff);

	return options;
}

/*
 * make_hashed_index_expr
 *
 * Create an index expression that hashes the grouping columns into a single
 * 32-bit value
 */
static Node *
make_hashed_index_expr(Query *query, TupleDesc desc)
{
	ListCell *lc;
	List *args = NIL;
	FuncExpr *hash;
	Oid hashoid = HASH_GROUP_OID;

	foreach(lc, query->groupClause)
	{
		SortGroupClause *g = (SortGroupClause *) lfirst(lc);
		TargetEntry *te = (TargetEntry *) get_sortgroupref_tle(g->tleSortGroupRef, query->targetList);
		Form_pg_attribute attr;
		Var *var;
		bool found = false;
		int i;

		/*
		 * Instead of using the expression itself as an argument, we use a variable that
		 * points to the column that stores the result of the expression.
		 */
		for (i=0; i<desc->natts; i++)
		{
			attr = (Form_pg_attribute) desc->attrs[i];
			if (pg_strcasecmp(te->resname, NameStr(attr->attname)) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
			elog(ERROR, "could not find index attribute in tuple descriptor");

		if (TypeCategory(attr->atttypid) == TYPCATEGORY_DATETIME)
			hashoid = LS_HASH_GROUP_OID;

		var = makeVar(1, attr->attnum, attr->atttypid, attr->atttypmod,
				attr->attcollation, 0);

		args = lappend(args, var);
	}

	/*
	 * We can only index on expressions having immutable results, so if any of the
	 * grouping expressions are mutable, we can't use a hashed index.
	 */
	hash = makeFuncExpr(hashoid, get_func_rettype(hashoid), args, 0, 0, COERCE_EXPLICIT_CALL);

	return (Node *) hash;
}

/*
 * create_indices_on_mat_relation
 *
 * If feasible, create an index on the new materialization table to make
 * combine retrievals on it as efficient as possible. Sometimes this may be
 * impossible to do automatically in a smart way, but for some queries,
 * such as single-column GROUP BYs, it's straightforward.
 */
static Oid
create_index_on_mat_relation(Oid matreloid, RangeVar *matrelname, Query *query,
		SelectStmt *workerstmt, SelectStmt *viewstmt)
{
	IndexStmt *index;
	IndexElem *indexcol;
	Node *expr = NULL;
	bool sliding = IsSlidingWindowSelectStmt(workerstmt);
	char *indexcolname = NULL;
	Oid index_oid;

	if (query->groupClause == NIL && !sliding)
		return InvalidOid;

	if (query->groupClause == NIL && sliding)
	{
		/*
		 * We still want an index on the timestamp column for sliding window
		 * queries without any grouping, because there is an implicit WHERE clause
		 * used in queries against sliding window CVs.
		 */
		ColumnRef *col;
		Node *node = NULL;
		char *namespace;

		node = (Node *) GetColumnRefInSlidingWindowExpr(viewstmt);

		if (!IsA(node, ColumnRef))
			elog(ERROR, "unexpected sliding window expression type found: %d", nodeTag(node));

		col = (ColumnRef *) node;
		DeconstructQualifiedName(col->fields, &namespace, &indexcolname);
	}
	else
	{
		Relation matrel = heap_open(matreloid, NoLock);

		expr = make_hashed_index_expr(query, RelationGetDescr(matrel));
		heap_close(matrel, NoLock);
	}

	indexcol = makeNode(IndexElem);
	indexcol->name = indexcolname;
	indexcol->expr = expr;
	indexcol->indexcolname = NULL;
	indexcol->collation = NULL;
	indexcol->opclass = NULL;
	indexcol->ordering = SORTBY_DEFAULT;
	indexcol->nulls_ordering = SORTBY_NULLS_DEFAULT;

	index = makeNode(IndexStmt);
	index->idxname = NULL;
	index->relation = matrelname;
	index->accessMethod = CQ_MATREL_INDEX_TYPE;
	index->tableSpace = NULL;
	index->indexParams = list_make1(indexcol);
	index->unique = false;
	index->primary = false;
	index->isconstraint = false;
	index->deferrable = false;
	index->initdeferred = false;
	index->concurrent = false;

	index_oid = DefineIndex(matreloid, index, InvalidOid, false, false, false, false);
	CommandCounterIncrement();

	return index_oid;
}

static char *
get_select_query_sql(RangeVar *view, const char *sql)
{
	int trimmedlen;
	char *trimmed;
	int pos;
	StringInfo str = makeStringInfo();

	if (view->catalogname)
	{
		appendStringInfoString(str, view->catalogname);
		appendStringInfoChar(str, '.');
	}

	if (view->schemaname)
	{
		appendStringInfoString(str, view->schemaname);
		appendStringInfoChar(str, '.');
	}

	appendStringInfoString(str, view->relname);

	/*
	 * Technically the CV could be named "create" or "continuous",
	 * so it's not enough to simply advance to the CV name. We need
	 * to skip past the keywords first. Note that these find() calls
	 * should never return -1 for this string since it's already been
	 * validated.
	 */
	pos = skip_token(sql, "CREATE", 0);
	pos = skip_token(sql, "CONTINUOUS", pos);
	pos = skip_token(sql, "VIEW", pos);
	pos = skip_token(sql, str->data, pos);
	pos = skip_token(sql, "AS", pos);

	trimmedlen = strlen(sql) - pos + 1;
	trimmed = palloc(trimmedlen);

	memcpy(trimmed, &sql[pos], trimmedlen);

	pfree(str->data);
	pfree(str);

	return trimmed;
}

static void
record_dependencies(Oid cvoid, Oid matreloid, Oid viewoid, Oid indexoid, List *from)
{
	ObjectAddress referenced;
	ObjectAddress dependent;
	ListCell *lc;
	ContAnalyzeContext cxt;

	MemSet(&cxt, 0, sizeof(ContAnalyzeContext));

	/*
	 * Record a dependency between the matrel and the view, so when we drop the view
	 * the matrel is automatically dropped as well. The user will enter the view name
	 * when dropping, so the alternative is to rewrite the drop target to the matrel.
	 * This seems simpler.
	 */
	dependent.classId = RelationRelationId;
	dependent.objectId = matreloid;
	dependent.objectSubId = 0;

	referenced.classId = RelationRelationId;
	referenced.objectId = viewoid;
	referenced.objectSubId = 0;

	recordDependencyOn(&dependent, &referenced, DEPENDENCY_INTERNAL);

	/*
	 * Record a dependency between the view its pipeline_query entry so that when
	 * the view is dropped the pipeline_query metadata cleanup hook is invoked.
	 */
	dependent.classId = PipelineQueryRelationId;
	dependent.objectId = cvoid;
	dependent.objectSubId = 0;

	referenced.classId = RelationRelationId;
	referenced.objectId = viewoid;
	referenced.objectSubId = 0;

	recordDependencyOn(&dependent, &referenced, DEPENDENCY_INTERNAL);

	/*
	 * Record a dependency between the matrel and the group lookup index so that the
	 * index can never be dropped.
	 */
	if (OidIsValid(indexoid))
	{
		dependent.classId = RelationRelationId;
		dependent.objectId = indexoid;
		dependent.objectSubId = 0;

		referenced.classId = RelationRelationId;
		referenced.objectId = matreloid;
		referenced.objectSubId = 0;

		recordDependencyOn(&dependent, &referenced, DEPENDENCY_INTERNAL);
	}

	collect_rels_and_streams((Node *) from, &cxt);

	/*
	 * Record a dependency between any strongly typed streams and a pipeline_query object,
	 * so that it is not possible to drop a stream that is being read by a CV.
	 */
	foreach(lc, cxt.streams)
	{
		RangeVar *rv;
		Oid relid;

		if (!IsA(lfirst(lc), RangeVar))
			continue;

		rv = (RangeVar *) lfirst(lc);
		relid = RangeVarGetRelid(rv, AccessShareLock, false);

		if (IsInferredStream(relid))
		{
			Relation rel = relation_open(relid, NoLock);
			Oid typid = rel->rd_att->tdtypeid;

			relation_close(rel, NoLock);

			referenced.classId = RelationRelationId;
			referenced.objectId = viewoid;
			referenced.objectSubId = 0;

			dependent.classId = TypeRelationId;
			dependent.objectId = typid;
			dependent.objectSubId = 0;

			recordDependencyOn(&dependent, &referenced, DEPENDENCY_STREAM);
		}
		else
		{
			referenced.classId = RelationRelationId;
			referenced.objectId = relid;
			referenced.objectSubId = 0;

			dependent.classId = RelationRelationId;
			dependent.objectId = viewoid;
			dependent.objectSubId = 0;

			recordDependencyOn(&dependent, &referenced, DEPENDENCY_NORMAL);
		}
	}
}

/*
 * ExecCreateContViewStmt
 *
 * Creates a table for backing the result of the continuous query,
 * and stores the query in a catalog table.
 */
void
ExecCreateContViewStmt(CreateContViewStmt *stmt, const char *querystring)
{
	CreateStmt *create_stmt;
	ViewStmt *view_stmt;
	Query *query;
	RangeVar *mat_relation;
	RangeVar *view;
	List *tableElts = NIL;
	List *tlist;
	ListCell *col;
	Oid matreloid;
	Oid viewoid;
	Oid cvoid;
	Oid indexoid;
	Datum toast_options;
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;
	SelectStmt *workerselect;
	SelectStmt *viewselect;
	CQAnalyzeContext context;
	bool saveAllowSystemTableMods;
	Relation pipeline_query;

	view = stmt->into->rel;
	mat_relation = makeRangeVar(view->schemaname, GetUniqueMatRelName(view->relname, view->schemaname), -1);

	/*
	 * Check if CV already exists?
	 */
	if (IsAContinuousView(view))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_CONTINUOUS_VIEW),
				errmsg("continuous view \"%s\" already exists", view->relname)));

	/*
	 * allowSystemTableMods is a global flag that, when true, allows certain column types
	 * to be created. We need it set to true to create some hidden state columns. In particular,
	 * ones with a type of anyarray.
	 */
	saveAllowSystemTableMods = allowSystemTableMods;
	allowSystemTableMods = true;

	pipeline_query = heap_open(PipelineQueryRelationId, ExclusiveLock);

	CreateInferredStreams((SelectStmt *) stmt->query);
	ValidateContQuery(stmt->into->rel, stmt->query, querystring);

	/*
	 * Get the transformed SelectStmt used by CQ workers. We do this
	 * because the targetList of this SelectStmt contains all columns
	 * that need to be created in the underlying materialization table.
	 */
	workerselect = GetSelectStmtForCQWorker(copyObject(stmt->query), &viewselect);
	InitializeCQAnalyzeContext(workerselect, NULL, &context);

	query = parse_analyze(copyObject(workerselect), querystring, 0, 0);
	tlist = query->targetList;

	/*
	 * Run it through the planner, so that if something goes wrong we know now
	 * rather than finding out when the CV actually activates.
	 */
	pg_plan_queries(list_make1(query), 0, NULL);

	/*
	 * Build a list of columns from the SELECT statement that we
	 * can use to create a table with
	 */
	foreach(col, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(col);
		ColumnDef   *coldef;
		char		*colname;
		Oid			hiddentype;
		Oid type;

		/* Ignore junk columns from the targetlist */
		if (tle->resjunk)
			continue;

		colname = pstrdup(tle->resname);

		/*
		 * Set typeOid and typemod. The name of the type is derived while
		 * generating query
		 */
		type = exprType((Node *) tle->expr);
		/* Replace void type with a bool type. We need this because of the use of pg_sleep in some CQ tests */
		if (type == VOIDOID)
			type = BOOLOID;
		coldef = make_cv_columndef(colname, type, exprTypmod((Node *) tle->expr));
		tableElts = lappend(tableElts, coldef);

		/*
		 * If this column requires state to support incremental transitions, create it. Note: since this
		 * column isn't in the target list, it won't be visible when selecting from this CV,
		 * which will have an overlay view that only exposes target list columns.
		 */
		hiddentype = GetCombineStateColumnType(tle->expr);
		if (OidIsValid(hiddentype))
		{
			char *hiddenname = GetUniqueInternalColname(&context);
			ColumnDef *hidden = make_cv_columndef(hiddenname, hiddentype, DEFAULT_TYPEMOD);
			tableElts = lappend(tableElts, hidden);
		}
	}

	if (!has_fillfactor(stmt->into->options))
		stmt->into->options = add_default_fillfactor(stmt->into->options);

	/*
	 * Create the actual underlying materialzation relation.
	 */
	create_stmt = makeNode(CreateStmt);
	create_stmt->relation = mat_relation;
	create_stmt->tableElts = tableElts;
	create_stmt->tablespacename = stmt->into->tableSpaceName;
	create_stmt->oncommit = stmt->into->onCommit;
	create_stmt->options = stmt->into->options;

	matreloid = DefineRelation(create_stmt, RELKIND_RELATION, InvalidOid);
	CommandCounterIncrement();

	toast_options = transformRelOptions((Datum) 0, create_stmt->options, "toast",
			validnsps, true, false);

	(void) heap_reloptions(RELKIND_TOASTVALUE, toast_options,
						   true);

	AlterTableCreateToastTable(matreloid, toast_options, AccessExclusiveLock);

	/*
	 * Now save the underlying query in the `pipeline_query` catalog
	 * relation.
	 */
	cvoid = DefineContinuousView(view, get_select_query_sql(view, querystring),
			mat_relation, IsSlidingWindowSelectStmt(viewselect), !SelectsFromStreamOnly(workerselect));
	CommandCounterIncrement();

	/* Create the view on the matrel */
	viewselect->fromClause = list_make1(mat_relation);
	view_stmt = makeNode(ViewStmt);
	view_stmt->view = view;
	view_stmt->query = (Node *) viewselect;

	viewoid = DefineView(view_stmt, querystring);
	CommandCounterIncrement();

	/* Create group look up index and record dependencies */
	indexoid = create_index_on_mat_relation(matreloid, mat_relation, query, workerselect, viewselect);
	record_dependencies(cvoid, matreloid, viewoid, indexoid, workerselect->fromClause);

	allowSystemTableMods = saveAllowSystemTableMods;

	heap_close(pipeline_query, NoLock);
}

/*
 * ExecTruncateContViewStmt
 */
void
ExecTruncateContViewStmt(TruncateStmt *stmt)
{
	ListCell *lc;
	Relation pipeline_query;
	List *views = NIL;

	pipeline_query = heap_open(PipelineQueryRelationId, RowExclusiveLock);

	/* Ensure that all *relations* are CQs. */
	foreach(lc, stmt->relations)
	{
		RangeVar *rv = (RangeVar *) lfirst(lc);
		RangeVar *matrel;
		HeapTuple tuple = GetPipelineQueryTuple(rv);
		Form_pipeline_query row;

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_CONTINUOUS_VIEW),
					errmsg("continuous view \"%s\" does not exist", rv->relname)));

		row = (Form_pipeline_query) GETSTRUCT(tuple);

		ReleaseSysCache(tuple);

		views = lappend_oid(views, row->id);
		matrel = GetMatRelationName(rv);

		rv->relname = matrel->relname;
	}

	/* Reset all CQ level transition state */
	foreach(lc, views)
		ResetTStateEntry(lfirst_oid(lc));

	/* Call TRUNCATE on the backing view table(s). */
	stmt->objType = OBJECT_TABLE;
	ExecuteTruncate(stmt);

	heap_close(pipeline_query, NoLock);
}

/*
 * ExecActivateStmt
 */
void
ExecActivateStmt(ActivateStmt *stmt)
{
	if (!ContQuerySetStateAndWait(true, 1000))
		ereport(ERROR,
				(errmsg("failed to activate continuous views")));
}

/*
 * ExecDeactivateStmt
 */
void
ExecDeactivateStmt(DeactivateStmt *stmt)
{
	if (!ContQuerySetStateAndWait(false, 1000))
		ereport(ERROR,
				(errmsg("failed to deactivate continuous views")));
}

static void
explain_cont_plan(char *name, PlannedStmt *plan, ExplainState *base_es, TupleDesc desc, DestReceiver *dest)
{
	TupOutputState *tstate;
	ExplainState es;

	Assert(plan);

	memcpy(&es, base_es, sizeof(ExplainState));
	es.str = makeStringInfo();
	es.indent = 1;
	appendStringInfoString(es.str, name);
	appendStringInfoString(es.str, ":\n");

	/* emit opening boilerplate */
	ExplainBeginOutput(&es);

	ExplainOnePlan(plan, NULL, &es, NULL, NULL, NULL);

	/* emit closing boilerplate */
	ExplainEndOutput(&es);
	Assert(es.indent == 1);

	/* Non text formats only return single line inputs so do have a new line at the end */
	if (es.format != EXPLAIN_FORMAT_TEXT)
		appendStringInfoChar(es.str, '\n');

	/* output tuples */
	tstate = begin_tup_output_tupdesc(dest, desc);
	do_text_output_multiline(tstate, es.str->data);
	end_tup_output(tstate);

	pfree(es.str->data);
	pfree(es.str);
}

/*
 * ExplainContViewResultDesc
 */
TupleDesc
ExplainContViewResultDesc(ExplainContViewStmt *stmt)
{
	ExplainStmt *explain = makeNode(ExplainStmt);
	TupleDesc desc;

	explain->options = stmt->options;
	desc = ExplainResultDesc(explain);
	pfree(explain);

	Assert(desc->natts == 1);
	namestrcpy(&(desc->attrs[0]->attname), "CONTINUOUS QUERY PLANS");

	return desc;
}

/*
 * ExecExplainContViewStmt
 */
void
ExecExplainContViewStmt(ExplainContViewStmt *stmt, const char *queryString,
			 ParamListInfo params, DestReceiver *dest)
{
	ExplainState es;
	ListCell *lc;
	TupleDesc desc;
	ContQueryProc cq_proc;
	ContinuousView *view;
	HeapTuple tuple = GetPipelineQueryTuple(stmt->view);
	Oid cq_id;
	Form_pipeline_query row;
	PlannedStmt *plan;
	Tuplestorestate *tupstore;

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CONTINUOUS_VIEW),
				errmsg("continuous view \"%s\" does not exist", stmt->view->relname)));

	row = (Form_pipeline_query) GETSTRUCT(tuple);
	cq_id = row->id;
	ReleaseSysCache(tuple);

	/* Initialize ExplainState. */
	ExplainInitState(&es);
	es.format = EXPLAIN_FORMAT_TEXT;
	pfree(es.str);

	/* Parse options list. */
	foreach(lc, stmt->options)
	{
		DefElem *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "verbose") == 0)
			es.verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "costs") == 0)
			es.costs = defGetBoolean(opt);
		else if (strcmp(opt->defname, "format") == 0)
		{
			char *p = defGetString(opt);

			if (strcmp(p, "text") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("unrecognized value for EXPLAIN CONTINUOUS VIEW option \"%s\": \"%s\"",
								opt->defname, p)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized EXPLAIN CONTINUOUS VIEW option \"%s\"",
							opt->defname)));
	}

	desc = ExplainContViewResultDesc(stmt);

	view = GetContinuousView(cq_id);

	MyContQueryProc = &cq_proc;
	MyContQueryProc->type = Worker;
	explain_cont_plan("Worker Plan", GetContPlan(view), &es, desc, dest);

	MyContQueryProc = &cq_proc;
	MyContQueryProc->type = Combiner;

	plan = GetContPlan(view);
	tupstore = tuplestore_begin_heap(false, false, work_mem);
	SetCombinerPlanTuplestorestate(plan, tupstore);
	explain_cont_plan("Combiner Plan", plan, &es, desc, dest);
	tuplestore_end(tupstore);

	explain_cont_plan("Combiner Lookup Plan", GetCombinerLookupPlan(view), &es, desc, dest);
	MyContQueryProc = NULL;
}
