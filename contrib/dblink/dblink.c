/*
 * dblink.c
 *
 * Functions returning results from a remote database
 *
 * Joe Conway <mail@joeconway.com>
 * And contributors:
 * Darko Prenosil <Darko.Prenosil@finteh.hr>
 * Shridhar Daithankar <shridhar_daithankar@persistent.co.in>
 *
 * $PostgreSQL: pgsql/contrib/dblink/dblink.c,v 1.69.2.8 2010/06/15 19:04:28 tgl Exp $
 * Copyright (c) 2001-2008, PostgreSQL Global Development Group
 * ALL RIGHTS RESERVED;
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */
#include "postgres.h"

#include <limits.h>

#include "libpq-fe.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/tupdesc.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "nodes/execnodes.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "parser/parse_type.h"
#include "parser/scansup.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/dynahash.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/memutils.h"

#include "dblink.h"

PG_MODULE_MAGIC;

typedef struct remoteConn
{
	PGconn	   *conn;			/* Hold the remote connection */
	int			openCursorCount;	/* The number of open cursors */
	bool		newXactForCursor;		/* Opened a transaction for a cursor */
}	remoteConn;

/*
 * Internal declarations
 */
static Datum dblink_record_internal(FunctionCallInfo fcinfo, bool is_async, bool do_get);
static remoteConn *getConnectionByName(const char *name);
static HTAB *createConnHash(void);
static void createNewConnection(const char *name, remoteConn * rconn);
static void deleteConnection(const char *name);
static char **get_pkey_attnames(Relation rel, int16 *numatts);
static char **get_text_array_contents(ArrayType *array, int *numitems);
static char *get_sql_insert(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals, char **tgt_pkattvals);
static char *get_sql_delete(Relation rel, int *pkattnums, int pknumatts, char **tgt_pkattvals);
static char *get_sql_update(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals, char **tgt_pkattvals);
static char *quote_literal_cstr(char *rawstr);
static char *quote_ident_cstr(char *rawstr);
static int	get_attnum_pk_pos(int *pkattnums, int pknumatts, int key);
static HeapTuple get_tuple_of_interest(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals);
static Relation get_rel_from_relname(text *relname_text, LOCKMODE lockmode, AclMode aclmode);
static char *generate_relation_name(Relation rel);
static char *dblink_connstr_check(const char *connstr);
static void dblink_security_check(PGconn *conn, remoteConn *rconn);
static void dblink_res_error(const char *conname, PGresult *res, const char *dblink_context_msg, bool fail);
static void dblink_security_check(PGconn *conn, remoteConn *rconn);
static void validate_pkattnums(Relation rel,
				   int2vector *pkattnums_arg, int32 pknumatts_arg,
				   int **pkattnums, int *pknumatts);

/* Global */
static remoteConn *pconn = NULL;
static HTAB *remoteConnHash = NULL;

/*
 *	Following is list that holds multiple remote connections.
 *	Calling convention of each dblink function changes to accept
 *	connection name as the first parameter. The connection list is
 *	much like ecpg e.g. a mapping between a name and a PGconn object.
 */

typedef struct remoteConnHashEnt
{
	char		name[NAMEDATALEN];
	remoteConn *rconn;
}	remoteConnHashEnt;

/* initial number of connection hashes */
#define NUMCONN 16

/* general utility */
#define GET_TEXT(cstrp) DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum((char*)cstrp)))
#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))
#define xpfree(var_) \
	do { \
		if (var_ != NULL) \
		{ \
			pfree(var_); \
			var_ = NULL; \
		} \
	} while (0)

#define xpstrdup(tgtvar_, srcvar_) \
    do { \
        if (srcvar_) \
            tgtvar_ = pstrdup(srcvar_); \
        else \
            tgtvar_ = NULL; \
    } while (0)

#define DBLINK_RES_INTERNALERROR(p2) \
	do { \
			msg = pstrdup(PQerrorMessage(conn)); \
			if (res) \
				PQclear(res); \
			elog(ERROR, "%s: %s", p2, msg); \
	} while (0)

#define DBLINK_RES_ERROR(p2) \
	do { \
			msg = pstrdup(PQerrorMessage(conn)); \
			if (res) \
				PQclear(res); \
			ereport(ERROR, \
					(errcode(ERRCODE_SYNTAX_ERROR), \
					 errmsg("%s", p2), \
					 errdetail("%s", msg))); \
	} while (0)

#define DBLINK_RES_ERROR_AS_NOTICE(p2) \
	do { \
			msg = pstrdup(PQerrorMessage(conn)); \
			if (res) \
				PQclear(res); \
			ereport(NOTICE, \
					(errcode(ERRCODE_SYNTAX_ERROR), \
					 errmsg("%s", p2), \
					 errdetail("%s", msg))); \
	} while (0)

#define DBLINK_CONN_NOT_AVAIL \
	do { \
		if(conname) \
			ereport(ERROR, \
					(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST), \
					 errmsg("connection \"%s\" not available", conname))); \
		else \
			ereport(ERROR, \
					(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST), \
					 errmsg("connection not available"))); \
	} while (0)

#define DBLINK_GET_CONN \
	do { \
			char *conname_or_str = GET_STR(PG_GETARG_TEXT_P(0)); \
			rconn = getConnectionByName(conname_or_str); \
			if(rconn) \
			{ \
				conn = rconn->conn; \
			} \
			else \
			{ \
				connstr = dblink_connstr_check(conname_or_str); \
				conn = PQconnectdb(connstr); \
				if (PQstatus(conn) == CONNECTION_BAD) \
				{ \
					msg = pstrdup(PQerrorMessage(conn)); \
					PQfinish(conn); \
					ereport(ERROR, \
							(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION), \
							 errmsg("could not establish connection"), \
							 errdetail("%s", msg))); \
				} \
				dblink_security_check(conn, rconn); \
				freeconn = true; \
			} \
	} while (0)

#define DBLINK_GET_NAMED_CONN \
	do { \
			char *conname = GET_STR(PG_GETARG_TEXT_P(0)); \
			rconn = getConnectionByName(conname); \
			if(rconn) \
				conn = rconn->conn; \
			else \
				DBLINK_CONN_NOT_AVAIL; \
	} while (0)

#define DBLINK_INIT \
	do { \
			if (!pconn) \
			{ \
				pconn = (remoteConn *) MemoryContextAlloc(TopMemoryContext, sizeof(remoteConn)); \
				pconn->conn = NULL; \
				pconn->openCursorCount = 0; \
				pconn->newXactForCursor = FALSE; \
			} \
	} while (0)

/*
 * Create a persistent connection to another database
 */
PG_FUNCTION_INFO_V1(dblink_connect);
Datum
dblink_connect(PG_FUNCTION_ARGS)
{
	char	   *connstr = NULL;
	char	   *connname = NULL;
	char	   *msg;
	PGconn	   *conn = NULL;
	remoteConn *rconn = NULL;

	DBLINK_INIT;

	if (PG_NARGS() == 2)
	{
		connstr = GET_STR(PG_GETARG_TEXT_P(1));
		connname = GET_STR(PG_GETARG_TEXT_P(0));
	}
	else
		connstr = GET_STR(PG_GETARG_TEXT_P(0));

	if (connname)
		rconn = (remoteConn *) MemoryContextAlloc(TopMemoryContext,
												  sizeof(remoteConn));

	/* check password in connection string if not superuser */
	connstr = dblink_connstr_check(connstr);
	conn = PQconnectdb(connstr);

	if (PQstatus(conn) == CONNECTION_BAD)
	{
		msg = pstrdup(PQerrorMessage(conn));
		PQfinish(conn);
		if (rconn)
			pfree(rconn);

		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("could not establish connection"),
				 errdetail("%s", msg)));
	}

	/* check password actually used if not superuser */
	dblink_security_check(conn, rconn);

	if (connname)
	{
		rconn->conn = conn;
		createNewConnection(connname, rconn);
	}
	else
		pconn->conn = conn;

	PG_RETURN_TEXT_P(GET_TEXT("OK"));
}

