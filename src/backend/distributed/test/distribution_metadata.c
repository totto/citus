/*-------------------------------------------------------------------------
 *
 * test/src/distribution_metadata.c
 *
 * This file contains functions to exercise distributed table metadata
 * functionality within Citus.
 *
 * Copyright (c) 2014-2015, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "c.h"
#include "fmgr.h"

#include <stddef.h>
#include <stdint.h>

#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "distributed/master_metadata_utility.h"
#include "distributed/master_protocol.h"
#include "distributed/metadata_cache.h"
#include "distributed/multi_join_order.h"
#include "distributed/pg_dist_shard.h"
#include "distributed/resource_lock.h"
#include "distributed/test_helper_functions.h" /* IWYU pragma: keep */
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "storage/lock.h"
#include "utils/array.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/builtins.h"
#include "utils/palloc.h"


/* declarations for dynamic loading */
PG_FUNCTION_INFO_V1(load_shard_id_array);
PG_FUNCTION_INFO_V1(load_shard_interval_array);
PG_FUNCTION_INFO_V1(load_shard_placement_array);
PG_FUNCTION_INFO_V1(partition_column_id);
PG_FUNCTION_INFO_V1(partition_type);
PG_FUNCTION_INFO_V1(is_distributed_table);
PG_FUNCTION_INFO_V1(column_name_to_column);
PG_FUNCTION_INFO_V1(column_name_to_column_id);
PG_FUNCTION_INFO_V1(create_monolithic_shard_row);
PG_FUNCTION_INFO_V1(create_healthy_local_shard_placement_row);
PG_FUNCTION_INFO_V1(delete_shard_placement_row);
PG_FUNCTION_INFO_V1(update_shard_placement_row_state);
PG_FUNCTION_INFO_V1(acquire_shared_shard_lock);


/*
 * load_shard_id_array returns the shard identifiers for a particular
 * distributed table as a bigint array. If the table is not distributed
 * yet, the function errors-out.
 */
Datum
load_shard_id_array(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	ArrayType *shardIdArrayType = NULL;
	ListCell *shardCell = NULL;
	int shardIdIndex = 0;
	Oid shardIdTypeId = INT8OID;

	int shardIdCount = -1;
	Datum *shardIdDatumArray = NULL;
	List *shardList = LoadShardIntervalList(distributedTableId);

	shardIdCount = list_length(shardList);
	shardIdDatumArray = palloc0(shardIdCount * sizeof(Datum));

	foreach(shardCell, shardList)
	{
		ShardInterval *shardId = (ShardInterval *) lfirst(shardCell);
		Datum shardIdDatum = Int64GetDatum(shardId->shardId);

		shardIdDatumArray[shardIdIndex] = shardIdDatum;
		shardIdIndex++;
	}

	shardIdArrayType = DatumArrayToArrayType(shardIdDatumArray, shardIdCount,
											 shardIdTypeId);

	PG_RETURN_ARRAYTYPE_P(shardIdArrayType);
}


/*
 * load_shard_interval_array loads a shard interval using a provided identifier
 * and returns a two-element array consisting of min/max values contained in
 * that shard interval. If no such interval can be found, this function raises
 * an error instead.
 */
Datum
load_shard_interval_array(PG_FUNCTION_ARGS)
{
	int64 shardId = PG_GETARG_INT64(0);
	Oid expectedType PG_USED_FOR_ASSERTS_ONLY = get_fn_expr_argtype(fcinfo->flinfo, 1);
	ShardInterval *shardInterval = LoadShardInterval(shardId);
	Datum shardIntervalArray[] = { shardInterval->minValue, shardInterval->maxValue };
	ArrayType *shardIntervalArrayType = NULL;

	Assert(expectedType == shardInterval->valueTypeId);

	shardIntervalArrayType = DatumArrayToArrayType(shardIntervalArray, 2,
												   shardInterval->valueTypeId);

	PG_RETURN_ARRAYTYPE_P(shardIntervalArrayType);
}


/*
 * load_shard_placement_array loads a shard interval using the provided ID
 * and returns an array of strings containing the node name and port for each
 * placement of the specified shard interval. If the second argument is true,
 * only finalized placements are returned; otherwise, all are. If no such shard
 * interval can be found, this function raises an error instead.
 */
Datum
load_shard_placement_array(PG_FUNCTION_ARGS)
{
	int64 shardId = PG_GETARG_INT64(0);
	bool onlyFinalized = PG_GETARG_BOOL(1);
	ArrayType *placementArrayType = NULL;
	List *placementList = NIL;
	ListCell *placementCell = NULL;
	int placementCount = -1;
	int placementIndex = 0;
	Datum *placementDatumArray = NULL;
	Oid placementTypeId = TEXTOID;
	StringInfo placementInfo = makeStringInfo();

	if (onlyFinalized)
	{
		placementList = FinalizedShardPlacementList(shardId);
	}
	else
	{
		placementList = ShardPlacementList(shardId);
	}

	placementCount = list_length(placementList);
	placementDatumArray = palloc0(placementCount * sizeof(Datum));

	foreach(placementCell, placementList)
	{
		ShardPlacement *placement = (ShardPlacement *) lfirst(placementCell);
		appendStringInfo(placementInfo, "%s:%d", placement->nodeName,
						 placement->nodePort);

		placementDatumArray[placementIndex] = CStringGetTextDatum(placementInfo->data);
		placementIndex++;
		resetStringInfo(placementInfo);
	}

	placementArrayType = DatumArrayToArrayType(placementDatumArray, placementCount,
											   placementTypeId);

	PG_RETURN_ARRAYTYPE_P(placementArrayType);
}


