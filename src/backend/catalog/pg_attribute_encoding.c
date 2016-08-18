/*
 * pg_attribute_encoding.c
 *
 * Routines to manipulation and retrieve column encoding information.
 *
 * Copyright (c) EMC, 2011
 */

#include "postgres.h"
#include "fmgr.h"

#include "access/reloptions.h"
#include "catalog/catquery.h"
#include "catalog/pg_attribute_encoding.h"
#include "catalog/pg_compression.h"
#include "catalog/dependency.h"
#include "parser/analyze.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/formatting.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

/*
 * Add a single attribute encoding entry.
 */
static void
add_attribute_encoding_entry(Oid relid, AttrNumber attnum, Datum attoptions)
{
	Datum values[Natts_pg_attribute_encoding];
	bool nulls[Natts_pg_attribute_encoding];
	HeapTuple tuple;
	cqContext	   *pcqCtx;
	
	Insist(attnum != InvalidAttrNumber);
	
	pcqCtx = caql_beginscan(
			NULL,
			cql("INSERT INTO pg_attribute_encoding",
				NULL));

	MemSet(nulls, 0, sizeof(nulls));
	values[Anum_pg_attribute_encoding_attrelid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_attribute_encoding_attnum - 1] = Int16GetDatum(attnum);
	values[Anum_pg_attribute_encoding_attoptions - 1] = attoptions;

	tuple = caql_form_tuple(pcqCtx, values, nulls);

	/* insert a new tuple */
	caql_insert(pcqCtx, tuple); /* implicit update of index as well */

	heap_freetuple(tuple);

	caql_endscan(pcqCtx);
}

/*
 * Get the set of functions implementing a compression algorithm.
 *
 * Intercept requests for "none", since that is not a real compression
 * implementation but a fake one to indicate no compression desired.
 */
PGFunction *
get_funcs_for_compression(char *compresstype)
{
	PGFunction *func = NULL;

	if (compresstype == NULL ||
		compresstype[0] == '\0' ||
		pg_strcasecmp("none", compresstype) == 0)
	{
		return func;
	}
	else
	{
		func = GetCompressionImplementation(compresstype);

		Insist(PointerIsValid(func));
	}
	return func;
}

/*
 * Get datum representations of the attoptions field in pg_attribute_encoding
 * for the given relation.
 */
Datum *
get_rel_attoptions(Oid relid, AttrNumber max_attno)
{
	Form_pg_attribute attform;
	HeapTuple		tuple;
	cqContext		cqc;
	cqContext	   *pcqCtx;
	Datum		   *dats;
	Relation 		pgae = heap_open(AttributeEncodingRelationId,
									 AccessShareLock);

	/* used for attbyval and len below */
	attform = pgae->rd_att->attrs[Anum_pg_attribute_encoding_attoptions - 1];

	dats = palloc0(max_attno * sizeof(Datum));

	pcqCtx = caql_beginscan(
			caql_addrel(cqclr(&cqc), pgae),
			cql("SELECT * FROM pg_attribute_encoding "
				" WHERE attrelid = :1 ",
				ObjectIdGetDatum(relid)));

	while (HeapTupleIsValid(tuple = caql_getnext(pcqCtx)))
	{
		Form_pg_attribute_encoding a = 
			(Form_pg_attribute_encoding)GETSTRUCT(tuple);
		int16 attnum = a->attnum;
		Datum attoptions;
		bool isnull;

		Insist(attnum > 0 && attnum <= max_attno);

		attoptions = heap_getattr(tuple, Anum_pg_attribute_encoding_attoptions,
								  RelationGetDescr(pgae), &isnull);
		Insist(!isnull);

		dats[attnum - 1] = datumCopy(attoptions,
									 attform->attbyval,
									 attform->attlen);
	}

	caql_endscan(pcqCtx);

	heap_close(pgae, AccessShareLock);

	return dats;

}

/*
 * Add pg_attribute_encoding entries for newrelid. Make them identical to those
 * stored for oldrelid.
 */
