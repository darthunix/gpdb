/*-------------------------------------------------------------------------
 *
 * pg_regress --- regression test driver
 *
 * This is a C implementation of the previous shell script for running
 * the regression tests, and should be mostly compatible with it.
 * Initial author of C translation: Magnus Hagander
 *
 * This code is released under the terms of the PostgreSQL License.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/test/regress/pg_regress.c,v 1.41.2.4 2009/11/14 15:39:41 mha Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pg_regress.h"

#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#ifdef __linux__
#include <mntent.h>
#endif

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/resource.h>
#endif

#include "getopt_long.h"
#include "pg_config_paths.h"

/* for resultmap we need a list of pairs of strings */
typedef struct _resultmap
{
	char	   *test;
	char	   *type;
	char	   *resultfile;
	struct _resultmap *next;
}	_resultmap;

/*
 * Values obtained from pg_config_paths.h and Makefile.  The PG installation
 * paths are only used in temp_install mode: we use these strings to find
 * out where "make install" will put stuff under the temp_install directory.
 * In non-temp_install mode, the only thing we need is the location of psql,
 * which we expect to find in psqldir, or in the PATH if psqldir isn't given.
 *
 * XXX Because pg_regress is not installed in bindir, we can't support
 * this for relocatable trees as it is.  --psqldir would need to be
 * specified in those cases.
 */
char	   *bindir = PGBINDIR;
char	   *libdir = LIBDIR;
char	   *datadir = PGSHAREDIR;
char	   *host_platform = HOST_TUPLE;

#ifndef WIN32_ONLY_COMPILER
static char *makeprog = MAKEPROG;
#endif

#ifndef WIN32					/* not used in WIN32 case */
static char *shellprog = SHELLPROG;
#endif

static char gpdiffprog[MAXPGPATH];
static char gpstringsubsprog[MAXPGPATH];

/* currently we can use the same diff switches on all platforms */
/* MPP:  Add stuff to ignore all the extra NOTICE messages we give */
const char *basic_diff_opts = "-w -I HINT: -I CONTEXT: -I GP_IGNORE:";
const char *pretty_diff_opts = "-w -I HINT: -I CONTEXT: -I GP_IGNORE: -C3";

/* options settable from command line */
_stringlist *dblist = NULL;
bool		debug = false;
char	   *inputdir = ".";
char	   *outputdir = ".";
char	   *psqldir = PGBINDIR;
bool 		optimizer_enabled = false;
bool 		resgroup_enabled = false;
static _stringlist *loadlanguage = NULL;
static int	max_connections = 0;
static char *encoding = NULL;
static _stringlist *schedulelist = NULL;
static _stringlist *exclude_tests = NULL;
static _stringlist *extra_tests = NULL;
static char *temp_install = NULL;
static char *temp_config = NULL;
static char *top_builddir = NULL;
static int	temp_port = 65432;
static bool nolocale = false;
static bool use_existing = false;
static char *hostname = NULL;
static int	port = -1;
static char *user = NULL;
static char *srcdir = NULL;
static _stringlist *extraroles = NULL;
static char *initfile = NULL;
static char *aodir = NULL;
static char *resgroupdir = NULL;

/* internal variables */
static const char *progname;
static char *logfilename;
static FILE *logfile;
static char *difffilename;

static _resultmap *resultmap = NULL;

static PID_TYPE postmaster_pid = INVALID_PID;
static bool postmaster_running = false;

static int	success_count = 0;
static int	fail_count = 0;
static int	fail_ignore_count = 0;

static bool directory_exists(const char *dir);
static void make_directory(const char *dir);

static void create_database(const char *dbname);
static void drop_database_if_exists(const char *dbname);

static int
run_diff(const char *cmd, const char *filename);

static bool should_exclude_test(char *test);

static void
header(const char *fmt,...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(PG_PRINTF_ATTRIBUTE, 1, 2)));
static void
status(const char *fmt,...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(PG_PRINTF_ATTRIBUTE, 1, 2)));
static void
psql_command(const char *database, const char *query,...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 3)));

#ifdef WIN32
typedef BOOL (WINAPI * __CreateRestrictedToken) (HANDLE, DWORD, DWORD, PSID_AND_ATTRIBUTES, DWORD, PLUID_AND_ATTRIBUTES, DWORD, PSID_AND_ATTRIBUTES, PHANDLE);

/* Windows API define missing from MingW headers */
#define DISABLE_MAX_PRIVILEGE	0x1
#endif

static bool detectCgroupMountPoint(char *cgdir, int len);

/*
 * allow core files if possible.
 */
#if defined(HAVE_GETRLIMIT) && defined(RLIMIT_CORE)
static void
unlimit_core_size(void)
{
	struct rlimit lim;

	getrlimit(RLIMIT_CORE, &lim);
	if (lim.rlim_max == 0)
	{
		fprintf(stderr,
				_("%s: could not set core size: disallowed by hard limit\n"),
				progname);
		return;
	}
	else if (lim.rlim_max == RLIM_INFINITY || lim.rlim_cur < lim.rlim_max)
	{
		lim.rlim_cur = lim.rlim_max;
		setrlimit(RLIMIT_CORE, &lim);
	}
}
#endif


/*
 * Add an item at the end of a stringlist.
 */
void
add_stringlist_item(_stringlist ** listhead, const char *str)
{
	_stringlist *newentry = malloc(sizeof(_stringlist));
	_stringlist *oldentry;

	newentry->str = strdup(str);
	newentry->next = NULL;
	if (*listhead == NULL)
		*listhead = newentry;
	else
	{
		for (oldentry = *listhead; oldentry->next; oldentry = oldentry->next)
			 /* skip */ ;
		oldentry->next = newentry;
	}
}

/*
 * Free a stringlist.
 */
static void
free_stringlist(_stringlist ** listhead)
{
	if (listhead == NULL || *listhead == NULL)
		return;
	if ((*listhead)->next != NULL)
		free_stringlist(&((*listhead)->next));
	free((*listhead)->str);
	free(*listhead);
	*listhead = NULL;
}

/*
 * Split a delimited string into a stringlist
 */
static void
split_to_stringlist(const char *s, const char *delim, _stringlist ** listhead)
{
	char	   *sc = strdup(s);
	char	   *token = strtok(sc, delim);

	while (token)
	{
		add_stringlist_item(listhead, token);
		token = strtok(NULL, delim);
	}
	free(sc);
}

/*
 * Print a progress banner on stdout.
 */
static void
header(const char *fmt,...)
{
	char		tmp[64];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);

	fprintf(stdout, "============== %-38s ==============\n", tmp);
	fflush(stdout);
}

/*
 * Print "doing something ..." --- supplied text should not end with newline
 */
static void
status(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	fflush(stdout);
	va_end(ap);

	if (logfile)
	{
		va_start(ap, fmt);
		vfprintf(logfile, fmt, ap);
		va_end(ap);
	}
}

/*
 * Done "doing something ..."
 */
static void
status_end(void)
{
	fprintf(stdout, "\n");
	fflush(stdout);
	if (logfile)
		fprintf(logfile, "\n");
}

/*
 * shut down temp postmaster
 */
static void
stop_postmaster(void)
{
	if (postmaster_running)
	{
		/* We use pg_ctl to issue the kill and wait for stop */
		char		buf[MAXPGPATH * 2];
		int			r;

		/* On Windows, system() seems not to force fflush, so... */
		fflush(stdout);
		fflush(stderr);

		snprintf(buf, sizeof(buf),
				 SYSTEMQUOTE "\"%s/pg_ctl\" stop -D \"%s/data\" -s -m fast" SYSTEMQUOTE,
				 bindir, temp_install);
		r = system(buf);
		if (r != 0)
		{
			fprintf(stderr, _("\n%s: could not stop postmaster: exit code was %d\n"),
					progname, r);
			exit(2);			/* not exit_nicely(), that would be recursive */
		}

		postmaster_running = false;
	}
}

/*
 * Always exit through here, not through plain exit(), to ensure we make
 * an effort to shut down a temp postmaster
 */
void
exit_nicely(int code)
{
	stop_postmaster();
	exit(code);
}

/*
 * Check whether string matches pattern
 *
 * In the original shell script, this function was implemented using expr(1),
 * which provides basic regular expressions restricted to match starting at
 * the string start (in conventional regex terms, there's an implicit "^"
 * at the start of the pattern --- but no implicit "$" at the end).
 *
 * For now, we only support "." and ".*" as non-literal metacharacters,
 * because that's all that anyone has found use for in resultmap.  This
 * code could be extended if more functionality is needed.
 */
static bool
string_matches_pattern(const char *str, const char *pattern)
{
	while (*str && *pattern)
	{
		if (*pattern == '.' && pattern[1] == '*')
		{
			pattern += 2;
			/* Trailing .* matches everything. */
			if (*pattern == '\0')
				return true;

			/*
			 * Otherwise, scan for a text position at which we can match the
			 * rest of the pattern.
			 */
			while (*str)
			{
				/*
				 * Optimization to prevent most recursion: don't recurse
				 * unless first pattern char might match this text char.
				 */
				if (*str == *pattern || *pattern == '.')
				{
					if (string_matches_pattern(str, pattern))
						return true;
				}

				str++;
			}

			/*
			 * End of text with no match.
			 */
			return false;
		}
		else if (*pattern != '.' && *str != *pattern)
		{
			/*
			 * Not the single-character wildcard and no explicit match? Then
			 * time to quit...
			 */
			return false;
		}

		str++;
		pattern++;
	}

	if (*pattern == '\0')
		return true;			/* end of pattern, so declare match */

	/* End of input string.  Do we have matching pattern remaining? */
	while (*pattern == '.' && pattern[1] == '*')
		pattern += 2;
	if (*pattern == '\0')
		return true;			/* end of pattern, so declare match */

	return false;
}

/*
 * Replace all occurances of a string in a string with a different string.
 * NOTE: Assumes there is enough room in the target buffer!
 */
void
replace_string(char *string, char *replace, char *replacement)
{
	char	   *ptr;

	while ((ptr = strstr(string, replace)) != NULL)
	{
		char	   *dup = strdup(string);

		strlcpy(string, dup, ptr - string + 1);
		strcat(string, replacement);
		strcat(string, dup + (ptr - string) + strlen(replace));
		free(dup);
	}
}

typedef struct replacements
{
	char *abs_srcdir;
	char *abs_builddir;
	char *testtablespace;
	char *dlsuffix;
	char *bindir;
	char *orientation;
	char *cgroup_mnt_point;
} replacements;

