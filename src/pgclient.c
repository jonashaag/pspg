/*-------------------------------------------------------------------------
 *
 * pgcliend.c
 *	  execute query and format result
 *
 * Portions Copyright (c) 2017-2019 Pavel Stehule
 *
 * IDENTIFICATION
 *	  src/pgclient.c
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <string.h>

#include "pspg.h"
#include "unicode.h"

#ifdef HAVE_POSTGRESQL

#include <libpq-fe.h>

#if PG_VERSION >= 11

#include "server/catalog/pg_type_d.h"

#else

#include "server/catalog/pg_types.h"

#endif

char errmsg[1024];

static RowBucketType *
push_row(RowBucketType *rb, RowType *row, bool is_multiline)
{
	if (rb->nrows >= 1000)
	{
		RowBucketType *new = malloc(sizeof(RowBucketType));

		if (!new)
			return NULL;

		new->nrows = 0;
		new->allocated = true;
		new->next_bucket = NULL;

		rb->next_bucket = new;
		rb = new;
	}

	rb->rows[rb->nrows] = row;
	rb->multilines[rb->nrows++] = is_multiline;

	return rb;
}

static char
column_type_class(Oid ftype)
{
	char		align;

	switch (ftype)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
		case OIDOID:
		case XIDOID:
		case CIDOID:
		case CASHOID:
			align = 'd';
			break;
		default:
			align = 'a';
			break;
	}
	return align;
}


#endif

#define EXIT_OUT_OF_MEMORY()		do { PQclear(result); PQfinish(conn); leave_ncurses("out of memory"); } while (0)
#define RELEASE_AND_LEAVE(s)		do { PQclear(result); PQfinish(conn); *err = s; return false; } while (0)
#define RELEASE_AND_EXIT(s)			do { PQclear(result); PQfinish(conn); leave_ncurses(s); } while (0)

static int
field_info(Options *opts, char *str, bool *multiline)
{
	long int	digits;
	long int	others;

	if (opts->force8bit)
	{
		int		cw = 0;
		int		width = 0;

		while (*str)
		{
			if (*str++ == '\n')
			{
				*multiline = true;
				width = cw > width ? cw : width;
				cw = 0;
			}
			else
				cw++;
		}

		return cw > width ? cw : width;
	}
	else
		return utf_string_dsplen_multiline(str, strlen(str), multiline, false, &digits, &others);
}

static int
max_int(int a, int b)
{
	return a > b ? a : b;
}

/*
 * exit on fatal error, or return error
 */
bool
pg_exec_query(Options *opts, RowBucketType *rb, PrintDataDesc *pdesc, const char **err)
{

#ifdef HAVE_POSTGRESQL

	PGconn	   *conn = NULL;
	PGresult   *result = NULL;

	int			nfields;
	int			size;
	int			i, j;
	char	   *locbuf;
	RowType	   *row;
	bool		multiline_row;
	bool		multiline_col;


	const char *keywords[3];
	const char *values[3];

	keywords[0] = "dbname"; values[0] = "postgres";
	keywords[1] = "host"; values[1] = "localhost";
	keywords[2] = NULL; values[2] = NULL;

	rb->nrows = 0;
	rb->next_bucket = NULL;

	conn = PQconnectdbParams(keywords, values, true);

	/* Check to see that the backend connection was successfully made */
	if (PQstatus(conn) != CONNECTION_OK)
	{
		sprintf(errmsg, "Connection to database failed: %s", PQerrorMessage(conn));
		RELEASE_AND_LEAVE(errmsg);
	}

	/*
	 * ToDo: Because data are copied to local memory, the result can be fetched.
	 * It can save 1/2 memory.
	 */
	result = PQexec(conn, opts->query);
	if (PQresultStatus(result) != PGRES_TUPLES_OK)
	{
		sprintf(errmsg, "Query doesn't return data: %s", PQerrorMessage(conn));
		RELEASE_AND_LEAVE(errmsg);
	}

	if ((nfields = PQnfields(result)) > 1024)
		RELEASE_AND_EXIT("too much columns");

	pdesc->nfields = nfields;
	pdesc->has_header = true;
	for (i = 0; i < nfields; i++)
		pdesc->types[i] = column_type_class(PQftype(result, i));

	/* calculate necessary size of header data */
	size = 0;
	for (i = 0; i < nfields; i++)
		size += strlen(PQfname(result, i)) + 1;

	locbuf = malloc(size);
	if (!locbuf)
		EXIT_OUT_OF_MEMORY();

	/* store header */
	row = malloc(offsetof(RowType, fields) + (nfields * sizeof(char *)));
	if (!row)
		EXIT_OUT_OF_MEMORY();

	row->nfields = nfields;

	multiline_row = false;
	for (i = 0; i < nfields; i++)
	{
		char   *name = PQfname(result, i);

		strcpy(locbuf, name);
		row->fields[i] = locbuf;
		locbuf += strlen(name) + 1;

		pdesc->widths[i] = field_info(opts, row->fields[i], &multiline_col);
		pdesc->multilines[i] = multiline_col;

		multiline_row |= multiline_col;
	}

	rb = push_row(rb, row, multiline_row);
	if (!rb)
		EXIT_OUT_OF_MEMORY();

	/* calculate size for any row and store it */
	for (i = 0; i < PQntuples(result); i++)
	{
		size = 0;
		for (j = 0; j < nfields; j++)
			size += strlen(PQgetvalue(result, i, j)) + 1;

		locbuf = malloc(size);
		if (!locbuf)
			EXIT_OUT_OF_MEMORY();

		/* store data */
		row = malloc(offsetof(RowType, fields) + (nfields * sizeof(char *)));
		if (!row)
			EXIT_OUT_OF_MEMORY();

		row->nfields = nfields;

		multiline_row = false;
		for (j = 0; j < nfields; j++)
		{
			char	*value = PQgetvalue(result, i, j);

			strcpy(locbuf, value);
			row->fields[j] = locbuf;
			locbuf += strlen(value) + 1;

			pdesc->widths[j] = max_int(pdesc->widths[j],
									  field_info(opts, row->fields[j], &multiline_col));
			pdesc->multilines[j] |= multiline_col;
			multiline_row |= multiline_col;
		}

		rb = push_row(rb, row, multiline_row);
		if (!rb)
			EXIT_OUT_OF_MEMORY();
	}

    PQclear(result);
    PQfinish(conn);

	*err = NULL;

	return true;

#else

	err = "Query cannot be executed. The Postgres library was not available at compile time."

	return false;

#endif

}