/*
 * Clear a persistent connection to another database
 */
PG_FUNCTION_INFO_V1(dblink_disconnect);
Datum
dblink_disconnect(PG_FUNCTION_ARGS)
{
	char	   *conname = NULL;
	remoteConn *rconn = NULL;
	PGconn	   *conn = NULL;

	DBLINK_INIT;

	if (PG_NARGS() == 1)
	{
		conname = GET_STR(PG_GETARG_TEXT_P(0));
		rconn = getConnectionByName(conname);
		if (rconn)
			conn = rconn->conn;
	}
	else
		conn = pconn->conn;

	if (!conn)
		DBLINK_CONN_NOT_AVAIL;

	PQfinish(conn);
	if (rconn)
	{
		deleteConnection(conname);
		pfree(rconn);
	}
	else
		pconn->conn = NULL;

	PG_RETURN_TEXT_P(GET_TEXT("OK"));
}

/*
 * opens a cursor using a persistent connection
 */
PG_FUNCTION_INFO_V1(dblink_open);
Datum
dblink_open(PG_FUNCTION_ARGS)
{
	char	   *msg;
	PGresult   *res = NULL;
	PGconn	   *conn = NULL;
	char	   *curname = NULL;
	char	   *sql = NULL;
	char	   *conname = NULL;
	StringInfoData buf;
	remoteConn *rconn = NULL;
	bool		fail = true;	/* default to backward compatible behavior */

	DBLINK_INIT;
	initStringInfo(&buf);

	if (PG_NARGS() == 2)
	{
		/* text,text */
		curname = GET_STR(PG_GETARG_TEXT_P(0));
		sql = GET_STR(PG_GETARG_TEXT_P(1));
		rconn = pconn;
	}
	else if (PG_NARGS() == 3)
	{
		/* might be text,text,text or text,text,bool */
		if (get_fn_expr_argtype(fcinfo->flinfo, 2) == BOOLOID)
		{
			curname = GET_STR(PG_GETARG_TEXT_P(0));
			sql = GET_STR(PG_GETARG_TEXT_P(1));
			fail = PG_GETARG_BOOL(2);
			rconn = pconn;
		}
		else
		{
			conname = GET_STR(PG_GETARG_TEXT_P(0));
			curname = GET_STR(PG_GETARG_TEXT_P(1));
			sql = GET_STR(PG_GETARG_TEXT_P(2));
			rconn = getConnectionByName(conname);
		}
	}
	else if (PG_NARGS() == 4)
	{
		/* text,text,text,bool */
		conname = GET_STR(PG_GETARG_TEXT_P(0));
		curname = GET_STR(PG_GETARG_TEXT_P(1));
		sql = GET_STR(PG_GETARG_TEXT_P(2));
		fail = PG_GETARG_BOOL(3);
		rconn = getConnectionByName(conname);
	}

	if (!rconn || !rconn->conn)
		DBLINK_CONN_NOT_AVAIL;
	else
		conn = rconn->conn;

	/* If we are not in a transaction, start one */
	if (PQtransactionStatus(conn) == PQTRANS_IDLE)
	{
		res = PQexec(conn, "BEGIN");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			DBLINK_RES_INTERNALERROR("begin error");
		PQclear(res);
		rconn->newXactForCursor = TRUE;

		/*
		 * Since transaction state was IDLE, we force cursor count to
		 * initially be 0. This is needed as a previous ABORT might have wiped
		 * out our transaction without maintaining the cursor count for us.
		 */
		rconn->openCursorCount = 0;
	}

	/* if we started a transaction, increment cursor count */
	if (rconn->newXactForCursor)
		(rconn->openCursorCount)++;

	appendStringInfo(&buf, "DECLARE %s CURSOR FOR %s", curname, sql);
	res = PQexec(conn, buf.data);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		if (fail)
			DBLINK_RES_ERROR("sql error");
		else
		{
			DBLINK_RES_ERROR_AS_NOTICE("sql error");
			PG_RETURN_TEXT_P(GET_TEXT("ERROR"));
		}
	}

	PQclear(res);
	PG_RETURN_TEXT_P(GET_TEXT("OK"));
}

/*
 * closes a cursor
 */
PG_FUNCTION_INFO_V1(dblink_close);
Datum
dblink_close(PG_FUNCTION_ARGS)
{
	PGconn	   *conn = NULL;
	PGresult   *res = NULL;
	char	   *curname = NULL;
	char	   *conname = NULL;
	StringInfoData buf;
	char	   *msg;
	remoteConn *rconn = NULL;
	bool		fail = true;	/* default to backward compatible behavior */

	DBLINK_INIT;
	initStringInfo(&buf);

	if (PG_NARGS() == 1)
	{
		/* text */
		curname = GET_STR(PG_GETARG_TEXT_P(0));
		rconn = pconn;
	}
	else if (PG_NARGS() == 2)
	{
		/* might be text,text or text,bool */
		if (get_fn_expr_argtype(fcinfo->flinfo, 1) == BOOLOID)
		{
			curname = GET_STR(PG_GETARG_TEXT_P(0));
			fail = PG_GETARG_BOOL(1);
			rconn = pconn;
		}
		else
		{
			conname = GET_STR(PG_GETARG_TEXT_P(0));
			curname = GET_STR(PG_GETARG_TEXT_P(1));
			rconn = getConnectionByName(conname);
		}
	}
	if (PG_NARGS() == 3)
	{
		/* text,text,bool */
		conname = GET_STR(PG_GETARG_TEXT_P(0));
		curname = GET_STR(PG_GETARG_TEXT_P(1));
		fail = PG_GETARG_BOOL(2);
		rconn = getConnectionByName(conname);
	}

	if (!rconn || !rconn->conn)
		DBLINK_CONN_NOT_AVAIL;
	else
		conn = rconn->conn;

	appendStringInfo(&buf, "CLOSE %s", curname);

	/* close the cursor */
	res = PQexec(conn, buf.data);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		if (fail)
			DBLINK_RES_ERROR("sql error");
		else
		{
			DBLINK_RES_ERROR_AS_NOTICE("sql error");
			PG_RETURN_TEXT_P(GET_TEXT("ERROR"));
		}
	}

	PQclear(res);

	/* if we started a transaction, decrement cursor count */
	if (rconn->newXactForCursor)
	{
		(rconn->openCursorCount)--;

		/* if count is zero, commit the transaction */
		if (rconn->openCursorCount == 0)
		{
			rconn->newXactForCursor = FALSE;

			res = PQexec(conn, "COMMIT");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
				DBLINK_RES_INTERNALERROR("commit error");
			PQclear(res);
		}
	}

	PG_RETURN_TEXT_P(GET_TEXT("OK"));
}

/*
 * Fetch results from an open cursor
 */