/* Internal helper function to detect cgroup mount point at runtime.*/
static bool
detectCgroupMountPoint(char *cgdir, int len)
{
#ifdef __linux__
	struct mntent *me;
	FILE *fp;
	bool ret = false;

	fp = setmntent("/proc/self/mounts", "r");
	if (fp == NULL)
		return ret;

	while ((me = getmntent(fp)))
	{
		char *p;

		if (strcmp(me->mnt_type, "cgroup"))
			continue;

		strncpy(cgdir, me->mnt_dir, len);

		p = strrchr(cgdir, '/');
		if (p != NULL)
		{
			*p = 0;
			ret = true;
		}
		break;
	}

	endmntent(fp);
	return ret;
#else
	return false;
#endif
}

static void
convert_line(char *line, replacements *repls)
{
	replace_string(line, "@cgroup_mnt_point@", repls->cgroup_mnt_point);
	replace_string(line, "@abs_srcdir@", repls->abs_srcdir);
	replace_string(line, "@abs_builddir@", repls->abs_builddir);
	replace_string(line, "@testtablespace@", repls->testtablespace);
	replace_string(line, "@DLSUFFIX@", repls->dlsuffix);
	replace_string(line, "@bindir@", repls->bindir);
	if (repls->orientation)
	{
		replace_string(line, "@orientation@", repls->orientation);
		if (strcmp(repls->orientation, "row") == 0)
			replace_string(line, "@aoseg@", "aoseg");
		else
			replace_string(line, "@aoseg@", "aocsseg");
	}
}

/*
 * Generate two files for each UAO test case, one for row and the
 * other for column orientation.
 */
static int
generate_uao_sourcefiles(char *src_dir, char *dest_dir, char *suffix, replacements *repls)
{
	struct stat st;
	int			ret;
	char	  **name;
	char	  **names;
	int			count = 0;

	/*
	 * Return silently if src_dir or dest_dir is not a directory, in
	 * the same spirit as in convert_sourcefiles_in().
	 */
	ret = stat(src_dir, &st);
	if (ret != 0 || !S_ISDIR(st.st_mode))
		return 0;

	ret = stat(dest_dir, &st);
	if (ret != 0 || !S_ISDIR(st.st_mode))
		return 0;

	names = pgfnames(src_dir);
	if (!names)
		/* Error logged in pgfnames */
		exit_nicely(2);

	/* finally loop on each file and generate the files */
	for (name = names; *name; name++)
	{
		char		srcfile[MAXPGPATH];
		char		destfile_row[MAXPGPATH];
		char		destfile_col[MAXPGPATH];
		char		prefix[MAXPGPATH];
		FILE	   *infile,
				   *outfile_row,
				   *outfile_col;
		char		line[1024];
		char		line_row[1024];
		bool		has_tokens = false;

		/* reject filenames not finishing in ".source" */
		if (strlen(*name) < 8)
			continue;
		if (strcmp(*name + strlen(*name) - 7, ".source") != 0)
			continue;

		count++;

		/*
		 * Build the full actual paths to open.  Optimizer specific
		 * answer filenames must end with "optimizer".
		 */
		snprintf(srcfile, MAXPGPATH, "%s/%s", src_dir, *name);
		if (strlen(*name) > 17 &&
			strcmp(*name + strlen(*name) - 17, "_optimizer.source") == 0)
		{
			snprintf(prefix, strlen(*name) - 16, "%s", *name);
			snprintf(destfile_row, MAXPGPATH, "%s/%s_row_optimizer.%s",
					 dest_dir, prefix, suffix);
			snprintf(destfile_col, MAXPGPATH, "%s/%s_column_optimizer.%s",
					 dest_dir, prefix, suffix);
		}
		else
		{
			snprintf(prefix, strlen(*name) - 6, "%s", *name);
			snprintf(destfile_row, MAXPGPATH, "%s/%s_row.%s",
					 dest_dir, prefix, suffix);
			snprintf(destfile_col, MAXPGPATH, "%s/%s_column.%s",
					 dest_dir, prefix, suffix);
		}

		infile = fopen(srcfile, "r");
		if (!infile)
		{
			fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
					progname, srcfile, strerror(errno));
			exit_nicely(2);
		}
		outfile_row = fopen(destfile_row, "w");
		if (!outfile_row)
		{
			fprintf(stderr, _("%s: could not open file \"%s\" for writing: %s\n"),
					progname, destfile_row, strerror(errno));
			exit_nicely(2);
		}
		outfile_col = fopen(destfile_col, "w");
		if (!outfile_col)
		{
			fprintf(stderr, _("%s: could not open file \"%s\" for writing: %s\n"),
					progname, destfile_col, strerror(errno));
			exit_nicely(2);
		}

		while (fgets(line, sizeof(line), infile))
		{
			strncpy(line_row, line, sizeof(line));
			repls->orientation = "row";
			convert_line(line_row, repls);
			repls->orientation = "column";
			convert_line(line, repls);
			fputs(line, outfile_col);
			fputs(line_row, outfile_row);
			/*
			 * Remember if there are any more tokens that we didn't recognize.
			 * They need to be handled by the gpstringsubs.pl script
			 */
			if (!has_tokens && strchr(line, '@') != NULL)
				has_tokens = true;
		}

		fclose(infile);
		fclose(outfile_row);
		fclose(outfile_col);
		if (has_tokens)
		{
			char		cmd[MAXPGPATH * 3];
			snprintf(cmd, sizeof(cmd),
					 SYSTEMQUOTE "%s %s" SYSTEMQUOTE, gpstringsubsprog, destfile_row);
			if (run_diff(cmd, destfile_row) != 0)
			{
				fprintf(stderr, _("%s: could not convert %s\n"),
						progname, destfile_row);
			}
			snprintf(cmd, sizeof(cmd),
					 SYSTEMQUOTE "%s %s" SYSTEMQUOTE, gpstringsubsprog, destfile_col);
			if (run_diff(cmd, destfile_col) != 0)
			{
				fprintf(stderr, _("%s: could not convert %s\n"),
						progname, destfile_col);
			}
		}
	}

	pgfnames_cleanup(names);
	return count;
}

/*
 * Convert *.source found in the "source" directory, replacing certain tokens
 * in the file contents with their intended values, and put the resulting files
 * in the "dest" directory, replacing the ".source" prefix in their names with
 * the given suffix.
 */
static int
convert_sourcefiles_in(char *source, char * dest_dir, char *dest, char *suffix)
{
	char		abs_srcdir[MAXPGPATH];
	char		abs_builddir[MAXPGPATH];
	char		testtablespace[MAXPGPATH];
	char		indir[MAXPGPATH];
	char		cgroup_mnt_point[MAXPGPATH];
	replacements repls;
	struct stat st;
	int			ret;
	char	  **name;
	char	  **names;
	int			count = 0;

#ifdef WIN32
	char	   *c;
#endif

	if (!getcwd(abs_builddir, sizeof(abs_builddir)))
	{
		fprintf(stderr, _("%s: could not get current directory: %s\n"),
				progname, strerror(errno));
		exit_nicely(2);
	}

	/*
	 * in a VPATH build, use the provided source directory; otherwise, use the
	 * current directory.
	 */
	if (srcdir)
		strlcpy(abs_srcdir, srcdir, MAXPGPATH);
	else
		strlcpy(abs_srcdir, abs_builddir, MAXPGPATH);

	snprintf(indir, MAXPGPATH, "%s/%s", abs_srcdir, source);

	/* Check that indir actually exists and is a directory */
	ret = stat(indir, &st);
	if (ret != 0 || !S_ISDIR(st.st_mode))
	{
		/*
		 * No warning, to avoid noise in tests that do not have
		 * these directories; for example, ecpg, contrib and src/pl.
		 */
		return count;
	}

	names = pgfnames(indir);
	if (!names)
		/* Error logged in pgfnames */
		exit_nicely(2);

	/* also create the output directory if not present */
	{
		char		outdir[MAXPGPATH];

		snprintf(outdir, MAXPGPATH, "%s/%s", dest_dir, dest);

		if (!directory_exists(outdir))
			make_directory(outdir);
	}

#ifdef WIN32
	/* in Win32, replace backslashes with forward slashes */
	for (c = abs_builddir; *c; c++)
		if (*c == '\\')
			*c = '/';
	for (c = abs_srcdir; *c; c++)
		if (*c == '\\')
			*c = '/';
#endif

	snprintf(testtablespace, MAXPGPATH, "%s/testtablespace", abs_builddir);

#ifdef WIN32
	/*
	 * On Windows only, clean out the test tablespace dir, or create it if it
	 * doesn't exist.  On other platforms we expect the Makefile to take
	 * care of that.  (We don't migrate that functionality in here because
	 * it'd be harder to cope with platform-specific issues such as SELinux.)
	 *
	 * XXX it would be better if pg_regress.c had nothing at all to do with
	 * testtablespace, and this were handled by a .BAT file or similar on
	 * Windows.  See pgsql-hackers discussion of 2008-01-18.
	 */
	if (directory_exists(testtablespace))
		rmtree(testtablespace, true);
	make_directory(testtablespace);
#endif

	memset(cgroup_mnt_point, 0, sizeof(cgroup_mnt_point));
	if (!detectCgroupMountPoint(cgroup_mnt_point,
								sizeof(cgroup_mnt_point) - 1))
		strcpy(cgroup_mnt_point, "/sys/fs/cgroup");

	memset(&repls, 0, sizeof(repls));
	repls.abs_srcdir = abs_srcdir;
	repls.abs_builddir = abs_builddir;
	repls.testtablespace = testtablespace;
	repls.dlsuffix = DLSUFFIX;
	repls.bindir = bindir;
	repls.cgroup_mnt_point = cgroup_mnt_point;

	/* finally loop on each file and do the replacement */
	for (name = names; *name; name++)
	{
		char		srcfile[MAXPGPATH];
		char		destfile[MAXPGPATH];
		char		prefix[MAXPGPATH];
		FILE	   *infile,
				   *outfile;
		char		line[1024];
		bool		has_tokens = false;


		if (aodir && strncmp(*name, aodir, strlen(aodir)) == 0 &&
			(strlen(*name) < 8 || strcmp(*name + strlen(*name) - 7, ".source") != 0))
		{
			snprintf(srcfile, MAXPGPATH, "%s/%s",  indir, *name);
			snprintf(destfile, MAXPGPATH, "%s/%s/%s", dest_dir, dest, *name);
			count += generate_uao_sourcefiles(srcfile, destfile, suffix, &repls);
			continue;
		}

		if (resgroupdir && strncmp(*name, resgroupdir, strlen(resgroupdir)) == 0 &&
			(strlen(*name) < 8 || strcmp(*name + strlen(*name) - 7, ".source") != 0))
		{
			snprintf(srcfile, MAXPGPATH, "%s/%s", source, *name);
			snprintf(destfile, MAXPGPATH, "%s/%s/%s", dest_dir, dest, *name);
			count += convert_sourcefiles_in(srcfile, dest_dir, destfile, suffix);
			continue;
		}

		/* reject filenames not finishing in ".source" */
		if (strlen(*name) < 8)
			continue;
		if (strcmp(*name + strlen(*name) - 7, ".source") != 0)
			continue;

		count++;

		/* build the full actual paths to open */
		snprintf(prefix, strlen(*name) - 6, "%s", *name);
		snprintf(srcfile, MAXPGPATH, "%s/%s", indir, *name);
		snprintf(destfile, MAXPGPATH, "%s/%s/%s.%s", dest_dir, dest, 
				 prefix, suffix);

		infile = fopen(srcfile, "r");
		if (!infile)
		{
			fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
					progname, srcfile, strerror(errno));
			exit_nicely(2);
		}
		outfile = fopen(destfile, "w");
		if (!outfile)
		{
			fprintf(stderr, _("%s: could not open file \"%s\" for writing: %s\n"),
					progname, destfile, strerror(errno));
			exit_nicely(2);
		}
		while (fgets(line, sizeof(line), infile))
		{
			convert_line(line, &repls);
			fputs(line, outfile);

			/*
			 * Remember if there are any more tokens that we didn't recognize.
			 * They need to be handled by the gpstringsubs.pl script
			 */
			if (!has_tokens && strchr(line, '@') != NULL)
				has_tokens = true;
		}
		fclose(infile);
		fclose(outfile);

		if (has_tokens)
		{
			char		cmd[MAXPGPATH * 3];
			snprintf(cmd, sizeof(cmd),
					 SYSTEMQUOTE "%s %s" SYSTEMQUOTE, gpstringsubsprog, destfile);
			if (run_diff(cmd, destfile) != 0)
			{
				fprintf(stderr, _("%s: could not convert %s\n"),
						progname, destfile);
			}
		}

	}

	/*
	 * If we didn't process any files, complain because it probably means
	 * somebody neglected to pass the needed --inputdir argument.
	 */
	if (count <= 0)
	{
		fprintf(stderr, _("%s: no *.source files found in \"%s\"\n"),
				progname, indir);
		exit_nicely(2);
	}

	pgfnames_cleanup(names);

	return count;
}