/*
 * partition_column_id simply finds a distributed table using the provided Oid
 * and returns the column_id of its partition column. If the specified table is
 * not distributed, this function raises an error instead.
 */
Datum
partition_column_id(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	uint32 rangeTableId = 1;
	Var *partitionColumn = PartitionColumn(distributedTableId, rangeTableId);

	PG_RETURN_INT16((int16) partitionColumn->varattno);
}


/*
 * partition_type simply finds a distributed table using the provided Oid and
 * returns the type of partitioning in use by that table. If the specified
 * table is not distributed, this function raises an error instead.
 */
Datum
partition_type(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	char partitionType = PartitionMethod(distributedTableId);

	PG_RETURN_CHAR(partitionType);
}


/*
 * is_distributed_table simply returns whether a given table is distributed. No
 * errors, just a boolean.
 */
Datum
is_distributed_table(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	bool isDistributedTable = IsDistributedTable(distributedTableId);

	PG_RETURN_BOOL(isDistributedTable);
}


/*
 * column_name_to_column is an internal UDF to obtain a textual representation
 * of a particular column node (Var), given a relation identifier and column
 * name. There is no requirement that the table be distributed; this function
 * simply returns the textual representation of a Var representing a column.
 * This function will raise an ERROR if no such column can be found or if the
 * provided name refers to a system column.
 */
Datum
column_name_to_column(PG_FUNCTION_ARGS)
{
	Oid relationId = PG_GETARG_OID(0);
	text *columnText = PG_GETARG_TEXT_P(1);
	Relation relation = NULL;
	char *columnName = text_to_cstring(columnText);
	Var *column = NULL;
	char *columnNodeString = NULL;
	text *columnNodeText = NULL;

	relation = relation_open(relationId, AccessExclusiveLock);

	column = (Var *) BuildDistributionKeyFromColumnName(relation, columnName);
	columnNodeString = nodeToString(column);
	columnNodeText = cstring_to_text(columnNodeString);

	relation_close(relation, NoLock);

	PG_RETURN_TEXT_P(columnNodeText);
}


/*
 * column_name_to_column_id takes a relation identifier and a name of a column
 * in that relation and returns the index of that column in the relation. If
 * the provided name is a system column or no column at all, this function will
 * throw an error instead.
 */
Datum
column_name_to_column_id(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	char *columnName = PG_GETARG_CSTRING(1);
	Relation relation = NULL;
	Var *column = NULL;

	relation = relation_open(distributedTableId, AccessExclusiveLock);

	column = (Var *) BuildDistributionKeyFromColumnName(relation, columnName);

	relation_close(relation, NoLock);

	PG_RETURN_INT16((int16) column->varattno);
}


/*
 * create_monolithic_shard_row creates a single shard covering all possible
 * hash values for a given table and inserts a row representing that shard
 * into the backing store. It returns the primary key of the new row.
 */
Datum
create_monolithic_shard_row(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	StringInfo minInfo = makeStringInfo();
	StringInfo maxInfo = makeStringInfo();
	Datum newShardIdDatum = master_get_new_shardid(NULL);
	int64 newShardId = DatumGetInt64(newShardIdDatum);
	text *maxInfoText = NULL;
	text *minInfoText = NULL;

	appendStringInfo(minInfo, "%d", INT32_MIN);
	appendStringInfo(maxInfo, "%d", INT32_MAX);

	minInfoText = cstring_to_text(minInfo->data);
	maxInfoText = cstring_to_text(maxInfo->data);

	InsertShardRow(distributedTableId, newShardId, SHARD_STORAGE_TABLE, minInfoText,
				   maxInfoText);

	PG_RETURN_INT64(newShardId);
}


/*
 * create_healthy_local_shard_placement_row inserts a row representing a
 * finalized placement for localhost (on the default port) into the backing
 * store.
 */
Datum
create_healthy_local_shard_placement_row(PG_FUNCTION_ARGS)
{
	int64 shardId = PG_GETARG_INT64(0);
	int64 shardLength = 0;

	InsertShardPlacementRow(shardId, FILE_FINALIZED, shardLength, "localhost", 5432);

	PG_RETURN_VOID();
}


/*
 * delete_shard_placement_row removes a shard placement with the specified ID.
 */
Datum
delete_shard_placement_row(PG_FUNCTION_ARGS)
{
	int64 shardId = PG_GETARG_INT64(0);
	text *hostName = PG_GETARG_TEXT_P(1);
	int64 hostPort = PG_GETARG_INT64(2);
	bool successful = true;
	char *hostNameString = text_to_cstring(hostName);

	DeleteShardPlacementRow(shardId, hostNameString, hostPort);

	PG_RETURN_BOOL(successful);
}


/*
 * update_shard_placement_row_state sets the state of the placement with the
 * specified ID.
 */
Datum
update_shard_placement_row_state(PG_FUNCTION_ARGS)
{
	int64 shardId = PG_GETARG_INT64(0);
	text *hostName = PG_GETARG_TEXT_P(1);
	int64 hostPort = PG_GETARG_INT64(2);
	RelayFileState shardState = (RelayFileState) PG_GETARG_INT32(3);
	bool successful = true;
	char *hostNameString = text_to_cstring(hostName);
	uint64 shardLength = 0;

	DeleteShardPlacementRow(shardId, hostNameString, hostPort);
	InsertShardPlacementRow(shardId, shardState, shardLength, hostNameString, hostPort);

	PG_RETURN_BOOL(successful);
}


/*
 * acquire_shared_shard_lock grabs a shared lock for the specified shard.
 */
Datum
acquire_shared_shard_lock(PG_FUNCTION_ARGS)
{
	int64 shardId = PG_GETARG_INT64(0);

	LockShardResource(shardId, ShareLock);

	PG_RETURN_VOID();
}
