/*-------------------------------------------------------------------------
 *
 * pg_dump.c
 *	  pg_dump is a utility for dumping out a postgres database
 *	  into a script file.
 *
 * Portions Copyright (c) 2005-2010, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	pg_dump will read the system catalogs in a database and dump out a
 *	script that reproduces the schema in terms of SQL that is understood
 *	by PostgreSQL
 *
 *	Note that pg_dump runs in a serializable transaction, so it sees a
 *	consistent snapshot of the database including system catalogs.
 *	However, it relies in part on various specialized backend functions
 *	like pg_get_indexdef(), and those things tend to run on SnapshotNow
 *	time, ie they look at the currently committed state.  So it is
 *	possible to get 'cache lookup failed' error if someone performs DDL
 *	changes while a dump is happening. The window for this sort of thing
 *	is from the beginning of the serializable transaction to
 *	getSchemaData() (when pg_dump acquires AccessShareLock on every
 *	table it intends to dump). It isn't very large, but it can happen.
 *
 *	http://archives.postgresql.org/pgsql-bugs/2010-02/msg00187.php
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/bin/pg_dump/pg_dump.c,v 1.547 2009/09/11 19:17:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * Although this is not a backend module, we must include postgres.h anyway
 * so that we can include a bunch of backend include files.  pg_dump has
 * never pretended to be very independent of the backend anyhow ...
 * Is this still true?  PG 9 doesn't include this.
 */
#include "postgres.h"
#include "postgres_fe.h"

#include <unistd.h>
#include <ctype.h>
#ifdef ENABLE_NLS
#include <locale.h>
#endif
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include "getopt_long.h"

#ifndef HAVE_INT_OPTRESET
int			optreset;
#endif

#include "access/attnum.h"
#include "access/sysattr.h"
#include "catalog/pg_magic_oid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/sequence.h"
#include "libpq/libpq-fs.h"

#include "pg_backup_archiver.h"
#include "dumputils.h"

extern char *optarg;
extern int	optind,
			opterr;


typedef struct
{
	const char *descr;			/* comment for an object */
	Oid			classoid;		/* object class (catalog OID) */
	Oid			objoid;			/* object OID */
	int			objsubid;		/* subobject (table column #) */
} CommentItem;


/* global decls */
bool		g_verbose;			/* User wants verbose narration of our
								 * activities. */
Archive    *g_fout;				/* the script file */
PGconn	   *g_conn;				/* the database connection */

/* various user-settable parameters */
bool		schemaOnly;
bool		dataOnly;
bool		aclsSkip;

/* START MPP ADDITION */
bool		dumpPolicy;
bool		isGPbackend;
int			preDataSchemaOnly;	/* int because getopt_long() */
int			postDataSchemaOnly;

/* END MPP ADDITION */

/* subquery used to convert user ID (eg, datdba) to user name */
static const char *username_subquery;

/*
 * Object inclusion/exclusion lists
 *
 * The string lists record the patterns given by command-line switches,
 * which we then convert to lists of OIDs of matching objects.
 */
static SimpleStringList schema_include_patterns = {NULL, NULL};
static SimpleOidList schema_include_oids = {NULL, NULL};
static SimpleStringList schema_exclude_patterns = {NULL, NULL};
static SimpleOidList schema_exclude_oids = {NULL, NULL};

static SimpleStringList table_include_patterns = {NULL, NULL};
static SimpleOidList table_include_oids = {NULL, NULL};
static SimpleStringList table_exclude_patterns = {NULL, NULL};
static SimpleOidList table_exclude_oids = {NULL, NULL};

static SimpleStringList relid_string_list = {NULL, NULL};
static SimpleStringList funcid_string_list = {NULL, NULL};
static SimpleOidList function_include_oids = {NULL, NULL};

/*
 * Indicates whether or not SET SESSION AUTHORIZATION statements should be emitted
 * instead of ALTER ... OWNER statements to establish object ownership.
 * Set through the --use-set-session-authorization option.
 */
static int	use_setsessauth = 0;

/* default, if no "inclusion" switches appear, is to dump everything */
static bool include_everything = true;

static int	binary_upgrade = 0;

char		g_opaque_type[10];	/* name for the opaque type */

/* placeholders for the delimiters for comments */
char		g_comment_start[10];
char		g_comment_end[10];

static const CatalogId nilCatalogId = {0, 0};

const char *EXT_PARTITION_NAME_POSTFIX = "_external_partition__";
/* flag to turn on/off dollar quoting */
static int	disable_dollar_quoting = 0;
static int	dump_inserts = 0;
static int	column_inserts = 0;

/* flag indicating whether or not this GP database supports partitioning */
static bool gp_partitioning_available = false;

/* flag indicating whether or not this GP database supports column encoding */
static bool gp_attribute_encoding_available = false;

static void help(const char *progname);
static void expand_schema_name_patterns(SimpleStringList *patterns,
							SimpleOidList *oids);
static void expand_table_name_patterns(SimpleStringList *patterns,
						   SimpleOidList *oids);
static void expand_oid_patterns(SimpleStringList *patterns,
						   SimpleOidList *oids);
static NamespaceInfo *findNamespace(Oid nsoid, Oid objoid);
static void dumpTableData(Archive *fout, TableDataInfo *tdinfo);
static void dumpComment(Archive *fout, const char *target,
			const char *namespace, const char *owner,
			CatalogId catalogId, int subid, DumpId dumpId);
static int findComments(Archive *fout, Oid classoid, Oid objoid,
			 CommentItem **items);
static int	collectComments(Archive *fout, CommentItem **items);
static void dumpDumpableObject(Archive *fout, DumpableObject *dobj);
static void dumpNamespace(Archive *fout, NamespaceInfo *nspinfo);
static void dumpExtension(Archive *fout, ExtensionInfo *extinfo);
static void dumpType(Archive *fout, TypeInfo *tinfo);
static void dumpBaseType(Archive *fout, TypeInfo *tinfo);
static void dumpTypeStorageOptions(Archive *fout, TypeStorageOptions *tstorageoptions);
static void dumpEnumType(Archive *fout, TypeInfo *tinfo);
static void dumpDomain(Archive *fout, TypeInfo *tinfo);
static void dumpCompositeType(Archive *fout, TypeInfo *tinfo);
static void dumpShellType(Archive *fout, ShellTypeInfo *stinfo);
static void dumpProcLang(Archive *fout, ProcLangInfo *plang);
static void dumpFunc(Archive *fout, FuncInfo *finfo);
static char *getFuncOwner(Oid funcOid, const char *templateField);
static void dumpPlTemplateFunc(Oid funcOid, const char *templateField, PQExpBuffer buffer);
static void dumpCast(Archive *fout, CastInfo *cast);
static void dumpOpr(Archive *fout, OprInfo *oprinfo);
static void dumpOpclass(Archive *fout, OpclassInfo *opcinfo);
static void dumpOpfamily(Archive *fout, OpfamilyInfo *opfinfo);
static void dumpConversion(Archive *fout, ConvInfo *convinfo);
static void dumpRule(Archive *fout, RuleInfo *rinfo);
static void dumpAgg(Archive *fout, AggInfo *agginfo);
static void dumpExtProtocol(Archive *fout, ExtProtInfo *ptcinfo);
static void dumpTrigger(Archive *fout, TriggerInfo *tginfo);
static void dumpTable(Archive *fout, TableInfo *tbinfo);
static void dumpTableSchema(Archive *fout, TableInfo *tbinfo);
static void dumpAttrDef(Archive *fout, AttrDefInfo *adinfo);
static void dumpSequence(Archive *fout, TableInfo *tbinfo);
static void dumpIndex(Archive *fout, IndxInfo *indxinfo);
static void dumpConstraint(Archive *fout, ConstraintInfo *coninfo);
static void dumpTableConstraintComment(Archive *fout, ConstraintInfo *coninfo);
static void dumpTSParser(Archive *fout, TSParserInfo *prsinfo);
static void dumpTSDictionary(Archive *fout, TSDictInfo *dictinfo);
static void dumpTSTemplate(Archive *fout, TSTemplateInfo *tmplinfo);
static void dumpTSConfig(Archive *fout, TSConfigInfo *cfginfo);

static void dumpACL(Archive *fout, CatalogId objCatId, DumpId objDumpId,
		const char *type, const char *name,
		const char *tag, const char *nspname, const char *owner,
		const char *acls);

static void getDependencies(void);
static void setExtPartDependency(TableInfo *tblinfo, int numTables);
static void getDomainConstraints(TypeInfo *tinfo);
static void getTableData(TableInfo *tblinfo, int numTables, bool oids);
static void makeTableDataInfo(TableInfo *tbinfo, bool oids);
static char *format_function_arguments(FuncInfo *finfo, char *funcargs);
static char *format_function_arguments_old(FuncInfo *finfo, int nallargs,
						  char **allargtypes,
						  char **argmodes,
						  char **argnames);
static char *format_function_signature(FuncInfo *finfo, bool honor_quotes);
static bool is_returns_table_function(int nallargs, char **argmodes);
static char *format_table_function_columns(FuncInfo *finfo, int nallargs,
							  char **allargtypes,
							  char **argmodes,
							  char **argnames);
static const char *convertRegProcReference(const char *proc);
static const char *convertOperatorReference(const char *opr);
static const char *convertTSFunction(Oid funcOid);
static void selectSourceSchema(const char *schemaName);
static bool testGPbackend(void);
static bool testPartitioningSupport(void);
static bool testAttributeEncodingSupport(void);
static char *getFormattedTypeName(Oid oid, OidOptions opts);
static const char *fmtQualifiedId(const char *schema, const char *id);
static bool hasBlobs(Archive *AH);
static int	dumpBlobs(Archive *AH, void *arg __attribute__((unused)));
static int	dumpBlobComments(Archive *AH, void *arg __attribute__((unused)));
static void dumpDatabase(Archive *AH);
static void dumpEncoding(Archive *AH);
static void dumpStdStrings(Archive *AH);
static void binary_upgrade_extension_member(PQExpBuffer upgrade_buffer,
								DumpableObject *dobj,
								const char *objlabel);
static const char *getAttrName(int attrnum, TableInfo *tblInfo);
static const char *fmtCopyColumnList(const TableInfo *ti);
static void do_sql_command(PGconn *conn, const char *query);

/* START MPP ADDITION */
static char *nextToken(register char **stringp, register const char *delim);
static void addDistributedBy(PQExpBuffer q, TableInfo *tbinfo, int actual_atts);
static bool isGPDB4300OrLater(void);
static bool isGPDB(void);
static bool isGPDB5000OrLater(void);

/* END MPP ADDITION */

/*
 * If GPDB version is 4.3, pg_proc has prodataaccess column.
 */
static bool
isGPDB4300OrLater(void)
{
	static int	value = -1;		/* -1 = not known yet, 0 = no, 1 = yes */

	/* Query the server on first call, and cache the result */
	if (value == -1)
	{
		if (isGPbackend)
		{
			const char *query;
			PGresult   *res;

			query = "select attnum from pg_catalog.pg_attribute "
					"where attrelid = 'pg_catalog.pg_proc'::regclass and "
					"attname = 'prodataaccess'";

			res = PQexec(g_conn, query);
			check_sql_result(res, g_conn, query, PGRES_TUPLES_OK);

			if (PQntuples(res) == 1)
				value = 1;
			else
				value = 0;

			PQclear(res);
		}
		else
			value = 0;
	}

	return (value == 1) ? true : false;
}

/*
 * Check if we are talking to GPDB
 */
static bool
isGPDB(void)
{
	static int	value = -1;		/* -1 = not known yet, 0 = no, 1 = yes */

	/* Query the server on first call, and cache the result */
	if (value == -1)
	{
		const char *query = "select pg_catalog.version()";
		PGresult   *res;
		char	   *ver;

		res = PQexec(g_conn, query);
		check_sql_result(res, g_conn, query, PGRES_TUPLES_OK);

		ver = (PQgetvalue(res, 0, 0));
		if (strstr(ver, "Greenplum") != NULL)
			value = 1;
		else
			value = 0;

		PQclear(res);
	}
	return (value == 1) ? true : false;
}


static bool
isGPDB5000OrLater(void)
{
	if (!isGPDB())
		return false;		/* Not Greenplum at all. */

	/* GPDB 5 is based on PostgreSQL 8.3 */
	return g_fout->remoteVersion >= 80300;
}

int
main(int argc, char **argv)
{
	int			c;
	const char *filename = NULL;
	const char *format = "p";
	const char *dbname = NULL;
	const char *pghost = NULL;
	const char *pgport = NULL;
	const char *username = NULL;
	const char *dumpencoding = NULL;
	const char *std_strings;
	bool		oids = false;
	TableInfo  *tblinfo;
	int			numTables;
	DumpableObject **dobjs;
	int			numObjs;
	int			i;
	enum trivalue prompt_password = TRI_DEFAULT;
	int			compressLevel = -1;
	int			plainText = 0;
	int			outputClean = 0;
	int			outputCreateDB = 0;
	bool		outputBlobs = false;
	int			outputNoOwner = 0;
	char	   *outputSuperuser = NULL;

	/*
	 * The default value for gp_syntax_option depends upon whether or not the
	 * backend is a GP or non-GP backend -- a GP backend defaults to ENABLED.
	 */
	static enum
	{
		GPS_NOT_SPECIFIED, GPS_DISABLED, GPS_ENABLED
	}			gp_syntax_option = GPS_NOT_SPECIFIED;

	RestoreOptions *ropt;

	static int	disable_triggers = 0;
	/* static int	outputNoTablespaces = 0; */
	static int	use_setsessauth = 0;

	struct option long_options[] = {
		{"binary-upgrade", no_argument, &binary_upgrade, 1},	/* not documented */
		{"data-only", no_argument, NULL, 'a'},
		{"blobs", no_argument, NULL, 'b'},
		{"clean", no_argument, NULL, 'c'},
		{"create", no_argument, NULL, 'C'},
		{"file", required_argument, NULL, 'f'},
		{"format", required_argument, NULL, 'F'},
		{"host", required_argument, NULL, 'h'},
		{"ignore-version", no_argument, NULL, 'i'},
		{"no-reconnect", no_argument, NULL, 'R'},
		{"oids", no_argument, NULL, 'o'},
		{"no-owner", no_argument, NULL, 'O'},
		{"port", required_argument, NULL, 'p'},
		{"schema", required_argument, NULL, 'n'},
		{"exclude-schema", required_argument, NULL, 'N'},
		{"schema-only", no_argument, NULL, 's'},
		{"superuser", required_argument, NULL, 'S'},
		{"table", required_argument, NULL, 't'},
		{"exclude-table", required_argument, NULL, 'T'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"username", required_argument, NULL, 'U'},
		{"verbose", no_argument, NULL, 'v'},
		{"no-privileges", no_argument, NULL, 'x'},
		{"no-acl", no_argument, NULL, 'x'},
		{"compress", required_argument, NULL, 'Z'},
		{"encoding", required_argument, NULL, 'E'},
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},

		/*
		 * the following options don't have an equivalent short option letter
		 */
		{"attribute-inserts", no_argument, &column_inserts, 1},
		{"column-inserts", no_argument, &column_inserts, 1},
		{"disable-dollar-quoting", no_argument, &disable_dollar_quoting, 1},
		{"disable-triggers", no_argument, &disable_triggers, 1},
		{"inserts", no_argument, &dump_inserts, 1},
		{"use-set-session-authorization", no_argument, &use_setsessauth, 1},

		/* START MPP ADDITION */

		/*
		 * the following are mpp specific, and don't have an equivalent short
		 * option
		 */
		{"gp-syntax", no_argument, NULL, 1},
		{"no-gp-syntax", no_argument, NULL, 2},
		{"pre-data-schema-only", no_argument, &preDataSchemaOnly, 1},
		{"post-data-schema-only", no_argument, &postDataSchemaOnly, 1},
		{"function-oids", required_argument, NULL, 3},
		{"relation-oids", required_argument, NULL, 4},
		/* END MPP ADDITION */
		{NULL, 0, NULL, 0}
	};
	int			optindex;

	set_pglocale_pgservice(argv[0], "pg_dump");

	g_verbose = false;

	strcpy(g_comment_start, "-- ");
	g_comment_end[0] = '\0';
	strcpy(g_opaque_type, "opaque");

	dataOnly = schemaOnly = dump_inserts = column_inserts = false;
	preDataSchemaOnly = postDataSchemaOnly = false;

	progname = get_progname(argv[0]);

	/* Set default options based on progname */
	if (strcmp(progname, "pg_backup") == 0)
		format = "c";

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_dump (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "abcCdDE:f:F:h:in:N:oOp:RsS:t:T:uU:vwWxX:Z:",
							long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'a':			/* Dump data only */
				dataOnly = true;
				break;

			case 'b':			/* Dump blobs */
				outputBlobs = true;
				break;

			case 'c':			/* clean (i.e., drop) schema prior to create */
				outputClean = 1;
				break;

			case 'C':			/* Create DB */
				outputCreateDB = 1;
				break;

			case 'd':			/* dump data as proper insert strings */
				dump_inserts = true;
				fprintf(stderr," --inserts is preferred over -d.  -d is deprecated.\n");
				break;

			case 'D':			/* dump data as proper insert strings with
								 * attr names */
				dump_inserts = true;
				column_inserts = true;
				fprintf(stderr," --column-inserts is preferred over -D.  -D is deprecated.\n");
				break;

			case 'E':			/* Dump encoding */
				dumpencoding = optarg;
				break;

			case 'f':
				filename = optarg;
				break;

			case 'F':
				format = optarg;
				break;

			case 'h':			/* server host */
				pghost = optarg;
				break;

			case 'i':
				/* ignored, deprecated option */
				break;

			case 'n':			/* include schema(s) */
				simple_string_list_append(&schema_include_patterns, optarg);
				include_everything = false;
				break;

			case 'N':			/* exclude schema(s) */
				simple_string_list_append(&schema_exclude_patterns, optarg);
				break;

			case 'o':			/* Dump oids */
				oids = true;
				break;

			case 'O':			/* Don't reconnect to match owner */
				outputNoOwner = 1;
				break;

			case 'p':			/* server port */
				pgport = optarg;
				break;

			case 'R':
				/* no-op, still accepted for backwards compatibility */
				break;

			case 's':			/* dump schema only */
				schemaOnly = true;
				break;

			case 'S':			/* Username for superuser in plain text output */
				outputSuperuser = strdup(optarg);
				break;

			case 't':			/* include table(s) */
				simple_string_list_append(&table_include_patterns, optarg);
				include_everything = false;
				break;

			case 'T':			/* exclude table(s) */
				simple_string_list_append(&table_exclude_patterns, optarg);
				break;

			case 'u':
				prompt_password = TRI_YES;
				username = simple_prompt("User name: ", 100, true);
				break;

			case 'U':
				username = optarg;
				break;

			case 'v':			/* verbose */
				g_verbose = true;
				break;

			case 'w':
				prompt_password = TRI_NO;
				break;

			case 'W':
				prompt_password = TRI_YES;
				break;

			case 'x':			/* skip ACL dump */
				aclsSkip = true;
				break;

			case 'X':
				/* -X is a deprecated alternative to long options */
				if (strcmp(optarg, "disable-dollar-quoting") == 0)
					disable_dollar_quoting = 1;
				else if (strcmp(optarg, "disable-triggers") == 0)
					disable_triggers = 1;
				else if (strcmp(optarg, "use-set-session-authorization") == 0)
					use_setsessauth = 1;
				else
				{
					fprintf(stderr,
							_("%s: invalid -X option -- %s\n"),
							progname, optarg);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				break;

			case 'Z':			/* Compression Level */
				compressLevel = atoi(optarg);
				break;

			case 0:
				/* This covers the long options equivalent to -X xxx. */
				break;

			case 1:				/* gp-syntax */
				if (gp_syntax_option != GPS_NOT_SPECIFIED)
				{
					write_msg(NULL, "options \"--gp-syntax\" and \"--no-gp-syntax\" cannot be used together\n");
					exit(1);
				}
				gp_syntax_option = GPS_ENABLED;
				break;

			case 2:				/* no-gp-syntax */
				if (gp_syntax_option != GPS_NOT_SPECIFIED)
				{
					write_msg(NULL, "options \"--gp-syntax\" and \"--no-gp-syntax\" cannot be used together\n");
					exit(1);
				}
				gp_syntax_option = GPS_DISABLED;
				break;

			case 3:
				simple_string_list_append(&funcid_string_list, optarg);
				include_everything = false;
				break;

			case 4:
				simple_string_list_append(&relid_string_list, optarg);
				include_everything = false;
				break;

			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	/* Get database name from command line */
	if (optind < argc)
		dbname = argv[optind++];

	/* Complain if any arguments remain */
	if (optind < argc)
	{
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/* --column-inserts implies --inserts */
	if (column_inserts)
		dump_inserts = 1;

	/* --pre-data-schema-only or --post-data-schema-only implies --schema-only */
	if (preDataSchemaOnly || postDataSchemaOnly)
		schemaOnly = true;

	if (dataOnly && schemaOnly)
	{
		write_msg(NULL, "options -s/--schema-only and -a/--data-only cannot be used together\n");
		exit(1);
	}

	if (dataOnly && outputClean)
	{
		write_msg(NULL, "options -c/--clean and -a/--data-only cannot be used together\n");
		exit(1);
	}

	if (dump_inserts && oids)
	{
		write_msg(NULL, "options --inserts/--column-inserts (-d, -D) and OID (-o, --oids) options cannot be used together\n");
		write_msg(NULL, "(The INSERT command cannot set OIDs.)\n");
		exit(1);
	}

	/* open the output file */
	if (pg_strcasecmp(format, "a") == 0 || pg_strcasecmp(format, "append") == 0)
	{
		/* This is used by pg_dumpall, and is not documented */
		plainText = 1;
		g_fout = CreateArchive(filename, archNull, 0, archModeAppend);
	}
	else if (pg_strcasecmp(format, "c") == 0 || pg_strcasecmp(format, "custom") == 0)
		g_fout = CreateArchive(filename, archCustom, compressLevel, archModeWrite);
	else if (pg_strcasecmp(format, "f") == 0 || pg_strcasecmp(format, "file") == 0)
	{
		/*
		 * Dump files into the current directory; for demonstration only, not
		 * documented.
		 */
		g_fout = CreateArchive(filename, archFiles, compressLevel, archModeWrite);
	}
	else if (pg_strcasecmp(format, "p") == 0 || pg_strcasecmp(format, "plain") == 0)
	{
		plainText = 1;
		g_fout = CreateArchive(filename, archNull, 0, archModeWrite);
	}
	else if (pg_strcasecmp(format, "t") == 0 || pg_strcasecmp(format, "tar") == 0)
		g_fout = CreateArchive(filename, archTar, compressLevel, archModeWrite);
	else
	{
		write_msg(NULL, "invalid output format \"%s\" specified\n", format);
		exit(1);
	}

	if (g_fout == NULL)
	{
		write_msg(NULL, "could not open output file \"%s\" for writing\n", filename);
		exit(1);
	}

	/* Let the archiver know how noisy to be */
	g_fout->verbose = g_verbose;
	g_fout->minRemoteVersion = 80200;	/* we can handle back to 8.2 */
	g_fout->maxRemoteVersion = parse_version(PG_VERSION);
	if (g_fout->maxRemoteVersion < 0)
	{
		write_msg(NULL, "could not parse version string \"%s\"\n", PG_VERSION);
		exit(1);
	}

	/*
	 * Open the database using the Archiver, so it knows about it. Errors mean
	 * death.
	 */
	g_conn = ConnectDatabase(g_fout, dbname, pghost, pgport,
							 username, prompt_password, binary_upgrade);

	/* Set the client encoding if requested */
	if (dumpencoding)
	{
		if (PQsetClientEncoding(g_conn, dumpencoding) < 0)
		{
			write_msg(NULL, "invalid client encoding \"%s\" specified\n",
					  dumpencoding);
			exit(1);
		}
	}

	/*
	 * Get the active encoding and the standard_conforming_strings setting, so
	 * we know how to escape strings.
	 */
	g_fout->encoding = PQclientEncoding(g_conn);

	std_strings = PQparameterStatus(g_conn, "standard_conforming_strings");
	g_fout->std_strings = (std_strings && strcmp(std_strings, "on") == 0);

	/* Set the datestyle to ISO to ensure the dump's portability */
	do_sql_command(g_conn, "SET DATESTYLE = ISO");

	/*
	 * Determine whether or not we're interacting with a GP backend.
	 */
	isGPbackend = testGPbackend();

	/*
	 * Now that the type of backend is known, determine the gp-syntax option
	 * value and set processing accordingly.
	 */
	switch (gp_syntax_option)
	{
		case GPS_NOT_SPECIFIED:
			dumpPolicy = isGPbackend;
			break;
		case GPS_DISABLED:
			dumpPolicy = false;
			break;
		case GPS_ENABLED:
			dumpPolicy = isGPbackend;
			if (!isGPbackend)
			{
				write_msg(NULL, "Server is not a Greenplum Database instance; --gp-syntax option ignored.\n");
			}
			break;
	}

	/*
	 * If supported, set extra_float_digits so that we can dump float data
	 * exactly (given correctly implemented float I/O code, anyway)
	 */
	if (g_fout->remoteVersion >= 80500)
		do_sql_command(g_conn, "SET extra_float_digits TO 3");
	else if (g_fout->remoteVersion >= 70400)
		do_sql_command(g_conn, "SET extra_float_digits TO 2");

	/*
	 * If synchronized scanning is supported, disable it, to prevent
	 * unpredictable changes in row ordering across a dump and reload.
	 */
	if (g_fout->remoteVersion >= 80300)
		do_sql_command(g_conn, "SET synchronize_seqscans TO off");

	/*
	 * The default for enable_nestloop is off in GPDB. However, many of the queries
	 * that we issue best run with nested loop joins, so enable it.
	 */
	do_sql_command(g_conn, "SET enable_nestloop TO on");

	/*
	 * Start serializable transaction to dump consistent data.
	 */
	do_sql_command(g_conn, "BEGIN");

	do_sql_command(g_conn, "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE");

	/* Select the appropriate subquery to convert user IDs to names */
	username_subquery = "SELECT rolname FROM pg_catalog.pg_roles WHERE oid =";

	/*
	 * Remember whether or not this GP database supports partitioning.
	 */
	gp_partitioning_available = testPartitioningSupport();

	/*
	 * Remember whether or not this GP database supports column encoding.
	 */
	gp_attribute_encoding_available = testAttributeEncodingSupport();

	/* Expand schema selection patterns into OID lists */
	if (schema_include_patterns.head != NULL)
	{
		expand_schema_name_patterns(&schema_include_patterns,
									&schema_include_oids);
		if (schema_include_oids.head == NULL)
		{
			write_msg(NULL, "No matching schemas were found\n");
			exit_nicely();
		}
	}
	expand_schema_name_patterns(&schema_exclude_patterns,
								&schema_exclude_oids);
	/* non-matching exclusion patterns aren't an error */

	/* Expand table selection patterns into OID lists */
	if (table_include_patterns.head != NULL)
	{
		expand_table_name_patterns(&table_include_patterns,
								   &table_include_oids);
		if (table_include_oids.head == NULL)
		{
			write_msg(NULL, "No matching tables were found\n");
			exit_nicely();
		}
	}
	expand_table_name_patterns(&table_exclude_patterns,
							   &table_exclude_oids);
	/* non-matching exclusion patterns aren't an error */


	expand_oid_patterns(&relid_string_list, &table_include_oids);
	expand_oid_patterns(&funcid_string_list, &function_include_oids);

	/*
	 * Dumping blobs is now default unless we saw an inclusion switch or -s
	 * ... but even if we did see one of these, -b turns it back on.
	 */
	if (include_everything && !schemaOnly)
		outputBlobs = true;

	/*
	 * Now scan the database and create DumpableObject structs for all the
	 * objects we intend to dump.
	 */
	tblinfo = getSchemaData(&numTables, 1);

	if (!schemaOnly)
		getTableData(tblinfo, numTables, oids);

	if (outputBlobs && hasBlobs(g_fout))
	{
		/* Add placeholders to allow correct sorting of blobs */
		DumpableObject *blobobj;

		blobobj = (DumpableObject *) malloc(sizeof(DumpableObject));
		blobobj->objType = DO_BLOBS;
		blobobj->catId = nilCatalogId;
		AssignDumpId(blobobj);
		blobobj->name = strdup("BLOBS");

		blobobj = (DumpableObject *) malloc(sizeof(DumpableObject));
		blobobj->objType = DO_BLOB_COMMENTS;
		blobobj->catId = nilCatalogId;
		AssignDumpId(blobobj);
		blobobj->name = strdup("BLOB COMMENTS");
	}

	/*
	 * Collect dependency data to assist in ordering the objects.
	 */
	getDependencies();

	setExtPartDependency(tblinfo, numTables);

	/*
	 * Sort the objects into a safe dump order (no forward references).
	 *
	 * In 7.3 or later, we can rely on dependency information to help us
	 * determine a safe order, so the initial sort is mostly for cosmetic
	 * purposes: we sort by name to ensure that logically identical schemas
	 * will dump identically.
	 */
	getDumpableObjects(&dobjs, &numObjs);

	sortDumpableObjectsByTypeName(dobjs, numObjs);

	sortDumpableObjects(dobjs, numObjs);

	/*
	 * Create archive TOC entries for all the objects to be dumped, in a safe
	 * order.
	 */

	/* First the special ENCODING and STDSTRINGS entries. */
	dumpEncoding(g_fout);
	dumpStdStrings(g_fout);

	/* The database item is always next, unless we don't want it at all */
	if (include_everything && !dataOnly)
		dumpDatabase(g_fout);

	/* Now the rearrangeable objects. */
	for (i = 0; i < numObjs; i++)
		dumpDumpableObject(g_fout, dobjs[i]);

	/*
	 * And finally we can do the actual output.
	 */
	if (plainText)
	{
		ropt = NewRestoreOptions();
		ropt->filename = (char *) filename;
		ropt->dropSchema = outputClean;
		ropt->aclsSkip = aclsSkip;
		ropt->superuser = outputSuperuser;
		ropt->createDB = outputCreateDB;
		ropt->noOwner = outputNoOwner;
		ropt->disable_triggers = disable_triggers;
		ropt->use_setsessauth = use_setsessauth;
		ropt->dataOnly = dataOnly;

		if (compressLevel == -1)
			ropt->compression = 0;
		else
			ropt->compression = compressLevel;

		ropt->suppressDumpWarnings = true;		/* We've already shown them */

		RestoreArchive(g_fout, ropt);
	}

	CloseArchive(g_fout);

	PQfinish(g_conn);

	exit(0);
}


static void
help(const char *progname)
{
	printf(_("%s dumps a database as a text file or to other formats.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME]\n"), progname);

	printf(_("\nGeneral options:\n"));
	printf(_("  -f, --file=FILENAME      output file name\n"));
	printf(_("  -F, --format=c|t|p       output file format (custom, tar, plain text)\n"));
	printf(_("  -i, --ignore-version     proceed even when server version mismatches\n"
			 "                           pg_dump version\n"));
	printf(_("  -v, --verbose            verbose mode\n"));
	printf(_("  -Z, --compress=0-9       compression level for compressed formats\n"));
	printf(_("  --help                   show this help, then exit\n"));
	printf(_("  --version                output version information, then exit\n"));

	printf(_("\nOptions controlling the output content:\n"));
	printf(_("  -a, --data-only             dump only the data, not the schema\n"));
	printf(_("  -b, --blobs                 include large objects in dump\n"));
	printf(_("  -c, --clean                 clean (drop) schema prior to create\n"));
	printf(_("  -C, --create                include commands to create database in dump\n"));
	printf(_("  -d, --inserts            dump data as INSERT, rather than COPY, commands\n"));
	printf(_("  -D, --column-inserts     dump data as INSERT commands with column names\n"));
	printf(_("  -E, --encoding=ENCODING     dump the data in encoding ENCODING\n"));
	printf(_("  -n, --schema=SCHEMA         dump the named schema(s) only\n"));
	printf(_("  -N, --exclude-schema=SCHEMA do NOT dump the named schema(s)\n"));
	printf(_("  -o, --oids                  include OIDs in dump\n"));
	printf(_("  -O, --no-owner              skip restoration of object ownership\n"
			 "                              in plain text format\n"));
	printf(_("  -s, --schema-only           dump only the schema, no data\n"));
	printf(_("  -S, --superuser=NAME        specify the superuser user name to use in\n"
			 "                              plain text format\n"));
	printf(_("  -t, --table=TABLE           dump only matching table(s) (or views or sequences)\n"));
	printf(_("  -T, --exclude-table=TABLE   do NOT dump matching table(s) (or views or sequences)\n"));
	printf(_("  -x, --no-privileges         do not dump privileges (grant/revoke)\n"));
	printf(_("  --disable-dollar-quoting    disable dollar quoting, use SQL standard quoting\n"));
	printf(_("  --disable-triggers          disable triggers during data-only restore\n"));
	printf(_("  --use-set-session-authorization\n"
			 "                              use SESSION AUTHORIZATION commands instead of\n"
	"                              ALTER OWNER commands to set ownership\n"));
	/* START MPP ADDITION */
	printf(_("  --gp-syntax                 dump with Greenplum Database syntax (default if gpdb)\n"));
	printf(_("  --no-gp-syntax              dump without Greenplum Database syntax (default if postgresql)\n"));
	printf(_("  --function-oids             dump only function(s) of given list of oids\n"));
	printf(_("  --relation-oids             dump only relation(s) of given list of oids\n"));
	/* END MPP ADDITION */

	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));

	printf(_("\nIf no database name is supplied, then the PGDATABASE environment\n"
			 "variable value is used.\n\n"));
	printf(_("Report bugs to <bugs@greenplum.org>.\n"));
}

void
exit_nicely(void)
{
	PQfinish(g_conn);
	if (g_verbose)
		write_msg(NULL, "*** aborted because of error\n");
	exit(1);
}

/*
 * Find the OIDs of all schemas matching the given list of patterns,
 * and append them to the given OID list.
 */
static void
expand_schema_name_patterns(SimpleStringList *patterns, SimpleOidList *oids)
{
	PQExpBuffer query;
	PGresult   *res;
	SimpleStringListCell *cell;
	int			i;

	if (patterns->head == NULL)
		return;					/* nothing to do */

	query = createPQExpBuffer();

	/*
	 * We use UNION ALL rather than UNION; this might sometimes result in
	 * duplicate entries in the OID list, but we don't care.
	 */

	for (cell = patterns->head; cell; cell = cell->next)
	{
		if (cell != patterns->head)
			appendPQExpBuffer(query, "UNION ALL\n");
		appendPQExpBuffer(query,
						  "SELECT oid FROM pg_catalog.pg_namespace n\n");
		processSQLNamePattern(g_conn, query, cell->val, false, false,
							  NULL, "n.nspname", NULL,
							  NULL);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	for (i = 0; i < PQntuples(res); i++)
	{
		simple_oid_list_append(oids, atooid(PQgetvalue(res, i, 0)));
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * Find the OIDs of all tables matching the given list of patterns,
 * and append them to the given OID list.
 */
static void
expand_table_name_patterns(SimpleStringList *patterns, SimpleOidList *oids)
{
	PQExpBuffer query;
	PGresult   *res;
	SimpleStringListCell *cell;
	int			i;

	if (patterns->head == NULL)
		return;					/* nothing to do */

	query = createPQExpBuffer();

	/*
	 * We use UNION ALL rather than UNION; this might sometimes result in
	 * duplicate entries in the OID list, but we don't care.
	 */

	for (cell = patterns->head; cell; cell = cell->next)
	{
		if (cell != patterns->head)
			appendPQExpBuffer(query, "UNION ALL\n");
		appendPQExpBuffer(query,
						  "SELECT c.oid"
						  "\nFROM pg_catalog.pg_class c"
		"\n     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace"
						  "\nWHERE c.relkind in ('%c', '%c', '%c')\n",
						  RELKIND_RELATION, RELKIND_SEQUENCE, RELKIND_VIEW);
		processSQLNamePattern(g_conn, query, cell->val, true, false,
							  "n.nspname", "c.relname", NULL,
							  "pg_catalog.pg_table_is_visible(c.oid)");
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	for (i = 0; i < PQntuples(res); i++)
	{
		simple_oid_list_append(oids, atooid(PQgetvalue(res, i, 0)));
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * checkExtensionMembership
 *		Determine whether object is an extension member, and if so,
 *		record an appropriate dependency and set the object's dump flag.
 *
 * It's important to call this for each object that could be an extension
 * member.  Generally, we integrate this with determining the object's
 * to-be-dumped-ness, since extension membership overrides other rules for that.
 *
 * Returns true if object is an extension member, else false.
 */
static bool
checkExtensionMembership(DumpableObject *dobj)
{
	ExtensionInfo *ext = findOwningExtension(dobj->catId);

	if (ext == NULL)
		return false;

	dobj->ext_member = true;

	/* Record dependency so that getDependencies needn't deal with that */
	addObjectDependency(dobj, ext->dobj.dumpId);

	/*
	 * Normally, mark the member object as not to be dumped.  But in binary
	 * upgrades, we still dump the members individually, since the idea is to
	 * exactly reproduce the database contents rather than replace the
	 * extension contents with something different.
	 */
	if (!binary_upgrade)
		dobj->dump = false;
	else
		dobj->dump = ext->dobj.dump;

	return true;
}

/*
 * Parse the OIDs matching the given list of patterns separated by non-digit
 * characters, and append them to the given OID list.
 */
static void
expand_oid_patterns(SimpleStringList *patterns, SimpleOidList *oids)
{
	SimpleStringListCell *cell;

	if (patterns->head == NULL)
		return;					/* nothing to do */

	for (cell = patterns->head; cell; cell = cell->next)
	{
		const char *seperator = ",";
		char *oidstr = strdup(cell->val);
		if (oidstr == NULL)
		{
			write_msg(NULL, "memory allocation failed for function \"expand_oid_patterns\"\n");
			exit_nicely();
		}

		char *token = strtok(oidstr, seperator);
		while (token)
		{
			if (strstr(seperator, token) == NULL)
				simple_oid_list_append(oids, atooid(token));

			token = strtok(NULL, seperator);
		}

		free(oidstr);
	}
}

/*
 * selectDumpableNamespace: policy-setting subroutine
 *		Mark a namespace as to be dumped or not
 *
 * Normally, we dump all extensions, or none of them if include_everything
 * is false (i.e., a --schema or --table switch was given).  However, in
 * binary-upgrade mode it's necessary to skip built-in extensions, since we
 * assume those will already be installed in the target database.  We identify
 * such extensions by their having OIDs in the range reserved for initdb.
 */
static void
selectDumpableNamespace(NamespaceInfo *nsinfo)
{
	if (checkExtensionMembership(&nsinfo->dobj))
		return;					/* extension membership overrides all else */

	/*
	 * If specific tables are being dumped, do not dump any complete
	 * namespaces. If specific namespaces are being dumped, dump just those
	 * namespaces. Otherwise, dump all non-system namespaces.
	 */
	if (table_include_oids.head != NULL)
		nsinfo->dobj.dump = false;
	else if (schema_include_oids.head != NULL)
		nsinfo->dobj.dump = simple_oid_list_member(&schema_include_oids,
												   nsinfo->dobj.catId.oid);
	else if (strncmp(nsinfo->dobj.name, "pg_", 3) == 0 ||
			 strcmp(nsinfo->dobj.name, "information_schema") == 0 ||
			 strcmp(nsinfo->dobj.name, "gp_toolkit") == 0)
		nsinfo->dobj.dump = false;
	else
		nsinfo->dobj.dump = true;

	/*
	 * In any case, a namespace can be excluded by an exclusion switch
	 */
	if (nsinfo->dobj.dump &&
		simple_oid_list_member(&schema_exclude_oids,
							   nsinfo->dobj.catId.oid))
		nsinfo->dobj.dump = false;
}

/*
 * selectDumpableTable: policy-setting subroutine
 *		Mark a table as to be dumped or not
 */
static void
selectDumpableTable(TableInfo *tbinfo)
{
	if (checkExtensionMembership(&tbinfo->dobj))
		return;					/* extension membership overrides all else */

	/*
	 * If specific tables are being dumped, dump just those tables; else, dump
	 * according to the parent namespace's dump flag.
	 */
	if (table_include_oids.head != NULL)
		tbinfo->dobj.dump = simple_oid_list_member(&table_include_oids,
												   tbinfo->dobj.catId.oid);
	else
		tbinfo->dobj.dump = tbinfo->dobj.namespace->dobj.dump;

	/*
	 * In any case, a table can be excluded by an exclusion switch
	 */
	if (tbinfo->dobj.dump &&
		simple_oid_list_member(&table_exclude_oids,
							   tbinfo->dobj.catId.oid))
		tbinfo->dobj.dump = false;
}

/*
 * selectDumpableType: policy-setting subroutine
 *		Mark a type as to be dumped or not
 *
 * If it's a table's rowtype or an autogenerated array type, we also apply a
 * special type code to facilitate sorting into the desired order.  (We don't
 * want to consider those to be ordinary types because that would bring tables
 * up into the datatype part of the dump order.)  We still set the object's
 * dump flag; that's not going to cause the dummy type to be dumped, but we
 * need it so that casts involving such types will be dumped correctly -- see
 * dumpCast.  This means the flag should be set the same as for the underlying
 * object (the table or base type).
 */
static void
selectDumpableType(TypeInfo *tyinfo)
{
	/* skip complex types, except for standalone composite types */
	if (OidIsValid(tyinfo->typrelid) &&
			tyinfo->typrelkind != RELKIND_COMPOSITE_TYPE)
	{
		TableInfo  *tytable = findTableByOid(tyinfo->typrelid);

		tyinfo->dobj.objType = DO_DUMMY_TYPE;
		if (tytable != NULL)
			tyinfo->dobj.dump = tytable->dobj.dump;
		else
			tyinfo->dobj.dump = false;
		return;
	}

	/* skip auto-generated array types */
	if (tyinfo->isArray)
	{
		tyinfo->dobj.objType = DO_DUMMY_TYPE;
		/*
		 * Fall through to set the dump flag; we assume that the subsequent
		 * rules will do the same thing as they would for the array's base
		 * type.  (We cannot reliably look up the base type here, since
		 * getTypes may not have processed it yet.)
		 */
	}

	if (checkExtensionMembership(&tyinfo->dobj))
		return;					/* extension membership overrides all else */

	/* dump only types in dumpable namespaces */
	if (!tyinfo->dobj.namespace->dobj.dump)
		tyinfo->dobj.dump = false;

	/* skip undefined placeholder types */
	else if (!tyinfo->isDefined)
		tyinfo->dobj.dump = false;

	/* skip auto-generated array types */
	else if (tyinfo->isArray)
		tyinfo->dobj.dump = false;

	else
		tyinfo->dobj.dump = true;
}


/*
 * selectDumpableCast: policy-setting subroutine
 *		Mark a cast as to be dumped or not
 *
 * Casts do not belong to any particular namespace (since they haven't got
 * names), nor do they have identifiable owners.  To distinguish user-defined
 * casts from built-in ones, we must resort to checking whether the cast's
 * OID is in the range reserved for initdb.
 */
static void
selectDumpableCast(CastInfo *cast)
{
	if (checkExtensionMembership(&cast->dobj))
		return;					/* extension membership overrides all else */

	if (cast->dobj.catId.oid < (Oid) FirstNormalObjectId)
		cast->dobj.dump = false;
	else
		cast->dobj.dump = include_everything;
}

/*
 * selectDumpableProcLang: policy-setting subroutine
 *		Mark a procedural language as to be dumped or not
 *
 * Procedural languages do not belong to any particular namespace.  To
 * identify built-in languages, we must resort to checking whether the
 * language's OID is in the range reserved for initdb.
 */
static void
selectDumpableProcLang(ProcLangInfo *plang)
{
	if (checkExtensionMembership(&plang->dobj))
		return;					/* extension membership overrides all else */

	if (plang->dobj.catId.oid < (Oid) FirstNormalObjectId)
		plang->dobj.dump = false;
	else
		plang->dobj.dump = include_everything;
}

/*
 * selectDumpableExtension: policy-setting subroutine
 *		Mark an extension as to be dumped or not
 *
 * Normally, we dump all extensions, or none of them if include_everything
 * is false (i.e., a --schema or --table switch was given).  However, in
 * binary-upgrade mode it's necessary to skip built-in extensions, since we
 * assume those will already be installed in the target database.  We identify
 * such extensions by their having OIDs in the range reserved for initdb.
 */
static void
selectDumpableExtension(ExtensionInfo *extinfo)
{
	if (binary_upgrade && extinfo->dobj.catId.oid < (Oid) FirstNormalObjectId)
		extinfo->dobj.dump = false;
	else
		extinfo->dobj.dump = include_everything;
}

/*
 * selectDumpableFunction: policy-setting subroutine
 *		Mark a function as to be dumped or not
 */
static void
selectDumpableFunction(FuncInfo *finfo)
{
	/*
	 * If specific functions are being dumped, dump just those functions; else, dump
	 * according to the parent namespace's dump flag if parent namespace is not null;
	 * else, always dump the function.
	 */
	if (function_include_oids.head != NULL)
		finfo->dobj.dump = simple_oid_list_member(&function_include_oids,
												   finfo->dobj.catId.oid);
	else if (finfo->dobj.namespace)
		finfo->dobj.dump = finfo->dobj.namespace->dobj.dump;
	else
		finfo->dobj.dump = true;
}

/*
 * selectDumpableObject: policy-setting subroutine
 *		Mark a generic dumpable object as to be dumped or not
 *
 * Use this only for object types without a special-case routine above.
 */
static void
selectDumpableObject(DumpableObject *dobj)
{
	if (checkExtensionMembership(dobj))
		return;					/* extension membership overrides all else */

	/*
	 * Default policy is to dump if parent namespace is dumpable, or always
	 * for non-namespace-associated items.
	 */
	if (dobj->namespace)
		dobj->dump = dobj->namespace->dobj.dump;
	else
		dobj->dump = include_everything;
}

/*
 *	Dump a table's contents for loading using the COPY command
 *	- this routine is called by the Archiver when it wants the table
 *	  to be dumped.
 */

static int
dumpTableData_copy(Archive *fout, void *dcontext)
{
	TableDataInfo *tdinfo = (TableDataInfo *) dcontext;
	TableInfo  *tbinfo = tdinfo->tdtable;
	const char *classname = tbinfo->dobj.name;
	const bool	hasoids = tbinfo->hasoids;
	const bool	oids = tdinfo->oids;
	PQExpBuffer q = createPQExpBuffer();
	PGresult   *res;
	int			ret;
	char	   *copybuf;
	const char *column_list;

	if (g_verbose)
		write_msg(NULL, "dumping contents of table %s\n", classname);

	/*
	 * Make sure we are in proper schema.  We will qualify the table name
	 * below anyway (in case its name conflicts with a pg_catalog table); but
	 * this ensures reproducible results in case the table contains regproc,
	 * regclass, etc columns.
	 */
	selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

	/*
	 * If possible, specify the column list explicitly so that we have no
	 * possibility of retrieving data in the wrong column order.  (The default
	 * column ordering of COPY will not be what we want in certain corner
	 * cases involving ADD COLUMN and inheritance.)
	 */
	column_list = fmtCopyColumnList(tbinfo);

	if (oids && hasoids)
	{
		appendPQExpBuffer(q, "COPY %s %s WITH OIDS TO stdout IGNORE EXTERNAL PARTITIONS;",
						  fmtQualifiedId(tbinfo->dobj.namespace->dobj.name,
										 classname),
						  column_list);
	}
	else
	{
		appendPQExpBuffer(q, "COPY %s %s TO stdout IGNORE EXTERNAL PARTITIONS;",
						  fmtQualifiedId(tbinfo->dobj.namespace->dobj.name,
										 classname),
						  column_list);
	}
	res = PQexec(g_conn, q->data);
	check_sql_result(res, g_conn, q->data, PGRES_COPY_OUT);
	PQclear(res);

	for (;;)
	{
		ret = PQgetCopyData(g_conn, &copybuf, 0);

		if (ret < 0)
			break;				/* done or error */

		if (copybuf)
		{
			WriteData(fout, copybuf, ret);
			PQfreemem(copybuf);
		}

		/* ----------
		 * THROTTLE:
		 *
		 * There was considerable discussion in late July, 2000 regarding
		 * slowing down pg_dump when backing up large tables. Users with both
		 * slow & fast (multi-processor) machines experienced performance
		 * degradation when doing a backup.
		 *
		 * Initial attempts based on sleeping for a number of ms for each ms
		 * of work were deemed too complex, then a simple 'sleep in each loop'
		 * implementation was suggested. The latter failed because the loop
		 * was too tight. Finally, the following was implemented:
		 *
		 * If throttle is non-zero, then
		 *		See how long since the last sleep.
		 *		Work out how long to sleep (based on ratio).
		 *		If sleep is more than 100ms, then
		 *			sleep
		 *			reset timer
		 *		EndIf
		 * EndIf
		 *
		 * where the throttle value was the number of ms to sleep per ms of
		 * work. The calculation was done in each loop.
		 *
		 * Most of the hard work is done in the backend, and this solution
		 * still did not work particularly well: on slow machines, the ratio
		 * was 50:1, and on medium paced machines, 1:1, and on fast
		 * multi-processor machines, it had little or no effect, for reasons
		 * that were unclear.
		 *
		 * Further discussion ensued, and the proposal was dropped.
		 *
		 * For those people who want this feature, it can be implemented using
		 * gettimeofday in each loop, calculating the time since last sleep,
		 * multiplying that by the sleep ratio, then if the result is more
		 * than a preset 'minimum sleep time' (say 100ms), call the 'select'
		 * function to sleep for a subsecond period ie.
		 *
		 * select(0, NULL, NULL, NULL, &tvi);
		 *
		 * This will return after the interval specified in the structure tvi.
		 * Finally, call gettimeofday again to save the 'last sleep time'.
		 * ----------
		 */
	}
	archprintf(fout, "\\.\n\n\n");

	if (ret == -2)
	{
		/* copy data transfer failed */
		write_msg(NULL, "Dumping the contents of table \"%s\" failed: PQgetCopyData() failed.\n", classname);
		write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
		write_msg(NULL, "The command was: %s\n", q->data);
		exit_nicely();
	}

	/* Check command status and return to normal libpq state */
	res = PQgetResult(g_conn);
	check_sql_result(res, g_conn, q->data, PGRES_COMMAND_OK);
	PQclear(res);

	destroyPQExpBuffer(q);
	return 1;
}

/*
 * Dump table data using INSERT commands.
 *
 * Caution: when we restore from an archive file direct to database, the
 * INSERT commands emitted by this function have to be parsed by
 * pg_backup_db.c's ExecuteInsertCommands(), which will not handle comments,
 * E'' strings, or dollar-quoted strings.  So don't emit anything like that.
 */
static int
dumpTableData_insert(Archive *fout, void *dcontext)
{
	TableDataInfo *tdinfo = (TableDataInfo *) dcontext;
	TableInfo  *tbinfo = tdinfo->tdtable;
	const char *classname = tbinfo->dobj.name;
	PQExpBuffer q = createPQExpBuffer();
	PGresult   *res;
	int			tuple;
	int			nfields;
	int			field;

	/*
	 * Make sure we are in proper schema.  We will qualify the table name
	 * below anyway (in case its name conflicts with a pg_catalog table); but
	 * this ensures reproducible results in case the table contains regproc,
	 * regclass, etc columns.
	 */
	selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

	appendPQExpBuffer(q, "DECLARE _pg_dump_cursor CURSOR FOR "
					  "SELECT * FROM ONLY %s",
					  fmtQualifiedId(tbinfo->dobj.namespace->dobj.name,
									 classname));

	res = PQexec(g_conn, q->data);
	check_sql_result(res, g_conn, q->data, PGRES_COMMAND_OK);

	do
	{
		PQclear(res);

		res = PQexec(g_conn, "FETCH 100 FROM _pg_dump_cursor");
		check_sql_result(res, g_conn, "FETCH 100 FROM _pg_dump_cursor",
						 PGRES_TUPLES_OK);
		nfields = PQnfields(res);
		for (tuple = 0; tuple < PQntuples(res); tuple++)
		{
			archprintf(fout, "INSERT INTO %s ", fmtId(classname));
			if (nfields == 0)
			{
				/* corner case for zero-column table */
				archprintf(fout, "DEFAULT VALUES;\n");
				continue;
			}
			if (column_inserts)
			{
				resetPQExpBuffer(q);
				appendPQExpBuffer(q, "(");
				for (field = 0; field < nfields; field++)
				{
					if (field > 0)
						appendPQExpBuffer(q, ", ");
					appendPQExpBufferStr(q, fmtId(PQfname(res, field)));
				}
				appendPQExpBuffer(q, ") ");
				archputs(q->data, fout);
			}
			archprintf(fout, "VALUES (");
			for (field = 0; field < nfields; field++)
			{
				if (field > 0)
					archprintf(fout, ", ");
				if (PQgetisnull(res, tuple, field))
				{
					archprintf(fout, "NULL");
					continue;
				}

				/* XXX This code is partially duplicated in ruleutils.c */
				switch (PQftype(res, field))
				{
					case INT2OID:
					case INT4OID:
					case INT8OID:
					case OIDOID:
					case FLOAT4OID:
					case FLOAT8OID:
					case NUMERICOID:
						{
							/*
							 * These types are printed without quotes unless
							 * they contain values that aren't accepted by the
							 * scanner unquoted (e.g., 'NaN').	Note that
							 * strtod() and friends might accept NaN, so we
							 * can't use that to test.
							 *
							 * In reality we only need to defend against
							 * infinity and NaN, so we need not get too crazy
							 * about pattern matching here.
							 */
							const char *s = PQgetvalue(res, tuple, field);

							if (strspn(s, "0123456789 +-eE.") == strlen(s))
								archprintf(fout, "%s", s);
							else
								archprintf(fout, "'%s'", s);
						}
						break;

					case BITOID:
					case VARBITOID:
						archprintf(fout, "B'%s'",
								   PQgetvalue(res, tuple, field));
						break;

					case BOOLOID:
						if (strcmp(PQgetvalue(res, tuple, field), "t") == 0)
							archprintf(fout, "true");
						else
							archprintf(fout, "false");
						break;

					default:
						/* All other types are printed as string literals. */
						resetPQExpBuffer(q);
						appendStringLiteralAH(q,
											  PQgetvalue(res, tuple, field),
											  fout);
						archputs(q->data, fout);
						break;
				}
			}
			archprintf(fout, ");\n");
		}
	} while (PQntuples(res) > 0);

	PQclear(res);

	archprintf(fout, "\n\n");

	do_sql_command(g_conn, "CLOSE _pg_dump_cursor");

	destroyPQExpBuffer(q);
	return 1;
}


/*
 * dumpTableData -
 *	  dump the contents of a single table
 *
 * Actually, this just makes an ArchiveEntry for the table contents.
 */
static void
dumpTableData(Archive *fout, TableDataInfo *tdinfo)
{
	TableInfo  *tbinfo = tdinfo->tdtable;
	PQExpBuffer copyBuf = createPQExpBuffer();
	DataDumperPtr dumpFn;
	char	   *copyStmt;

	if (!dump_inserts)
	{
		/* Dump/restore using COPY */
		dumpFn = dumpTableData_copy;
		/* must use 2 steps here 'cause fmtId is nonreentrant */
		appendPQExpBuffer(copyBuf, "COPY %s ",
						  fmtId(tbinfo->dobj.name));
		appendPQExpBuffer(copyBuf, "%s %sFROM stdin;\n",
						  fmtCopyColumnList(tbinfo),
					  (tdinfo->oids && tbinfo->hasoids) ? "WITH OIDS " : "");
		copyStmt = copyBuf->data;
	}
	else
	{
		/* Restore using INSERT */
		dumpFn = dumpTableData_insert;
		copyStmt = NULL;
	}

	ArchiveEntry(fout, tdinfo->dobj.catId, tdinfo->dobj.dumpId,
				 tbinfo->dobj.name,
				 tbinfo->dobj.namespace->dobj.name,
				 NULL,
				 tbinfo->rolname, false,
				 "TABLE DATA", "", "", copyStmt,
				 tdinfo->dobj.dependencies, tdinfo->dobj.nDeps,
				 dumpFn, tdinfo);

	destroyPQExpBuffer(copyBuf);
}

/*
 * getTableData -
 *	  set up dumpable objects representing the contents of tables
 */
static void
getTableData(TableInfo *tblinfo, int numTables, bool oids)
{
	int			i;

	for (i = 0; i < numTables; i++)
	{
		if (tblinfo[i].dobj.dump)
			makeTableDataInfo(&(tblinfo[i]), oids);
	}
}

/*
 * Make a dumpable object for the data of this specific table
 *
 * Note: we make a TableDataInfo if and only if we are going to dump the
 * table data; the "dump" flag in such objects isn't used.
 */
static void
makeTableDataInfo(TableInfo *tbinfo, bool oids)
{
	TableDataInfo *tdinfo;

	/*
	 * Nothing to do if we already decided to dump the table.  This will
	 * happen for "config" tables.
	 */
	if (tbinfo->dataObj != NULL)
		return;

	/* Skip VIEWs (no data to dump) */
	if (tbinfo->relkind == RELKIND_VIEW)
		return;
	/* START MPP ADDITION */
	/* Skip EXTERNAL TABLEs */
	if (tbinfo->relstorage == RELSTORAGE_EXTERNAL)
		return;
	/* END MPP ADDITION */
	/* Skip SEQUENCEs (handled elsewhere) */
	if (tbinfo->relkind == RELKIND_SEQUENCE)
		return;

	/* OK, let's dump it */
	tdinfo = (TableDataInfo *) malloc(sizeof(TableDataInfo));

	tdinfo->dobj.objType = DO_TABLE_DATA;

	/*
	 * Note: use tableoid 0 so that this object won't be mistaken for
	 * something that pg_depend entries apply to.
	 */
	tdinfo->dobj.catId.tableoid = 0;
	tdinfo->dobj.catId.oid = tbinfo->dobj.catId.oid;
	AssignDumpId(&tdinfo->dobj);
	tdinfo->dobj.name = tbinfo->dobj.name;
	tdinfo->dobj.namespace = tbinfo->dobj.namespace;
	tdinfo->tdtable = tbinfo;
	tdinfo->oids = oids;
	tdinfo->filtercond = NULL;	/* might get set later */
	addObjectDependency(&tdinfo->dobj, tbinfo->dobj.dumpId);

	tbinfo->dataObj = tdinfo;
}

/*
 * dumpDatabase:
 *	dump the database definition
 */
static void
dumpDatabase(Archive *AH)
{
	PQExpBuffer dbQry = createPQExpBuffer();
	PQExpBuffer delQry = createPQExpBuffer();
	PQExpBuffer creaQry = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			i_tableoid,
				i_oid,
				i_dba,
				i_encoding,
				i_tablespace;
	CatalogId	dbCatId;
	DumpId		dbDumpId;
	const char *datname,
			   *dba,
			   *encoding,
			   *tablespace;
	char	   *comment;

	datname = PQdb(g_conn);

	if (g_verbose)
		write_msg(NULL, "saving database definition\n");

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* Get the database owner and parameters from pg_database */
	appendPQExpBuffer(dbQry, "SELECT tableoid, oid, "
					  "(%s datdba) as dba, "
					  "pg_encoding_to_char(encoding) as encoding, "
					  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = dattablespace) as tablespace, "
					  "shobj_description(oid, 'pg_database') as description "

					  "FROM pg_database "
					  "WHERE datname = ",
					  username_subquery);
	appendStringLiteralAH(dbQry, datname, AH);

	res = PQexec(g_conn, dbQry->data);
	check_sql_result(res, g_conn, dbQry->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	if (ntups <= 0)
	{
		write_msg(NULL, "missing pg_database entry for database \"%s\"\n",
				  datname);
		exit_nicely();
	}

	if (ntups != 1)
	{
		write_msg(NULL, "query returned more than one (%d) pg_database entry for database \"%s\"\n",
				  ntups, datname);
		exit_nicely();
	}

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_dba = PQfnumber(res, "dba");
	i_encoding = PQfnumber(res, "encoding");
	i_tablespace = PQfnumber(res, "tablespace");

	dbCatId.tableoid = atooid(PQgetvalue(res, 0, i_tableoid));
	dbCatId.oid = atooid(PQgetvalue(res, 0, i_oid));
	dba = PQgetvalue(res, 0, i_dba);
	encoding = PQgetvalue(res, 0, i_encoding);
	tablespace = PQgetvalue(res, 0, i_tablespace);

	appendPQExpBuffer(creaQry, "CREATE DATABASE %s WITH TEMPLATE = template0",
					  fmtId(datname));
	if (strlen(encoding) > 0)
	{
		appendPQExpBuffer(creaQry, " ENCODING = ");
		appendStringLiteralAH(creaQry, encoding, AH);
	}
	if (strlen(tablespace) > 0 && strcmp(tablespace, "pg_default") != 0)
		appendPQExpBuffer(creaQry, " TABLESPACE = %s",
						  fmtId(tablespace));
	appendPQExpBuffer(creaQry, ";\n");

	appendPQExpBuffer(delQry, "DROP DATABASE %s;\n",
					  fmtId(datname));

	dbDumpId = createDumpId();

	ArchiveEntry(AH,
				 dbCatId,		/* catalog ID */
				 dbDumpId,		/* dump ID */
				 datname,		/* Name */
				 NULL,			/* Namespace */
				 NULL,			/* Tablespace */
				 dba,			/* Owner */
				 false,			/* with oids */
				 "DATABASE",	/* Desc */
				 creaQry->data, /* Create */
				 delQry->data,	/* Del */
				 NULL,			/* Copy */
				 NULL,			/* Deps */
				 0,				/* # Deps */
				 NULL,			/* Dumper */
				 NULL);			/* Dumper Arg */

	/* Dump DB comment if any */
	comment = PQgetvalue(res, 0, PQfnumber(res, "description"));

	if (comment && strlen(comment))
	{
		resetPQExpBuffer(dbQry);
		/* Generates warning when loaded into a differently-named database.*/
		appendPQExpBuffer(dbQry, "COMMENT ON DATABASE %s IS ", fmtId(datname));
		appendStringLiteralAH(dbQry, comment, AH);
		appendPQExpBuffer(dbQry, ";\n");

		ArchiveEntry(AH, dbCatId, createDumpId(), datname, NULL, NULL,
					 dba, false, "COMMENT", dbQry->data, "", NULL,
					 &dbDumpId, 1, NULL, NULL);
	}

	PQclear(res);

	destroyPQExpBuffer(dbQry);
	destroyPQExpBuffer(delQry);
	destroyPQExpBuffer(creaQry);
}


/*
 * dumpEncoding: put the correct encoding into the archive
 */
static void
dumpEncoding(Archive *AH)
{
	const char *encname = pg_encoding_to_char(AH->encoding);
	PQExpBuffer qry = createPQExpBuffer();

	if (g_verbose)
		write_msg(NULL, "saving encoding = %s\n", encname);

	appendPQExpBuffer(qry, "SET client_encoding = ");
	appendStringLiteralAH(qry, encname, AH);
	appendPQExpBuffer(qry, ";\n");

	ArchiveEntry(AH, nilCatalogId, createDumpId(),
				 "ENCODING", NULL, NULL, "",
				 false, "ENCODING", qry->data, "", NULL,
				 NULL, 0,
				 NULL, NULL);

	destroyPQExpBuffer(qry);
}


/*
 * dumpStdStrings: put the correct escape string behavior into the archive
 */
static void
dumpStdStrings(Archive *AH)
{
	const char *stdstrings = AH->std_strings ? "on" : "off";
	PQExpBuffer qry = createPQExpBuffer();

	if (g_verbose)
		write_msg(NULL, "saving standard_conforming_strings = %s\n",
				  stdstrings);

	appendPQExpBuffer(qry, "SET standard_conforming_strings = '%s';\n",
					  stdstrings);

	ArchiveEntry(AH, nilCatalogId, createDumpId(),
				 "STDSTRINGS", NULL, NULL, "",
				 false, "STDSTRINGS", qry->data, "", NULL,
				 NULL, 0,
				 NULL, NULL);

	destroyPQExpBuffer(qry);
}


/*
 * hasBlobs:
 *	Test whether database contains any large objects
 */
static bool
hasBlobs(Archive *AH)
{
	bool		result;
	const char *blobQry;
	PGresult   *res;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* Check for BLOB OIDs */
	blobQry = "SELECT loid FROM pg_largeobject LIMIT 1";

	res = PQexec(g_conn, blobQry);
	check_sql_result(res, g_conn, blobQry, PGRES_TUPLES_OK);

	result = PQntuples(res) > 0;

	PQclear(res);

	return result;
}

/*
 * dumpBlobs:
 *	dump all blobs
 */
static int
dumpBlobs(Archive *AH, void *arg __attribute__((unused)))
{
	const char *blobQry;
	const char *blobFetchQry;
	PGresult   *res;
	char		buf[LOBBUFSIZE];
	int			i;
	int			cnt;

	if (g_verbose)
		write_msg(NULL, "saving large objects\n");

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* Cursor to get all BLOB OIDs */
	blobQry = "DECLARE bloboid CURSOR FOR SELECT DISTINCT loid FROM pg_largeobject";

	res = PQexec(g_conn, blobQry);
	check_sql_result(res, g_conn, blobQry, PGRES_COMMAND_OK);

	/* Command to fetch from cursor */
	blobFetchQry = "FETCH 1000 IN bloboid";

	do
	{
		PQclear(res);

		/* Do a fetch */
		res = PQexec(g_conn, blobFetchQry);
		check_sql_result(res, g_conn, blobFetchQry, PGRES_TUPLES_OK);

		/* Process the tuples, if any */
		for (i = 0; i < PQntuples(res); i++)
		{
			Oid			blobOid;
			int			loFd;

			blobOid = atooid(PQgetvalue(res, i, 0));
			/* Open the BLOB */
			loFd = lo_open(g_conn, blobOid, INV_READ);
			if (loFd == -1)
			{
				write_msg(NULL, "dumpBlobs(): could not open large object: %s",
						  PQerrorMessage(g_conn));
				exit_nicely();
			}

			StartBlob(AH, blobOid);

			/* Now read it in chunks, sending data to archive */
			do
			{
				cnt = lo_read(g_conn, loFd, buf, LOBBUFSIZE);
				if (cnt < 0)
				{
					write_msg(NULL, "dumpBlobs(): error reading large object: %s",
							  PQerrorMessage(g_conn));
					exit_nicely();
				}

				WriteData(AH, buf, cnt);
			} while (cnt > 0);

			lo_close(g_conn, loFd);

			EndBlob(AH, blobOid);
		}
	} while (PQntuples(res) > 0);

	PQclear(res);

	return 1;
}

/*
 * dumpBlobComments
 *	dump all blob comments
 *
 * Since we don't provide any way to be selective about dumping blobs,
 * there's no need to be selective about their comments either.  We put
 * all the comments into one big TOC entry.
 */
static int
dumpBlobComments(Archive *AH, void *arg __attribute__((unused)))
{
	const char *blobQry;
	const char *blobFetchQry;
	PQExpBuffer commentcmd = createPQExpBuffer();
	PGresult   *res;
	int			i;

	if (g_verbose)
		write_msg(NULL, "saving large object comments\n");

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* Cursor to get all BLOB comments */
	blobQry = "DECLARE blobcmt CURSOR FOR SELECT loid, "
			  "obj_description(loid, 'pg_largeobject') "
			  "FROM (SELECT DISTINCT loid FROM "
			  "pg_description d JOIN pg_largeobject l ON (objoid = loid) "
			  "WHERE classoid = 'pg_largeobject'::regclass) ss";

	res = PQexec(g_conn, blobQry);
	check_sql_result(res, g_conn, blobQry, PGRES_COMMAND_OK);

	/* Command to fetch from cursor */
	blobFetchQry = "FETCH 100 IN blobcmt";

	do
	{
		PQclear(res);

		/* Do a fetch */
		res = PQexec(g_conn, blobFetchQry);
		check_sql_result(res, g_conn, blobFetchQry, PGRES_TUPLES_OK);

		/* Process the tuples, if any */
		for (i = 0; i < PQntuples(res); i++)
		{
			Oid			blobOid;
			char	   *comment;

			/* ignore blobs without comments */
			if (PQgetisnull(res, i, 1))
				continue;

			blobOid = atooid(PQgetvalue(res, i, 0));
			comment = PQgetvalue(res, i, 1);

			printfPQExpBuffer(commentcmd, "COMMENT ON LARGE OBJECT %u IS ",
							  blobOid);
			appendStringLiteralAH(commentcmd, comment, AH);
			appendPQExpBuffer(commentcmd, ";\n");

			archputs(commentcmd->data, AH);
		}
	} while (PQntuples(res) > 0);

	PQclear(res);

	archputs("\n", AH);

	destroyPQExpBuffer(commentcmd);

	return 1;
}

/*
 * If the DumpableObject is a member of an extension, add a suitable
 * ALTER EXTENSION ADD command to the creation commands in upgrade_buffer.
 */
static void
binary_upgrade_extension_member(PQExpBuffer upgrade_buffer,
								DumpableObject *dobj,
								const char *objlabel)
{
	DumpableObject *extobj = NULL;
	int			i;

	if (!dobj->ext_member)
		return;

	/*
	 * Find the parent extension.  We could avoid this search if we wanted to
	 * add a link field to DumpableObject, but the space costs of that would
	 * be considerable.  We assume that member objects could only have a
	 * direct dependency on their own extension, not any others.
	 */
	for (i = 0; i < dobj->nDeps; i++)
	{
		extobj = findObjectByDumpId(dobj->dependencies[i]);
		if (extobj && extobj->objType == DO_EXTENSION)
			break;
		extobj = NULL;
	}
	if (extobj == NULL)
	{
		write_msg(NULL, "could not find parent extension for %s", objlabel);
		exit_nicely();
	}

	appendPQExpBuffer(upgrade_buffer,
	  "\n-- For binary upgrade, handle extension membership the hard way\n");
	appendPQExpBuffer(upgrade_buffer, "ALTER EXTENSION %s ADD %s;\n",
					  fmtId(extobj->name),
					  objlabel);
}

/*
 * getNamespaces:
 *	  read all namespaces in the system catalogs and return them in the
 * NamespaceInfo* structure
 *
 *	numNamespaces is set to the number of namespaces read in
 */
NamespaceInfo *
getNamespaces(int *numNamespaces)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	NamespaceInfo *nsinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_nspname;
	int			i_rolname;
	int			i_nspacl;

	query = createPQExpBuffer();

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/*
	 * we fetch all namespaces including system ones, so that every object we
	 * read in can be linked to a containing namespace.
	 */
	appendPQExpBuffer(query, "SELECT tableoid, oid, nspname, "
					  "(%s nspowner) as rolname, "
					  "nspacl FROM pg_namespace",
					  username_subquery);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	nsinfo = (NamespaceInfo *) malloc(ntups * sizeof(NamespaceInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_nspname = PQfnumber(res, "nspname");
	i_rolname = PQfnumber(res, "rolname");
	i_nspacl = PQfnumber(res, "nspacl");

	for (i = 0; i < ntups; i++)
	{
		nsinfo[i].dobj.objType = DO_NAMESPACE;
		nsinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		nsinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&nsinfo[i].dobj);
		nsinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_nspname));
		nsinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));
		nsinfo[i].nspacl = strdup(PQgetvalue(res, i, i_nspacl));

		/* Decide whether to dump this namespace */
		selectDumpableNamespace(&nsinfo[i]);

		if (strlen(nsinfo[i].rolname) == 0)
			write_msg(NULL, "WARNING: owner of schema \"%s\" appears to be invalid\n",
					  nsinfo[i].dobj.name);
	}

	PQclear(res);
	destroyPQExpBuffer(query);

	*numNamespaces = ntups;

	return nsinfo;
}

/*
 * findNamespace:
 *		given a namespace OID and an object OID, look up the info read by
 *		getNamespaces
 *
 * NB: for pre-7.3 source database, we use object OID to guess whether it's
 * a system object or not.	In 7.3 and later there is no guessing, and we
 * don't use objoid at all.
 */
static NamespaceInfo *
findNamespace(Oid nsoid, Oid objoid)
{
	NamespaceInfo *nsinfo;

	nsinfo = findNamespaceByOid(nsoid);

	if (nsinfo == NULL)
	{
		write_msg(NULL, "schema with OID %u does not exist\n", nsoid);
		exit_nicely();
	}

	return nsinfo;
}

/*
 * getExtensions:
 *	  read all extensions in the system catalogs and return them in the
 * ExtensionInfo* structure
 *
 *	numExtensions is set to the number of extensions read in
 */
ExtensionInfo *
getExtensions(int *numExtensions)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	ExtensionInfo *extinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_extname;
	int			i_nspname;
	int			i_extrelocatable;
	int			i_extversion;
	int			i_extconfig;
	int			i_extcondition;

	/*
	 * Before 8.3 (porting from PG 9.1), there are no extensions.
	 */
	if (g_fout->remoteVersion < 80300)
	{
		*numExtensions = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(query, "SELECT x.tableoid, x.oid, "
			"x.extname, n.nspname, x.extrelocatable, x.extversion, x.extconfig, x.extcondition "
			"FROM pg_extension x "
			"JOIN pg_namespace n ON n.oid = x.extnamespace");

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	extinfo = (ExtensionInfo *) malloc(ntups * sizeof(ExtensionInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_extname = PQfnumber(res, "extname");
	i_nspname = PQfnumber(res, "nspname");
	i_extrelocatable = PQfnumber(res, "extrelocatable");
	i_extversion = PQfnumber(res, "extversion");
	i_extconfig = PQfnumber(res, "extconfig");
	i_extcondition = PQfnumber(res, "extcondition");

	for (i = 0; i < ntups; i++)
	{
		extinfo[i].dobj.objType = DO_EXTENSION;
		extinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		extinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&extinfo[i].dobj);
		extinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_extname));
		extinfo[i].namespace = strdup(PQgetvalue(res, i, i_nspname));
		extinfo[i].relocatable = *(PQgetvalue(res, i, i_extrelocatable)) == 't';
		extinfo[i].extversion = strdup(PQgetvalue(res, i, i_extversion));
		extinfo[i].extconfig = strdup(PQgetvalue(res, i, i_extconfig));
		extinfo[i].extcondition = strdup(PQgetvalue(res, i, i_extcondition));

		/* Decide whether we want to dump it */
		selectDumpableExtension(&(extinfo[i]));
	}

	PQclear(res);
	destroyPQExpBuffer(query);

	*numExtensions = ntups;

	return extinfo;
}

/*
 * getTypes:
 *	  read all types in the system catalogs and return them in the
 * TypeInfo* structure
 *
 *	numTypes is set to the number of types read in
 *
 * NB: this must run after getFuncs() because we assume we can do
 * findFuncByOid().
 */
TypeInfo *
getTypes(int *numTypes)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	TypeInfo   *tinfo;
	ShellTypeInfo *stinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_typname;
	int			i_typnamespace;
	int			i_rolname;
	int			i_typinput;
	int			i_typoutput;
	int			i_typelem;
	int			i_typrelid;
	int			i_typrelkind;
	int			i_typtype;
	int			i_typisdefined;
	int			i_isarray;

	/*
	 * we include even the built-in types because those may be used as array
	 * elements by user-defined types
	 *
	 * we filter out the built-in types when we dump out the types
	 *
	 * same approach for undefined (shell) types and array types
	 *
	 * Note: as of 8.3 we can reliably detect whether a type is an
	 * auto-generated array type by checking the element type's typarray.
	 * (Before that the test is capable of generating false positives.) We
	 * still check for name beginning with '_', though, so as to avoid the
	 * cost of the subselect probe for all standard types.	This would have to
	 * be revisited if the backend ever allows renaming of array types.
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	if (g_fout->remoteVersion >= 80300)
	{
		appendPQExpBuffer(query, "SELECT tableoid, oid, typname, "
						  "typnamespace, "
						  "(%s typowner) as rolname, "
						  "typinput::oid as typinput, "
						  "typoutput::oid as typoutput, typelem, typrelid, "
						  "CASE WHEN typrelid = 0 THEN ' '::\"char\" "
						  "ELSE (SELECT relkind FROM pg_class WHERE oid = typrelid) END as typrelkind, "
						  "typtype, typisdefined, "
						  "typname[0] = '_' AND typelem != 0 AND "
						  "(SELECT typarray FROM pg_type te WHERE oid = pg_type.typelem) = oid AS isarray "
						  "FROM pg_type",
						  username_subquery);
	}
	else
	{
         appendPQExpBuffer(query, "SELECT tableoid, oid, typname, "
						   "typnamespace, "
						   "(%s typowner) as rolname, "
						   "typinput::oid as typinput, "
						   "typoutput::oid as typoutput, typelem, typrelid, "
						   "CASE WHEN typrelid = 0 THEN ' '::\"char\" "
						   "ELSE (SELECT relkind FROM pg_class WHERE oid = typrelid) END as typrelkind, "
						   "typtype, typisdefined, "
						   "typname[0] = '_' AND typelem != 0 AS isarray "
						   "FROM pg_type",
						   username_subquery);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	tinfo = (TypeInfo *) malloc(ntups * sizeof(TypeInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_typname = PQfnumber(res, "typname");
	i_typnamespace = PQfnumber(res, "typnamespace");
	i_rolname = PQfnumber(res, "rolname");
	i_typinput = PQfnumber(res, "typinput");
	i_typoutput = PQfnumber(res, "typoutput");
	i_typelem = PQfnumber(res, "typelem");
	i_typrelid = PQfnumber(res, "typrelid");
	i_typrelkind = PQfnumber(res, "typrelkind");
	i_typtype = PQfnumber(res, "typtype");
	i_typisdefined = PQfnumber(res, "typisdefined");
	i_isarray = PQfnumber(res, "isarray");

	for (i = 0; i < ntups; i++)
	{
		tinfo[i].dobj.objType = DO_TYPE;
		tinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		tinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&tinfo[i].dobj);
		tinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_typname));
		tinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_typnamespace)),
												tinfo[i].dobj.catId.oid);
		tinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));
		tinfo[i].typelem = atooid(PQgetvalue(res, i, i_typelem));
		tinfo[i].typrelid = atooid(PQgetvalue(res, i, i_typrelid));
		tinfo[i].typrelkind = *PQgetvalue(res, i, i_typrelkind);
		tinfo[i].typtype = *PQgetvalue(res, i, i_typtype);
		tinfo[i].shellType = NULL;

		if (strcmp(PQgetvalue(res, i, i_typisdefined), "t") == 0)
			tinfo[i].isDefined = true;
		else
			tinfo[i].isDefined = false;

		if (strcmp(PQgetvalue(res, i, i_isarray), "t") == 0)
			tinfo[i].isArray = true;
		else
			tinfo[i].isArray = false;

		/* Decide whether we want to dump it */
		selectDumpableType(&tinfo[i]);

		/*
		 * If it's a domain, fetch info about its constraints, if any
		 */
		tinfo[i].nDomChecks = 0;
		tinfo[i].domChecks = NULL;
		if (tinfo[i].dobj.dump && tinfo[i].typtype == TYPTYPE_DOMAIN)
			getDomainConstraints(&(tinfo[i]));

		/*
		 * If it's a base type, make a DumpableObject representing a shell
		 * definition of the type.	We will need to dump that ahead of the I/O
		 * functions for the type.
		 *
		 * Note: the shell type doesn't have a catId.  You might think it
		 * should copy the base type's catId, but then it might capture the
		 * pg_depend entries for the type, which we don't want.
		 */
		if (tinfo[i].dobj.dump && tinfo[i].typtype == TYPTYPE_BASE)
		{
			stinfo = (ShellTypeInfo *) malloc(sizeof(ShellTypeInfo));
			stinfo->dobj.objType = DO_SHELL_TYPE;
			stinfo->dobj.catId = nilCatalogId;
			AssignDumpId(&stinfo->dobj);
			stinfo->dobj.name = strdup(tinfo[i].dobj.name);
			stinfo->dobj.namespace = tinfo[i].dobj.namespace;
			stinfo->baseType = &(tinfo[i]);
			tinfo[i].shellType = stinfo;

			/*
			 * Initially mark the shell type as not to be dumped.  We'll only
			 * dump it if the I/O functions need to be dumped; this is taken
			 * care of while sorting dependencies.
			 */
			stinfo->dobj.dump = false;
		}

		if (strlen(tinfo[i].rolname) == 0 && tinfo[i].isDefined)
			write_msg(NULL, "WARNING: owner of data type \"%s\" appears to be invalid\n",
					  tinfo[i].dobj.name);
	}

	*numTypes = ntups;

	PQclear(res);

	destroyPQExpBuffer(query);

	return tinfo;
}



/*
 * getTypeStorageOptions:
 *	  read all types with storage options in the system catalogs and return them in the
 * TypeStorageOptions* structure
 *
 *	numTypes is set to the number of types with storage options read in
 *
 */
TypeStorageOptions *
getTypeStorageOptions(int *numTypes)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	TypeStorageOptions   *tstorageoptions;
	int			i_oid;
	int			i_typname;
	int			i_typnamespace;
	int			i_typoptions;
	int			i_rolname;

	if (gp_attribute_encoding_available == false)
	{
		numTypes = 0;
		tstorageoptions = (TypeStorageOptions *) malloc(0);
		return tstorageoptions;
	}

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/*
	 * The following statement used format_type to resolve an internal name to its equivalent sql name.
	 * The format_type seems to do two things, it translates an internal type name (e.g. bpchar) into its
	 * sql equivalent (e.g. character), and it puts trailing "[]" on a type if it is an array.
	 * For any user defined type (ie. oid > 10000) or any type that might be an array (ie. starts with '_'),
	 * then we will call quote_ident. If the type is a system defined type (i.e. oid <= 10000)
	 * and can not possibly be an array (i.e. does not start with '_'), then call format_type to get the name. The
	 * reason we do not call format_type for arrays is that it will return a '[]' on the end, which can not be used
	 * when dumping the type.
	 */
	appendPQExpBuffer(query, "SELECT "
      " CASE WHEN t.oid > 10000 OR substring(t.typname from 1 for 1) = '_' "
      " THEN  quote_ident(t.typname) "
      " ELSE  pg_catalog.format_type(t.oid, NULL) "
      " END   as typname "
			", t.oid AS oid"
			", t.typnamespace AS typnamespace"
			", (%s typowner) as rolname"
			", array_to_string(a.typoptions, ', ') AS typoptions "
			" FROM pg_type AS t "
			" INNER JOIN pg_catalog.pg_type_encoding a ON a.typid = t.oid"
			" WHERE t.typisdefined = 't'", username_subquery);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	tstorageoptions = (TypeStorageOptions *) malloc(ntups * sizeof(TypeStorageOptions));

	i_typname = PQfnumber(res, "typname");
	i_oid = PQfnumber(res, "oid");
	i_typnamespace = PQfnumber(res, "typnamespace");
	i_typoptions = PQfnumber(res, "typoptions");
	i_rolname = PQfnumber(res, "rolname");

	for (i = 0; i < ntups; i++)
	{
		tstorageoptions[i].dobj.objType = DO_TYPE_STORAGE_OPTIONS;
		AssignDumpId(&tstorageoptions[i].dobj);
		tstorageoptions[i].dobj.name = strdup(PQgetvalue(res, i, i_typname));
		tstorageoptions[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		tstorageoptions[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_typnamespace)),tstorageoptions[i].dobj.catId.oid);
		tstorageoptions[i].typoptions = strdup(PQgetvalue(res, i, i_typoptions));
		tstorageoptions[i].rolname = strdup(PQgetvalue(res, i, i_rolname));
	}

	*numTypes = ntups;

	PQclear(res);

	destroyPQExpBuffer(query);

	return tstorageoptions;
}



/*
 * getOperators:
 *	  read all operators in the system catalogs and return them in the
 * OprInfo* structure
 *
 *	numOprs is set to the number of operators read in
 */
OprInfo *
getOperators(int *numOprs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	OprInfo    *oprinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_oprname;
	int			i_oprnamespace;
	int			i_rolname;
	int			i_oprcode;

	/*
	 * find all operators, including builtin operators; we filter out
	 * system-defined operators at dump-out time.
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(query, "SELECT tableoid, oid, oprname, "
					  "oprnamespace, "
					  "(%s oprowner) as rolname, "
					  "oprcode::oid as oprcode "
					  "FROM pg_operator",
					  username_subquery);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numOprs = ntups;

	oprinfo = (OprInfo *) malloc(ntups * sizeof(OprInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_oprname = PQfnumber(res, "oprname");
	i_oprnamespace = PQfnumber(res, "oprnamespace");
	i_rolname = PQfnumber(res, "rolname");
	i_oprcode = PQfnumber(res, "oprcode");

	for (i = 0; i < ntups; i++)
	{
		oprinfo[i].dobj.objType = DO_OPERATOR;
		oprinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		oprinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&oprinfo[i].dobj);
		oprinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_oprname));
		oprinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_oprnamespace)),
												  oprinfo[i].dobj.catId.oid);
		oprinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));
		oprinfo[i].oprcode = atooid(PQgetvalue(res, i, i_oprcode));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(oprinfo[i].dobj));

		if (strlen(oprinfo[i].rolname) == 0)
			write_msg(NULL, "WARNING: owner of operator \"%s\" appears to be invalid\n",
					  oprinfo[i].dobj.name);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return oprinfo;
}

/*
 * getConversions:
 *	  read all conversions in the system catalogs and return them in the
 * ConvInfo* structure
 *
 *	numConversions is set to the number of conversions read in
 */
ConvInfo *
getConversions(int *numConversions)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	ConvInfo   *convinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_conname;
	int			i_connamespace;
	int			i_rolname;

	/*
	 * find all conversions, including builtin conversions; we filter out
	 * system-defined conversions at dump-out time.
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(query, "SELECT tableoid, oid, conname, "
					  "connamespace, "
					  "(%s conowner) as rolname "
					  "FROM pg_conversion",
					  username_subquery);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numConversions = ntups;

	convinfo = (ConvInfo *) malloc(ntups * sizeof(ConvInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_conname = PQfnumber(res, "conname");
	i_connamespace = PQfnumber(res, "connamespace");
	i_rolname = PQfnumber(res, "rolname");

	for (i = 0; i < ntups; i++)
	{
		convinfo[i].dobj.objType = DO_CONVERSION;
		convinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		convinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&convinfo[i].dobj);
		convinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_conname));
		convinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_connamespace)),
												 convinfo[i].dobj.catId.oid);
		convinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(convinfo[i].dobj));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return convinfo;
}

/*
 * getOpclasses:
 *	  read all opclasses in the system catalogs and return them in the
 * OpclassInfo* structure
 *
 *	numOpclasses is set to the number of opclasses read in
 */
OpclassInfo *
getOpclasses(int *numOpclasses)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	OpclassInfo *opcinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_opcname;
	int			i_opcnamespace;
	int			i_rolname;

	/*
	 * find all opclasses, including builtin opclasses; we filter out
	 * system-defined opclasses at dump-out time.
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(query, "SELECT tableoid, oid, opcname, "
					  "opcnamespace, "
					  "(%s opcowner) as rolname "
					  "FROM pg_opclass",
					  username_subquery);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numOpclasses = ntups;

	opcinfo = (OpclassInfo *) malloc(ntups * sizeof(OpclassInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_opcname = PQfnumber(res, "opcname");
	i_opcnamespace = PQfnumber(res, "opcnamespace");
	i_rolname = PQfnumber(res, "rolname");

	for (i = 0; i < ntups; i++)
	{
		opcinfo[i].dobj.objType = DO_OPCLASS;
		opcinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		opcinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&opcinfo[i].dobj);
		opcinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_opcname));
		opcinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_opcnamespace)),
												  opcinfo[i].dobj.catId.oid);
		opcinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(opcinfo[i].dobj));

		if (strlen(opcinfo[i].rolname) == 0)
			write_msg(NULL, "WARNING: owner of operator class \"%s\" appears to be invalid\n",
					  opcinfo[i].dobj.name);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return opcinfo;
}

/*
 * getOpfamilies:
 *	  read all opfamilies in the system catalogs and return them in the
 * OpfamilyInfo* structure
 *
 *	numOpfamilies is set to the number of opfamilies read in
 */
OpfamilyInfo *
getOpfamilies(int *numOpfamilies)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	OpfamilyInfo *opfinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_opfname;
	int			i_opfnamespace;
	int			i_rolname;

	/* Before 8.3, there is no separate concept of opfamilies */
	if (g_fout->remoteVersion < 80300)
	{
		*numOpfamilies = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	/*
	 * find all opfamilies, including builtin opfamilies; we filter out
	 * system-defined opfamilies at dump-out time.
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(query, "SELECT tableoid, oid, opfname, "
					  "opfnamespace, "
					  "(%s opfowner) as rolname "
					  "FROM pg_opfamily",
					  username_subquery);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numOpfamilies = ntups;

	opfinfo = (OpfamilyInfo *) malloc(ntups * sizeof(OpfamilyInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_opfname = PQfnumber(res, "opfname");
	i_opfnamespace = PQfnumber(res, "opfnamespace");
	i_rolname = PQfnumber(res, "rolname");

	for (i = 0; i < ntups; i++)
	{
		opfinfo[i].dobj.objType = DO_OPFAMILY;
		opfinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		opfinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&opfinfo[i].dobj);
		opfinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_opfname));
		opfinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_opfnamespace)),
												  opfinfo[i].dobj.catId.oid);
		opfinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(opfinfo[i].dobj));

		if (g_fout->remoteVersion >= 70300)
		{
			if (strlen(opfinfo[i].rolname) == 0)
				write_msg(NULL, "WARNING: owner of operator family \"%s\" appears to be invalid\n",
						  opfinfo[i].dobj.name);
		}
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return opfinfo;
}

/*
 * getAggregates:
 *	  read all the user-defined aggregates in the system catalogs and
 * return them in the AggInfo* structure
 *
 * numAggs is set to the number of aggregates read in
 */
AggInfo *
getAggregates(int *numAggs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	AggInfo    *agginfo;
	int			i_tableoid;
	int			i_oid;
	int			i_aggname;
	int			i_aggnamespace;
	int			i_pronargs;
	int			i_proargtypes;
	int			i_rolname;
	int			i_aggacl;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* find all user-defined aggregates */

	appendPQExpBuffer(query, "SELECT tableoid, oid, proname as aggname, "
					  "pronamespace as aggnamespace, "
					  "pronargs, proargtypes, "
					  "(%s proowner) as rolname, "
					  "proacl as aggacl "
					  "FROM pg_proc "
					  "WHERE proisagg "
					  "AND pronamespace != "
			   "(select oid from pg_namespace where nspname = 'pg_catalog')",
					  username_subquery);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numAggs = ntups;

	agginfo = (AggInfo *) malloc(ntups * sizeof(AggInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_aggname = PQfnumber(res, "aggname");
	i_aggnamespace = PQfnumber(res, "aggnamespace");
	i_pronargs = PQfnumber(res, "pronargs");
	i_proargtypes = PQfnumber(res, "proargtypes");
	i_rolname = PQfnumber(res, "rolname");
	i_aggacl = PQfnumber(res, "aggacl");

	for (i = 0; i < ntups; i++)
	{
		agginfo[i].aggfn.dobj.objType = DO_AGG;
		agginfo[i].aggfn.dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		agginfo[i].aggfn.dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&agginfo[i].aggfn.dobj);
		agginfo[i].aggfn.dobj.name = strdup(PQgetvalue(res, i, i_aggname));
		agginfo[i].aggfn.dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_aggnamespace)),
											agginfo[i].aggfn.dobj.catId.oid);
		agginfo[i].aggfn.rolname = strdup(PQgetvalue(res, i, i_rolname));
		if (strlen(agginfo[i].aggfn.rolname) == 0)
			write_msg(NULL, "WARNING: owner of aggregate function \"%s\" appears to be invalid\n",
					  agginfo[i].aggfn.dobj.name);
		agginfo[i].aggfn.lang = InvalidOid;		/* not currently interesting */
		agginfo[i].aggfn.prorettype = InvalidOid;		/* not saved */
		agginfo[i].aggfn.proacl = strdup(PQgetvalue(res, i, i_aggacl));
		agginfo[i].aggfn.nargs = atoi(PQgetvalue(res, i, i_pronargs));
		if (agginfo[i].aggfn.nargs == 0)
			agginfo[i].aggfn.argtypes = NULL;
		else
		{
			agginfo[i].aggfn.argtypes = (Oid *) malloc(agginfo[i].aggfn.nargs * sizeof(Oid));
			parseOidArray(PQgetvalue(res, i, i_proargtypes),
						  agginfo[i].aggfn.argtypes,
						  agginfo[i].aggfn.nargs);
		}

		/* Decide whether we want to dump it */
		selectDumpableObject(&(agginfo[i].aggfn.dobj));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return agginfo;
}

/*
 * getExtProtocols:
 *	  read all the user-defined protocols in the system catalogs and
 * return them in the ExtProtInfo* structure
 *
 * numExtProtocols is set to the number of protocols read in
 */
/*	Declared in pg_dump.h */
ExtProtInfo *
getExtProtocols(int *numExtProtocols)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	ExtProtInfo *ptcinfo;
	int			i_oid;
	int			i_tableoid;
	int			i_ptcname;
	int			i_rolname;
	int			i_ptcacl;
	int			i_ptctrusted;
	int 		i_ptcreadid;
	int			i_ptcwriteid;
	int			i_ptcvalidid;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* find all user-defined aggregates */

	appendPQExpBuffer(query, "SELECT ptc.tableoid as tableoid, "
							 "       ptc.oid as oid, "
							 "       ptc.ptcname as ptcname, "
							 "       ptcreadfn as ptcreadoid, "
							 "       ptcwritefn as ptcwriteoid, "
							 "		 ptcvalidatorfn as ptcvaloid, "
							 "       (%s ptc.ptcowner) as rolname, "
							 "       ptc.ptctrusted as ptctrusted, "
							 "       ptc.ptcacl as ptcacl "
							 "FROM   pg_extprotocol ptc",
							 	 	 username_subquery);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numExtProtocols = ntups;

	ptcinfo = (ExtProtInfo *) malloc(ntups * sizeof(ExtProtInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_ptcname = PQfnumber(res, "ptcname");
	i_rolname = PQfnumber(res, "rolname");
	i_ptcacl = PQfnumber(res, "ptcacl");
	i_ptctrusted = PQfnumber(res, "ptctrusted");
	i_ptcreadid = PQfnumber(res, "ptcreadoid");
	i_ptcwriteid = PQfnumber(res, "ptcwriteoid");
	i_ptcvalidid = PQfnumber(res, "ptcvaloid");

	for (i = 0; i < ntups; i++)
	{
		ptcinfo[i].dobj.objType = DO_EXTPROTOCOL;
		ptcinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		ptcinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&ptcinfo[i].dobj);
		ptcinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_ptcname));
		ptcinfo[i].dobj.namespace = NULL;
		ptcinfo[i].ptcowner = strdup(PQgetvalue(res, i, i_rolname));
		if (strlen(ptcinfo[i].ptcowner) == 0)
			write_msg(NULL, "WARNING: owner of external protocol \"%s\" appears to be invalid\n",
						ptcinfo[i].dobj.name);

		if (PQgetisnull(res, i, i_ptcreadid))
			ptcinfo[i].ptcreadid = InvalidOid;
		else
			ptcinfo[i].ptcreadid = atooid(PQgetvalue(res, i, i_ptcreadid));

		if (PQgetisnull(res, i, i_ptcwriteid))
			ptcinfo[i].ptcwriteid = InvalidOid;
		else
			ptcinfo[i].ptcwriteid = atooid(PQgetvalue(res, i, i_ptcwriteid));

		if (PQgetisnull(res, i, i_ptcvalidid))
			ptcinfo[i].ptcvalidid = InvalidOid;
		else
			ptcinfo[i].ptcvalidid = atooid(PQgetvalue(res, i, i_ptcvalidid));

		ptcinfo[i].ptcacl = strdup(PQgetvalue(res, i, i_ptcacl));
		ptcinfo[i].ptctrusted = *(PQgetvalue(res, i, i_ptctrusted)) == 't';

		/* Decide whether we want to dump it */
		selectDumpableObject(&(ptcinfo[i].dobj));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return ptcinfo;
}

/*
 * getFuncs:
 *	  read all the user-defined functions in the system catalogs and
 * return them in the FuncInfo* structure
 *
 * numFuncs is set to the number of functions read in
 */
FuncInfo *
getFuncs(int *numFuncs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	FuncInfo   *finfo;
	int			i_tableoid;
	int			i_oid;
	int			i_proname;
	int			i_pronamespace;
	int			i_rolname;
	int			i_prolang;
	int			i_pronargs;
	int			i_proargtypes;
	int			i_prorettype;
	int			i_proacl;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* find all user-defined funcs */

	appendPQExpBuffer(query,
					  "SELECT tableoid, oid, proname, prolang, "
					  "pronargs, proargtypes, prorettype, proacl, "
					  "pronamespace, "
					  "(%s proowner) as rolname "
					  "FROM pg_proc "
					  "WHERE NOT proisagg "
					  "AND pronamespace != "
					  "(select oid from pg_namespace"
					  " where nspname = 'pg_catalog')",
					  username_subquery);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numFuncs = ntups;

	finfo = (FuncInfo *) calloc(ntups, sizeof(FuncInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_proname = PQfnumber(res, "proname");
	i_pronamespace = PQfnumber(res, "pronamespace");
	i_rolname = PQfnumber(res, "rolname");
	i_prolang = PQfnumber(res, "prolang");
	i_pronargs = PQfnumber(res, "pronargs");
	i_proargtypes = PQfnumber(res, "proargtypes");
	i_prorettype = PQfnumber(res, "prorettype");
	i_proacl = PQfnumber(res, "proacl");

	for (i = 0; i < ntups; i++)
	{
		finfo[i].dobj.objType = DO_FUNC;
		finfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		finfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&finfo[i].dobj);
		finfo[i].dobj.name = strdup(PQgetvalue(res, i, i_proname));
		finfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_pronamespace)),
						  finfo[i].dobj.catId.oid);
		finfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));
		finfo[i].lang = atooid(PQgetvalue(res, i, i_prolang));
		finfo[i].prorettype = atooid(PQgetvalue(res, i, i_prorettype));
		finfo[i].proacl = strdup(PQgetvalue(res, i, i_proacl));
		finfo[i].nargs = atoi(PQgetvalue(res, i, i_pronargs));
		if (finfo[i].nargs == 0)
			finfo[i].argtypes = NULL;
		else
		{
			finfo[i].argtypes = (Oid *) malloc(finfo[i].nargs * sizeof(Oid));
			parseOidArray(PQgetvalue(res, i, i_proargtypes),
						  finfo[i].argtypes, finfo[i].nargs);
		}

		/* Decide whether we want to dump it */
		selectDumpableFunction(&finfo[i]);
		selectDumpableObject(&(finfo[i].dobj));

		if (strlen(finfo[i].rolname) == 0)
			write_msg(NULL,
				 "WARNING: owner of function \"%s\" appears to be invalid\n",
					  finfo[i].dobj.name);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return finfo;
}

/*
 * getTables
 *	  read all the user-defined tables (no indexes, no catalogs)
 * in the system catalogs return them in the TableInfo* structure
 *
 * numTables is set to the number of tables read in
 */
TableInfo *
getTables(int *numTables)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();
	PQExpBuffer lockquery = createPQExpBuffer();
	TableInfo  *tblinfo;
	int			i_reltableoid;
	int			i_reloid;
	int			i_relname;
	int			i_relnamespace;
	int			i_relkind;
	int			i_relstorage;
	int			i_relacl;
	int			i_rolname;
	int			i_relchecks;
	int			i_reltriggers;
	int			i_relhasindex;
	int			i_relhasrules;
	int			i_relhasoids;
	int			i_relistoasted;
	int			i_owning_tab;
	int			i_owning_col;
	int			i_reltablespace;
	int			i_reloptions;
	int			i_parrelid;
	int			i_parlevel;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/*
	 * Find all the tables (including views and sequences).
	 *
	 * We include system catalogs, so that we can work if a user table is
	 * defined to inherit from a system catalog (pretty weird, but...)
	 *
	 * We ignore tables that are not type 'r' (ordinary relation), 'S'
	 * (sequence), 'v' (view), or 'c' (composite type).
	 *
	 * Composite-type table entries won't be dumped as such, but we have to
	 * make a DumpableObject for them so that we can track dependencies of the
	 * composite type (pg_depend entries for columns of the composite type
	 * link to the pg_class entry not the pg_type entry).
	 *
	 * Note: in this phase we should collect only a minimal amount of
	 * information about each table, basically just enough to decide if it is
	 * interesting. We must fetch all tables in this phase because otherwise
	 * we cannot correctly identify inherited columns, owned sequences, etc.
	 */

	/*
	 * Left join to pick up dependency info linking sequences to their owning
	 * column, if any (note this dependency is AUTO as of 8.2)
	 */
	appendPQExpBuffer(query,
					  "SELECT c.tableoid, c.oid, relname, "
					  "relacl, relkind, relstorage, relnamespace, "
					  "(%s relowner) as rolname, "
					  "relchecks, reltriggers, "
					  "relhasindex, relhasrules, relhasoids, "
					  "(reltoastrelid != 0) as relistoasted, "
					  "d.refobjid as owning_tab, "
					  "d.refobjsubid as owning_col, "
					  "(SELECT spcname FROM pg_tablespace t WHERE t.oid = c.reltablespace) AS reltablespace, "
					  "array_to_string(c.reloptions, ', ') as reloptions, "
					  "p.parrelid as parrelid, "
					  "pl.parlevel as parlevel "
					  "from pg_class c "
					  "left join pg_depend d on "
					  "(c.relkind = '%c' and "
					  "d.classid = c.tableoid and d.objid = c.oid and "
					  "d.objsubid = 0 and "
					  "d.refclassid = c.tableoid and d.deptype = 'a') "
					  "left join pg_partition_rule pr on c.oid = pr.parchildrelid "
					  "left join pg_partition p on pr.paroid = p.oid "
					  "left join pg_partition pl on (c.oid = pl.parrelid and pl.parlevel = 0) "
					  "where relkind in ('%c', '%c', '%c', '%c') %s"
					  "order by c.oid",
					  username_subquery,
					  RELKIND_SEQUENCE,
					  RELKIND_RELATION, RELKIND_SEQUENCE,
					  RELKIND_VIEW, RELKIND_COMPOSITE_TYPE,
					  g_fout->remoteVersion >= 80209 ?
					  "AND c.oid NOT IN (select p.parchildrelid from pg_partition_rule p left "
					  "join pg_exttable e on p.parchildrelid=e.reloid where e.reloid is null)" : "");

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numTables = ntups;

	/*
	 * Extract data from result and lock dumpable tables.  We do the locking
	 * before anything else, to minimize the window wherein a table could
	 * disappear under us.
	 *
	 * Note that we have to save info about all tables here, even when dumping
	 * only one, because we don't yet know which tables might be inheritance
	 * ancestors of the target table.
	 */
	tblinfo = (TableInfo *) calloc(ntups, sizeof(TableInfo));

	i_reltableoid = PQfnumber(res, "tableoid");
	i_reloid = PQfnumber(res, "oid");
	i_relname = PQfnumber(res, "relname");
	i_relnamespace = PQfnumber(res, "relnamespace");
	i_relacl = PQfnumber(res, "relacl");
	i_relkind = PQfnumber(res, "relkind");
	i_relstorage = PQfnumber(res, "relstorage");
	i_rolname = PQfnumber(res, "rolname");
	i_relchecks = PQfnumber(res, "relchecks");
	i_reltriggers = PQfnumber(res, "reltriggers");
	i_relhasindex = PQfnumber(res, "relhasindex");
	i_relhasrules = PQfnumber(res, "relhasrules");
	i_relhasoids = PQfnumber(res, "relhasoids");
	i_relistoasted = PQfnumber(res, "relistoasted");
	i_owning_tab = PQfnumber(res, "owning_tab");
	i_owning_col = PQfnumber(res, "owning_col");
	i_reltablespace = PQfnumber(res, "reltablespace");
	i_reloptions = PQfnumber(res, "reloptions");
	i_parrelid = PQfnumber(res, "parrelid");
	i_parlevel = PQfnumber(res, "parlevel");

	for (i = 0; i < ntups; i++)
	{
		tblinfo[i].dobj.objType = DO_TABLE;
		tblinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_reltableoid));
		tblinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_reloid));
		AssignDumpId(&tblinfo[i].dobj);
		tblinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_relname));
		tblinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_relnamespace)),
												  tblinfo[i].dobj.catId.oid);
		tblinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));
		tblinfo[i].relacl = strdup(PQgetvalue(res, i, i_relacl));
		tblinfo[i].relkind = *(PQgetvalue(res, i, i_relkind));
		tblinfo[i].relstorage = *(PQgetvalue(res, i, i_relstorage));
		tblinfo[i].hasindex = (strcmp(PQgetvalue(res, i, i_relhasindex), "t") == 0);
		tblinfo[i].hasrules = (strcmp(PQgetvalue(res, i, i_relhasrules), "t") == 0);
		tblinfo[i].hasoids = (strcmp(PQgetvalue(res, i, i_relhasoids), "t") == 0);
		tblinfo[i].istoasted = (strcmp(PQgetvalue(res, i, i_relistoasted), "t") == 0);
		tblinfo[i].ncheck = atoi(PQgetvalue(res, i, i_relchecks));
		tblinfo[i].ntrig = atoi(PQgetvalue(res, i, i_reltriggers));
		if (PQgetisnull(res, i, i_owning_tab))
		{
			tblinfo[i].owning_tab = InvalidOid;
			tblinfo[i].owning_col = 0;
		}
		else
		{
			tblinfo[i].owning_tab = atooid(PQgetvalue(res, i, i_owning_tab));
			tblinfo[i].owning_col = atoi(PQgetvalue(res, i, i_owning_col));
		}
		tblinfo[i].reltablespace = strdup(PQgetvalue(res, i, i_reltablespace));
		tblinfo[i].reloptions = strdup(PQgetvalue(res, i, i_reloptions));
		tblinfo[i].parrelid = atooid(PQgetvalue(res, i, i_parrelid));
		if (tblinfo[i].parrelid != 0)
		{
			/*
			 * Length of tmpStr is bigger than the sum of NAMEDATALEN 
			 * and the length of EXT_PARTITION_NAME_POSTFIX 
			 */
			char tmpStr[500];
			snprintf(tmpStr, sizeof(tmpStr), "%s%s", tblinfo[i].dobj.name, EXT_PARTITION_NAME_POSTFIX);
			tblinfo[i].dobj.name = strdup(tmpStr);
		}
		if (PQgetisnull(res, i, i_parlevel) ||
			atoi(PQgetvalue(res, i, i_parlevel)) > 0)
			tblinfo[i].parparent = false;
		else
			tblinfo[i].parparent = true;

		/* other fields were zeroed above */

		/*
		 * Decide whether we want to dump this table.
		 */
		if (tblinfo[i].relkind == RELKIND_COMPOSITE_TYPE)
			tblinfo[i].dobj.dump = false;
		else
			selectDumpableTable(&tblinfo[i]);
		tblinfo[i].interesting = tblinfo[i].dobj.dump;

		/*
		 * Read-lock target tables to make sure they aren't DROPPED or altered
		 * in schema before we get around to dumping them.
		 *
		 * Note that we don't explicitly lock parents of the target tables; we
		 * assume our lock on the child is enough to prevent schema
		 * alterations to parent tables.
		 *
		 * NOTE: it'd be kinda nice to lock other relations too, not only
		 * plain tables, but the backend doesn't presently allow that.
		 */
		if (tblinfo[i].dobj.dump && tblinfo[i].relkind == RELKIND_RELATION && tblinfo[i].parrelid == 0)
		{
			resetPQExpBuffer(lockquery);
			appendPQExpBuffer(lockquery,
							  "LOCK TABLE %s IN ACCESS SHARE MODE",
						 fmtQualifiedId(tblinfo[i].dobj.namespace->dobj.name,
										tblinfo[i].dobj.name));
			do_sql_command(g_conn, lockquery->data);
		}

		/* Emit notice if join for owner failed */
		if (strlen(tblinfo[i].rolname) == 0)
			write_msg(NULL, "WARNING: owner of table \"%s\" appears to be invalid\n",
					  tblinfo[i].dobj.name);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(lockquery);

	return tblinfo;
}

/*
 * getOwnedSeqs
 *	  identify owned sequences and mark them as dumpable if owning table is
 *
 * We used to do this in getTables(), but it's better to do it after the
 * index used by findTableByOid() has been set up.
 */
void
getOwnedSeqs(TableInfo tblinfo[], int numTables)
{
	int			i;

	/*
	 * Force sequences that are "owned" by table columns to be dumped whenever
	 * their owning table is being dumped.
	 */
	for (i = 0; i < numTables; i++)
	{
		TableInfo  *seqinfo = &tblinfo[i];
		TableInfo  *owning_tab;

		if (!OidIsValid(seqinfo->owning_tab))
			continue;			/* not an owned sequence */
		if (seqinfo->dobj.dump)
			continue;			/* no need to search */
		owning_tab = findTableByOid(seqinfo->owning_tab);
		if (owning_tab && owning_tab->dobj.dump)
		{
			seqinfo->interesting = true;
			seqinfo->dobj.dump = true;
		}
	}
}

/*
 * getInherits
 *	  read all the inheritance information
 * from the system catalogs return them in the InhInfo* structure
 *
 * numInherits is set to the number of pairs read in
 */
InhInfo *
getInherits(int *numInherits)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	InhInfo    *inhinfo;

	int			i_inhrelid;
	int			i_inhparent;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/* find all the inheritance information */

	appendPQExpBuffer(query, "SELECT inhrelid, inhparent FROM pg_inherits");

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numInherits = ntups;

	inhinfo = (InhInfo *) malloc(ntups * sizeof(InhInfo));

	i_inhrelid = PQfnumber(res, "inhrelid");
	i_inhparent = PQfnumber(res, "inhparent");

	for (i = 0; i < ntups; i++)
	{
		inhinfo[i].inhrelid = atooid(PQgetvalue(res, i, i_inhrelid));
		inhinfo[i].inhparent = atooid(PQgetvalue(res, i, i_inhparent));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return inhinfo;
}

/*
 * getIndexes
 *	  get information about every index on a dumpable table
 *
 * Note: index data is not returned directly to the caller, but it
 * does get entered into the DumpableObject tables.
 */
void
getIndexes(TableInfo tblinfo[], int numTables)
{
	int			i,
				j;
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	IndxInfo   *indxinfo;
	ConstraintInfo *constrinfo;
	int			i_tableoid,
				i_oid,
				i_indexname,
				i_indexdef,
				i_indnkeys,
				i_indkey,
				i_indisclustered,
				i_contype,
				i_conname,
				i_contableoid,
				i_conoid,
				i_tablespace,
				i_options;
	int			ntups;

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		/* Only plain tables have indexes */
		if (tbinfo->relkind != RELKIND_RELATION || !tbinfo->hasindex)
			continue;

		/* Ignore indexes of tables not to be dumped */
		if (!tbinfo->dobj.dump)
			continue;

		if (g_verbose)
			write_msg(NULL, "reading indexes for table \"%s\"\n",
					  tbinfo->dobj.name);

		/* Make sure we are in proper schema so indexdef is right */
		selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

		/*
		 * The point of the messy-looking outer join is to find a constraint
		 * that is related by an internal dependency link to the index. If we
		 * find one, create a CONSTRAINT entry linked to the INDEX entry.  We
		 * assume an index won't have more than one internal dependency.
		 */
		resetPQExpBuffer(query);
		appendPQExpBuffer(query,
						  "SELECT t.tableoid, t.oid, "
						  "t.relname as indexname, "
					 "pg_catalog.pg_get_indexdef(i.indexrelid) as indexdef, "
						  "t.relnatts as indnkeys, "
						  "i.indkey, i.indisclustered, "
						  "c.contype, c.conname, "
						  "c.tableoid as contableoid, "
						  "c.oid as conoid, "
						  "(SELECT spcname FROM pg_catalog.pg_tablespace s WHERE s.oid = t.reltablespace) as tablespace, "
						  "array_to_string(t.reloptions, ', ') as options "
						  "FROM pg_catalog.pg_index i "
					  "JOIN pg_catalog.pg_class t ON (t.oid = i.indexrelid) "
						  "LEFT JOIN pg_catalog.pg_depend d "
						  "ON (d.classid = t.tableoid "
						  "AND d.objid = t.oid "
						  "AND d.deptype = 'i') "
						  "LEFT JOIN pg_catalog.pg_constraint c "
						  "ON (d.refclassid = c.tableoid "
						  "AND d.refobjid = c.oid) "
						  "WHERE i.indrelid = '%u'::pg_catalog.oid "
						  "ORDER BY indexname",
						  tbinfo->dobj.catId.oid);

		res = PQexec(g_conn, query->data);
		check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

		i_tableoid = PQfnumber(res, "tableoid");
		i_oid = PQfnumber(res, "oid");
		i_indexname = PQfnumber(res, "indexname");
		i_indexdef = PQfnumber(res, "indexdef");
		i_indnkeys = PQfnumber(res, "indnkeys");
		i_indkey = PQfnumber(res, "indkey");
		i_indisclustered = PQfnumber(res, "indisclustered");
		i_contype = PQfnumber(res, "contype");
		i_conname = PQfnumber(res, "conname");
		i_contableoid = PQfnumber(res, "contableoid");
		i_conoid = PQfnumber(res, "conoid");
		i_tablespace = PQfnumber(res, "tablespace");
		i_options = PQfnumber(res, "options");

		indxinfo = (IndxInfo *) malloc(ntups * sizeof(IndxInfo));
		constrinfo = (ConstraintInfo *) malloc(ntups * sizeof(ConstraintInfo));

		for (j = 0; j < ntups; j++)
		{
			char		contype;

			indxinfo[j].dobj.objType = DO_INDEX;
			indxinfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_tableoid));
			indxinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
			AssignDumpId(&indxinfo[j].dobj);
			indxinfo[j].dobj.name = strdup(PQgetvalue(res, j, i_indexname));
			indxinfo[j].dobj.namespace = tbinfo->dobj.namespace;
			indxinfo[j].indextable = tbinfo;
			indxinfo[j].indexdef = strdup(PQgetvalue(res, j, i_indexdef));
			indxinfo[j].indnkeys = atoi(PQgetvalue(res, j, i_indnkeys));
			indxinfo[j].tablespace = strdup(PQgetvalue(res, j, i_tablespace));
			indxinfo[j].options = strdup(PQgetvalue(res, j, i_options));

			/*
			 * In pre-7.4 releases, indkeys may contain more entries than
			 * indnkeys says (since indnkeys will be 1 for a functional
			 * index).	We don't actually care about this case since we don't
			 * examine indkeys except for indexes associated with PRIMARY and
			 * UNIQUE constraints, which are never functional indexes. But we
			 * have to allocate enough space to keep parseOidArray from
			 * complaining.
			 */
			indxinfo[j].indkeys = (Oid *) malloc(INDEX_MAX_KEYS * sizeof(Oid));
			parseOidArray(PQgetvalue(res, j, i_indkey),
						  indxinfo[j].indkeys, INDEX_MAX_KEYS);
			indxinfo[j].indisclustered = (PQgetvalue(res, j, i_indisclustered)[0] == 't');
			contype = *(PQgetvalue(res, j, i_contype));

			if (contype == 'p' || contype == 'u')
			{
				/*
				 * If we found a constraint matching the index, create an
				 * entry for it.
				 *
				 * In a pre-7.3 database, we take this path iff the index was
				 * marked indisprimary.
				 */
				constrinfo[j].dobj.objType = DO_CONSTRAINT;
				constrinfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_contableoid));
				constrinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_conoid));
				AssignDumpId(&constrinfo[j].dobj);
				constrinfo[j].dobj.name = strdup(PQgetvalue(res, j, i_conname));
				constrinfo[j].dobj.namespace = tbinfo->dobj.namespace;
				constrinfo[j].contable = tbinfo;
				constrinfo[j].condomain = NULL;
				constrinfo[j].contype = contype;
				constrinfo[j].condef = NULL;
				constrinfo[j].conindex = indxinfo[j].dobj.dumpId;
				constrinfo[j].coninherited = false;
				constrinfo[j].separate = true;

				indxinfo[j].indexconstraint = constrinfo[j].dobj.dumpId;

				/* If pre-7.3 DB, better make sure table comes first */
				addObjectDependency(&constrinfo[j].dobj,
									tbinfo->dobj.dumpId);
			}
			else
			{
				/* Plain secondary index */
				indxinfo[j].indexconstraint = 0;
			}
		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
}

/*
 * getConstraints
 *
 * Get info about constraints on dumpable tables.
 *
 * Currently handles foreign keys only.
 * Unique and primary key constraints are handled with indexes,
 * while check constraints are processed in getTableAttrs().
 */
void
getConstraints(TableInfo tblinfo[], int numTables)
{
	int			i,
				j;
	ConstraintInfo *constrinfo;
	PQExpBuffer query;
	PGresult   *res;
	int			i_condef,
				i_contableoid,
				i_conoid,
				i_conname;
	int			ntups;

	query = createPQExpBuffer();

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		if (tbinfo->ntrig == 0 || !tbinfo->dobj.dump)
			continue;

		if (g_verbose)
			write_msg(NULL, "reading foreign key constraints for table \"%s\"\n",
					  tbinfo->dobj.name);

		/*
		 * select table schema to ensure constraint expr is qualified if
		 * needed
		 */
		selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

		resetPQExpBuffer(query);
		appendPQExpBuffer(query,
						  "SELECT tableoid, oid, conname, "
						  "pg_catalog.pg_get_constraintdef(oid) as condef "
						  "FROM pg_catalog.pg_constraint "
						  "WHERE conrelid = '%u'::pg_catalog.oid "
						  "AND contype = 'f'",
						  tbinfo->dobj.catId.oid);
		res = PQexec(g_conn, query->data);
		check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

		i_contableoid = PQfnumber(res, "tableoid");
		i_conoid = PQfnumber(res, "oid");
		i_conname = PQfnumber(res, "conname");
		i_condef = PQfnumber(res, "condef");

		constrinfo = (ConstraintInfo *) malloc(ntups * sizeof(ConstraintInfo));

		for (j = 0; j < ntups; j++)
		{
			constrinfo[j].dobj.objType = DO_FK_CONSTRAINT;
			constrinfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_contableoid));
			constrinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_conoid));
			AssignDumpId(&constrinfo[j].dobj);
			constrinfo[j].dobj.name = strdup(PQgetvalue(res, j, i_conname));
			constrinfo[j].dobj.namespace = tbinfo->dobj.namespace;
			constrinfo[j].contable = tbinfo;
			constrinfo[j].condomain = NULL;
			constrinfo[j].contype = 'f';
			constrinfo[j].condef = strdup(PQgetvalue(res, j, i_condef));
			constrinfo[j].conindex = 0;
			constrinfo[j].coninherited = false;
			constrinfo[j].separate = true;
		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
}

/*
 * getDomainConstraints
 *
 * Get info about constraints on a domain.
 */
static void
getDomainConstraints(TypeInfo *tinfo)
{
	int			i;
	ConstraintInfo *constrinfo;
	PQExpBuffer query;
	PGresult   *res;
	int			i_tableoid,
				i_oid,
				i_conname,
				i_consrc;
	int			ntups;


	/*
	 * select appropriate schema to ensure names in constraint are properly
	 * qualified
	 */
	selectSourceSchema(tinfo->dobj.namespace->dobj.name);

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "SELECT tableoid, oid, conname, "
					  "pg_catalog.pg_get_constraintdef(oid) AS consrc "
					  "FROM pg_catalog.pg_constraint "
					  "WHERE contypid = '%u'::pg_catalog.oid "
					  "ORDER BY conname",
					  tinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_conname = PQfnumber(res, "conname");
	i_consrc = PQfnumber(res, "consrc");

	constrinfo = (ConstraintInfo *) malloc(ntups * sizeof(ConstraintInfo));

	tinfo->nDomChecks = ntups;
	tinfo->domChecks = constrinfo;

	for (i = 0; i < ntups; i++)
	{
		constrinfo[i].dobj.objType = DO_CONSTRAINT;
		constrinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		constrinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&constrinfo[i].dobj);
		constrinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_conname));
		constrinfo[i].dobj.namespace = tinfo->dobj.namespace;
		constrinfo[i].contable = NULL;
		constrinfo[i].condomain = tinfo;
		constrinfo[i].contype = 'c';
		constrinfo[i].condef = strdup(PQgetvalue(res, i, i_consrc));
		constrinfo[i].conindex = 0;
		constrinfo[i].coninherited = false;
		constrinfo[i].separate = false;

		/*
		 * Make the domain depend on the constraint, ensuring it won't be
		 * output till any constraint dependencies are OK.
		 */
		addObjectDependency(&tinfo->dobj,
							constrinfo[i].dobj.dumpId);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getRules
 *	  get basic information about every rule in the system
 *
 * numRules is set to the number of rules read in
 */
RuleInfo *
getRules(int *numRules)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	RuleInfo   *ruleinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_rulename;
	int			i_ruletable;
	int			i_ev_type;
	int			i_is_instead;
	int			i_ev_enabled;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	if (g_fout->remoteVersion >= 80300)
	{
		appendPQExpBuffer(query, "SELECT "
						  "tableoid, oid, rulename, "
						  "ev_class as ruletable, ev_type, is_instead, "
						  "ev_enabled "
						  "FROM pg_rewrite "
						  "ORDER BY oid");
	}
	else
	{
		appendPQExpBuffer(query, "SELECT "
						  "tableoid, oid, rulename, "
						  "ev_class as ruletable, ev_type, is_instead, "
						  "'O'::char as ev_enabled "
						  "FROM pg_rewrite "
						  "ORDER BY oid");
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numRules = ntups;

	ruleinfo = (RuleInfo *) malloc(ntups * sizeof(RuleInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_rulename = PQfnumber(res, "rulename");
	i_ruletable = PQfnumber(res, "ruletable");
	i_ev_type = PQfnumber(res, "ev_type");
	i_is_instead = PQfnumber(res, "is_instead");
	i_ev_enabled = PQfnumber(res, "ev_enabled");

	for (i = 0; i < ntups; i++)
	{
		Oid			ruletableoid;

		ruleinfo[i].dobj.objType = DO_RULE;
		ruleinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		ruleinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&ruleinfo[i].dobj);
		ruleinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_rulename));
		ruletableoid = atooid(PQgetvalue(res, i, i_ruletable));
		ruleinfo[i].ruletable = findTableByOid(ruletableoid);
		if (ruleinfo[i].ruletable == NULL)
		{
			write_msg(NULL, "failed sanity check, parent table OID %u of pg_rewrite entry OID %u not found\n",
					  ruletableoid,
					  ruleinfo[i].dobj.catId.oid);
			exit_nicely();
		}
		ruleinfo[i].dobj.namespace = ruleinfo[i].ruletable->dobj.namespace;
		ruleinfo[i].dobj.dump = ruleinfo[i].ruletable->dobj.dump;
		ruleinfo[i].ev_type = *(PQgetvalue(res, i, i_ev_type));
		ruleinfo[i].is_instead = *(PQgetvalue(res, i, i_is_instead)) == 't';
		ruleinfo[i].ev_enabled = *(PQgetvalue(res, i, i_ev_enabled));
		if (ruleinfo[i].ruletable)
		{
			/*
			 * If the table is a view, force its ON SELECT rule to be sorted
			 * before the view itself --- this ensures that any dependencies
			 * for the rule affect the table's positioning. Other rules are
			 * forced to appear after their table.
			 */
			if (ruleinfo[i].ruletable->relkind == RELKIND_VIEW &&
				ruleinfo[i].ev_type == '1' && ruleinfo[i].is_instead)
			{
				addObjectDependency(&ruleinfo[i].ruletable->dobj,
									ruleinfo[i].dobj.dumpId);
				/* We'll merge the rule into CREATE VIEW, if possible */
				ruleinfo[i].separate = false;
			}
			else
			{
				addObjectDependency(&ruleinfo[i].dobj,
									ruleinfo[i].ruletable->dobj.dumpId);
				ruleinfo[i].separate = true;
			}
		}
		else
			ruleinfo[i].separate = true;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return ruleinfo;
}

/*
 * getTriggers
 *	  get information about every trigger on a dumpable table
 *
 * Note: trigger data is not returned directly to the caller, but it
 * does get entered into the DumpableObject tables.
 */
void
getTriggers(TableInfo tblinfo[], int numTables)
{
	int			i,
				j;
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	TriggerInfo *tginfo;
	int			i_tableoid,
				i_oid,
				i_tgname,
				i_tgfname,
				i_tgtype,
				i_tgnargs,
				i_tgargs,
				i_tgisconstraint,
				i_tgconstrname,
				i_tgconstrrelid,
				i_tgconstrrelname,
				i_tgenabled,
				i_tgdeferrable,
				i_tginitdeferred;
	int			ntups;

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		if (tbinfo->ntrig == 0 || !tbinfo->dobj.dump)
			continue;

		if (g_verbose)
			write_msg(NULL, "reading triggers for table \"%s\"\n",
					  tbinfo->dobj.name);

		/*
		 * select table schema to ensure regproc name is qualified if needed
		 */
		selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

		resetPQExpBuffer(query);

		if (g_fout->remoteVersion >= 80300)
		{
			/*
			 * We ignore triggers that are tied to a foreign-key constraint
			 */
			appendPQExpBuffer(query,
							  "SELECT tgname, "
							  "tgfoid::pg_catalog.regproc as tgfname, "
							  "tgtype, tgnargs, tgargs, tgenabled, "
							  "tgisconstraint, tgconstrname, tgdeferrable, "
							  "tgconstrrelid, tginitdeferred, tableoid, oid, "
					 "tgconstrrelid::pg_catalog.regclass as tgconstrrelname "
							  "from pg_catalog.pg_trigger t "
							  "where tgrelid = '%u'::pg_catalog.oid "
							  "and tgconstraint = 0",
							  tbinfo->dobj.catId.oid);
		}
		else
		{
			/*
			 * We ignore triggers that are tied to a foreign-key constraint,
			 * but in these versions we have to grovel through pg_constraint
			 * to find out
			 */
			appendPQExpBuffer(query,
							  "SELECT tgname, "
							  "tgfoid::pg_catalog.regproc as tgfname, "
							  "tgtype, tgnargs, tgargs, tgenabled, "
							  "tgisconstraint, tgconstrname, tgdeferrable, "
							  "tgconstrrelid, tginitdeferred, tableoid, oid, "
					 "tgconstrrelid::pg_catalog.regclass as tgconstrrelname "
							  "from pg_catalog.pg_trigger t "
							  "where tgrelid = '%u'::pg_catalog.oid "
							  "and (not tgisconstraint "
							  " OR NOT EXISTS"
							  "  (SELECT 1 FROM pg_catalog.pg_depend d "
							  "   JOIN pg_catalog.pg_constraint c ON (d.refclassid = c.tableoid AND d.refobjid = c.oid) "
							  "   WHERE d.classid = t.tableoid AND d.objid = t.oid AND d.deptype = 'i' AND c.contype = 'f'))",
							  tbinfo->dobj.catId.oid);
		}

		res = PQexec(g_conn, query->data);
		check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

		/*
		 * We may have less triggers than recorded due to having ignored
		 * foreign-key triggers
		 */
		if (ntups > tbinfo->ntrig)
		{
			write_msg(NULL, "expected %d triggers on table \"%s\" but found %d\n",
					  tbinfo->ntrig, tbinfo->dobj.name, ntups);
			exit_nicely();
		}
		i_tableoid = PQfnumber(res, "tableoid");
		i_oid = PQfnumber(res, "oid");
		i_tgname = PQfnumber(res, "tgname");
		i_tgfname = PQfnumber(res, "tgfname");
		i_tgtype = PQfnumber(res, "tgtype");
		i_tgnargs = PQfnumber(res, "tgnargs");
		i_tgargs = PQfnumber(res, "tgargs");
		i_tgisconstraint = PQfnumber(res, "tgisconstraint");
		i_tgconstrname = PQfnumber(res, "tgconstrname");
		i_tgconstrrelid = PQfnumber(res, "tgconstrrelid");
		i_tgconstrrelname = PQfnumber(res, "tgconstrrelname");
		i_tgenabled = PQfnumber(res, "tgenabled");
		i_tgdeferrable = PQfnumber(res, "tgdeferrable");
		i_tginitdeferred = PQfnumber(res, "tginitdeferred");

		tginfo = (TriggerInfo *) malloc(ntups * sizeof(TriggerInfo));

		for (j = 0; j < ntups; j++)
		{
			tginfo[j].dobj.objType = DO_TRIGGER;
			tginfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_tableoid));
			tginfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
			AssignDumpId(&tginfo[j].dobj);
			tginfo[j].dobj.name = strdup(PQgetvalue(res, j, i_tgname));
			tginfo[j].dobj.namespace = tbinfo->dobj.namespace;
			tginfo[j].tgtable = tbinfo;
			tginfo[j].tgfname = strdup(PQgetvalue(res, j, i_tgfname));
			tginfo[j].tgtype = atoi(PQgetvalue(res, j, i_tgtype));
			tginfo[j].tgnargs = atoi(PQgetvalue(res, j, i_tgnargs));
			tginfo[j].tgargs = strdup(PQgetvalue(res, j, i_tgargs));
			tginfo[j].tgisconstraint = *(PQgetvalue(res, j, i_tgisconstraint)) == 't';
			tginfo[j].tgenabled = *(PQgetvalue(res, j, i_tgenabled));
			tginfo[j].tgdeferrable = *(PQgetvalue(res, j, i_tgdeferrable)) == 't';
			tginfo[j].tginitdeferred = *(PQgetvalue(res, j, i_tginitdeferred)) == 't';

			if (tginfo[j].tgisconstraint)
			{
				tginfo[j].tgconstrname = strdup(PQgetvalue(res, j, i_tgconstrname));
				tginfo[j].tgconstrrelid = atooid(PQgetvalue(res, j, i_tgconstrrelid));
				if (OidIsValid(tginfo[j].tgconstrrelid))
				{
					if (PQgetisnull(res, j, i_tgconstrrelname))
					{
						write_msg(NULL, "query produced null referenced table name for foreign key trigger \"%s\" on table \"%s\" (OID of table: %u)\n",
								  tginfo[j].dobj.name, tbinfo->dobj.name,
								  tginfo[j].tgconstrrelid);
						exit_nicely();
					}
					tginfo[j].tgconstrrelname = strdup(PQgetvalue(res, j, i_tgconstrrelname));
				}
				else
					tginfo[j].tgconstrrelname = NULL;
			}
			else
			{
				tginfo[j].tgconstrname = NULL;
				tginfo[j].tgconstrrelid = InvalidOid;
				tginfo[j].tgconstrrelname = NULL;
			}
		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
}

/*
 * getProcLangs
 *	  get basic information about every procedural language in the system
 *
 * numProcLangs is set to the number of langs read in
 *
 * NB: this must run after getFuncs() because we assume we can do
 * findFuncByOid().
 */
ProcLangInfo *
getProcLangs(int *numProcLangs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	ProcLangInfo *planginfo;
	int			i_tableoid;
	int			i_oid;
	int			i_lanname;
	int			i_lanpltrusted;
	int			i_lanplcallfoid;
	int			i_laninline;
	int			i_lanvalidator;
	int			i_lanacl;
	int			i_lanowner;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	/*
	 * The laninline column was added in upstream 90000 but was backported to
	 * Greenplum 5, so the check needs to go further back than 90000.
	 */
	if (g_fout->remoteVersion >= 80300)
	{
		/* pg_language has a laninline column */
		/* pg_language has a lanowner column */
		appendPQExpBuffer(query, "SELECT tableoid, oid, "
						  "lanname, lanpltrusted, lanplcallfoid, "
						  "laninline, lanvalidator, lanacl, "
						  "(%s lanowner) AS lanowner "
						  "FROM pg_language "
						  "WHERE lanispl "
						  "ORDER BY oid",
						  username_subquery);
	}
	else
	{
		/* Languages are owned by the bootstrap superuser, OID 10 */
		appendPQExpBuffer(query, "SELECT tableoid, oid, *, "
						  "(%s '10') as lanowner "
						  "FROM pg_language "
						  "WHERE lanispl "
						  "ORDER BY oid",
						  username_subquery);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numProcLangs = ntups;

	planginfo = (ProcLangInfo *) malloc(ntups * sizeof(ProcLangInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_lanname = PQfnumber(res, "lanname");
	i_lanpltrusted = PQfnumber(res, "lanpltrusted");
	i_lanplcallfoid = PQfnumber(res, "lanplcallfoid");
	/* these may fail and return -1: */
	i_laninline = PQfnumber(res, "laninline");
	i_lanvalidator = PQfnumber(res, "lanvalidator");
	i_lanacl = PQfnumber(res, "lanacl");
	i_lanowner = PQfnumber(res, "lanowner");

	for (i = 0; i < ntups; i++)
	{
		planginfo[i].dobj.objType = DO_PROCLANG;
		planginfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		planginfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&planginfo[i].dobj);

		planginfo[i].dobj.name = strdup(PQgetvalue(res, i, i_lanname));
		planginfo[i].lanpltrusted = *(PQgetvalue(res, i, i_lanpltrusted)) == 't';
		planginfo[i].lanplcallfoid = atooid(PQgetvalue(res, i, i_lanplcallfoid));
		if (i_laninline >= 0)
			planginfo[i].laninline = atooid(PQgetvalue(res, i, i_laninline));
		else
			planginfo[i].laninline = InvalidOid;
		if (i_lanvalidator >= 0)
			planginfo[i].lanvalidator = atooid(PQgetvalue(res, i, i_lanvalidator));
		else
			planginfo[i].lanvalidator = InvalidOid;
		if (i_lanacl >= 0)
			planginfo[i].lanacl = strdup(PQgetvalue(res, i, i_lanacl));
		else
			planginfo[i].lanacl = strdup("{=U}");
		if (i_lanowner >= 0)
			planginfo[i].lanowner = strdup(PQgetvalue(res, i, i_lanowner));
		else
			planginfo[i].lanowner = strdup("");

		/* Decide whether we want to dump it */
		selectDumpableProcLang(&(planginfo[i]));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return planginfo;
}

/*
 * getCasts
 *	  get basic information about every cast in the system
 *
 * numCasts is set to the number of casts read in
 */
CastInfo *
getCasts(int *numCasts)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	CastInfo   *castinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_castsource;
	int			i_casttarget;
	int			i_castfunc;
	int			i_castcontext;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(query, "SELECT tableoid, oid, "
					  "castsource, casttarget, castfunc, castcontext "
					  "FROM pg_cast ORDER BY 3,4");

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numCasts = ntups;

	castinfo = (CastInfo *) malloc(ntups * sizeof(CastInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_castsource = PQfnumber(res, "castsource");
	i_casttarget = PQfnumber(res, "casttarget");
	i_castfunc = PQfnumber(res, "castfunc");
	i_castcontext = PQfnumber(res, "castcontext");

	for (i = 0; i < ntups; i++)
	{
		PQExpBufferData namebuf;
		TypeInfo   *sTypeInfo;
		TypeInfo   *tTypeInfo;

		castinfo[i].dobj.objType = DO_CAST;
		castinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		castinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&castinfo[i].dobj);
		castinfo[i].castsource = atooid(PQgetvalue(res, i, i_castsource));
		castinfo[i].casttarget = atooid(PQgetvalue(res, i, i_casttarget));
		castinfo[i].castfunc = atooid(PQgetvalue(res, i, i_castfunc));
		castinfo[i].castcontext = *(PQgetvalue(res, i, i_castcontext));

		/*
		 * Try to name cast as concatenation of typnames.  This is only used
		 * for purposes of sorting.  If we fail to find either type, the name
		 * will be an empty string.
		 */
		initPQExpBuffer(&namebuf);
		sTypeInfo = findTypeByOid(castinfo[i].castsource);
		tTypeInfo = findTypeByOid(castinfo[i].casttarget);
		if (sTypeInfo && tTypeInfo)
			appendPQExpBuffer(&namebuf, "%s %s",
							  sTypeInfo->dobj.name, tTypeInfo->dobj.name);
		castinfo[i].dobj.name = namebuf.data;

		if (OidIsValid(castinfo[i].castfunc))
		{
			/*
			 * We need to make a dependency to ensure the function will be
			 * dumped first.  (In 7.3 and later the regular dependency
			 * mechanism will handle this for us.)
			 */
			FuncInfo   *funcInfo;

			funcInfo = findFuncByOid(castinfo[i].castfunc);
			if (funcInfo)
				addObjectDependency(&castinfo[i].dobj,
									funcInfo->dobj.dumpId);
		}

		/* Decide whether we want to dump it */
		selectDumpableCast(&(castinfo[i]));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return castinfo;
}

/*
 * getTableAttrs -
 *	  for each interesting table, read info about its attributes
 *	  (names, types, default values, CHECK constraints, etc)
 *
 * This is implemented in a very inefficient way right now, looping
 * through the tblinfo and doing a join per table to find the attrs and their
 * types.  However, because we want type names and so forth to be named
 * relative to the schema of each table, we couldn't do it in just one
 * query.  (Maybe one query per schema?)
 *
 *	modifies tblinfo
 */
void
getTableAttrs(TableInfo *tblinfo, int numTables)
{
	int			i,
				j;
	PQExpBuffer q = createPQExpBuffer();
	int			i_attnum;
	int			i_attname;
	int			i_atttypname;
	int			i_atttypmod;
	int			i_attstattarget;
	int			i_attstorage;
	int			i_typstorage;
	int			i_attnotnull;
	int			i_atthasdef;
	int			i_attisdropped;
	int			i_attlen;
	int			i_attalign;
	int			i_attislocal;
	int			i_attndims;
	int			i_attbyval;
	int			i_attencoding;
	PGresult   *res;
	int			ntups;
	bool		hasdefaults;

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		/* Don't bother to collect info for sequences */
		if (tbinfo->relkind == RELKIND_SEQUENCE)
			continue;

		/* Don't bother with uninteresting tables, either */
		if (!tbinfo->interesting)
			continue;

		/*
		 * Make sure we are in proper schema for this table; this allows
		 * correct retrieval of formatted type names and default exprs
		 */
		selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

		/* find all the user attributes and their types */

		/*
		 * we must read the attribute names in attribute number order! because
		 * we will use the attnum to index into the attnames array later.  We
		 * actually ask to order by "attrelid, attnum" because (at least up to
		 * 7.3) the planner is not smart enough to realize it needn't re-sort
		 * the output of an indexscan on pg_attribute_relid_attnum_index.
		 */
		if (g_verbose)
			write_msg(NULL, "finding the columns and types of table \"%s\"\n",
					  tbinfo->dobj.name);

		resetPQExpBuffer(q);

		/* need left join here to not fail on dropped columns ... */
		appendPQExpBuffer(q, "SELECT a.attnum, a.attname, a.atttypmod, a.attstattarget, a.attstorage, t.typstorage, "
						  "a.attlen, a.attndims, a.attbyval, a.attalign, "		/* Added for dropped
																				 * column reconstruction */
				  "a.attnotnull, a.atthasdef, a.attisdropped, "
						  "a.attlen, a.attalign, a.attislocal, "
				   "pg_catalog.format_type(t.oid,a.atttypmod) as atttypname ");
		if (gp_attribute_encoding_available)
			appendPQExpBuffer(q, ", pg_catalog.array_to_string(e.attoptions, ',') as attencoding ");
		appendPQExpBuffer(q, "from pg_catalog.pg_attribute a left join pg_catalog.pg_type t "
						  "on a.atttypid = t.oid ");
		if (gp_attribute_encoding_available)
			appendPQExpBuffer(q, "	 LEFT OUTER JOIN pg_catalog.pg_attribute_encoding e ON e.attrelid = a.attrelid AND e.attnum = a.attnum ");
		appendPQExpBuffer(q, "where a.attrelid = '%u'::pg_catalog.oid "
						  "and a.attnum > 0::pg_catalog.int2 "
						  "order by a.attrelid, a.attnum",
						  tbinfo->dobj.catId.oid);

		res = PQexec(g_conn, q->data);
		check_sql_result(res, g_conn, q->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);

		i_attnum = PQfnumber(res, "attnum");
		i_attname = PQfnumber(res, "attname");
		i_atttypname = PQfnumber(res, "atttypname");
		i_atttypmod = PQfnumber(res, "atttypmod");
		i_attstattarget = PQfnumber(res, "attstattarget");
		i_attstorage = PQfnumber(res, "attstorage");
		i_typstorage = PQfnumber(res, "typstorage");
		i_attnotnull = PQfnumber(res, "attnotnull");
		i_atthasdef = PQfnumber(res, "atthasdef");
		i_attisdropped = PQfnumber(res, "attisdropped");
		i_attlen = PQfnumber(res, "attlen");
		i_attalign = PQfnumber(res, "attalign");
		i_attislocal = PQfnumber(res, "attislocal");
		i_attlen = PQfnumber(res, "attlen");
		i_attndims = PQfnumber(res, "attndims");
		i_attbyval = PQfnumber(res, "attbyval");
		i_attalign = PQfnumber(res, "attalign");
		i_attencoding = PQfnumber(res, "attencoding");

		tbinfo->numatts = ntups;
		tbinfo->attnames = (char **) malloc(ntups * sizeof(char *));
		tbinfo->atttypnames = (char **) malloc(ntups * sizeof(char *));
		tbinfo->atttypmod = (int *) malloc(ntups * sizeof(int));
		tbinfo->attstattarget = (int *) malloc(ntups * sizeof(int));
		tbinfo->attstorage = (char *) malloc(ntups * sizeof(char));
		tbinfo->typstorage = (char *) malloc(ntups * sizeof(char));
		tbinfo->attisdropped = (bool *) malloc(ntups * sizeof(bool));
		tbinfo->attlen = (int *) malloc(ntups * sizeof(int));
		tbinfo->attalign = (char *) malloc(ntups * sizeof(char));
		tbinfo->attislocal = (bool *) malloc(ntups * sizeof(bool));
		tbinfo->notnull = (bool *) malloc(ntups * sizeof(bool));
		tbinfo->inhNotNull = (bool *) malloc(ntups * sizeof(bool));
		tbinfo->attencoding = (char **) malloc(ntups * sizeof(char *));
		tbinfo->attrdefs = (AttrDefInfo **) malloc(ntups * sizeof(AttrDefInfo *));
		hasdefaults = false;

		for (j = 0; j < ntups; j++)
		{
			if (j + 1 != atoi(PQgetvalue(res, j, i_attnum)))
			{
				write_msg(NULL, "invalid column numbering in table \"%s\"\n",
						  tbinfo->dobj.name);
				exit_nicely();
			}
			tbinfo->attnames[j] = strdup(PQgetvalue(res, j, i_attname));
			tbinfo->atttypnames[j] = strdup(PQgetvalue(res, j, i_atttypname));
			tbinfo->atttypmod[j] = atoi(PQgetvalue(res, j, i_atttypmod));
			tbinfo->attstattarget[j] = atoi(PQgetvalue(res, j, i_attstattarget));
			tbinfo->attstorage[j] = *(PQgetvalue(res, j, i_attstorage));
			tbinfo->typstorage[j] = *(PQgetvalue(res, j, i_typstorage));
			tbinfo->attisdropped[j] = (PQgetvalue(res, j, i_attisdropped)[0] == 't');
			tbinfo->attlen[j] = atoi(PQgetvalue(res, j, i_attlen));
			tbinfo->attalign[j] = *(PQgetvalue(res, j, i_attalign));
			tbinfo->attislocal[j] = (PQgetvalue(res, j, i_attislocal)[0] == 't');
			tbinfo->notnull[j] = (PQgetvalue(res, j, i_attnotnull)[0] == 't');
			tbinfo->attrdefs[j] = NULL; /* fix below */
			if (PQgetvalue(res, j, i_atthasdef)[0] == 't')
				hasdefaults = true;

			/* these flags will be set in flagInhAttrs() */
			tbinfo->inhNotNull[j] = false;

			/* column storage attributes */
			if (gp_attribute_encoding_available && !PQgetisnull(res, j, i_attencoding))
				tbinfo->attencoding[j] = strdup(PQgetvalue(res, j, i_attencoding));
			else
				tbinfo->attencoding[j] = NULL;

			/*
			 * External table doesn't support inheritance so ensure that all
			 * attributes are marked as local.  Applicable to partitioned
			 * tables where a partition is exchanged for an external table.
			 */
			if (tbinfo->relstorage == RELSTORAGE_EXTERNAL && tbinfo->attislocal[j])
				tbinfo->attislocal[j] = false;
		}

		PQclear(res);

		/*
		 * Get info about column defaults
		 */
		if (hasdefaults)
		{
			AttrDefInfo *attrdefs;
			int			numDefaults;

			if (g_verbose)
				write_msg(NULL, "finding default expressions of table \"%s\"\n",
						  tbinfo->dobj.name);

			resetPQExpBuffer(q);

			appendPQExpBuffer(q, "SELECT tableoid, oid, adnum, "
						   "pg_catalog.pg_get_expr(adbin, adrelid) AS adsrc "
							  "FROM pg_catalog.pg_attrdef "
							  "WHERE adrelid = '%u'::pg_catalog.oid",
							  tbinfo->dobj.catId.oid);

			res = PQexec(g_conn, q->data);
			check_sql_result(res, g_conn, q->data, PGRES_TUPLES_OK);

			numDefaults = PQntuples(res);
			attrdefs = (AttrDefInfo *) malloc(numDefaults * sizeof(AttrDefInfo));

			for (j = 0; j < numDefaults; j++)
			{
				int			adnum;

				adnum = atoi(PQgetvalue(res, j, 2));

				if (adnum <= 0 || adnum > ntups)
				{
					write_msg(NULL, "invalid adnum value %d for table \"%s\"\n",
							  adnum, tbinfo->dobj.name);
					exit_nicely();
				}

				/*
				 * dropped columns shouldn't have defaults, but just in case,
				 * ignore 'em
				 */
				if (tbinfo->attisdropped[adnum - 1])
					continue;

				attrdefs[j].dobj.objType = DO_ATTRDEF;
				attrdefs[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, 0));
				attrdefs[j].dobj.catId.oid = atooid(PQgetvalue(res, j, 1));
				AssignDumpId(&attrdefs[j].dobj);
				attrdefs[j].adtable = tbinfo;
				attrdefs[j].adnum = adnum;
				attrdefs[j].adef_expr = strdup(PQgetvalue(res, j, 3));

				attrdefs[j].dobj.name = strdup(tbinfo->dobj.name);
				attrdefs[j].dobj.namespace = tbinfo->dobj.namespace;

				attrdefs[j].dobj.dump = tbinfo->dobj.dump;

				/*
				 * Defaults on a VIEW must always be dumped as separate ALTER
				 * TABLE commands.	Defaults on regular tables are dumped as
				 * part of the CREATE TABLE if possible, which it won't be
				 * if the column is not going to be emitted explicitly.
				 */
				if (tbinfo->relkind == RELKIND_VIEW)
				{
					attrdefs[j].separate = true;
					/* needed in case pre-7.3 DB: */
					addObjectDependency(&attrdefs[j].dobj,
										tbinfo->dobj.dumpId);
				}
				else if (!shouldPrintColumn(tbinfo, adnum - 1))
				{
					/* column will be suppressed, print default separately */
					attrdefs[j].separate = true;
					/* needed in case pre-7.3 DB: */
					addObjectDependency(&attrdefs[j].dobj,
										tbinfo->dobj.dumpId);
				}
				else
				{
					attrdefs[j].separate = false;
					/*
					 * Mark the default as needing to appear before the table,
					 * so that any dependencies it has must be emitted before
					 * the CREATE TABLE.  If this is not possible, we'll
					 * change to "separate" mode while sorting dependencies.
					 */
					addObjectDependency(&tbinfo->dobj,
										attrdefs[j].dobj.dumpId);
				}

				tbinfo->attrdefs[adnum - 1] = &attrdefs[j];
			}
			PQclear(res);
		}

		/*
		 * Get info about table CHECK constraints
		 */
		if (tbinfo->ncheck > 0)
		{
			ConstraintInfo *constrs;
			int			numConstrs;

			if (g_verbose)
				write_msg(NULL, "finding check constraints for table \"%s\"\n",
						  tbinfo->dobj.name);

			resetPQExpBuffer(q);

			appendPQExpBuffer(q, "SELECT tableoid, oid, conname, "
							"pg_catalog.pg_get_constraintdef(oid) AS consrc "
							  "FROM pg_catalog.pg_constraint "
							  "WHERE conrelid = '%u'::pg_catalog.oid "
							  "   AND contype = 'c' "
							  "ORDER BY conname",
							  tbinfo->dobj.catId.oid);

			res = PQexec(g_conn, q->data);
			check_sql_result(res, g_conn, q->data, PGRES_TUPLES_OK);

			numConstrs = PQntuples(res);
			if (numConstrs != tbinfo->ncheck)
			{
				write_msg(NULL, "expected %d check constraints on table \"%s\" but found %d\n",
						  tbinfo->ncheck, tbinfo->dobj.name, numConstrs);
				write_msg(NULL, "(The system catalogs might be corrupted.)\n");
				exit_nicely();
			}

			constrs = (ConstraintInfo *) malloc(numConstrs * sizeof(ConstraintInfo));
			tbinfo->checkexprs = constrs;

			for (j = 0; j < numConstrs; j++)
			{
				constrs[j].dobj.objType = DO_CONSTRAINT;
				constrs[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, 0));
				constrs[j].dobj.catId.oid = atooid(PQgetvalue(res, j, 1));
				AssignDumpId(&constrs[j].dobj);
				constrs[j].dobj.name = strdup(PQgetvalue(res, j, 2));
				constrs[j].dobj.namespace = tbinfo->dobj.namespace;
				constrs[j].contable = tbinfo;
				constrs[j].condomain = NULL;
				constrs[j].contype = 'c';
				constrs[j].condef = strdup(PQgetvalue(res, j, 3));
				constrs[j].conindex = 0;
				constrs[j].coninherited = false;
				constrs[j].separate = false;

				constrs[j].dobj.dump = tbinfo->dobj.dump;

				/*
				 * Mark the constraint as needing to appear before the table
				 * --- this is so that any other dependencies of the
				 * constraint will be emitted before we try to create the
				 * table.
				 */
				addObjectDependency(&tbinfo->dobj,
									constrs[j].dobj.dumpId);

				/*
				 * If the constraint is inherited, this will be detected
				 * later.  We also detect later if the constraint must be
				 * split out from the table definition.
				 */
			}
			PQclear(res);
		}
	}

	destroyPQExpBuffer(q);
}

/*
 * Test whether a column should be printed as part of table's CREATE TABLE.
 * Column number is zero-based.
 *
 * Normally this is always true, but it's false for dropped columns, as well
 * as those that were inherited without any local definition.  (If we print
 * such a column it will mistakenly get pg_attribute.attislocal set to true.)
 *
 * This function exists because there are scattered nonobvious places that
 * must be kept in sync with this decision.
 */
bool
shouldPrintColumn(TableInfo *tbinfo, int colno)
{
	return (((tbinfo->attislocal[colno] || tbinfo->relstorage == RELSTORAGE_EXTERNAL) &&
			!tbinfo->attisdropped[colno]) || binary_upgrade);
}


/*
 * getTSParsers:
 *	  read all text search parsers in the system catalogs and return them
 *	  in the TSParserInfo* structure
 *
 *	numTSParsers is set to the number of parsers read in
 */
TSParserInfo *
getTSParsers(int *numTSParsers)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	TSParserInfo *prsinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_prsname;
	int			i_prsnamespace;
	int			i_prsstart;
	int			i_prstoken;
	int			i_prsend;
	int			i_prsheadline;
	int			i_prslextype;

	/* Before 8.3, there is no built-in text search support */
	if (g_fout->remoteVersion < 80300)
	{
		*numTSParsers = 0;
		return NULL;
	}

	/*
	 * find all text search objects, including builtin ones; we filter out
	 * system-defined objects at dump-out time.
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(query, "SELECT tableoid, oid, prsname, prsnamespace, "
					  "prsstart::oid, prstoken::oid, "
					  "prsend::oid, prsheadline::oid, prslextype::oid "
					  "FROM pg_ts_parser");

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numTSParsers = ntups;

	prsinfo = (TSParserInfo *) malloc(ntups * sizeof(TSParserInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_prsname = PQfnumber(res, "prsname");
	i_prsnamespace = PQfnumber(res, "prsnamespace");
	i_prsstart = PQfnumber(res, "prsstart");
	i_prstoken = PQfnumber(res, "prstoken");
	i_prsend = PQfnumber(res, "prsend");
	i_prsheadline = PQfnumber(res, "prsheadline");
	i_prslextype = PQfnumber(res, "prslextype");

	for (i = 0; i < ntups; i++)
	{
		prsinfo[i].dobj.objType = DO_TSPARSER;
		prsinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		prsinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&prsinfo[i].dobj);
		prsinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_prsname));
		prsinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_prsnamespace)),
												  prsinfo[i].dobj.catId.oid);
		prsinfo[i].prsstart = atooid(PQgetvalue(res, i, i_prsstart));
		prsinfo[i].prstoken = atooid(PQgetvalue(res, i, i_prstoken));
		prsinfo[i].prsend = atooid(PQgetvalue(res, i, i_prsend));
		prsinfo[i].prsheadline = atooid(PQgetvalue(res, i, i_prsheadline));
		prsinfo[i].prslextype = atooid(PQgetvalue(res, i, i_prslextype));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(prsinfo[i].dobj));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return prsinfo;
}

/*
 * getTSDictionaries:
 *	  read all text search dictionaries in the system catalogs and return them
 *	  in the TSDictInfo* structure
 *
 *	numTSDicts is set to the number of dictionaries read in
 */
TSDictInfo *
getTSDictionaries(int *numTSDicts)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	TSDictInfo *dictinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_dictname;
	int			i_dictnamespace;
	int			i_rolname;
	int			i_dicttemplate;
	int			i_dictinitoption;

	/* Before 8.3, there is no built-in text search support */
	if (g_fout->remoteVersion < 80300)
	{
		*numTSDicts = 0;
		return NULL;
	}

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(query, "SELECT tableoid, oid, dictname, "
					  "dictnamespace, (%s dictowner) as rolname, "
					  "dicttemplate, dictinitoption "
					  "FROM pg_ts_dict",
					  username_subquery);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numTSDicts = ntups;

	dictinfo = (TSDictInfo *) malloc(ntups * sizeof(TSDictInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_dictname = PQfnumber(res, "dictname");
	i_dictnamespace = PQfnumber(res, "dictnamespace");
	i_rolname = PQfnumber(res, "rolname");
	i_dictinitoption = PQfnumber(res, "dictinitoption");
	i_dicttemplate = PQfnumber(res, "dicttemplate");

	for (i = 0; i < ntups; i++)
	{
		dictinfo[i].dobj.objType = DO_TSDICT;
		dictinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		dictinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&dictinfo[i].dobj);
		dictinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_dictname));
		dictinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_dictnamespace)),
												 dictinfo[i].dobj.catId.oid);
		dictinfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));
		dictinfo[i].dicttemplate = atooid(PQgetvalue(res, i, i_dicttemplate));
		if (PQgetisnull(res, i, i_dictinitoption))
			dictinfo[i].dictinitoption = NULL;
		else
			dictinfo[i].dictinitoption = strdup(PQgetvalue(res, i, i_dictinitoption));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(dictinfo[i].dobj));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return dictinfo;
}

/*
 * getTSTemplates:
 *	  read all text search templates in the system catalogs and return them
 *	  in the TSTemplateInfo* structure
 *
 *	numTSTemplates is set to the number of templates read in
 */
TSTemplateInfo *
getTSTemplates(int *numTSTemplates)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	TSTemplateInfo *tmplinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_tmplname;
	int			i_tmplnamespace;
	int			i_tmplinit;
	int			i_tmpllexize;

	/* Before 8.3, there is no built-in text search support */
	if (g_fout->remoteVersion < 80300)
	{
		*numTSTemplates = 0;
		return NULL;
	}

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(query, "SELECT tableoid, oid, tmplname, "
					  "tmplnamespace, tmplinit::oid, tmpllexize::oid "
					  "FROM pg_ts_template");

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numTSTemplates = ntups;

	tmplinfo = (TSTemplateInfo *) malloc(ntups * sizeof(TSTemplateInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_tmplname = PQfnumber(res, "tmplname");
	i_tmplnamespace = PQfnumber(res, "tmplnamespace");
	i_tmplinit = PQfnumber(res, "tmplinit");
	i_tmpllexize = PQfnumber(res, "tmpllexize");

	for (i = 0; i < ntups; i++)
	{
		tmplinfo[i].dobj.objType = DO_TSTEMPLATE;
		tmplinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		tmplinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&tmplinfo[i].dobj);
		tmplinfo[i].dobj.name = strdup(PQgetvalue(res, i, i_tmplname));
		tmplinfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_tmplnamespace)),
												 tmplinfo[i].dobj.catId.oid);
		tmplinfo[i].tmplinit = atooid(PQgetvalue(res, i, i_tmplinit));
		tmplinfo[i].tmpllexize = atooid(PQgetvalue(res, i, i_tmpllexize));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(tmplinfo[i].dobj));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return tmplinfo;
}

/*
 * getTSConfigurations:
 *	  read all text search configurations in the system catalogs and return
 *	  them in the TSConfigInfo* structure
 *
 *	numTSConfigs is set to the number of configurations read in
 */
TSConfigInfo *
getTSConfigurations(int *numTSConfigs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	TSConfigInfo *cfginfo;
	int			i_tableoid;
	int			i_oid;
	int			i_cfgname;
	int			i_cfgnamespace;
	int			i_rolname;
	int			i_cfgparser;

	/* Before 8.3, there is no built-in text search support */
	if (g_fout->remoteVersion < 80300)
	{
		*numTSConfigs = 0;
		return NULL;
	}

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(query, "SELECT tableoid, oid, cfgname, "
					  "cfgnamespace, (%s cfgowner) as rolname, cfgparser "
					  "FROM pg_ts_config",
					  username_subquery);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numTSConfigs = ntups;

	cfginfo = (TSConfigInfo *) malloc(ntups * sizeof(TSConfigInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_cfgname = PQfnumber(res, "cfgname");
	i_cfgnamespace = PQfnumber(res, "cfgnamespace");
	i_rolname = PQfnumber(res, "rolname");
	i_cfgparser = PQfnumber(res, "cfgparser");

	for (i = 0; i < ntups; i++)
	{
		cfginfo[i].dobj.objType = DO_TSCONFIG;
		cfginfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		cfginfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&cfginfo[i].dobj);
		cfginfo[i].dobj.name = strdup(PQgetvalue(res, i, i_cfgname));
		cfginfo[i].dobj.namespace = findNamespace(atooid(PQgetvalue(res, i, i_cfgnamespace)),
												  cfginfo[i].dobj.catId.oid);
		cfginfo[i].rolname = strdup(PQgetvalue(res, i, i_rolname));
		cfginfo[i].cfgparser = atooid(PQgetvalue(res, i, i_cfgparser));

		/* Decide whether we want to dump it */
		selectDumpableObject(&(cfginfo[i].dobj));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return cfginfo;
}


/*
 * dumpComment --
 *
 * This routine is used to dump any comments associated with the
 * object handed to this routine. The routine takes a constant character
 * string for the target part of the comment-creation command, plus
 * the namespace and owner of the object (for labeling the ArchiveEntry),
 * plus catalog ID and subid which are the lookup key for pg_description,
 * plus the dump ID for the object (for setting a dependency).
 * If a matching pg_description entry is found, it is dumped.
 *
 * Note: although this routine takes a dumpId for dependency purposes,
 * that purpose is just to mark the dependency in the emitted dump file
 * for possible future use by pg_restore.  We do NOT use it for determining
 * ordering of the comment in the dump file, because this routine is called
 * after dependency sorting occurs.  This routine should be called just after
 * calling ArchiveEntry() for the specified object.
 */
static void
dumpComment(Archive *fout, const char *target,
			const char *namespace, const char *owner,
			CatalogId catalogId, int subid, DumpId dumpId)
{
	CommentItem *comments;
	int			ncomments;

	/* Comments are SCHEMA not data */
	if (dataOnly)
		return;

	/* Search for comments associated with catalogId, using table */
	ncomments = findComments(fout, catalogId.tableoid, catalogId.oid,
							 &comments);

	/* Is there one matching the subid? */
	while (ncomments > 0)
	{
		if (comments->objsubid == subid)
			break;
		comments++;
		ncomments--;
	}

	/* If a comment exists, build COMMENT ON statement */
	if (ncomments > 0)
	{
		PQExpBuffer query = createPQExpBuffer();

		appendPQExpBuffer(query, "COMMENT ON %s IS ", target);
		appendStringLiteralAH(query, comments->descr, fout);
		appendPQExpBuffer(query, ";\n");

		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 target, namespace, NULL, owner, false,
					 "COMMENT", query->data, "", NULL,
					 &(dumpId), 1,
					 NULL, NULL);

		destroyPQExpBuffer(query);
	}
}

/*
 * dumpTableComment --
 *
 * As above, but dump comments for both the specified table (or view)
 * and its columns.
 */
static void
dumpTableComment(Archive *fout, TableInfo *tbinfo,
				 const char *reltypename)
{
	CommentItem *comments;
	int			ncomments;
	PQExpBuffer query;
	PQExpBuffer target;

	/* Comments are SCHEMA not data */
	if (dataOnly)
		return;

	/* Search for comments associated with relation, using table */
	ncomments = findComments(fout,
							 tbinfo->dobj.catId.tableoid,
							 tbinfo->dobj.catId.oid,
							 &comments);

	/* If comments exist, build COMMENT ON statements */
	if (ncomments <= 0)
		return;

	query = createPQExpBuffer();
	target = createPQExpBuffer();

	while (ncomments > 0)
	{
		const char *descr = comments->descr;
		int			objsubid = comments->objsubid;

		if (objsubid == 0)
		{
			resetPQExpBuffer(target);
			if (strcmp(reltypename, "EXTERNAL TABLE") == 0)
				reltypename = "TABLE";
			appendPQExpBuffer(target, "%s %s.", reltypename,
							  fmtId(tbinfo->dobj.namespace->dobj.name));
			appendPQExpBuffer(target, "%s ", fmtId(tbinfo->dobj.name));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON %s IS ", target->data);
			appendStringLiteralAH(query, descr, fout);
			appendPQExpBuffer(query, ";\n");

			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 target->data,
						 tbinfo->dobj.namespace->dobj.name,
						 NULL,
						 tbinfo->rolname,
						 false, "COMMENT", query->data, "", NULL,
						 &(tbinfo->dobj.dumpId), 1,
						 NULL, NULL);
		}
		else if (objsubid > 0 && objsubid <= tbinfo->numatts)
		{
			resetPQExpBuffer(target);
			appendPQExpBuffer(target, "COLUMN %s.",
							  fmtId(tbinfo->dobj.name));
			appendPQExpBuffer(target, "%s",
							  fmtId(tbinfo->attnames[objsubid - 1]));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON %s IS ", target->data);
			appendStringLiteralAH(query, descr, fout);
			appendPQExpBuffer(query, ";\n");

			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 target->data,
						 tbinfo->dobj.namespace->dobj.name,
						 NULL,
						 tbinfo->rolname,
						 false, "COMMENT", query->data, "", NULL,
						 &(tbinfo->dobj.dumpId), 1,
						 NULL, NULL);
		}

		comments++;
		ncomments--;
	}

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(target);
}

/*
 * findComments --
 *
 * Find the comment(s), if any, associated with the given object.  All the
 * objsubid values associated with the given classoid/objoid are found with
 * one search.
 */
static int
findComments(Archive *fout, Oid classoid, Oid objoid,
			 CommentItem **items)
{
	/* static storage for table of comments */
	static CommentItem *comments = NULL;
	static int	ncomments = -1;

	CommentItem *middle = NULL;
	CommentItem *low;
	CommentItem *high;
	int			nmatch;

	/* Get comments if we didn't already */
	if (ncomments < 0)
		ncomments = collectComments(fout, &comments);

	/*
	 * Do binary search to find some item matching the object.
	 */
	low = &comments[0];
	high = &comments[ncomments - 1];
	while (low <= high)
	{
		middle = low + (high - low) / 2;

		if (classoid < middle->classoid)
			high = middle - 1;
		else if (classoid > middle->classoid)
			low = middle + 1;
		else if (objoid < middle->objoid)
			high = middle - 1;
		else if (objoid > middle->objoid)
			low = middle + 1;
		else
			break;				/* found a match */
	}

	if (low > high)				/* no matches */
	{
		*items = NULL;
		return 0;
	}

	/*
	 * Now determine how many items match the object.  The search loop
	 * invariant still holds: only items between low and high inclusive could
	 * match.
	 */
	nmatch = 1;
	while (middle > low)
	{
		if (classoid != middle[-1].classoid ||
			objoid != middle[-1].objoid)
			break;
		middle--;
		nmatch++;
	}

	*items = middle;

	middle += nmatch;
	while (middle <= high)
	{
		if (classoid != middle->classoid ||
			objoid != middle->objoid)
			break;
		middle++;
		nmatch++;
	}

	return nmatch;
}

/*
 * collectComments --
 *
 * Construct a table of all comments available for database objects.
 * We used to do per-object queries for the comments, but it's much faster
 * to pull them all over at once, and on most databases the memory cost
 * isn't high.
 *
 * The table is sorted by classoid/objid/objsubid for speed in lookup.
 */
static int
collectComments(Archive *fout, CommentItem **items)
{
	PGresult   *res;
	PQExpBuffer query;
	int			i_description;
	int			i_classoid;
	int			i_objoid;
	int			i_objsubid;
	int			ntups;
	int			i;
	CommentItem *comments;

	/*
	 * Note we do NOT change source schema here; preserve the caller's
	 * setting, instead.
	 */

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "SELECT description, classoid, objoid, objsubid "
					  "FROM pg_catalog.pg_description "
					  "ORDER BY classoid, objoid, objsubid");

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Construct lookup table containing OIDs in numeric form */

	i_description = PQfnumber(res, "description");
	i_classoid = PQfnumber(res, "classoid");
	i_objoid = PQfnumber(res, "objoid");
	i_objsubid = PQfnumber(res, "objsubid");

	ntups = PQntuples(res);

	comments = (CommentItem *) malloc(ntups * sizeof(CommentItem));

	for (i = 0; i < ntups; i++)
	{
		comments[i].descr = PQgetvalue(res, i, i_description);
		comments[i].classoid = atooid(PQgetvalue(res, i, i_classoid));
		comments[i].objoid = atooid(PQgetvalue(res, i, i_objoid));
		comments[i].objsubid = atoi(PQgetvalue(res, i, i_objsubid));
	}

	/* Do NOT free the PGresult since we are keeping pointers into it */
	destroyPQExpBuffer(query);

	*items = comments;
	return ntups;
}

/*
 * dumpDumpableObject
 *
 * This routine and its subsidiaries are responsible for creating
 * ArchiveEntries (TOC objects) for each object to be dumped.
 */
static void
dumpDumpableObject(Archive *fout, DumpableObject *dobj)
{
	switch (dobj->objType)
	{
		case DO_NAMESPACE:
			if (!postDataSchemaOnly)
			dumpNamespace(fout, (NamespaceInfo *) dobj);
			break;
		case DO_EXTENSION:
			if (!postDataSchemaOnly)
			dumpExtension(fout, (ExtensionInfo *) dobj);
			break;
		case DO_TYPE:
			if (!postDataSchemaOnly)
			dumpType(fout, (TypeInfo *) dobj);
			break;
		case DO_TYPE_STORAGE_OPTIONS:
			if (!postDataSchemaOnly)
				dumpTypeStorageOptions(fout, (TypeStorageOptions *) dobj);
			break;
		case DO_SHELL_TYPE:
			if (!postDataSchemaOnly)
			dumpShellType(fout, (ShellTypeInfo *) dobj);
			break;
		case DO_FUNC:
			if (!postDataSchemaOnly)
			dumpFunc(fout, (FuncInfo *) dobj);
			break;
		case DO_AGG:
			if (!postDataSchemaOnly)
			dumpAgg(fout, (AggInfo *) dobj);
			break;
		case DO_EXTPROTOCOL:
			if (!postDataSchemaOnly)
			dumpExtProtocol(fout, (ExtProtInfo *) dobj);
			break;
		case DO_OPERATOR:
			if (!postDataSchemaOnly)
			dumpOpr(fout, (OprInfo *) dobj);
			break;
		case DO_OPCLASS:
			if (!postDataSchemaOnly)
			dumpOpclass(fout, (OpclassInfo *) dobj);
			break;
		case DO_OPFAMILY:
			dumpOpfamily(fout, (OpfamilyInfo *) dobj);
			break;
		case DO_CONVERSION:
			if (!postDataSchemaOnly)
			dumpConversion(fout, (ConvInfo *) dobj);
			break;
		case DO_TABLE:
			if (!postDataSchemaOnly)
			dumpTable(fout, (TableInfo *) dobj);
			break;
		case DO_ATTRDEF:
			if (!postDataSchemaOnly)
			dumpAttrDef(fout, (AttrDefInfo *) dobj);
			break;
		case DO_INDEX:
			if (!preDataSchemaOnly)
			dumpIndex(fout, (IndxInfo *) dobj);
			break;
		case DO_RULE:
			if (!preDataSchemaOnly)
			dumpRule(fout, (RuleInfo *) dobj);
			break;
		case DO_TRIGGER:
			dumpTrigger(fout, (TriggerInfo *) dobj);
			break;
		case DO_CONSTRAINT:
			if (!preDataSchemaOnly)
			dumpConstraint(fout, (ConstraintInfo *) dobj);
			break;
		case DO_FK_CONSTRAINT:
			if (!preDataSchemaOnly)
			dumpConstraint(fout, (ConstraintInfo *) dobj);
			break;
		case DO_PROCLANG:
			if (!postDataSchemaOnly)
			dumpProcLang(fout, (ProcLangInfo *) dobj);
			break;
		case DO_CAST:
			if (!postDataSchemaOnly)
			dumpCast(fout, (CastInfo *) dobj);
			break;
		case DO_TABLE_DATA:
			if (!postDataSchemaOnly)
			dumpTableData(fout, (TableDataInfo *) dobj);
			break;
		case DO_DUMMY_TYPE:
			/* table rowtypes and array types are never dumped separately */
			break;
		case DO_TSPARSER:
			dumpTSParser(fout, (TSParserInfo *) dobj);
			break;
		case DO_TSDICT:
			dumpTSDictionary(fout, (TSDictInfo *) dobj);
			break;
		case DO_TSTEMPLATE:
			dumpTSTemplate(fout, (TSTemplateInfo *) dobj);
			break;
		case DO_TSCONFIG:
			dumpTSConfig(fout, (TSConfigInfo *) dobj);
			break;
		case DO_BLOBS:
			if (!postDataSchemaOnly)
			ArchiveEntry(fout, dobj->catId, dobj->dumpId,
						 dobj->name, NULL, NULL, "",
						 false, "BLOBS", "", "", NULL,
						 NULL, 0,
						 dumpBlobs, NULL);
			break;
		case DO_BLOB_COMMENTS:
			if (!postDataSchemaOnly)
			ArchiveEntry(fout, dobj->catId, dobj->dumpId,
						 dobj->name, NULL, NULL, "",
						 false, "BLOB COMMENTS", "", "", NULL,
						 NULL, 0,
						 dumpBlobComments, NULL);
			break;
		/*
		 * The TYPE_CACHE object is only used for the pg_type cache during
		 * binary_upgrade operation and should not be dumped. To keep the
		 * compilers and static analyzers happy we still need to handle
		 * the case though.
		 */
		case DO_TYPE_CACHE:
			break;
	}
}

/*
 * dumpNamespace
 *	  writes out to fout the queries to recreate a user-defined namespace
 */
static void
dumpNamespace(Archive *fout, NamespaceInfo *nspinfo)
{
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qnspname;

	/* Skip if not to be dumped */
	if (!nspinfo->dobj.dump || dataOnly)
		return;

	/* don't dump dummy namespace from pre-7.3 source */
	if (strlen(nspinfo->dobj.name) == 0)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qnspname = strdup(fmtId(nspinfo->dobj.name));

	appendPQExpBuffer(delq, "DROP SCHEMA %s;\n", qnspname);

	appendPQExpBuffer(q, "CREATE SCHEMA %s;\n", qnspname);

	ArchiveEntry(fout, nspinfo->dobj.catId, nspinfo->dobj.dumpId,
				 nspinfo->dobj.name,
				 NULL, NULL,
				 nspinfo->rolname,
				 false, "SCHEMA", q->data, delq->data, NULL,
				 nspinfo->dobj.dependencies, nspinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Schema Comments */
	resetPQExpBuffer(q);
	appendPQExpBuffer(q, "SCHEMA %s", qnspname);
	dumpComment(fout, q->data,
				NULL, nspinfo->rolname,
				nspinfo->dobj.catId, 0, nspinfo->dobj.dumpId);

	dumpACL(fout, nspinfo->dobj.catId, nspinfo->dobj.dumpId, "SCHEMA",
			qnspname, nspinfo->dobj.name, NULL,
			nspinfo->rolname, nspinfo->nspacl);

	free(qnspname);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpExtension
 *	  writes out to fout the queries to recreate an extension
 */
static void
dumpExtension(Archive *fout, ExtensionInfo *extinfo)
{
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer labelq;
	char	   *qextname;

	/* Skip if not to be dumped */
	if (!extinfo->dobj.dump || dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	labelq = createPQExpBuffer();

	qextname = strdup(fmtId(extinfo->dobj.name));

	appendPQExpBuffer(delq, "DROP EXTENSION %s;\n", qextname);

	if (!binary_upgrade)
	{
		/*
		 * In a regular dump, we use IF NOT EXISTS so that there isn't a
		 * problem if the extension already exists in the target database;
		 * this is essential for installed-by-default extensions such as
		 * plpgsql.
		 *
		 * In binary-upgrade mode, that doesn't work well, so instead we skip
		 * built-in extensions based on their OIDs; see
		 * selectDumpableExtension.
		 */
		appendPQExpBuffer(q, "CREATE EXTENSION IF NOT EXISTS %s WITH SCHEMA %s;\n",
						  qextname, fmtId(extinfo->namespace));
	}
	else
	{
		int			i;
		int			n;

		appendPQExpBuffer(q, "-- For binary upgrade, create an empty extension and insert objects into it\n");
		appendPQExpBuffer(q,
						  "SELECT binary_upgrade.create_empty_extension(");
		appendStringLiteralAH(q, extinfo->dobj.name, fout);
		appendPQExpBuffer(q, ", ");
		appendStringLiteralAH(q, extinfo->namespace, fout);
		appendPQExpBuffer(q, ", ");
		appendPQExpBuffer(q, "%s, ", extinfo->relocatable ? "true" : "false");
		appendStringLiteralAH(q, extinfo->extversion, fout);
		appendPQExpBuffer(q, ", ");

		/*
		 * Note that we're pushing extconfig (an OID array) back into
		 * pg_extension exactly as-is.  This is OK because pg_class OIDs are
		 * preserved in binary upgrade.
		 */
		if (strlen(extinfo->extconfig) > 2)
			appendStringLiteralAH(q, extinfo->extconfig, fout);
		else
			appendPQExpBuffer(q, "NULL");
		appendPQExpBuffer(q, ", ");
		if (strlen(extinfo->extcondition) > 2)
			appendStringLiteralAH(q, extinfo->extcondition, fout);
		else
			appendPQExpBuffer(q, "NULL");
		appendPQExpBuffer(q, ", ");
		appendPQExpBuffer(q, "ARRAY[");
		n = 0;
		for (i = 0; i < extinfo->dobj.nDeps; i++)
		{
			DumpableObject *extobj;

			extobj = findObjectByDumpId(extinfo->dobj.dependencies[i]);
			if (extobj && extobj->objType == DO_EXTENSION)
			{
				if (n++ > 0)
					appendPQExpBuffer(q, ",");
				appendStringLiteralAH(q, extobj->name, fout);
			}
		}
		appendPQExpBuffer(q, "]::pg_catalog.text[]");
		appendPQExpBuffer(q, ");\n");
	}

	appendPQExpBuffer(labelq, "EXTENSION %s", qextname);

	ArchiveEntry(fout, extinfo->dobj.catId, extinfo->dobj.dumpId,
				 extinfo->dobj.name,
				 NULL, NULL,
				 "",
				 false, "EXTENSION",
				 q->data, delq->data, NULL,
				 extinfo->dobj.dependencies, extinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Extension Comments and Security Labels */
	dumpComment(fout, labelq->data,
				NULL, "",
				extinfo->dobj.catId, 0, extinfo->dobj.dumpId);

	free(qextname);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
}

/*
 * dumpType
 *	  writes out to fout the queries to recreate a user-defined type
 */
static void
dumpType(Archive *fout, TypeInfo *tinfo)
{
	/* Skip if not to be dumped */
	if (!tinfo->dobj.dump || dataOnly)
		return;

	/* Dump out in proper style */
	if (tinfo->typtype == TYPTYPE_BASE)
		dumpBaseType(fout, tinfo);
	else if (tinfo->typtype == TYPTYPE_DOMAIN)
		dumpDomain(fout, tinfo);
	else if (tinfo->typtype == TYPTYPE_COMPOSITE)
		dumpCompositeType(fout, tinfo);
	else if (tinfo->typtype == TYPTYPE_ENUM)
		dumpEnumType(fout, tinfo);
}

/*
 * dumpEnumType
 *	  writes out to fout the queries to recreate a user-defined enum type
 */
static void
dumpEnumType(Archive *fout, TypeInfo *tinfo)
{
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer labelq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			num,
				i;
	Oid			enum_oid;
	char	   *label;

	/* Set proper schema search path so regproc references list correctly */
	selectSourceSchema(tinfo->dobj.namespace->dobj.name);

	appendPQExpBuffer(query, "SELECT oid, enumlabel "
					  "FROM pg_catalog.pg_enum "
					  "WHERE enumtypid = '%u'"
					  "ORDER BY oid",
					  tinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	num = PQntuples(res);

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog.
	 * CASCADE shouldn't be required here as for normal types since the I/O
	 * functions are generic and do not get dropped.
	 */
	appendPQExpBuffer(delq, "DROP TYPE %s.",
					  fmtId(tinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, "%s;\n",
					  fmtId(tinfo->dobj.name));

	appendPQExpBuffer(q, "CREATE TYPE %s AS ENUM (\n",
					  fmtId(tinfo->dobj.name));

	if (!binary_upgrade)
	{
		for (i = 0; i < num; i++)
		{
			/* Labels with server-assigned oids */
			label = PQgetvalue(res, i, PQfnumber(res, "oid"));
			if (i > 0)
				appendPQExpBuffer(q, ",\n");
			appendPQExpBuffer(q, "    ");
			appendStringLiteralAH(q, label, fout);
		}
	}

	appendPQExpBuffer(q, "\n);\n");

	if (binary_upgrade)
	{
		/* Labels with dump-assigned (preserved) oids */
		for (i = 0; i < num; i++)
		{
			enum_oid = atooid(PQgetvalue(res, i, PQfnumber(res, "oid")));
			label = PQgetvalue(res, i, PQfnumber(res, "enumlabel"));

			if (i == 0)
				appendPQExpBuffer(q, "\n-- For binary upgrade, must preserve pg_enum oids\n");
			appendPQExpBuffer(q,
			 "SELECT binary_upgrade.add_pg_enum_label('%u'::pg_catalog.oid, "
							  "'%u'::pg_catalog.oid, ",
							  enum_oid, tinfo->dobj.catId.oid);
			appendStringLiteralAH(q, label, fout);
			appendPQExpBuffer(q, ");\n");
		}
		appendPQExpBuffer(q, "\n");
	}

	appendPQExpBuffer(labelq, "TYPE %s", fmtId(tinfo->dobj.name));

	if (binary_upgrade)
		binary_upgrade_extension_member(q, &tinfo->dobj, labelq->data);

	ArchiveEntry(fout, tinfo->dobj.catId, tinfo->dobj.dumpId,
				 tinfo->dobj.name,
				 tinfo->dobj.namespace->dobj.name,
				 NULL,
				 tinfo->rolname, false,
				 "TYPE", q->data, delq->data, NULL,
				 tinfo->dobj.dependencies, tinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Type Comments */
	dumpComment(fout, labelq->data,
				tinfo->dobj.namespace->dobj.name, tinfo->rolname,
				tinfo->dobj.catId, 0, tinfo->dobj.dumpId);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(query);
}

/*
 * dumpBaseType
 *	  writes out to fout the queries to recreate a user-defined base type
 */
static void
dumpBaseType(Archive *fout, TypeInfo *tinfo)
{
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer labelq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	char	   *typlen;
	char	   *typinput;
	char	   *typoutput;
	char	   *typreceive;
	char	   *typsend;
	char	   *typmodin;
	char	   *typmodout;
	char	   *typanalyze;
	Oid			typinputoid;
	Oid			typoutputoid;
	Oid			typreceiveoid;
	Oid			typsendoid;
	Oid			typmodinoid;
	Oid			typmodoutoid;
	Oid			typanalyzeoid;
	char	   *typdelim;
	char	   *typbyval;
	char	   *typalign;
	char	   *typstorage;
	char	   *typdefault;
	bool		typdefault_is_literal = false;

	/* Set proper schema search path so regproc references list correctly */
	selectSourceSchema(tinfo->dobj.namespace->dobj.name);

	/* Fetch type-specific details */
	if (fout->remoteVersion >= 80300)
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, typreceive, typsend, "
						  "typmodin, typmodout, typanalyze, "
						  "typinput::pg_catalog.oid as typinputoid, "
						  "typoutput::pg_catalog.oid as typoutputoid, "
						  "typreceive::pg_catalog.oid as typreceiveoid, "
						  "typsend::pg_catalog.oid as typsendoid, "
						  "typmodin::pg_catalog.oid as typmodinoid, "
						  "typmodout::pg_catalog.oid as typmodoutoid, "
						  "typanalyze::pg_catalog.oid as typanalyzeoid, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "pg_catalog.pg_get_expr(typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) as typdefaultbin, typdefault "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  tinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 80000)
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, typreceive, typsend, "
						  "'-' as typmodin, '-' as typmodout, "
						  "typanalyze, "
						  "typinput::pg_catalog.oid as typinputoid, "
						  "typoutput::pg_catalog.oid as typoutputoid, "
						  "typreceive::pg_catalog.oid as typreceiveoid, "
						  "typsend::pg_catalog.oid as typsendoid, "
						  "0 as typmodinoid, 0 as typmodoutoid, "
						  "typanalyze::pg_catalog.oid as typanalyzeoid, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "pg_catalog.pg_get_expr(typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) as typdefaultbin, typdefault "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  tinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 70400)
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, typreceive, typsend, "
						  "'-' as typmodin, '-' as typmodout, "
						  "'-' as typanalyze, "
						  "typinput::pg_catalog.oid as typinputoid, "
						  "typoutput::pg_catalog.oid as typoutputoid, "
						  "typreceive::pg_catalog.oid as typreceiveoid, "
						  "typsend::pg_catalog.oid as typsendoid, "
						  "0 as typmodinoid, 0 as typmodoutoid, "
						  "0 as typanalyzeoid, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "pg_catalog.pg_get_expr(typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) as typdefaultbin, typdefault "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  tinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, "
						  "'-' as typreceive, '-' as typsend, "
						  "'-' as typmodin, '-' as typmodout, "
						  "'-' as typanalyze, "
						  "typinput::pg_catalog.oid as typinputoid, "
						  "typoutput::pg_catalog.oid as typoutputoid, "
						  "0 as typreceiveoid, 0 as typsendoid, "
						  "0 as typmodinoid, 0 as typmodoutoid, "
						  "0 as typanalyzeoid, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "pg_catalog.pg_get_expr(typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) as typdefaultbin, typdefault "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  tinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 70200)
	{
		/*
		 * Note: although pre-7.3 catalogs contain typreceive and typsend,
		 * ignore them because they are not right.
		 */
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, "
						  "'-' as typreceive, '-' as typsend, "
						  "'-' as typmodin, '-' as typmodout, "
						  "'-' as typanalyze, "
						  "typinput::oid as typinputoid, "
						  "typoutput::oid as typoutputoid, "
						  "0 as typreceiveoid, 0 as typsendoid, "
						  "0 as typmodinoid, 0 as typmodoutoid, "
						  "0 as typanalyzeoid, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "NULL as typdefaultbin, typdefault "
						  "FROM pg_type "
						  "WHERE oid = '%u'::oid",
						  tinfo->dobj.catId.oid);
	}
	else if (fout->remoteVersion >= 70100)
	{
		/*
		 * Ignore pre-7.2 typdefault; the field exists but has an unusable
		 * representation.
		 */
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, "
						  "'-' as typreceive, '-' as typsend, "
						  "'-' as typmodin, '-' as typmodout, "
						  "'-' as typanalyze, "
						  "typinput::oid as typinputoid, "
						  "typoutput::oid as typoutputoid, "
						  "0 as typreceiveoid, 0 as typsendoid, "
						  "0 as typmodinoid, 0 as typmodoutoid, "
						  "0 as typanalyzeoid, "
						  "typdelim, typbyval, typalign, typstorage, "
						  "NULL as typdefaultbin, NULL as typdefault "
						  "FROM pg_type "
						  "WHERE oid = '%u'::oid",
						  tinfo->dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT typlen, "
						  "typinput, typoutput, "
						  "'-' as typreceive, '-' as typsend, "
						  "'-' as typmodin, '-' as typmodout, "
						  "'-' as typanalyze, "
						  "typinput::oid as typinputoid, "
						  "typoutput::oid as typoutputoid, "
						  "0 as typreceiveoid, 0 as typsendoid, "
						  "0 as typmodinoid, 0 as typmodoutoid, "
						  "0 as typanalyzeoid, "
						  "typdelim, typbyval, typalign, "
						  "'p'::char as typstorage, "
						  "NULL as typdefaultbin, NULL as typdefault "
						  "FROM pg_type "
						  "WHERE oid = '%u'::oid",
						  tinfo->dobj.catId.oid);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query returned %d rows instead of one: %s\n",
				  ntups, query->data);
		exit_nicely();
	}

	typlen = PQgetvalue(res, 0, PQfnumber(res, "typlen"));
	typinput = PQgetvalue(res, 0, PQfnumber(res, "typinput"));
	typoutput = PQgetvalue(res, 0, PQfnumber(res, "typoutput"));
	typreceive = PQgetvalue(res, 0, PQfnumber(res, "typreceive"));
	typsend = PQgetvalue(res, 0, PQfnumber(res, "typsend"));
	typmodin = PQgetvalue(res, 0, PQfnumber(res, "typmodin"));
	typmodout = PQgetvalue(res, 0, PQfnumber(res, "typmodout"));
	typanalyze = PQgetvalue(res, 0, PQfnumber(res, "typanalyze"));
	typinputoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typinputoid")));
	typoutputoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typoutputoid")));
	typreceiveoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typreceiveoid")));
	typsendoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typsendoid")));
	typmodinoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typmodinoid")));
	typmodoutoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typmodoutoid")));
	typanalyzeoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typanalyzeoid")));
	typdelim = PQgetvalue(res, 0, PQfnumber(res, "typdelim"));
	typbyval = PQgetvalue(res, 0, PQfnumber(res, "typbyval"));
	typalign = PQgetvalue(res, 0, PQfnumber(res, "typalign"));
	typstorage = PQgetvalue(res, 0, PQfnumber(res, "typstorage"));
	if (!PQgetisnull(res, 0, PQfnumber(res, "typdefaultbin")))
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefaultbin"));
	else if (!PQgetisnull(res, 0, PQfnumber(res, "typdefault")))
	{
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefault"));
		typdefault_is_literal = true;	/* it needs quotes */
	}
	else
		typdefault = NULL;

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog.
	 * The reason we include CASCADE is that the circular dependency between
	 * the type and its I/O functions makes it impossible to drop the type any
	 * other way.
	 */
	appendPQExpBuffer(delq, "DROP TYPE %s.",
					  fmtId(tinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, "%s CASCADE;\n",
					  fmtId(tinfo->dobj.name));

	appendPQExpBuffer(q,
					  "CREATE TYPE %s (\n"
					  "    INTERNALLENGTH = %s",
					  fmtId(tinfo->dobj.name),
					  (strcmp(typlen, "-1") == 0) ? "variable" : typlen);

	if (fout->remoteVersion >= 70300)
	{
		/* regproc result is correctly quoted as of 7.3 */
		appendPQExpBuffer(q, ",\n    INPUT = %s", typinput);
		appendPQExpBuffer(q, ",\n    OUTPUT = %s", typoutput);
		if (OidIsValid(typreceiveoid))
			appendPQExpBuffer(q, ",\n    RECEIVE = %s", typreceive);
		if (OidIsValid(typsendoid))
			appendPQExpBuffer(q, ",\n    SEND = %s", typsend);
		if (OidIsValid(typmodinoid))
			appendPQExpBuffer(q, ",\n    TYPMOD_IN = %s", typmodin);
		if (OidIsValid(typmodoutoid))
			appendPQExpBuffer(q, ",\n    TYPMOD_OUT = %s", typmodout);
		if (OidIsValid(typanalyzeoid))
			appendPQExpBuffer(q, ",\n    ANALYZE = %s", typanalyze);
	}
	else
	{
		/* regproc delivers an unquoted name before 7.3 */
		/* cannot combine these because fmtId uses static result area */
		appendPQExpBuffer(q, ",\n    INPUT = %s", fmtId(typinput));
		appendPQExpBuffer(q, ",\n    OUTPUT = %s", fmtId(typoutput));
		/* receive/send/typmodin/typmodout/analyze need not be printed */
	}

	if (typdefault != NULL)
	{
		appendPQExpBuffer(q, ",\n    DEFAULT = ");
		if (typdefault_is_literal)
			appendStringLiteralAH(q, typdefault, fout);
		else
			appendPQExpBufferStr(q, typdefault);
	}

	if (OidIsValid(tinfo->typelem))
	{
		char	   *elemType;

		/* reselect schema in case changed by function dump */
		selectSourceSchema(tinfo->dobj.namespace->dobj.name);
		elemType = getFormattedTypeName(tinfo->typelem, zeroAsOpaque);
		appendPQExpBuffer(q, ",\n    ELEMENT = %s", elemType);
		free(elemType);
	}

	if (typdelim && strcmp(typdelim, ",") != 0)
	{
		appendPQExpBuffer(q, ",\n    DELIMITER = ");
		appendStringLiteralAH(q, typdelim, fout);
	}

	if (strcmp(typalign, "c") == 0)
		appendPQExpBuffer(q, ",\n    ALIGNMENT = char");
	else if (strcmp(typalign, "s") == 0)
		appendPQExpBuffer(q, ",\n    ALIGNMENT = int2");
	else if (strcmp(typalign, "i") == 0)
		appendPQExpBuffer(q, ",\n    ALIGNMENT = int4");
	else if (strcmp(typalign, "d") == 0)
		appendPQExpBuffer(q, ",\n    ALIGNMENT = double");

	if (strcmp(typstorage, "p") == 0)
		appendPQExpBuffer(q, ",\n    STORAGE = plain");
	else if (strcmp(typstorage, "e") == 0)
		appendPQExpBuffer(q, ",\n    STORAGE = external");
	else if (strcmp(typstorage, "x") == 0)
		appendPQExpBuffer(q, ",\n    STORAGE = extended");
	else if (strcmp(typstorage, "m") == 0)
		appendPQExpBuffer(q, ",\n    STORAGE = main");

	if (strcmp(typbyval, "t") == 0)
		appendPQExpBuffer(q, ",\n    PASSEDBYVALUE");

	appendPQExpBuffer(q, "\n);\n");

	appendPQExpBuffer(labelq, "TYPE %s", fmtId(tinfo->dobj.name));

	if (binary_upgrade)
		binary_upgrade_extension_member(q, &tinfo->dobj, labelq->data);

	ArchiveEntry(fout, tinfo->dobj.catId, tinfo->dobj.dumpId,
				 tinfo->dobj.name,
				 tinfo->dobj.namespace->dobj.name,
				 NULL,
				 tinfo->rolname, false,
				 "TYPE", q->data, delq->data, NULL,
				 tinfo->dobj.dependencies, tinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Type Comments */
	dumpComment(fout, labelq->data,
				tinfo->dobj.namespace->dobj.name, tinfo->rolname,
				tinfo->dobj.catId, 0, tinfo->dobj.dumpId);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(query);
}

/*
 * dumpTypeStorageOptions
 *     writes out to fout the ALTER TYPE queries to set default storage options for type
 */
static void
dumpTypeStorageOptions(Archive *fout, TypeStorageOptions *tstorageoptions)
{
	PQExpBuffer q;
	PQExpBuffer delq;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	/* Set proper schema search path so regproc references list correctly */
	selectSourceSchema(tstorageoptions->dobj.namespace->dobj.name);

	appendPQExpBuffer(q, "ALTER TYPE %s ", tstorageoptions->dobj.name);
	appendPQExpBuffer(q, " SET DEFAULT ENCODING (%s);\n", tstorageoptions->typoptions);

	ArchiveEntry(	fout
	            , tstorageoptions->dobj.catId                 /* catalog ID  */
	            , tstorageoptions->dobj.dumpId                /* dump ID     */
	            , tstorageoptions->dobj.name                  /* type name   */
	            , tstorageoptions->dobj.namespace->dobj.name  /* name space  */
	            , NULL                                        /* table space */
	            , tstorageoptions->rolname                    /* owner name  */
	            , false                                       /* with oids   */
	            , "TYPE STORAGE OPTIONS"                      /* Desc        */
	            , q->data                                     /* ALTER...    */
	            , ""                                          /* Del         */
	            , NULL                                        /* Copy        */
	            , NULL                                        /* Deps        */
	            , 0                                           /* num Deps    */
	            , NULL                                        /* Dumper      */
	            , NULL                                        /* Dumper Arg  */
	            );

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);


}  /* end dumpTypeStorageOptions */

/*
 * dumpDomain
 *	  writes out to fout the queries to recreate a user-defined domain
 */
static void
dumpDomain(Archive *fout, TypeInfo *tinfo)
{
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer labelq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			i;
	char	   *typnotnull;
	char	   *typdefn;
	char	   *typdefault;
	bool		typdefault_is_literal = false;

	/* Set proper schema search path so type references list correctly */
	selectSourceSchema(tinfo->dobj.namespace->dobj.name);

	/* Fetch domain specific details */
	appendPQExpBuffer(query, "SELECT typnotnull, "
				"pg_catalog.format_type(typbasetype, typtypmod) as typdefn, "
					  "pg_catalog.pg_get_expr(typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) as typdefaultbin, typdefault "
					  "FROM pg_catalog.pg_type "
					  "WHERE oid = '%u'::pg_catalog.oid",
					  tinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query returned %d rows instead of one: %s\n",
				  ntups, query->data);
		exit_nicely();
	}

	typnotnull = PQgetvalue(res, 0, PQfnumber(res, "typnotnull"));
	typdefn = PQgetvalue(res, 0, PQfnumber(res, "typdefn"));
	if (!PQgetisnull(res, 0, PQfnumber(res, "typdefaultbin")))
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefaultbin"));
	else if (!PQgetisnull(res, 0, PQfnumber(res, "typdefault")))
	{
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefault"));
		typdefault_is_literal = true;	/* it needs quotes */
	}
	else
		typdefault = NULL;

	appendPQExpBuffer(q,
					  "CREATE DOMAIN %s AS %s",
					  fmtId(tinfo->dobj.name),
					  typdefn);

	if (typnotnull[0] == 't')
		appendPQExpBuffer(q, " NOT NULL");

	if (typdefault != NULL)
	{
		appendPQExpBuffer(q, " DEFAULT ");
		if (typdefault_is_literal)
			appendStringLiteralAH(q, typdefault, fout);
		else
			appendPQExpBufferStr(q, typdefault);
	}

	PQclear(res);

	/*
	 * Add any CHECK constraints for the domain
	 */
	for (i = 0; i < tinfo->nDomChecks; i++)
	{
		ConstraintInfo *domcheck = &(tinfo->domChecks[i]);

		if (!domcheck->separate)
			appendPQExpBuffer(q, "\n\tCONSTRAINT %s %s",
							  fmtId(domcheck->dobj.name), domcheck->condef);
	}

	appendPQExpBuffer(q, ";\n");

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP DOMAIN %s.",
					  fmtId(tinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, "%s;\n",
					  fmtId(tinfo->dobj.name));

	ArchiveEntry(fout, tinfo->dobj.catId, tinfo->dobj.dumpId,
				 tinfo->dobj.name,
				 tinfo->dobj.namespace->dobj.name,
				 NULL,
				 tinfo->rolname, false,
				 "DOMAIN", q->data, delq->data, NULL,
				 tinfo->dobj.dependencies, tinfo->dobj.nDeps,
				 NULL, NULL);

	appendPQExpBuffer(labelq, "DOMAIN %s", fmtId(tinfo->dobj.name));

	if (binary_upgrade)
		binary_upgrade_extension_member(q, &tinfo->dobj, labelq->data);

	/* Dump Domain Comments */
	dumpComment(fout, labelq->data,
				tinfo->dobj.namespace->dobj.name, tinfo->rolname,
				tinfo->dobj.catId, 0, tinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(query);
}

/*
 * dumpCompositeType
 *	  writes out to fout the queries to recreate a user-defined stand-alone
 *	  composite type
 */
static void
dumpCompositeType(Archive *fout, TypeInfo *tinfo)
{
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer labelq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			i_attname;
	int			i_atttypdefn;
	int			i;

	/* Set proper schema search path so type references list correctly */
	selectSourceSchema(tinfo->dobj.namespace->dobj.name);

	/* Fetch type specific details */

	appendPQExpBuffer(query, "SELECT a.attname, "
			 "pg_catalog.format_type(a.atttypid, a.atttypmod) as atttypdefn "
					  "FROM pg_catalog.pg_type t, pg_catalog.pg_attribute a "
					  "WHERE t.oid = '%u'::pg_catalog.oid "
					  "AND a.attrelid = t.typrelid "
					  "AND NOT a.attisdropped "
					  "ORDER BY a.attnum ",
					  tinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting at least a single result */
	ntups = PQntuples(res);
	if (ntups < 1)
	{
		write_msg(NULL, "query returned no rows: %s\n", query->data);
		exit_nicely();
	}

	i_attname = PQfnumber(res, "attname");
	i_atttypdefn = PQfnumber(res, "atttypdefn");

	appendPQExpBuffer(q, "CREATE TYPE %s AS (",
					  fmtId(tinfo->dobj.name));

	for (i = 0; i < ntups; i++)
	{
		char	   *attname;
		char	   *atttypdefn;

		attname = PQgetvalue(res, i, i_attname);
		atttypdefn = PQgetvalue(res, i, i_atttypdefn);

		appendPQExpBuffer(q, "\n\t%s %s", fmtId(attname), atttypdefn);
		if (i < ntups - 1)
			appendPQExpBuffer(q, ",");
	}
	appendPQExpBuffer(q, "\n);\n");

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP TYPE %s.",
					  fmtId(tinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, "%s;\n",
					  fmtId(tinfo->dobj.name));

	appendPQExpBuffer(labelq, "TYPE %s", fmtId(tinfo->dobj.name));

	if (binary_upgrade)
		binary_upgrade_extension_member(q, &tinfo->dobj, labelq->data);

	ArchiveEntry(fout, tinfo->dobj.catId, tinfo->dobj.dumpId,
				 tinfo->dobj.name,
				 tinfo->dobj.namespace->dobj.name,
				 NULL,
				 tinfo->rolname, false,
				 "TYPE", q->data, delq->data, NULL,
				 tinfo->dobj.dependencies, tinfo->dobj.nDeps,
				 NULL, NULL);


	/* Dump Type Comments */
	dumpComment(fout, labelq->data,
				tinfo->dobj.namespace->dobj.name, tinfo->rolname,
				tinfo->dobj.catId, 0, tinfo->dobj.dumpId);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(query);
}

/*
 * dumpShellType
 *	  writes out to fout the queries to create a shell type
 *
 * We dump a shell definition in advance of the I/O functions for the type.
 */
static void
dumpShellType(Archive *fout, ShellTypeInfo *stinfo)
{
	PQExpBuffer q;

	/* Skip if not to be dumped */
	if (!stinfo->dobj.dump || dataOnly)
		return;

	q = createPQExpBuffer();

	/*
	 * Note the lack of a DROP command for the shell type; any required DROP
	 * is driven off the base type entry, instead.	This interacts with
	 * _printTocEntry()'s use of the presence of a DROP command to decide
	 * whether an entry needs an ALTER OWNER command.  We don't want to alter
	 * the shell type's owner immediately on creation; that should happen only
	 * after it's filled in, otherwise the backend complains.
	 */

	appendPQExpBuffer(q, "CREATE TYPE %s;\n",
					  fmtId(stinfo->dobj.name));

	ArchiveEntry(fout, stinfo->dobj.catId, stinfo->dobj.dumpId,
				 stinfo->dobj.name,
				 stinfo->dobj.namespace->dobj.name,
				 NULL,
				 stinfo->baseType->rolname, false,
				 "SHELL TYPE", q->data, "", NULL,
				 stinfo->dobj.dependencies, stinfo->dobj.nDeps,
				 NULL, NULL);

	destroyPQExpBuffer(q);
}

/*
 * Determine whether we want to dump definitions for procedural languages.
 * Since the languages themselves don't have schemas, we can't rely on
 * the normal schema-based selection mechanism.  We choose to dump them
 * whenever neither --schema nor --table was given.  (Before 8.1, we used
 * the dump flag of the PL's call handler function, but in 8.1 this will
 * probably always be false since call handlers are created in pg_catalog.)
 *
 * For some backwards compatibility with the older behavior, we forcibly
 * dump a PL if its handler function (and validator if any) are in a
 * dumpable namespace.	That case is not checked here.
 */
static bool
shouldDumpProcLangs(void)
{
	if (!include_everything)
		return false;
	/* And they're schema not data */
	if (dataOnly)
		return false;
	return true;
}

/*
 * dumpProcLang
 *		  writes out to fout the queries to recreate a user-defined
 *		  procedural language
 */
static void
dumpProcLang(Archive *fout, ProcLangInfo *plang)
{
	PQExpBuffer defqry;
	PQExpBuffer delqry;
	PQExpBuffer labelq;
	bool		useParams;
	char	   *qlanname;
	char	   *lanschema;
	FuncInfo   *funcInfo;
	FuncInfo   *inlineInfo = NULL;
	FuncInfo   *validatorInfo = NULL;

	/* Skip if not to be dumped */
	if (!plang->dobj.dump || dataOnly)
		return;

	/*
	 * Try to find the support function(s).  It is not an error if we don't
	 * find them --- if the functions are in the pg_catalog schema, as is
	 * standard in 8.1 and up, then we won't have loaded them. (In this case
	 * we will emit a parameterless CREATE LANGUAGE command, which will
	 * require PL template knowledge in the backend to reload.)
	 */

	funcInfo = findFuncByOid(plang->lanplcallfoid);
	if (funcInfo != NULL && !funcInfo->dobj.dump)
		funcInfo = NULL;		/* treat not-dumped same as not-found */

	if (OidIsValid(plang->laninline))
	{
		inlineInfo = findFuncByOid(plang->laninline);
		if (inlineInfo != NULL && !inlineInfo->dobj.dump)
			inlineInfo = NULL;
	}

	if (OidIsValid(plang->lanvalidator))
	{
		validatorInfo = findFuncByOid(plang->lanvalidator);
		if (validatorInfo != NULL && !validatorInfo->dobj.dump)
			validatorInfo = NULL;
	}

	/*
	 * If the functions are dumpable then emit a traditional CREATE LANGUAGE
	 * with parameters.  Otherwise, dump only if shouldDumpProcLangs() says to
	 * dump it.
	 */
	useParams = (funcInfo != NULL &&
				 (inlineInfo != NULL || !OidIsValid(plang->laninline)) &&
				 (validatorInfo != NULL || !OidIsValid(plang->lanvalidator)));

	if (!useParams && !shouldDumpProcLangs())
		return;

	defqry = createPQExpBuffer();
	delqry = createPQExpBuffer();
	labelq = createPQExpBuffer();

	qlanname = strdup(fmtId(plang->dobj.name));

	/*
	 * If dumping a HANDLER clause, treat the language as being in the handler
	 * function's schema; this avoids cluttering the HANDLER clause. Otherwise
	 * it doesn't really have a schema.
	 */
	if (useParams)
		lanschema = funcInfo->dobj.namespace->dobj.name;
	else
		lanschema = NULL;

	appendPQExpBuffer(delqry, "DROP PROCEDURAL LANGUAGE %s;\n",
					  qlanname);

	appendPQExpBuffer(defqry, "CREATE %sPROCEDURAL LANGUAGE %s",
					  (useParams && plang->lanpltrusted) ? "TRUSTED " : "",
					  qlanname);
	if (useParams)
	{
		appendPQExpBuffer(defqry, " HANDLER %s",
						  fmtId(funcInfo->dobj.name));
		if (OidIsValid(plang->laninline))
		{
			appendPQExpBuffer(defqry, " INLINE ");
			/* Cope with possibility that inline is in different schema */
			if (inlineInfo->dobj.namespace != funcInfo->dobj.namespace)
				appendPQExpBuffer(defqry, "%s.",
							fmtId(inlineInfo->dobj.namespace->dobj.name));
			appendPQExpBuffer(defqry, "%s",
							  fmtId(inlineInfo->dobj.name));
		}
		if (OidIsValid(plang->lanvalidator))
		{
			appendPQExpBuffer(defqry, " VALIDATOR ");
			/* Cope with possibility that validator is in different schema */
			if (validatorInfo->dobj.namespace != funcInfo->dobj.namespace)
				appendPQExpBuffer(defqry, "%s.",
							fmtId(validatorInfo->dobj.namespace->dobj.name));
			appendPQExpBuffer(defqry, "%s",
							  fmtId(validatorInfo->dobj.name));
		}
	}
	appendPQExpBuffer(defqry, ";\n");

	/*
	 * If the language is one of those for which the call handler and
	 * validator functions are defined in pg_pltemplate, we must add ALTER
	 * FUNCTION ... OWNER statements to switch the functions to the user to
	 * whom the functions are assigned -OR- adjust the language owner to
	 * reflect the call handler owner so a SET SESSION AUTHORIZATION statement
	 * properly reflects the "language" owner.
	 *
	 * Functions specified in pg_pltemplate are entered into pg_proc under
	 * pg_catalog.	Functions in pg_catalog are omitted from the function list
	 * structure resulting in the references to them in this procedure to be
	 * NULL.
	 *
	 * TODO: Adjust for ALTER LANGUAGE ... OWNER support.
	 */
	if (use_setsessauth)
	{
		/*
		 * If using SET SESSION AUTHORIZATION statements to reflect
		 * language/function ownership, alter the LANGUAGE owner to reflect
		 * the owner of the call handler function (or the validator function)
		 * if the fuction is from pg_pltempate. (Other functions are
		 * explicitly created and not subject the user in effect with CREATE
		 * LANGUAGE.)
		 */
		char	   *languageOwner = NULL;

		if (funcInfo == NULL)
		{
			languageOwner = getFuncOwner(plang->lanplcallfoid, "tmplhandler");
		}
		else if (validatorInfo == NULL)
		{
			languageOwner = getFuncOwner(plang->lanvalidator, "tmplvalidator");
		}
		if (languageOwner != NULL)
		{
			free(plang->lanowner);
			plang->lanowner = languageOwner;
		}
	}
	else
	{
		/*
		 * If the call handler or validator is defined, check to see if it's
		 * one of the pre-defined ones.  If so, it won't have been dumped as a
		 * function so won't have the proper owner -- we need to emit an ALTER
		 * FUNCTION ... OWNER statement for it.
		 */
		if (funcInfo == NULL)
		{
			dumpPlTemplateFunc(plang->lanplcallfoid, "tmplhandler", defqry);
		}
		if (validatorInfo == NULL)
		{
			dumpPlTemplateFunc(plang->lanvalidator, "tmplvalidator", defqry);
		}
	}

	appendPQExpBuffer(labelq, "LANGUAGE %s", qlanname);

	if (binary_upgrade)
		binary_upgrade_extension_member(defqry, &plang->dobj, labelq->data);

	ArchiveEntry(fout, plang->dobj.catId, plang->dobj.dumpId,
				 plang->dobj.name,
				 lanschema, NULL, plang->lanowner,
				 false, "PROCEDURAL LANGUAGE",
				 defqry->data,
				 (g_fout->remoteVersion >= 80209 ? "" : delqry->data), //don 't drop plpgsql if can' t be dropped
				 NULL,
				 plang->dobj.dependencies, plang->dobj.nDeps,
				 NULL, NULL);

	/* Dump Proc Lang Comments */
	dumpComment(fout, labelq->data,
				NULL, "",
				plang->dobj.catId, 0, plang->dobj.dumpId);

	if (plang->lanpltrusted)
		dumpACL(fout, plang->dobj.catId, plang->dobj.dumpId, "LANGUAGE",
				qlanname, plang->dobj.name,
				lanschema,
				plang->lanowner, plang->lanacl);

	free(qlanname);

	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(labelq);
}


/*
 * getFuncOwner - retrieves the "proowner" of the function identified by funcOid
 * if, and only if, funcOid represents a function specified in pg_pltemplate.
 */
static char *
getFuncOwner(Oid funcOid, const char *templateField)
{
	PGresult   *res;
	int			ntups;
	int			i_funcowner;
	char	   *functionOwner = NULL;
	PQExpBuffer query = createPQExpBuffer();

	/* Ensure we're in the proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(query,
					  "SELECT ( %s proowner ) AS funcowner "
					  "FROM pg_proc "
		"WHERE ( oid = %d AND proname IN ( SELECT %s FROM pg_pltemplate ) )",
					  username_subquery, funcOid, templateField);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	if (ntups != 0)
	{
		i_funcowner = PQfnumber(res, "funcowner");
		functionOwner = strdup(PQgetvalue(res, 0, i_funcowner));
	}

	PQclear(res);
	destroyPQExpBuffer(query);

	return functionOwner;
}


/*
 * dumpPlTemplateFunc - appends an "ALTER FUNCTION ... OWNER" statement for the
 * pg_pltemplate-defined language function specified to the PQExpBuffer provided.
 *
 * The ALTER FUNCTION statement is added if, and only if, the function is defined
 * in the pg_catalog schema AND is identified in the pg_pltemplate table.
 */
static void
dumpPlTemplateFunc(Oid funcOid, const char *templateField, PQExpBuffer buffer)
{
	PGresult   *res;
	int			ntups;
	int			i_signature;
	int			i_owner;
	char	   *functionSignature = NULL;
	char	   *ownerName = NULL;
	PQExpBuffer fquery = createPQExpBuffer();

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	appendPQExpBuffer(fquery,
					  "SELECT p.oid::pg_catalog.regprocedure AS signature, "
					  "( %s proowner ) AS owner "
					  "FROM pg_pltemplate t, pg_proc p "
					  "WHERE p.oid = %d "
					  "AND proname = %s "
					  "AND pronamespace = ( SELECT oid FROM pg_namespace WHERE nspname = 'pg_catalog' )",
					  username_subquery, funcOid, templateField);

	res = PQexec(g_conn, fquery->data);
	check_sql_result(res, g_conn, fquery->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	if (ntups != 0)
	{
		i_signature = PQfnumber(res, "signature");
		i_owner = PQfnumber(res, "owner");
		functionSignature = strdup(PQgetvalue(res, 0, i_signature));
		ownerName = strdup(PQgetvalue(res, 0, i_owner));

		if (functionSignature != NULL && ownerName != NULL)
		{
			appendPQExpBuffer(buffer, "ALTER FUNCTION %s OWNER TO %s;\n", functionSignature, ownerName);
		}

		free(functionSignature);
		free(ownerName);
	}

	PQclear(res);
	destroyPQExpBuffer(fquery);
}

/*
 * format_function_arguments: generate function name and argument list
 *
 * This is used when we can rely on pg_get_function_arguments to format
 * the argument list.
 */
static char *
format_function_arguments(FuncInfo *finfo, char *funcargs)
{
	PQExpBufferData fn;

	initPQExpBuffer(&fn);
	appendPQExpBuffer(&fn, "%s(%s)", fmtId(finfo->dobj.name), funcargs);
	return fn.data;
}

/*
 * format_function_arguments_old: generate function name and argument list
 *
 * The argument type names are qualified if needed.  The function name
 * is never qualified.
 *
 * This is used only with pre-GPDB 5.0 servers, so we aren't expecting to see
 * DEFAULT arguments.
 *
 * Any or all of allargtypes, argmodes, argnames may be NULL.
 */
static char *
format_function_arguments_old(FuncInfo *finfo, int nallargs,
						  char **allargtypes,
						  char **argmodes,
						  char **argnames)
{
	PQExpBufferData fn;
	int			j;

	initPQExpBuffer(&fn);
	appendPQExpBuffer(&fn, "%s(", fmtId(finfo->dobj.name));
	for (j = 0; j < nallargs; j++)
	{
		Oid			typid;
		char	   *typname;
		const char *argmode;
		const char *argname;

		typid = allargtypes ? atooid(allargtypes[j]) : finfo->argtypes[j];
		typname = getFormattedTypeName(typid, zeroAsOpaque);

		if (argmodes)
		{
			switch (argmodes[j][0])
			{
				case PROARGMODE_IN:
					argmode = "";
					break;
				case PROARGMODE_OUT:
					argmode = "OUT ";
					break;
				case PROARGMODE_INOUT:
					argmode = "INOUT ";
					break;
				case PROARGMODE_VARIADIC:
					argmode = "VARIADIC ";
					break;
				case PROARGMODE_TABLE:
					/* skip table column's names */
					free(typname);
					continue;
				default:
					write_msg(NULL, "WARNING: bogus value in proargmodes array\n");
					argmode = "";
					break;
			}
		}
		else
		{
			argmode = "";
		}

		argname = argnames ? argnames[j] : (char *) NULL;
		if (argname && argname[0] == '\0')
			argname = NULL;

		appendPQExpBuffer(&fn, "%s%s%s%s%s",
						  (j > 0) ? ", " : "",
						  argmode,
						  argname ? fmtId(argname) : "",
						  argname ? " " : "",
						  typname);
		free(typname);
	}
	appendPQExpBuffer(&fn, ")");
	return fn.data;
}

/*
 *	is_returns_table_function: returns true if function id declared as
 *	RETURNS TABLE, i.e. at least one argument is PROARGMODE_TABLE
 */
static bool
is_returns_table_function(int nallargs, char **argmodes)
{
	int			j;

	if (argmodes)
		for (j = 0; j < nallargs; j++)
			if (argmodes[j][0] == PROARGMODE_TABLE)
				return true;

	return false;
}


/*
 * format_table_function_columns: generate column list for
 * table functions.
 */
static char *
format_table_function_columns(FuncInfo *finfo, int nallargs,
							  char **allargtypes,
							  char **argmodes,
							  char **argnames)
{
	PQExpBufferData fn;
	int			j;
	bool		first_column = true;

	initPQExpBuffer(&fn);
	appendPQExpBuffer(&fn, "(");

	for (j = 0; j < nallargs; j++)
	{
		Oid			typid;
		char	   *typname;

		/*
		 * argmodes are checked in format_function_arguments. Isn't neccessery
		 * check argmodes here again
		 */
		if (argmodes[j][0] == PROARGMODE_TABLE)
		{
			typid = allargtypes ? atooid(allargtypes[j]) : finfo->argtypes[j];
			typname = getFormattedTypeName(typid, zeroAsOpaque);

			/* column's name is always NOT NULL (checked in gram.y) */
			appendPQExpBuffer(&fn, "%s%s %s",
							  first_column ? "" : ", ",
							  fmtId(argnames[j]),
							  typname);
			free(typname);
			first_column = false;
		}
	}

	appendPQExpBuffer(&fn, ")");
	return fn.data;
}


/*
 * format_function_signature: generate function name and argument list
 *
 * This is like format_function_arguments except that only a minimal
 * list of input argument types is generated; this is sufficient to
 * reference the function, but not to define it.
 *
 * If honor_quotes is false then the function name is never quoted.
 * This is appropriate for use in TOC tags, but not in SQL commands.
 */
static char *
format_function_signature(FuncInfo *finfo, bool honor_quotes)
{
	PQExpBufferData fn;
	int			j;

	initPQExpBuffer(&fn);
	if (honor_quotes)
		appendPQExpBuffer(&fn, "%s(", fmtId(finfo->dobj.name));
	else
		appendPQExpBuffer(&fn, "%s(", finfo->dobj.name);
	for (j = 0; j < finfo->nargs; j++)
	{
		char	   *typname;

		typname = getFormattedTypeName(finfo->argtypes[j], zeroAsOpaque);

		appendPQExpBuffer(&fn, "%s%s",
						  (j > 0) ? ", " : "",
						  typname);
		free(typname);
	}
	appendPQExpBuffer(&fn, ")");
	return fn.data;
}


/*
 * dumpFunc:
 *	  dump out one function
 */
static void
dumpFunc(Archive *fout, FuncInfo *finfo)
{
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delqry;
	PQExpBuffer labelq;
	PQExpBuffer asPart;
	PGresult   *res;
	char       *funcsig;                /* identity signature */
	char       *funcfullsig;            /* full signature */
	char	   *funcsig_tag;
	int			ntups;
	char	   *proretset;
	char	   *prosrc;
	char	   *probin;
	char       *funcargs;
	char	   *funciargs;
	char       *funcresult;
	char	   *proallargtypes;
	char	   *proargmodes;
	char	   *proargnames;
	char	   *provolatile;
	char	   *proisstrict;
	char	   *prosecdef;
	char	   *proconfig;
	char	   *procost;
	char	   *prorows;
	char	   *lanname;
	char	   *prodataaccess;
	char	   *rettypename;
	int			nallargs;
	char	  **allargtypes = NULL;
	char	  **argmodes = NULL;
	char	  **argnames = NULL;
	bool		isGE43 = isGPDB4300OrLater();
	bool		isGE50 = isGPDB5000OrLater();
	char	  **configitems = NULL;
	int			nconfigitems = 0;
	int			i;

	/* Skip if not to be dumped */
	if (!finfo->dobj.dump || dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delqry = createPQExpBuffer();
	labelq = createPQExpBuffer();
	asPart = createPQExpBuffer();

	/* Set proper schema search path so type references list correctly */
	selectSourceSchema(finfo->dobj.namespace->dobj.name);

	/* Fetch function-specific details */

	if (isGE50)
	{
		/*
		 * In GPDB 5.0 and up we rely on pg_get_function_arguments and
		 * pg_get_function_result instead of examining proallargtypes etc.
		 */
		appendPQExpBuffer(query,
						  "SELECT proretset, prosrc, probin, "
						  "pg_catalog.pg_get_function_arguments(oid) as funcargs, "
						  "pg_catalog.pg_get_function_identity_arguments(oid) as funciargs, "
						  "pg_catalog.pg_get_function_result(oid) as funcresult, "
						  "provolatile, proisstrict, prosecdef, "
						  "proconfig, procost, prorows, prodataaccess, "
						  "(SELECT lanname FROM pg_catalog.pg_language WHERE oid = prolang) as lanname "
						  "FROM pg_catalog.pg_proc "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  finfo->dobj.catId.oid);

	}
	else
	{
		appendPQExpBuffer(query,
						  "SELECT proretset, prosrc, probin, "
						  "proallargtypes, proargmodes, proargnames, "
						  "provolatile, proisstrict, prosecdef, "
						  "null as proconfig, 0 as procost, 0 as prorows, %s"
						  "(SELECT lanname FROM pg_catalog.pg_language WHERE oid = prolang) as lanname "
						  "FROM pg_catalog.pg_proc "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  (isGE43 ? "prodataaccess, " : ""),
						  finfo->dobj.catId.oid);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query returned %d rows instead of one: %s\n",
				  ntups, query->data);
		exit_nicely();
	}

	proretset = PQgetvalue(res, 0, PQfnumber(res, "proretset"));
	prosrc = PQgetvalue(res, 0, PQfnumber(res, "prosrc"));
	probin = PQgetvalue(res, 0, PQfnumber(res, "probin"));
	if (isGE50)
	{
		funcargs = PQgetvalue(res, 0, PQfnumber(res, "funcargs"));
		funciargs = PQgetvalue(res, 0, PQfnumber(res, "funciargs"));
		funcresult = PQgetvalue(res, 0, PQfnumber(res, "funcresult"));
		proallargtypes = proargmodes = proargnames = NULL;
	}
	else
	{
		proallargtypes = PQgetvalue(res, 0, PQfnumber(res, "proallargtypes"));
		proargmodes = PQgetvalue(res, 0, PQfnumber(res, "proargmodes"));
		proargnames = PQgetvalue(res, 0, PQfnumber(res, "proargnames"));
		funcargs = funciargs = funcresult = NULL;
	}
	provolatile = PQgetvalue(res, 0, PQfnumber(res, "provolatile"));
	proisstrict = PQgetvalue(res, 0, PQfnumber(res, "proisstrict"));
	prosecdef = PQgetvalue(res, 0, PQfnumber(res, "prosecdef"));
	proconfig = PQgetvalue(res, 0, PQfnumber(res, "proconfig"));
	procost = PQgetvalue(res, 0, PQfnumber(res, "procost"));
	prorows = PQgetvalue(res, 0, PQfnumber(res, "prorows"));
	lanname = PQgetvalue(res, 0, PQfnumber(res, "lanname"));
	prodataaccess = PQgetvalue(res, 0, PQfnumber(res, "prodataaccess"));

	/*
	 * See backend/commands/define.c for details of how the 'AS' clause is
	 * used. In GPDB Paris and up, an unused probin is NULL (here ""); previous
	 * versions would set it to "-".  There are no known cases in which prosrc
	 * is unused, so the tests below for "-" are probably useless.
	 */
	if (probin[0] != '\0' && strcmp(probin, "-") != 0)
	{
		appendPQExpBuffer(asPart, "AS ");
		appendStringLiteralAH(asPart, probin, fout);
		if (strcmp(prosrc, "-") != 0)
		{
			appendPQExpBuffer(asPart, ", ");

			/*
			 * where we have bin, use dollar quoting if allowed and src
			 * contains quote or backslash; else use regular quoting.
			 */
			if (disable_dollar_quoting ||
			  (strchr(prosrc, '\'') == NULL && strchr(prosrc, '\\') == NULL))
				appendStringLiteralAH(asPart, prosrc, fout);
			else
				appendStringLiteralDQ(asPart, prosrc, NULL);
		}
	}
	else
	{
		if (strcmp(prosrc, "-") != 0)
		{
			appendPQExpBuffer(asPart, "AS ");
			/* with no bin, dollar quote src unconditionally if allowed */
			if (disable_dollar_quoting)
				appendStringLiteralAH(asPart, prosrc, fout);
			else
				appendStringLiteralDQ(asPart, prosrc, NULL);
		}
	}

	nallargs = finfo->nargs;	/* unless we learn different from allargs */

	if (proallargtypes && *proallargtypes)
	{
		int			nitems = 0;

		if (!parsePGArray(proallargtypes, &allargtypes, &nitems) ||
			nitems < finfo->nargs)
		{
			write_msg(NULL, "WARNING: could not parse proallargtypes array\n");
			if (allargtypes)
				free(allargtypes);
			allargtypes = NULL;
		}
		else
			nallargs = nitems;
	}

	if (proargmodes && *proargmodes)
	{
		int			nitems = 0;

		if (!parsePGArray(proargmodes, &argmodes, &nitems) ||
			nitems != nallargs)
		{
			write_msg(NULL, "WARNING: could not parse proargmodes array\n");
			if (argmodes)
				free(argmodes);
			argmodes = NULL;
		}
	}

	if (proargnames && *proargnames)
	{
		int			nitems = 0;

		if (!parsePGArray(proargnames, &argnames, &nitems) ||
			nitems != nallargs)
		{
			write_msg(NULL, "WARNING: could not parse proargnames array\n");
			if (argnames)
				free(argnames);
			argnames = NULL;
		}
	}

	if (funcargs)
	{
		/* GPDB 5.0 or later; we rely on server-side code for most of the work */
		funcfullsig = format_function_arguments(finfo, funcargs);
		funcsig = format_function_arguments(finfo, funciargs);
	}
	else
	{
		/* pre-GPDB 5.0, do it ourselves */
		funcsig = format_function_arguments_old(finfo, nallargs, allargtypes,
												argmodes, argnames);
		funcfullsig = funcsig;
	}
	funcsig_tag = format_function_signature(finfo, false);

	if (proconfig && *proconfig)
	{
		if (!parsePGArray(proconfig, &configitems, &nconfigitems))
		{
			write_msg(NULL, "WARNING: could not parse proconfig array\n");
			if (configitems)
				free(configitems);
			configitems = NULL;
			nconfigitems = 0;
		}
	}


	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delqry, "DROP FUNCTION %s.%s;\n",
					  fmtId(finfo->dobj.namespace->dobj.name),
					  funcsig);

	appendPQExpBuffer(q, "CREATE FUNCTION %s ", funcfullsig);

	if (funcresult)
		appendPQExpBuffer(q, "RETURNS %s", funcresult);
	else
	{
		/* switch between RETURNS SETOF RECORD and RETURNS TABLE functions */
		if (!is_returns_table_function(nallargs, argmodes))
		{
			rettypename = getFormattedTypeName(finfo->prorettype, zeroAsOpaque);
			appendPQExpBuffer(q, "RETURNS %s%s",
							  (proretset[0] == 't') ? "SETOF " : "",
							  rettypename);
			free(rettypename);
		}
		else
		{
			char	   *func_cols;
			func_cols = format_table_function_columns(finfo, nallargs, allargtypes,
													  argmodes, argnames);
			appendPQExpBuffer(q, "RETURNS TABLE %s", func_cols);
			free(func_cols);
		}
	}

	appendPQExpBuffer(q, "\n    %s", asPart->data);
	appendPQExpBuffer(q, "\n    LANGUAGE %s", fmtId(lanname));

	if (provolatile[0] != PROVOLATILE_VOLATILE)
	{
		if (provolatile[0] == PROVOLATILE_IMMUTABLE)
			appendPQExpBuffer(q, " IMMUTABLE");
		else if (provolatile[0] == PROVOLATILE_STABLE)
			appendPQExpBuffer(q, " STABLE");
		else if (provolatile[0] != PROVOLATILE_VOLATILE)
		{
			write_msg(NULL, "unrecognized provolatile value for function \"%s\"\n",
					  finfo->dobj.name);
			exit_nicely();
		}
	}

	if (proisstrict[0] == 't')
		appendPQExpBuffer(q, " STRICT");

	if (prosecdef[0] == 't')
		appendPQExpBuffer(q, " SECURITY DEFINER");

	/*
	 * COST and ROWS are emitted only if present and not default, so as not to
	 * break backwards-compatibility of the dump without need.	Keep this code
	 * in sync with the defaults in functioncmds.c.
	 */
	if (strcmp(procost, "0") != 0)
	{
		if (strcmp(lanname, "internal") == 0 || strcmp(lanname, "c") == 0)
		{
			/* default cost is 1 */
			if (strcmp(procost, "1") != 0)
				appendPQExpBuffer(q, " COST %s", procost);
		}
		else
		{
			/* default cost is 100 */
			if (strcmp(procost, "100") != 0)
				appendPQExpBuffer(q, " COST %s", procost);
		}
	}
	if (proretset[0] == 't' &&
		strcmp(prorows, "0") != 0 && strcmp(prorows, "1000") != 0)
		appendPQExpBuffer(q, " ROWS %s", prorows);

	if (prodataaccess[0] == PRODATAACCESS_NONE)
		appendPQExpBuffer(q, " NO SQL");
	else if (prodataaccess[0] == PRODATAACCESS_CONTAINS)
		appendPQExpBuffer(q, " CONTAINS SQL");
	else if (prodataaccess[0] == PRODATAACCESS_READS)
		appendPQExpBuffer(q, " READS SQL DATA");
	else if (prodataaccess[0] == PRODATAACCESS_MODIFIES)
		appendPQExpBuffer(q, " MODIFIES SQL DATA");

	for (i = 0; i < nconfigitems; i++)
	{
		/* we feel free to scribble on configitems[] here */
		char	   *configitem = configitems[i];
		char	   *pos;

		pos = strchr(configitem, '=');
		if (pos == NULL)
			continue;
		*pos++ = '\0';
		appendPQExpBuffer(q, "\n    SET %s TO ", fmtId(configitem));

		/*
		 * Some GUC variable names are 'LIST' type and hence must not be
		 * quoted.
		 */
		if (pg_strcasecmp(configitem, "DateStyle") == 0
			|| pg_strcasecmp(configitem, "search_path") == 0)
			appendPQExpBuffer(q, "%s", pos);
		else
			appendStringLiteralAH(q, pos, fout);
	}

	appendPQExpBuffer(q, ";\n");

	appendPQExpBuffer(labelq, "FUNCTION %s", funcsig);

	if (binary_upgrade)
		binary_upgrade_extension_member(q, &finfo->dobj, labelq->data);

	ArchiveEntry(fout, finfo->dobj.catId, finfo->dobj.dumpId,
				 funcsig_tag,
				 finfo->dobj.namespace->dobj.name,
				 NULL,
				 finfo->rolname, false,
				 "FUNCTION", q->data, delqry->data, NULL,
				 finfo->dobj.dependencies, finfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Function Comments */
	dumpComment(fout, labelq->data,
				finfo->dobj.namespace->dobj.name, finfo->rolname,
				finfo->dobj.catId, 0, finfo->dobj.dumpId);

	dumpACL(fout, finfo->dobj.catId, finfo->dobj.dumpId, "FUNCTION",
			funcsig, funcsig_tag,
			finfo->dobj.namespace->dobj.name,
			finfo->rolname, finfo->proacl);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(asPart);
	free(funcsig);
	free(funcsig_tag);
	if (allargtypes)
		free(allargtypes);
	if (argmodes)
		free(argmodes);
	if (argnames)
		free(argnames);
	if (configitems)
		free(configitems);
}


/*
 * Dump a user-defined cast
 */
static void
dumpCast(Archive *fout, CastInfo *cast)
{
	PQExpBuffer defqry;
	PQExpBuffer delqry;
	PQExpBuffer labelq;
	FuncInfo   *funcInfo = NULL;
	TypeInfo   *sourceInfo;
	TypeInfo   *targetInfo;

	/* Skip if not to be dumped */
	if (!cast->dobj.dump || dataOnly)
		return;

	if (OidIsValid(cast->castfunc))
	{
		funcInfo = findFuncByOid(cast->castfunc);
		if (funcInfo == NULL)
			return;
	}

	/*
	 * As per discussion we dump casts if one or more of the underlying
	 * objects (the conversion function and the two data types) are not
	 * builtin AND if all of the non-builtin objects are included in the dump.
	 * Builtin meaning, the namespace name does not start with "pg_".
	 */
	sourceInfo = findTypeByOid(cast->castsource);
	targetInfo = findTypeByOid(cast->casttarget);

	if (sourceInfo == NULL || targetInfo == NULL)
		return;

	/*
	 * Skip this cast if all objects are from pg_
	 */
	if ((funcInfo == NULL ||
		 strncmp(funcInfo->dobj.namespace->dobj.name, "pg_", 3) == 0) &&
		strncmp(sourceInfo->dobj.namespace->dobj.name, "pg_", 3) == 0 &&
		strncmp(targetInfo->dobj.namespace->dobj.name, "pg_", 3) == 0)
		return;

	/*
	 * Skip cast if function isn't from pg_ and is not to be dumped.
	 */
	if (funcInfo &&
		strncmp(funcInfo->dobj.namespace->dobj.name, "pg_", 3) != 0 &&
		!funcInfo->dobj.dump)
		return;

	/*
	 * Same for the source type
	 */
	if (strncmp(sourceInfo->dobj.namespace->dobj.name, "pg_", 3) != 0 &&
		!sourceInfo->dobj.dump)
		return;

	/*
	 * and the target type.
	 */
	if (strncmp(targetInfo->dobj.namespace->dobj.name, "pg_", 3) != 0 &&
		!targetInfo->dobj.dump)
		return;

	/* Make sure we are in proper schema (needed for getFormattedTypeName) */
	selectSourceSchema("pg_catalog");

	defqry = createPQExpBuffer();
	delqry = createPQExpBuffer();
	labelq = createPQExpBuffer();

	appendPQExpBuffer(delqry, "DROP CAST (%s AS %s);\n",
					  getFormattedTypeName(cast->castsource, zeroAsNone),
					  getFormattedTypeName(cast->casttarget, zeroAsNone));

	appendPQExpBuffer(defqry, "CREATE CAST (%s AS %s) ",
					  getFormattedTypeName(cast->castsource, zeroAsNone),
					  getFormattedTypeName(cast->casttarget, zeroAsNone));

	if (!OidIsValid(cast->castfunc))
		appendPQExpBuffer(defqry, "WITHOUT FUNCTION");
	else
	{
		/*
		 * Always qualify the function name, in case it is not in pg_catalog
		 * schema (format_function_signature won't qualify it).
		 */
		appendPQExpBuffer(defqry, "WITH FUNCTION %s.",
						  fmtId(funcInfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(defqry, "%s",
						  format_function_signature(funcInfo, true));
	}

	if (cast->castcontext == 'a')
		appendPQExpBuffer(defqry, " AS ASSIGNMENT");
	else if (cast->castcontext == 'i')
		appendPQExpBuffer(defqry, " AS IMPLICIT");
	appendPQExpBuffer(defqry, ";\n");

	appendPQExpBuffer(labelq, "CAST (%s AS %s)",
					  getFormattedTypeName(cast->castsource, zeroAsNone),
					  getFormattedTypeName(cast->casttarget, zeroAsNone));

	if (binary_upgrade)
		binary_upgrade_extension_member(defqry, &cast->dobj, labelq->data);

	ArchiveEntry(fout, cast->dobj.catId, cast->dobj.dumpId,
				 labelq->data,
				 "pg_catalog", NULL, "",
				 false, "CAST", defqry->data, delqry->data, NULL,
				 cast->dobj.dependencies, cast->dobj.nDeps,
				 NULL, NULL);

	/* Dump Cast Comments */
	dumpComment(fout, labelq->data,
				NULL, "",
				cast->dobj.catId, 0, cast->dobj.dumpId);

	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(labelq);
}

/*
 * dumpOpr
 *	  write out a single operator definition
 */
static void
dumpOpr(Archive *fout, OprInfo *oprinfo)
{
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer labelq;
	PQExpBuffer oprid;
	PQExpBuffer details;
	const char *name;
	PGresult   *res;
	int			ntups;
	int			i_oprkind;
	int			i_oprcode;
	int			i_oprleft;
	int			i_oprright;
	int			i_oprcom;
	int			i_oprnegate;
	int			i_oprrest;
	int			i_oprjoin;
	int			i_oprcanmerge;
	int			i_oprcanhash;
	char	   *oprkind;
	char	   *oprcode;
	char	   *oprleft;
	char	   *oprright;
	char	   *oprcom;
	char	   *oprnegate;
	char	   *oprrest;
	char	   *oprjoin;
	char	   *oprcanmerge;
	char	   *oprcanhash;

	/* Skip if not to be dumped */
	if (!oprinfo->dobj.dump || dataOnly)
		return;

	/*
	 * some operators are invalid because they were the result of user
	 * defining operators before commutators exist
	 */
	if (!OidIsValid(oprinfo->oprcode))
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	labelq = createPQExpBuffer();
	oprid = createPQExpBuffer();
	details = createPQExpBuffer();

	/* Make sure we are in proper schema so regoperator works correctly */
	selectSourceSchema(oprinfo->dobj.namespace->dobj.name);

	if (g_fout->remoteVersion >= 80300)
	{
		appendPQExpBuffer(query, "SELECT oprkind, "
						  "oprcode::pg_catalog.regprocedure, "
						  "oprleft::pg_catalog.regtype, "
						  "oprright::pg_catalog.regtype, "
						  "oprcom::pg_catalog.regoperator, "
						  "oprnegate::pg_catalog.regoperator, "
						  "oprrest::pg_catalog.regprocedure, "
						  "oprjoin::pg_catalog.regprocedure, "
						  "oprcanmerge, oprcanhash "
						  "from pg_catalog.pg_operator "
						  "where oid = '%u'::pg_catalog.oid",
						  oprinfo->dobj.catId.oid);
	}
	else if (g_fout->remoteVersion >= 70300)
	{
		appendPQExpBuffer(query, "SELECT oprkind, "
						  "oprcode::pg_catalog.regprocedure, "
						  "oprleft::pg_catalog.regtype, "
						  "oprright::pg_catalog.regtype, "
						  "oprcom::pg_catalog.regoperator, "
						  "oprnegate::pg_catalog.regoperator, "
						  "oprrest::pg_catalog.regprocedure, "
						  "oprjoin::pg_catalog.regprocedure, "
						  "(oprlsortop != 0) as oprcanmerge, "
						  "oprcanhash "
						  "from pg_catalog.pg_operator "
						  "where oid = '%u'::pg_catalog.oid",
						  oprinfo->dobj.catId.oid);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		appendPQExpBuffer(query, "SELECT oprkind, oprcode, "
						  "CASE WHEN oprleft = 0 THEN '-' "
						  "ELSE format_type(oprleft, NULL) END as oprleft, "
						  "CASE WHEN oprright = 0 THEN '-' "
						  "ELSE format_type(oprright, NULL) END as oprright, "
						  "oprcom, oprnegate, oprrest, oprjoin, "
						  "(oprlsortop != 0) as oprcanmerge, "
						  "oprcanhash "
						  "from pg_operator "
						  "where oid = '%u'::oid",
						  oprinfo->dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT oprkind, oprcode, "
						  "CASE WHEN oprleft = 0 THEN '-'::name "
						  "ELSE (select typname from pg_type where oid = oprleft) END as oprleft, "
						  "CASE WHEN oprright = 0 THEN '-'::name "
						  "ELSE (select typname from pg_type where oid = oprright) END as oprright, "
						  "oprcom, oprnegate, oprrest, oprjoin, "
						  "(oprlsortop != 0) as oprcanmerge, "
						  "oprcanhash "
						  "from pg_operator "
						  "where oid = '%u'::oid",
						  oprinfo->dobj.catId.oid);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query returned %d rows instead of one: %s\n",
				  ntups, query->data);
		exit_nicely();
	}

	i_oprkind = PQfnumber(res, "oprkind");
	i_oprcode = PQfnumber(res, "oprcode");
	i_oprleft = PQfnumber(res, "oprleft");
	i_oprright = PQfnumber(res, "oprright");
	i_oprcom = PQfnumber(res, "oprcom");
	i_oprnegate = PQfnumber(res, "oprnegate");
	i_oprrest = PQfnumber(res, "oprrest");
	i_oprjoin = PQfnumber(res, "oprjoin");
	i_oprcanmerge = PQfnumber(res, "oprcanmerge");
	i_oprcanhash = PQfnumber(res, "oprcanhash");

	oprkind = PQgetvalue(res, 0, i_oprkind);
	oprcode = PQgetvalue(res, 0, i_oprcode);
	oprleft = PQgetvalue(res, 0, i_oprleft);
	oprright = PQgetvalue(res, 0, i_oprright);
	oprcom = PQgetvalue(res, 0, i_oprcom);
	oprnegate = PQgetvalue(res, 0, i_oprnegate);
	oprrest = PQgetvalue(res, 0, i_oprrest);
	oprjoin = PQgetvalue(res, 0, i_oprjoin);
	oprcanmerge = PQgetvalue(res, 0, i_oprcanmerge);
	oprcanhash = PQgetvalue(res, 0, i_oprcanhash);

	appendPQExpBuffer(details, "    PROCEDURE = %s",
					  convertRegProcReference(oprcode));

	appendPQExpBuffer(oprid, "%s (",
					  oprinfo->dobj.name);

	/*
	 * right unary means there's a left arg and left unary means there's a
	 * right arg
	 */
	if (strcmp(oprkind, "r") == 0 ||
		strcmp(oprkind, "b") == 0)
	{
		name = oprleft;

		appendPQExpBuffer(details, ",\n    LEFTARG = %s", name);
		appendPQExpBuffer(oprid, "%s", name);
	}
	else
		appendPQExpBuffer(oprid, "NONE");

	if (strcmp(oprkind, "l") == 0 ||
		strcmp(oprkind, "b") == 0)
	{
		name = oprright;

		appendPQExpBuffer(details, ",\n    RIGHTARG = %s", name);
		appendPQExpBuffer(oprid, ", %s)", name);
	}
	else
		appendPQExpBuffer(oprid, ", NONE)");

	name = convertOperatorReference(oprcom);
	if (name)
		appendPQExpBuffer(details, ",\n    COMMUTATOR = %s", name);

	name = convertOperatorReference(oprnegate);
	if (name)
		appendPQExpBuffer(details, ",\n    NEGATOR = %s", name);

	if (strcmp(oprcanmerge, "t") == 0)
		appendPQExpBuffer(details, ",\n    MERGES");

	if (strcmp(oprcanhash, "t") == 0)
		appendPQExpBuffer(details, ",\n    HASHES");

	name = convertRegProcReference(oprrest);
	if (name)
		appendPQExpBuffer(details, ",\n    RESTRICT = %s", name);

	name = convertRegProcReference(oprjoin);
	if (name)
		appendPQExpBuffer(details, ",\n    JOIN = %s", name);

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP OPERATOR %s.%s;\n",
					  fmtId(oprinfo->dobj.namespace->dobj.name),
					  oprid->data);

	appendPQExpBuffer(q, "CREATE OPERATOR %s (\n%s\n);\n",
					  oprinfo->dobj.name, details->data);

	appendPQExpBuffer(labelq, "OPERATOR %s", oprid->data);

	if (binary_upgrade)
		binary_upgrade_extension_member(q, &oprinfo->dobj, labelq->data);

	ArchiveEntry(fout, oprinfo->dobj.catId, oprinfo->dobj.dumpId,
				 oprinfo->dobj.name,
				 oprinfo->dobj.namespace->dobj.name,
				 NULL,
				 oprinfo->rolname,
				 false, "OPERATOR", q->data, delq->data, NULL,
				 oprinfo->dobj.dependencies, oprinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Operator Comments */
	dumpComment(fout, labelq->data,
				oprinfo->dobj.namespace->dobj.name, oprinfo->rolname,
				oprinfo->dobj.catId, 0, oprinfo->dobj.dumpId);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(oprid);
	destroyPQExpBuffer(details);
}

/*
 * Convert a function reference obtained from pg_operator
 *
 * Returns what to print, or NULL if function references is InvalidOid
 *
 * In 7.3 the input is a REGPROCEDURE display; we have to strip the
 * argument-types part.  In prior versions, the input is a REGPROC display.
 */
static const char *
convertRegProcReference(const char *proc)
{
	char	   *name;
	char	   *paren;
	bool		inquote;

	/* In all cases "-" means a null reference */
	if (strcmp(proc, "-") == 0)
		return NULL;

	name = strdup(proc);
	/* find non-double-quoted left paren */
	inquote = false;
	for (paren = name; *paren; paren++)
	{
		if (*paren == '(' && !inquote)
		{
			*paren = '\0';
			break;
		}
		if (*paren == '"')
			inquote = !inquote;
	}
	return name;
}

/*
 * Convert an operator cross-reference obtained from pg_operator
 *
 * Returns what to print, or NULL to print nothing
 *
 * In 7.3 and up the input is a REGOPERATOR display; we have to strip the
 * argument-types part, and add OPERATOR() decoration if the name is
 * schema-qualified.  In older versions, the input is just a numeric OID,
 * which we search our operator list for.
 */
static const char *
convertOperatorReference(const char *opr)
{
	char	   *name;
	char	   *oname;
	char	   *ptr;
	bool		inquote;
	bool		sawdot;

	/* In all cases "0" means a null reference */
	if (strcmp(opr, "0") == 0)
		return NULL;

	name = strdup(opr);
	/* find non-double-quoted left paren, and check for non-quoted dot */
	inquote = false;
	sawdot = false;
	for (ptr = name; *ptr; ptr++)
	{
		if (*ptr == '"')
			inquote = !inquote;
		else if (*ptr == '.' && !inquote)
			sawdot = true;
		else if (*ptr == '(' && !inquote)
		{
			*ptr = '\0';
			break;
		}
	}
	/* If not schema-qualified, don't need to add OPERATOR() */
	if (!sawdot)
		return name;
	oname = malloc(strlen(name) + 11);
	sprintf(oname, "OPERATOR(%s)", name);
	free(name);
	return oname;
}

/*
 * Convert a function OID obtained from pg_ts_parser or pg_ts_template
 *
 * It is sufficient to use REGPROC rather than REGPROCEDURE, since the
 * argument lists of these functions are predetermined.  Note that the
 * caller should ensure we are in the proper schema, because the results
 * are search path dependent!
 */
static const char *
convertTSFunction(Oid funcOid)
{
	char	   *result;
	char		query[128];
	PGresult   *res;
	int			ntups;

	snprintf(query, sizeof(query),
			 "SELECT '%u'::pg_catalog.regproc", funcOid);
	res = PQexec(g_conn, query);
	check_sql_result(res, g_conn, query, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query returned %d rows instead of one: %s\n",
				  ntups, query);
		exit_nicely();
	}

	result = strdup(PQgetvalue(res, 0, 0));

	PQclear(res);

	return result;
}


/*
 * dumpOpclass
 *	  write out a single operator class definition
 */
static void
dumpOpclass(Archive *fout, OpclassInfo *opcinfo)
{
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer labelq;
	PGresult   *res;
	int			ntups;
	int			i_opcintype;
	int			i_opckeytype;
	int			i_opcdefault;
	int			i_opcfamily;
	int			i_opcfamilynsp;
	int			i_amname;
	int			i_amopstrategy;
	int			i_amopreqcheck;
	int			i_amopopr;
	int			i_amprocnum;
	int			i_amproc;
	char	   *opcintype;
	char	   *opckeytype;
	char	   *opcdefault;
	char	   *opcfamily;
	char	   *opcfamilynsp;
	char	   *amname;
	char	   *amopstrategy;
	char	   *amopreqcheck;
	char	   *amopopr;
	char	   *amprocnum;
	char	   *amproc;
	bool		needComma;
	int			i;

	/* Skip if not to be dumped */
	if (!opcinfo->dobj.dump || dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	labelq = createPQExpBuffer();

	/* Make sure we are in proper schema so regoperator works correctly */
	selectSourceSchema(opcinfo->dobj.namespace->dobj.name);

	/* Get additional fields from the pg_opclass row */
	if (g_fout->remoteVersion >= 80300)
	{
		appendPQExpBuffer(query, "SELECT opcintype::pg_catalog.regtype, "
						  "opckeytype::pg_catalog.regtype, "
						  "opcdefault, "
						  "opfname AS opcfamily, "
						  "nspname AS opcfamilynsp, "
						  "(SELECT amname FROM pg_catalog.pg_am WHERE oid = opcmethod) AS amname "
						  "FROM pg_catalog.pg_opclass c "
				   "LEFT JOIN pg_catalog.pg_opfamily f ON f.oid = opcfamily "
			   "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = opfnamespace "
						  "WHERE c.oid = '%u'::pg_catalog.oid",
						  opcinfo->dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT opcintype::pg_catalog.regtype, "
						  "opckeytype::pg_catalog.regtype, "
						  "opcdefault, "
						  "NULL AS opcfamily, "
						  "NULL AS opcfamilynsp, "
		"(SELECT amname FROM pg_catalog.pg_am WHERE oid = opcamid) AS amname "
						  "FROM pg_catalog.pg_opclass "
						  "WHERE oid = '%u'::pg_catalog.oid",
						  opcinfo->dobj.catId.oid);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query returned %d rows instead of one: %s\n",
				  ntups, query->data);
		exit_nicely();
	}

	i_opcintype = PQfnumber(res, "opcintype");
	i_opckeytype = PQfnumber(res, "opckeytype");
	i_opcdefault = PQfnumber(res, "opcdefault");
	i_opcfamily = PQfnumber(res, "opcfamily");
	i_opcfamilynsp = PQfnumber(res, "opcfamilynsp");
	i_amname = PQfnumber(res, "amname");

	/* opcintype may still be needed after we PQclear res */
	opcintype = pg_strdup(PQgetvalue(res, 0, i_opcintype));
	opckeytype = PQgetvalue(res, 0, i_opckeytype);
	opcdefault = PQgetvalue(res, 0, i_opcdefault);
	opcfamily = PQgetvalue(res, 0, i_opcfamily);
	opcfamilynsp = PQgetvalue(res, 0, i_opcfamilynsp);
	/* amname will still be needed after we PQclear res */
	amname = strdup(PQgetvalue(res, 0, i_amname));

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP OPERATOR CLASS %s",
					  fmtId(opcinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, ".%s",
					  fmtId(opcinfo->dobj.name));
	appendPQExpBuffer(delq, " USING %s;\n",
					  fmtId(amname));

	/* Build the fixed portion of the CREATE command */
	appendPQExpBuffer(q, "CREATE OPERATOR CLASS %s\n    ",
					  fmtId(opcinfo->dobj.name));
	if (strcmp(opcdefault, "t") == 0)
		appendPQExpBuffer(q, "DEFAULT ");
	appendPQExpBuffer(q, "FOR TYPE %s USING %s",
					  opcintype,
					  fmtId(amname));
	if (strlen(opcfamily) > 0 &&
		(strcmp(opcfamily, opcinfo->dobj.name) != 0 ||
		 strcmp(opcfamilynsp, opcinfo->dobj.namespace->dobj.name) != 0))
	{
		appendPQExpBuffer(q, " FAMILY ");
		if (strcmp(opcfamilynsp, opcinfo->dobj.namespace->dobj.name) != 0)
			appendPQExpBuffer(q, "%s.", fmtId(opcfamilynsp));
		appendPQExpBuffer(q, "%s", fmtId(opcfamily));
	}
	appendPQExpBuffer(q, " AS\n    ");

	needComma = false;

	if (strcmp(opckeytype, "-") != 0)
	{
		appendPQExpBuffer(q, "STORAGE %s",
						  opckeytype);
		needComma = true;
	}

	PQclear(res);

	/*
	 * Now fetch and print the OPERATOR entries (pg_amop rows).
	 */
	resetPQExpBuffer(query);

	if (g_fout->remoteVersion >= 80300)
	{
		/*
		 * Print only those opfamily members that are tied to the opclass by
		 * pg_depend entries.
		 */
		appendPQExpBuffer(query, "SELECT amopstrategy, amopreqcheck, "
						  "amopopr::pg_catalog.regoperator "
						  "FROM pg_catalog.pg_amop ao, pg_catalog.pg_depend "
		   "WHERE refclassid = 'pg_catalog.pg_opclass'::pg_catalog.regclass "
						  "AND refobjid = '%u'::pg_catalog.oid "
				   "AND classid = 'pg_catalog.pg_amop'::pg_catalog.regclass "
						  "AND objid = ao.oid "
						  "ORDER BY amopstrategy",
						  opcinfo->dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT amopstrategy, amopreqcheck, "
						  "amopopr::pg_catalog.regoperator "
						  "FROM pg_catalog.pg_amop "
						  "WHERE amopclaid = '%u'::pg_catalog.oid "
						  "ORDER BY amopstrategy",
						  opcinfo->dobj.catId.oid);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_amopstrategy = PQfnumber(res, "amopstrategy");
	i_amopreqcheck = PQfnumber(res, "amopreqcheck");
	i_amopopr = PQfnumber(res, "amopopr");

	for (i = 0; i < ntups; i++)
	{
		amopstrategy = PQgetvalue(res, i, i_amopstrategy);
		amopreqcheck = PQgetvalue(res, i, i_amopreqcheck);
		amopopr = PQgetvalue(res, i, i_amopopr);

		if (needComma)
			appendPQExpBuffer(q, " ,\n    ");

		appendPQExpBuffer(q, "OPERATOR %s %s",
						  amopstrategy, amopopr);
		if (strcmp(amopreqcheck, "t") == 0)
			appendPQExpBuffer(q, " RECHECK");

		needComma = true;
	}

	PQclear(res);

	/*
	 * Now fetch and print the FUNCTION entries (pg_amproc rows).
	 */
	resetPQExpBuffer(query);

	if (g_fout->remoteVersion >= 80300)
	{
		/*
		 * Print only those opfamily members that are tied to the opclass by
		 * pg_depend entries.
		 */
		appendPQExpBuffer(query, "SELECT amprocnum, "
						  "amproc::pg_catalog.regprocedure "
						"FROM pg_catalog.pg_amproc ap, pg_catalog.pg_depend "
		   "WHERE refclassid = 'pg_catalog.pg_opclass'::pg_catalog.regclass "
						  "AND refobjid = '%u'::pg_catalog.oid "
				 "AND classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass "
						  "AND objid = ap.oid "
						  "ORDER BY amprocnum",
						  opcinfo->dobj.catId.oid);
	}
	else
	{
		appendPQExpBuffer(query, "SELECT amprocnum, "
						  "amproc::pg_catalog.regprocedure "
						  "FROM pg_catalog.pg_amproc "
						  "WHERE amopclaid = '%u'::pg_catalog.oid "
						  "ORDER BY amprocnum",
						  opcinfo->dobj.catId.oid);
	}

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_amprocnum = PQfnumber(res, "amprocnum");
	i_amproc = PQfnumber(res, "amproc");

	for (i = 0; i < ntups; i++)
	{
		amprocnum = PQgetvalue(res, i, i_amprocnum);
		amproc = PQgetvalue(res, i, i_amproc);

		if (needComma)
			appendPQExpBuffer(q, " ,\n    ");

		appendPQExpBuffer(q, "FUNCTION %s %s",
						  amprocnum, amproc);

		needComma = true;
	}

	PQclear(res);

	/*
	 * If needComma is still false it means we haven't added anything after
	 * the AS keyword.  To avoid printing broken SQL, append a dummy STORAGE
	 * clause with the same datatype.  This isn't sanctioned by the
	 * documentation, but actually DefineOpClass will treat it as a no-op.
	 */
	if (!needComma)
		appendPQExpBuffer(q, "STORAGE %s", opcintype);

	appendPQExpBuffer(q, ";\n");

	appendPQExpBuffer(labelq, "OPERATOR CLASS %s",
					  fmtId(opcinfo->dobj.name));
	appendPQExpBuffer(labelq, " USING %s",
					  fmtId(amname));

	if (binary_upgrade)
		binary_upgrade_extension_member(q, &opcinfo->dobj, labelq->data);

	ArchiveEntry(fout, opcinfo->dobj.catId, opcinfo->dobj.dumpId,
				 opcinfo->dobj.name,
				 opcinfo->dobj.namespace->dobj.name,
				 NULL,
				 opcinfo->rolname,
				 false, "OPERATOR CLASS", q->data, delq->data, NULL,
				 opcinfo->dobj.dependencies, opcinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Operator Class Comments */
	dumpComment(fout, labelq->data,
				NULL, opcinfo->rolname,
				opcinfo->dobj.catId, 0, opcinfo->dobj.dumpId);

	free(opcintype);
	free(amname);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
}

/*
 * dumpOpfamily
 *	  write out a single operator family definition
 */
static void
dumpOpfamily(Archive *fout, OpfamilyInfo *opfinfo)
{
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer labelq;
	PGresult   *res;
	PGresult   *res_ops;
	PGresult   *res_procs;
	int			ntups;
	int			i_amname;
	int			i_amopstrategy;
	int			i_amopreqcheck;
	int			i_amopopr;
	int			i_amprocnum;
	int			i_amproc;
	int			i_amproclefttype;
	int			i_amprocrighttype;
	char	   *amname;
	char	   *amopstrategy;
	char	   *amopreqcheck;
	char	   *amopopr;
	char	   *amprocnum;
	char	   *amproc;
	char	   *amproclefttype;
	char	   *amprocrighttype;
	bool		needComma;
	int			i;

	/* Skip if not to be dumped */
	if (!opfinfo->dobj.dump || dataOnly)
		return;

	/*
	 * We want to dump the opfamily only if (1) it contains "loose" operators
	 * or functions, or (2) it contains an opclass with a different name or
	 * owner.  Otherwise it's sufficient to let it be created during creation
	 * of the contained opclass, and not dumping it improves portability of
	 * the dump.  Since we have to fetch the loose operators/funcs anyway, do
	 * that first.
	 */

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	labelq = createPQExpBuffer();

	/* Make sure we are in proper schema so regoperator works correctly */
	selectSourceSchema(opfinfo->dobj.namespace->dobj.name);

	/*
	 * Fetch only those opfamily members that are tied directly to the
	 * opfamily by pg_depend entries.
	 */
	appendPQExpBuffer(query, "SELECT amopstrategy, amopreqcheck, "
					  "amopopr::pg_catalog.regoperator "
					  "FROM pg_catalog.pg_amop ao, pg_catalog.pg_depend "
		  "WHERE refclassid = 'pg_catalog.pg_opfamily'::pg_catalog.regclass "
					  "AND refobjid = '%u'::pg_catalog.oid "
				   "AND classid = 'pg_catalog.pg_amop'::pg_catalog.regclass "
					  "AND objid = ao.oid "
					  "ORDER BY amopstrategy",
					  opfinfo->dobj.catId.oid);

	res_ops = PQexec(g_conn, query->data);
	check_sql_result(res_ops, g_conn, query->data, PGRES_TUPLES_OK);

	resetPQExpBuffer(query);

	appendPQExpBuffer(query, "SELECT amprocnum, "
					  "amproc::pg_catalog.regprocedure, "
					  "amproclefttype::pg_catalog.regtype, "
					  "amprocrighttype::pg_catalog.regtype "
					  "FROM pg_catalog.pg_amproc ap, pg_catalog.pg_depend "
		  "WHERE refclassid = 'pg_catalog.pg_opfamily'::pg_catalog.regclass "
					  "AND refobjid = '%u'::pg_catalog.oid "
				 "AND classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass "
					  "AND objid = ap.oid "
					  "ORDER BY amprocnum",
					  opfinfo->dobj.catId.oid);

	res_procs = PQexec(g_conn, query->data);
	check_sql_result(res_procs, g_conn, query->data, PGRES_TUPLES_OK);

	if (PQntuples(res_ops) == 0 && PQntuples(res_procs) == 0)
	{
		/* No loose members, so check contained opclasses */
		resetPQExpBuffer(query);

		appendPQExpBuffer(query, "SELECT 1 "
						  "FROM pg_catalog.pg_opclass c, pg_catalog.pg_opfamily f, pg_catalog.pg_depend "
						  "WHERE f.oid = '%u'::pg_catalog.oid "
			"AND refclassid = 'pg_catalog.pg_opfamily'::pg_catalog.regclass "
						  "AND refobjid = f.oid "
				"AND classid = 'pg_catalog.pg_opclass'::pg_catalog.regclass "
						  "AND objid = c.oid "
						  "AND (opcname != opfname OR opcnamespace != opfnamespace OR opcowner != opfowner) "
						  "LIMIT 1",
						  opfinfo->dobj.catId.oid);

		res = PQexec(g_conn, query->data);
		check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

		if (PQntuples(res) == 0)
		{
			/* no need to dump it, so bail out */
			PQclear(res);
			PQclear(res_ops);
			PQclear(res_procs);
			destroyPQExpBuffer(query);
			destroyPQExpBuffer(q);
			destroyPQExpBuffer(delq);
			destroyPQExpBuffer(labelq);
			return;
		}

		PQclear(res);
	}

	/* Get additional fields from the pg_opfamily row */
	resetPQExpBuffer(query);

	appendPQExpBuffer(query, "SELECT "
	 "(SELECT amname FROM pg_catalog.pg_am WHERE oid = opfmethod) AS amname "
					  "FROM pg_catalog.pg_opfamily "
					  "WHERE oid = '%u'::pg_catalog.oid",
					  opfinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query returned %d rows instead of one: %s\n",
				  ntups, query->data);
		exit_nicely();
	}

	i_amname = PQfnumber(res, "amname");

	/* amname will still be needed after we PQclear res */
	amname = strdup(PQgetvalue(res, 0, i_amname));

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP OPERATOR FAMILY %s",
					  fmtId(opfinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, ".%s",
					  fmtId(opfinfo->dobj.name));
	appendPQExpBuffer(delq, " USING %s;\n",
					  fmtId(amname));

	/* Build the fixed portion of the CREATE command */
	appendPQExpBuffer(q, "CREATE OPERATOR FAMILY %s",
					  fmtId(opfinfo->dobj.name));
	appendPQExpBuffer(q, " USING %s;\n",
					  fmtId(amname));

	PQclear(res);

	/* Do we need an ALTER to add loose members? */
	if (PQntuples(res_ops) > 0 || PQntuples(res_procs) > 0)
	{
		appendPQExpBuffer(q, "ALTER OPERATOR FAMILY %s",
						  fmtId(opfinfo->dobj.name));
		appendPQExpBuffer(q, " USING %s ADD\n    ",
						  fmtId(amname));

		needComma = false;

		/*
		 * Now fetch and print the OPERATOR entries (pg_amop rows).
		 */
		ntups = PQntuples(res_ops);

		i_amopstrategy = PQfnumber(res_ops, "amopstrategy");
		i_amopreqcheck = PQfnumber(res_ops, "amopreqcheck");
		i_amopopr = PQfnumber(res_ops, "amopopr");

		for (i = 0; i < ntups; i++)
		{
			amopstrategy = PQgetvalue(res_ops, i, i_amopstrategy);
			amopreqcheck = PQgetvalue(res_ops, i, i_amopreqcheck);
			amopopr = PQgetvalue(res_ops, i, i_amopopr);

			if (needComma)
				appendPQExpBuffer(q, " ,\n    ");

			appendPQExpBuffer(q, "OPERATOR %s %s",
							  amopstrategy, amopopr);
			if (strcmp(amopreqcheck, "t") == 0)
				appendPQExpBuffer(q, " RECHECK");

			needComma = true;
		}

		/*
		 * Now fetch and print the FUNCTION entries (pg_amproc rows).
		 */
		ntups = PQntuples(res_procs);

		i_amprocnum = PQfnumber(res_procs, "amprocnum");
		i_amproc = PQfnumber(res_procs, "amproc");
		i_amproclefttype = PQfnumber(res_procs, "amproclefttype");
		i_amprocrighttype = PQfnumber(res_procs, "amprocrighttype");

		for (i = 0; i < ntups; i++)
		{
			amprocnum = PQgetvalue(res_procs, i, i_amprocnum);
			amproc = PQgetvalue(res_procs, i, i_amproc);
			amproclefttype = PQgetvalue(res_procs, i, i_amproclefttype);
			amprocrighttype = PQgetvalue(res_procs, i, i_amprocrighttype);

			if (needComma)
				appendPQExpBuffer(q, " ,\n    ");

			appendPQExpBuffer(q, "FUNCTION %s (%s, %s) %s",
							  amprocnum, amproclefttype, amprocrighttype,
							  amproc);

			needComma = true;
		}

		appendPQExpBuffer(q, ";\n");
	}

	appendPQExpBuffer(labelq, "OPERATOR FAMILY %s",
					  fmtId(opfinfo->dobj.name));
	appendPQExpBuffer(labelq, " USING %s",
					  fmtId(amname));

	if (binary_upgrade)
		binary_upgrade_extension_member(q, &opfinfo->dobj, labelq->data);

	ArchiveEntry(fout, opfinfo->dobj.catId, opfinfo->dobj.dumpId,
				 opfinfo->dobj.name,
				 opfinfo->dobj.namespace->dobj.name,
				 NULL,
				 opfinfo->rolname,
				 false, "OPERATOR FAMILY", q->data, delq->data, NULL,
				 opfinfo->dobj.dependencies, opfinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Operator Family Comments */
	dumpComment(fout, labelq->data,
				NULL, opfinfo->rolname,
				opfinfo->dobj.catId, 0, opfinfo->dobj.dumpId);

	free(amname);
	PQclear(res_ops);
	PQclear(res_procs);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
}

/*
 * dumpConversion
 *	  write out a single conversion definition
 */
static void
dumpConversion(Archive *fout, ConvInfo *convinfo)
{
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer labelq;
	PGresult   *res;
	int			ntups;
	int			i_conname;
	int			i_conforencoding;
	int			i_contoencoding;
	int			i_conproc;
	int			i_condefault;
	const char *conname;
	const char *conforencoding;
	const char *contoencoding;
	const char *conproc;
	bool		condefault;

	/* Skip if not to be dumped */
	if (!convinfo->dobj.dump || dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	labelq = createPQExpBuffer();

	/* Make sure we are in proper schema */
	selectSourceSchema(convinfo->dobj.namespace->dobj.name);

	/* Get conversion-specific details */
	appendPQExpBuffer(query, "SELECT conname, "
		 "pg_catalog.pg_encoding_to_char(conforencoding) AS conforencoding, "
		   "pg_catalog.pg_encoding_to_char(contoencoding) AS contoencoding, "
					  "conproc, condefault "
					  "FROM pg_catalog.pg_conversion c "
					  "WHERE c.oid = '%u'::pg_catalog.oid",
					  convinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query returned %d rows instead of one: %s\n",
				  ntups, query->data);
		exit_nicely();
	}

	i_conname = PQfnumber(res, "conname");
	i_conforencoding = PQfnumber(res, "conforencoding");
	i_contoencoding = PQfnumber(res, "contoencoding");
	i_conproc = PQfnumber(res, "conproc");
	i_condefault = PQfnumber(res, "condefault");

	conname = PQgetvalue(res, 0, i_conname);
	conforencoding = PQgetvalue(res, 0, i_conforencoding);
	contoencoding = PQgetvalue(res, 0, i_contoencoding);
	conproc = PQgetvalue(res, 0, i_conproc);
	condefault = (PQgetvalue(res, 0, i_condefault)[0] == 't');

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP CONVERSION %s",
					  fmtId(convinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, ".%s;\n",
					  fmtId(convinfo->dobj.name));

	appendPQExpBuffer(q, "CREATE %sCONVERSION %s FOR ",
					  (condefault) ? "DEFAULT " : "",
					  fmtId(convinfo->dobj.name));
	appendStringLiteralAH(q, conforencoding, fout);
	appendPQExpBuffer(q, " TO ");
	appendStringLiteralAH(q, contoencoding, fout);
	/* regproc is automatically quoted in 7.3 and above */
	appendPQExpBuffer(q, " FROM %s;\n", conproc);

	appendPQExpBuffer(labelq, "CONVERSION %s", fmtId(convinfo->dobj.name));

	if (binary_upgrade)
		binary_upgrade_extension_member(q, &convinfo->dobj, labelq->data);

	ArchiveEntry(fout, convinfo->dobj.catId, convinfo->dobj.dumpId,
				 convinfo->dobj.name,
				 convinfo->dobj.namespace->dobj.name,
				 NULL,
				 convinfo->rolname,
				 false, "CONVERSION", q->data, delq->data, NULL,
				 convinfo->dobj.dependencies, convinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Conversion Comments */
	dumpComment(fout, labelq->data,
				convinfo->dobj.namespace->dobj.name, convinfo->rolname,
				convinfo->dobj.catId, 0, convinfo->dobj.dumpId);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
}

/*
 * format_aggregate_signature: generate aggregate name and argument list
 *
 * The argument type names are qualified if needed.  The aggregate name
 * is never qualified.
 */
static char *
format_aggregate_signature(AggInfo *agginfo, Archive *fout, bool honor_quotes)
{
	PQExpBufferData buf;
	int			j;

	initPQExpBuffer(&buf);
	if (honor_quotes)
		appendPQExpBuffer(&buf, "%s",
						  fmtId(agginfo->aggfn.dobj.name));
	else
		appendPQExpBuffer(&buf, "%s", agginfo->aggfn.dobj.name);

	if (agginfo->aggfn.nargs == 0)
		appendPQExpBuffer(&buf, "(*)");
	else
	{
		appendPQExpBuffer(&buf, "(");
		for (j = 0; j < agginfo->aggfn.nargs; j++)
		{
			char	   *typname;

			typname = getFormattedTypeName(agginfo->aggfn.argtypes[j], zeroAsOpaque);

			appendPQExpBuffer(&buf, "%s%s",
							  (j > 0) ? ", " : "",
							  typname);
			free(typname);
		}
		appendPQExpBuffer(&buf, ")");
	}
	return buf.data;
}

/*
 * dumpAgg
 *	  write out a single aggregate definition
 */
static void
dumpAgg(Archive *fout, AggInfo *agginfo)
{
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer labelq;
	PQExpBuffer details;
	char	   *aggsig;
	char	   *aggsig_tag;
	PGresult   *res;
	int			ntups;
	int			i_aggtransfn;
	int			i_aggfinalfn;
	int			i_aggsortop;
	int			i_aggtranstype;
	int			i_agginitval;
	int			i_aggprelimfn;
	int			i_convertok;
	int			i_aggordered;
	const char *aggtransfn;
	const char *aggfinalfn;
	const char *aggsortop;
	const char *aggtranstype;
	const char *agginitval;
	const char *aggprelimfn;
	bool		convertok;
	bool		aggordered;

	/* Skip if not to be dumped */
	if (!agginfo->aggfn.dobj.dump || dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	labelq = createPQExpBuffer();
	details = createPQExpBuffer();

	/* Make sure we are in proper schema */
	selectSourceSchema(agginfo->aggfn.dobj.namespace->dobj.name);

	appendPQExpBuffer(query, "SELECT aggtransfn, "
					  "aggfinalfn, aggtranstype::pg_catalog.regtype, "
					  "aggsortop::pg_catalog.regoperator, "
					  "agginitval, "
					  "%s, "
					  "'t'::boolean as convertok, "
					  "aggordered "
					  "from pg_catalog.pg_aggregate a, pg_catalog.pg_proc p "
					  "where a.aggfnoid = p.oid "
					  "and p.oid = '%u'::pg_catalog.oid",
					  (isGPbackend ? "aggprelimfn" : "NULL as aggprelimfn"),
					  agginfo->aggfn.dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query returned %d rows instead of one: %s\n",
				  ntups, query->data);
		exit_nicely();
	}

	i_aggtransfn = PQfnumber(res, "aggtransfn");
	i_aggfinalfn = PQfnumber(res, "aggfinalfn");
	i_aggsortop = PQfnumber(res, "aggsortop");
	i_aggtranstype = PQfnumber(res, "aggtranstype");
	i_agginitval = PQfnumber(res, "agginitval");
	i_aggprelimfn = PQfnumber(res, "aggprelimfn");
	i_convertok = PQfnumber(res, "convertok");
	i_aggordered = PQfnumber(res, "aggordered");

	aggtransfn = PQgetvalue(res, 0, i_aggtransfn);
	aggfinalfn = PQgetvalue(res, 0, i_aggfinalfn);
	aggsortop = PQgetvalue(res, 0, i_aggsortop);
	aggtranstype = PQgetvalue(res, 0, i_aggtranstype);
	agginitval = PQgetvalue(res, 0, i_agginitval);
	aggprelimfn = PQgetvalue(res, 0, i_aggprelimfn);
	convertok = (PQgetvalue(res, 0, i_convertok)[0] == 't');
	aggordered = (PQgetvalue(res, 0, i_aggordered)[0] == 't');

	aggsig = format_aggregate_signature(agginfo, fout, true);
	aggsig_tag = format_aggregate_signature(agginfo, fout, false);

	if (!convertok)
	{
		write_msg(NULL, "WARNING: aggregate function %s could not be dumped correctly for this database version; ignored\n",
				  aggsig);
		return;
	}

	/* If using 7.3's regproc or regtype, data is already quoted */
	appendPQExpBuffer(details, "    SFUNC = %s,\n    STYPE = %s",
					  aggtransfn,
					  aggtranstype);

	if (!PQgetisnull(res, 0, i_agginitval))
	{
		appendPQExpBuffer(details, ",\n    INITCOND = ");
		appendStringLiteralAH(details, agginitval, fout);
	}

	if (!PQgetisnull(res, 0, i_aggprelimfn))
	{
		if (strcmp(aggprelimfn, "-") != 0)
			appendPQExpBuffer(details, ",\n    PREFUNC = %s",
							  aggprelimfn);
	}

	if (strcmp(aggfinalfn, "-") != 0)
	{
		appendPQExpBuffer(details, ",\n    FINALFUNC = %s",
						  aggfinalfn);
	}

	aggsortop = convertOperatorReference(aggsortop);
	if (aggsortop)
	{
		appendPQExpBuffer(details, ",\n    SORTOP = %s",
						  aggsortop);
	}

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP AGGREGATE %s.%s;\n",
					  fmtId(agginfo->aggfn.dobj.namespace->dobj.name),
					  aggsig);

	appendPQExpBuffer(q, "CREATE %s %s (\n%s\n);\n",
					  aggordered == true ? "ORDERED AGGREGATE" : "AGGREGATE",
					  aggsig, details->data);

	appendPQExpBuffer(labelq, "AGGREGATE %s", aggsig);

	if (binary_upgrade)
		binary_upgrade_extension_member(q, &agginfo->aggfn.dobj, labelq->data);

	ArchiveEntry(fout, agginfo->aggfn.dobj.catId, agginfo->aggfn.dobj.dumpId,
				 aggsig_tag,
				 agginfo->aggfn.dobj.namespace->dobj.name,
				 NULL,
				 agginfo->aggfn.rolname,
				 false, "AGGREGATE", q->data, delq->data, NULL,
				 agginfo->aggfn.dobj.dependencies, agginfo->aggfn.dobj.nDeps,
				 NULL, NULL);

	/* Dump Aggregate Comments */
	dumpComment(fout, labelq->data,
			agginfo->aggfn.dobj.namespace->dobj.name, agginfo->aggfn.rolname,
				agginfo->aggfn.dobj.catId, 0, agginfo->aggfn.dobj.dumpId);

	/*
	 * Since there is no GRANT ON AGGREGATE syntax, we have to make the ACL
	 * command look like a function's GRANT; in particular this affects the
	 * syntax for zero-argument aggregates.
	 */
	free(aggsig);
	free(aggsig_tag);

	aggsig = format_function_signature(&agginfo->aggfn, true);
	aggsig_tag = format_function_signature(&agginfo->aggfn, false);

	dumpACL(fout, agginfo->aggfn.dobj.catId, agginfo->aggfn.dobj.dumpId,
			"FUNCTION",
			aggsig, aggsig_tag,
			agginfo->aggfn.dobj.namespace->dobj.name,
			agginfo->aggfn.rolname, agginfo->aggfn.proacl);

	free(aggsig);
	free(aggsig_tag);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(details);
}

/*
 * getFunctionName - retrieves a function name from an oid
 *
 */
static char *
getFunctionName(Oid oid)
{
	char	   *result;
	PQExpBuffer query;
	PGresult   *res;
	int			ntups;

	if (oid == InvalidOid)
	{
		return NULL;
	}

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "SELECT proname FROM pg_proc WHERE oid = %u;",oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query yielded %d rows instead of one: %s\n",
				  ntups, query->data);
		exit_nicely();
	}

	/* already quoted */
	result = strdup(PQgetvalue(res, 0, 0));

	PQclear(res);
	destroyPQExpBuffer(query);

	return result;
}

/*
 * dumpExtProtocol
 *	  write out a single external protocol definition
 */
static void
dumpExtProtocol(Archive *fout, ExtProtInfo *ptcinfo)
{
#define FCOUNT	3
#define READFN_IDX 0
#define WRITEFN_IDX 1
#define VALIDFN_IDX 2

	typedef struct
	{
		Oid oid; 				/* func's oid */
		char* name; 			/* func name */
		FuncInfo* pfuncinfo; 	/* FuncInfo ptr */
		bool dumpable; 			/* should we dump this function */
		bool internal;			/* is it an internal function */
	} ProtoFunc;

	ProtoFunc	protoFuncs[FCOUNT];

	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer	nsq;
	char	   *prev_ns;
	char	   *namecopy;
	int			i;
	bool		has_internal = false;

	/* Skip if not to be dumped */
	if (!ptcinfo->dobj.dump || dataOnly)
		return;

	/* init and fill the protoFuncs array */
	memset(protoFuncs, 0, sizeof(protoFuncs));
	protoFuncs[READFN_IDX].oid = ptcinfo->ptcreadid;
	protoFuncs[WRITEFN_IDX].oid = ptcinfo->ptcwriteid;
	protoFuncs[VALIDFN_IDX].oid = ptcinfo->ptcvalidid;

	for (i = 0; i < FCOUNT; i++)
	{
		if (protoFuncs[i].oid == InvalidOid)
		{
			protoFuncs[i].dumpable = false;
			protoFuncs[i].internal = true;
			/*
			 * We have at least one internal function, signal that we need the
			 * public schema in the search_path
			 */
			has_internal = true;
		}
		else
		{
			protoFuncs[i].pfuncinfo = findFuncByOid(protoFuncs[i].oid);
			if (protoFuncs[i].pfuncinfo != NULL)
			{
				protoFuncs[i].dumpable = true;
				protoFuncs[i].name = strdup(protoFuncs[i].pfuncinfo->dobj.name);
				protoFuncs[i].internal = false;
			}
			else
				protoFuncs[i].internal = true;
		}
	}

	/* if all funcs are internal then we do not need to dump this protocol */
	if (protoFuncs[READFN_IDX].internal && protoFuncs[WRITEFN_IDX].internal
			&& protoFuncs[VALIDFN_IDX].internal)
		return;

	/* obtain the function name for internal functions (if any) */
	for (i = 0; i < FCOUNT; i++)
	{
		if (protoFuncs[i].internal && protoFuncs[i].oid)
		{
			protoFuncs[i].name = getFunctionName(protoFuncs[i].oid);
			if (protoFuncs[i].name)
				protoFuncs[i].dumpable = true;
		}
	}

	nsq = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	/*
	 * Since the function parameters to the external protocol cannot be fully
	 * qualified with namespace, we must ensure that we have the search_path
	 * set with the namespaces of the referenced functions. We only need the
	 * dump file to have the search_path so inject a SET search_path = .. ;
	 * into the output stream instead of calling selectSourceSchema().
	 */
	prev_ns = NULL;
	for (i = 0; i < FCOUNT; i++)
	{
		if (!protoFuncs[i].pfuncinfo || protoFuncs[i].internal)
			continue;

		if (prev_ns && strcmp(prev_ns, protoFuncs[i].pfuncinfo->dobj.namespace->dobj.name) == 0)
			continue;

		appendPQExpBuffer(nsq, "%s%s", (prev_ns ? "," : ""), protoFuncs[i].pfuncinfo->dobj.namespace->dobj.name);
		prev_ns = protoFuncs[i].pfuncinfo->dobj.namespace->dobj.name;

		/*
		 * If we are adding public to the search_path, then we don't need to do
		 * so again for any internal functions
		 */
		if (strcmp(prev_ns, "public") == 0)
			has_internal = false;
	}

	if (prev_ns)
	{
		appendPQExpBufferStr(q, "-- Set the search_path required to look up the functions\n");
		appendPQExpBuffer(q, "SET search_path = %s%s;\n\n",
						  nsq->data, (has_internal ? ", public" : ""));
	}
	destroyPQExpBuffer(nsq);

	appendPQExpBuffer(q, "CREATE %s PROTOCOL %s (",
			ptcinfo->ptctrusted == true ? "TRUSTED" : "",
			fmtId(ptcinfo->dobj.name));

	if (protoFuncs[READFN_IDX].dumpable)
	{
		appendPQExpBuffer(q, " readfunc = '%s'%s",
						  protoFuncs[READFN_IDX].name,
						  (protoFuncs[WRITEFN_IDX].dumpable ? "," : ""));
	}

	if (protoFuncs[WRITEFN_IDX].dumpable)
	{
		appendPQExpBuffer(q, " writefunc = '%s'%s",
						  protoFuncs[WRITEFN_IDX].name,
					      (protoFuncs[VALIDFN_IDX].dumpable ? "," : ""));
	}

	if (protoFuncs[VALIDFN_IDX].dumpable)
	{
		appendPQExpBuffer(q, " validatorfunc = '%s'",
						  protoFuncs[VALIDFN_IDX].name);
	}
	appendPQExpBufferStr(q, ");\n");

	appendPQExpBuffer(delq, "DROP PROTOCOL %s;\n",
					  fmtId(ptcinfo->dobj.name));

	ArchiveEntry(fout, ptcinfo->dobj.catId, ptcinfo->dobj.dumpId,
				 ptcinfo->dobj.name,
				 NULL,
				 NULL,
				 ptcinfo->ptcowner,
				 false, "PROTOCOL",
				 q->data, delq->data, NULL,
				 ptcinfo->dobj.dependencies, ptcinfo->dobj.nDeps,
				 NULL, NULL);

	/* Handle the ACL */
	namecopy = strdup(fmtId(ptcinfo->dobj.name));
	dumpACL(fout, ptcinfo->dobj.catId, ptcinfo->dobj.dumpId,
			"PROTOCOL",
			namecopy, ptcinfo->dobj.name,
			NULL, ptcinfo->ptcowner,
			ptcinfo->ptcacl);
	free(namecopy);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);

	for (i = 0; i < FCOUNT; i++)
	{
		if (protoFuncs[i].name)
			free(protoFuncs[i].name);
	}
}

/*
 * dumpTSParser
 *	  write out a single text search parser
 */
static void
dumpTSParser(Archive *fout, TSParserInfo *prsinfo)
{
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer labelq;

	/* Skip if not to be dumped */
	if (!prsinfo->dobj.dump || dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	labelq = createPQExpBuffer();

	/* Make sure we are in proper schema */
	selectSourceSchema(prsinfo->dobj.namespace->dobj.name);

	appendPQExpBuffer(q, "CREATE TEXT SEARCH PARSER %s (\n",
					  fmtId(prsinfo->dobj.name));

	appendPQExpBuffer(q, "    START = %s,\n",
					  convertTSFunction(prsinfo->prsstart));
	appendPQExpBuffer(q, "    GETTOKEN = %s,\n",
					  convertTSFunction(prsinfo->prstoken));
	appendPQExpBuffer(q, "    END = %s,\n",
					  convertTSFunction(prsinfo->prsend));
	if (prsinfo->prsheadline != InvalidOid)
		appendPQExpBuffer(q, "    HEADLINE = %s,\n",
						  convertTSFunction(prsinfo->prsheadline));
	appendPQExpBuffer(q, "    LEXTYPES = %s );\n",
					  convertTSFunction(prsinfo->prslextype));

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP TEXT SEARCH PARSER %s",
					  fmtId(prsinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, ".%s;\n",
					  fmtId(prsinfo->dobj.name));

	ArchiveEntry(fout, prsinfo->dobj.catId, prsinfo->dobj.dumpId,
				 prsinfo->dobj.name,
				 prsinfo->dobj.namespace->dobj.name,
				 NULL,
				 "",
				 false, "TEXT SEARCH PARSER", q->data, delq->data, NULL,
				 prsinfo->dobj.dependencies, prsinfo->dobj.nDeps,
				 NULL, NULL);

	appendPQExpBuffer(labelq, "TEXT SEARCH PARSER %s",
					  fmtId(prsinfo->dobj.name));

	if (binary_upgrade)
		binary_upgrade_extension_member(q, &prsinfo->dobj, labelq->data);

	/* Dump Parser Comments */
	dumpComment(fout, labelq->data,
				NULL, "",
				prsinfo->dobj.catId, 0, prsinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
}

/*
 * dumpTSDictionary
 *	  write out a single text search dictionary
 */
static void
dumpTSDictionary(Archive *fout, TSDictInfo *dictinfo)
{
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer labelq;
	PQExpBuffer query;
	PGresult   *res;
	int			ntups;
	char	   *nspname;
	char	   *tmplname;

	/* Skip if not to be dumped */
	if (!dictinfo->dobj.dump || dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	labelq = createPQExpBuffer();
	query = createPQExpBuffer();

	/* Fetch name and namespace of the dictionary's template */
	selectSourceSchema("pg_catalog");
	appendPQExpBuffer(query, "SELECT nspname, tmplname "
					  "FROM pg_ts_template p, pg_namespace n "
					  "WHERE p.oid = '%u' AND n.oid = tmplnamespace",
					  dictinfo->dicttemplate);
	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query returned %d rows instead of one: %s\n",
				  ntups, query->data);
		exit_nicely();
	}
	nspname = PQgetvalue(res, 0, 0);
	tmplname = PQgetvalue(res, 0, 1);

	/* Make sure we are in proper schema */
	selectSourceSchema(dictinfo->dobj.namespace->dobj.name);

	appendPQExpBuffer(q, "CREATE TEXT SEARCH DICTIONARY %s (\n",
					  fmtId(dictinfo->dobj.name));

	appendPQExpBuffer(q, "    TEMPLATE = ");
	if (strcmp(nspname, dictinfo->dobj.namespace->dobj.name) != 0)
		appendPQExpBuffer(q, "%s.", fmtId(nspname));
	appendPQExpBuffer(q, "%s", fmtId(tmplname));

	PQclear(res);

	/* the dictinitoption can be dumped straight into the command */
	if (dictinfo->dictinitoption)
		appendPQExpBuffer(q, ",\n    %s", dictinfo->dictinitoption);

	appendPQExpBuffer(q, " );\n");

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP TEXT SEARCH DICTIONARY %s",
					  fmtId(dictinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, ".%s;\n",
					  fmtId(dictinfo->dobj.name));

	appendPQExpBuffer(labelq, "TEXT SEARCH DICTIONARY %s",
					  fmtId(dictinfo->dobj.name));

	ArchiveEntry(fout, dictinfo->dobj.catId, dictinfo->dobj.dumpId,
				 dictinfo->dobj.name,
				 dictinfo->dobj.namespace->dobj.name,
				 NULL,
				 dictinfo->rolname,
				 false, "TEXT SEARCH DICTIONARY", q->data, delq->data, NULL,
				 dictinfo->dobj.dependencies, dictinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Dictionary Comments */
	dumpComment(fout, labelq->data,
				NULL, dictinfo->rolname,
				dictinfo->dobj.catId, 0, dictinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(query);
}

/*
 * dumpTSTemplate
 *	  write out a single text search template
 */
static void
dumpTSTemplate(Archive *fout, TSTemplateInfo *tmplinfo)
{
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer labelq;

	/* Skip if not to be dumped */
	if (!tmplinfo->dobj.dump || dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	labelq = createPQExpBuffer();

	/* Make sure we are in proper schema */
	selectSourceSchema(tmplinfo->dobj.namespace->dobj.name);

	appendPQExpBuffer(q, "CREATE TEXT SEARCH TEMPLATE %s (\n",
					  fmtId(tmplinfo->dobj.name));

	if (tmplinfo->tmplinit != InvalidOid)
		appendPQExpBuffer(q, "    INIT = %s,\n",
						  convertTSFunction(tmplinfo->tmplinit));
	appendPQExpBuffer(q, "    LEXIZE = %s );\n",
					  convertTSFunction(tmplinfo->tmpllexize));

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP TEXT SEARCH TEMPLATE %s",
					  fmtId(tmplinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, ".%s;\n",
					  fmtId(tmplinfo->dobj.name));

	appendPQExpBuffer(labelq, "TEXT SEARCH TEMPLATE %s",
					  fmtId(tmplinfo->dobj.name));

	ArchiveEntry(fout, tmplinfo->dobj.catId, tmplinfo->dobj.dumpId,
				 tmplinfo->dobj.name,
				 tmplinfo->dobj.namespace->dobj.name,
				 NULL,
				 "",
				 false, "TEXT SEARCH TEMPLATE", q->data, delq->data, NULL,
				 tmplinfo->dobj.dependencies, tmplinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Template Comments */
	dumpComment(fout, labelq->data,
				NULL, "",
				tmplinfo->dobj.catId, 0, tmplinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
}

/*
 * dumpTSConfig
 *	  write out a single text search configuration
 */
static void
dumpTSConfig(Archive *fout, TSConfigInfo *cfginfo)
{
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer labelq;
	PQExpBuffer query;
	PGresult   *res;
	char	   *nspname;
	char	   *prsname;
	int			ntups,
				i;
	int			i_tokenname;
	int			i_dictname;

	/* Skip if not to be dumped */
	if (!cfginfo->dobj.dump || dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	labelq = createPQExpBuffer();
	query = createPQExpBuffer();

	/* Fetch name and namespace of the config's parser */
	selectSourceSchema("pg_catalog");
	appendPQExpBuffer(query, "SELECT nspname, prsname "
					  "FROM pg_ts_parser p, pg_namespace n "
					  "WHERE p.oid = '%u' AND n.oid = prsnamespace",
					  cfginfo->cfgparser);
	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query returned %d rows instead of one: %s\n",
				  ntups, query->data);
		exit_nicely();
	}
	nspname = PQgetvalue(res, 0, 0);
	prsname = PQgetvalue(res, 0, 1);

	/* Make sure we are in proper schema */
	selectSourceSchema(cfginfo->dobj.namespace->dobj.name);

	appendPQExpBuffer(q, "CREATE TEXT SEARCH CONFIGURATION %s (\n",
					  fmtId(cfginfo->dobj.name));

	appendPQExpBuffer(q, "    PARSER = ");
	if (strcmp(nspname, cfginfo->dobj.namespace->dobj.name) != 0)
		appendPQExpBuffer(q, "%s.", fmtId(nspname));
	appendPQExpBuffer(q, "%s );\n", fmtId(prsname));

	PQclear(res);

	resetPQExpBuffer(query);
	appendPQExpBuffer(query,
					  "SELECT \n"
					  "  ( SELECT alias FROM pg_catalog.ts_token_type('%u'::pg_catalog.oid) AS t \n"
					  "    WHERE t.tokid = m.maptokentype ) AS tokenname, \n"
					  "  m.mapdict::pg_catalog.regdictionary AS dictname \n"
					  "FROM pg_catalog.pg_ts_config_map AS m \n"
					  "WHERE m.mapcfg = '%u' \n"
					  "ORDER BY m.mapcfg, m.maptokentype, m.mapseqno",
					  cfginfo->cfgparser, cfginfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);
	ntups = PQntuples(res);

	i_tokenname = PQfnumber(res, "tokenname");
	i_dictname = PQfnumber(res, "dictname");

	for (i = 0; i < ntups; i++)
	{
		char	   *tokenname = PQgetvalue(res, i, i_tokenname);
		char	   *dictname = PQgetvalue(res, i, i_dictname);

		if (i == 0 ||
			strcmp(tokenname, PQgetvalue(res, i - 1, i_tokenname)) != 0)
		{
			/* starting a new token type, so start a new command */
			if (i > 0)
				appendPQExpBuffer(q, ";\n");
			appendPQExpBuffer(q, "\nALTER TEXT SEARCH CONFIGURATION %s\n",
							  fmtId(cfginfo->dobj.name));
			/* tokenname needs quoting, dictname does NOT */
			appendPQExpBuffer(q, "    ADD MAPPING FOR %s WITH %s",
							  fmtId(tokenname), dictname);
		}
		else
			appendPQExpBuffer(q, ", %s", dictname);
	}

	if (ntups > 0)
		appendPQExpBuffer(q, ";\n");

	PQclear(res);

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "DROP TEXT SEARCH CONFIGURATION %s",
					  fmtId(cfginfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, ".%s;\n",
					  fmtId(cfginfo->dobj.name));

	appendPQExpBuffer(labelq, "TEXT SEARCH CONFIGURATION %s",
					  fmtId(cfginfo->dobj.name));

	ArchiveEntry(fout, cfginfo->dobj.catId, cfginfo->dobj.dumpId,
				 cfginfo->dobj.name,
				 cfginfo->dobj.namespace->dobj.name,
				 NULL,
				 cfginfo->rolname,
			   false, "TEXT SEARCH CONFIGURATION", q->data, delq->data, NULL,
				 cfginfo->dobj.dependencies, cfginfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Configuration Comments */
	dumpComment(fout, labelq->data,
				NULL, cfginfo->rolname,
				cfginfo->dobj.catId, 0, cfginfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(query);
}


/*----------
 * Write out grant/revoke information
 *
 * 'objCatId' is the catalog ID of the underlying object.
 * 'objDumpId' is the dump ID of the underlying object.
 * 'type' must be TABLE, FUNCTION, LANGUAGE, SCHEMA, DATABASE, or TABLESPACE.
 * 'name' is the formatted name of the object.	Must be quoted etc. already.
 * 'tag' is the tag for the archive entry (typ. unquoted name of object).
 * 'nspname' is the namespace the object is in (NULL if none).
 * 'owner' is the owner, NULL if there is no owner (for languages).
 * 'acls' is the string read out of the fooacl system catalog field;
 * it will be parsed here.
 *----------
 */
static void
dumpACL(Archive *fout, CatalogId objCatId, DumpId objDumpId,
		const char *type, const char *name,
		const char *tag, const char *nspname, const char *owner,
		const char *acls)
{
	PQExpBuffer sql;

	/* Do nothing if ACL dump is not enabled */
	if (dataOnly || aclsSkip)
		return;

	sql = createPQExpBuffer();

	if (!buildACLCommands(name, type, acls, owner, fout->remoteVersion, sql))
	{
		write_msg(NULL, "could not parse ACL list (%s) for object \"%s\" (%s)\n",
				  acls, name, type);
		exit_nicely();
	}

	if (sql->len > 0)
		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 tag, nspname,
					 NULL,
					 owner ? owner : "",
					 false, "ACL", sql->data, "", NULL,
					 &(objDumpId), 1,
					 NULL, NULL);

	destroyPQExpBuffer(sql);
}

/*
 * dumpTable
 *	  write out to fout the declarations (not data) of a user-defined table
 */
static void
dumpTable(Archive *fout, TableInfo *tbinfo)
{
	char	   *namecopy;

	if (tbinfo->dobj.dump)
	{
		if (tbinfo->relkind == RELKIND_SEQUENCE)
			dumpSequence(fout, tbinfo);
		else if (!dataOnly)
			dumpTableSchema(fout, tbinfo);

		/* Handle the ACL here */
		namecopy = strdup(fmtId(tbinfo->dobj.name));
		dumpACL(fout, tbinfo->dobj.catId, tbinfo->dobj.dumpId,
				(tbinfo->relkind == RELKIND_SEQUENCE) ? "SEQUENCE" : "TABLE",
				namecopy, tbinfo->dobj.name,
				tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
				tbinfo->relacl);
		free(namecopy);
	}
}

static void
dumpExternal(TableInfo *tbinfo, PQExpBuffer query, PQExpBuffer q, PQExpBuffer delq)
{
		PGresult   *res;
		char	   *urilocations;
		char	   *execlocations;
		char	   *location;
		char	   *fmttype;
		char	   *fmtopts;
		char	   *command = NULL;
		char	   *rejlim;
		char	   *rejlimtype;
		char	   *errnspname;
		char	   *errtblname;
		char	   *extencoding;
		char	   *writable = NULL;
		char	   *tmpstring = NULL;
		char 	   *tabfmt = NULL;
		char	   *customfmt = NULL;
		bool		isweb = false;
		bool		iswritable = false;
		char	   *options;
		bool		gpdb5OrLater = isGPDB5000OrLater();
		char	   *on_clause;

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delq, "DROP EXTERNAL TABLE %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(delq, "%s;\n",
						  fmtId(tbinfo->dobj.name));

		/* Now get required information from pg_exttable */
		if (gpdb5OrLater)
		{
			appendPQExpBuffer(query,
					"SELECT x.urilocation, x.execlocation, x.fmttype, x.fmtopts, x.command, "
						   "x.rejectlimit, x.rejectlimittype, "
						   "(SELECT relname "
							"FROM pg_catalog.pg_class "
							"WHERE Oid=x.fmterrtbl) AS errtblname, "
						   "x.fmterrtbl = x.reloid AS errortofile , "
						   "pg_catalog.pg_encoding_to_char(x.encoding), "
						   "x.writable, "
						   "array_to_string(ARRAY( "
						   "SELECT pg_catalog.quote_ident(option_name) || ' ' || "
						   "pg_catalog.quote_literal(option_value) "
						   "FROM pg_options_to_table(x.options) "
						   "ORDER BY option_name"
						   "), E',\n    ') AS options "
					"FROM pg_catalog.pg_exttable x, pg_catalog.pg_class c "
					"WHERE x.reloid = c.oid AND c.oid = '%u'::oid ", tbinfo->dobj.catId.oid);
		}
		else if (g_fout->remoteVersion >= 80214)
		{
			appendPQExpBuffer(query,
					"SELECT x.location, "
						   "CASE WHEN x.command <> '' THEN x.location "
								"ELSE '{ALL_SEGMENTS}' "
						   "END AS execlocation, "
						   "x.fmttype, x.fmtopts, x.command, "
						   "x.rejectlimit, x.rejectlimittype, "
						   "n.nspname AS errnspname, d.relname AS errtblname, "
						   "pg_catalog.pg_encoding_to_char(x.encoding), "
						   "x.writable, null AS options "
					"FROM pg_catalog.pg_class c "
					"JOIN pg_catalog.pg_exttable x ON ( c.oid = x.reloid ) "
					"LEFT JOIN pg_catalog.pg_class d ON ( d.oid = x.fmterrtbl ) "
					"LEFT JOIN pg_catalog.pg_namespace n ON ( n.oid = d.relnamespace ) "
					"WHERE c.oid = '%u'::oid ",
					tbinfo->dobj.catId.oid);
		}
		else if (g_fout->remoteVersion >= 80205)
		{

			appendPQExpBuffer(query,
					"SELECT x.location, "
						   "CASE WHEN x.command <> '' THEN x.location "
								"ELSE '{ALL_SEGMENTS}' "
						   "END AS execlocation, "
						   "x.fmttype, x.fmtopts, x.command, "
						   "x.rejectlimit, x.rejectlimittype, "
						   "n.nspname AS errnspname, d.relname AS errtblname, "
						   "pg_catalog.pg_encoding_to_char(x.encoding), "
						   "null as writable, null as options "
					"FROM pg_catalog.pg_class c "
					"JOIN pg_catalog.pg_exttable x ON ( c.oid = x.reloid ) "
					"LEFT JOIN pg_catalog.pg_class d ON ( d.oid = x.fmterrtbl ) "
					"LEFT JOIN pg_catalog.pg_namespace n ON ( n.oid = d.relnamespace ) "
					"WHERE c.oid = '%u'::oid ",
					tbinfo->dobj.catId.oid);
		}
		else
		{
			/* not SREH and encoding colums yet */
			appendPQExpBuffer(query,
					"SELECT x.location, "
						   "CASE WHEN x.command <> '' THEN x.location "
								"ELSE '{ALL_SEGMENTS}' "
						   "END AS execlocation, "
						   "x.fmttype, x.fmtopts, x.command, "
						   "-1 as rejectlimit, null as rejectlimittype,"
						   "null as errnspname, null as errtblname, "
						   "null as encoding, null as writable, "
						   "null as options "
					"FROM pg_catalog.pg_exttable x, pg_catalog.pg_class c "
					"WHERE x.reloid = c.oid AND c.oid = '%u'::oid",
					tbinfo->dobj.catId.oid);
		}

		res = PQexec(g_conn, query->data);
		check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

		if (PQntuples(res) != 1)
		{
			if (PQntuples(res) < 1)
				write_msg(NULL, "query to obtain definition of external table "
						  "\"%s\" returned no data\n",
						  tbinfo->dobj.name);
			else
				write_msg(NULL, "query to obtain definition of external table "
						  "\"%s\" returned more than one definition\n",
						  tbinfo->dobj.name);
			exit_nicely();

		}


		urilocations = PQgetvalue(res, 0, 0);
		execlocations = PQgetvalue(res, 0, 1);
		fmttype = PQgetvalue(res, 0, 2);
		fmtopts = PQgetvalue(res, 0, 3);
		command = PQgetvalue(res, 0, 4);
		rejlim = PQgetvalue(res, 0, 5);
		rejlimtype = PQgetvalue(res, 0, 6);
		errnspname = PQgetvalue(res, 0, 7);
		errtblname = PQgetvalue(res, 0, 8);
		extencoding = PQgetvalue(res, 0, 9);
		writable = PQgetvalue(res, 0, 10);
		options = PQgetvalue(res, 0, 11);

		on_clause = execlocations;

		if ((command && strlen(command) > 0) ||
			(strncmp(urilocations + 1, "http", strlen("http")) == 0))
			isweb = true;

		if (writable && writable[0] == 't')
			iswritable = true;

		appendPQExpBuffer(q, "CREATE %sEXTERNAL %sTABLE %s (",
						  (iswritable ? "WRITABLE " : ""),
						  (isweb ? "WEB " : ""),
						  fmtId(tbinfo->dobj.name));

		int actual_atts = 0;
		int j;
		for (j = 0; j < tbinfo->numatts; j++)
		{
			/* Is the attribute not dropped? */
			if (shouldPrintColumn(tbinfo, j))
			{
				/* Format properly if not first attr */
				if (actual_atts > 0)
					appendPQExpBufferChar(q, ',');
				appendPQExpBufferStr(q, "\n    ");

				/* Attribute name */
				appendPQExpBuffer(q, "%s ", fmtId(tbinfo->attnames[j]));

				/* Attribute type */
				appendPQExpBufferStr(q, tbinfo->atttypnames[j]);

				actual_atts++;
			}
		}

		appendPQExpBufferStr(q, "\n)");

		if (command && strlen(command) > 0)
		{
			/* add EXECUTE clause */
			tmpstring = escape_backslashes(command, true);
			appendPQExpBuffer(q, " EXECUTE E'%s' ", tmpstring);
			free(tmpstring);
			tmpstring = NULL;
		}
		else
		{
			/* add LOCATION clause, remove '{"' and '"}' */
			urilocations[strlen(urilocations) - 1] = '\0';
			urilocations++;

			/* the URI of custom protocol will contains \"\" and need to be removed */

			location = nextToken(&urilocations, ",");

			if (location[0] == '\"')
			{
				location++;
				location[strlen(location) - 1] = '\0';
			}
			appendPQExpBuffer(q, " LOCATION (\n    '%s'", location);
			for (; (location = nextToken(&urilocations, ",")) != NULL;)
			{
				if (location[0] == '\"')
				{
					location++;
					location[strlen(location) - 1] = '\0';
				}
				appendPQExpBuffer(q, ",\n    '%s'", location);
			}
			appendPQExpBufferStr(q, "\n) ");
		}

		/*
		 * Add ON clause (unless WRITABLE table, which doesn't allow ON).
		 * ON clauses were up until 5.0 supported only on EXECUTE, in 5.0
		 * and thereafter they are allowed on all external tables.
		 */
		if (!iswritable)
		{
			/* remove curly braces */
			on_clause[strlen(on_clause) - 1] = '\0';
			on_clause++;

			if (strncmp(on_clause, "HOST:", strlen("HOST:")) == 0)
				appendPQExpBuffer(q, "ON HOST '%s' ", on_clause + strlen("HOST:"));
			else if (strncmp(on_clause, "PER_HOST", strlen("PER_HOST")) == 0)
				appendPQExpBufferStr(q, "ON HOST ");
			else if (strncmp(on_clause, "MASTER_ONLY", strlen("MASTER_ONLY")) == 0)
				appendPQExpBufferStr(q, "ON MASTER ");
			else if (strncmp(on_clause, "SEGMENT_ID:", strlen("SEGMENT_ID:")) == 0)
				appendPQExpBuffer(q, "ON SEGMENT %s ", on_clause + strlen("SEGMENT_ID:"));
			else if (strncmp(on_clause, "TOTAL_SEGS:", strlen("TOTAL_SEGS:")) == 0)
				appendPQExpBuffer(q, "ON %s ", on_clause + strlen("TOTAL_SEGS:"));
			else if (strncmp(on_clause, "ALL_SEGMENTS", strlen("ALL_SEGMENTS")) == 0)
				appendPQExpBufferStr(q, "ON ALL ");
			else
			{
				write_msg(NULL, "illegal ON clause catalog information \"%s\" "
						  "for command '%s' on table \"%s\"\n",
						  on_clause, command, fmtId(tbinfo->dobj.name));
				exit_nicely();
			}
		}
		appendPQExpBufferChar(q, '\n');

		/* add FORMAT clause */
		tmpstring = escape_fmtopts_string((const char *) fmtopts);

		switch (fmttype[0])
		{
			case 't':
				tabfmt = "text";
				break;
			case 'b':
				/*
				 * b denotes that a custom format is used.
				 * the fmtopts string should be formatted as:
				 * a1 = 'val1',...,an = 'valn'
				 *
				 */
				tabfmt = "custom";
				customfmt = custom_fmtopts_string(tmpstring);
				break;
			case 'a':
				tabfmt = "avro";
				customfmt = custom_fmtopts_string(tmpstring);
				break;
			case 'p':
				tabfmt = "parquet";
				customfmt = custom_fmtopts_string(tmpstring);
				break;	
			default:
				tabfmt = "csv";
		}
		appendPQExpBuffer(q, "FORMAT '%s' (%s)\n",
						  tabfmt,
						  customfmt ? customfmt : tmpstring);
		free(tmpstring);
		tmpstring = NULL;
		if (customfmt)
		{
			free(customfmt);
			customfmt = NULL;
		}

		if (options && options[0] != '\0')
		{
			appendPQExpBuffer(q, "OPTIONS (\n %s\n )\n", options);
		}

		if (g_fout->remoteVersion >= 80205)
		{
			/* add ENCODING clause */
			appendPQExpBuffer(q, "ENCODING '%s'", extencoding);

			/* add Single Row Error Handling clause (if any) */
			if (rejlim && strlen(rejlim) > 0)
			{
				appendPQExpBufferChar(q, '\n');

				/*
				 * Error tables were removed in 5.0 and replaced with file
				 * error logging. The catalog syntax for identifying error
				 * logging is however still using the pg_exttable.fmterrtbl
				 * attribute so we use the errtblname for emitting LOG ERRORS.
				 */
				if (errtblname && strlen(errtblname) > 0)
					appendPQExpBufferStr(q, "LOG ERRORS ");

				/* reject limit */
				appendPQExpBuffer(q, "SEGMENT REJECT LIMIT %s", rejlim);

				/* reject limit type */
				if (rejlimtype[0] == 'r')
					appendPQExpBufferStr(q, " ROWS");
				else
					appendPQExpBufferStr(q, " PERCENT");
			}
		}

		/* DISTRIBUTED BY clause (if WRITABLE table) */
		if (iswritable)
			addDistributedBy(q, tbinfo, actual_atts);

		appendPQExpBufferStr(q, ";\n");

		PQclear(res);
}

/*
 * dumpTableSchema
 *	  write the declaration (not data) of one user-defined table or view
 */
static void
dumpTableSchema(Archive *fout, TableInfo *tbinfo)
{
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer labelq = createPQExpBuffer();
	PGresult   *res;
	int			numParents;
	TableInfo **parents;
	int			actual_atts;	/* number of attrs in this CREATE statment */
	char	   *reltypename;
	char	   *storage;
	int			j,
				k;
	bool		isPartitioned = false;

	/* Make sure we are in proper schema */
	selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

	/* Is it a table or a view? */
	if (tbinfo->relkind == RELKIND_VIEW)
	{
		char	   *viewdef;

		reltypename = "VIEW";

		/* Fetch the view definition */
		appendPQExpBuffer(query,
		 "SELECT pg_catalog.pg_get_viewdef('%u'::pg_catalog.oid) as viewdef",
						  tbinfo->dobj.catId.oid);

		res = PQexec(g_conn, query->data);
		check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

		if (PQntuples(res) != 1)
		{
			if (PQntuples(res) < 1)
				write_msg(NULL, "query to obtain definition of view \"%s\" returned no data\n",
						  tbinfo->dobj.name);
			else
				write_msg(NULL, "query to obtain definition of view \"%s\" returned more than one definition\n",
						  tbinfo->dobj.name);
			exit_nicely();
		}

		viewdef = PQgetvalue(res, 0, 0);

		if (strlen(viewdef) == 0)
		{
			write_msg(NULL, "definition of view \"%s\" appears to be empty (length zero)\n",
					  tbinfo->dobj.name);
			exit_nicely();
		}

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delq, "DROP VIEW %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(delq, "%s;\n",
						  fmtId(tbinfo->dobj.name));

		appendPQExpBuffer(q, "CREATE VIEW %s AS\n    %s\n",
						  fmtId(tbinfo->dobj.name), viewdef);

		appendPQExpBuffer(labelq, "VIEW %s",
						  fmtId(tbinfo->dobj.name));

		PQclear(res);
	}
	/* START MPP ADDITION */
	else if (tbinfo->relstorage == RELSTORAGE_EXTERNAL)
	{
		reltypename = "EXTERNAL TABLE";
		dumpExternal(tbinfo, query, q, delq);

		appendPQExpBuffer(labelq, "EXTERNAL TABLE %s",
						  fmtId(tbinfo->dobj.name));
	}
	/* END MPP ADDITION */
	else
	{
		reltypename = "TABLE";
		numParents = tbinfo->numParents;
		parents = tbinfo->parents;

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delq, "DROP TABLE %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(delq, "%s;\n",
						  fmtId(tbinfo->dobj.name));

		appendPQExpBuffer(labelq, "%s %s", reltypename,
						  fmtId(tbinfo->dobj.name));

		appendPQExpBuffer(q, "CREATE TABLE %s (",
						  fmtId(tbinfo->dobj.name));

		actual_atts = 0;
		for (j = 0; j < tbinfo->numatts; j++)
		{
			/* Dump if it's locally defined in this table, and not dropped */
			if (shouldPrintColumn(tbinfo, j))
			{
				/* Format properly if not first attr */
				if (actual_atts > 0)
					appendPQExpBuffer(q, ",");
				appendPQExpBuffer(q, "\n    ");
				actual_atts++;

				/* Attribute name */
				appendPQExpBuffer(q, "%s ",
								  fmtId(tbinfo->attnames[j]));

				if (tbinfo->attisdropped[j])
				{
					/*
					 * ALTER TABLE DROP COLUMN clears pg_attribute.atttypid,
					 * so we will not have gotten a valid type name; insert
					 * INTEGER as a stopgap.  We'll clean things up later.
					 */
					appendPQExpBuffer(q, "INTEGER /* dummy */");
					/* Skip all the rest, too */
					continue;
				}

				/* Attribute type */
				appendPQExpBuffer(q, "%s",
								  tbinfo->atttypnames[j]);

				/*
				 * Default value --- suppress if to be printed separately.
				 */
				if (tbinfo->attrdefs[j] != NULL &&
					!tbinfo->attrdefs[j]->separate)
					appendPQExpBuffer(q, " DEFAULT %s",
									  tbinfo->attrdefs[j]->adef_expr);

				/*
				 * Not Null constraint --- suppress if inherited, except in
				 * binary-upgrade mode where taht won't work.
				 */
				if (tbinfo->notnull[j] &&
					(!tbinfo->inhNotNull[j] || binary_upgrade))
					appendPQExpBuffer(q, " NOT NULL");

				/* Column Storage attributes */
				if (tbinfo->attencoding[j] != NULL)
					appendPQExpBuffer(q, " ENCODING (%s)",
										tbinfo->attencoding[j]);
			}
		}

		/*
		 * Add non-inherited CHECK constraints, if any.
		 */
		for (j = 0; j < tbinfo->ncheck; j++)
		{
			ConstraintInfo *constr = &(tbinfo->checkexprs[j]);

			if (constr->coninherited || constr->separate)
				continue;

			if (actual_atts > 0)
				appendPQExpBuffer(q, ",\n    ");

			appendPQExpBuffer(q, "CONSTRAINT %s ",
							  fmtId(constr->dobj.name));
			appendPQExpBuffer(q, "%s", constr->condef);

			actual_atts++;
		}

		appendPQExpBuffer(q, "\n)");

		/*
		 * Emit the INHERITS clause if this table has parents.
		 */
		if (numParents > 0 && !binary_upgrade)
		{
			appendPQExpBuffer(q, "\nINHERITS (");
			for (k = 0; k < numParents; k++)
			{
				TableInfo  *parentRel = parents[k];

				if (k > 0)
					appendPQExpBuffer(q, ", ");
				if (parentRel->dobj.namespace != tbinfo->dobj.namespace)
					appendPQExpBuffer(q, "%s.",
								fmtId(parentRel->dobj.namespace->dobj.name));
				appendPQExpBuffer(q, "%s",
								  fmtId(parentRel->dobj.name));
			}
			appendPQExpBuffer(q, ")");
		}

		if (tbinfo->reloptions && strlen(tbinfo->reloptions) > 0)
			appendPQExpBuffer(q, "\nWITH (%s)", tbinfo->reloptions);

		/* START MPP ADDITION */

		/*
		 * Dump distributed by clause. We skip this in binary-upgrade mode,
		 * because that runs against a single segment server, and we don't
		 * store the distribution policy information in segments.
		 */
		if (dumpPolicy)
			addDistributedBy(q, tbinfo, actual_atts);

		/*
		 * If GP partitioning is supported add the partitioning constraints to
		 * the table definition.
		 */
		if (gp_partitioning_available)
		{
			bool		isTemplatesSupported = g_fout->remoteVersion >= 80214;

			/* does support GP partitioning. */
			resetPQExpBuffer(query);
			/* MPP-6297: dump by tablename */
			if (isTemplatesSupported)
				/* use 4.x version of function */
				appendPQExpBuffer(query, "SELECT "
				   "pg_get_partition_def('%u'::pg_catalog.oid, true, true) ",
								  tbinfo->dobj.catId.oid);
			else	/* use 3.x version of function */
				appendPQExpBuffer(query, "SELECT "
						 "pg_get_partition_def('%u'::pg_catalog.oid, true) ",
								  tbinfo->dobj.catId.oid);

			res = PQexec(g_conn, query->data);
			check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

			if (PQntuples(res) != 1)
			{
				if (PQntuples(res) < 1)
					write_msg(NULL, "query to obtain definition of table \"%s\" returned no data\n",
							  tbinfo->dobj.name);
				else
					write_msg(NULL, "query to obtain definition of table \"%s\" returned more than one definition\n",
							  tbinfo->dobj.name);
				exit_nicely();
			}
			isPartitioned = !PQgetisnull(res, 0, 0);
			if (isPartitioned)
				appendPQExpBuffer(q, " %s", PQgetvalue(res, 0, 0));

			PQclear(res);

			/*
			 * MPP-6095: dump ALTER TABLE statements for subpartition
			 * templates
			 */
			if (isTemplatesSupported)
			{
				resetPQExpBuffer(query);

				appendPQExpBuffer(
								  query, "SELECT "
								  "pg_get_partition_template_def('%u'::pg_catalog.oid, true, true) ",
								  tbinfo->dobj.catId.oid);

				res = PQexec(g_conn, query->data);
				check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

				if (PQntuples(res) != 1)
				{
					if (PQntuples(res) < 1)
						write_msg(
								  NULL,
								  "query to obtain definition of table \"%s\" returned no data\n",
								  tbinfo->dobj.name);
					else
						write_msg(
								  NULL,
								  "query to obtain definition of table \"%s\" returned more than one definition\n",
								  tbinfo->dobj.name);
					exit_nicely();
				}

				/*
				 * MPP-9537: terminate (with semicolon) the previous
				 * statement, and dump the template definitions
				 */
				if (!PQgetisnull(res, 0, 0) &&
					PQgetlength(res, 0, 0))
					appendPQExpBuffer(q, ";\n %s", PQgetvalue(res, 0, 0));

				PQclear(res);
			}

		}

		/* END MPP ADDITION */

		appendPQExpBuffer(q, ";\n");

		/* Exchange external partition */
		if (isPartitioned)
		{
			int i = 0;
			int ntups = 0;
			char *relname = NULL;
			int i_relname = 0;
			int i_parname = 0;
			int i_partitionrank = 0;
			resetPQExpBuffer(query);

			appendPQExpBuffer(query, "SELECT DISTINCT cc.relname, ps.partitionrank, pp.parname "
					"FROM pg_partition p "
					"JOIN pg_class c on (p.parrelid = c.oid) "
					"JOIN pg_partitions ps on (c.relname = ps.tablename) "
					"JOIN pg_class cc on (ps.partitiontablename = cc.relname) "
					"JOIN pg_partition_rule pp on (cc.oid = pp.parchildrelid) "
					"WHERE p.parrelid = %u AND cc.relstorage = '%c';",
					tbinfo->dobj.catId.oid, RELSTORAGE_EXTERNAL);

			res = PQexec(g_conn, query->data);
			check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

			ntups = PQntuples(res);
			i_relname = PQfnumber(res, "relname");
			i_parname = PQfnumber(res, "parname");
			i_partitionrank = PQfnumber(res, "partitionrank");

			for (i = 0; i < ntups; i++)
			{
				char tmpExtTable[500] = {0};
				relname = strdup(PQgetvalue(res, i, i_relname));
				snprintf(tmpExtTable, sizeof(tmpExtTable), "%s%s", relname, EXT_PARTITION_NAME_POSTFIX);
				appendPQExpBuffer(q, "ALTER TABLE %s ", fmtId(tbinfo->dobj.name));
				/*
				 * If it is an anonymous range partition we must exchange for
				 * the rank rather than the parname.
				 */
				if (PQgetisnull(res, i, i_parname) || !strlen(PQgetvalue(res, i, i_parname)))
				{
					appendPQExpBuffer(q, "EXCHANGE PARTITION FOR (RANK(%s)) ",
									  PQgetvalue(res, i, i_partitionrank));
				}
				else
				{
					appendPQExpBuffer(q, "EXCHANGE PARTITION %s ",
									  fmtId(PQgetvalue(res, i, i_parname)));
				}
				appendPQExpBuffer(q, "WITH TABLE %s WITHOUT VALIDATION; ", fmtId(tmpExtTable));

				appendPQExpBuffer(q, "\n");

				appendPQExpBuffer(q, "DROP TABLE %s; ", fmtId(tmpExtTable));

				appendPQExpBuffer(q, "\n");
				free(relname);
			}

			PQclear(res);
		}

		/*
		 * To create binary-compatible heap files, we have to ensure the
		 * same physical column order, including dropped columns, as in the
		 * original.  Therefore, we create dropped columns above and drop
		 * them here, also updating their attlen/attalign values so that
		 * the dropped column can be skipped properly.  (We do not bother
		 * with restoring the original attbyval setting.)  Also, inheritance
		 * relationships are set up by doing ALTER INHERIT rather than using
		 * an INHERITS clause --- the latter would possibly mess up the
		 * column order.  That also means we have to take care about setting
		 * attislocal correctly, plus fix up any inherited CHECK constraints.
		 */
		if (binary_upgrade)
		{
			for (j = 0; j < tbinfo->numatts; j++)
			{
				if (tbinfo->attisdropped[j])
				{
					/*
					 * Greenplum doesn't allow altering system catalogs without
					 * setting the allow_system_table_mods GUC first.
					 */
					appendPQExpBuffer(q, "SET allow_system_table_mods = 'dml';\n");

					appendPQExpBuffer(q, "\n-- For binary upgrade, recreate dropped column.\n");
					appendPQExpBuffer(q, "UPDATE pg_catalog.pg_attribute\n"
									  "SET attlen = %d, "
									  "attalign = '%c', attbyval = false\n"
									  "WHERE attname = ",
									  tbinfo->attlen[j],
									  tbinfo->attalign[j]);
					appendStringLiteralAH(q, tbinfo->attnames[j], fout);
					appendPQExpBuffer(q, "\n  AND attrelid = ");
					appendStringLiteralAH(q, fmtId(tbinfo->dobj.name), fout);
					appendPQExpBuffer(q, "::pg_catalog.regclass;\n");

					appendPQExpBuffer(q, "ALTER TABLE ONLY %s ",
									  fmtId(tbinfo->dobj.name));
					appendPQExpBuffer(q, "DROP COLUMN %s;\n",
									  fmtId(tbinfo->attnames[j]));
				}
				else if (!tbinfo->attislocal[j])
				{
					/*
					 * Greenplum doesn't allow altering system catalogs without
					 * setting the allow_system_table_mods GUC first.
					 */
					appendPQExpBuffer(q, "SET allow_system_table_mods = 'dml';\n");

					appendPQExpBuffer(q, "\n-- For binary upgrade, recreate inherited column.\n");
					appendPQExpBuffer(q, "UPDATE pg_catalog.pg_attribute\n"
									  "SET attislocal = false\n"
									  "WHERE attname = ");
					appendStringLiteralAH(q, tbinfo->attnames[j], fout);
					appendPQExpBuffer(q, "\n  AND attrelid = ");
					appendStringLiteralAH(q, fmtId(tbinfo->dobj.name), fout);
					appendPQExpBuffer(q, "::pg_catalog.regclass;\n");
				}
			}

			for (k = 0; k < tbinfo->ncheck; k++)
			{
				ConstraintInfo *constr = &(tbinfo->checkexprs[k]);

				/* GPDB_84_MERGE_FIXME: related to the below */
				if (!constr->coninherited || constr->separate)
					continue;

				/*
				 * Greenplum doesn't allow altering system catalogs without
				 * setting the allow_system_table_mods GUC first.
				 */
				appendPQExpBuffer(q, "SET allow_system_table_mods = 'dml';\n");

				appendPQExpBuffer(q, "\n-- For binary upgrade, set up inherited constraint.\n");
				appendPQExpBuffer(q, "ALTER TABLE ONLY %s ",
								  fmtId(tbinfo->dobj.name));
				appendPQExpBuffer(q, " ADD CONSTRAINT %s ",
								  fmtId(constr->dobj.name));
				appendPQExpBuffer(q, "%s;\n", constr->condef);
				/*
				 * GPDB_84_MERGE_FIXME - When we in 8.4 get conislocal, reactivate this code
				 * for handling constraints. Left if 0'd out to minimize merge conflicts.
				 */
#if 0
				appendPQExpBuffer(q, "UPDATE pg_catalog.pg_constraint\n"
								  "SET conislocal = false\n"
								  "WHERE contype = 'c' AND conname = ");
				appendStringLiteralAH(q, constr->dobj.name, fout);
				appendPQExpBuffer(q, "\n  AND conrelid = ");
				appendStringLiteralAH(q, fmtId(tbinfo->dobj.name), fout);
				appendPQExpBuffer(q, "::pg_catalog.regclass;\n");
#endif
			}

			if (numParents > 0)
			{
				appendPQExpBuffer(q, "\n-- For binary upgrade, set up inheritance this way.\n");
				for (k = 0; k < numParents; k++)
				{
					TableInfo  *parentRel = parents[k];

					appendPQExpBuffer(q, "ALTER TABLE ONLY %s INHERIT ",
									  fmtId(tbinfo->dobj.name));
					if (parentRel->dobj.namespace != tbinfo->dobj.namespace)
						appendPQExpBuffer(q, "%s.",
										  fmtId(parentRel->dobj.namespace->dobj.name));
					appendPQExpBuffer(q, "%s;\n",
									  fmtId(parentRel->dobj.name));
				}
			}

			/*
			 * We have probably bumped allow_system_table_mods to 'dml' in the
			 * above processing, but even we didn't let's just reset it here
			 * since it doesn't to do any harm to.
			 */
			appendPQExpBuffer(q, "RESET allow_system_table_mods;\n");
		}
	
		/*
		 * Dump additional per-column properties that we can't handle in the
		 * main CREATE TABLE command.
		 */
		/* Loop dumping statistics and storage statements */
		for (j = 0; j < tbinfo->numatts; j++)
		{
			/* None of this applies to dropped columns */
			if (tbinfo->attisdropped[j])
				continue;

			/*
			 * If we didn't dump the column definition explicitly above, and
			 * it is NOT NULL and did not inherit that property from a parent,
			 * we have to mark it separately.
			 */
			if (!shouldPrintColumn(tbinfo, j) &&
				tbinfo->notnull[j] && !tbinfo->inhNotNull[j])
			{
				appendPQExpBuffer(q, "ALTER TABLE ONLY %s ",
								  fmtId(tbinfo->dobj.name));
				appendPQExpBuffer(q, "ALTER COLUMN %s SET NOT NULL;\n",
								  fmtId(tbinfo->attnames[j]));
			}

			/*
			 * Dump per-column statistics information. We only issue an ALTER
			 * TABLE statement if the attstattarget entry for this column is
			 * non-negative (i.e. it's not the default value)
			 */
			if (tbinfo->attstattarget[j] >= 0)
			{
				appendPQExpBuffer(q, "ALTER TABLE ONLY %s ",
								  fmtId(tbinfo->dobj.name));
				appendPQExpBuffer(q, "ALTER COLUMN %s ",
								  fmtId(tbinfo->attnames[j]));
				appendPQExpBuffer(q, "SET STATISTICS %d;\n",
								  tbinfo->attstattarget[j]);
			}

			/*
			 * Dump per-column storage information.  The statement is only
			 * dumped if the storage has been changed from the type's default.
			 * An inherited column can have
			 * its storage type changed independently from the parent
			 * specification.
			 */
			if (tbinfo->attstorage[j] != tbinfo->typstorage[j])
			{
				switch (tbinfo->attstorage[j])
				{
					case 'p':
						storage = "PLAIN";
						break;
					case 'e':
						storage = "EXTERNAL";
						break;
					case 'm':
						storage = "MAIN";
						break;
					case 'x':
						storage = "EXTENDED";
						break;
					default:
						storage = NULL;
				}

				/*
				 * Only dump the statement if it's a storage type we recognize
				 */
				if (storage != NULL)
				{
					appendPQExpBuffer(q, "ALTER TABLE ONLY %s ",
									  fmtId(tbinfo->dobj.name));
					appendPQExpBuffer(q, "ALTER COLUMN %s ",
									  fmtId(tbinfo->attnames[j]));
					appendPQExpBuffer(q, "SET STORAGE %s;\n",
									  storage);
				}
			}
		}


		/* MPP-1890 */

		/*
		 * An inherited constraint may be dropped from a child table.  While
		 * this arguably severs the inheritance contract between the child and
		 * the parent, the current pg_constraint content doesn't track
		 * inherited/shared/disjoint constraints of a child.
		 * the INHERITS clause is used on a CREATE
		 * TABLE statement to re-establish the inheritance relationship and
		 * "recovers" the dropped constraint(s).
		 */
		if (numParents > 0)
			DetectChildConstraintDropped(tbinfo, q);
	}

	if (binary_upgrade)
		binary_upgrade_extension_member(q, &tbinfo->dobj, labelq->data);

	ArchiveEntry(fout, tbinfo->dobj.catId, tbinfo->dobj.dumpId,
				 tbinfo->dobj.name,
				 tbinfo->dobj.namespace->dobj.name,
			(tbinfo->relkind == RELKIND_VIEW) ? NULL : tbinfo->reltablespace,
				 tbinfo->rolname,
				 (strcmp(reltypename, "TABLE") == 0 ||
				  strcmp(reltypename, "EXTERNAL TABLE") == 0
					 ) ? tbinfo->hasoids : false,
				 reltypename, q->data, delq->data, NULL,
				 tbinfo->dobj.dependencies, tbinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump Table Comments */
	dumpTableComment(fout, tbinfo, reltypename);

	/* Dump comments on inlined table constraints */
	for (j = 0; j < tbinfo->ncheck; j++)
	{
		ConstraintInfo *constr = &(tbinfo->checkexprs[j]);

		if (constr->coninherited || constr->separate)
			continue;

		dumpTableConstraintComment(fout, constr);
	}

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
}

/*
 * dumpAttrDef --- dump an attribute's default-value declaration
 */
static void
dumpAttrDef(Archive *fout, AttrDefInfo *adinfo)
{
	TableInfo  *tbinfo = adinfo->adtable;
	int			adnum = adinfo->adnum;
	PQExpBuffer q;
	PQExpBuffer delq;

	/* Skip if table definition not to be dumped */
	if (!tbinfo->dobj.dump || dataOnly)
		return;

	/* Skip if not "separate"; it was dumped in the table's definition */
	if (!adinfo->separate)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	/*
	 * If the table is the parent of a partitioning hierarchy, the default
	 * constraint must be applied to all children as well.
	 */
	appendPQExpBuffer(q, "ALTER TABLE %s %s ",
					  tbinfo->parparent ? "" : "ONLY",
					  fmtId(tbinfo->dobj.name));
	appendPQExpBuffer(q, "ALTER COLUMN %s SET DEFAULT %s;\n",
					  fmtId(tbinfo->attnames[adnum - 1]),
					  adinfo->adef_expr);

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delq, "ALTER TABLE %s.",
					  fmtId(tbinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delq, "%s ",
					  fmtId(tbinfo->dobj.name));
	appendPQExpBuffer(delq, "ALTER COLUMN %s DROP DEFAULT;\n",
					  fmtId(tbinfo->attnames[adnum - 1]));

	ArchiveEntry(fout, adinfo->dobj.catId, adinfo->dobj.dumpId,
				 tbinfo->attnames[adnum - 1],
				 tbinfo->dobj.namespace->dobj.name,
				 NULL,
				 tbinfo->rolname,
				 false, "DEFAULT", q->data, delq->data, NULL,
				 adinfo->dobj.dependencies, adinfo->dobj.nDeps,
				 NULL, NULL);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * getAttrName: extract the correct name for an attribute
 *
 * The array tblInfo->attnames[] only provides names of user attributes;
 * if a system attribute number is supplied, we have to fake it.
 * We also do a little bit of bounds checking for safety's sake.
 */
static const char *
getAttrName(int attrnum, TableInfo *tblInfo)
{
	if (attrnum > 0 && attrnum <= tblInfo->numatts)
		return tblInfo->attnames[attrnum - 1];
	switch (attrnum)
	{
		case SelfItemPointerAttributeNumber:
			return "ctid";
		case ObjectIdAttributeNumber:
			return "oid";
		case MinTransactionIdAttributeNumber:
			return "xmin";
		case MinCommandIdAttributeNumber:
			return "cmin";
		case MaxTransactionIdAttributeNumber:
			return "xmax";
		case MaxCommandIdAttributeNumber:
			return "cmax";
		case TableOidAttributeNumber:
			return "tableoid";
	}
	write_msg(NULL, "invalid column number %d for table \"%s\"\n",
			  attrnum, tblInfo->dobj.name);
	exit_nicely();
	return NULL;				/* keep compiler quiet */
}

/*
 * dumpIndex
 *	  write out to fout a user-defined index
 */
static void
dumpIndex(Archive *fout, IndxInfo *indxinfo)
{
	TableInfo  *tbinfo = indxinfo->indextable;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer labelq;

	if (dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	labelq = createPQExpBuffer();

	appendPQExpBuffer(labelq, "INDEX %s",
					  fmtId(indxinfo->dobj.name));

	/*
	 * If there's an associated constraint, don't dump the index per se, but
	 * do dump any comment for it.	(This is safe because dependency ordering
	 * will have ensured the constraint is emitted first.)
	 */
	if (indxinfo->indexconstraint == 0)
	{
		/* Plain secondary index */
		appendPQExpBuffer(q, "%s;\n", indxinfo->indexdef);

		/* If the index is clustered, we need to record that. */
		if (indxinfo->indisclustered)
		{
			appendPQExpBuffer(q, "\nALTER TABLE %s CLUSTER",
							  fmtId(tbinfo->dobj.name));
			appendPQExpBuffer(q, " ON %s;\n",
							  fmtId(indxinfo->dobj.name));
		}

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delq, "DROP INDEX %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(delq, "%s;\n",
						  fmtId(indxinfo->dobj.name));

		ArchiveEntry(fout, indxinfo->dobj.catId, indxinfo->dobj.dumpId,
					 indxinfo->dobj.name,
					 tbinfo->dobj.namespace->dobj.name,
					 indxinfo->tablespace,
					 tbinfo->rolname, false,
					 "INDEX", q->data, delq->data, NULL,
					 indxinfo->dobj.dependencies, indxinfo->dobj.nDeps,
					 NULL, NULL);
	}

	/* Dump Index Comments */
	dumpComment(fout, labelq->data,
				tbinfo->dobj.namespace->dobj.name,
				tbinfo->rolname,
				indxinfo->dobj.catId, 0, indxinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(labelq);
}

/*
 * dumpConstraint
 *	  write out to fout a user-defined constraint
 */
static void
dumpConstraint(Archive *fout, ConstraintInfo *coninfo)
{
	TableInfo  *tbinfo = coninfo->contable;
	PQExpBuffer q;
	PQExpBuffer delq;

	/* Skip if not to be dumped */
	if (!coninfo->dobj.dump || dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	if (coninfo->contype == 'p' || coninfo->contype == 'u')
	{
		/* Index-related constraint */
		IndxInfo   *indxinfo;
		int			k;

		indxinfo = (IndxInfo *) findObjectByDumpId(coninfo->conindex);

		if (indxinfo == NULL)
		{
			write_msg(NULL, "missing index for constraint \"%s\"\n",
					  coninfo->dobj.name);
			exit_nicely();
		}

		appendPQExpBuffer(q, "ALTER TABLE ONLY %s\n",
						  fmtId(tbinfo->dobj.name));
		appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s (",
						  fmtId(coninfo->dobj.name),
						  coninfo->contype == 'p' ? "PRIMARY KEY" : "UNIQUE");

		for (k = 0; k < indxinfo->indnkeys; k++)
		{
			int			indkey = (int) indxinfo->indkeys[k];
			const char *attname;

			if (indkey == InvalidAttrNumber)
				break;
			attname = getAttrName(indkey, tbinfo);

			appendPQExpBuffer(q, "%s%s",
							  (k == 0) ? "" : ", ",
							  fmtId(attname));
		}

		appendPQExpBuffer(q, ")");

		if (indxinfo->options && strlen(indxinfo->options) > 0)
			appendPQExpBuffer(q, " WITH (%s)", indxinfo->options);

		appendPQExpBuffer(q, ";\n");

		/* If the index is clustered, we need to record that. */
		if (indxinfo->indisclustered)
		{
			appendPQExpBuffer(q, "\nALTER TABLE %s CLUSTER",
							  fmtId(tbinfo->dobj.name));
			appendPQExpBuffer(q, " ON %s;\n",
							  fmtId(indxinfo->dobj.name));
		}

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delq, "ALTER TABLE ONLY %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(delq, "%s ",
						  fmtId(tbinfo->dobj.name));
		appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
						  fmtId(coninfo->dobj.name));

		ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
					 coninfo->dobj.name,
					 tbinfo->dobj.namespace->dobj.name,
					 indxinfo->tablespace,
					 tbinfo->rolname, false,
					 "CONSTRAINT", q->data, delq->data, NULL,
					 coninfo->dobj.dependencies, coninfo->dobj.nDeps,
					 NULL, NULL);
	}
	else if (coninfo->contype == 'f')
	{
		/*
		 * XXX Potentially wrap in a 'SET CONSTRAINTS OFF' block so that the
		 * current table data is not processed
		 */
		appendPQExpBuffer(q, "ALTER TABLE ONLY %s\n",
						  fmtId(tbinfo->dobj.name));
		appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s;\n",
						  fmtId(coninfo->dobj.name),
						  coninfo->condef);

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delq, "ALTER TABLE ONLY %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(delq, "%s ",
						  fmtId(tbinfo->dobj.name));
		appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
						  fmtId(coninfo->dobj.name));

		ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
					 coninfo->dobj.name,
					 tbinfo->dobj.namespace->dobj.name,
					 NULL,
					 tbinfo->rolname, false,
					 "FK CONSTRAINT", q->data, delq->data, NULL,
					 coninfo->dobj.dependencies, coninfo->dobj.nDeps,
					 NULL, NULL);
	}
	else if (coninfo->contype == 'c' && tbinfo)
	{
		/* CHECK constraint on a table */

		/* Ignore if not to be dumped separately */
		if (coninfo->separate)
		{
			/* not ONLY since we want it to propagate to children */
			appendPQExpBuffer(q, "ALTER TABLE %s\n",
							  fmtId(tbinfo->dobj.name));
			appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s;\n",
							  fmtId(coninfo->dobj.name),
							  coninfo->condef);

			/*
			 * DROP must be fully qualified in case same name appears in
			 * pg_catalog
			 */
			appendPQExpBuffer(delq, "ALTER TABLE %s.",
							  fmtId(tbinfo->dobj.namespace->dobj.name));
			appendPQExpBuffer(delq, "%s ",
							  fmtId(tbinfo->dobj.name));
			appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
							  fmtId(coninfo->dobj.name));

			ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
						 coninfo->dobj.name,
						 tbinfo->dobj.namespace->dobj.name,
						 NULL,
						 tbinfo->rolname, false,
						 "CHECK CONSTRAINT", q->data, delq->data, NULL,
						 coninfo->dobj.dependencies, coninfo->dobj.nDeps,
						 NULL, NULL);
		}
	}
	else if (coninfo->contype == 'c' && tbinfo == NULL)
	{
		/* CHECK constraint on a domain */
		TypeInfo   *tinfo = coninfo->condomain;

		/* Ignore if not to be dumped separately */
		if (coninfo->separate)
		{
			appendPQExpBuffer(q, "ALTER DOMAIN %s\n",
							  fmtId(tinfo->dobj.name));
			appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s;\n",
							  fmtId(coninfo->dobj.name),
							  coninfo->condef);

			/*
			 * DROP must be fully qualified in case same name appears in
			 * pg_catalog
			 */
			appendPQExpBuffer(delq, "ALTER DOMAIN %s.",
							  fmtId(tinfo->dobj.namespace->dobj.name));
			appendPQExpBuffer(delq, "%s ",
							  fmtId(tinfo->dobj.name));
			appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
							  fmtId(coninfo->dobj.name));

			ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
						 coninfo->dobj.name,
						 tinfo->dobj.namespace->dobj.name,
						 NULL,
						 tinfo->rolname, false,
						 "CHECK CONSTRAINT", q->data, delq->data, NULL,
						 coninfo->dobj.dependencies, coninfo->dobj.nDeps,
						 NULL, NULL);
		}
	}
	else
	{
		write_msg(NULL, "unrecognized constraint type: %c\n", coninfo->contype);
		exit_nicely();
	}

	/* Dump Constraint Comments --- only works for table constraints */
	if (tbinfo && coninfo->separate)
		dumpTableConstraintComment(fout, coninfo);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpTableConstraintComment --- dump a constraint's comment if any
 *
 * This is split out because we need the function in two different places
 * depending on whether the constraint is dumped as part of CREATE TABLE
 * or as a separate ALTER command.
 */
static void
dumpTableConstraintComment(Archive *fout, ConstraintInfo *coninfo)
{
	TableInfo  *tbinfo = coninfo->contable;
	PQExpBuffer q = createPQExpBuffer();

	appendPQExpBuffer(q, "CONSTRAINT %s ",
					  fmtId(coninfo->dobj.name));
	appendPQExpBuffer(q, "ON %s",
					  fmtId(tbinfo->dobj.name));
	dumpComment(fout, q->data,
				tbinfo->dobj.namespace->dobj.name,
				tbinfo->rolname,
				coninfo->dobj.catId, 0,
			 coninfo->separate ? coninfo->dobj.dumpId : tbinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
}

static void
dumpSequence(Archive *fout, TableInfo *tbinfo)
{
	PGresult   *res;
	char	   *last,
			   *incby,
			   *maxv = NULL,
			   *minv = NULL,
			   *cache;
	char		bufm[100],
				bufx[100];
	bool		cycled,
				called;
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();

	/* Make sure we are in proper schema */
	selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

	snprintf(bufm, sizeof(bufm), INT64_FORMAT, SEQ_MINVALUE);
	snprintf(bufx, sizeof(bufx), INT64_FORMAT, SEQ_MAXVALUE);

	appendPQExpBuffer(query,
					  "SELECT sequence_name, last_value, increment_by, "
				   "CASE WHEN increment_by > 0 AND max_value = %s THEN NULL "
				   "     WHEN increment_by < 0 AND max_value = -1 THEN NULL "
					  "     ELSE max_value "
					  "END AS max_value, "
					"CASE WHEN increment_by > 0 AND min_value = 1 THEN NULL "
				   "     WHEN increment_by < 0 AND min_value = %s THEN NULL "
					  "     ELSE min_value "
					  "END AS min_value, "
					  "cache_value, is_cycled, is_called from %s",
					  bufx, bufm,
					  fmtId(tbinfo->dobj.name));

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	if (PQntuples(res) != 1)
	{
		write_msg(NULL, "query to get data of sequence \"%s\" returned %d rows (expected 1)\n",
				  tbinfo->dobj.name, PQntuples(res));
		exit_nicely();
	}

	/* Disable this check: it fails if sequence has been renamed */
#ifdef NOT_USED
	if (strcmp(PQgetvalue(res, 0, 0), tbinfo->dobj.name) != 0)
	{
		write_msg(NULL, "query to get data of sequence \"%s\" returned name \"%s\"\n",
				  tbinfo->dobj.name, PQgetvalue(res, 0, 0));
		exit_nicely();
	}
#endif

	last = PQgetvalue(res, 0, 1);
	incby = PQgetvalue(res, 0, 2);
	if (!PQgetisnull(res, 0, 3))
		maxv = PQgetvalue(res, 0, 3);
	if (!PQgetisnull(res, 0, 4))
		minv = PQgetvalue(res, 0, 4);
	cache = PQgetvalue(res, 0, 5);
	cycled = (strcmp(PQgetvalue(res, 0, 6), "t") == 0);
	called = (strcmp(PQgetvalue(res, 0, 7), "t") == 0);

	/*
	 * The logic we use for restoring sequences is as follows:
	 *
	 * Add a CREATE SEQUENCE statement as part of a "schema" dump (use
	 * last_val for start if called is false, else use min_val for start_val).
	 * Also, if the sequence is owned by a column, add an ALTER SEQUENCE OWNED
	 * BY command for it.
	 *
	 * Add a 'SETVAL(seq, last_val, iscalled)' as part of a "data" dump.
	 */
	if (!dataOnly)
	{
		resetPQExpBuffer(delqry);

		/*
		 * DROP must be fully qualified in case same name appears in
		 * pg_catalog
		 */
		appendPQExpBuffer(delqry, "DROP SEQUENCE %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(delqry, "%s;\n",
						  fmtId(tbinfo->dobj.name));

		resetPQExpBuffer(query);

		appendPQExpBuffer(query,
						  "CREATE SEQUENCE %s\n",
						  fmtId(tbinfo->dobj.name));

		if (!called)
			appendPQExpBuffer(query, "    START WITH %s\n", last);

		appendPQExpBuffer(query, "    INCREMENT BY %s\n", incby);

		if (maxv)
			appendPQExpBuffer(query, "    MAXVALUE %s\n", maxv);
		else
			appendPQExpBuffer(query, "    NO MAXVALUE\n");

		if (minv)
			appendPQExpBuffer(query, "    MINVALUE %s\n", minv);
		else
			appendPQExpBuffer(query, "    NO MINVALUE\n");

		appendPQExpBuffer(query,
						  "    CACHE %s%s",
						  cache, (cycled ? "\n    CYCLE" : ""));

		appendPQExpBuffer(query, ";\n");

		/* binary_upgrade:  no need to clear TOAST table oid */
		
		ArchiveEntry(fout, tbinfo->dobj.catId, tbinfo->dobj.dumpId,
					 tbinfo->dobj.name,
					 tbinfo->dobj.namespace->dobj.name,
					 NULL,
					 tbinfo->rolname,
					 false, "SEQUENCE", query->data, delqry->data, NULL,
					 tbinfo->dobj.dependencies, tbinfo->dobj.nDeps,
					 NULL, NULL);

		/*
		 * If the sequence is owned by a table column, emit the ALTER for it
		 * as a separate TOC entry immediately following the sequence's own
		 * entry.  It's OK to do this rather than using full sorting logic,
		 * because the dependency that tells us it's owned will have forced
		 * the table to be created first.  We can't just include the ALTER in
		 * the TOC entry because it will fail if we haven't reassigned the
		 * sequence owner to match the table's owner.
		 *
		 * We need not schema-qualify the table reference because both
		 * sequence and table must be in the same schema.
		 */
		if (OidIsValid(tbinfo->owning_tab))
		{
			TableInfo  *owning_tab = findTableByOid(tbinfo->owning_tab);

			if (owning_tab && owning_tab->dobj.dump)
			{
				resetPQExpBuffer(query);
				appendPQExpBuffer(query, "ALTER SEQUENCE %s",
								  fmtId(tbinfo->dobj.name));
				appendPQExpBuffer(query, " OWNED BY %s",
								  fmtId(owning_tab->dobj.name));
				appendPQExpBuffer(query, ".%s;\n",
						fmtId(owning_tab->attnames[tbinfo->owning_col - 1]));

				ArchiveEntry(fout, nilCatalogId, createDumpId(),
							 tbinfo->dobj.name,
							 tbinfo->dobj.namespace->dobj.name,
							 NULL,
							 tbinfo->rolname,
						   false, "SEQUENCE OWNED BY", query->data, "", NULL,
							 &(tbinfo->dobj.dumpId), 1,
							 NULL, NULL);
			}
		}

		/* Dump Sequence Comments */
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "SEQUENCE %s", fmtId(tbinfo->dobj.name));
		dumpComment(fout, query->data,
					tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					tbinfo->dobj.catId, 0, tbinfo->dobj.dumpId);
	}

	if (!schemaOnly)
	{
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "SELECT pg_catalog.setval(");
		appendStringLiteralAH(query, fmtId(tbinfo->dobj.name), fout);
		appendPQExpBuffer(query, ", %s, %s);\n",
						  last, (called ? "true" : "false"));

		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 tbinfo->dobj.name,
					 tbinfo->dobj.namespace->dobj.name,
					 NULL,
					 tbinfo->rolname,
					 false, "SEQUENCE SET", query->data, "", NULL,
					 &(tbinfo->dobj.dumpId), 1,
					 NULL, NULL);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
}

static void
dumpTrigger(Archive *fout, TriggerInfo *tginfo)
{
	TableInfo  *tbinfo = tginfo->tgtable;
	PQExpBuffer query;
	PQExpBuffer delqry;
	char	   *tgargs;
	size_t		lentgargs;
	const char *p;
	int			findx;

	if (dataOnly)
		return;

	query = createPQExpBuffer();
	delqry = createPQExpBuffer();

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delqry, "DROP TRIGGER %s ",
					  fmtId(tginfo->dobj.name));
	appendPQExpBuffer(delqry, "ON %s.",
					  fmtId(tbinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delqry, "%s;\n",
					  fmtId(tbinfo->dobj.name));

	if (tginfo->tgisconstraint)
	{
		appendPQExpBuffer(query, "CREATE CONSTRAINT TRIGGER ");
		appendPQExpBufferStr(query, fmtId(tginfo->tgconstrname));
	}
	else
	{
		appendPQExpBuffer(query, "CREATE TRIGGER ");
		appendPQExpBufferStr(query, fmtId(tginfo->dobj.name));
	}
	appendPQExpBuffer(query, "\n    ");

	/* Trigger type */
	findx = 0;
	if (TRIGGER_FOR_BEFORE(tginfo->tgtype))
		appendPQExpBuffer(query, "BEFORE");
	else
		appendPQExpBuffer(query, "AFTER");
	if (TRIGGER_FOR_INSERT(tginfo->tgtype))
	{
		appendPQExpBuffer(query, " INSERT");
		findx++;
	}
	if (TRIGGER_FOR_DELETE(tginfo->tgtype))
	{
		if (findx > 0)
			appendPQExpBuffer(query, " OR DELETE");
		else
			appendPQExpBuffer(query, " DELETE");
		findx++;
	}
	if (TRIGGER_FOR_UPDATE(tginfo->tgtype))
	{
		if (findx > 0)
			appendPQExpBuffer(query, " OR UPDATE");
		else
			appendPQExpBuffer(query, " UPDATE");
	}
	appendPQExpBuffer(query, " ON %s\n",
					  fmtId(tbinfo->dobj.name));

	if (tginfo->tgisconstraint)
	{
		if (OidIsValid(tginfo->tgconstrrelid))
		{
			/* If we are using regclass, name is already quoted */
			appendPQExpBuffer(query, "    FROM %s\n    ",
							  tginfo->tgconstrrelname);
		}
		if (!tginfo->tgdeferrable)
			appendPQExpBuffer(query, "NOT ");
		appendPQExpBuffer(query, "DEFERRABLE INITIALLY ");
		if (tginfo->tginitdeferred)
			appendPQExpBuffer(query, "DEFERRED\n");
		else
			appendPQExpBuffer(query, "IMMEDIATE\n");
	}

	if (TRIGGER_FOR_ROW(tginfo->tgtype))
		appendPQExpBuffer(query, "    FOR EACH ROW\n    ");
	else
		appendPQExpBuffer(query, "    FOR EACH STATEMENT\n    ");

	appendPQExpBuffer(query, "EXECUTE PROCEDURE %s(",
					  tginfo->tgfname);

	tgargs = (char *) PQunescapeBytea((unsigned char *) tginfo->tgargs,
									  &lentgargs);
	p = tgargs;
	for (findx = 0; findx < tginfo->tgnargs; findx++)
	{
		/* find the embedded null that terminates this trigger argument */
		size_t	tlen = strlen(p);

		if (p + tlen >= tgargs + lentgargs)
		{
			/* hm, not found before end of bytea value... */
			write_msg(NULL, "invalid argument string (%s) for trigger \"%s\" on table \"%s\"\n",
					  tginfo->tgargs,
					  tginfo->dobj.name,
					  tbinfo->dobj.name);
			exit_nicely();
		}

		if (findx > 0)
			appendPQExpBuffer(query, ", ");
		appendStringLiteralAH(query, p, fout);
		p += tlen + 1;
	}
	free(tgargs);
	appendPQExpBuffer(query, ");\n");

	if (tginfo->tgenabled != 't' && tginfo->tgenabled != 'O')
	{
		appendPQExpBuffer(query, "\nALTER TABLE %s ",
						  fmtId(tbinfo->dobj.name));
		switch (tginfo->tgenabled)
		{
			case 'D':
			case 'f':
				appendPQExpBuffer(query, "DISABLE");
				break;
			case 'A':
				appendPQExpBuffer(query, "ENABLE ALWAYS");
				break;
			case 'R':
				appendPQExpBuffer(query, "ENABLE REPLICA");
				break;
			default:
				appendPQExpBuffer(query, "ENABLE");
				break;
		}
		appendPQExpBuffer(query, " TRIGGER %s;\n",
						  fmtId(tginfo->dobj.name));
	}

	ArchiveEntry(fout, tginfo->dobj.catId, tginfo->dobj.dumpId,
				 tginfo->dobj.name,
				 tbinfo->dobj.namespace->dobj.name,
				 NULL,
				 tbinfo->rolname, false,
				 "TRIGGER", query->data, delqry->data, NULL,
				 tginfo->dobj.dependencies, tginfo->dobj.nDeps,
				 NULL, NULL);

	resetPQExpBuffer(query);
	appendPQExpBuffer(query, "TRIGGER %s ",
					  fmtId(tginfo->dobj.name));
	appendPQExpBuffer(query, "ON %s",
					  fmtId(tbinfo->dobj.name));

	dumpComment(fout, query->data,
				tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
				tginfo->dobj.catId, 0, tginfo->dobj.dumpId);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
}

/*
 * dumpRule
 *		Dump a rule
 */
static void
dumpRule(Archive *fout, RuleInfo *rinfo)
{
	TableInfo  *tbinfo = rinfo->ruletable;
	PQExpBuffer query;
	PQExpBuffer cmd;
	PQExpBuffer delcmd;
	PGresult   *res;

	/* Skip if not to be dumped */
	if (!rinfo->dobj.dump || dataOnly)
		return;

	/*
	 * If it is an ON SELECT rule that is created implicitly by CREATE VIEW,
	 * we do not want to dump it as a separate object.
	 */
	if (!rinfo->separate)
		return;

	/*
	 * Make sure we are in proper schema.
	 */
	selectSourceSchema(tbinfo->dobj.namespace->dobj.name);

	query = createPQExpBuffer();
	cmd = createPQExpBuffer();
	delcmd = createPQExpBuffer();

	appendPQExpBuffer(query,
	  "SELECT pg_catalog.pg_get_ruledef('%u'::pg_catalog.oid) AS definition",
					  rinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	if (PQntuples(res) != 1)
	{
		write_msg(NULL, "query to get rule \"%s\" for table \"%s\" failed: wrong number of rows returned\n",
				  rinfo->dobj.name, tbinfo->dobj.name);
		exit_nicely();
	}

	printfPQExpBuffer(cmd, "%s\n", PQgetvalue(res, 0, 0));

	/*
	 * Add the command to alter the rules replication firing semantics if it
	 * differs from the default.
	 */
	if (rinfo->ev_enabled != 'O')
	{
		appendPQExpBuffer(cmd, "ALTER TABLE %s.",
						  fmtId(tbinfo->dobj.namespace->dobj.name));
		appendPQExpBuffer(cmd, "%s ",
						  fmtId(tbinfo->dobj.name));
		switch (rinfo->ev_enabled)
		{
			case 'A':
				appendPQExpBuffer(cmd, "ENABLE ALWAYS RULE %s;\n",
								  fmtId(rinfo->dobj.name));
				break;
			case 'R':
				appendPQExpBuffer(cmd, "ENABLE REPLICA RULE %s;\n",
								  fmtId(rinfo->dobj.name));
				break;
			case 'D':
				appendPQExpBuffer(cmd, "DISABLE RULE %s;\n",
								  fmtId(rinfo->dobj.name));
				break;
		}
	}

	/*
	 * DROP must be fully qualified in case same name appears in pg_catalog
	 */
	appendPQExpBuffer(delcmd, "DROP RULE %s ",
					  fmtId(rinfo->dobj.name));
	appendPQExpBuffer(delcmd, "ON %s.",
					  fmtId(tbinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(delcmd, "%s;\n",
					  fmtId(tbinfo->dobj.name));

	ArchiveEntry(fout, rinfo->dobj.catId, rinfo->dobj.dumpId,
				 rinfo->dobj.name,
				 tbinfo->dobj.namespace->dobj.name,
				 NULL,
				 tbinfo->rolname, false,
				 "RULE", cmd->data, delcmd->data, NULL,
				 rinfo->dobj.dependencies, rinfo->dobj.nDeps,
				 NULL, NULL);

	/* Dump rule comments */
	resetPQExpBuffer(query);
	appendPQExpBuffer(query, "RULE %s",
					  fmtId(rinfo->dobj.name));
	appendPQExpBuffer(query, " ON %s.",
					  fmtId(tbinfo->dobj.namespace->dobj.name));
	appendPQExpBuffer(query, "%s\n", fmtId(tbinfo->dobj.name));
	dumpComment(fout, query->data,
				tbinfo->dobj.namespace->dobj.name,
				tbinfo->rolname,
				rinfo->dobj.catId, 0, rinfo->dobj.dumpId);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(cmd);
	destroyPQExpBuffer(delcmd);
}

/*
 * getExtensionMembership --- obtain extension membership data
 *
 * We need to identify objects that are extension members as soon as they're
 * loaded, so that we can correctly determine whether they need to be dumped.
 * Generally speaking, extension member objects will get marked as *not* to
 * be dumped, as they will be recreated by the single CREATE EXTENSION
 * command.  However, in binary upgrade mode we still need to dump the members
 * individually.
 */
void
getExtensionMembership(ExtensionInfo extinfo[], int numExtensions)
{
	PQExpBuffer query;
	PGresult   *res;
	int			ntups,
				nextmembers,
				i;
	int			i_classid,
				i_objid,
				i_refobjid;
	ExtensionMemberId *extmembers;
	ExtensionInfo *ext;

	/* Nothing to do if no extensions */
	if (numExtensions == 0)
		return;

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	query = createPQExpBuffer();

	/* refclassid constraint is redundant but may speed the search */
	appendPQExpBufferStr(query, "SELECT "
			"classid, objid, refobjid "
			"FROM pg_depend "
			"WHERE refclassid = 'pg_extension'::regclass "
			"AND deptype = 'e' "
			"ORDER BY 3");

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_classid = PQfnumber(res, "classid");
	i_objid = PQfnumber(res, "objid");
	i_refobjid = PQfnumber(res, "refobjid");

	extmembers = (ExtensionMemberId *) pg_malloc(ntups * sizeof(ExtensionMemberId));
	nextmembers = 0;

	/*
	 * Accumulate data into extmembers[].
	 *
	 * Since we ordered the SELECT by referenced ID, we can expect that
	 * multiple entries for the same extension will appear together; this
	 * saves on searches.
	 */
	ext = NULL;

	for (i = 0; i < ntups; i++)
	{
		CatalogId	objId;
		Oid			extId;

		objId.tableoid = atooid(PQgetvalue(res, i, i_classid));
		objId.oid = atooid(PQgetvalue(res, i, i_objid));
		extId = atooid(PQgetvalue(res, i, i_refobjid));

		if (ext == NULL ||
			ext->dobj.catId.oid != extId)
			ext = findExtensionByOid(extId);

		if (ext == NULL)
		{
			/* shouldn't happen */
			fprintf(stderr, "could not find referenced extension %u\n", extId);
			continue;
		}

		extmembers[nextmembers].catId = objId;
		extmembers[nextmembers].ext = ext;
		nextmembers++;
	}

	PQclear(res);

	/* Remember the data for use later */
	setExtensionMembership(extmembers, nextmembers);

	destroyPQExpBuffer(query);
}

/*
 * processExtensionTables --- deal with extension configuration tables
 *
 * There are two parts to this process:
 *
 * 1. Identify and create dump records for extension configuration tables.
 *
 *	  Extensions can mark tables as "configuration", which means that the user
 *	  is able and expected to modify those tables after the extension has been
 *	  loaded.  For these tables, we dump out only the data- the structure is
 *	  expected to be handled at CREATE EXTENSION time, including any indexes or
 *	  foreign keys, which brings us to-
 *
 * 2. Record FK dependencies between configuration tables.
 *
 *	  Due to the FKs being created at CREATE EXTENSION time and therefore before
 *	  the data is loaded, we have to work out what the best order for reloading
 *	  the data is, to avoid FK violations when the tables are restored.  This is
 *	  not perfect- we can't handle circular dependencies and if any exist they
 *	  will cause an invalid dump to be produced (though at least all of the data
 *	  is included for a user to manually restore).  This is currently documented
 *	  but perhaps we can provide a better solution in the future.
 */
void
processExtensionTables(ExtensionInfo extinfo[], int numExtensions)
{
	PQExpBuffer query;
	PGresult   *res;
	int			ntups,
				i;
	int			i_conrelid,
				i_confrelid;

	/* Nothing to do if no extensions */
	if (numExtensions == 0)
		return;

	/*
	 * Identify extension configuration tables and create TableDataInfo
	 * objects for them, ensuring their data will be dumped even though the
	 * tables themselves won't be.
	 *
	 * Note that we create TableDataInfo objects even in schemaOnly mode, ie,
	 * user data in a configuration table is treated like schema data. This
	 * seems appropriate since system data in a config table would get
	 * reloaded by CREATE EXTENSION.
	 */
	for (i = 0; i < numExtensions; i++)
	{
		ExtensionInfo *curext = &(extinfo[i]);
		char	   *extconfig = curext->extconfig;
		char	   *extcondition = curext->extcondition;
		char	  **extconfigarray = NULL;
		char	  **extconditionarray = NULL;
		int			nconfigitems;
		int			nconditionitems;

		if (parsePGArray(extconfig, &extconfigarray, &nconfigitems) &&
			parsePGArray(extcondition, &extconditionarray, &nconditionitems) &&
			nconfigitems == nconditionitems)
		{
			int			j;

			for (j = 0; j < nconfigitems; j++)
			{
				TableInfo  *configtbl;
				Oid			configtbloid = atooid(extconfigarray[j]);
				bool		dumpobj = curext->dobj.dump;

				configtbl = findTableByOid(configtbloid);
				if (configtbl && configtbl->dataObj == NULL)
				{
					/*
					 * Tables of not-to-be-dumped extensions shouldn't be dumped
					 * unless the table or its schema is explicitly included
					 */
					if (!curext->dobj.dump)
					{
						/* check table explicitly requested */
						if (table_include_oids.head != NULL &&
							simple_oid_list_member(&table_include_oids,
												   configtbloid))
							dumpobj = true;

						/* check table's schema explicitly requested */
						if (configtbl->dobj.namespace->dobj.dump)
							dumpobj = true;
					}

					/* check table excluded by an exclusion switch */
					if (table_exclude_oids.head != NULL &&
						simple_oid_list_member(&table_exclude_oids,
											   configtbloid))
						dumpobj = false;

					/* check schema excluded by an exclusion switch */
					if (simple_oid_list_member(&schema_exclude_oids,
											   configtbl->dobj.namespace->dobj.catId.oid))
						dumpobj = false;

					if (dumpobj)
					{
						/*
						 * Note: config tables are dumped without OIDs regardless
						 * of the --oids setting.  This is because row filtering
						 * conditions aren't compatible with dumping OIDs.
						 */
						makeTableDataInfo(configtbl, false);
						if (strlen(extconditionarray[j]) > 0)
							configtbl->dataObj->filtercond = strdup(extconditionarray[j]);
					}
				}
			}
		}
		if (extconfigarray)
			free(extconfigarray);
		if (extconditionarray)
			free(extconditionarray);
	}

	/*
	 * Now that all the TableInfoData objects have been created for all the
	 * extensions, check their FK dependencies and register them to try and
	 * dump the data out in an order that they can be restored in.
	 *
	 * Note that this is not a problem for user tables as their FKs are
	 * recreated after the data has been loaded.
	 */

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	query = createPQExpBuffer();

	printfPQExpBuffer(query,
					  "SELECT conrelid, confrelid "
							  "FROM pg_constraint "
							  "JOIN pg_depend ON (objid = confrelid) "
							  "WHERE contype = 'f' "
							  "AND refclassid = 'pg_extension'::regclass "
							  "AND classid = 'pg_class'::regclass;");

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_conrelid = PQfnumber(res, "conrelid");
	i_confrelid = PQfnumber(res, "confrelid");

	/* Now get the dependencies and register them */
	for (i = 0; i < ntups; i++)
	{
		Oid			conrelid, confrelid;
		TableInfo  *reftable, *contable;

		conrelid = atooid(PQgetvalue(res, i, i_conrelid));
		confrelid = atooid(PQgetvalue(res, i, i_confrelid));
		contable = findTableByOid(conrelid);
		reftable = findTableByOid(confrelid);

		if (reftable == NULL ||
			reftable->dataObj == NULL ||
			contable == NULL ||
			contable->dataObj == NULL)
			continue;

		/*
		 * Make referencing TABLE_DATA object depend on the
		 * referenced table's TABLE_DATA object.
		 */
		addObjectDependency(&contable->dataObj->dobj,
							reftable->dataObj->dobj.dumpId);
	}
	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * setExtPartDependency -
 */
static void
setExtPartDependency(TableInfo *tblinfo, int numTables)
{
	int			i;
	int			j;

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &(tblinfo[i]);
		Oid parrelid = tbinfo->parrelid;

		if (parrelid == 0)
			continue;

		for (j = 0; j < numTables; j++)
		{
			TableInfo  *ti = &(tblinfo[j]);
			if (ti->dobj.catId.oid != parrelid)
				continue;
			addObjectDependency(&ti->dobj, tbinfo->dobj.dumpId);
			removeObjectDependency(&tbinfo->dobj, ti->dobj.dumpId);
		}
	}
}

/*
 * getDependencies --- obtain available dependency data
 */
static void
getDependencies(void)
{
	PQExpBuffer query;
	PGresult   *res;
	int			ntups,
				i;
	int			i_classid,
				i_objid,
				i_refclassid,
				i_refobjid,
				i_deptype;
	DumpableObject *dobj,
			   *refdobj;

	if (g_verbose)
		write_msg(NULL, "reading dependency data\n");

	/* Make sure we are in proper schema */
	selectSourceSchema("pg_catalog");

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "SELECT "
					  "classid, objid, refclassid, refobjid, deptype "
					  "FROM pg_depend "
					  "WHERE deptype != 'p' AND deptype != 'e' "
					  "ORDER BY 1,2");

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_classid = PQfnumber(res, "classid");
	i_objid = PQfnumber(res, "objid");
	i_refclassid = PQfnumber(res, "refclassid");
	i_refobjid = PQfnumber(res, "refobjid");
	i_deptype = PQfnumber(res, "deptype");

	/*
	 * Since we ordered the SELECT by referencing ID, we can expect that
	 * multiple entries for the same object will appear together; this saves
	 * on searches.
	 */
	dobj = NULL;

	for (i = 0; i < ntups; i++)
	{
		CatalogId	objId;
		CatalogId	refobjId;
		char		deptype;

		objId.tableoid = atooid(PQgetvalue(res, i, i_classid));
		objId.oid = atooid(PQgetvalue(res, i, i_objid));
		refobjId.tableoid = atooid(PQgetvalue(res, i, i_refclassid));
		refobjId.oid = atooid(PQgetvalue(res, i, i_refobjid));
		deptype = *(PQgetvalue(res, i, i_deptype));

		if (dobj == NULL ||
			dobj->catId.tableoid != objId.tableoid ||
			dobj->catId.oid != objId.oid)
			dobj = findObjectByCatalogId(objId);

		/*
		 * Failure to find objects mentioned in pg_depend is not unexpected,
		 * since for example we don't collect info about TOAST tables.
		 */
		if (dobj == NULL)
		{
#ifdef NOT_USED
			fprintf(stderr, "no referencing object %u %u\n",
					objId.tableoid, objId.oid);
#endif
			continue;
		}

		refdobj = findObjectByCatalogId(refobjId);

		if (refdobj == NULL)
		{
#ifdef NOT_USED
			fprintf(stderr, "no referenced object %u %u\n",
					refobjId.tableoid, refobjId.oid);
#endif
			continue;
		}

		/*
		 * Ordinarily, table rowtypes have implicit dependencies on their
		 * tables.	However, for a composite type the implicit dependency goes
		 * the other way in pg_depend; which is the right thing for DROP but
		 * it doesn't produce the dependency ordering we need. So in that one
		 * case, we reverse the direction of the dependency.
		 */
		if (deptype == 'i' &&
			dobj->objType == DO_TABLE &&
			refdobj->objType == DO_TYPE)
			addObjectDependency(refdobj, dobj->dumpId);
		else
			/* normal case */
			addObjectDependency(dobj, refdobj->dumpId);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}


/*
 * selectSourceSchema - make the specified schema the active search path
 * in the source database.
 *
 * NB: pg_catalog is explicitly searched after the specified schema;
 * so user names are only qualified if they are cross-schema references,
 * and system names are only qualified if they conflict with a user name
 * in the current schema.
 *
 * Whenever the selected schema is not pg_catalog, be careful to qualify
 * references to system catalogs and types in our emitted commands!
 */
static void
selectSourceSchema(const char *schemaName)
{
	static char *curSchemaName = NULL;
	PQExpBuffer query;

	/* Ignore null schema names */
	if (schemaName == NULL || *schemaName == '\0')
		return;
	/* Optimize away repeated selection of same schema */
	if (curSchemaName && strcmp(curSchemaName, schemaName) == 0)
		return;

	query = createPQExpBuffer();
	appendPQExpBuffer(query, "SET search_path = %s",
					  fmtId(schemaName));
	if (strcmp(schemaName, "pg_catalog") != 0)
		appendPQExpBuffer(query, ", pg_catalog");

	do_sql_command(g_conn, query->data);

	destroyPQExpBuffer(query);
	if (curSchemaName)
		free(curSchemaName);
	curSchemaName = strdup(schemaName);
}

/*
 * isGPbackend - returns true if the connected backend is a GreenPlum DB backend.
 */
static bool
testGPbackend(void)
{
	PQExpBuffer query;
	PGresult   *res;
	bool		isGPbackend;

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "SELECT current_setting('gp_role');");
	res = PQexec(g_conn, query->data);

	isGPbackend = (PQresultStatus(res) == PGRES_TUPLES_OK);

	PQclear(res);
	destroyPQExpBuffer(query);

	return isGPbackend;
}

/*
 * testPartitioningSupport - tests whether or not the current GP
 * database includes support for partitioning.
 */
static bool
testPartitioningSupport(void)
{
	PQExpBuffer query;
	PGresult   *res;
	bool		isSupported;

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "SELECT 1 FROM pg_class WHERE relname = 'pg_partition' and relnamespace = 11;");
	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	isSupported = (PQntuples(res) == 1);

	PQclear(res);
	destroyPQExpBuffer(query);

	return isSupported;
}



/*
 * testAttributeEncodingSupport - tests whether or not the current GP
 * database includes support for column encoding.
 */
static bool
testAttributeEncodingSupport(void)
{
	PQExpBuffer query;
	PGresult   *res;
	bool		isSupported;

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "SELECT 1 from pg_catalog.pg_class where relnamespace = 11 and relname  = 'pg_attribute_encoding';");
	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	isSupported = (PQntuples(res) == 1);

	PQclear(res);
	destroyPQExpBuffer(query);

	return isSupported;
}


bool
testExtProtocolSupport(void)
{
	PQExpBuffer query;
	PGresult   *res;
	bool		isSupported;

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "SELECT 1 FROM pg_class WHERE relname = 'pg_extprotocol' and relnamespace = 11;");
	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	isSupported = (PQntuples(res) == 1);

	PQclear(res);
	destroyPQExpBuffer(query);

	return isSupported;
}


/*
 *	addDistributedBy
 *
 *	find the distribution policy of the passed in relation and append the
 *	DISTRIBUTED BY clause to the passed in dump buffer (q).
 */
static void
addDistributedBy(PQExpBuffer q, TableInfo *tbinfo, int actual_atts)
{
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	char	   *policydef;
	char	   *policycol;

	appendPQExpBuffer(query,
					  "SELECT attrnums FROM gp_distribution_policy as p "
					  "WHERE p.localoid = %u",
					  tbinfo->dobj.catId.oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	if (PQntuples(res) != 1)
	{
		/*
		 * There is no entry in the policy table for this table. Report an
		 * error unless this is a zero attribute table (actual_atts == 0).
		 *
		 * In binary_upgrade mode, we run directly against segments, and there
		 * are no gp_distribution_policy rows in segments.
		 */
		if (PQntuples(res) < 1 && actual_atts > 0 && !binary_upgrade)
		{
			/* if this is a catalog table we allow dumping it, skip the error */
			if (strncmp(tbinfo->dobj.namespace->dobj.name, "pg_", 3) != 0)
			{
				write_msg(NULL, "query to obtain distribution policy of table \"%s\" returned no data\n",
						  tbinfo->dobj.name);
				exit_nicely();
			}
		}

		/*
		 * There is more than 1 entry in the policy table for this table.
		 * Report an error.
		 */
		if (PQntuples(res) > 1)
		{
			write_msg(NULL, "query to obtain distribution policy of table \"%s\" returned more than one policy\n",
					  tbinfo->dobj.name);
			exit_nicely();
		}
	}
	else
	{
		/*
		 * There is exactly 1 policy entry for this table (either a concrete
		 * one or NULL).
		 */
		policydef = PQgetvalue(res, 0, 0);

		if (strlen(policydef) > 0)
		{
			/* policy indicates one or more columns to distribute on */
			policydef[strlen(policydef) - 1] = '\0';
			policydef++;
			policycol = nextToken(&policydef, ",");
			appendPQExpBuffer(q, " DISTRIBUTED BY (%s",
							  fmtId(tbinfo->attnames[atoi(policycol) - 1]));
			while ((policycol = nextToken(&policydef, ",")) != NULL)
			{
				appendPQExpBuffer(q, ", %s",
							   fmtId(tbinfo->attnames[atoi(policycol) - 1]));
			}
			appendPQExpBufferChar(q, ')');
		}
		else
		{
			/* policy has an empty policy - distribute randomly */
			appendPQExpBufferStr(q, " DISTRIBUTED RANDOMLY");
		}
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * getFormattedTypeName - retrieve a nicely-formatted type name for the
 * given type name.
 *
 * NB: in 7.3 and up the result may depend on the currently-selected
 * schema; this is why we don't try to cache the names.
 */
static char *
getFormattedTypeName(Oid oid, OidOptions opts)
{
	char	   *result;
	PQExpBuffer query;
	PGresult   *res;
	int			ntups;

	if (oid == 0)
	{
		if ((opts & zeroAsOpaque) != 0)
			return strdup(g_opaque_type);
		else if ((opts & zeroAsAny) != 0)
			return strdup("'any'");
		else if ((opts & zeroAsStar) != 0)
			return strdup("*");
		else if ((opts & zeroAsNone) != 0)
			return strdup("NONE");
	}

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "SELECT pg_catalog.format_type('%u'::pg_catalog.oid, NULL)",
					  oid);

	res = PQexec(g_conn, query->data);
	check_sql_result(res, g_conn, query->data, PGRES_TUPLES_OK);

	/* Expecting a single result only */
	ntups = PQntuples(res);
	if (ntups != 1)
	{
		write_msg(NULL, "query returned %d rows instead of one: %s\n",
				  ntups, query->data);
		exit_nicely();
	}

	/* already quoted */
	result = strdup(PQgetvalue(res, 0, 0));

	PQclear(res);
	destroyPQExpBuffer(query);

	return result;
}

/*
 * fmtQualifiedId - convert a qualified name to the proper format for
 * the source database.
 *
 * Like fmtId, use the result before calling again.
 */
static const char *
fmtQualifiedId(const char *schema, const char *id)
{
	static PQExpBuffer id_return = NULL;

	if (id_return)				/* first time through? */
		resetPQExpBuffer(id_return);
	else
		id_return = createPQExpBuffer();

	if (schema && *schema)
	{
		appendPQExpBuffer(id_return, "%s.",
						  fmtId(schema));
	}
	appendPQExpBuffer(id_return, "%s",
					  fmtId(id));

	return id_return->data;
}

/*
 * Return a column list clause for the given relation.
 *
 * Special case: if there are no undropped columns in the relation, return
 * "", not an invalid "()" column list.
 */
static const char *
fmtCopyColumnList(const TableInfo *ti)
{
	static PQExpBuffer q = NULL;
	int			numatts = ti->numatts;
	char	  **attnames = ti->attnames;
	bool	   *attisdropped = ti->attisdropped;
	bool		needComma;
	int			i;

	if (q)						/* first time through? */
		resetPQExpBuffer(q);
	else
		q = createPQExpBuffer();

	appendPQExpBuffer(q, "(");
	needComma = false;
	for (i = 0; i < numatts; i++)
	{
		if (attisdropped[i])
			continue;
		if (needComma)
			appendPQExpBuffer(q, ", ");
		appendPQExpBuffer(q, "%s", fmtId(attnames[i]));
		needComma = true;
	}

	if (!needComma)
		return "";				/* no undropped columns */

	appendPQExpBuffer(q, ")");
	return q->data;
}

/*
 * Convenience subroutine to execute a SQL command and check for
 * COMMAND_OK status.
 */
static void
do_sql_command(PGconn *conn, const char *query)
{
	PGresult   *res;

	res = PQexec(conn, query);
	check_sql_result(res, conn, query, PGRES_COMMAND_OK);
	PQclear(res);
}

/*
 * Convenience subroutine to verify a SQL command succeeded,
 * and exit with a useful error message if not.
 */
void
check_sql_result(PGresult *res, PGconn *conn, const char *query,
				 ExecStatusType expected)
{
	const char *err;

	if (res && PQresultStatus(res) == expected)
		return;					/* A-OK */

	write_msg(NULL, "SQL command failed\n");
	if (res)
		err = PQresultErrorMessage(res);
	else
		err = PQerrorMessage(conn);
	write_msg(NULL, "Error message from server: %s", err);
	write_msg(NULL, "The command was: %s\n", query);
	exit_nicely();
}

/* START MPP ADDITION */
/*
 * Get next token from string *stringp, where tokens are possibly-empty
 * strings separated by characters from delim.
 *
 * Writes NULs into the string at *stringp to end tokens.
 * delim need not remain constant from call to call.
 * On return, *stringp points past the last NUL written (if there might
														 * be further tokens), or is NULL (if there are definitely no more tokens).
 *
 * If *stringp is NULL, strsep returns NULL.
 */
static char *
nextToken(register char **stringp, register const char *delim)
{
	register char *s;
	register const char *spanp;
	register int c,
				sc;
	char	   *tok;

	if ((s = *stringp) == NULL)
		return (NULL);
	for (tok = s;;)
	{
		c = *s++;
		spanp = delim;
		do
		{
			if ((sc = *spanp++) == c)
			{
				if (c == 0)
					s = NULL;
				else
					s[-1] = 0;
				*stringp = s;
				return (tok);
			}
		} while (sc != 0);
	}
	/* NOTREACHED */
}

/* END MPP ADDITION */