/* Create the .sql, .out and .yml files from the .source files, if any */
static void
convert_sourcefiles(void)
{
	convert_sourcefiles_in("input", outputdir, "sql", "sql");
	convert_sourcefiles_in("output", outputdir, "expected", "out");

	convert_sourcefiles_in("yml_in", inputdir, "yml", "yml");
}

/*
 * Scan resultmap file to find which platform-specific expected files to use.
 *
 * The format of each line of the file is
 *		   testname/hostplatformpattern=substitutefile
 * where the hostplatformpattern is evaluated per the rules of expr(1),
 * namely, it is a standard regular expression with an implicit ^ at the start.
 * (We currently support only a very limited subset of regular expressions,
 * see string_matches_pattern() above.)  What hostplatformpattern will be
 * matched against is the config.guess output.	(In the shell-script version,
 * we also provided an indication of whether gcc or another compiler was in
 * use, but that facility isn't used anymore.)
 */
static void
load_resultmap(void)
{
	char		buf[MAXPGPATH];
	FILE	   *f;

	/* scan the file ... */
	snprintf(buf, sizeof(buf), "%s/resultmap", inputdir);
	f = fopen(buf, "r");
	if (!f)
	{
		/* OK if it doesn't exist, else complain */
		if (errno == ENOENT)
			return;
		fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
				progname, buf, strerror(errno));
		exit_nicely(2);
	}

	while (fgets(buf, sizeof(buf), f))
	{
		char	   *platform;
		char	   *file_type;
		char	   *expected;
		int			i;

		/* strip trailing whitespace, especially the newline */
		i = strlen(buf);
		while (i > 0 && isspace((unsigned char) buf[i - 1]))
			buf[--i] = '\0';

		/* parse out the line fields */
		file_type = strchr(buf, ':');
		if (!file_type)
		{
			fprintf(stderr, _("incorrectly formatted resultmap entry: %s\n"),
					buf);
			exit_nicely(2);
		}
		*file_type++ = '\0';

		platform = strchr(file_type, ':');
		if (!platform)
		{
			fprintf(stderr, _("incorrectly formatted resultmap entry: %s\n"),
					buf);
			exit_nicely(2);
		}
		*platform++ = '\0';
		expected = strchr(platform, '=');
		if (!expected)
		{
			fprintf(stderr, _("incorrectly formatted resultmap entry: %s\n"),
					buf);
			exit_nicely(2);
		}
		*expected++ = '\0';

		/*
		 * if it's for current platform, save it in resultmap list. Note: by
		 * adding at the front of the list, we ensure that in ambiguous cases,
		 * the last match in the resultmap file is used. This mimics the
		 * behavior of the old shell script.
		 */
		if (string_matches_pattern(host_platform, platform))
		{
			_resultmap *entry = malloc(sizeof(_resultmap));

			entry->test = strdup(buf);
			entry->type = strdup(file_type);
			entry->resultfile = strdup(expected);
			entry->next = resultmap;
			resultmap = entry;
		}
	}
	fclose(f);
}

/*
 * Check in resultmap if we should be looking at a different file
 */
static
const char *
get_expectfile(const char *testname, const char *file)
{
	char	   *file_type;
	_resultmap *rm;

	/*
	 * Determine the file type from the file name. This is just what is
	 * following the last dot in the file name.
	 */
	if (!file || !(file_type = strrchr(file, '.')))
		return NULL;

	file_type++;

	for (rm = resultmap; rm != NULL; rm = rm->next)
	{
		if (strcmp(testname, rm->test) == 0 && strcmp(file_type, rm->type) == 0)
		{
			return rm->resultfile;
		}
	}

	return NULL;
}

/*
 * Handy subroutine for setting an environment variable "var" to "val"
 */
static void
doputenv(const char *var, const char *val)
{
	char	   *s = malloc(strlen(var) + strlen(val) + 2);

	sprintf(s, "%s=%s", var, val);
	putenv(s);
}

/*
 * Set the environment variable "pathname", prepending "addval" to its
 * old value (if any).
 */
static void
add_to_path(const char *pathname, char separator, const char *addval)
{
	char	   *oldval = getenv(pathname);
	char	   *newval;

	if (!oldval || !oldval[0])
	{
		/* no previous value */
		newval = malloc(strlen(pathname) + strlen(addval) + 2);
		sprintf(newval, "%s=%s", pathname, addval);
	}
	else
	{
		newval = malloc(strlen(pathname) + strlen(addval) + strlen(oldval) + 3);
		sprintf(newval, "%s=%s%c%s", pathname, addval, separator, oldval);
	}
	putenv(newval);
}

/*
 * Prepare environment variables for running regression tests
 */
static void
initialize_environment(void)
{
	char	   *tmp;

	if (nolocale)
	{
		/*
		 * Clear out any non-C locale settings
		 */
		unsetenv("LC_COLLATE");
		unsetenv("LC_CTYPE");
		unsetenv("LC_MONETARY");
		unsetenv("LC_NUMERIC");
		unsetenv("LC_TIME");
		unsetenv("LANG");
		/* On Windows the default locale cannot be English, so force it */
#if defined(WIN32) || defined(__CYGWIN__)
		putenv("LANG=en");
#endif
	}

	/*
	 * Set translation-related settings to English; otherwise psql will
	 * produce translated messages and produce diffs.  (XXX If we ever support
	 * translation of pg_regress, this needs to be moved elsewhere, where psql
	 * is actually called.)
	 */
	unsetenv("LANGUAGE");
	unsetenv("LC_ALL");
	putenv("LC_MESSAGES=C");

	/*
	 * Set multibyte as requested
	 */
	if (encoding && strlen(encoding) > 0)
		doputenv("PGCLIENTENCODING", encoding);
	else
		unsetenv("PGCLIENTENCODING");

	/*
	 * Set timezone and datestyle for datetime-related tests
	 */
	putenv("PGTZ=PST8PDT");
	putenv("PGDATESTYLE=Postgres, MDY");

	/*
	 * Likewise set intervalstyle to ensure consistent results.  This is a bit
	 * more painful because we must use PGOPTIONS, and we want to preserve the
	 * user's ability to set other variables through that.
	 */
	{
		const char *my_pgoptions = "-c intervalstyle=postgres_verbose";
		const char *old_pgoptions = getenv("PGOPTIONS");
		char	   *new_pgoptions;

		if (!old_pgoptions)
			old_pgoptions = "";
		new_pgoptions = malloc(strlen(old_pgoptions) + strlen(my_pgoptions) + 12);
		sprintf(new_pgoptions, "PGOPTIONS=%s %s", old_pgoptions, my_pgoptions);
		putenv(new_pgoptions);
	}

	if (temp_install)
	{
		/*
		 * Clear out any environment vars that might cause psql to connect to
		 * the wrong postmaster, or otherwise behave in nondefault ways. (Note
		 * we also use psql's -X switch consistently, so that ~/.psqlrc files
		 * won't mess things up.)  Also, set PGPORT to the temp port, and set
		 * or unset PGHOST depending on whether we are using TCP or Unix
		 * sockets.
		 */
		unsetenv("PGDATABASE");
		unsetenv("PGUSER");
		unsetenv("PGSERVICE");
		unsetenv("PGSSLMODE");
		unsetenv("PGREQUIRESSL");
		unsetenv("PGCONNECT_TIMEOUT");
		unsetenv("PGDATA");
		if (hostname != NULL)
			doputenv("PGHOST", hostname);
		else
			unsetenv("PGHOST");
		unsetenv("PGHOSTADDR");
		if (port != -1)
		{
			char		s[16];

			sprintf(s, "%d", port);
			doputenv("PGPORT", s);
		}

		/*
		 * GNU make stores some flags in the MAKEFLAGS environment variable to
		 * pass arguments to its own children.	If we are invoked by make,
		 * that causes the make invoked by us to think its part of the make
		 * task invoking us, and so it tries to communicate with the toplevel
		 * make.  Which fails.
		 *
		 * Unset the variable to protect against such problems.  We also reset
		 * MAKELEVEL to be certain the child doesn't notice the make above us.
		 */
		unsetenv("MAKEFLAGS");
		unsetenv("MAKELEVEL");

		/*
		 * Adjust path variables to point into the temp-install tree
		 */
		tmp = malloc(strlen(temp_install) + 32 + strlen(bindir));
		sprintf(tmp, "%s/install/%s", temp_install, bindir);
		bindir = tmp;

		tmp = malloc(strlen(temp_install) + 32 + strlen(libdir));
		sprintf(tmp, "%s/install/%s", temp_install, libdir);
		libdir = tmp;

		tmp = malloc(strlen(temp_install) + 32 + strlen(datadir));
		sprintf(tmp, "%s/install/%s", temp_install, datadir);
		datadir = tmp;

		/* psql will be installed into temp-install bindir */
		psqldir = bindir;

		/*
		 * Set up shared library paths to include the temp install.
		 *
		 * LD_LIBRARY_PATH covers many platforms.  DYLD_LIBRARY_PATH works on
		 * Darwin, and maybe other Mach-based systems.	LIBPATH is for AIX.
		 * Windows needs shared libraries in PATH (only those linked into
		 * executables, not dlopen'ed ones). Feel free to account for others
		 * as well.
		 */
		add_to_path("LD_LIBRARY_PATH", ':', libdir);
		add_to_path("DYLD_LIBRARY_PATH", ':', libdir);
		add_to_path("LIBPATH", ':', libdir);
#if defined(WIN32)
		add_to_path("PATH", ';', libdir);
#elif defined(__CYGWIN__)
		add_to_path("PATH", ':', libdir);
#endif
	}
	else
	{
		const char *pghost;
		const char *pgport;

		/*
		 * When testing an existing install, we honor existing environment
		 * variables, except if they're overridden by command line options.
		 */
		if (hostname != NULL)
		{
			doputenv("PGHOST", hostname);
			unsetenv("PGHOSTADDR");
		}
		if (port != -1)
		{
			char		s[16];

			sprintf(s, "%d", port);
			doputenv("PGPORT", s);
		}
		if (user != NULL)
			doputenv("PGUSER", user);

		/*
		 * Report what we're connecting to
		 */
		pghost = getenv("PGHOST");
		pgport = getenv("PGPORT");
#ifndef HAVE_UNIX_SOCKETS
		if (!pghost)
			pghost = "localhost";
#endif

		if (pghost && pgport)
			printf(_("(using postmaster on %s, port %s)\n"), pghost, pgport);
		if (pghost && !pgport)
			printf(_("(using postmaster on %s, default port)\n"), pghost);
		if (!pghost && pgport)
			printf(_("(using postmaster on Unix socket, port %s)\n"), pgport);
		if (!pghost && !pgport)
			printf(_("(using postmaster on Unix socket, default port)\n"));
	}

	convert_sourcefiles();
	load_resultmap();
}