PG_FUNCTION_INFO_V1(dblink_fetch);
Datum
dblink_fetch(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	TupleDesc	tupdesc = NULL;
	int			call_cntr;
	int			max_calls;
	AttInMetadata *attinmeta;
	char	   *msg;
	PGresult   *res = NULL;
	MemoryContext oldcontext;
	char	   *conname = NULL;
	remoteConn *rconn = NULL;

	DBLINK_INIT;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		PGconn	   *conn = NULL;
		StringInfoData buf;
		char	   *curname = NULL;
		int			howmany = 0;
		bool		fail = true;	/* default to backward compatible */

		if (PG_NARGS() == 4)
		{
			/* text,text,int,bool */
			conname = GET_STR(PG_GETARG_TEXT_P(0));
			curname = GET_STR(PG_GETARG_TEXT_P(1));
			howmany = PG_GETARG_INT32(2);
			fail = PG_GETARG_BOOL(3);

			rconn = getConnectionByName(conname);
			if (rconn)
				conn = rconn->conn;
		}
		else if (PG_NARGS() == 3)
		{
			/* text,text,int or text,int,bool */
			if (get_fn_expr_argtype(fcinfo->flinfo, 2) == BOOLOID)
			{
				curname = GET_STR(PG_GETARG_TEXT_P(0));
				howmany = PG_GETARG_INT32(1);
				fail = PG_GETARG_BOOL(2);
				conn = pconn->conn;
			}
			else
			{
				conname = GET_STR(PG_GETARG_TEXT_P(0));
				curname = GET_STR(PG_GETARG_TEXT_P(1));
				howmany = PG_GETARG_INT32(2);

				rconn = getConnectionByName(conname);
				if (rconn)
					conn = rconn->conn;
			}
		}
		else if (PG_NARGS() == 2)
		{
			/* text,int */
			curname = GET_STR(PG_GETARG_TEXT_P(0));
			howmany = PG_GETARG_INT32(1);
			conn = pconn->conn;
		}

		if (!conn)
			DBLINK_CONN_NOT_AVAIL;

		initStringInfo(&buf);
		appendStringInfo(&buf, "FETCH %d FROM %s", howmany, curname);

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * Try to execute the query.  Note that since libpq uses malloc,
		 * the PGresult will be long-lived even though we are still in
		 * a short-lived memory context.
		 */
		res = PQexec(conn, buf.data);
		if (!res ||
			(PQresultStatus(res) != PGRES_COMMAND_OK &&
			 PQresultStatus(res) != PGRES_TUPLES_OK))
		{
			if (fail)
				DBLINK_RES_ERROR("sql error");
			else
			{
				DBLINK_RES_ERROR_AS_NOTICE("sql error");
				SRF_RETURN_DONE(funcctx);
			}
		}
		else if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			/* cursor does not exist - closed already or bad name */
			PQclear(res);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_NAME),
					 errmsg("cursor \"%s\" does not exist", curname)));
		}

		funcctx->max_calls = PQntuples(res);

		/* got results, keep track of them */
		funcctx->user_fctx = res;

		/* get a tuple descriptor for our result type */
		switch (get_call_result_type(fcinfo, NULL, &tupdesc))
		{
			case TYPEFUNC_COMPOSITE:
				/* success */
				break;
			case TYPEFUNC_RECORD:
				/* failed to determine actual type of RECORD */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function returning record called in context "
								"that cannot accept type record")));
				break;
			default:
				/* result type isn't composite */
				elog(ERROR, "return type must be a row type");
				break;
		}

		/* check result and tuple descriptor have the same number of columns */
		if (PQnfields(res) != tupdesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("remote query result rowtype does not match "
							"the specified FROM clause rowtype")));

		/*
		 * fast track when no results.  We could exit earlier, but then
		 * we'd not report error if the result tuple type is wrong.
		 */
		if (funcctx->max_calls < 1)
		{
			PQclear(res);
			SRF_RETURN_DONE(funcctx);
		}

		/*
		 * switch to memory context appropriate for multiple function calls,
		 * so we can make long-lived copy of tupdesc etc
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* make sure we have a persistent copy of the tupdesc */
		tupdesc = CreateTupleDescCopy(tupdesc);

		/* store needed metadata for subsequent calls */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * initialize per-call variables
	 */
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;

	res = (PGresult *) funcctx->user_fctx;
	attinmeta = funcctx->attinmeta;
	tupdesc = attinmeta->tupdesc;

	if (call_cntr < max_calls)	/* do when there is more left to send */
	{
		char	  **values;
		HeapTuple	tuple;
		Datum		result;
		int			i;
		int			nfields = PQnfields(res);

		values = (char **) palloc(nfields * sizeof(char *));
		for (i = 0; i < nfields; i++)
		{
			if (PQgetisnull(res, call_cntr, i) == 0)
				values[i] = PQgetvalue(res, call_cntr, i);
			else
				values[i] = NULL;
		}

		/* build the tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* make the tuple into a datum */
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
	{
		/* do when there is no more left */
		PQclear(res);
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * Note: this is the new preferred version of dblink
 */
PG_FUNCTION_INFO_V1(dblink_record);
Datum
dblink_record(PG_FUNCTION_ARGS)
{
	return dblink_record_internal(fcinfo, false, false);
}

PG_FUNCTION_INFO_V1(dblink_send_query);
Datum
dblink_send_query(PG_FUNCTION_ARGS)
{
	return dblink_record_internal(fcinfo, true, false);
}

PG_FUNCTION_INFO_V1(dblink_get_result);
Datum
dblink_get_result(PG_FUNCTION_ARGS)
{
	return dblink_record_internal(fcinfo, true, true);
}

static Datum
dblink_record_internal(FunctionCallInfo fcinfo, bool is_async, bool do_get)
{
	FuncCallContext *funcctx;
	TupleDesc	tupdesc = NULL;
	int			call_cntr;
	int			max_calls;
	AttInMetadata *attinmeta;
	char	   *msg;
	PGresult   *res = NULL;
	bool		is_sql_cmd = false;
	char	   *sql_cmd_status = NULL;
	MemoryContext oldcontext;
	bool		freeconn = false;

	DBLINK_INIT;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		PGconn	   *conn = NULL;
		char	   *connstr = NULL;
		char	   *sql = NULL;
		char	   *conname = NULL;
		remoteConn *rconn = NULL;
		bool		fail = true;	/* default to backward compatible */

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (!is_async)
		{
			if (PG_NARGS() == 3)
			{
				/* text,text,bool */
				DBLINK_GET_CONN;
				sql = GET_STR(PG_GETARG_TEXT_P(1));
				fail = PG_GETARG_BOOL(2);
			}
			else if (PG_NARGS() == 2)
			{
				/* text,text or text,bool */
				if (get_fn_expr_argtype(fcinfo->flinfo, 1) == BOOLOID)
				{
					conn = pconn->conn;
					sql = GET_STR(PG_GETARG_TEXT_P(0));
					fail = PG_GETARG_BOOL(1);
				}
				else
				{
					DBLINK_GET_CONN;
					sql = GET_STR(PG_GETARG_TEXT_P(1));
				}
			}
			else if (PG_NARGS() == 1)
			{
				/* text */
				conn = pconn->conn;
				sql = GET_STR(PG_GETARG_TEXT_P(0));
			}
			else
				/* shouldn't happen */
				elog(ERROR, "wrong number of arguments");
		}
		else if (is_async && do_get)
		{
			/* get async result */
			if (PG_NARGS() == 2)
			{
				/* text,bool */
				DBLINK_GET_CONN;
				fail = PG_GETARG_BOOL(1);
			}
			else if (PG_NARGS() == 1)
			{
				/* text */
				DBLINK_GET_CONN;
			}
			else
				/* shouldn't happen */
				elog(ERROR, "wrong number of arguments");
		}
		else
		{
			/* send async query */
			if (PG_NARGS() == 2)
			{
				DBLINK_GET_CONN;
				sql = GET_STR(PG_GETARG_TEXT_P(1));
			}
			else
				/* shouldn't happen */
				elog(ERROR, "wrong number of arguments");
		}

		if (!conn)
			DBLINK_CONN_NOT_AVAIL;

		if (!is_async || (is_async && do_get))
		{
			/* synchronous query, or async result retrieval */
			if (!is_async)
				res = PQexec(conn, sql);
			else
			{
				res = PQgetResult(conn);
				/* NULL means we're all done with the async results */
				if (!res)
				{
					MemoryContextSwitchTo(oldcontext);
					SRF_RETURN_DONE(funcctx);
				}
			}

			if (!res ||
				(PQresultStatus(res) != PGRES_COMMAND_OK &&
				 PQresultStatus(res) != PGRES_TUPLES_OK))
			{
				dblink_res_error(conname, res, "could not execute query", fail);
					if (freeconn)
						PQfinish(conn);
					MemoryContextSwitchTo(oldcontext);
					SRF_RETURN_DONE(funcctx);
			}

			if (PQresultStatus(res) == PGRES_COMMAND_OK)
			{
				is_sql_cmd = true;

				/* need a tuple descriptor representing one TEXT column */
				tupdesc = CreateTemplateTupleDesc(1, false);
				TupleDescInitEntry(tupdesc, (AttrNumber) 1, "status",
								   TEXTOID, -1, 0);

				/*
				 * and save a copy of the command status string to return as
				 * our result tuple
				 */
				sql_cmd_status = PQcmdStatus(res);
				funcctx->max_calls = 1;
			}
			else
				funcctx->max_calls = PQntuples(res);

			/* got results, keep track of them */
			funcctx->user_fctx = res;

			/* if needed, close the connection to the database and cleanup */
			if (freeconn)
				PQfinish(conn);

			if (!is_sql_cmd)
			{
				/* get a tuple descriptor for our result type */
				switch (get_call_result_type(fcinfo, NULL, &tupdesc))
				{
					case TYPEFUNC_COMPOSITE:
						/* success */
						break;
					case TYPEFUNC_RECORD:
						/* failed to determine actual type of RECORD */
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("function returning record called in context "
							   "that cannot accept type record")));
						break;
					default:
						/* result type isn't composite */
						elog(ERROR, "return type must be a row type");
						break;
				}

				/* make sure we have a persistent copy of the tupdesc */
				tupdesc = CreateTupleDescCopy(tupdesc);
			}

			/*
			 * check result and tuple descriptor have the same number of
			 * columns
			 */
			if (PQnfields(res) != tupdesc->natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("remote query result rowtype does not match "
								"the specified FROM clause rowtype")));

			/* fast track when no results */
			if (funcctx->max_calls < 1)
			{
				if (res)
					PQclear(res);
				MemoryContextSwitchTo(oldcontext);
				SRF_RETURN_DONE(funcctx);
			}

			/* store needed metadata for subsequent calls */
			attinmeta = TupleDescGetAttInMetadata(tupdesc);
			funcctx->attinmeta = attinmeta;

			MemoryContextSwitchTo(oldcontext);
		}
		else
		{
			/* async query send */
			MemoryContextSwitchTo(oldcontext);
			PG_RETURN_INT32(PQsendQuery(conn, sql));
		}
	}

	if (is_async && !do_get)
	{
		/* async query send -- should not happen */
		elog(ERROR, "async query send called more than once");

	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * initialize per-call variables
	 */
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;

	res = (PGresult *) funcctx->user_fctx;
	attinmeta = funcctx->attinmeta;
	tupdesc = attinmeta->tupdesc;

	if (call_cntr < max_calls)	/* do when there is more left to send */
	{
		char	  **values;
		HeapTuple	tuple;
		Datum		result;

		if (!is_sql_cmd)
		{
			int			i;
			int			nfields = PQnfields(res);

			values = (char **) palloc(nfields * sizeof(char *));
			for (i = 0; i < nfields; i++)
			{
				if (PQgetisnull(res, call_cntr, i) == 0)
					values[i] = PQgetvalue(res, call_cntr, i);
				else
					values[i] = NULL;
			}
		}
		else
		{
			values = (char **) palloc(1 * sizeof(char *));
			values[0] = sql_cmd_status;
		}

		/* build the tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* make the tuple into a datum */
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
	{
		/* do when there is no more left */
		PQclear(res);
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * List all open dblink connections by name.
 * Returns an array of all connection names.
 * Takes no params
 */
PG_FUNCTION_INFO_V1(dblink_get_connections);
Datum
dblink_get_connections(PG_FUNCTION_ARGS)
{
	HASH_SEQ_STATUS status;
	remoteConnHashEnt *hentry;
	ArrayBuildState *astate = NULL;

	if (remoteConnHash)
	{
		hash_seq_init(&status, remoteConnHash);
		while ((hentry = (remoteConnHashEnt *) hash_seq_search(&status)) != NULL)
		{
			/* stash away current value */
			astate = accumArrayResult(astate,
									  PointerGetDatum(GET_TEXT(hentry->name)),
									  false, TEXTOID, CurrentMemoryContext);
		}
	}

	if (astate)
		PG_RETURN_DATUM(makeArrayResult(astate,
											  CurrentMemoryContext));
	else
		PG_RETURN_NULL();
}

/*
 * Checks if a given remote connection is busy
 *
 * Returns 1 if the connection is busy, 0 otherwise
 * Params:
 *	text connection_name - name of the connection to check
 *
 */
PG_FUNCTION_INFO_V1(dblink_is_busy);
Datum
dblink_is_busy(PG_FUNCTION_ARGS)
{
	PGconn	   *conn = NULL;
	remoteConn *rconn = NULL;

	DBLINK_INIT;
	DBLINK_GET_NAMED_CONN;

	PQconsumeInput(conn);
	PG_RETURN_INT32(PQisBusy(conn));
}

/*
 * Cancels a running request on a connection
 *
 * Returns text:
 *	"OK" if the cancel request has been sent correctly,
 *		an error message otherwise
 *
 * Params:
 *	text connection_name - name of the connection to check
 *
 */
PG_FUNCTION_INFO_V1(dblink_cancel_query);
Datum
dblink_cancel_query(PG_FUNCTION_ARGS)
{
	int			res = 0;
	PGconn	   *conn = NULL;
	remoteConn *rconn = NULL;
	PGcancel   *cancel;
	char		errbuf[256];

	DBLINK_INIT;
	DBLINK_GET_NAMED_CONN;
	cancel = PQgetCancel(conn);

	res = PQcancel(cancel, errbuf, 256);
	PQfreeCancel(cancel);

	if (res == 1)
		PG_RETURN_TEXT_P(GET_TEXT("OK"));
	else
		PG_RETURN_TEXT_P(GET_TEXT(errbuf));
}


/*
 * Get error message from a connection
 *
 * Returns text:
 *	"OK" if no error, an error message otherwise
 *
 * Params:
 *	text connection_name - name of the connection to check
 *
 */
PG_FUNCTION_INFO_V1(dblink_error_message);
Datum
dblink_error_message(PG_FUNCTION_ARGS)
{
	char	   *msg;
	PGconn	   *conn = NULL;
	remoteConn *rconn = NULL;

	DBLINK_INIT;
	DBLINK_GET_NAMED_CONN;

	msg = PQerrorMessage(conn);
	if (msg == NULL || msg[0] == '\0')
		PG_RETURN_TEXT_P(GET_TEXT("OK"));
	else
		PG_RETURN_TEXT_P(GET_TEXT(msg));
}

/*
 * Execute an SQL non-SELECT command
 */
PG_FUNCTION_INFO_V1(dblink_exec);
Datum
dblink_exec(PG_FUNCTION_ARGS)
{
	text	   *volatile sql_cmd_status = NULL;
	PGconn	   *volatile conn = NULL;
	volatile bool freeconn = false;

	DBLINK_INIT;

	PG_TRY();
	{
	char	   *msg;
	PGresult   *res = NULL;
	TupleDesc	tupdesc = NULL;
	char	   *connstr = NULL;
	char	   *sql = NULL;
	char	   *conname = NULL;
	remoteConn *rconn = NULL;
	bool		fail = true;	/* default to backward compatible behavior */

	if (PG_NARGS() == 3)
	{
		/* must be text,text,bool */
		DBLINK_GET_CONN;
		sql = GET_STR(PG_GETARG_TEXT_P(1));
		fail = PG_GETARG_BOOL(2);
	}
	else if (PG_NARGS() == 2)
	{
		/* might be text,text or text,bool */
		if (get_fn_expr_argtype(fcinfo->flinfo, 1) == BOOLOID)
		{
			conn = pconn->conn;
			sql = GET_STR(PG_GETARG_TEXT_P(0));
			fail = PG_GETARG_BOOL(1);
		}
		else
		{
			DBLINK_GET_CONN;
			sql = GET_STR(PG_GETARG_TEXT_P(1));
		}
	}
	else if (PG_NARGS() == 1)
	{
		/* must be single text argument */
		conn = pconn->conn;
		sql = GET_STR(PG_GETARG_TEXT_P(0));
	}
	else
		/* shouldn't happen */
		elog(ERROR, "wrong number of arguments");

	if (!conn)
		DBLINK_CONN_NOT_AVAIL;

	res = PQexec(conn, sql);
	if (!res ||
		(PQresultStatus(res) != PGRES_COMMAND_OK &&
		 PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		if (fail)
			DBLINK_RES_ERROR("sql error");
		else
			DBLINK_RES_ERROR_AS_NOTICE("sql error");

		/* need a tuple descriptor representing one TEXT column */
		tupdesc = CreateTemplateTupleDesc(1, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "status",
						   TEXTOID, -1, 0);

		/*
		 * and save a copy of the command status string to return as our
		 * result tuple
		 */
		sql_cmd_status = GET_TEXT("ERROR");

	}
	else if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		/* need a tuple descriptor representing one TEXT column */
		tupdesc = CreateTemplateTupleDesc(1, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "status",
						   TEXTOID, -1, 0);

		/*
		 * and save a copy of the command status string to return as our
		 * result tuple
		 */
		sql_cmd_status = GET_TEXT(PQcmdStatus(res));
		PQclear(res);
	}
	else
	{
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
				 errmsg("statement returning results not allowed")));
	}
	}
	PG_CATCH();
	{
		/* if needed, close the connection to the database */
		if (freeconn)
			PQfinish(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* if needed, close the connection to the database */
	if (freeconn)
		PQfinish(conn);

	PG_RETURN_TEXT_P(sql_cmd_status);
}


/*
 * dblink_get_pkey
 *
 * Return list of primary key fields for the supplied relation,
 * or NULL if none exists.
 */
PG_FUNCTION_INFO_V1(dblink_get_pkey);
Datum
dblink_get_pkey(PG_FUNCTION_ARGS)
{
	int16		numatts;
	char	  **results;
	FuncCallContext *funcctx;
	int32		call_cntr;
	int32		max_calls;
	AttInMetadata *attinmeta;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		Relation	rel;
		TupleDesc	tupdesc;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* open target relation */
		rel = get_rel_from_relname(PG_GETARG_TEXT_P(0), AccessShareLock, ACL_SELECT);

		/* get the array of attnums */
		results = get_pkey_attnames(rel, &numatts);

		relation_close(rel, AccessShareLock);

		/*
		 * need a tuple descriptor representing one INT and one TEXT column
		 */
		tupdesc = CreateTemplateTupleDesc(2, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "position",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "colname",
						   TEXTOID, -1, 0);

		/*
		 * Generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		if ((results != NULL) && (numatts > 0))
		{
			funcctx->max_calls = numatts;

			/* got results, keep track of them */
			funcctx->user_fctx = results;
		}
		else
		{
			/* fast track when no results */
			MemoryContextSwitchTo(oldcontext);
			SRF_RETURN_DONE(funcctx);
		}

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * initialize per-call variables
	 */
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;

	results = (char **) funcctx->user_fctx;
	attinmeta = funcctx->attinmeta;

	if (call_cntr < max_calls)	/* do when there is more left to send */
	{
		char	  **values;
		HeapTuple	tuple;
		Datum		result;

		values = (char **) palloc(2 * sizeof(char *));
		values[0] = (char *) palloc(12);		/* sign, 10 digits, '\0' */

		sprintf(values[0], "%d", call_cntr + 1);

		values[1] = results[call_cntr];

		/* build the tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* make the tuple into a datum */
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}


/*
 * dblink_build_sql_insert
 *
 * Used to generate an SQL insert statement
 * based on an existing tuple in a local relation.
 * This is useful for selectively replicating data
 * to another server via dblink.
 *
 * API:
 * <relname> - name of local table of interest
 * <pkattnums> - an int2vector of attnums which will be used
 * to identify the local tuple of interest
 * <pknumatts> - number of attnums in pkattnums
 * <src_pkattvals_arry> - text array of key values which will be used
 * to identify the local tuple of interest
 * <tgt_pkattvals_arry> - text array of key values which will be used
 * to build the string for execution remotely. These are substituted
 * for their counterparts in src_pkattvals_arry
 */
PG_FUNCTION_INFO_V1(dblink_build_sql_insert);
Datum
dblink_build_sql_insert(PG_FUNCTION_ARGS)
{
	text	   *relname_text = PG_GETARG_TEXT_P(0);
	int2vector *pkattnums_arg = (int2vector *) PG_GETARG_POINTER(1);
	int32		pknumatts_arg = PG_GETARG_INT32(2);
	ArrayType  *src_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);
	ArrayType  *tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(4);
	Relation	rel;
	int		   *pkattnums;
	int			pknumatts;
	char	  **src_pkattvals;
	char	  **tgt_pkattvals;
	int			src_nitems;
	int			tgt_nitems;
	char	   *sql;

	/*
	 * Open target relation.
	 */
	rel = get_rel_from_relname(relname_text, AccessShareLock, ACL_SELECT);

	/*
	 * Process pkattnums argument.
	 */
	validate_pkattnums(rel, pkattnums_arg, pknumatts_arg,
					   &pkattnums, &pknumatts);

	/*
	 * Source array is made up of key values that will be used to locate the
	 * tuple of interest from the local system.
	 */
	src_pkattvals = get_text_array_contents(src_pkattvals_arry, &src_nitems);

	/*
	 * There should be one source array key value for each key attnum
	 */
	if (src_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("source key array length must match number of key " \
						"attributes")));

	/*
	 * Target array is made up of key values that will be used to build the
	 * SQL string for use on the remote system.
	 */
	tgt_pkattvals = get_text_array_contents(tgt_pkattvals_arry, &tgt_nitems);

	/*
	 * There should be one target array key value for each key attnum
	 */
	if (tgt_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("target key array length must match number of key " \
						"attributes")));

	/*
	 * Prep work is finally done. Go get the SQL string.
	 */
	sql = get_sql_insert(rel, pkattnums, pknumatts, src_pkattvals, tgt_pkattvals);

	/*
	 * Now we can close the relation.
	 */
	relation_close(rel, AccessShareLock);

	/*
	 * And send it
	 */
	PG_RETURN_TEXT_P(GET_TEXT(sql));
}