void
cloneAttributeEncoding(Oid oldrelid, Oid newrelid, AttrNumber max_attno)
{
	Datum *attoptions = get_rel_attoptions(oldrelid, max_attno);
	AttrNumber n;

	for (n = 0; n < max_attno; n++)
	{
		if (DatumGetPointer(attoptions[n]) != NULL)
			add_attribute_encoding_entry(newrelid,
										 n + 1,
										 attoptions[n]);
	}
	CommandCounterIncrement();
}

List **
RelationGetUntransformedAttributeOptions(Relation rel)
{
	List **l;
	int i;
	Datum *dats = get_rel_attoptions(RelationGetRelid(rel),
									 RelationGetNumberOfAttributes(rel));

	l = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(List *));

	for (i = 0; i < RelationGetNumberOfAttributes(rel); i++)
	{
		l[i] = untransformRelOptions(dats[i]);
	}

	return l;
}

/*
 * Get all storage options for all user attributes of the table.
 */
StdRdOptions **
RelationGetAttributeOptions(Relation rel)
{
	Datum *dats;
	StdRdOptions **opts;
	int i;

	opts = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(StdRdOptions *));

	dats = get_rel_attoptions(RelationGetRelid(rel),
							  RelationGetNumberOfAttributes(rel));

	for (i = 0; i < RelationGetNumberOfAttributes(rel); i++)
	{
		if (DatumGetPointer(dats[i]) != NULL)
		{
			opts[i] = (StdRdOptions *)heap_reloptions(
					RELKIND_RELATION, dats[i], false);
			pfree(DatumGetPointer(dats[i]));
		}
	}
	pfree(dats);

	return opts;
}

/*
 * Given a WITH(...) clause and no other column encoding directives -- such as
 * in the case of CREATE TABLE WITH () AS SELECT -- fill in the column encoding
 * catalog entries for that relation.
 */
void
AddDefaultRelationAttributeOptions(Relation rel, List *options)
{
	Datum opts;
	AttrNumber attno;
	List *ce;

	/* only supported on AOCO at this stage */
	if (!RelationIsAoCols(rel))
		return;

 	ce = form_default_storage_directive(options);
	if (!ce)
		ce = default_column_encoding_clause();

	ce = transformStorageEncodingClause(ce);

	opts = transformRelOptions(PointerGetDatum(NULL), ce, true, false);

	for (attno = 1; attno <= RelationGetNumberOfAttributes(rel); attno++)
		add_attribute_encoding_entry(RelationGetRelid(rel),
									 attno,
									 opts);
	CommandCounterIncrement();
}

/*
 * Work horse underneath DefineRelation().
 *
 * Simply adds user specified ENCODING () clause information to
 * pg_attribute_encoding. Should be absolutely valid at this point.
 */
void
AddRelationAttributeEncodings(Relation rel, List *attr_encodings)
{
	Oid relid = RelationGetRelid(rel);
	ListCell *lc;

	foreach(lc, attr_encodings)
	{
		Datum attoptions;
		ColumnReferenceStorageDirective *c = lfirst(lc);
		List *encoding;
		AttrNumber attnum;

		Insist(IsA(c, ColumnReferenceStorageDirective));

		attnum = get_attnum(relid, c->column);

		if (attnum == InvalidAttrNumber)
			elog(ERROR, "column \"%s\" does not exist", c->column);

		if (attnum < 0)
			elog(ERROR, "column \"%s\" is a system column", c->column);

		encoding = c->encoding;

		if (!encoding)
			continue;

		attoptions = transformRelOptions(PointerGetDatum(NULL),
										 encoding,
										 true,
										 false);

		add_attribute_encoding_entry(relid, attnum, attoptions);
	}
}

void
RemoveAttributeEncodingsByRelid(Oid relid)
{
	bool 		found = false;

	found = (0 != caql_getcount(
					 NULL,
					 cql("DELETE FROM pg_attribute_encoding "
						 " WHERE attrelid = :1 ",
						 ObjectIdGetDatum(relid))));
}