/*
 * Issue a command via psql, connecting to the specified database
 *
 * Since we use system(), this doesn't return until the operation finishes
 */
static void
psql_command(const char *database, const char *query,...)
{
	char		query_formatted[1024];
	char		query_escaped[2048];
	char		psql_cmd[MAXPGPATH + 2048];
	va_list		args;
	char	   *s;
	char	   *d;

	/* Generate the query with insertion of sprintf arguments */
	va_start(args, query);
	vsnprintf(query_formatted, sizeof(query_formatted), query, args);
	va_end(args);

	/* Now escape any shell double-quote metacharacters */
	d = query_escaped;
	for (s = query_formatted; *s; s++)
	{
		if (strchr("\\\"$`", *s))
			*d++ = '\\';
		*d++ = *s;
	}
	*d = '\0';

	/* And now we can build and execute the shell command */
	snprintf(psql_cmd, sizeof(psql_cmd),
			 SYSTEMQUOTE "\"%s%spsql\" -X -c \"%s\" \"%s\"" SYSTEMQUOTE,
			 psqldir ? psqldir : "",
			 psqldir ? "/" : "",
			 query_escaped,
			 database);

	if (system(psql_cmd) != 0)
	{
		/* psql probably already reported the error */
		fprintf(stderr, _("command failed: %s\n"), psql_cmd);
		exit_nicely(2);
	}
}

/*
 * Spawn a process to execute the given shell command; don't wait for it
 *
 * Returns the process ID (or HANDLE) so we can wait for it later
 */
PID_TYPE
spawn_process(const char *cmdline)
{
#ifndef WIN32
	pid_t		pid;

	/*
	 * Must flush I/O buffers before fork.	Ideally we'd use fflush(NULL) here
	 * ... does anyone still care about systems where that doesn't work?
	 */
	fflush(stdout);
	fflush(stderr);
	if (logfile)
		fflush(logfile);

	pid = fork();
	if (pid == -1)
	{
		fprintf(stderr, _("%s: could not fork: %s\n"),
				progname, strerror(errno));
		exit_nicely(2);
	}
	if (pid == 0)
	{
		/*
		 * In child
		 *
		 * Instead of using system(), exec the shell directly, and tell it to
		 * "exec" the command too.	This saves two useless processes per
		 * parallel test case.
		 */
		char	   *cmdline2 = malloc(strlen(cmdline) + 6);

		sprintf(cmdline2, "exec %s", cmdline);
		execl(shellprog, shellprog, "-c", cmdline2, (char *) NULL);
		fprintf(stderr, _("%s: could not exec \"%s\": %s\n"),
				progname, shellprog, strerror(errno));
		exit(1);				/* not exit_nicely here... */
	}
	/* in parent */
	return pid;
#else
	char	   *cmdline2;
	BOOL		b;
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	HANDLE		origToken;
	HANDLE		restrictedToken;
	SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
	SID_AND_ATTRIBUTES dropSids[2];
	__CreateRestrictedToken _CreateRestrictedToken = NULL;
	HANDLE		Advapi32Handle;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);

	Advapi32Handle = LoadLibrary("ADVAPI32.DLL");
	if (Advapi32Handle != NULL)
	{
		_CreateRestrictedToken = (__CreateRestrictedToken) GetProcAddress(Advapi32Handle, "CreateRestrictedToken");
	}

	if (_CreateRestrictedToken == NULL)
	{
		if (Advapi32Handle != NULL)
			FreeLibrary(Advapi32Handle);
		fprintf(stderr, _("%s: cannot create restricted tokens on this platform\n"),
				progname);
		exit_nicely(2);
	}

	/* Open the current token to use as base for the restricted one */
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &origToken))
	{
		fprintf(stderr, _("could not open process token: %lu\n"),
				GetLastError());
		exit_nicely(2);
	}

	/* Allocate list of SIDs to remove */
	ZeroMemory(&dropSids, sizeof(dropSids));
	if (!AllocateAndInitializeSid(&NtAuthority, 2,
								  SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &dropSids[0].Sid) ||
		!AllocateAndInitializeSid(&NtAuthority, 2,
								  SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_POWER_USERS, 0, 0, 0, 0, 0, 0, &dropSids[1].Sid))
	{
		fprintf(stderr, _("could not allocate SIDs: %lu\n"), GetLastError());
		exit_nicely(2);
	}

	b = _CreateRestrictedToken(origToken,
							   DISABLE_MAX_PRIVILEGE,
							   sizeof(dropSids) / sizeof(dropSids[0]),
							   dropSids,
							   0, NULL,
							   0, NULL,
							   &restrictedToken);

	FreeSid(dropSids[1].Sid);
	FreeSid(dropSids[0].Sid);
	CloseHandle(origToken);
	FreeLibrary(Advapi32Handle);

	if (!b)
	{
		fprintf(stderr, _("could not create restricted token: %lu\n"),
				GetLastError());
		exit_nicely(2);
	}

	cmdline2 = malloc(strlen(cmdline) + 8);
	sprintf(cmdline2, "cmd /c %s", cmdline);

#ifndef __CYGWIN__
	AddUserToTokenDacl(restrictedToken);
#endif

	if (!CreateProcessAsUser(restrictedToken,
							 NULL,
							 cmdline2,
							 NULL,
							 NULL,
							 TRUE,
							 CREATE_SUSPENDED,
							 NULL,
							 NULL,
							 &si,
							 &pi))
	{
		fprintf(stderr, _("could not start process for \"%s\": %lu\n"),
				cmdline2, GetLastError());
		exit_nicely(2);
	}

	free(cmdline2);

    ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	return pi.hProcess;
#endif
}

/*
 * Count bytes in file
 */
static long
file_size(const char *file)
{
	long		r;
	FILE	   *f = fopen(file, "r");

	if (!f)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
				progname, file, strerror(errno));
		return -1;
	}
	fseek(f, 0, SEEK_END);
	r = ftell(f);
	fclose(f);
	return r;
}

/*
 * Count lines in file
 */
static int
file_line_count(const char *file)
{
	int			c;
	int			l = 0;
	FILE	   *f = fopen(file, "r");

	if (!f)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
				progname, file, strerror(errno));
		return -1;
	}
	while ((c = fgetc(f)) != EOF)
	{
		if (c == '\n')
			l++;
	}
	fclose(f);
	return l;
}

bool
file_exists(const char *file)
{
	FILE	   *f = fopen(file, "r");

	if (!f)
		return false;
	fclose(f);
	return true;
}

static bool
directory_exists(const char *dir)
{
	struct stat st;

	if (stat(dir, &st) != 0)
		return false;
	if (S_ISDIR(st.st_mode))
		return true;
	return false;
}

/* Create a directory */
static void
make_directory(const char *dir)
{
	if (mkdir(dir, S_IRWXU | S_IRWXG | S_IRWXO) < 0)
	{
		fprintf(stderr, _("%s: could not create directory \"%s\": %s\n"),
				progname, dir, strerror(errno));
		exit_nicely(2);
	}
}

/*
 * In: filename.ext, Return: filename_i.ext, where 0 < i <= 9
 */
static char *
get_alternative_expectfile(const char *expectfile, int i)
{
	char	   *last_dot;
	int			ssize = strlen(expectfile) + 2 + 1;
	char	   *tmp = (char *) malloc(ssize);
	char	   *s = (char *) malloc(ssize);

	strcpy(tmp, expectfile);
	last_dot = strrchr(tmp, '.');
	if (!last_dot)
	{
		free(tmp);
		free(s);
		return NULL;
	}
	*last_dot = '\0';
	snprintf(s, ssize, "%s_%d.%s", tmp, i, last_dot + 1);
	free(tmp);
	return s;
}

/*
 * Run a "diff" command and also check that it didn't crash
 */