/*
 * dblink_build_sql_delete
 *
 * Used to generate an SQL delete statement.
 * This is useful for selectively replicating a
 * delete to another server via dblink.
 *
 * API:
 * <relname> - name of remote table of interest
 * <pkattnums> - an int2vector of attnums which will be used
 * to identify the remote tuple of interest
 * <pknumatts> - number of attnums in pkattnums
 * <tgt_pkattvals_arry> - text array of key values which will be used
 * to build the string for execution remotely.
 */
PG_FUNCTION_INFO_V1(dblink_build_sql_delete);
Datum
dblink_build_sql_delete(PG_FUNCTION_ARGS)
{
	text	   *relname_text = PG_GETARG_TEXT_P(0);
	int2vector *pkattnums_arg = (int2vector *) PG_GETARG_POINTER(1);
	int32		pknumatts_arg = PG_GETARG_INT32(2);
	ArrayType  *tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);
	Relation	rel;
	int		   *pkattnums;
	int			pknumatts;
	char	  **tgt_pkattvals;
	int			tgt_nitems;
	char	   *sql;

	/*
	 * Open target relation.
	 */
	rel = get_rel_from_relname(relname_text, AccessShareLock, ACL_SELECT);

	/*
	 * Process pkattnums argument.
	 */
	validate_pkattnums(rel, pkattnums_arg, pknumatts_arg,
					   &pkattnums, &pknumatts);

	/*
	 * Target array is made up of key values that will be used to build the
	 * SQL string for use on the remote system.
	 */
	tgt_pkattvals = get_text_array_contents(tgt_pkattvals_arry, &tgt_nitems);

	/*
	 * There should be one target array key value for each key attnum
	 */
	if (tgt_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("target key array length must match number of key " \
						"attributes")));

	/*
	 * Prep work is finally done. Go get the SQL string.
	 */
	sql = get_sql_delete(rel, pkattnums, pknumatts, tgt_pkattvals);

	/*
	 * Now we can close the relation.
	 */
	relation_close(rel, AccessShareLock);

	/*
	 * And send it
	 */
	PG_RETURN_TEXT_P(GET_TEXT(sql));
}