static int
run_diff(const char *cmd, const char *filename)
{
	int			r;

	r = system(cmd);
	if (!WIFEXITED(r) || WEXITSTATUS(r) > 1)
	{
		fprintf(stderr, _("diff command failed with status %d: %s\n"), r, cmd);
		exit_nicely(2);
	}
#ifdef WIN32

	/*
	 * On WIN32, if the 'diff' command cannot be found, system() returns 1,
	 * but produces nothing to stdout, so we check for that here.
	 */
	if (WEXITSTATUS(r) == 1 && file_size(filename) <= 0)
	{
		fprintf(stderr, _("diff command not found: %s\n"), cmd);
		exit_nicely(2);
	}
#else
	UnusedArg(filename);
#endif

	return WEXITSTATUS(r);
}

/*
 * Check the actual result file for the given test against expected results
 *
 * Returns true if different (failure), false if correct match found.
 * In the true case, the diff is appended to the diffs file.
 */
static bool
results_differ(const char *testname, const char *resultsfile, const char *default_expectfile)
{
	char		expectfile[MAXPGPATH];
	char		diff[MAXPGPATH];
	char		cmd[MAXPGPATH * 3];
	char		best_expect_file[MAXPGPATH];
    char        diff_opts[MAXPGPATH];
    char        m_pretty_diff_opts[MAXPGPATH];
	FILE	   *difffile;
	int			best_line_count;
	int			i;
	int			l;
	const char *platform_expectfile;

	/*
	 * We can pass either the resultsfile or the expectfile, they should have
	 * the same type (filename.type) anyway.
	 */
	platform_expectfile = get_expectfile(testname, resultsfile);

	strlcpy(expectfile, default_expectfile, sizeof(expectfile));
	if (platform_expectfile)
	{
		/*
		 * Replace everything afer the last slash in expectfile with what the
		 * platform_expectfile contains.
		 */
		char	   *p = strrchr(expectfile, '/');

		if (p)
			strcpy(++p, platform_expectfile);
	}

	/* Name to use for temporary diff file */
	snprintf(diff, sizeof(diff), "%s.diff", resultsfile);
    
	/* Add init file arguments if provided via commandline */
	if (initfile)
	{
	  snprintf(diff_opts, sizeof(diff_opts),
			   "%s --gpd_init %s", basic_diff_opts, initfile);

	  snprintf(m_pretty_diff_opts, sizeof(m_pretty_diff_opts),
			   "%s --gpd_init %s", pretty_diff_opts, initfile);
	}
	else
	{
		snprintf(diff_opts, sizeof(diff_opts),
			   "%s", basic_diff_opts);

		snprintf(m_pretty_diff_opts, sizeof(m_pretty_diff_opts),
                 "%s", pretty_diff_opts);
	}

	/* OK, run the diff */
	snprintf(cmd, sizeof(cmd),
			 SYSTEMQUOTE "%s %s \"%s\" \"%s\" > \"%s\"" SYSTEMQUOTE,
			 gpdiffprog, diff_opts, expectfile, resultsfile, diff);

	/* Is the diff file empty? */
	if (run_diff(cmd, diff) == 0)
	{
		unlink(diff);
		return false;
	}

	/* There may be secondary comparison files that match better */
	best_line_count = file_line_count(diff);
	strcpy(best_expect_file, expectfile);

	for (i = 0; i <= 9; i++)
	{
		char	   *alt_expectfile;

		alt_expectfile = get_alternative_expectfile(expectfile, i);
		if (!file_exists(alt_expectfile))
			continue;

		snprintf(cmd, sizeof(cmd),
				 SYSTEMQUOTE "%s %s \"%s\" \"%s\" > \"%s\"" SYSTEMQUOTE,
				 gpdiffprog, diff_opts, alt_expectfile, resultsfile, diff);

		if (run_diff(cmd, diff) == 0)
		{
			unlink(diff);
			return false;
		}

		l = file_line_count(diff);
		if (l < best_line_count)
		{
			/* This diff was a better match than the last one */
			best_line_count = l;
			strlcpy(best_expect_file, alt_expectfile, sizeof(best_expect_file));
		}
		free(alt_expectfile);
	}

	/*
	 * fall back on the canonical results file if we haven't tried it yet and
	 * haven't found a complete match yet.
	 */

	if (platform_expectfile)
	{
		snprintf(cmd, sizeof(cmd),
				 SYSTEMQUOTE "%s %s \"%s\" \"%s\" > \"%s\"" SYSTEMQUOTE,
				 gpdiffprog, diff_opts, default_expectfile, resultsfile, diff);

		if (run_diff(cmd, diff) == 0)
		{
			/* No diff = no changes = good */
			unlink(diff);
			return false;
		}

		l = file_line_count(diff);
		if (l < best_line_count)
		{
			/* This diff was a better match than the last one */
			best_line_count = l;
			strlcpy(best_expect_file, default_expectfile, sizeof(best_expect_file));
		}
	}

	/*
	 * Use the best comparison file to generate the "pretty" diff, which we
	 * append to the diffs summary file.
	 */
	snprintf(cmd, sizeof(cmd),
			 SYSTEMQUOTE "%s %s \"%s\" \"%s\" >> \"%s\"" SYSTEMQUOTE,
			 gpdiffprog, m_pretty_diff_opts, best_expect_file, resultsfile, difffilename);
	run_diff(cmd, difffilename);

	/* And append a separator */
	difffile = fopen(difffilename, "a");
	if (difffile)
	{
		fprintf(difffile,
				"\n======================================================================\n\n");
		fclose(difffile);
	}

	unlink(diff);
	return true;
}

/*
 * Wait for specified subprocesses to finish, and return their exit
 * statuses into statuses[]
 *
 * If names isn't NULL, print each subprocess's name as it finishes
 *
 * Note: it's OK to scribble on the pids array, but not on the names array
 */
static void
wait_for_tests(PID_TYPE *pids, int *statuses, char **names, struct timeval *end_times, int num_tests)
{
	int			tests_left;
	int			i;

#ifdef WIN32
	PID_TYPE   *active_pids = malloc(num_tests * sizeof(PID_TYPE));

	memcpy(active_pids, pids, num_tests * sizeof(PID_TYPE));
#endif

	tests_left = num_tests;
	while (tests_left > 0)
	{
		PID_TYPE	p;

#ifndef WIN32
		int			exit_status;

		p = wait(&exit_status);

		if (p == INVALID_PID)
		{
			fprintf(stderr, _("failed to wait for subprocesses: %s\n"),
					strerror(errno));
			exit_nicely(2);
		}
#else
		DWORD		exit_status;
		int			r;

		r = WaitForMultipleObjects(tests_left, active_pids, FALSE, INFINITE);
		if (r < WAIT_OBJECT_0 || r >= WAIT_OBJECT_0 + tests_left)
		{
			fprintf(stderr, _("failed to wait for subprocesses: %lu\n"),
					GetLastError());
			exit_nicely(2);
		}
		p = active_pids[r - WAIT_OBJECT_0];
		/* compact the active_pids array */
		active_pids[r - WAIT_OBJECT_0] = active_pids[tests_left - 1];
#endif   /* WIN32 */

		for (i = 0; i < num_tests; i++)
		{
			if (p == pids[i])
			{
#ifdef WIN32
				GetExitCodeProcess(pids[i], &exit_status);
				CloseHandle(pids[i]);
#endif
				pids[i] = INVALID_PID;
				statuses[i] = (int) exit_status;
				if (names)
					status(" %s", names[i]);
				if (end_times)
					gettimeofday(&end_times[i], NULL);
				tests_left--;
				break;
			}
		}
	}

#ifdef WIN32
	free(active_pids);
#endif
}

/*
 * report nonzero exit code from a test process
 */
static void
log_child_failure(int exitstatus)
{
	if (WIFEXITED(exitstatus))
		status(_(" (test process exited with exit code %d)"),
			   WEXITSTATUS(exitstatus));
	else if (WIFSIGNALED(exitstatus))
	{
#if defined(WIN32)
		status(_(" (test process was terminated by exception 0x%X)"),
			   WTERMSIG(exitstatus));
#elif defined(HAVE_DECL_SYS_SIGLIST) && HAVE_DECL_SYS_SIGLIST
		status(_(" (test process was terminated by signal %d: %s)"),
			   WTERMSIG(exitstatus),
			   WTERMSIG(exitstatus) < NSIG ?
			   sys_siglist[WTERMSIG(exitstatus)] : "(unknown))");
#else
		status(_(" (test process was terminated by signal %d)"),
			   WTERMSIG(exitstatus));
#endif
	}
	else
		status(_(" (test process exited with unrecognized status %d)"),
			   exitstatus);
}

/*
 * Run all the tests specified in one schedule file
 */
static void
run_schedule(const char *schedule, test_function tfunc)
{
#define MAX_PARALLEL_TESTS 100
	char	   *tests[MAX_PARALLEL_TESTS];
	_stringlist *resultfiles[MAX_PARALLEL_TESTS];
	_stringlist *expectfiles[MAX_PARALLEL_TESTS];
	_stringlist *tags[MAX_PARALLEL_TESTS];
	PID_TYPE	pids[MAX_PARALLEL_TESTS];
	int			statuses[MAX_PARALLEL_TESTS];
	struct timeval end_times[MAX_PARALLEL_TESTS];
	_stringlist *ignorelist = NULL;
	char		scbuf[1024];
	FILE	   *scf;
	int			line_num = 0;

	memset(resultfiles, 0, sizeof(_stringlist *) * MAX_PARALLEL_TESTS);
	memset(expectfiles, 0, sizeof(_stringlist *) * MAX_PARALLEL_TESTS);
	memset(tags, 0, sizeof(_stringlist *) * MAX_PARALLEL_TESTS);
	memset(end_times, 0, sizeof(struct timeval) * MAX_PARALLEL_TESTS);

	scf = fopen(schedule, "r");
	if (!scf)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
				progname, schedule, strerror(errno));
		exit_nicely(2);
	}

	while (fgets(scbuf, sizeof(scbuf), scf))
	{
		char	   *test = NULL;
		char	   *c;
		int			num_tests;
		bool		inword;
		int			i;
		struct timeval start_time;

		line_num++;

		for (i = 0; i < MAX_PARALLEL_TESTS; i++)
		{
			if (resultfiles[i] == NULL)
				break;
			free_stringlist(&resultfiles[i]);
			free_stringlist(&expectfiles[i]);
			free_stringlist(&tags[i]);
		}

		/* strip trailing whitespace, especially the newline */
		i = strlen(scbuf);
		while (i > 0 && isspace((unsigned char) scbuf[i - 1]))
			scbuf[--i] = '\0';

		if (scbuf[0] == '\0' || scbuf[0] == '#')
			continue;
		if (strncmp(scbuf, "test: ", 6) == 0)
			test = scbuf + 6;
		else if (strncmp(scbuf, "ignore: ", 8) == 0)
		{
			c = scbuf + 8;
			while (*c && isspace((unsigned char) *c))
				c++;
			add_stringlist_item(&ignorelist, c);

			/*
			 * Note: ignore: lines do not run the test, they just say that
			 * failure of this test when run later on is to be ignored. A bit
			 * odd but that's how the shell-script version did it.
			 */
			continue;
		}
		else
		{
			fprintf(stderr, _("syntax error in schedule file \"%s\" line %d: %s\n"),
					schedule, line_num, scbuf);
			exit_nicely(2);
		}

		num_tests = 0;
		inword = false;
		for (c = test; *c; c++)
		{
			if (isspace((unsigned char) *c))
			{
				*c = '\0';
				inword = false;
			}
			else if (!inword)
			{
				if (num_tests >= MAX_PARALLEL_TESTS)
				{
					/* can't print scbuf here, it's already been trashed */
					fprintf(stderr, _("too many parallel tests in schedule file \"%s\", line %d\n"),
							schedule, line_num);
					exit_nicely(2);
				}

				if (num_tests - 1 >= 0 && should_exclude_test(tests[num_tests - 1]))
					num_tests--;

				tests[num_tests] = c;
				num_tests++;
				inword = true;
			}
		}

		/* The last test in the line needs to be checked for exclusion */
		if (num_tests - 1 >= 0 && should_exclude_test(tests[num_tests - 1]))
		{
			num_tests--;

			/* All tests in this line are to be excluded, so go to the next line */
			if (num_tests == 0)
				continue;
		}

		if (num_tests == 0)
		{
			fprintf(stderr, _("syntax error in schedule file \"%s\" line %d: %s\n"),
					schedule, line_num, scbuf);
			exit_nicely(2);
		}

		gettimeofday(&start_time, NULL);
		if (num_tests == 1)
		{
#ifdef TEST_EACH_SCRIPT_IN_ITS_OWN_DB
		    _stringlist *strList;
            for (strList = dblist; strList; strList = strList->next)
        	    drop_database_if_exists(strList->str);
            for (strList = dblist; strList; strList = strList->next)
        		create_database(strList->str);
#endif
			status(_("test %-20s ... "), tests[0]);
			pids[0] = (tfunc) (tests[0], &resultfiles[0], &expectfiles[0], &tags[0]);
			wait_for_tests(pids, statuses, NULL, end_times, 1);
			/* status line is finished below */
		}
		else if (max_connections > 0 && max_connections < num_tests)
		{
			int			oldest = 0;

			status(_("parallel group (%d tests, in groups of %d): "),
				   num_tests, max_connections);
			for (i = 0; i < num_tests; i++)
			{
				if (i - oldest >= max_connections)
				{
					wait_for_tests(pids + oldest, statuses + oldest,
								   tests + oldest, end_times + oldest, i - oldest);
					oldest = i;
				}
				pids[i] = (tfunc) (tests[i], &resultfiles[i], &expectfiles[i], &tags[i]);
			}
			wait_for_tests(pids + oldest, statuses + oldest,
						   tests + oldest, end_times + oldest, i - oldest);
			status_end();
		}
		else
		{
			status(_("parallel group (%d tests): "), num_tests);
			for (i = 0; i < num_tests; i++)
			{
				pids[i] = (tfunc) (tests[i], &resultfiles[i], &expectfiles[i], &tags[i]);
			}
			wait_for_tests(pids, statuses, tests, end_times, num_tests);
			status_end();
		}

		/* Check results for all tests */
		for (i = 0; i < num_tests; i++)
		{
			_stringlist *rl,
					   *el,
					   *tl;
			bool		differ = false;
			double		diff_secs = 0, diff_elapse = 0;
			struct timeval diff_start_time, diff_end_time;

			if (num_tests > 1)
				status(_("     %-20s ... "), tests[i]);

			diff_secs = end_times[i].tv_usec - start_time.tv_usec;
			diff_secs /= 1000000;
			diff_secs += end_times[i].tv_sec - start_time.tv_sec;
			/*
			 * Advance over all three lists simultaneously.
			 *
			 * Compare resultfiles[j] with expectfiles[j] always. Tags are
			 * optional but if there are tags, the tag list has the same
			 * length as the other two lists.
			 */

			gettimeofday(&diff_start_time, NULL);
			for (rl = resultfiles[i], el = expectfiles[i], tl = tags[i];
				 rl != NULL;	/* rl and el have the same length */
				 rl = rl->next, el = el->next)
			{
				bool		newdiff;

				if (tl)
					tl = tl->next;		/* tl has the same lengt has rl and el
										 * if it exists */

				newdiff = results_differ(tests[i], rl->str, el->str);
				if (newdiff && tl)
				{
					printf("%s ", tl->str);
				}
				differ |= newdiff;
			}
			gettimeofday(&diff_end_time, NULL);

			diff_elapse = diff_end_time.tv_usec - diff_start_time.tv_usec;
			diff_elapse /= 1000000;
			diff_elapse += diff_end_time.tv_sec - diff_start_time.tv_sec;

			if (differ)
			{
				bool		ignore = false;
				_stringlist *sl;

				for (sl = ignorelist; sl != NULL; sl = sl->next)
				{
					if (strcmp(tests[i], sl->str) == 0)
					{
						ignore = true;
						break;
					}
				}
				if (ignore)
				{
					status(_("failed (ignored)"));
					fail_ignore_count++;
				}
				else
				{
					status(_("FAILED"));
    				status(_(" (%.2f sec)  (diff:%.2f sec)"), diff_secs, diff_elapse);
					fail_count++;
				}
			}
			else
			{
				status(_("ok"));
				status(_(" (%.2f sec)  (diff:%.2f sec)"), diff_secs, diff_elapse);
				success_count++;
			}

			if (statuses[i] != 0)
				log_child_failure(statuses[i]);

			status_end();
		}
	}

	fclose(scf);
}

/*
 * Run a single test
 */
static void
run_single_test(const char *test, test_function tfunc)
{
	PID_TYPE	pid;
	int			exit_status;
	_stringlist *resultfiles = NULL;
	_stringlist *expectfiles = NULL;
	_stringlist *tags = NULL;
	_stringlist *rl,
			   *el,
			   *tl;
	bool		differ = false;

	status(_("test %-20s ... "), test);
	pid = (tfunc) (test, &resultfiles, &expectfiles, &tags);
	wait_for_tests(&pid, &exit_status, NULL, NULL, 1);

	/*
	 * Advance over all three lists simultaneously.
	 *
	 * Compare resultfiles[j] with expectfiles[j] always. Tags are optional
	 * but if there are tags, the tag list has the same length as the other
	 * two lists.
	 */
	for (rl = resultfiles, el = expectfiles, tl = tags;
		 rl != NULL;			/* rl and el have the same length */
		 rl = rl->next, el = el->next)
	{
		bool		newdiff;

		if (tl)
			tl = tl->next;		/* tl has the same lengt has rl and el if it
								 * exists */

		newdiff = results_differ(test, rl->str, el->str);
		if (newdiff && tl)
		{
			printf("%s ", tl->str);
		}
		differ |= newdiff;
	}

	if (differ)
	{
		status(_("FAILED"));
		fail_count++;
	}
	else
	{
		status(_("ok"));
		success_count++;
	}

	if (exit_status != 0)
		log_child_failure(exit_status);

	status_end();
}

/*
 * Find the other binaries that we need. Currently, gpdiff.pl and
 * gpstringsubs.pl.
 */
static void
find_helper_programs(const char *argv0)
{
	if (find_other_exec(argv0, "gpdiff.pl", NULL, gpdiffprog) != 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		fprintf(stderr,
				_("The program \"gpdiff.pl\" is needed by %s "
				  "but was not found in the same directory as \"%s\".\n"),
				progname, full_path);
		exit(1);
	}
	if (find_other_exec(argv0, "gpstringsubs.pl", NULL, gpstringsubsprog) != 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		fprintf(stderr,
				_("The program \"gpstringsubs.pl\" is needed by %s "
				  "but was not found in the same directory as \"%s\".\n"),
				progname, full_path);
		exit(1);
	}
}
/*
 * Create the summary-output files (making them empty if already existing)
 */
static void
open_result_files(void)
{
	char		file[MAXPGPATH];
	FILE	   *difffile;

	/* create the log file (copy of running status output) */
	snprintf(file, sizeof(file), "%s/regression.out", outputdir);
	logfilename = strdup(file);
	logfile = fopen(logfilename, "w");
	if (!logfile)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for writing: %s\n"),
				progname, logfilename, strerror(errno));
		exit_nicely(2);
	}

	/* create the diffs file as empty */
	snprintf(file, sizeof(file), "%s/regression.diffs", outputdir);
	difffilename = strdup(file);
	difffile = fopen(difffilename, "w");
	if (!difffile)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for writing: %s\n"),
				progname, difffilename, strerror(errno));
		exit_nicely(2);
	}
	/* we don't keep the diffs file open continuously */
	fclose(difffile);

	/* also create the output directory if not present */
	snprintf(file, sizeof(file), "%s/results", outputdir);
	if (!directory_exists(file))
		make_directory(file);
}

static void
drop_database_if_exists(const char *dbname)
{
	header(_("dropping database \"%s\""), dbname);
	psql_command("postgres", "DROP DATABASE IF EXISTS \"%s\"", dbname);
}

static void
create_database(const char *dbname)
{
	_stringlist *sl;

	/*
	 * We use template0 so that any installation-local cruft in template1 will
	 * not mess up the tests.
	 */
	header(_("creating database \"%s\""), dbname);
	if (encoding && strlen(encoding) > 0)
		psql_command("postgres", "CREATE DATABASE \"%s\" TEMPLATE=template0 ENCODING='%s'", dbname, encoding);
	else
		psql_command("postgres", "CREATE DATABASE \"%s\" TEMPLATE=template0", dbname);
	psql_command(dbname,
				 "ALTER DATABASE \"%s\" SET lc_messages TO 'C';"
				 "ALTER DATABASE \"%s\" SET lc_monetary TO 'C';"
				 "ALTER DATABASE \"%s\" SET lc_numeric TO 'C';"
				 "ALTER DATABASE \"%s\" SET lc_time TO 'C';"
			"ALTER DATABASE \"%s\" SET timezone_abbreviations TO 'Default';",
				 dbname, dbname, dbname, dbname, dbname);

	/*
	 * Install any requested procedural languages
	 */
	for (sl = loadlanguage; sl != NULL; sl = sl->next)
	{
		header(_("installing %s"), sl->str);
		psql_command(dbname, "CREATE LANGUAGE \"%s\"", sl->str);
	}
}