/*
 * dblink_build_sql_update
 *
 * Used to generate an SQL update statement
 * based on an existing tuple in a local relation.
 * This is useful for selectively replicating data
 * to another server via dblink.
 *
 * API:
 * <relname> - name of local table of interest
 * <pkattnums> - an int2vector of attnums which will be used
 * to identify the local tuple of interest
 * <pknumatts> - number of attnums in pkattnums
 * <src_pkattvals_arry> - text array of key values which will be used
 * to identify the local tuple of interest
 * <tgt_pkattvals_arry> - text array of key values which will be used
 * to build the string for execution remotely. These are substituted
 * for their counterparts in src_pkattvals_arry
 */
PG_FUNCTION_INFO_V1(dblink_build_sql_update);
Datum
dblink_build_sql_update(PG_FUNCTION_ARGS)
{
	text	   *relname_text = PG_GETARG_TEXT_P(0);
	int2vector *pkattnums_arg = (int2vector *) PG_GETARG_POINTER(1);
	int32		pknumatts_arg = PG_GETARG_INT32(2);
	ArrayType  *src_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(3);
	ArrayType  *tgt_pkattvals_arry = PG_GETARG_ARRAYTYPE_P(4);
	Relation	rel;
	int		   *pkattnums;
	int			pknumatts;
	char	  **src_pkattvals;
	char	  **tgt_pkattvals;
	int			src_nitems;
	int			tgt_nitems;
	char	   *sql;

	/*
	 * Open target relation.
	 */
	rel = get_rel_from_relname(relname_text, AccessShareLock, ACL_SELECT);

	/*
	 * Process pkattnums argument.
	 */
	validate_pkattnums(rel, pkattnums_arg, pknumatts_arg,
					   &pkattnums, &pknumatts);

	/*
	 * Source array is made up of key values that will be used to locate the
	 * tuple of interest from the local system.
	 */
	src_pkattvals = get_text_array_contents(src_pkattvals_arry, &src_nitems);

	/*
	 * There should be one source array key value for each key attnum
	 */
	if (src_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("source key array length must match number of key " \
						"attributes")));

	/*
	 * Target array is made up of key values that will be used to build the
	 * SQL string for use on the remote system.
	 */
	tgt_pkattvals = get_text_array_contents(tgt_pkattvals_arry, &tgt_nitems);

	/*
	 * There should be one target array key value for each key attnum
	 */
	if (tgt_nitems != pknumatts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("target key array length must match number of key " \
						"attributes")));

	/*
	 * Prep work is finally done. Go get the SQL string.
	 */
	sql = get_sql_update(rel, pkattnums, pknumatts, src_pkattvals, tgt_pkattvals);

	/*
	 * Now we can close the relation.
	 */
	relation_close(rel, AccessShareLock);

	/*
	 * And send it
	 */
	PG_RETURN_TEXT_P(GET_TEXT(sql));
}