static void
drop_role_if_exists(const char *rolename)
{
	header(_("dropping role \"%s\""), rolename);
	psql_command("postgres", "DROP ROLE IF EXISTS \"%s\"", rolename);
}

static void
create_role(const char *rolename, const _stringlist * granted_dbs)
{
	header(_("creating role \"%s\""), rolename);
	psql_command("postgres", "CREATE ROLE \"%s\" WITH LOGIN", rolename);
	for (; granted_dbs != NULL; granted_dbs = granted_dbs->next)
	{
		psql_command("postgres", "GRANT ALL ON DATABASE \"%s\" TO \"%s\"",
					 granted_dbs->str, rolename);
	}
}

static char *
make_absolute_path(const char *in)
{
	char	   *result;

	if (is_absolute_path(in))
		result = strdup(in);
	else
	{
		static char cwdbuf[MAXPGPATH];

		if (!cwdbuf[0])
		{
			if (!getcwd(cwdbuf, sizeof(cwdbuf)))
			{
				fprintf(stderr, _("could not get current working directory: %s\n"), strerror(errno));
				exit_nicely(2);
			}
		}

		result = malloc(strlen(cwdbuf) + strlen(in) + 2);
		sprintf(result, "%s/%s", cwdbuf, in);
	}

	canonicalize_path(result);
	return result;
}

static char *
trim_white_space(char *str)
{
	char *end;
	while (isspace((unsigned char)*str))
	{
		str++;
	}

	if (*str == 0)
	{
		return str;
	}

	end = str + strlen(str) - 1;
	while (end > str && isspace((unsigned char)*end))
	{
		end--;
	}

	*(end+1) = 0;
	return str;
}

/*
 * Should the test be excluded from running
 */
static bool
should_exclude_test(char *test)
{
	_stringlist *sl;
	for (sl = exclude_tests; sl != NULL; sl = sl->next)
	{
		if (strcmp(test, sl->str) == 0)
			return true;
	}

	return false;
}

/*
 * @brief Check whether a feature (e.g. optimizer) is on or off.
 * If the input feature is optimizer, then set the global
 * variable "optimizer_enabled" accordingly.
 *
 * @param feature_name Name of the feature to be checked (e.g. optimizer)
 * @param feature_value Expected value when the feature is enabled (i.e., on or group)
 * @param on_msg Message to be printed when the feature is enabled
 * @param off_msg Message to be printed when the feature is disabled
 * @return true if the feature is enabled; false otherwise
 */
static bool
check_feature_status(const char *feature_name, const char *feature_value,
					 const char *on_msg, const char *off_msg)
{
	char psql_cmd[MAXPGPATH];
	char statusfilename[MAXPGPATH];
	char line[1024];
	bool isEnabled = false;
	int len;

	header(_("checking %s status"), feature_name);

	snprintf(statusfilename, sizeof(statusfilename), SYSTEMQUOTE "%s/%s_status.out" SYSTEMQUOTE, outputdir, feature_name);

	len = snprintf(psql_cmd, sizeof(psql_cmd),
			SYSTEMQUOTE "\"%s%spsql\" -X -t -c \"show %s;\" -o \"%s\" -d \"postgres\"" SYSTEMQUOTE,
			psqldir ? psqldir : "",
			psqldir ? "/" : "",
			feature_name,
			statusfilename);

	if (len >= sizeof(psql_cmd))
		exit_nicely(2);

	if (system(psql_cmd) != 0)
		exit_nicely(2);

	FILE *statusfile = fopen(statusfilename, "r");
	if (!statusfile)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
				progname, statusfilename, strerror(errno));
		exit_nicely(2);
	}

	while (fgets(line, sizeof(line), statusfile))
	{
		char *trimmed = trim_white_space(line);
		if (strcmp(trimmed, feature_value) == 0)
		{
			status(_("%s"), on_msg);
			isEnabled = true;
			break;
		}
	}
	if (!isEnabled)
		status(_("%s"), off_msg);

	status_end();
	fclose(statusfile);
	unlink(statusfilename);
	return isEnabled;
}

static void
help(void)
{
	printf(_("PostgreSQL regression test driver\n"));
	printf(_("\n"));
	printf(_("Usage: %s [options...] [extra tests...]\n"), progname);
	printf(_("\n"));
	printf(_("Options:\n"));
	printf(_("  --dbname=DB               use database DB (default \"regression\")\n"));
	printf(_("  --debug                   turn on debug mode in programs that are run\n"));
	printf(_("  --inputdir=DIR            take input files from DIR (default \".\")\n"));
	printf(_("  --load-language=lang      load the named language before running the\n"));
	printf(_("                            tests; can appear multiple times\n"));
	printf(_("  --create-role=ROLE        create the specified role before testing\n"));
	printf(_("  --max-connections=N       maximum number of concurrent connections\n"));
	printf(_("                            (default is 0 meaning unlimited)\n"));
	printf(_("  --multibyte=ENCODING      use ENCODING as the multibyte encoding\n"));
	printf(_("  --outputdir=DIR           place output files in DIR (default \".\")\n"));
	printf(_("  --schedule=FILE           use test ordering schedule from FILE\n"));
	printf(_("                            (can be used multiple times to concatenate)\n"));
	printf(_("  --exclude-tests=TEST      command or space delimited tests to exclude from running\n"));
	printf(_("  --srcdir=DIR              absolute path to source directory (for VPATH builds)\n"));
	printf(_("  --temp-install=DIR        create a temporary installation in DIR\n"));
    printf(_(" --init-file=GPD_INIT_FILE  init file to be used for gpdiff\n"));
	printf(_("  --ao-dir=DIR              directory name prefix containing generic\n"));
	printf(_("                            UAO row and column tests\n"));
	printf(_("  --resgroup-dir=DIR        directory name prefix containing resgroup tests\n"));
	printf(_("  --use-existing            use an existing installation\n"));
	printf(_("\n"));
	printf(_("Options for \"temp-install\" mode:\n"));
	printf(_("  --no-locale               use C locale\n"));
	printf(_("  --top-builddir=DIR        (relative) path to top level build directory\n"));
	printf(_("  --temp-port=PORT          port number to start temp postmaster on\n"));
	printf(_("  --temp-config=PATH        append contents of PATH to temporary config\n"));
	printf(_("\n"));
	printf(_("Options for using an existing installation:\n"));
	printf(_("  --host=HOST               use postmaster running on HOST\n"));
	printf(_("  --port=PORT               use postmaster running at PORT\n"));
	printf(_("  --user=USER               connect as USER\n"));
	printf(_("  --psqldir=DIR             use psql in DIR (default: find in PATH)\n"));
	printf(_("\n"));
	printf(_("The exit status is 0 if all tests passed, 1 if some tests failed, and 2\n"));
	printf(_("if the tests could not be run for some reason.\n"));
	printf(_("\n"));
	printf(_("Report bugs to <bugs@greenplum.org>.\n"));
}

int
regression_main(int argc, char *argv[], init_function ifunc, test_function tfunc)
{
	_stringlist *sl;
	int			c;
	int			i;
	int			option_index;
	char		buf[MAXPGPATH * 4];

	static struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'V'},
		{"dbname", required_argument, NULL, 1},
		{"debug", no_argument, NULL, 2},
		{"inputdir", required_argument, NULL, 3},
		{"load-language", required_argument, NULL, 4},
		{"max-connections", required_argument, NULL, 5},
		{"multibyte", required_argument, NULL, 6},
		{"outputdir", required_argument, NULL, 7},
		{"schedule", required_argument, NULL, 8},
		{"temp-install", required_argument, NULL, 9},
		{"no-locale", no_argument, NULL, 10},
		{"top-builddir", required_argument, NULL, 11},
		{"temp-port", required_argument, NULL, 12},
		{"host", required_argument, NULL, 13},
		{"port", required_argument, NULL, 14},
		{"user", required_argument, NULL, 15},
		{"psqldir", required_argument, NULL, 16},
		{"srcdir", required_argument, NULL, 17},
		{"create-role", required_argument, NULL, 18},
		{"temp-config", required_argument, NULL, 19},
        {"init-file", required_argument, NULL, 20},
        {"ao-dir", required_argument, NULL, 21},
        {"resgroup-dir", required_argument, NULL, 22},
        {"exclude-tests", required_argument, NULL, 23},
        {"use-existing", no_argument, NULL, 24},
		{NULL, 0, NULL, 0}
	};

	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_regress"));

#ifndef HAVE_UNIX_SOCKETS
	/* no unix domain sockets available, so change default */
	hostname = "localhost";
#endif

	/*
	 * We call the initialization function here because that way we can set
	 * default parameters and let them be overwritten by the commandline.
	 */
	ifunc();

	while ((c = getopt_long(argc, argv, "hV", long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'h':
				help();
				exit_nicely(0);
			case 'V':
				puts("pg_regress (PostgreSQL) " PG_VERSION);
				exit_nicely(0);
			case 1:

				/*
				 * If a default database was specified, we need to remove it
				 * before we add the specified one.
				 */
				free_stringlist(&dblist);
				split_to_stringlist(strdup(optarg), ", ", &dblist);
				break;
			case 2:
				debug = true;
				break;
			case 3:
				inputdir = strdup(optarg);
				break;
			case 4:
				add_stringlist_item(&loadlanguage, optarg);
				break;
			case 5:
				max_connections = atoi(optarg);
				break;
			case 6:
				encoding = strdup(optarg);
				break;
			case 7:
				outputdir = strdup(optarg);
				break;
			case 8:
				add_stringlist_item(&schedulelist, optarg);
				break;
			case 9:
				temp_install = make_absolute_path(optarg);
				break;
			case 10:
				nolocale = true;
				break;
			case 11:
				top_builddir = strdup(optarg);
				break;
			case 12:
				{
					int			p = atoi(optarg);

					/* Since Makefile isn't very bright, check port range */
					if (p >= 1024 && p <= 65535)
						temp_port = p;
				}
				break;
			case 13:
				hostname = strdup(optarg);
				break;
			case 14:
				port = atoi(optarg);
				break;
			case 15:
				user = strdup(optarg);
				break;
			case 16:
				/* "--psqldir=" should mean to use PATH */
				if (strlen(optarg))
					psqldir = strdup(optarg);
				break;
			case 17:
				srcdir = strdup(optarg);
				break;
			case 18:
				split_to_stringlist(strdup(optarg), ", ", &extraroles);
				break;
			case 19:
				temp_config = strdup(optarg);
				break;
            case 20:
                initfile = strdup(optarg);
                break;
            case 21:
                aodir = strdup(optarg);
                break;
            case 22:
                resgroupdir = strdup(optarg);
                break;
            case 23:
                split_to_stringlist(strdup(optarg), ", ", &exclude_tests);
                break;
            case 24:
                use_existing = true;
                break;
			default:
				/* getopt_long already emitted a complaint */
				fprintf(stderr, _("\nTry \"%s -h\" for more information.\n"),
						progname);
				exit_nicely(2);
		}
	}

	/*
	 * if we still have arguments, they are extra tests to run
	 */
	while (argc - optind >= 1)
	{
		add_stringlist_item(&extra_tests, argv[optind]);
		optind++;
	}

	if (temp_install)
		port = temp_port;

	/*
	 * Initialization
	 */
	find_helper_programs(argv[0]);
	open_result_files();

	initialize_environment();

#if defined(HAVE_GETRLIMIT) && defined(RLIMIT_CORE)
	unlimit_core_size();
#endif

	if (temp_install)
	{
		/*
		 * Prepare the temp installation
		 */
		if (!top_builddir)
		{
			fprintf(stderr, _("--top-builddir must be specified when using --temp-install\n"));
			exit_nicely(2);
		}

		if (directory_exists(temp_install))
		{
			header(_("removing existing temp installation"));
			rmtree(temp_install, true);
		}

		header(_("creating temporary installation"));

		/* make the temp install top directory */
		make_directory(temp_install);

		/* and a directory for log files */
		snprintf(buf, sizeof(buf), "%s/log", outputdir);
		if (!directory_exists(buf))
			make_directory(buf);

		/* "make install" */
#ifndef WIN32_ONLY_COMPILER
		snprintf(buf, sizeof(buf),
				 SYSTEMQUOTE "\"%s\" -C \"%s\" DESTDIR=\"%s/install\" install with_perl=no with_python=no > \"%s/log/install.log\" 2>&1" SYSTEMQUOTE,
				 makeprog, top_builddir, temp_install, outputdir);
#else
		snprintf(buf, sizeof(buf),
				 SYSTEMQUOTE "perl \"%s/src/tools/msvc/install.pl\" \"%s/install\" >\"%s/log/install.log\" 2>&1" SYSTEMQUOTE,
				 top_builddir, temp_install, outputdir);
#endif
		if (system(buf))
		{
			fprintf(stderr, _("\n%s: installation failed\nExamine %s/log/install.log for the reason.\nCommand was: %s\n"), progname, outputdir, buf);
			exit_nicely(2);
		}

		/* initdb */
		header(_("initializing database system"));
		snprintf(buf, sizeof(buf),
				 SYSTEMQUOTE "\"%s/initdb\" -D \"%s/data\" -L \"%s\" --noclean%s%s > \"%s/log/initdb.log\" 2>&1" SYSTEMQUOTE,
				 bindir, temp_install, datadir,
				 debug ? " --debug" : "",
				 nolocale ? " --no-locale" : "",
				 outputdir);
		if (system(buf))
		{
			fprintf(stderr, _("\n%s: initdb failed\nExamine %s/log/initdb.log for the reason.\nCommand was: %s\n"), progname, outputdir, buf);
			exit_nicely(2);
		}

		/* add any extra config specified to the postgresql.conf */
		if (temp_config != NULL)
		{
			FILE	   *extra_conf;
			FILE	   *pg_conf;
			char		line_buf[1024];

			snprintf(buf, sizeof(buf), "%s/data/postgresql.conf", temp_install);
			pg_conf = fopen(buf, "a");
			if (pg_conf == NULL)
			{
				fprintf(stderr, _("\n%s: could not open %s for adding extra config:\nError was %s\n"), progname, buf, strerror(errno));
				exit_nicely(2);
			}
			extra_conf = fopen(temp_config, "r");
			if (extra_conf == NULL)
			{
				fprintf(stderr, _("\n%s: could not open %s to read extra config:\nError was %s\n"), progname, temp_config, strerror(errno));
				exit_nicely(2);
			}
			while (fgets(line_buf, sizeof(line_buf), extra_conf) != NULL)
				fputs(line_buf, pg_conf);
			fclose(extra_conf);
			fclose(pg_conf);
		}

		/*
		 * Start the temp postmaster
		 */
		header(_("starting postmaster"));
		snprintf(buf, sizeof(buf),
				 SYSTEMQUOTE "\"%s/postgres\" -D \"%s/data\" -F%s -c \"listen_addresses=%s\" > \"%s/log/postmaster.log\" 2>&1" SYSTEMQUOTE,
				 bindir, temp_install,
				 debug ? " -d 5" : "",
				 hostname ? hostname : "",
				 outputdir);
		postmaster_pid = spawn_process(buf);
		if (postmaster_pid == INVALID_PID)
		{
			fprintf(stderr, _("\n%s: could not spawn postmaster: %s\n"),
					progname, strerror(errno));
			exit_nicely(2);
		}

		/*
		 * Wait till postmaster is able to accept connections (normally only a
		 * second or so, but Cygwin is reportedly *much* slower).  Don't wait
		 * forever, however.
		 */
		snprintf(buf, sizeof(buf),
				 SYSTEMQUOTE "\"%s/psql\" -X postgres <%s 2>%s" SYSTEMQUOTE,
				 bindir, DEVNULL, DEVNULL);
		for (i = 0; i < 60; i++)
		{
			/* Done if psql succeeds */
			if (system(buf) == 0)
				break;

			/*
			 * Fail immediately if postmaster has exited
			 */
#ifndef WIN32
			if (kill(postmaster_pid, 0) != 0)
#else
			if (WaitForSingleObject(postmaster_pid, 0) == WAIT_OBJECT_0)
#endif
			{
				fprintf(stderr, _("\n%s: postmaster failed\nExamine %s/log/postmaster.log for the reason\n"), progname, outputdir);
				exit_nicely(2);
			}

			pg_usleep(1000000L);
		}
		if (i >= 60)
		{
			fprintf(stderr, _("\n%s: postmaster did not respond within 60 seconds\nExamine %s/log/postmaster.log for the reason\n"), progname, outputdir);

			/*
			 * If we get here, the postmaster is probably wedged somewhere in
			 * startup.  Try to kill it ungracefully rather than leaving a
			 * stuck postmaster that might interfere with subsequent test
			 * attempts.
			 */
#ifndef WIN32
			if (kill(postmaster_pid, SIGKILL) != 0 &&
				errno != ESRCH)
				fprintf(stderr, _("\n%s: could not kill failed postmaster: %s\n"),
						progname, strerror(errno));
#else
			if (TerminateProcess(postmaster_pid, 255) == 0)
				fprintf(stderr, _("\n%s: could not kill failed postmaster: %lu\n"),
						progname, GetLastError());
#endif

			exit_nicely(2);
		}

		postmaster_running = true;

		printf(_("running on port %d with pid %lu\n"),
			   temp_port, (unsigned long) postmaster_pid);
	}
	else
	{
		/*
		 * Using an existing installation, so may need to get rid of
		 * pre-existing database(s) and role(s)
		 */
		if (!use_existing)
		{
			for (sl = dblist; sl; sl = sl->next)
				drop_database_if_exists(sl->str);
			for (sl = extraroles; sl; sl = sl->next)
				drop_role_if_exists(sl->str);
		}
	}

	/*
	 * Create the test database(s) and role(s)
	 */
	if (!use_existing)
	{
		for (sl = dblist; sl; sl = sl->next)
			create_database(sl->str);
		for (sl = extraroles; sl; sl = sl->next)
			create_role(sl->str, dblist);
	}

	/*
	 * Find out if optimizer is on or off
	 */
	optimizer_enabled = check_feature_status("optimizer", "on",
			"Optimizer enabled. Using optimizer answer files whenever possible",
			"Optimizer disabled. Using planner answer files");

	/*
	 * Find out if gp_resource_manager is group or not
	 */
	resgroup_enabled = check_feature_status("gp_resource_manager", "group",
			"Resource group enabled. Using resource group answer files whenever possible",
			"Resource group disabled. Using default answer files");

	/*
	 * Ready to run the tests
	 */
	header(_("running regression test queries"));

	for (sl = schedulelist; sl != NULL; sl = sl->next)
	{
		run_schedule(sl->str, tfunc);
	}

	for (sl = extra_tests; sl != NULL; sl = sl->next)
	{
		run_single_test(sl->str, tfunc);
	}

	/*
	 * Shut down temp installation's postmaster
	 */
	if (temp_install)
	{
		header(_("shutting down postmaster"));
		stop_postmaster();
	}

	fclose(logfile);

	/*
	 * Emit nice-looking summary message
	 */
	if (fail_count == 0 && fail_ignore_count == 0)
		snprintf(buf, sizeof(buf),
				 _(" All %d tests passed. "),
				 success_count);
	else if (fail_count == 0)	/* fail_count=0, fail_ignore_count>0 */
		snprintf(buf, sizeof(buf),
				 _(" %d of %d tests passed, %d failed test(s) ignored. "),
				 success_count,
				 success_count + fail_ignore_count,
				 fail_ignore_count);
	else if (fail_ignore_count == 0)	/* fail_count>0 && fail_ignore_count=0 */
		snprintf(buf, sizeof(buf),
				 _(" %d of %d tests failed. "),
				 fail_count,
				 success_count + fail_count);
	else
		/* fail_count>0 && fail_ignore_count>0 */
		snprintf(buf, sizeof(buf),
				 _(" %d of %d tests failed, %d of these failures ignored. "),
				 fail_count + fail_ignore_count,
				 success_count + fail_count + fail_ignore_count,
				 fail_ignore_count);

	putchar('\n');
	for (i = strlen(buf); i > 0; i--)
		putchar('=');
	printf("\n%s\n", buf);
	for (i = strlen(buf); i > 0; i--)
		putchar('=');
	putchar('\n');
	putchar('\n');

	if (file_size(difffilename) > 0)
	{
		printf(_("The differences that caused some tests to fail can be viewed in the\n"
				 "file \"%s\".  A copy of the test summary that you see\n"
				 "above is saved in the file \"%s\".\n\n"),
			   difffilename, logfilename);
	}
	else
	{
		unlink(difffilename);
		unlink(logfilename);
	}

	if (fail_count != 0)
		exit_nicely(1);

	return 0;
}