/*************************************************************
 * internal functions
 */


/*
 * get_pkey_attnames
 *
 * Get the primary key attnames for the given relation.
 * Return NULL, and set numatts = 0, if no primary key exists.
 */
static char **
get_pkey_attnames(Relation rel, int16 *numatts)
{
	Relation	indexRelation;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	indexTuple;
	int			i;
	char	  **result = NULL;
	TupleDesc	tupdesc;

	/* initialize numatts to 0 in case no primary key exists */
	*numatts = 0;

	tupdesc = rel->rd_att;

	/* Prepare to scan pg_index for entries having indrelid = this rel. */
	indexRelation = heap_open(IndexRelationId, AccessShareLock);
	ScanKeyInit(&skey,
				Anum_pg_index_indrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));

	scan = systable_beginscan(indexRelation, IndexIndrelidIndexId, true,
							  SnapshotNow, 1, &skey);

	while (HeapTupleIsValid(indexTuple = systable_getnext(scan)))
	{
		Form_pg_index index = (Form_pg_index) GETSTRUCT(indexTuple);

		/* we're only interested if it is the primary key */
		if (index->indisprimary)
		{
			*numatts = index->indnatts;
			if (*numatts > 0)
			{
				result = (char **) palloc(*numatts * sizeof(char *));

				for (i = 0; i < *numatts; i++)
					result[i] = SPI_fname(tupdesc, index->indkey.values[i]);
			}
			break;
		}
	}

	systable_endscan(scan);
	heap_close(indexRelation, AccessShareLock);

	return result;
}

/*
 * Deconstruct a text[] into C-strings (note any NULL elements will be
 * returned as NULL pointers)
 */
static char **
get_text_array_contents(ArrayType *array, int *numitems)
{
	int			ndim = ARR_NDIM(array);
	int		   *dims = ARR_DIMS(array);
	int			nitems;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char	  **values;
	char	   *ptr;
	bits8	   *bitmap;
	int			bitmask;
	int			i;

	Assert(ARR_ELEMTYPE(array) == TEXTOID);

	*numitems = nitems = ArrayGetNItems(ndim, dims);

	get_typlenbyvalalign(ARR_ELEMTYPE(array),
						 &typlen, &typbyval, &typalign);

	values = (char **) palloc(nitems * sizeof(char *));

	ptr = ARR_DATA_PTR(array);
	bitmap = ARR_NULLBITMAP(array);
	bitmask = 1;

	for (i = 0; i < nitems; i++)
	{
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			values[i] = NULL;
		}
		else
		{
			values[i] = DatumGetCString(DirectFunctionCall1(textout,
													  PointerGetDatum(ptr)));
			ptr = att_addlength_pointer(ptr, typlen, ptr);
			ptr = (char *) att_align_nominal(ptr, typalign);
		}

		/* advance bitmap pointer if any */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}

	return values;
}

static char *
get_sql_insert(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals, char **tgt_pkattvals)
{
	char	   *relname;
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			natts;
	StringInfoData buf;
	char	   *val;
	int			key;
	int			i;
	bool		needComma;

	initStringInfo(&buf);

	/* get relation name including any needed schema prefix and quoting */
	relname = generate_relation_name(rel);

	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	tuple = get_tuple_of_interest(rel, pkattnums, pknumatts, src_pkattvals);
	if (!tuple)
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("source row not found")));

	appendStringInfo(&buf, "INSERT INTO %s(", relname);

	needComma = false;
	for (i = 0; i < natts; i++)
	{
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		if (needComma)
			appendStringInfo(&buf, ",");

		appendStringInfoString(&buf,
					  quote_ident_cstr(NameStr(tupdesc->attrs[i]->attname)));
		needComma = true;
	}

	appendStringInfo(&buf, ") VALUES(");

	/*
	 * Note: i is physical column number (counting from 0).
	 */
	needComma = false;
	for (i = 0; i < natts; i++)
	{
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		if (needComma)
			appendStringInfo(&buf, ",");

		key = get_attnum_pk_pos(pkattnums, pknumatts, i);

		if (key >= 0)
			val = tgt_pkattvals[key] ? pstrdup(tgt_pkattvals[key]) : NULL;
		else
			val = SPI_getvalue(tuple, tupdesc, i + 1);

		if (val != NULL)
		{
			appendStringInfoString(&buf, quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfo(&buf, "NULL");
		needComma = true;
	}
	appendStringInfo(&buf, ")");

	return (buf.data);
}

static char *
get_sql_delete(Relation rel, int *pkattnums, int pknumatts, char **tgt_pkattvals)
{
	char	   *relname;
	TupleDesc	tupdesc;
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);

	/* get relation name including any needed schema prefix and quoting */
	relname = generate_relation_name(rel);

	tupdesc = rel->rd_att;

	appendStringInfo(&buf, "DELETE FROM %s WHERE ", relname);
	for (i = 0; i < pknumatts; i++)
	{
		int			pkattnum = pkattnums[i];

		if (i > 0)
			appendStringInfo(&buf, " AND ");

		appendStringInfoString(&buf,
		   quote_ident_cstr(NameStr(tupdesc->attrs[pkattnum]->attname)));

		if (tgt_pkattvals[i] != NULL)
			appendStringInfo(&buf, " = %s",
							 quote_literal_cstr(tgt_pkattvals[i]));
		else
			appendStringInfo(&buf, " IS NULL");
	}

	return (buf.data);
}

static char *
get_sql_update(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals, char **tgt_pkattvals)
{
	char	   *relname;
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			natts;
	StringInfoData buf;
	char	   *val;
	int			key;
	int			i;
	bool		needComma;

	initStringInfo(&buf);

	/* get relation name including any needed schema prefix and quoting */
	relname = generate_relation_name(rel);

	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	tuple = get_tuple_of_interest(rel, pkattnums, pknumatts, src_pkattvals);
	if (!tuple)
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("source row not found")));

	appendStringInfo(&buf, "UPDATE %s SET ", relname);

	/*
	 * Note: i is physical column number (counting from 0).
	 */
	needComma = false;
	for (i = 0; i < natts; i++)
	{
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		if (needComma)
			appendStringInfo(&buf, ", ");

		appendStringInfo(&buf, "%s = ",
					  quote_ident_cstr(NameStr(tupdesc->attrs[i]->attname)));

		key = get_attnum_pk_pos(pkattnums, pknumatts, i);

		if (key >= 0)
			val = tgt_pkattvals[key] ? pstrdup(tgt_pkattvals[key]) : NULL;
		else
			val = SPI_getvalue(tuple, tupdesc, i + 1);

		if (val != NULL)
		{
			appendStringInfoString(&buf, quote_literal_cstr(val));
			pfree(val);
		}
		else
			appendStringInfoString(&buf, "NULL");
		needComma = true;
	}

	appendStringInfo(&buf, " WHERE ");

	for (i = 0; i < pknumatts; i++)
	{
		int			pkattnum = pkattnums[i];

		if (i > 0)
			appendStringInfo(&buf, " AND ");

		appendStringInfo(&buf, "%s",
		   quote_ident_cstr(NameStr(tupdesc->attrs[pkattnum]->attname)));

		val = tgt_pkattvals[i];

		if (val != NULL)
			appendStringInfo(&buf, " = %s", quote_literal_cstr(val));
		else
			appendStringInfo(&buf, " IS NULL");
	}

	return (buf.data);
}

/*
 * Return a properly quoted literal value.
 * Uses quote_literal in quote.c
 */
static char *
quote_literal_cstr(char *rawstr)
{
	text	   *rawstr_text;
	text	   *result_text;
	char	   *result;

	rawstr_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(rawstr)));
	result_text = DatumGetTextP(DirectFunctionCall1(quote_literal, PointerGetDatum(rawstr_text)));
	result = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(result_text)));

	return result;
}

/*
 * Return a properly quoted identifier.
 * Uses quote_ident in quote.c
 */
static char *
quote_ident_cstr(char *rawstr)
{
	text	   *rawstr_text;
	text	   *result_text;
	char	   *result;

	rawstr_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(rawstr)));
	result_text = DatumGetTextP(DirectFunctionCall1(quote_ident, PointerGetDatum(rawstr_text)));
	result = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(result_text)));

	return result;
}

static int
get_attnum_pk_pos(int *pkattnums, int pknumatts, int key)
{
	int			i;

	/*
	 * Not likely a long list anyway, so just scan for the value
	 */
	for (i = 0; i < pknumatts; i++)
		if (key == pkattnums[i])
			return i;

	return -1;
}

static HeapTuple
get_tuple_of_interest(Relation rel, int *pkattnums, int pknumatts, char **src_pkattvals)
{
	char	   *relname;
	TupleDesc	tupdesc;
	int			natts;
	StringInfoData buf;
	int			ret;
	HeapTuple	tuple;
	int			i;

	/*
	 * Connect to SPI manager
	 */
	if ((ret = SPI_connect()) < 0)
		/* internal error */
		elog(ERROR, "SPI connect failure - returned %d", ret);

	initStringInfo(&buf);

	/* get relation name including any needed schema prefix and quoting */
	relname = generate_relation_name(rel);

	tupdesc = rel->rd_att;
	natts = tupdesc->natts;

	/*
	 * Build sql statement to look up tuple of interest, ie, the one matching
	 * src_pkattvals.  We used to use "SELECT *" here, but it's simpler to
	 * generate a result tuple that matches the table's physical structure,
	 * with NULLs for any dropped columns.  Otherwise we have to deal with
	 * two different tupdescs and everything's very confusing.
	 */
	appendStringInfoString(&buf, "SELECT ");

	for (i = 0; i < natts; i++)
	{
		if (i > 0)
			appendStringInfoString(&buf, ", ");

		if (tupdesc->attrs[i]->attisdropped)
			appendStringInfoString(&buf, "NULL");
		else
			appendStringInfoString(&buf,
								   quote_ident_cstr(NameStr(tupdesc->attrs[i]->attname)));
	}

	appendStringInfo(&buf, " FROM %s WHERE ", relname);

	for (i = 0; i < pknumatts; i++)
	{
		int			pkattnum = pkattnums[i];

		if (i > 0)
			appendStringInfo(&buf, " AND ");

		appendStringInfoString(&buf,
		   quote_ident_cstr(NameStr(tupdesc->attrs[pkattnum]->attname)));

		if (src_pkattvals[i] != NULL)
			appendStringInfo(&buf, " = %s",
							 quote_literal_cstr(src_pkattvals[i]));
		else
			appendStringInfo(&buf, " IS NULL");
	}

	/*
	 * Retrieve the desired tuple
	 */
	ret = SPI_exec(buf.data, 0);
	pfree(buf.data);

	/*
	 * Only allow one qualifying tuple
	 */
	if ((ret == SPI_OK_SELECT) && (SPI_processed > 1))
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("source criteria matched more than one record")));

	else if (ret == SPI_OK_SELECT && SPI_processed == 1)
	{
		SPITupleTable *tuptable = SPI_tuptable;

		tuple = SPI_copytuple(tuptable->vals[0]);
		SPI_finish();

		return tuple;
	}
	else
	{
		/*
		 * no qualifying tuples
		 */
		SPI_finish();

		return NULL;
	}

	/*
	 * never reached, but keep compiler quiet
	 */
	return NULL;
}

/*
 * Open the relation named by relname_text, acquire specified type of lock,
 * verify we have specified permissions.
 * Caller must close rel when done with it.
 */
static Relation
get_rel_from_relname(text *relname_text, LOCKMODE lockmode, AclMode aclmode)
{
	RangeVar   *relvar;
	Relation	rel;
	AclResult	aclresult;

	relvar = makeRangeVarFromNameList(textToQualifiedNameList(relname_text));
	rel = heap_openrv(relvar, lockmode);

	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  aclmode);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_CLASS,
					   RelationGetRelationName(rel));

	return rel;
}

/*
 * generate_relation_name - copied from ruleutils.c
 *		Compute the name to display for a relation
 *
 * The result includes all necessary quoting and schema-prefixing.
 */
static char *
generate_relation_name(Relation rel)
{
	char	   *nspname;
	char	   *result;

	/* Qualify the name if not visible in search path */
	if (RelationIsVisible(RelationGetRelid(rel)))
		nspname = NULL;
	else
		nspname = get_namespace_name(rel->rd_rel->relnamespace);

	result = quote_qualified_identifier(nspname, RelationGetRelationName(rel));

	return result;
}


static remoteConn *
getConnectionByName(const char *name)
{
	remoteConnHashEnt *hentry;
	char	   *key;

	if (!remoteConnHash)
		remoteConnHash = createConnHash();

	key = pstrdup(name);
	truncate_identifier(key, strlen(key), false);
	hentry = (remoteConnHashEnt *) hash_search(remoteConnHash,
											   key, HASH_FIND, NULL);

	if (hentry)
		return (hentry->rconn);

	return (NULL);
}

static HTAB *
createConnHash(void)
{
	HASHCTL		ctl;

	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = sizeof(remoteConnHashEnt);

	return hash_create("Remote Con hash", NUMCONN, &ctl, HASH_ELEM);
}

static void
createNewConnection(const char *name, remoteConn * rconn)
{
	remoteConnHashEnt *hentry;
	bool		found;
	char	   *key;

	if (!remoteConnHash)
		remoteConnHash = createConnHash();

	key = pstrdup(name);
	truncate_identifier(key, strlen(key), true);
	hentry = (remoteConnHashEnt *) hash_search(remoteConnHash, key,
											   HASH_ENTER, &found);

	if (found)
	{
		PQfinish(rconn->conn);
		pfree(rconn);

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("duplicate connection name")));
	}

	hentry->rconn = rconn;
	strlcpy(hentry->name, name, sizeof(hentry->name));
}

static void
deleteConnection(const char *name)
{
	remoteConnHashEnt *hentry;
	bool		found;
	char	   *key;

	if (!remoteConnHash)
		remoteConnHash = createConnHash();

	key = pstrdup(name);
	truncate_identifier(key, strlen(key), false);
	hentry = (remoteConnHashEnt *) hash_search(remoteConnHash,
											   key, HASH_REMOVE, &found);

	if (!hentry)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("undefined connection name")));

}

static void
dblink_security_check(PGconn *conn, remoteConn *rconn)
{
	if (!superuser())
	{
		if (!PQconnectionUsedPassword(conn))
		{
			PQfinish(conn);
			if (rconn)
				pfree(rconn);

			ereport(ERROR,
				  (errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
				   errmsg("password is required"),
				   errdetail("Non-superuser cannot connect if the server does not request a password."),
				   errhint("Target server's authentication method must be changed.")));
		}
	}
}

/*
 * For non-superusers, insist that the connstr specify a password.  This
 * prevents a password from being picked up from .pgpass, a service file,
 * the environment, etc.  We don't want the postgres user's passwords
 * to be accessible to non-superusers.
 *
 * For Greenplum, dblink uses built libpq to construct conninfo, whose user is
 * environment variable PGUSER, which is wrong, modifies this function to add
 * the session's username into connstr.
 *
 */
static char *
dblink_connstr_check(const char *connstr)
{
	char	*connstr_modified = (char *) connstr;

	if (!superuser())
	{
		PQconninfoOption   *options;
		PQconninfoOption   *option;
		bool				connstr_gives_password = false;
		bool				username_is_set = false;
		bool				host_is_set = false;

		options = PQconninfoParse(connstr, NULL);
		if (options)
		{
			for (option = options; option->keyword != NULL; option++)
			{
				if (strcmp(option->keyword, "host") == 0)
				{
					if (option->val != NULL && option->val[0] != '\0')
					{
						host_is_set = true;
					}
				}

				if (strcmp(option->keyword, "user") == 0)
				{
					if (option->val == NULL || option->val[0] == '\0')
					{
						char *username = GetUserNameFromId(GetUserId());

						/* 7 is strlen("user= ") + length of '\0' */
						connstr_modified = palloc0(7 + strlen(username) + strlen(connstr));
						sprintf(connstr_modified, "user=%s %s", username, connstr);
					}

					username_is_set = true;
				}

				if (strcmp(option->keyword, "password") == 0)
				{
					if (option->val != NULL && option->val[0] != '\0')
					{
						connstr_gives_password = true;
					}
				}

				if (host_is_set && username_is_set && connstr_gives_password)
					break;
			}
			PQconninfoFree(options);
		}

		if (!host_is_set)
			ereport(ERROR,
					(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
					 errmsg("host is required"),
					 errdetail("Non-superusers must provide a host in the connection string.")));

		if (!connstr_gives_password)
			ereport(ERROR,
					(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
					 errmsg("password is required"),
					 errdetail("Non-superusers must provide a password in the connection string.")));
	}

	return connstr_modified;
}

static void
dblink_res_error(const char *conname, PGresult *res, const char *dblink_context_msg, bool fail)
{
	int			level;
	char	   *pg_diag_sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	char	   *pg_diag_message_primary = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	char	   *pg_diag_message_detail = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
	char	   *pg_diag_message_hint = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
	char	   *pg_diag_context = PQresultErrorField(res, PG_DIAG_CONTEXT);
	int			sqlstate;
	char	   *message_primary;
	char	   *message_detail;
	char	   *message_hint;
	char	   *message_context;
	const char *dblink_context_conname = "unnamed";

	if (fail)
		level = ERROR;
	else
		level = NOTICE;

	if (pg_diag_sqlstate)
		sqlstate = MAKE_SQLSTATE(pg_diag_sqlstate[0],
								 pg_diag_sqlstate[1],
								 pg_diag_sqlstate[2],
								 pg_diag_sqlstate[3],
								 pg_diag_sqlstate[4]);
	else
		sqlstate = ERRCODE_CONNECTION_FAILURE;

	xpstrdup(message_primary, pg_diag_message_primary);
	xpstrdup(message_detail, pg_diag_message_detail);
	xpstrdup(message_hint, pg_diag_message_hint);
	xpstrdup(message_context, pg_diag_context);

	if (res)
		PQclear(res);

	if (conname)
		dblink_context_conname = conname;

	ereport(level,
		(errcode(sqlstate),
		 message_primary ? errmsg("%s", message_primary) : errmsg("unknown error"),
		 message_detail ? errdetail("%s", message_detail) : 0,
		 message_hint ? errhint("%s", message_hint) : 0,
		 message_context ? errcontext("%s", message_context) : 0,
		 errcontext("Error occurred on dblink connection named \"%s\": %s.",
					dblink_context_conname, dblink_context_msg)));
}

/*
 * Validate the PK-attnums argument for dblink_build_sql_insert() and related
 * functions, and translate to the internal representation.
 *
 * The user supplies an int2vector of 1-based physical attnums, plus a count
 * argument (the need for the separate count argument is historical, but we
 * still check it).  We check that each attnum corresponds to a valid,
 * non-dropped attribute of the rel.  We do *not* prevent attnums from being
 * listed twice, though the actual use-case for such things is dubious.
 *
 * The internal representation is a palloc'd int array of 0-based physical
 * attnums.
 */
static void
validate_pkattnums(Relation rel,
				   int2vector *pkattnums_arg, int32 pknumatts_arg,
				   int **pkattnums, int *pknumatts)
{
	TupleDesc	tupdesc = rel->rd_att;
	int			natts = tupdesc->natts;
	int			i;

	/* Don't take more array elements than there are */
	pknumatts_arg = Min(pknumatts_arg, pkattnums_arg->dim1);

	/* Must have at least one pk attnum selected */
	if (pknumatts_arg <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("number of key attributes must be > 0")));

	/* Allocate output array */
	*pkattnums = (int *) palloc(pknumatts_arg * sizeof(int));
	*pknumatts = pknumatts_arg;

	/* Validate attnums and convert to internal form */
	for (i = 0; i < pknumatts_arg; i++)
	{
		int		pkattnum = pkattnums_arg->values[i];

		if (pkattnum <= 0 || pkattnum > natts ||
			tupdesc->attrs[pkattnum - 1]->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid attribute number %d", pkattnum)));
		(*pkattnums)[i] = pkattnum - 1;
	}
}
