/* -------------------------------------------------------------------------
 *
 * tablecmds.cpp
 *	  Commands for creating and altering table structures and settings
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/optimizer/commands/tablecmds.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"
#include "access/cstore_delete.h"
#include "access/cstore_insert.h"
#include "access/cstore_rewrite.h"
#include "access/dfs/dfs_am.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "access/tuptoaster.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/tableam.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/dfsstore_ctlg.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_inherits_fn.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_object.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_partition.h"
#include "catalog/pg_partition_fn.h"
#include "catalog/pg_hashbucket.h"
#include "catalog/pg_hashbucket_fn.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/pg_type_fn.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "catalog/toasting.h"
#include "catalog/cstore_ctlg.h"
#include "catalog/storage_gtt.h"
#include "commands/cluster.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "gssignal/gs_signal.h"
#include "gtm/gtm_client.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/clauses.h"
#include "optimizer/planner.h"
#include "optimizer/var.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"
#include "parser/parser.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteRlsPolicy.h"
#include "replication/slot.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/predicate.h"
#include "storage/remote_read.h"
#include "storage/smgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/aiomem.h"
#include "utils/builtins.h"
#include "utils/extended_statistics.h"
#include "utils/fmgroids.h"
#include "utils/int8.h"
#include "utils/inval.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/partitionmap.h"
#include "utils/partitionmap_gs.h"
#include "utils/partitionkey.h"
#include "utils/relcache.h"
#include "utils/sec_rls_utils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "utils/typcache.h"
#include "utils/numeric.h"
#include "catalog/pg_database.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_user_status.h"
#include "gaussdb_version.h"
#include "workload/workload.h"
#include "utils/builtins.h"
#include "fmgr.h"
#include "pgstat.h"

#ifdef PGXC
#include "pgxc/pgxc.h"
#include "access/gtm.h"
#include "catalog/pgxc_class.h"
#include "catalog/pgxc_node.h"
#include "commands/sequence.h"
#include "optimizer/pgxcship.h"
#include "pgxc/execRemote.h"
#include "pgxc/redistrib.h"
#include "pgxc/groupmgr.h"
#endif
#include "dfs_adaptor.h"
#include "c.h"

enum {
    STATISTIC_WITHOUT_PERCENT_THRESHOLD = 10000,
    STATISTIC_WITH_PERCENT_THRESHOLD = 100
};

/* only for fastcheck debug */
static const bool force_createbucket = false;

extern void vacuum_set_xid_limits(Relation rel, int64 freeze_min_age, int64 freeze_table_age, TransactionId* oldestXmin,
    TransactionId* freezeLimit, TransactionId* freezeTableLimit);

/* ON COMMIT action list */
struct OnCommitItem {
    Oid relid;               /* relid of relation */
    OnCommitAction oncommit; /* what to do at end of xact */
    /*
     * If this entry was created during the current transaction,
     * creating_subid is the ID of the creating subxact; if created in a prior
     * transaction, creating_subid is zero.  If deleted during the current
     * transaction, deleting_subid is the ID of the deleting subxact; if no
     * deletion request is pending, deleting_subid is zero.
     */
    SubTransactionId creating_subid;
    SubTransactionId deleting_subid;
};

static const char* ORCSupportOption[] = {"orientation", "compression", "version", "partial_cluster_rows"};

/*
 * State information for ALTER TABLE
 *
 * The pending-work queue for an ALTER TABLE is a List of AlteredTableInfo
 * structs, one for each table modified by the operation (the named table
 * plus any child tables that are affected).  We save lists of subcommands
 * to apply to this table (possibly modified by parse transformation steps);
 * these lists will be executed in Phase 2.  If a Phase 3 step is needed,
 * necessary information is stored in the constraints and newvals lists.
 *
 * Phase 2 is divided into multiple passes; subcommands are executed in
 * a pass determined by subcommand type.
 */
#define AT_PASS_DROP 0       /* DROP (all flavors) */
#define AT_PASS_ALTER_TYPE 1 /* ALTER COLUMN TYPE */
#define AT_PASS_OLD_INDEX 2  /* re-add existing indexes */
#define AT_PASS_OLD_CONSTR 3 /* re-add existing constraints */
#define AT_PASS_COL_ATTRS 4  /* set other column attributes */
/* We could support a RENAME COLUMN pass here, but not currently used */
#define AT_PASS_ADD_COL 5    /* ADD COLUMN */
#define AT_PASS_ADD_INDEX 6  /* ADD indexes */
#define AT_PASS_ADD_CONSTR 7 /* ADD constraints, defaults */
#define AT_PASS_ADD_PARTITION 8
#define AT_PASS_MISC 9 /* other stuff */
#ifdef PGXC
#define AT_PASS_DISTRIB 10 /* Redistribution pass */
#define AT_NUM_PASSES 11
#else
#define AT_NUM_PASSES 10
#endif

struct AlteredTableInfo {
    /* Information saved before any work commences: */
    Oid relid;         /* Relation to work on */
    Oid partid;        /* Partition to work on */
    char relkind;      /* Its relkind */
    TupleDesc oldDesc; /* Pre-modification tuple descriptor */
    /* Information saved by Phase 1 for Phase 2: */
    List* subcmds[AT_NUM_PASSES]; /* Lists of AlterTableCmd */
    /* Information saved by Phases 1/2 for Phase 3: */
    List* constraints; /* List of NewConstraint */
    List* newvals;     /* List of NewColumnValue */
    bool new_notnull;  /* T if we added new NOT NULL constraints */
    bool rewrite;      /* T if a rewrite is forced */
    Oid newTableSpace; /* new tablespace; 0 means no change */
    /* Objects to rebuild after completing ALTER TYPE operations */
    List* changedConstraintOids; /* OIDs of constraints to rebuild */
    List* changedConstraintDefs; /* string definitions of same */
    List* changedIndexOids;      /* OIDs of indexes to rebuild */
    List* changedIndexDefs;      /* string definitions of same */
};

/* Struct describing one new constraint to check in Phase 3 scan */
/* Note: new NOT NULL constraints are handled elsewhere */
struct NewConstraint {
    char* name;         /* Constraint name, or NULL if none */
    ConstrType contype; /* CHECK or FOREIGN */
    Oid refrelid;       /* PK rel, if FOREIGN */
    Oid refindid;       /* OID of PK's index, if FOREIGN */
    Oid conid;          /* OID of pg_constraint entry, if FOREIGN */
    Node* qual;         /* Check expr or CONSTR_FOREIGN Constraint */
    List* qualstate;    /* Execution state for CHECK */
};

/*
 * Struct describing one new column value that needs to be computed during
 * Phase 3 copy (this could be either a new column with a non-null default, or
 * a column that we're changing the type of).  Columns without such an entry
 * are just copied from the old table during ATRewriteTable.  Note that the
 * expr is an expression over *old* table values.
 */
struct NewColumnValue {
    AttrNumber attnum;    /* which column */
    Expr* expr;           /* expression to compute */
    ExprState* exprstate; /* execution state */
};

/*
 * Error-reporting support for RemoveRelations
 */
struct dropmsgstrings {
    char kind;
    int nonexistent_code;
    const char* nonexistent_msg;
    const char* skipping_msg;
    const char* nota_msg;
    const char* drophint_msg;
};

static const struct dropmsgstrings dropmsgstringarray[] = {
    {RELKIND_RELATION,
        ERRCODE_UNDEFINED_TABLE,
        gettext_noop("table \"%s\" does not exist"),
        gettext_noop("table \"%s\" does not exist, skipping"),
        gettext_noop("\"%s\" is not a table"),
        gettext_noop("Use DROP TABLE to remove a table.")},
    {RELKIND_SEQUENCE,
        ERRCODE_UNDEFINED_TABLE,
        gettext_noop("sequence \"%s\" does not exist"),
        gettext_noop("sequence \"%s\" does not exist, skipping"),
        gettext_noop("\"%s\" is not a sequence"),
        gettext_noop("Use DROP SEQUENCE to remove a sequence.")},
    {RELKIND_VIEW,
        ERRCODE_UNDEFINED_TABLE,
        gettext_noop("view \"%s\" does not exist"),
        gettext_noop("view \"%s\" does not exist, skipping"),
        gettext_noop("\"%s\" is not a view"),
        gettext_noop("Use DROP VIEW to remove a view.")},
    {RELKIND_INDEX,
        ERRCODE_UNDEFINED_OBJECT,
        gettext_noop("index \"%s\" does not exist"),
        gettext_noop("index \"%s\" does not exist, skipping"),
        gettext_noop("\"%s\" is not an index"),
        gettext_noop("Use DROP INDEX to remove an index.")},
    {RELKIND_COMPOSITE_TYPE,
        ERRCODE_UNDEFINED_OBJECT,
        gettext_noop("type \"%s\" does not exist"),
        gettext_noop("type \"%s\" does not exist, skipping"),
        gettext_noop("\"%s\" is not a type"),
        gettext_noop("Use DROP TYPE to remove a type.")},
    {RELKIND_FOREIGN_TABLE,
        ERRCODE_UNDEFINED_OBJECT,
        gettext_noop("foreign table \"%s\" does not exist"),
        gettext_noop("foreign table \"%s\" does not exist, skipping"),
        gettext_noop("\"%s\" is not a foreign table"),
        gettext_noop("Use DROP FOREIGN TABLE to remove a foreign table.")},
    {'\0', 0, NULL, NULL, NULL, NULL}};

struct DropRelationCallbackState {
    char relkind;
    Oid heapOid;
    bool concurrent;
};

// When merge partitions, if toast tables have repeat chunkid, replace it.
//
struct ChunkIdHashKey {
    Oid toastTableOid;
    Oid oldChunkId;
};

// Entry structures for the hash tables
//
struct OldToNewChunkIdMappingData {
    ChunkIdHashKey key;
    Oid newChunkId;
};

typedef OldToNewChunkIdMappingData* OldToNewChunkIdMapping;

/* Alter table target-type flags for ATSimplePermissions */
#define ATT_NULL 0x0000
#define ATT_TABLE 0x0001
#define ATT_VIEW 0x0002
#define ATT_INDEX 0x0004
#define ATT_COMPOSITE_TYPE 0x0008
#define ATT_FOREIGN_TABLE 0x0010
#define ATT_SEQUENCE 0x0020

#define CSTORE_SUPPORT_AT_CMD(cmd)                                                                                  \
    ((cmd) == AT_AddPartition || (cmd) == AT_ExchangePartition || (cmd) == AT_TruncatePartition ||                  \
        (cmd) == AT_DropPartition || (cmd) == AT_AddConstraint || (cmd) == AT_DropConstraint ||                     \
        (cmd) == AT_AddNodeList || (cmd) == AT_DeleteNodeList || (cmd) == AT_SubCluster || (cmd) == AT_AddColumn || \
        (cmd) == AT_DropColumn || (cmd) == AT_AlterColumnType || (cmd) == AT_ColumnDefault ||                       \
        (cmd) == AT_SetStatistics || (cmd) == AT_AddStatistics || (cmd) == AT_DeleteStatistics ||                   \
        (cmd) == AT_SetTableSpace || (cmd) == AT_SetPartitionTableSpace || (cmd) == AT_SetOptions ||                \
        (cmd) == AT_ResetOptions || (cmd) == AT_SetStorage || (cmd) == AT_SetRelOptions ||                          \
        (cmd) == AT_ResetRelOptions || (cmd) == AT_MergePartition || (cmd) == AT_ChangeOwner ||                     \
        (cmd) == AT_EnableRls || (cmd) == AT_DisableRls || (cmd) == AT_ForceRls || (cmd) == AT_NoForceRls)

#define DFS_SUPPORT_AT_CMD(cmd)                                                                              \
    ((cmd) == AT_AddNodeList || (cmd) == AT_SubCluster || (cmd) == AT_AddColumn || (cmd) == AT_DropColumn || \
        (cmd) == AT_AddStatistics || (cmd) == AT_DeleteStatistics || (cmd) == AT_AddConstraint ||            \
        (cmd) == AT_DropConstraint || (cmd) == AT_ColumnDefault || (cmd) == AT_ChangeOwner)

#define HDFS_TBLSPC_SUPPORT_CREATE_LOGIC_OBJECT(kind)                                           \
    ((kind) == RELKIND_VIEW || (kind) == RELKIND_FOREIGN_TABLE || (kind) == RELKIND_SEQUENCE || \
        (kind) == RELKIND_COMPOSITE_TYPE)
#define MAX_SQL_LEN (2 * NAMEDATALEN + 128)

static void truncate_check_rel(Relation rel);
static List* MergeAttributes(
    List* schema, List* supers, char relpersistence, List** supOids, List** supconstr, int* supOidCount);
static bool MergeCheckConstraint(List* constraints, char* name, Node* expr);
static void MergeAttributesIntoExisting(Relation child_rel, Relation parent_rel);
static void MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel);
static void StoreCatalogInheritance(Oid relationId, List* supers);
static void StoreCatalogInheritance1(Oid relationId, Oid parentOid, int16 seqNumber, Relation inhRelation);
static int findAttrByName(const char* attributeName, List* schema);
static void AlterIndexNamespaces(
    Relation classRel, Relation rel, Oid oldNspOid, Oid newNspOid, ObjectAddresses* objsMoved);
static void AlterSeqNamespaces(
    Relation classRel, Relation rel, Oid oldNspOid, Oid newNspOid, ObjectAddresses* objsMoved, LOCKMODE lockmode);
static void ATExecValidateConstraint(Relation rel, char* constrName, bool recurse, bool recursing, LOCKMODE lockmode);
static int transformColumnNameList(Oid relId, List* colList, int16* attnums, Oid* atttypids);
static int transformFkeyGetPrimaryKey(
    Relation pkrel, Oid* indexOid, List** attnamelist, int16* attnums, Oid* atttypids, Oid* opclasses);
static Oid transformFkeyCheckAttrs(Relation pkrel, int numattrs, int16* attnums, Oid* opclasses);
static void checkFkeyPermissions(Relation rel, int16* attnums, int natts);
static CoercionPathType findFkeyCast(Oid targetTypeId, Oid sourceTypeId, Oid* funcid);
static void validateCheckConstraint(Relation rel, HeapTuple constrtup);
static void validateCheckConstraintForBucket(Relation rel, Partition part, HeapTuple constrtup);
static void validateForeignKeyConstraint(char* conname, Relation rel, Relation pkrel, Oid pkindOid, Oid constraintOid);
static void createForeignKeyTriggers(
    Relation rel, Oid refRelOid, Constraint* fkconstraint, Oid constraintOid, Oid indexOid);
static void ATController(Relation rel, List* cmds, bool recurse, LOCKMODE lockmode);
static void ATPrepCmd(List** wqueue, Relation rel, AlterTableCmd* cmd, bool recurse, bool recursing, LOCKMODE lockmode);
static void ATRewriteCatalogs(List** wqueue, LOCKMODE lockmode);
static void ATExecCmd(List** wqueue, AlteredTableInfo* tab, Relation rel, AlterTableCmd* cmd, LOCKMODE lockmode);
static void ATRewriteTables(List** wqueue, LOCKMODE lockmode);

static void ATRewriteTable(AlteredTableInfo* tab, Relation oldrel, Relation newrel);
static void ATCStoreRewriteTable(AlteredTableInfo* tab, Relation heapRel, LOCKMODE lockMode, Oid targetTblspc);
static void ATCStoreRewritePartition(AlteredTableInfo* tab, LOCKMODE lockMode);
static void at_timeseries_check(Relation rel, AlterTableCmd* cmd);

static void PSortChangeTableSpace(Oid psortOid, Oid newTableSpace, LOCKMODE lockmode);
static void ForbidToRewriteOrTestCstoreIndex(AlteredTableInfo* tab);
static inline void ChangeTableSpaceForDeltaRelation(Oid deltaOid, Oid targetTableSpace, LOCKMODE lockmode);
static inline void ChangeTableSpaceForCudescRelation(
    Oid cudescIdxOid, Oid cudescOid, Oid targetTableSpace, LOCKMODE lockmode);

static void ExecRewriteRowTable(AlteredTableInfo*, Oid, LOCKMODE);
static void ExecRewriteRowPartitionedTable(AlteredTableInfo*, Oid, LOCKMODE);
static void ExecRewriteCStoreTable(AlteredTableInfo*, Oid, LOCKMODE);
static void ExecRewriteCStorePartitionedTable(AlteredTableInfo*, Oid, LOCKMODE);
static void ExecOnlyTestRowTable(AlteredTableInfo*);
static void ExecOnlyTestRowPartitionedTable(AlteredTableInfo*);
/**
 * @Description: Only check the validity of existing data, because of some altering operators.
 * For example, the query "alter table ... add column col data type not null" contains
 * "NOT NULL" constraint, if the relation has no data, the query will be executed successfully,
 * otherwise get a fail result.
 * @in tab, The AlteredTableInfo struct.
 * @return None.
 */
static void exec_only_test_dfs_table(AlteredTableInfo* tab);
static void ExecOnlyTestCStoreTable(AlteredTableInfo*);
static void ExecOnlyTestCStorePartitionedTable(AlteredTableInfo*);
static void ExecChangeTableSpaceForRowTable(AlteredTableInfo*, LOCKMODE);
static void ExecChangeTableSpaceForRowPartition(AlteredTableInfo*, LOCKMODE);
static void ExecChangeTableSpaceForCStoreTable(AlteredTableInfo*, LOCKMODE);
static void ExecChangeTableSpaceForCStorePartition(AlteredTableInfo*, LOCKMODE);

static AlteredTableInfo* ATGetQueueEntry(List** wqueue, Relation rel);
static void ATSimplePermissions(Relation rel, int allowed_targets);
static void ATWrongRelkindError(Relation rel, int allowed_targets);
static void ATSimpleRecursion(List** wqueue, Relation rel, AlterTableCmd* cmd, bool recurse, LOCKMODE lockmode);
static void ATTypedTableRecursion(List** wqueue, Relation rel, AlterTableCmd* cmd, LOCKMODE lockmode);
static List* find_typed_table_dependencies(Oid typeOid, const char* typname, DropBehavior behavior);
static void ATPrepAddColumn(
    List** wqueue, Relation rel, bool recurse, bool recursing, AlterTableCmd* cmd, LOCKMODE lockmode);
static void ATExecAddColumn(List** wqueue, AlteredTableInfo* tab, Relation rel, ColumnDef* colDef, bool isOid,
    bool recurse, bool recursing, LOCKMODE lockmode);
static void check_for_column_name_collision(Relation rel, const char* colname);
static void add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid);
static void add_column_collation_dependency(Oid relid, int32 attnum, Oid collid);
static void ATPrepAddOids(List** wqueue, Relation rel, bool recurse, AlterTableCmd* cmd, LOCKMODE lockmode);
static void ATExecDropNotNull(Relation rel, const char* colName, LOCKMODE lockmode);
static void ATExecSetNotNull(AlteredTableInfo* tab, Relation rel, const char* colName, LOCKMODE lockmode);
static void ATExecColumnDefault(Relation rel, const char* colName, Node* newDefault, LOCKMODE lockmode);
static void ATPrepSetStatistics(Relation rel, const char* colName, Node* newValue, LOCKMODE lockmode);
static void ATExecSetStatistics(
    Relation rel, const char* colName, Node* newValue, AlterTableStatProperty additional_property, LOCKMODE lockmode);
static void ATExecAddStatistics(Relation rel, Node* def, LOCKMODE lockmode);
static void ATExecDeleteStatistics(Relation rel, Node* def, LOCKMODE lockmode);
static void ATExecSetOptions(Relation rel, const char* colName, Node* options, bool isReset, LOCKMODE lockmode);
static void ATExecSetStorage(Relation rel, const char* colName, Node* newValue, LOCKMODE lockmode);
static void ATPrepCheckDefault(Node* node);
static bool CheckLastColumn(Relation rel, AttrNumber attrnum);
static void ATPrepDropColumn(
    List** wqueue, Relation rel, bool recurse, bool recursing, AlterTableCmd* cmd, LOCKMODE lockmode);
static void ATExecDropColumn(List** wqueue, Relation rel, const char* colName, DropBehavior behavior, bool recurse,
    bool recursing, bool missing_ok, LOCKMODE lockmode);
static void ATExecAddIndex(AlteredTableInfo* tab, Relation rel, IndexStmt* stmt, bool is_rebuild, LOCKMODE lockmode);
static void ATExecAddConstraint(List** wqueue, AlteredTableInfo* tab, Relation rel, Constraint* newConstraint,
    bool recurse, bool is_readd, LOCKMODE lockmode);
static void ATExecAddIndexConstraint(AlteredTableInfo* tab, Relation rel, IndexStmt* stmt, LOCKMODE lockmode);
static void ATAddCheckConstraint(List** wqueue, AlteredTableInfo* tab, Relation rel, Constraint* constr, bool recurse,
    bool recursing, bool is_readd, LOCKMODE lockmode);
static void ATAddForeignKeyConstraint(AlteredTableInfo* tab, Relation rel, Constraint* fkconstraint, LOCKMODE lockmode);
static void ATExecDropConstraint(Relation rel, const char* constrName, DropBehavior behavior, bool recurse,
    bool recursing, bool missing_ok, LOCKMODE lockmode);
static void ATPrepAlterColumnType(List** wqueue, AlteredTableInfo* tab, Relation rel, bool recurse, bool recursing,
    AlterTableCmd* cmd, LOCKMODE lockmode);
static bool ATColumnChangeRequiresRewrite(Node* expr, AttrNumber varattno);
static void ATExecAlterColumnType(AlteredTableInfo* tab, Relation rel, AlterTableCmd* cmd, LOCKMODE lockmode);
static void ATExecAlterColumnGenericOptions(Relation rel, const char* colName, List* options, LOCKMODE lockmode);
static void ATPostAlterTypeCleanup(List** wqueue, AlteredTableInfo* tab, LOCKMODE lockmode);
static void ATPostAlterTypeParse(
    Oid oldId, Oid oldRelId, Oid refRelId, const char* cmd, List** wqueue, LOCKMODE lockmode, bool rewrite);
void TryReuseIndex(Oid oldId, IndexStmt* stmt);

void tryReusePartedIndex(Oid oldId, IndexStmt* stmt, Relation rel);

static void TryReuseForeignKey(Oid oldId, Constraint* con);
static void change_owner_fix_column_acls(Oid relationOid, Oid oldOwnerId, Oid newOwnerId);
static void change_owner_recurse_to_sequences(Oid relationOid, Oid newOwnerId, LOCKMODE lockmode);
static void ATExecClusterOn(Relation rel, const char* indexName, LOCKMODE lockmode);
static void ATExecDropCluster(Relation rel, LOCKMODE lockmode);
static void ATPrepSetTableSpace(AlteredTableInfo* tab, Relation rel, const char* tablespacename, LOCKMODE lockmode);
static void ATExecSetTableSpace(Oid tableOid, Oid newTableSpace, LOCKMODE lockmode);
static void ATExecSetRelOptionsToast(Oid toastid, List* defList, AlterTableType operation, LOCKMODE lockmode);
static void ATExecSetTableSpaceForPartitionP2(AlteredTableInfo* tab, Relation rel, Node* partition);
static void ATExecSetTableSpaceForPartitionP3(Oid tableOid, Oid partOid, Oid newTableSpace, LOCKMODE lockmode);
static void atexecset_table_space(Relation rel, Oid newTableSpace, Oid newrelfilenode);
static void ATExecSetRelOptions(Relation rel, List* defList, AlterTableType operation, LOCKMODE lockmode);
static void ATExecEnableDisableTrigger(
    Relation rel, const char* trigname, char fires_when, bool skip_system, LOCKMODE lockmode);
static void ATExecEnableDisableRule(Relation rel, const char* rulename, char fires_when, LOCKMODE lockmode);
static void ATPrepAddInherit(Relation child_rel);
static void ATExecAddInherit(Relation child_rel, RangeVar* parent, LOCKMODE lockmode);
static void ATExecDropInherit(Relation rel, RangeVar* parent, LOCKMODE lockmode);
static void drop_parent_dependency(Oid relid, Oid refclassid, Oid refobjid);
static void ATExecAddOf(Relation rel, const TypeName* ofTypename, LOCKMODE lockmode);
static void ATExecDropOf(Relation rel, LOCKMODE lockmode);
static void ATExecReplicaIdentity(Relation rel, ReplicaIdentityStmt* stmt, LOCKMODE lockmode);
static void ATExecGenericOptions(Relation rel, List* options);
static void ATExecSetCompress(Relation rel, const char* cmprsId);
#ifdef PGXC
static void AtExecDistributeBy(Relation rel, DistributeBy* options);
static void AtExecSubCluster(Relation rel, PGXCSubCluster* options);
static void AtExecAddNode(Relation rel, List* options);
static void AtExecDeleteNode(Relation rel, List* options);
static void ATCheckCmd(Relation rel, AlterTableCmd* cmd);
static RedistribState* BuildRedistribCommands(Oid relid, List* subCmds);
static Oid* delete_node_list(Oid* old_oids, int old_num, Oid* del_oids, int del_num, int* new_num);
static Oid* add_node_list(Oid* old_oids, int old_num, Oid* add_oids, int add_num, int* new_num);
#endif

static void copy_relation_data(Relation rel, SMgrRelation* dstptr, ForkNumber forkNum, char relpersistence);
static void mergeHeapBlock(Relation src, Relation dest, ForkNumber forkNum, char relpersistence, BlockNumber srcBlocks,
    BlockNumber destBlocks, TupleDesc srcTupleDesc, Oid srcToastOid, Oid destToastOid, HTAB* chunkIdHashTable,
    bool destHasFSM);
static void mergeVMBlock(Relation src, Relation dest, BlockNumber srcHeapBlocks, BlockNumber destHeapBloks);
static const char* storage_name(char c);

static void RangeVarCallbackForDropRelation(
    const RangeVar* rel, Oid relOid, Oid oldRelOid, bool target_is_partition, void* arg);
static void RangeVarCallbackForAlterRelation(
    const RangeVar* rv, Oid relid, Oid oldrelid, bool target_is_partition, void* arg);

static bool CheckRangePartitionKeyType(Oid typoid);
static void CheckRangePartitionKeyType(Form_pg_attribute* attrs, List* pos);
static void CheckIntervalPartitionKeyType(Form_pg_attribute* attrs, List* pos);
static void CheckPartitionTablespace(const char* spcname, Oid owner);
static void ComparePartitionValue(List* pos, Form_pg_attribute* attrs, PartitionState* partTableState);
static bool ConfirmTypeInfo(Oid* target_oid, int* target_mod, Const* src, Form_pg_attribute attrs, bool isinterval);

void addToastTableForNewPartition(Relation relation, Oid newPartId);
static void ATPrepAddPartition(Relation rel);
static void ATPrepDropPartition(Relation rel);
static void ATPrepUnusableIndexPartition(Relation rel);
static void ATPrepUnusableAllIndexOnPartition(Relation rel);
static void ATExecAddPartition(Relation rel, AddPartitionState* partState);
static void ATExecDropPartition(Relation rel, AlterTableCmd* cmd);
static void ATExecUnusableIndexPartition(Relation rel, const char* partition_name);
static void ATExecUnusableIndex(Relation rel);
static void ATExecUnusableAllIndexOnPartition(Relation rel, const char* partition_name);
static void ATExecModifyRowMovement(Relation rel, bool rowMovement);
static void ATExecTruncatePartition(Relation rel, AlterTableCmd* cmd);
static void checkColStoreForExchange(Relation partTableRel, Relation ordTableRel);
static void ATExecExchangePartition(Relation partTableRel, AlterTableCmd* cmd);
static void ATExecMergePartition(Relation partTableRel, AlterTableCmd* cmd);
static void checkCompressForExchange(Relation partTableRel, Relation ordTableRel);
static void checkColumnForExchange(Relation partTableRel, Relation ordTableRel);
static void checkConstraintForExchange(Relation partTableRel, Relation ordTableRel);

/**
 * @Description: Get the all constraint for specified table.
 * @in relOid, the specified table oid.
 * @in conType, the constraint type, default value is invalid.
 * @return return constraint list.
 */
static List* getConstraintList(Oid relOid, char conType = CONSTRAINT_INVALID);
static void freeConstraintList(List* list);

/**
 * @Description: Whether or not the column has partial cluster key.
 * @in rel, One relation.
 * @in attNum, Represnet the attribute number.
 * @return If exits partial cluster key in the column, return true,
 * otherwise return false.
 */
static bool colHasPartialClusterKey(Relation rel, AttrNumber attNum);
static void checkDistributeForExchange(Relation partTableRel, Relation ordTableRel);
static void checkIndexForExchange(
    Relation partTableRel, Oid partOid, Relation ordTableRel, List** partIndexList, List** ordIndexList);
static void checkValidationForExchange(Relation partTableRel, Relation ordTableRel, Oid partOid, bool exchangeVerbose);

static void finishIndexSwap(List* partIndexList, List* ordIndexList);
static Oid getPartitionOid(Relation partTableRel, const char* partName, RangePartitionDefState* rangePartDef);
static void ATExecSplitPartition(Relation partTableRel, AlterTableCmd* cmd);
static void checkSplitPointForSplit(SplitPartitionState* splitPart, Relation partTableRel, int srcPartSeq);
static void checkDestPartitionNameForSplit(Oid partTableOid, List* partDefList);
static List* getDestPartBoundaryList(Relation partTableRel, List* destPartDefList, List** listForFree);
static void freeDestPartBoundaryList(List* list1, List* list2);
static void fastAddPartition(Relation partTableRel, List* destPartDefList, List** newPartOidList);
static void readTuplesAndInsert(Relation tempTableRel, Relation partTableRel);
static Oid createTempTableForPartition(Relation partTableRel, Partition part);
static void ATPrepEnableRowMovement(Relation rel);
static void ATPrepDisableRowMovement(Relation rel);
static void ATExecModifyRowMovement(Relation rel, bool rowMovement);
static void ATPrepTruncatePartition(Relation rel);
static void ATPrepExchangePartition(Relation rel);
static void ATPrepMergePartition(Relation rel);
static void ATPrepSplitPartition(Relation rel);
static void replaceRepeatChunkId(HTAB* chunkIdHashTable, List* srcPartToastRels);
static bool checkChunkIdRepeat(List* srcPartToastRels, int index, Oid chunkId);
static void addCudescTableForNewPartition(Relation relation, Oid newPartId);
static void addDeltaTableForNewPartition(Relation relation, Oid newPartId);
extern DfsSrvOptions* GetDfsSrvOptions(Oid spcNode);
static bool OptionSupportedByORCRelation(const char* option);
static void checkObjectCreatedinHDFSTblspc(CreateStmt* stmt, char relkind);
/**
 * @Description: Previous check whether the object may be created.
 * @in stmt, the object struct.
 * @in dfsTablespace, whether is a HDFS tablespace.
 */
static void PreCheckCreatedObj(CreateStmt* stmt, bool dfsTablespace, char relKind);
static List* InitDfsOptions(List* options);
static void validateDfsTableDef(CreateStmt* stmt, bool isDfsTbl);
static void simple_delete_redis_tuples(Relation rel, Oid partOid);
static void ResetPartsRedisCtidRelOptions(Relation rel);
static void ResetOnePartRedisCtidRelOptions(Relation rel, Oid part_oid);
static void ResetRelRedisCtidRelOptions(
    Relation rel, Oid part_oid, int cat_id, int att_num, int att_inx, Oid pgcat_oid);
static bool WLMRelationCanTruncate(Relation rel);
static void alter_partition_policy_if_needed(Relation rel, List* defList);
static OnCommitAction GttOncommitOption(const List *options);


/* get all partitions oid */
static List* get_all_part_oid(Oid relid)
{
    List* oid_list = NIL;
    Relation pgpartition;
    HeapScanDesc scan;
    HeapTuple tuple;
    ScanKeyData keys[2];
    /* Process all partitions of this partitiond table */
    ScanKeyInit(&keys[0],
        Anum_pg_partition_parttype,
        BTEqualStrategyNumber,
        F_CHAREQ,
        CharGetDatum(PART_OBJ_TYPE_TABLE_PARTITION));
    ScanKeyInit(&keys[1], Anum_pg_partition_parentid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
    pgpartition = heap_open(PartitionRelationId, AccessShareLock);
    scan = heap_beginscan(pgpartition, SnapshotNow, 2, keys);
    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
        oid_list = lappend_oid(oid_list, HeapTupleGetOid(tuple));
    }
    heap_endscan(scan);
    heap_close(pgpartition, AccessShareLock);
    return oid_list;
}

static List* find_cstore_delta(Relation rel, LOCKMODE lockmode)
{
    List* children = NIL;
    if (!RELATION_IS_PARTITIONED(rel)) {
        /* Add column for delta table. */
        children = lappend_oid(children, RelationGetDeltaRelId(rel));
        LockRelationOid(RelationGetDeltaRelId(rel), lockmode);
    } else {
        List* part_oid_list = get_all_part_oid(RelationGetRelid(rel));
        ListCell* cell = NULL;
        foreach (cell, part_oid_list) {
            Oid part_oid = lfirst_oid(cell);
            Partition partrel = partitionOpen(rel, part_oid, lockmode);
            /* Add column for delta table. */
            children = lappend_oid(children, partrel->pd_part->reldeltarelid);
            LockRelationOid(partrel->pd_part->reldeltarelid, lockmode);
            partitionClose(rel, partrel, NoLock);
        }
        list_free(part_oid_list);
    }
    return children;
}

// all unsupported features are checked and error reported here for cstore table
static void CheckCStoreUnsupportedFeature(CreateStmt* stmt)
{
    Assert(stmt);

    if (stmt->ofTypename) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Unsupport feature"),
                errdetail("cstore/timeseries don't support relation defination "
                    "with composite type using CREATE TABLE OF TYPENAME.")));
    }

    if (stmt->inhRelations) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Unsupport feature"),
                errdetail("cstore/timeseries don't support relation defination with inheritance.")));
    }

    if (stmt->partTableState && stmt->partTableState->intervalPartDef) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Unsupport feature"),
                errdetail("cstore/timeseries don't support interval partition type.")));
    }

    /* Check constraints */
    ListCell* lc = NULL;
    foreach (lc, stmt->tableEltsDup) {
        Node* element = (Node*)lfirst(lc);
        /* check table-level constraints */
        if (IsA(element, Constraint) && !CSTORE_SUPPORT_CONSTRAINT(((Constraint*)element)->contype)) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("column/timeseries store unsupport constraint \"%s\"",
                        GetConstraintType(((Constraint*)element)->contype))));
        } else if (IsA(element, ColumnDef)) {
            List* colConsList = ((ColumnDef*)element)->constraints;
            ListCell* lc2 = NULL;
            /* check column-level constraints */
            foreach (lc2, colConsList) {
                Constraint* colCons = (Constraint*)lfirst(lc2);
                if (!CSTORE_SUPPORT_CONSTRAINT(colCons->contype)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("column/timeseries store unsupport constraint \"%s\"",
                                GetConstraintType(colCons->contype))));
                }
            }
        }
    }
}

void CheckCStoreRelOption(StdRdOptions* std_opt)
{
    Assert(std_opt);
    if (std_opt->partial_cluster_rows < std_opt->max_batch_rows && std_opt->partial_cluster_rows >= 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("PARTIAL_CLUSTER_ROWS cannot be less than MAX_BATCHROW."),
                errdetail("PARTIAL_CLUSTER_ROWS must be greater than or equal to MAX_BATCHROW."),
                errhint("PARTIAL_CLUSTER_ROWS is MAX_BATCHROW multiplied by an integer.")));
    }
}

static void partition_policy_interval_check(StdRdOptions* std_opt)
{
    int32 typmod = -1;
    Interval* ttl_interval = NULL;
    Interval* period_interval = NULL;
    if (pg_strcasecmp(TIME_UNDEFINED, StdRdOptionsGetStringData(std_opt, ttl, TIME_UNDEFINED)) != 0) {
        ttl_interval = char_to_interval((char*)StdRdOptionsGetStringData(std_opt, ttl, TIME_UNDEFINED), typmod);
    }
    if (pg_strcasecmp(TIME_UNDEFINED, StdRdOptionsGetStringData(std_opt, period, TIME_UNDEFINED)) != 0) {
        period_interval = char_to_interval((char*)StdRdOptionsGetStringData(std_opt, period, TIME_UNDEFINED), typmod);
    }
    if (period_interval == NULL) {
        period_interval = char_to_interval(TIME_ONE_DAY, typmod);
        ereport(WARNING, 
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg(" using %s as default period.", TIME_ONE_DAY)));
    }

    if (ttl_interval != NULL && period_interval != NULL && interval_cmp_internal(period_interval, ttl_interval) > 0) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg(" period must smaller than ttl.")));
    }
}

static void check_partion_policy_rel_option(List* options, StdRdOptions* std_opt)
{
    Assert(std_opt);
    ListCell* opt = NULL;
    bool has_opt = false;
    if (options == NULL) {
        return;
    }
    foreach (opt, options) {
        DefElem* def = (DefElem*)lfirst(opt);
        if (pg_strcasecmp(def->defname, "ttl") == 0 || pg_strcasecmp(def->defname, "period") == 0) {
            has_opt = true;
            break;
        }
    }
    if (!has_opt) {
        return;
    }
    partition_policy_interval_check(std_opt);
}

/*
 * Brief        : Whether or not set orientation option and check the validity
 * Input        : options, the options list.
 *                isDfsTbl, whether or not is a dfs table.
 * Output       : isCUFormat, whether ot not the table is CU format.
 * Return Value : Retutn the true if has been setted otherwise return false.
 * Notes        : None.
 */
static bool isOrientationSet(List* options, bool* isCUFormat, bool isDfsTbl)
{
    bool isSetFormat = false;
    ListCell* cell = NULL;
    foreach (cell, options) {
        DefElem* def = (DefElem*)lfirst(cell);
        if (pg_strcasecmp(def->defname, "orientation") == 0) {
            if (isDfsTbl) {
                /* The orientation option values must be "ORIENTATION_COLUMN" or "ORIENTATION_ORC". */
                if (pg_strcasecmp(defGetString(def), ORIENTATION_ORC) != 0) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_OPTION),
                            errmsg("Invalid string for  \"ORIENTATION\" option"),
                            errdetail("Valid string is \"orc\".")));
                }
            } else {
                if (pg_strcasecmp(defGetString(def), ORIENTATION_COLUMN) != 0 &&
                    pg_strcasecmp(defGetString(def), ORIENTATION_TIMESERIES) != 0 &&
                    pg_strcasecmp(defGetString(def), ORIENTATION_ROW) != 0) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_OPTION),
                            errmsg("Invalid string for  \"ORIENTATION\" option"),
                            errdetail("Valid string are \"column\", \"row\", \"timeseries\".")));
                }
            }
            if (pg_strcasecmp(defGetString(def), ORIENTATION_COLUMN) == 0 && isCUFormat != NULL) {
                *isCUFormat = true;
            }
            isSetFormat = true;
            break;
        }
    }
    return isSetFormat;
}

/*
 * @Description: add default reloption of both row and column table.
 * @Param [IN] options: the table's option which user defined.
 * @Param [IN] relkind: table's kind(ordinary table or other database object).
 * @return: option with defalut options.
 */
static List* AddDefaultOptionsIfNeed(List* options, const char relkind, int8 relcmprs)
{
    List* res = options;

    ListCell* cell = NULL;
    bool isCStore = false;
    bool isTsStore = false;
    bool hasCompression = false;
    /* To mark whether table have been create with(orientation = row) */
    bool createWithOrientationRow = false;
    (void)isOrientationSet(options, NULL, false);

    foreach (cell, options) {
        DefElem* def = (DefElem*)lfirst(cell);
        if (pg_strcasecmp(def->defname, "orientation") == 0 &&
            pg_strcasecmp(defGetString(def), ORIENTATION_COLUMN) == 0) {
            isCStore = true;
        } else if (pg_strcasecmp(def->defname, "orientation") == 0 &&
                   pg_strcasecmp(defGetString(def), ORIENTATION_ROW) == 0) {
            createWithOrientationRow = true;
        } else if (pg_strcasecmp(def->defname, "orientation") == 0 &&
            pg_strcasecmp(defGetString(def), ORIENTATION_TIMESERIES) == 0) {
            isTsStore = true;
        } else if (pg_strcasecmp(def->defname, "compression") == 0) {
            if (pg_strcasecmp(defGetString(def), COMPRESSION_NO) != 0 &&
                pg_strcasecmp(defGetString(def), COMPRESSION_YES) != 0 &&
                pg_strcasecmp(defGetString(def), COMPRESSION_LOW) != 0 &&
                pg_strcasecmp(defGetString(def), COMPRESSION_MIDDLE) != 0 &&
                pg_strcasecmp(defGetString(def), COMPRESSION_HIGH) != 0) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid string for  \"COMPRESSION\" option"),
                        errdetail(
                            "Valid string are \"no\", \"yes\", \"low\", \"middle\", \"high\" for non-dfs table.")));
            }
            hasCompression = true;
        } else if (pg_strcasecmp(def->defname, "version") == 0) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_OPTION),
                    errmsg("It is not allowed to assign version option for non-dfs table.")));
        }

        if (pg_strcasecmp(def->defname, "orientation") == 0 && pg_strcasecmp(defGetString(def), ORIENTATION_ORC) == 0) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_OPTION),
                    errmsg("Invalid string for  \"ORIENTATION\" option"),
                    errdetail("Valid string are \"column\", \"row\", \"timeseries\".")));
        }
    }
    if (isCStore && !hasCompression) {
        DefElem* def = makeDefElem("compression", (Node*)makeString(COMPRESSION_LOW));
        res = lappend(options, def);
    }
    /* If it's a row table, give the defalut reloption with orientation and compression. */
    if (!isCStore && !isTsStore && relkind == RELKIND_RELATION) {
        Value* rowCmprOpt = NULL;
        if (IsCompressedByCmprsInPgclass((RelCompressType)relcmprs)) {
            rowCmprOpt = makeString(COMPRESSION_YES);
        } else {
            rowCmprOpt = makeString(COMPRESSION_NO);
        }
        /*
         * As column table default reloptions is {orientation=column,compression=low}, we
         * set the row table default reloptions {orientation=row,compression=no} to keep the
         * display format on "\d(+)" consistently.
         */
        if (options == NULL) {
            DefElem* def1 = makeDefElem("orientation", (Node*)makeString(ORIENTATION_ROW));
            DefElem* def2 = makeDefElem("compression", (Node*)rowCmprOpt);
            res = list_make2(def1, def2);
        } else {
            /*
             * To show orientation at the head of reloption when createWithOrientationRow
             * is false, we use lcons instead of lappend here.
             */
            if (!createWithOrientationRow) {
                DefElem* def1 = makeDefElem("orientation", (Node*)makeString(ORIENTATION_ROW));
                res = lcons(def1, options);
            }
            if (!hasCompression) {
                DefElem* def2 = makeDefElem("compression", (Node*)rowCmprOpt);
                res = lappend(options, def2);
            }
        }
    }
    return res;
}

/*
 * @Description: Previous check whether the object may be created.
 * @in stmt, the object struct.
 * @in dfsTablespace, whether is a HDFS tablespace.
 */
static void PreCheckCreatedObj(CreateStmt* stmt, bool dfsTablespace, char relKind)
{
#ifndef ENABLE_MULTIPLE_NODES
    if (stmt->subcluster != NULL) {
        if (stmt->subcluster->clustertype == SUBCLUSTER_GROUP || stmt->subcluster->clustertype == SUBCLUSTER_NODE)
            DISTRIBUTED_FEATURE_NOT_SUPPORTED();
    }
#endif
    bool ignore_enable_hadoop_env = false;
    ListCell* cell = NULL;
    foreach (cell, stmt->options) {
        DefElem* def = (DefElem*)lfirst(cell);
        if (pg_strcasecmp(def->defname, OptIgnoreEnableHadoopEnv) == 0 && defGetInt64(def) == 1) {
            ignore_enable_hadoop_env = true;
            break;
        }
    }
    if (!ignore_enable_hadoop_env && u_sess->attr.attr_sql.enable_hadoop_env && !dfsTablespace) {
        if (relKind == RELKIND_RELATION && stmt->relation->relpersistence != RELPERSISTENCE_TEMP &&
            stmt->relation->relpersistence != RELPERSISTENCE_UNLOGGED) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("It is unsupported to create row/cstore non-temporary/non-unlogged table in hadoop "
                           "enviroment.")));
        }
    }
    if (dfsTablespace && (stmt->relation->relpersistence == RELPERSISTENCE_TEMP ||
                             stmt->relation->relpersistence == RELPERSISTENCE_UNLOGGED)) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("It is unsupported to create unlogged table and temporary table on DFS tablespace.")));
    }
    if (dfsTablespace && stmt->subcluster && !in_logic_cluster() && IS_PGXC_COORDINATOR &&
        stmt->subcluster->clustertype == SUBCLUSTER_GROUP) {
        /* For dfs table we are going to block TO-GROUP create table request */
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("It is unsupported to create table with to group option on DFS tablespace.")));
    }
    if (RELKIND_FOREIGN_TABLE == relKind && stmt->subcluster && !in_logic_cluster() && IS_PGXC_COORDINATOR &&
        stmt->subcluster->clustertype == SUBCLUSTER_GROUP) {
        /* For foreign table we are going to block TO-GROUP create table request */
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("It is unsupported to create foreign table with to group option.")));
    }
    if (!isRestoreMode && stmt->subcluster && stmt->subcluster->clustertype == SUBCLUSTER_NODE) {
        /* If not in restore mode,  we are going to block TO-NODE create table request */
        ereport(
            ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("CREATE TABLE ... TO NODE is not yet supported.")));
    }
}

/*
 * brief: Check Object to be created. initialize DFS table options.
 *        check DFS table definition.
 * input param @stmt: a CreateStmt struct.
 * input param @stmt: the object kind.
 * Return : None.
 */
static void checkObjectCreatedinHDFSTblspc(CreateStmt* stmt, char relkind)
{
    /* If the object is a logic object, do not initialize and check. */
    if (HDFS_TBLSPC_SUPPORT_CREATE_LOGIC_OBJECT(relkind)) {
        return;
    }
    /* Check options and add default option for Dfs relation if need. */
    stmt->options = InitDfsOptions(stmt->options);
    /* Validate Dfs table definition. */
    validateDfsTableDef(stmt, true);
}

/*
 * brief: Whether ORC format relation support the option or not.
 * input param @option: the option to be checked.
 * Return true if the option is supported by ORC format relation,
 * otherwise return false.
 */
static bool OptionSupportedByORCRelation(const char* option)
{
    for (uint32 i = 0; i < sizeof(ORCSupportOption) / sizeof(char*); ++i) {
        if (0 == pg_strcasecmp(ORCSupportOption[i], option)) {
            return true;
        }
    }
    return false;
}

/*
 * Brief        : Initialize and validity check for options. If the user do
 *                not set option, we assign default value to the option.
 * Input        : options, the setted table options by user.
 * Output       : None.
 * Return Value : Retrun new options list.
 * Notes        : 1. The default orientation value is "ORIENTATION_ORC".
 *                2. The defualt compression value is "COMPRESSION_NO";
 *                3. The default version value is "ORC_VERSION_012".
 */
static List* InitDfsOptions(List* options)
{
    List* res = options;
    ListCell* cell = NULL;
    bool isSetFormat = isOrientationSet(options, NULL, true);
    bool hasSetCompression = false;
    bool hasSetVersion = false;

    foreach (cell, options) {
        DefElem* def = (DefElem*)lfirst(cell);
        if (!OptionSupportedByORCRelation(def->defname)) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_OPTION),
                    errmsg("Unsupport \"%s\" option", def->defname),
                    errdetail("Valid options are \"%s\", \"%s\", \"%s\".",
                        ORCSupportOption[0],
                        ORCSupportOption[1],
                        ORCSupportOption[2])));
        }
        if (pg_strcasecmp(def->defname, "compression") == 0) {
            /*
             * YES equals LOW is SNAPPY plus LOW_COMPRESS.
             * MIDDLE is SNAPPY plus HIGH_COMPRESS.
             * SNAPPY and LZ4 with low compress and ZLIB with HIGH compress.
             * HIGH is ZLIB plus HIGH_COMPRESS.
             */
            if (pg_strcasecmp(defGetString(def), COMPRESSION_NO) != 0 &&
                pg_strcasecmp(defGetString(def), COMPRESSION_YES) != 0 &&
                pg_strcasecmp(defGetString(def), COMPRESSION_ZLIB) != 0 &&
                pg_strcasecmp(defGetString(def), COMPRESSION_SNAPPY) != 0 &&
                pg_strcasecmp(defGetString(def), COMPRESSION_LZ4) != 0 &&
                pg_strcasecmp(defGetString(def), COMPRESSION_LOW) != 0 &&
                pg_strcasecmp(defGetString(def), COMPRESSION_MIDDLE) != 0 &&
                pg_strcasecmp(defGetString(def), COMPRESSION_HIGH) != 0) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid string for  \"COMPRESSION\" option"),
                        errdetail("Valid string are \"no\", \"yes\", \"low\", \"middle\", \"high\", \"snappy\", "
                                  "\"zlib\", \"lz4\" for dfs table.")));
            }
            hasSetCompression = true;
        }
        if (pg_strcasecmp(def->defname, "version") == 0) {
            if (0 != pg_strcasecmp(defGetString(def), ORC_VERSION_012)) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_OPTION),
                        errmsg("Invalid string for  \"VERSION\" option"),
                        errdetail("Valid string is \"0.12\".")));
            }
            hasSetVersion = true;
        }
    }
    if (!isSetFormat) {
        DefElem* def = makeDefElem("orientation", (Node*)makeString(ORIENTATION_ORC));
        res = lappend(res, def);
    }
    if (!hasSetCompression) {
        DefElem* def = NULL;
        def = makeDefElem("compression", (Node*)makeString(COMPRESSION_SNAPPY));
        res = lappend(res, def);
    }
    if (!hasSetVersion) {
        DefElem* def = makeDefElem("version", (Node*)makeString(ORC_VERSION_012));
        res = lappend(res, def);
    }
    return res;
}

/*
 * Brief        : Validate Dfs table definition.
 * Input        : stmt, a CreateStmt struct.
 *                dfsTbl, whethre or not the table is dfs table.
 *                The dfsTbl is true, the table is dfs table.
 * Output       : None.
 * Return Value : None.
 * Notes        : None.
 */
static void validateDfsTableDef(CreateStmt* stmt, bool isDfsTbl)
{
    ListCell* optionCell = NULL;
    char* optionValue = NULL;
    bool cuFormat = false;
    if (!isDfsTbl) {
        return;
    }
    foreach (optionCell, stmt->options) {
        DefElem* optionDef = (DefElem*)lfirst(optionCell);
        char* optionDefName = optionDef->defname;

        if (pg_strcasecmp(optionDefName, "orientation") == 0) {
            optionValue = defGetString(optionDef);
            break;
        }
    }
    /* determine whether it is a CU */
    if ((NULL != optionValue) && (0 == pg_strcasecmp(optionValue, ORIENTATION_COLUMN))) {
        cuFormat = true;
    }
    /*
     * Currently, we only support "Value-Based" partitioning scheme for partitioned
     * HDFS table
     */
    if (stmt->partTableState) {
        /*
         * For value partitioned HDFS table we should force RowMovement ON, as we
         * will enable it anyway for a table created as columanr(PAX-ORC) and also
         * partition case.
         */
        stmt->partTableState->rowMovement = ROWMOVEMENT_ENABLE;

        /* Number of partition key check */
        if (list_length(stmt->partTableState->partitionKey) == 0) {
            ereport(ERROR,
                (errcode(ERRCODE_PARTITION_ERROR),
                    errmsg("Num of partition keys in value-partitioned table should not be zeror")));
        } else if (list_length(stmt->partTableState->partitionKey) > VALUE_PARTKEYMAXNUM) {
            ereport(ERROR,
                (errcode(ERRCODE_PARTITION_ERROR),
                    errmsg("Num of partition keys in value-partitioned table exceeds max allowed num:%d",
                        RANGE_PARTKEYMAXNUM)));
        }
        /* Partition stragegy check */
        if (stmt->partTableState->partitionStrategy != PART_STRATEGY_VALUE) {
            ereport(ERROR,
                (errcode(ERRCODE_PARTITION_ERROR),
                    errmsg("Unsupport partition strategy '%s' feature for dfs table.",
                        GetPartitionStrategyNameByType(stmt->partTableState->partitionStrategy))));
        }
    }
    /*
     * Currently, support hash/replication distribution for dfs table(not cu format).
     * when support other distribution, this code will be deleted.
     */
    if (!cuFormat && stmt->distributeby != NULL && stmt->distributeby->disttype != DISTTYPE_HASH &&
        stmt->distributeby->disttype != DISTTYPE_REPLICATION) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("Only support hash/replication distribution for dfs table.")));
    }
}

/* Check tablespace's permissions for partition */
static void check_part_tbl_space(CreateStmt* stmt, Oid ownerId, bool dfsTablespace)
{
    Oid partitionTablespaceId;
    bool isPartitionTablespaceDfs = false;
    RangePartitionDefState* partitiondef = NULL;
    ListCell* spccell = NULL;

    /* check value partition table is created at DFS table space */
    if (stmt->partTableState->partitionStrategy == PART_STRATEGY_VALUE && !dfsTablespace)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Value partitioned table can only be created on DFS tablespace.")));
    foreach (spccell, stmt->partTableState->partitionList) {
        partitiondef = (RangePartitionDefState*)lfirst(spccell);

        if (partitiondef->tablespacename) {
            partitionTablespaceId = get_tablespace_oid(partitiondef->tablespacename, false);
            isPartitionTablespaceDfs = IsSpecifiedTblspc(partitionTablespaceId, FILESYSTEM_HDFS);
            if (isPartitionTablespaceDfs) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("Partition can not be created on DFS tablespace.Only table-level tablespace can be "
                               "DFS.DFS table only support partition strategy '%s' feature.",
                            GetPartitionStrategyNameByType(PART_STRATEGY_VALUE))));
            }
        }
        CheckPartitionTablespace(partitiondef->tablespacename, ownerId);
    }
}

static void alter_orientation(CreateStmt** stmt, bool all_field, bool all_tag, 
    Datum* reloptions, Node** orientedFrom, char** storeChar)
{
    static const char* const validnsps[] = HEAP_RELOPT_NAMESPACES;
    ListCell* cell = NULL;
    if (all_field) {
        foreach (cell, (*stmt)->options) {
            DefElem* def = (DefElem*)lfirst(cell);
            if (pg_strcasecmp("orientation", def->defname) == 0) {
                def->arg = (Node*)makeString(ORIENTATION_COLUMN);
                break;
            }
        }
        ereport(NOTICE,
            (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
                errmsg("'TSTAG' not found. Using '%s' as the orientation.", ORIENTATION_COLUMN),
                errhint("Please use both 'TSTAG' and 'TSFIELD' if orientation is '%s'.", ORIENTATION_TIMESERIES)));
        *reloptions = transformRelOptions((Datum)0, (*stmt)->options, NULL, validnsps, true, false);
        *orientedFrom = (Node*)makeString(ORIENTATION_COLUMN);
        *storeChar = ORIENTATION_COLUMN;
    } else if (all_tag) {
        foreach (cell, (*stmt)->options) {
            DefElem* def = (DefElem*)lfirst(cell);
            if (pg_strcasecmp("orientation", def->defname) == 0) {
                def->arg = (Node*)makeString(ORIENTATION_ROW);
                break;
            }
        }
        ereport(NOTICE,
            (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
                errmsg("'TSFIELD' not found. Using '%s' as the orientation.", ORIENTATION_ROW),
                errhint("Please use both 'TSTAG' and 'TSFIELD' if orientation is '%s'.", ORIENTATION_TIMESERIES)));
        *reloptions = transformRelOptions((Datum)0, (*stmt)->options, NULL, validnsps, true, false);
        *orientedFrom = (Node*)makeString(ORIENTATION_ROW);
        *storeChar = ORIENTATION_ROW;
    }
}

static bool validate_timeseries(CreateStmt** stmt, Datum* reloptions, char** storeChar, Node** orientedFrom)
{
    bool kvtype_all_tag = true;
    bool kvtype_all_field = true;
    bool is_timeseries = true;
    int kvtype_time_count = 0;
    List *schema = (*stmt)->tableElts;
    ListCell *cell = NULL;

    if ((*stmt)->relation->relpersistence != RELPERSISTENCE_PERMANENT)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg(" unsupported persistency for timeseries table.")));
    /* Currently, only support hash distribution for timeseries table. */
    if ((*stmt)->distributeby != NULL && (*stmt)->distributeby->disttype != DISTTYPE_HASH) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION), 
                errmsg("Only support hash distribution for timeseries table.")));
    }
    foreach (cell, schema) {
        ColumnDef* colDef = (ColumnDef*)lfirst(cell);
        if (colDef->kvtype == ATT_KV_TAG) {
            kvtype_all_field = false;
        } else if (colDef->kvtype == ATT_KV_FIELD) {
            kvtype_all_tag = false;
        } else if (colDef->kvtype == ATT_KV_TIMETAG) {
            kvtype_time_count++;
        } else {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("kvtype of '%s' must be defined when using timeseries.", colDef->colname)));
        }
    }
    /* TIMESERIES only allowed one time column */
    if (kvtype_time_count != 1) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("TIMESERIES must have one and only one time column.")));
    }
    if (kvtype_all_field || kvtype_all_tag) {
        alter_orientation(stmt, kvtype_all_field, kvtype_all_tag, reloptions, orientedFrom, storeChar);
        is_timeseries = false;
    }
    if (is_timeseries && !u_sess->attr.attr_common.enable_tsdb) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Cannot use orientation is timeseries when enable_tsdb is off.")));
    }
    return is_timeseries;
}

static void add_partiton(CreateStmt* stmt, StdRdOptions* std_opt)
{
    List* schema = stmt->tableElts;
    ListCell* cell = NULL;
    int32 typmod = -1;
    RangePartitionDefState* part1 = NULL;
    RangePartitionDefState* part2 = NULL;
    ColumnRef* col_ref = NULL;
    PartitionState* part_state = NULL;
    Const* con1 = NULL;
    Const* con2 = NULL;
    Datum constvalue1, constvalue2;
    Interval* period_interval = NULL;

    if (0 != pg_strcasecmp(TIME_UNDEFINED, StdRdOptionsGetStringData(std_opt, period, TIME_UNDEFINED))) {
        period_interval = char_to_interval((char*)StdRdOptionsGetStringData(std_opt, period, TIME_UNDEFINED), typmod);
    } else {
        period_interval = char_to_interval(TIME_ONE_DAY, typmod);
    }
    constvalue1 = CStringGetDatum((char*)timestamptz_to_str(GetCurrentTimestamp()));
    con1 = makeConst(UNKNOWNOID, -1, InvalidOid, -1, constvalue1, false, true);
    constvalue2 = CStringGetDatum((char*)timestamptz_to_str(GetCurrentTimestamp() + INTERVAL_TO_USEC(period_interval)));
    con2 = makeConst(UNKNOWNOID, -1, InvalidOid, -1, constvalue2, false, true);
    
    part1 = makeNode(RangePartitionDefState);
    part1->partitionName = "default_part_1";
    part1->boundary = list_make1(con1);

    part2 = makeNode(RangePartitionDefState);
    part2->partitionName = "default_part_2";
    part2->boundary = list_make1(con2);

    part_state = makeNode(PartitionState);
    part_state->partitionStrategy = 'r';
    part_state->rowMovement = ROWMOVEMENT_DEFAULT;
    foreach (cell, schema) {
        ColumnDef* col_def = (ColumnDef*)lfirst(cell);
        if (col_def->kvtype == ATT_KV_TIMETAG) {
            col_ref = makeNode(ColumnRef);
            col_ref->fields = list_make1(makeString(col_def->colname));
            col_ref->location = -1;
            part_state->partitionKey = list_make1(col_ref);
            break;
        }
    }
    part_state->partitionList = list_make2(part1, part2);
    stmt->partTableState = part_state;
}

static void partition_policy_check(CreateStmt* stmt, StdRdOptions* std_opt, bool timeseries_checked)
{
    List* schema = stmt->tableElts;
    ListCell* cell = NULL;
    partition_policy_interval_check(std_opt);
    /* 
     * For TIMESERIES storage, it is based on the partition table.
     * If it is not a partition table, turn relation into partition table by construncting
     * regarding partition struncture explicitly with 2 partition forward
     */
    if (stmt->partTableState == NULL && timeseries_checked) {
        add_partiton(stmt, std_opt);
    } 

    if (stmt->partTableState != NULL) {
        List *partitionKey = stmt->partTableState->partitionKey;
        ColumnRef *colRef = NULL;

        if (partitionKey->length != 1) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("Only support one partition Key.")));
        }
        colRef = (ColumnRef*) lfirst(list_head(partitionKey));
        Value *val =  (Value *) lfirst(list_head(colRef->fields));
        char *key_name = val->val.str;

        foreach(cell, schema) {
            ColumnDef  *colDef = (ColumnDef*)lfirst(cell);
            if (pg_strcasecmp(colDef->colname, key_name) == 0) {
                Oid result = InvalidOid;
                Type typtup = LookupTypeName(NULL, colDef->typname, NULL);
                if (typtup == NULL)
                    ereport(ERROR,
                        (errcode(ERRCODE_UNDEFINED_OBJECT),
                            errmsg("type \"%s\" does not exist.",
                                TypeNameToString(colDef->typname))));
                result = typeTypeId(typtup);
                ReleaseSysCache(typtup);
                if (result != TIMESTAMPOID && result != TIMESTAMPTZOID)
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg(" Partition Key must be of type TIMESTAMP(TZ) when using ttl or period.")));
                if (timeseries_checked && colDef->kvtype != ATT_KV_TIMETAG) 
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg(" Partition Key must be of kv type TSTAG.")));
            }
        }
    }
}


/* ----------------------------------------------------------------
 *		DefineRelation
 *				Creates a new relation.
 *
 * stmt carries parsetree information from an ordinary CREATE TABLE statement.
 * The other arguments are used to extend the behavior for other cases:
 * relkind: relkind to assign to the new relation
 * ownerId: if not InvalidOid, use this as the new relation's owner.
 *
 * Note that permissions checks are done against current user regardless of
 * ownerId.  A nonzero ownerId is used when someone is creating a relation
 * "on behalf of" someone else, so we still want to see that the current user
 * has permissions to do it.
 *
 * If successful, returns the OID of the new relation.
 * ----------------------------------------------------------------
 */
Oid DefineRelation(CreateStmt* stmt, char relkind, Oid ownerId)
{
    char relname[NAMEDATALEN];
    Oid namespaceId;
    List* schema = stmt->tableElts;
    Oid relationId;
    Oid tablespaceId;
    Relation rel;
    TupleDesc descriptor;
    List* inheritOids = NIL;
    List* old_constraints = NIL;
    bool localHasOids = false;
    int parentOidCount;
    List* rawDefaults = NIL;
    List* cookedDefaults = NIL;
    Datum reloptions;
    ListCell* listptr = NULL;
    AttrNumber attnum;
    static const char* const validnsps[] = HEAP_RELOPT_NAMESPACES;
    Oid ofTypeId;
    Node* orientedFrom = NULL;
    char* storeChar = ORIENTATION_ROW;
    bool timeseries_checked = false;
    bool dfsTablespace = false;
    bool isInitdbOnDN = false;
    HashBucketInfo* bucketinfo = NULL;

    /*
     * isalter is true, change the owner of the objects as the owner of the
     * namespace, if the owner of the namespce has the same name as the namescpe
     */
    bool isalter = false;
    bool hashbucket = false;

    bool relisshared = u_sess->attr.attr_common.IsInplaceUpgrade && u_sess->upg_cxt.new_catalog_isshared;
    errno_t rc;
    /*
     * Truncate relname to appropriate length (probably a waste of time, as
     * parser should have done this already).
     */
    rc = strncpy_s(relname, NAMEDATALEN, stmt->relation->relname, NAMEDATALEN - 1);
    securec_check(rc, "", "");

    if (stmt->relation->relpersistence == RELPERSISTENCE_UNLOGGED && STMT_RETRY_ENABLED)
        stmt->relation->relpersistence = RELPERSISTENCE_PERMANENT;

    /* Check consistency of arguments */
    if (stmt->oncommit != ONCOMMIT_NOOP && 
        !(stmt->relation->relpersistence == RELPERSISTENCE_TEMP || 
          stmt->relation->relpersistence == RELPERSISTENCE_GLOBAL_TEMP))
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION), errmsg("ON COMMIT can only be used on temporary tables")));

    /* @Temp Table. We do not support on commit drop right now. */
    if ((stmt->relation->relpersistence == RELPERSISTENCE_TEMP ||
         stmt->relation->relpersistence == RELPERSISTENCE_GLOBAL_TEMP) &&
         stmt->oncommit == ONCOMMIT_DROP)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("ON COMMIT only support PRESERVE ROWS or DELETE ROWS option")));

    if (stmt->constraints != NIL && relkind == RELKIND_FOREIGN_TABLE) {
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("constraints on foreign tables are not supported")));
    }

    /*
     * For foreign table ROUNDROBIN distribution is a built-in support.
     */
    if (IsA(stmt, CreateForeignTableStmt) &&
        (IsSpecifiedFDW(((CreateForeignTableStmt*)stmt)->servername, DIST_FDW) ||
            IsSpecifiedFDW(((CreateForeignTableStmt*)stmt)->servername, LOG_FDW) ||
            IsSpecifiedFDW(((CreateForeignTableStmt*)stmt)->servername, GC_FDW)) &&
        (IS_PGXC_COORDINATOR || (isRestoreMode && stmt->subcluster)) && !stmt->distributeby) {
        stmt->distributeby = makeNode(DistributeBy);
        stmt->distributeby->disttype = DISTTYPE_ROUNDROBIN;
        stmt->distributeby->colname = NULL;
    }
    /*
     * Look up the namespace in which we are supposed to create the relation,
     * check we have permission to create there, lock it against concurrent
     * drop, and mark stmt->relation as RELPERSISTENCE_TEMP if a temporary
     * namespace is selected.
     */
    namespaceId = RangeVarGetAndCheckCreationNamespace(stmt->relation, NoLock, NULL);

    if (u_sess->attr.attr_sql.enforce_a_behavior) {
        /* Identify user ID that will own the table
         *
         * change the owner of the objects as the owner of the namespace
         * if the owner of the namespce has the same name as the namescpe
         * note: the object must be of the ordinary table, sequence, view or
         *		composite type
         */
        if (!OidIsValid(ownerId) && (relkind == RELKIND_RELATION || relkind == RELKIND_SEQUENCE ||
                                        relkind == RELKIND_VIEW || relkind == RELKIND_COMPOSITE_TYPE))
            ownerId = GetUserIdFromNspId(namespaceId);

        if (!OidIsValid(ownerId))
            ownerId = GetUserId();
        else if (ownerId != GetUserId())
            isalter = true;

        if (isalter) {
            /* Check namespace permissions. */
            AclResult aclresult;

            aclresult = pg_namespace_aclcheck(namespaceId, ownerId, ACL_CREATE);
            if (aclresult != ACLCHECK_OK)
                aclcheck_error(aclresult, ACL_KIND_NAMESPACE, get_namespace_name(namespaceId, true));
        }
    }
    /*
     * Security check: disallow creating temp tables from security-restricted
     * code.  This is needed because calling code might not expect untrusted
     * tables to appear in pg_temp at the front of its search path.
     */
    if ((stmt->relation->relpersistence == RELPERSISTENCE_TEMP ||
        stmt->relation->relpersistence == RELPERSISTENCE_GLOBAL_TEMP) &&
        InSecurityRestrictedOperation())
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                errmsg("cannot create temporary table within security-restricted operation")));

    /*
     * Select tablespace to use.  If not specified, use default tablespace
     * (which may in turn default to database's default).
     */
    if (stmt->tablespacename) {
        tablespaceId = get_tablespace_oid(stmt->tablespacename, false);
    } else {
        tablespaceId = GetDefaultTablespace(stmt->relation->relpersistence);
        /* note InvalidOid is OK in this case */
    }
    
    dfsTablespace = IsSpecifiedTblspc(tablespaceId, FILESYSTEM_HDFS);
    if (dfsTablespace) {
        FEATURE_NOT_PUBLIC_ERROR("HDFS is not yet supported.");
    }
    if (dfsTablespace && is_feature_disabled(DATA_STORAGE_FORMAT)) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Unsupport the dfs table in this version.")));
    }
    PreCheckCreatedObj(stmt, dfsTablespace, relkind);

    /* Check permissions except when using database's default */
    if (OidIsValid(tablespaceId) && tablespaceId != u_sess->proc_cxt.MyDatabaseTableSpace) {
        AclResult aclresult;

        aclresult = pg_tablespace_aclcheck(tablespaceId, GetUserId(), ACL_CREATE);
        if (aclresult != ACLCHECK_OK)
            aclcheck_error(aclresult, ACL_KIND_TABLESPACE, get_tablespace_name(tablespaceId));
        // view is not related to tablespace, so no need to check permissions
        if (isalter && relkind != RELKIND_VIEW) {
            aclresult = pg_tablespace_aclcheck(tablespaceId, ownerId, ACL_CREATE);
            if (aclresult != ACLCHECK_OK)
                aclcheck_error(aclresult, ACL_KIND_TABLESPACE, get_tablespace_name(tablespaceId));
        }
    }

    /* In all cases disallow placing user relations in pg_global */
    if (!relisshared && tablespaceId == GLOBALTABLESPACE_OID)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("only shared relations can be placed in pg_global tablespace")));

    /* Identify user ID that will own the table */
    if (!OidIsValid(ownerId))
        ownerId = GetUserId();

    /* Add default options for relation if need. */
    if (!dfsTablespace) {
        if (!u_sess->attr.attr_common.IsInplaceUpgrade)
            stmt->options = AddDefaultOptionsIfNeed(stmt->options, relkind, stmt->row_compress);
    } else {
        checkObjectCreatedinHDFSTblspc(stmt, relkind);
    }

    /* Only support one partial cluster key for dfs table. */
    if (stmt->clusterKeys && list_length(stmt->clusterKeys) > 1) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("Only support one partial cluster key for dfs/cstore table.")));
    }

    /* Check tablespace's permissions for partition */
    if (stmt->partTableState) {
        check_part_tbl_space(stmt, ownerId, dfsTablespace);
    }

    /*
     * Parse and validate reloptions, if any.
     */
    /* global temp table */
    OnCommitAction oncommitAction = GttOncommitOption(stmt->options);
    if (stmt->relation->relpersistence == RELPERSISTENCE_GLOBAL_TEMP &&
        relkind == RELKIND_RELATION) {
        if (oncommitAction != ONCOMMIT_NOOP) {
            if (stmt->oncommit != ONCOMMIT_NOOP) {
                elog(ERROR, "could not create global temporary table with on commit and with clause at same time");
            }
            stmt->oncommit = oncommitAction;
        } else {
            DefElem *opt = makeNode(DefElem);

            opt->type = T_DefElem;
            opt->defnamespace = NULL;
            opt->defname = "on_commit_delete_rows";
            opt->defaction = DEFELEM_UNSPEC;

            /* use reloptions to remember on commit clause */
            if (stmt->oncommit == ONCOMMIT_DELETE_ROWS) {
                opt->arg = reinterpret_cast<Node *>(makeString("true"));
            } else if (stmt->oncommit == ONCOMMIT_PRESERVE_ROWS) {
                opt->arg = reinterpret_cast<Node *>(makeString("false"));
            } else if (stmt->oncommit == ONCOMMIT_NOOP) {
                opt->arg = reinterpret_cast<Node *>(makeString("false"));
            } else {
                elog(ERROR, "global temp table not support on commit drop clause");
            }
            stmt->options = lappend(stmt->options, opt);
        }
    } else if (oncommitAction != ONCOMMIT_NOOP) {
        elog(ERROR, "The parameter on_commit_delete_rows is exclusive to the global temp table, which cannot be "
                    "specified by a regular table");
    }

    reloptions = transformRelOptions((Datum)0, stmt->options, NULL, validnsps, true, false);

    orientedFrom = (Node*)makeString(ORIENTATION_ROW); /* default is ORIENTATION_ROW */
    StdRdOptions* std_opt = (StdRdOptions*)heap_reloptions(relkind, reloptions, true);
    if (std_opt != NULL) {
        hashbucket = std_opt->hashbucket;
        if (hashbucket == true && t_thrd.proc->workingVersionNum < 92063) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("hash bucket table not supported in current version!")));
        }
        if (pg_strcasecmp(ORIENTATION_COLUMN, StdRdOptionsGetStringData(std_opt, orientation, ORIENTATION_ROW)) == 0) {
            orientedFrom = (Node*)makeString(ORIENTATION_COLUMN);
            storeChar = ORIENTATION_COLUMN;
        } else if (pg_strcasecmp(ORIENTATION_ORC,
            StdRdOptionsGetStringData(std_opt, orientation, ORIENTATION_ROW)) == 0) {
            /*
             * Don't allow "create DFS table" to run inside a transaction block.
             *
             * "DfsDDLIsTopLevelXact" is set in "case T_CreateStmt" of
             * standard_ProcessUtility function
             *
             * exception: allow "CREATE DFS TABLE" operation in transaction block
             * during redis a table.
             */
            if (IS_PGXC_COORDINATOR && !IsConnFromCoord() && u_sess->attr.attr_sql.enable_cluster_resize == false)
                PreventTransactionChain(u_sess->exec_cxt.DfsDDLIsTopLevelXact, "CREATE DFS TABLE");

            orientedFrom = (Node*)makeString(ORIENTATION_ORC);
            storeChar = ORIENTATION_COLUMN;
        } else if (0 == pg_strcasecmp(ORIENTATION_TIMESERIES,
            StdRdOptionsGetStringData(std_opt, orientation, ORIENTATION_ROW))) {
            orientedFrom = (Node *)makeString(ORIENTATION_TIMESERIES);
            storeChar = ORIENTATION_TIMESERIES;
            /*
             * Check the kvtype parameter legality for timeseries storage method.
             * If all the kvtype exclude tstime are same, change the orientation to row or column explicitly.
             */
            timeseries_checked = validate_timeseries(&stmt, &reloptions, &storeChar, &orientedFrom);
            std_opt = (StdRdOptions*)heap_reloptions(relkind, reloptions, true);
        }
        /*
         * Because we also support create partition policy for non timeseries table, we should check parameter
         * ttl and period if it contains
         */
        if (timeseries_checked ||
            0 != pg_strcasecmp(TIME_UNDEFINED, StdRdOptionsGetStringData(std_opt, ttl, TIME_UNDEFINED)) ||
            0 != pg_strcasecmp(TIME_UNDEFINED, StdRdOptionsGetStringData(std_opt, period, TIME_UNDEFINED))) {
            partition_policy_check(stmt, std_opt, timeseries_checked);
            if (stmt->partTableState != NULL) {
                check_part_tbl_space(stmt, ownerId, dfsTablespace);
                checkPartitionSynax(stmt);
            }
        }
        if (IS_SINGLE_NODE && stmt->partTableState != NULL) {
            if (stmt->partTableState->rowMovement != ROWMOVEMENT_DISABLE)
                stmt->partTableState->rowMovement = ROWMOVEMENT_ENABLE;
        }
        if (0 == pg_strcasecmp(storeChar, ORIENTATION_COLUMN)) {
            CheckCStoreUnsupportedFeature(stmt);
            CheckCStoreRelOption(std_opt);
            ForbidToSetOptionsForColTbl(stmt->options);
            if (stmt->partTableState) {
                if (stmt->partTableState->rowMovement == ROWMOVEMENT_DISABLE) {
                    ereport(NOTICE,
                        (errmsg("disable row movement is invalid for column stored tables."
                                " They always enable row movement between partitions.")));
                }
                /* always enable rowmovement for column stored tables */
                stmt->partTableState->rowMovement = ROWMOVEMENT_ENABLE;
            }
        } else if (0 == pg_strcasecmp(storeChar, ORIENTATION_TIMESERIES)) {
            /* check both support coloumn store and row store */
            CheckCStoreUnsupportedFeature(stmt);
            CheckCStoreRelOption(std_opt);
            ForbidToSetOptionsForColTbl(stmt->options);
            if (stmt->partTableState) {
                if (stmt->partTableState->rowMovement == ROWMOVEMENT_DISABLE)
                    ereport(NOTICE,
                        (errmsg("disable row movement is invalid for column stored tables."
                                " They always enable row movement between partitions.")));
                /* always enable rowmovement for column stored tables */
                stmt->partTableState->rowMovement = ROWMOVEMENT_ENABLE;
            }
            if (relkind == RELKIND_RELATION) {
                /* only care heap relation. ignore foreign table and index relation */
                forbid_to_set_options_for_timeseries_tbl(stmt->options);
            }

            /* construct distribute keys using tstag if not specified */
            if (stmt->distributeby == NULL) {
                ListCell* cell = NULL;
                DistributeBy* newnode = makeNode(DistributeBy);
                List* colnames = NIL;
                newnode->disttype = DISTTYPE_HASH;

                foreach (cell, schema) {
                    ColumnDef* colDef = (ColumnDef*)lfirst(cell);
                    if (colDef->kvtype == ATT_KV_TAG) {
                        Value* val = makeNode(Value);
                        val->type = T_String;
                        val->val.str = colDef->colname;
                        colnames = lappend(colnames, val);
                    }
                }
                newnode->colname = colnames;
                stmt->distributeby = newnode;
            }
        } else {
            if (relkind == RELKIND_RELATION) {
                /* only care heap relation. ignore foreign table and index relation */
                ForbidToSetOptionsForRowTbl(stmt->options);
            }
        }
        pfree_ext(std_opt);
    }

    if (stmt->ofTypename) {
        AclResult aclresult;
        ofTypeId = typenameTypeId(NULL, stmt->ofTypename);
        aclresult = pg_type_aclcheck(ofTypeId, GetUserId(), ACL_USAGE);
        if (aclresult != ACLCHECK_OK)
            aclcheck_error_type(aclresult, ofTypeId);
        if (isalter) {
            ofTypeId = typenameTypeId(NULL, stmt->ofTypename);
            aclresult = pg_type_aclcheck(ofTypeId, ownerId, ACL_USAGE);
            if (aclresult != ACLCHECK_OK)
                aclcheck_error_type(aclresult, ofTypeId);
        }
    } else
        ofTypeId = InvalidOid;

    /*
     * Look up inheritance ancestors and generate relation schema, including
     * inherited attributes.
     */
    schema = MergeAttributes(
        schema, stmt->inhRelations, stmt->relation->relpersistence, &inheritOids, &old_constraints, &parentOidCount);

    /*
     * Create a tuple descriptor from the relation schema.	Note that this
     * deals with column names, types, and NOT NULL constraints, but not
     * default values or CHECK constraints; we handle those below.
     */
    if (relkind == RELKIND_COMPOSITE_TYPE)
        descriptor = BuildDescForRelation(schema, orientedFrom, relkind);
    else
        descriptor = BuildDescForRelation(schema, orientedFrom);

    /* Must specify at least one column when creating a table. */
    if (descriptor->natts == 0 && relkind != RELKIND_COMPOSITE_TYPE) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("must have at least one column")));
    }

    if (stmt->partTableState) {
        List* pos = NIL;

        /* get partitionkey's position */
        pos = GetPartitionkeyPos(stmt->partTableState->partitionKey, schema);

        /* check partitionkey's datatype */
        if (stmt->partTableState->partitionStrategy == PART_STRATEGY_VALUE) {
            CheckValuePartitionKeyType(descriptor->attrs, pos);
        } else if (stmt->partTableState->partitionStrategy == PART_STRATEGY_INTERVAL) {
            CheckIntervalPartitionKeyType(descriptor->attrs, pos);
        } else {
            CheckRangePartitionKeyType(descriptor->attrs, pos);
        }

        /*
         * Check partitionkey's value for none value-partition table as for value
         * partition table, partition value is known until data get loaded.
         */
        if (stmt->partTableState->partitionStrategy != PART_STRATEGY_VALUE)
            ComparePartitionValue(pos, descriptor->attrs, stmt->partTableState);

        list_free_ext(pos);
    }

    localHasOids = interpretOidsOption(stmt->options);
    descriptor->tdhasoid = (localHasOids || parentOidCount > 0);

    if ((pg_strcasecmp(storeChar, ORIENTATION_COLUMN) == 0 || pg_strcasecmp(storeChar, ORIENTATION_TIMESERIES) == 0) &&
        localHasOids) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Local OID column not supported in column/timeseries store tables.")));
    }

    bool is_gc_fdw = false;
    if (!isRestoreMode && IsA(stmt, CreateForeignTableStmt) &&
        (IsSpecifiedFDW(((CreateForeignTableStmt*)stmt)->servername, GC_FDW))) {
        is_gc_fdw = true;
    }

    /*
     * Find columns with default values and prepare for insertion of the
     * defaults.  Pre-cooked (that is, inherited) defaults go into a list of
     * CookedConstraint structs that we'll pass to heap_create_with_catalog,
     * while raw defaults go into a list of RawColumnDefault structs that will
     * be processed by AddRelationNewConstraints.  (We can't deal with raw
     * expressions until we can do transformExpr.)
     *
     * We can set the atthasdef flags now in the tuple descriptor; this just
     * saves StoreAttrDefault from having to do an immediate update of the
     * pg_attribute rows.
     */
    rawDefaults = NIL;
    cookedDefaults = NIL;
    attnum = 0;

    foreach (listptr, schema) {
        ColumnDef* colDef = (ColumnDef*)lfirst(listptr);
        attnum++;
        if (is_gc_fdw) {
            if (colDef->constraints != NULL || colDef->is_not_null == true) {
                ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                        errmsg("column constraint on postgres foreign tables are not supported")));
            }

            Type ctype = typenameType(NULL, colDef->typname, NULL);
            if (ctype) {
                Form_pg_type typtup = (Form_pg_type)GETSTRUCT(ctype);
                if (typtup->typrelid > 0) {
                    ereport(ERROR,
                        (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                            errmsg("relation type column on postgres foreign tables are not supported")));
                }
                ReleaseSysCache(ctype);
            }
        }

        if (colDef->raw_default != NULL) {
            RawColumnDefault* rawEnt = NULL;
            if (relkind == RELKIND_FOREIGN_TABLE) {
                if (!(IsA(stmt, CreateForeignTableStmt) &&
                        isMOTTableFromSrvName(((CreateForeignTableStmt*)stmt)->servername)))
                    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                            errmsg("default values on foreign tables are not supported")));
            }
            Assert(colDef->cooked_default == NULL);
            rawEnt = (RawColumnDefault*)palloc(sizeof(RawColumnDefault));
            rawEnt->attnum = attnum;
            rawEnt->raw_default = colDef->raw_default;
            rawDefaults = lappend(rawDefaults, rawEnt);
            descriptor->attrs[attnum - 1]->atthasdef = true;
        } else if (colDef->cooked_default != NULL) {
            CookedConstraint* cooked = NULL;
            cooked = (CookedConstraint*)palloc(sizeof(CookedConstraint));
            cooked->contype = CONSTR_DEFAULT;
            cooked->name = NULL;
            cooked->attnum = attnum;
            cooked->expr = colDef->cooked_default;
            cooked->skip_validation = false;
            cooked->is_local = true; /* not used for defaults */
            cooked->inhcount = 0;    /* ditto */
            cooked->is_no_inherit = false;
            cookedDefaults = lappend(cookedDefaults, cooked);
            descriptor->attrs[attnum - 1]->atthasdef = true;
        }
    }

    /* Get hash partition key based on relation distribution info */
    bool createbucket = false;
    /* restore mode */
    if (isRestoreMode) {
        /* table need hash partition */
        if (hashbucket == true) {
            /* here is dn */
            if (u_sess->storage_cxt.dumpHashbucketIds != NULL) {
                Assert(stmt->distributeby == NULL);
                createbucket = true;
            } else {
                Assert(stmt->distributeby != NULL);
            }

            bucketinfo = GetRelationBucketInfo(stmt->distributeby, descriptor, &createbucket, InvalidOid, true);
            Assert((createbucket == true && bucketinfo->bucketlist != NULL && bucketinfo->bucketcol != NULL) ||
                   (createbucket == false && bucketinfo->bucketlist == NULL && bucketinfo->bucketcol != NULL));
        }
	    } else {
        /* here is normal mode */
        /* check if the table can be hash partition */
        if (!IS_SINGLE_NODE && !IsInitdb && (relkind == RELKIND_RELATION) && !IsSystemNamespace(namespaceId) &&
            !IsCStoreNamespace(namespaceId) && (0 == pg_strcasecmp(storeChar, ORIENTATION_ROW)) &&
            (stmt->relation->relpersistence == RELPERSISTENCE_PERMANENT)) {
            if (hashbucket == true || createbucket == true) {
                if (IS_PGXC_DATANODE) {
                    createbucket = true;
                }
                bucketinfo = GetRelationBucketInfo(stmt->distributeby, descriptor, 
                    &createbucket, stmt->oldBucket, hashbucket);

                Assert((bucketinfo == NULL && force_createbucket) ||
                       (createbucket == true && bucketinfo->bucketlist != NULL && bucketinfo->bucketcol != NULL) ||
                       (createbucket == false && bucketinfo->bucketlist == NULL && bucketinfo->bucketcol != NULL));
            }
        } else if (hashbucket == true) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("The table %s do not support hash bucket", stmt->relation->relname)));
        }
    }

    /*
     * Create the relation.  Inherited defaults and constraints are passed in
     * for immediate handling --- since they don't need parsing, they can be
     * stored immediately.
     */
    relationId = heap_create_with_catalog(relname,
        namespaceId,
        tablespaceId,
        InvalidOid,
        InvalidOid,
        ofTypeId,
        ownerId,
        descriptor,
        list_concat(cookedDefaults, old_constraints),
        relkind,
        stmt->relation->relpersistence,
        relisshared,
        relisshared,
        localHasOids,
        parentOidCount,
        stmt->oncommit,
        reloptions,
        true,
        (g_instance.attr.attr_common.allowSystemTableMods || u_sess->attr.attr_common.IsInplaceUpgrade),
        stmt->partTableState,
        stmt->row_compress,
        stmt->oldNode,
        bucketinfo);
    if (bucketinfo != NULL) {
        pfree_ext(bucketinfo->bucketcol);
        pfree_ext(bucketinfo->bucketlist);
        pfree_ext(bucketinfo);
    }

    /* Store inheritance information for new rel. */
    StoreCatalogInheritance(relationId, inheritOids);

    /*
     * We must bump the command counter to make the newly-created relation
     * tuple visible for opening.
     */
    CommandCounterIncrement();

#ifdef PGXC
    /*
     * Add to pgxc_class.
     * we need to do this after CommandCounterIncrement
     * Distribution info is to be added under the following conditions:
     * 1. The create table command is being run on a coordinator
     * 2. The create table command is being run in restore mode and
     *    the statement contains distribute by clause.
     *    While adding a new datanode to the cluster an existing dump
     *    that was taken from a datanode is used, and
     *    While adding a new coordinator to the cluster an exiting dump
     *    that was taken from a coordinator is used.
     *    The dump taken from a datanode does NOT contain any DISTRIBUTE BY
     *    clause. This fact is used here to make sure that when the
     *    DISTRIBUTE BY clause is missing in the statemnet the system
     *    should not try to find out the node list itself.
     * 3. When the sum of shmemNumDataNodes and shmemNumCoords equals to one,
     *    the create table command is executed on datanode.In this case, we
     *    do not write created table info in pgxc_class.
     */
    if ((*t_thrd.pgxc_cxt.shmemNumDataNodes + *t_thrd.pgxc_cxt.shmemNumCoords) == 1)
        isInitdbOnDN = true;

    if ((!u_sess->attr.attr_common.IsInplaceUpgrade || !IsSystemNamespace(namespaceId)) &&
        (IS_PGXC_COORDINATOR || (isRestoreMode && stmt->distributeby != NULL && !isInitdbOnDN)) &&
        (relkind == RELKIND_RELATION || (relkind == RELKIND_FOREIGN_TABLE))) {
        char* logic_cluster_name = NULL;
        PGXCSubCluster* subcluster = stmt->subcluster;
        bool isinstallationgroup = (dfsTablespace || relkind == RELKIND_FOREIGN_TABLE);
        if (in_logic_cluster()) {
            isinstallationgroup = false;
            if (subcluster == NULL) {
                logic_cluster_name = PgxcGroupGetCurrentLogicCluster();
                if (logic_cluster_name != NULL) {
                    subcluster = makeNode(PGXCSubCluster);
                    subcluster->clustertype = SUBCLUSTER_GROUP;
                    subcluster->members = list_make1(makeString(logic_cluster_name));
                }
            }
        }

        AddRelationDistribution(
            relationId, stmt->distributeby, subcluster, inheritOids, descriptor, isinstallationgroup);

        if (logic_cluster_name != NULL && subcluster != NULL) {
            list_free_deep(subcluster->members);
            pfree_ext(subcluster);
            pfree_ext(logic_cluster_name);
        }

        CommandCounterIncrement();
        /* Make sure locator info gets rebuilt */
        RelationCacheInvalidateEntry(relationId);
    }
    /* If no Datanodes defined, do not create foreign table  */
    if (IS_PGXC_COORDINATOR && relkind == RELKIND_FOREIGN_TABLE && u_sess->pgxc_cxt.NumDataNodes == 0) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("No Datanode defined in cluster")));
    }
#endif
    /*
     * Open the new relation and acquire exclusive lock on it.	This isn't
     * really necessary for locking out other backends (since they can't see
     * the new rel anyway until we commit), but it keeps the lock manager from
     * complaining about deadlock risks.
     */
    rel = relation_open(relationId, AccessExclusiveLock);

    /*
     * Now add any newly specified column default values and CHECK constraints
     * to the new relation.  These are passed to us in the form of raw
     * parsetrees; we need to transform them to executable expression trees
     * before they can be added. The most convenient way to do that is to
     * apply the parser's transformExpr routine, but transformExpr doesn't
     * work unless we have a pre-existing relation. So, the transformation has
     * to be postponed to this final step of CREATE TABLE.
     */
    if (rawDefaults != NULL || stmt->constraints != NULL)
        (void)AddRelationNewConstraints(rel, rawDefaults, stmt->constraints, true, true);

    /*
     * Now add any cluter key constraint for relation if has.
     */
    if (stmt->clusterKeys)
        AddRelClusterConstraints(rel, stmt->clusterKeys);

    /*
     * Clean up.  We keep lock on new relation (although it shouldn't be
     * visible to anyone else anyway, until commit).
     */
    relation_close(rel, NoLock);

    return relationId;
}

/*
 * Emit the right error or warning message for a "DROP" command issued on a
 * non-existent relation
 */
static void DropErrorMsgNonExistent(const char* relname, char rightkind, bool missing_ok)
{
    const struct dropmsgstrings* rentry = NULL;
    for (rentry = dropmsgstringarray; rentry->kind != '\0'; rentry++) {
        if (rentry->kind == rightkind) {
            if (!missing_ok) {
                ereport(ERROR, (errcode(rentry->nonexistent_code), errmsg(rentry->nonexistent_msg, relname)));
            } else {
                ereport(NOTICE, (errmsg(rentry->skipping_msg, relname)));
                break;
            }
        }
    }
    Assert(rentry->kind != '\0'); /* Should be impossible */
}

static void does_not_exist_skipping_ParallelDDLMode(ObjectType objtype, List* objname, List* objargs, bool missing_ok)
{
    char* msg = NULL;
    char* name = NULL;
    char* args = NULL;
    StringInfo message = makeStringInfo();

    switch (objtype) {
        case OBJECT_SCHEMA:
            msg = gettext_noop("schema \"%s\" does not exist");
            name = NameListToString(objname);
            break;
        case OBJECT_FUNCTION:
            msg = gettext_noop("function %s(%s) does not exist");
            name = NameListToString(objname);
            args = TypeNameListToString(objargs);
            break;
        default: {
            pfree_ext(message->data);
            pfree_ext(message);
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                    errmsg("unexpected object type (%d)", (int)objtype)));
        } break;
    }

    if (missing_ok) {
        if (args == NULL) {
            appendStringInfo(message, msg, name);
        } else {
            appendStringInfo(message, msg, name, args);
        }
        appendStringInfo(message, ", skipping");
        ereport(NOTICE, (errmsg("%s", message->data)));
        pfree_ext(message->data);
        pfree_ext(message);
    } else {
        pfree_ext(message->data);
        pfree_ext(message);
        if (args == NULL)
            ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA), errmsg(msg, name)));
        else
            ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg(msg, name, args)));
    }
}

/*
 * Emit the right error message for a "DROP" command issued on a
 * relation of the wrong type
 */
static void DropErrorMsgWrongType(const char* relname, char wrongkind, char rightkind)
{
    const struct dropmsgstrings* rentry = NULL;
    const struct dropmsgstrings* wentry = NULL;
    for (rentry = dropmsgstringarray; rentry->kind != '\0'; rentry++) {
        if (rentry->kind == rightkind) {
            break;
        }
    }

    Assert(rentry->kind != '\0');
    for (wentry = dropmsgstringarray; wentry->kind != '\0'; wentry++) {
        if (wentry->kind == wrongkind) {
            break;
        }
    }
    /* wrongkind could be something we don't have in our table... */
    ereport(ERROR,
        (errcode(ERRCODE_WRONG_OBJECT_TYPE),
            errmsg(rentry->nota_msg, relname),
            (wentry->kind != '\0') ? errhint("%s", _(wentry->drophint_msg)) : 0));
}

/*
 * PreCheckforRemoveRelation
 *		Check before implementing DROP TABLE, DROP INDEX, DROP SEQUENCE, DROP VIEW,
 *		DROP FOREIGN TABLE to exclude objects which do not exist.
 */
ObjectAddresses* PreCheckforRemoveRelation(DropStmt* drop, StringInfo tmp_queryString, RemoteQueryExecType* exec_type)
{
    ObjectAddresses* objects = NULL;
    char relkind;
    const char* relkind_s = NULL;
    ListCell* cell = NULL;
    LOCKMODE lockmode = AccessExclusiveLock;
    bool cn_miss_relation = false;
    uint32 flags = 0;
    StringInfo relation_namelist = makeStringInfo();

    /* DROP CONCURRENTLY uses a weaker lock, and has some restrictions */
    if (drop->concurrent) {
        flags |= PERFORM_DELETION_CONCURRENTLY;
        lockmode = ShareUpdateExclusiveLock;
        Assert(drop->removeType == OBJECT_INDEX);
        if (list_length(drop->objects) != 1)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("DROP INDEX CONCURRENTLY does not support dropping multiple objects")));
        if (drop->behavior == DROP_CASCADE)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("DROP INDEX CONCURRENTLY does not support CASCADE")));
    }

    /*
     * First we identify all the relations, then we delete them in a single
     * performMultipleDeletions() call.  This is to avoid unwanted DROP
     * RESTRICT errors if one of the relations depends on another.
     */
    /* Determine required relkind */
    switch (drop->removeType) {
        case OBJECT_TABLE:
            relkind = RELKIND_RELATION;
            relkind_s = "TABLE";
            break;
        case OBJECT_INDEX:
            relkind = RELKIND_INDEX;
            relkind_s = "INDEX";
            break;
        case OBJECT_SEQUENCE:
            relkind = RELKIND_SEQUENCE;
            relkind_s = "SEQUENCE";
            break;
        case OBJECT_VIEW:
            relkind = RELKIND_VIEW;
            relkind_s = "VIEW";
            break;
        case OBJECT_FOREIGN_TABLE:
            relkind = RELKIND_FOREIGN_TABLE;
            relkind_s = "FOREIGN TABLE";
            break;
        default: {
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized drop object type: %d", (int)drop->removeType)));
            relkind = 0;    /* keep compiler quiet */
            relkind_s = ""; /* keep compiler quiet */
        } break;
    }

    objects = new_object_addresses();
    foreach (cell, drop->objects) {
        RangeVar* rel = makeRangeVarFromNameList((List*)lfirst(cell));
        Oid relOid;
        ObjectAddress obj;
        Relation delrel;
        struct DropRelationCallbackState state = {0};

        /*
         * These next few steps are a great deal like relation_openrv, but we
         * don't bother building a relcache entry since we don't need it.
         *
         * Check for shared-cache-inval messages before trying to access the
         * relation.  This is needed to cover the case where the name
         * identifies a rel that has been dropped and recreated since the
         * start of our transaction: if we don't flush the old syscache entry,
         * then we'll latch onto that entry and suffer an error later.
         */
        AcceptInvalidationMessages();

        /* Look up the appropriate relation using namespace search. */
        state.relkind = relkind;
        state.heapOid = InvalidOid;
        state.concurrent = drop->concurrent;

        /*
         * Redis in online expansion will get AccessShareLock for resizing table.
         * We use ExclusiveLock here to avoid collisions with AccessShareLock so
         * that we can send drop query to first CN and cancel the redis thread in
         * first CN.
         */
        if (CheckRangeVarInRedistribution(rel)) {
            if (lockmode == AccessExclusiveLock)
                lockmode = ExclusiveLock;
        } else {
            /* Reset lockmode in the next loop for normal table not in redis. */
            if (lockmode == ExclusiveLock)
                lockmode = AccessExclusiveLock;
        }
        relOid = RangeVarGetRelidExtended(
            rel, lockmode, true, false, false, false, RangeVarCallbackForDropRelation, (void*)&state);

        /*
         * Relation not found.
         * For "DROP TABLE/INDEX/VIEW/...", just ERROR
         * For "DROP TABLE/INDEX/VIEW/... IF EXISTS ...",
         * local CN: rewrite the querystring without the not-found relations
         * remote nodes: should not happen since local CN does have the relation
         * so that the query is passed down. In this case, we just ERROR.
         * In maintenance mode, we pass down the original querystring anyway.
         */
        if (!OidIsValid(relOid)) {
            bool missing_ok = drop->missing_ok;
            if (!u_sess->attr.attr_common.xc_maintenance_mode) {
                if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
                    cn_miss_relation = true;
                else
                    missing_ok = false;
            }
            DropErrorMsgNonExistent(rel->relname, relkind, missing_ok);
            continue;
        }
        delrel = try_relation_open(relOid, NoLock);

        /*
         * Open up drop table command for table being redistributed right now.
         *
         * During online expansion time, we only allow to drop object when
         * the object is a table and the target table is not in read only mode
         */
        if (delrel != NULL && !u_sess->attr.attr_sql.enable_cluster_resize &&
            (RelationInClusterResizingReadOnly(delrel) ||
                (RelationInClusterResizing(delrel) && drop->removeType != OBJECT_TABLE))) {
            ereport(ERROR,
                (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
                    errmsg("%s is redistributing, please retry later.", delrel->rd_rel->relname.data)));
        }

        if (delrel != NULL) {
            relation_close(delrel, NoLock);
        }

        /* OK, we're ready to delete this one */
        obj.classId = RelationRelationId;
        obj.objectId = relOid;
        obj.objectSubId = 0;
        add_exact_object_address(&obj, objects);

        /* Record relations that do exist on local CN. */
        char* relation_name = NameListToQuotedString((List*)lfirst(cell));
        appendStringInfo(relation_namelist, relation_namelist->data[0] ? ", %s" : "%s", relation_name);
        pfree_ext(relation_name);
        if (OidIsValid(state.heapOid)) {
            UnlockRelationOid(state.heapOid, state.concurrent ? ShareUpdateExclusiveLock : AccessExclusiveLock);
        }
        UnlockRelationOid(relOid, lockmode);
    }

    /*
     * Fabricate a new DROP TABLE/INDEX/VIEW/... querystring with relations found on local CN.
     * If no such relations, then there is nothing to be done on remote nodes.
     */
    if (cn_miss_relation) {
        if (relation_namelist->data[0])
            appendStringInfo(tmp_queryString,
                "DROP %s IF EXISTS %s %s",
                relkind_s,
                relation_namelist->data,
                drop->behavior == DROP_CASCADE ? "CASCADE" : "RESTRICT");
        else
            *exec_type = EXEC_ON_NONE;
    }
    pfree_ext(relation_namelist->data);
    pfree_ext(relation_namelist);
    return objects;
}

/*
 * RemoveRelationsonMainExecCN
 *		Implements DROP TABLE, DROP INDEX, DROP SEQUENCE, DROP VIEW,
 *		DROP FOREIGN TABLE on main execute coordinator.
 */
void RemoveRelationsonMainExecCN(DropStmt* drop, ObjectAddresses* objects)
{
    uint32 flags = 0;
    /* DROP CONCURRENTLY uses a weaker lock, and has some restrictions */
    if (drop->concurrent) {
        flags |= PERFORM_DELETION_CONCURRENTLY;
        Assert(drop->removeType == OBJECT_INDEX);
        if (list_length(drop->objects) != 1)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("DROP INDEX CONCURRENTLY does not support dropping multiple objects")));
        if (drop->behavior == DROP_CASCADE)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("DROP INDEX CONCURRENTLY does not support CASCADE")));
    }

    for (int i = 0; i < objects->numrefs; i++) {
        const ObjectAddress* thisobj = objects->refs + i;
        Assert(thisobj->objectId);
        if (thisobj->classId == RelationRelationId) {
            /*
             * In DROP INDEX CONCURRENTLY, take only ShareUpdateExclusiveLock on
             * the index for the moment.  index_drop() will promote the lock once
             * it's safe to do so.  In all other cases we need full exclusive
             * lock.
             */
            if (flags & PERFORM_DELETION_CONCURRENTLY)
                LockRelationOid(thisobj->objectId, ShareUpdateExclusiveLock);
            else
                LockRelationOid(thisobj->objectId, AccessExclusiveLock);
        } else {
            /* assume we should lock the whole object not a sub-object */
            LockDatabaseObject(thisobj->classId, thisobj->objectId, 0, AccessExclusiveLock);
        }
    }
    performMultipleDeletions(objects, drop->behavior, flags);
}

/*
 * RemoveRelations
 *		Implements DROP TABLE, DROP INDEX, DROP SEQUENCE, DROP VIEW,
 *		DROP FOREIGN TABLE on datanodes and none main execute coordinator.
 */
void RemoveRelations(DropStmt* drop, StringInfo tmp_queryString, RemoteQueryExecType* exec_type)
{
    ObjectAddresses* objects = NULL;
    char relkind;
    const char* relkind_s = NULL;
    ListCell* cell = NULL;
    uint32 flags = 0;
    LOCKMODE lockmode = AccessExclusiveLock;
    bool cn_miss_relation = false;
    StringInfo relation_namelist = makeStringInfo();
    char relPersistence;

    /* DROP CONCURRENTLY uses a weaker lock, and has some restrictions */
    if (drop->concurrent) {
        lockmode = ShareUpdateExclusiveLock;
        Assert(drop->removeType == OBJECT_INDEX);
        if (list_length(drop->objects) != 1)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("DROP INDEX CONCURRENTLY does not support dropping multiple objects")));
        if (drop->behavior == DROP_CASCADE)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("DROP INDEX CONCURRENTLY does not support CASCADE")));
    }

    /*
     * First we identify all the relations, then we delete them in a single
     * performMultipleDeletions() call.  This is to avoid unwanted DROP
     * RESTRICT errors if one of the relations depends on another.
     */
    /* Determine required relkind */
    switch (drop->removeType) {
        case OBJECT_TABLE:
            relkind = RELKIND_RELATION;
            relkind_s = "TABLE";
            break;
        case OBJECT_INDEX:
            relkind = RELKIND_INDEX;
            relkind_s = "INDEX";
            break;
        case OBJECT_SEQUENCE:
            relkind = RELKIND_SEQUENCE;
            relkind_s = "SEQUENCE";
            break;
        case OBJECT_VIEW:
            relkind = RELKIND_VIEW;
            relkind_s = "VIEW";
            break;
        case OBJECT_FOREIGN_TABLE:
            relkind = RELKIND_FOREIGN_TABLE;
            relkind_s = "FOREIGN TABLE";
            break;
        default: {
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized drop object type: %d", (int)drop->removeType)));

            relkind = 0;    /* keep compiler quiet */
            relkind_s = ""; /* keep compiler quiet */
        } break;
    }

    /* Lock and validate each relation; build a list of object addresses */
    objects = new_object_addresses();

    foreach (cell, drop->objects) {
        RangeVar* rel = makeRangeVarFromNameList((List*)lfirst(cell));
        Oid relOid;
        ObjectAddress obj;
        Relation delrel;
        struct DropRelationCallbackState state = {0};

        /*
         * These next few steps are a great deal like relation_openrv, but we
         * don't bother building a relcache entry since we don't need it.
         *
         * Check for shared-cache-inval messages before trying to access the
         * relation.  This is needed to cover the case where the name
         * identifies a rel that has been dropped and recreated since the
         * start of our transaction: if we don't flush the old syscache entry,
         * then we'll latch onto that entry and suffer an error later.
         */
        AcceptInvalidationMessages();
        /* Look up the appropriate relation using namespace search. */
        state.relkind = relkind;
        state.heapOid = InvalidOid;
        state.concurrent = drop->concurrent;
        relOid = RangeVarGetRelidExtended(
            rel, lockmode, true, false, false, false, RangeVarCallbackForDropRelation, (void*)&state);
        /*
         * Relation not found.
         *
         * For "DROP TABLE/INDEX/VIEW/...", just ERROR
         *
         * For "DROP TABLE/INDEX/VIEW/... IF EXISTS ...",
         * local CN: rewrite the querystring without the not-found relations
         * remote nodes: should not happen since local CN does have the relation
         * so that the query is passed down. In this case, we just ERROR.
         * In maintenance mode, we pass down the original querystring anyway.
         */
        if (!OidIsValid(relOid)) {
            bool missing_ok = drop->missing_ok;

            if (!u_sess->attr.attr_common.xc_maintenance_mode && !IS_SINGLE_NODE) {
                if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
                    cn_miss_relation = true;
                else
                    missing_ok = false;
            }

            DropErrorMsgNonExistent(rel->relname, relkind, missing_ok);
            continue;
        }

        delrel = try_relation_open(relOid, NoLock);
        /*
         * Open up drop table command for table being redistributed right now.
         *
         * During online expansion time, we only allow to drop object when
         * the object is a table and the target table is not in read only mode
         */
        if (delrel != NULL && !u_sess->attr.attr_sql.enable_cluster_resize &&
            (RelationInClusterResizingReadOnly(delrel) ||
                (RelationInClusterResizing(delrel) && drop->removeType != OBJECT_TABLE))) {
            ereport(ERROR,
                (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
                    errmsg("%s is redistributing, please retry later.", delrel->rd_rel->relname.data)));
        }

        // cstore relation doesn't support concurrent INDEX now.
        if (drop->concurrent == true && delrel != NULL && OidIsValid(delrel->rd_rel->relcudescrelid)) {
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("column store table does not support concurrent INDEX yet"),
                errdetail("The feature is not currently supported")));
        }

        if (delrel != NULL) {
            relation_close(delrel, NoLock);
        }
        
        relPersistence = get_rel_persistence(relOid);
        if (drop->concurrent &&
                !(relPersistence == RELPERSISTENCE_TEMP || relPersistence == RELPERSISTENCE_GLOBAL_TEMP)) {
            Assert(list_length(drop->objects) == 1 && drop->removeType == OBJECT_INDEX);
            flags |= PERFORM_DELETION_CONCURRENTLY;
        }
        /* OK, we're ready to delete this one */
        obj.classId = RelationRelationId;
        obj.objectId = relOid;
        obj.objectSubId = 0;

        add_exact_object_address(&obj, objects);

        /* Record relations that do exist on local CN. */
        char* relation_name = NameListToQuotedString((List*)lfirst(cell));
        appendStringInfo(relation_namelist, relation_namelist->data[0] ? ", %s" : "%s", relation_name);
        pfree_ext(relation_name);
    }

    /*
     * Fabricate a new DROP TABLE/INDEX/VIEW/... querystring with relations found on local CN.
     * If no such relations, then there is nothing to be done on remote nodes.
     */
    if (cn_miss_relation) {
        if (relation_namelist->data[0])
            appendStringInfo(tmp_queryString,
                "DROP %s IF EXISTS %s %s",
                relkind_s,
                relation_namelist->data,
                drop->behavior == DROP_CASCADE ? "CASCADE" : "RESTRICT");
        else
            *exec_type = EXEC_ON_NONE;
    }

    performMultipleDeletions(objects, drop->behavior, flags);

    free_object_addresses(objects);
    pfree_ext(relation_namelist->data);
    pfree_ext(relation_namelist);
}

/*
 * PreCheckforRemoveObjects
 *		Check before implementing DROP SCHEMA, DROP FUNCTION to exclude objects which do not exist.
 */
ObjectAddresses* PreCheckforRemoveObjects(
    DropStmt* stmt, StringInfo tmp_queryString, RemoteQueryExecType* exec_type, bool isFirstNode, bool is_securityadmin)
{
    ObjectAddresses* objects = NULL;
    ListCell* cell1 = NULL;
    ListCell* cell2 = NULL;
    bool skip_check = false;
    bool cn_miss_relation = false;
    StringInfo relation_namelist = makeStringInfo();
    const char* relkind_s = NULL;

    if (stmt->removeType == OBJECT_SCHEMA)
        relkind_s = "SCHEMA";
    else if (stmt->removeType == OBJECT_FUNCTION)
        relkind_s = "FUNCATION";
    else {
        ereport(ERROR,
            (errcode(ERRCODE_UNEXPECTED_NODE_STATE), errmsg("unexpected object type (%d)", (int)stmt->removeType)));
    }
    objects = new_object_addresses();
    foreach (cell1, stmt->objects) {
        ObjectAddress address;
        List* objname = (List*)lfirst(cell1);
        List* objargs = NIL;
        Relation relation = NULL;
        Oid namespaceId;
        if (stmt->arguments) {
            cell2 = (!cell2 ? list_head(stmt->arguments) : lnext(cell2));
            objargs = (List*)lfirst(cell2);
        }

        /* Get an ObjectAddress for the object. */
        address =
            get_object_address(stmt->removeType, objname, objargs, &relation, AccessExclusiveLock, stmt->missing_ok);

        /* Issue NOTICE if supplied object was not found. */
        if (!OidIsValid(address.objectId)) {
            bool missing_ok = stmt->missing_ok;

            if (!u_sess->attr.attr_common.xc_maintenance_mode) {
                if (IS_PGXC_COORDINATOR && !IsConnFromCoord())
                    cn_miss_relation = true;
                else
                    missing_ok = false;
            }
            does_not_exist_skipping_ParallelDDLMode(stmt->removeType, objname, objargs, missing_ok);
            continue;
        }

        /*
         * Although COMMENT ON FUNCTION, SECURITY LABEL ON FUNCTION, etc. are
         * happy to operate on an aggregate as on any other function, we have
         * historically not allowed this for DROP FUNCTION.
         */
        if (stmt->removeType == OBJECT_FUNCTION) {
            Oid funcOid = address.objectId;
            HeapTuple tup;

            /* if the function is a builtin function, its oid is less than 10000.
             * we can't allow drop the builtin functions
             */
            if (IsBuiltinFuncOid(funcOid) && u_sess->attr.attr_common.IsInplaceUpgrade == false) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
                        errmsg(
                            "function \"%s\" is a builtin function,it can not be droped", NameListToString(objname))));
            }

            tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcOid));
            if (!HeapTupleIsValid(tup)) {
                /* should not happen */
                ereport(ERROR,
                    (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for function %u", funcOid)));
            }

            if (PROC_IS_AGG(((Form_pg_proc)GETSTRUCT(tup))->prokind))
                ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                        errmsg("\"%s\" is an aggregate function", NameListToString(objname)),
                        errhint("Use DROP AGGREGATE to drop aggregate functions.")));

            ReleaseSysCache(tup);
        }

        /* @Temp Table. myTempNamespace and myTempToastNamespace's owner is
         * bootstrap user, so can not be deleted by ordinary user. to ensuer this two
         * schema be deleted on session quiting, we should bypass acl check when
         * drop my own temp namespace
         */
        if (stmt->removeType == OBJECT_SCHEMA && (address.objectId == u_sess->catalog_cxt.myTempNamespace ||
                                                     address.objectId == u_sess->catalog_cxt.myTempToastNamespace))
            skip_check = true;

        /* Check permissions. */
        namespaceId = get_object_namespace(&address);
        if ((!is_securityadmin) && (!skip_check) &&
            (!OidIsValid(namespaceId) || !pg_namespace_ownercheck(namespaceId, GetUserId())))
            check_object_ownership(GetUserId(), stmt->removeType, address, objname, objargs, relation);

        /* Release any relcache reference count, but keep lock until commit. */
        if (relation)
            heap_close(relation, NoLock);

        add_exact_object_address(&address, objects);

        /* Record relations that do exist on local CN. */
        char* relation_name = NameListToQuotedString((List*)lfirst(cell1));
        appendStringInfo(relation_namelist, relation_namelist->data[0] ? ", %s" : "%s", relation_name);
        pfree_ext(relation_name);

        if (isFirstNode)
            continue;
        Assert(address.classId != RelationRelationId);
        if (IsSharedRelation(address.classId))
            UnlockSharedObject(address.classId, address.objectId, 0, AccessExclusiveLock);
        else
            UnlockDatabaseObject(address.classId, address.objectId, 0, AccessExclusiveLock);
    }

    /*
     * Fabricate a new DROP TABLE/INDEX/VIEW/... querystring with relations found on local CN.
     * If no such relations, then there is nothing to be done on remote nodes.
     */
    if (cn_miss_relation) {
        if (relation_namelist->data[0])
            appendStringInfo(tmp_queryString,
                "DROP %s IF EXISTS %s %s",
                relkind_s,
                relation_namelist->data,
                stmt->behavior == DROP_CASCADE ? "CASCADE" : "RESTRICT");
        else
            *exec_type = EXEC_ON_NONE;
    }

    pfree_ext(relation_namelist->data);
    pfree_ext(relation_namelist);

    return objects;
}

/*
 * RemoveObjectsonMainExecCN
 *		Implements DROP SCHEMA, DROP FUNCTION
 *		on main execute coordinator.
 */
void RemoveObjectsonMainExecCN(DropStmt* drop, ObjectAddresses* objects, bool isFirstNode)
{
    if (!isFirstNode) {
        for (int i = 0; i < objects->numrefs; i++) {
            const ObjectAddress* thisobj = objects->refs + i;

            Assert(thisobj->classId != RelationRelationId);
            Assert(thisobj->objectId);

            if (IsSharedRelation(thisobj->classId))
                LockSharedObject(thisobj->classId, thisobj->objectId, 0, AccessExclusiveLock);
            else
                LockDatabaseObject(thisobj->classId, thisobj->objectId, 0, AccessExclusiveLock);
        }
    }

    /* Here we really delete them. */
    performMultipleDeletions(objects, drop->behavior, 0);
}

/*
 * Before acquiring a table lock, check whether we have sufficient rights.
 * In the case of DROP INDEX, also try to lock the table before the index.
 */
static void RangeVarCallbackForDropRelation(
    const RangeVar* rel, Oid relOid, Oid oldRelOid, bool target_is_partition, void* arg)
{
    HeapTuple tuple;
    struct DropRelationCallbackState* state;
    char relkind;
    Form_pg_class classform;
    LOCKMODE heap_lockmode;
    state = (struct DropRelationCallbackState*)arg;
    relkind = state->relkind;
    heap_lockmode = state->concurrent ? ShareUpdateExclusiveLock : AccessExclusiveLock;
    if (target_is_partition)
        heap_lockmode = AccessShareLock;

    /*
     * If we previously locked some other index's heap, and the name we're
     * looking up no longer refers to that relation, release the now-useless
     * lock.
     */
    if (relOid != oldRelOid && OidIsValid(state->heapOid)) {
        UnlockRelationOid(state->heapOid, heap_lockmode);
        state->heapOid = InvalidOid;
    }

    /* Didn't find a relation, so no need for locking or permission checks. */
    if (!OidIsValid(relOid))
        return;

    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
    if (!HeapTupleIsValid(tuple))
        return; /* concurrently dropped, so nothing to do */
    classform = (Form_pg_class)GETSTRUCT(tuple);

    if ((classform->relkind != relkind) && !(u_sess->attr.attr_common.IsInplaceUpgrade && relkind == RELKIND_RELATION &&
                                               classform->relkind == RELKIND_TOASTVALUE))
        DropErrorMsgWrongType(rel->relname, classform->relkind, relkind);

    /* Allow DROP to either table owner or schema owner */
    if (!pg_class_ownercheck(relOid, GetUserId()) && !pg_namespace_ownercheck(classform->relnamespace, GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, rel->relname);

    if (!g_instance.attr.attr_common.allowSystemTableMods && !u_sess->attr.attr_common.IsInplaceUpgrade &&
        IsSystemClass(classform))
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                errmsg("permission denied: \"%s\" is a system catalog", rel->relname)));

    ReleaseSysCache(tuple);

    /*
     * In DROP INDEX, attempt to acquire lock on the parent table before
     * locking the index.  index_drop() will need this anyway, and since
     * regular queries lock tables before their indexes, we risk deadlock if
     * we do it the other way around.  No error if we don't find a pg_index
     * entry, though --- the relation may have been dropped.
     */
    if (relkind == RELKIND_INDEX && relOid != oldRelOid) {
        state->heapOid = IndexGetRelation(relOid, true);
        if (OidIsValid(state->heapOid))
            LockRelationOid(state->heapOid, heap_lockmode);
    }
}

/*
 * ExecuteTruncate
 *		Executes a TRUNCATE command.
 *
 * This is a multi-relation truncate.  We first open and grab exclusive
 * lock on all relations involved, checking permissions and otherwise
 * verifying that the relation is OK for truncation.  In CASCADE mode,
 * relations having FK references to the targeted relations are automatically
 * added to the group; in RESTRICT mode, we check that all FK references are
 * internal to the group that's being truncated.  Finally all the relations
 * are truncated and reindexed.
 */
#ifdef PGXC
void ExecuteTruncate(TruncateStmt* stmt, const char* sql_statement)
#else
void ExecuteTruncate(TruncateStmt* stmt)
#endif
{
    List* rels = NIL;
    List* relids = NIL;
    List* seq_relids = NIL;
    List* rels_in_redis = NIL;
    EState* estate = NULL;
    ResultRelInfo* resultRelInfos = NULL;
    ResultRelInfo* resultRelInfo = NULL;
    SubTransactionId mySubid;
    ListCell* cell = NULL;
    bool isDfsTruncate = false;
#ifdef PGXC
    char* FirstExecNode = NULL;
    bool isFirstNode = false;

    if (IS_PGXC_COORDINATOR && !IsConnFromCoord()) {
        FirstExecNode = find_first_exec_cn();
        isFirstNode = (strcmp(FirstExecNode, g_instance.attr.attr_common.PGXCNodeName) == 0);
    }
#endif

#ifdef PGXC
    if (stmt->restart_seqs)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("PGXC does not support RESTART IDENTITY yet"),
                errdetail("The feature is not supported currently")));
#endif

#ifdef PGXC
    /*
     * If I am the main execute CN but not CCN,
     * Notify the CCN to create firstly, and then notify other CNs except me.
     */
    if (IS_PGXC_COORDINATOR && !IsConnFromCoord()) {
        if (u_sess->attr.attr_sql.enable_parallel_ddl && !isFirstNode) {
            bool is_temp = false;
            RemoteQuery* step = makeNode(RemoteQuery);

            /* Check un-allowed case where truncate tables from different node groups */
            if (!ObjectsInSameNodeGroup(stmt->relations, T_TruncateStmt)) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("NOT-SUPPORT: Not support TRUNCATE multiple objects different nodegroup")));
            }

            foreach (cell, stmt->relations) {
                Oid relid;
                RangeVar* rel = (RangeVar*)lfirst(cell);

                relid = RangeVarGetRelid(rel, NoLock, false);

                if (IsTempTable(relid)) {
                    is_temp = true;
                    break;
                }
            }

            step->combine_type = COMBINE_TYPE_SAME;
            step->exec_nodes = NULL;
            step->sql_statement = pstrdup(sql_statement);
            step->force_autocommit = false;

            if (is_temp)
                step->exec_type = EXEC_ON_NONE;
            else
                step->exec_type = EXEC_ON_COORDS;

            step->is_temp = is_temp;
            ExecRemoteUtility_ParallelDDLMode(step, FirstExecNode);
            pfree_ext(step->sql_statement);
            pfree_ext(step);
        }
    }
#endif

    /*
     * Open, exclusive-lock, and check all the explicitly-specified relations
     */
    foreach (cell, stmt->relations) {
        RangeVar* rv = (RangeVar*)lfirst(cell);
        Relation rel, myrel;
        bool recurse = interpretInhOption(rv->inhOpt);
        Oid myrelid;

        /*
         * Need to let ProcSleep know if we could cancel redistribution transaction which
         * locks the table we want to truncate. ProcSleep will make sure we only cancel the
         * transaction doing redistribution.
         *
         * Move the following check outside of loop??
         */
        if (IS_PGXC_COORDINATOR)
            u_sess->exec_cxt.could_cancel_redistribution = true;

        rel = heap_openrv(rv, AccessExclusiveLock);
        myrel = rel;
        myrelid = RelationGetRelid(rel);

        /* don't throw error for "TRUNCATE foo, foo" */
        if (list_member_oid(relids, myrelid)) {
            heap_close(rel, AccessExclusiveLock);
            continue;
        }
        truncate_check_rel(rel);
        rels = lappend(rels, rel);
        relids = lappend_oid(relids, myrelid);

        if (recurse) {
            ListCell* child = NULL;
            List* children = NIL;

            children = find_all_inheritors(myrelid, AccessExclusiveLock, NULL);

            foreach (child, children) {
                Oid childrelid = lfirst_oid(child);

                if (list_member_oid(relids, childrelid))
                    continue;

                /* find_all_inheritors already got lock */
                rel = heap_open(childrelid, NoLock);
                truncate_check_rel(rel);
                rels = lappend(rels, rel);
                relids = lappend_oid(relids, childrelid);
            }
        }

#ifdef PGXC
        /*
         * If the truncate table is in the process of redistribution i.e
         * ALTER TABLE myrelid.table SET (APPEND_MODE=ON, rel_cn_oid = myrelid),
         * we have to truncate delete delta table and myrelid new table assocated
         * with myrelid.
         */
        if (IS_PGXC_DATANODE && RelationInClusterResizing(myrel) && !RelationInClusterResizingReadOnly(myrel)) {
            /*
             * Always keep the order consistent by operating on multi catchup delete delta first and then the delete
             * delta.
             */
            Relation delete_delta_rel_x =
                GetAndOpenDeleteDeltaRel(myrel, AccessExclusiveLock, true); /* Multi catchup delete delta */
            Relation delete_delta_rel = GetAndOpenDeleteDeltaRel(myrel, AccessExclusiveLock, false);
            Relation new_table_rel = GetAndOpenNewTableRel(myrel, AccessExclusiveLock);

            /* Multi catchup delta relation can be NULL. */
            if (delete_delta_rel_x) {
                truncate_check_rel(delete_delta_rel_x);
                rels = lappend(rels, delete_delta_rel_x);
                relids = lappend_oid(relids, RelationGetRelid(delete_delta_rel_x));
            }

            /*
             * delete_delta_rel and new_table_rel won't be NULL when myrel is in
             * redistribution. GetAndOpenDeleteDeltaRel and GetAndOpenNewTableRel will
             * issue errors if they are NULL. delete_delt_rel and new_table_rel will
             * be heap_close in the end when we travers rels list.
             */
            truncate_check_rel(delete_delta_rel);
            rels = lappend(rels, delete_delta_rel);
            relids = lappend_oid(relids, RelationGetRelid(delete_delta_rel));
            truncate_check_rel(new_table_rel);
            rels = lappend(rels, new_table_rel);
            relids = lappend_oid(relids, RelationGetRelid(new_table_rel));

            /*
             * Saved non partition relation in list and will truncate
             * redistribution relationed aux table before exit.
             * Partition table will be handled at paritioin table flow
             */
            rels_in_redis = lappend(rels_in_redis, myrel);
        }
#endif
    }

    /*
     * In CASCADE mode, suck in all referencing relations as well.	This
     * requires multiple iterations to find indirectly-dependent relations. At
     * each phase, we need to exclusive-lock new rels before looking for their
     * dependencies, else we might miss something.	Also, we check each rel as
     * soon as we open it, to avoid a faux pas such as holding lock for a long
     * time on a rel we have no permissions for.
     */
    if (stmt->behavior == DROP_CASCADE) {
        for (;;) {
            List* newrelids = NIL;

            newrelids = heap_truncate_find_FKs(relids);
            if (newrelids == NIL)
                break; /* nothing else to add */

            foreach (cell, newrelids) {
                Oid relid = lfirst_oid(cell);
                Relation rel;

                rel = heap_open(relid, AccessExclusiveLock);
                ereport(NOTICE, (errmsg("truncate cascades to table \"%s\"", RelationGetRelationName(rel))));
                truncate_check_rel(rel);
                rels = lappend(rels, rel);
                relids = lappend_oid(relids, relid);
            }
        }
    }

    /*
     * Check foreign key references.  In CASCADE mode, this should be
     * unnecessary since we just pulled in all the references; but as a
     * cross-check, do it anyway if in an Assert-enabled build.
     */
#ifdef USE_ASSERT_CHECKING
    heap_truncate_check_FKs(rels, false);
#else
    if (stmt->behavior == DROP_RESTRICT)
        heap_truncate_check_FKs(rels, false);
#endif

    /*
     * If we are asked to restart sequences, find all the sequences, lock them
     * (we need AccessExclusiveLock for ResetSequence), and check permissions.
     * We want to do this early since it's pointless to do all the truncation
     * work only to fail on sequence permissions.
     */
    if (stmt->restart_seqs) {
        foreach (cell, rels) {
            Relation rel = (Relation)lfirst(cell);
            List* seqlist = getOwnedSequences(RelationGetRelid(rel));
            ListCell* seqcell = NULL;

            foreach (seqcell, seqlist) {
                Oid seq_relid = lfirst_oid(seqcell);
                Relation seq_rel;

                seq_rel = relation_open(seq_relid, AccessExclusiveLock);

                /* This check must match AlterSequence! */
                if (!pg_class_ownercheck(seq_relid, GetUserId()))
                    aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, RelationGetRelationName(seq_rel));

                seq_relids = lappend_oid(seq_relids, seq_relid);

                relation_close(seq_rel, NoLock);
            }
        }
    }

    /* Prepare to catch AFTER triggers. */
    AfterTriggerBeginQuery();

    /*
     * To fire triggers, we'll need an EState as well as a ResultRelInfo for
     * each relation.  We don't need to call ExecOpenIndices, though.
     */
    estate = CreateExecutorState();
    resultRelInfos = (ResultRelInfo*)palloc(list_length(rels) * sizeof(ResultRelInfo));
    resultRelInfo = resultRelInfos;
    foreach (cell, rels) {
        Relation rel = (Relation)lfirst(cell);

        InitResultRelInfo(resultRelInfo,
            rel,
            0, /* dummy rangetable index */
            0);
        resultRelInfo++;
    }
    estate->es_result_relations = resultRelInfos;
    estate->es_num_result_relations = list_length(rels);

    /*
     * Process all BEFORE STATEMENT TRUNCATE triggers before we begin
     * truncating (this is because one of them might throw an error). Also, if
     * we were to allow them to prevent statement execution, that would need
     * to be handled here.
     */
    resultRelInfo = resultRelInfos;
    foreach (cell, rels) {
        estate->es_result_relation_info = resultRelInfo;
        ExecBSTruncateTriggers(estate, resultRelInfo);
        resultRelInfo++;
    }

    /*
     * OK, truncate each table.
     */
    mySubid = GetCurrentSubTransactionId();

    foreach (cell, rels) {
        Relation rel = (Relation)lfirst(cell);
        Oid heap_relid;
        Oid toast_relid;
        bool is_shared = rel->rd_rel->relisshared;
        /*
         * This effectively deletes all rows in the table, and may be done
         * in a serializable transaction.  In that case we must record a
         * rw-conflict in to this transaction from each transaction
         * holding a predicate lock on the table.
         */
        CheckTableForSerializableConflictIn(rel);

        if (RELATION_IS_GLOBAL_TEMP(rel) && !gtt_storage_attached(RelationGetRelid(rel))) {
            continue;
        }

        if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE && isMOTFromTblOid(RelationGetRelid(rel))) {
            FdwRoutine* fdwroutine = GetFdwRoutineByRelId(RelationGetRelid(rel));
            if (fdwroutine->TruncateForeignTable != NULL) {
                fdwroutine->TruncateForeignTable(stmt, rel);
            }
		} else if (!RELATION_IS_PARTITIONED(rel)) {
            /*
			 * non partitioned table
             * Need the full transaction-safe pushups.
             *
             * Create a new empty storage file for the relation, and assign it
             * as the relfilenode value. The old storage file is scheduled for
             * deletion at commit.
             */
            if (RelationIsPAXFormat(rel)) {
                /*
                 * For dfs table truncation, we need to get the current dfs files,
                 * and write them into a file. We will delete the correspoinding files
                 * when committed. The dfs files will be deleted on DN.
                 */
                isDfsTruncate = true;

                if (!IS_PGXC_COORDINATOR) {
                    DFSDescHandler* handler =
                        New(CurrentMemoryContext) DFSDescHandler(MAX_LOADED_DFSDESC, rel->rd_att->natts, rel);
                    /*
                     * Before truncation, delete the garbage file first.
                     */
                    RemoveGarbageFiles(rel, handler);
                    SaveDfsFilelist(rel, handler);
                    delete (handler);
                }
            }

            RelationSetNewRelfilenode(rel, u_sess->utils_cxt.RecentXmin, isDfsTruncate);

            if (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
                heap_create_init_fork(rel);

            heap_relid = RelationGetRelid(rel);
            toast_relid = rel->rd_rel->reltoastrelid;

            /*
             * The same for the toast table, if any.
             */
            if (OidIsValid(toast_relid)) {
                rel = relation_open(toast_relid, AccessExclusiveLock);
                RelationSetNewRelfilenode(rel, u_sess->utils_cxt.RecentXmin);
                if (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
                    heap_create_init_fork(rel);
                heap_close(rel, NoLock);
            }

            /* report truncate to PgStatCollector */
            pgstat_report_truncate(heap_relid, InvalidOid, is_shared);

            /*
             * Reconstruct the indexes to match, and we're done.
             */
            (void)reindex_relation(heap_relid, REINDEX_REL_PROCESS_TOAST, REINDEX_ALL_INDEX);
        } else {
            /* truncate partitioned table */
            List* partTupleList = NIL;
            ListCell* partCell = NULL;

            heap_relid = RelationGetRelid(rel);
            /* partitioned table unspport the unlogged table */
            Assert(rel->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED);

            /* process all partition and toast */
            partTupleList = searchPgPartitionByParentId(PART_OBJ_TYPE_TABLE_PARTITION, rel->rd_id);
            foreach (partCell, partTupleList) {
                HeapTuple tup = (HeapTuple)lfirst(partCell);
                Oid toastOid = ((Form_pg_partition)GETSTRUCT(tup))->reltoastrelid;
                Relation toastRel = NULL;

                Oid partOid = HeapTupleGetOid(tup);
                Partition p = partitionOpen(rel, partOid, AccessExclusiveLock);
                PartitionSetNewRelfilenode(rel, p, u_sess->utils_cxt.RecentXmin);

                /* process the toast table */
                if (OidIsValid(toastOid)) {
                    Assert(rel->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED);
                    toastRel = heap_open(toastOid, AccessExclusiveLock);
                    RelationSetNewRelfilenode(toastRel, u_sess->utils_cxt.RecentXmin);
                    heap_close(toastRel, NoLock);
                }
                partitionClose(rel, p, NoLock);

                /* report truncate partition to PgStatCollector */
                pgstat_report_truncate(partOid, heap_relid, is_shared);
            }
            RelationSetNewRelfilenode(rel, u_sess->utils_cxt.RecentXmin, isDfsTruncate);
            freePartList(partTupleList);
            pgstat_report_truncate(
                heap_relid, InvalidOid, is_shared); /* report truncate partitioned table to PgStatCollector */

            /* process all index */
            (void)reindex_relation(heap_relid, REINDEX_REL_PROCESS_TOAST, REINDEX_ALL_INDEX);
        }
    }

    /*
     * Restart owned sequences if we were asked to.
     */
    foreach (cell, seq_relids) {
        Oid seq_relid = lfirst_oid(cell);

        ResetSequence(seq_relid);
    }

    /*
     * Process all AFTER STATEMENT TRUNCATE triggers.
     */
    resultRelInfo = resultRelInfos;
    foreach (cell, rels) {
        estate->es_result_relation_info = resultRelInfo;
        ExecASTruncateTriggers(estate, resultRelInfo);
        resultRelInfo++;
    }

#ifdef ENABLE_MULTIPLE_NODES
    /*
     * In Postgres-XC, TRUNCATE needs to be launched to remote nodes before the
     * AFTER triggers are launched. This insures that the triggers are being fired
     * by correct events.
     */
    if (IS_PGXC_COORDINATOR && !IsConnFromCoord()) {
        if (u_sess->attr.attr_sql.enable_parallel_ddl && !isFirstNode) {
            bool is_temp = false;
            RemoteQuery* step = makeNode(RemoteQuery);
            ExecNodes* exec_nodes = NULL;

            /* Check un-allowed case where truncate tables from different node groups */
            if (!ObjectsInSameNodeGroup(stmt->relations, T_TruncateStmt)) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("NOT-SUPPORT: Not support TRUNCATE multiple objects different nodegroup")));
            }

            foreach (cell, stmt->relations) {
                Oid relid;
                RangeVar* rel = (RangeVar*)lfirst(cell);

                relid = RangeVarGetRelid(rel, NoLock, false);

                if (exec_nodes == NULL) {
                    exec_nodes = RelidGetExecNodes(relid);
                }

                if (IsTempTable(relid)) {
                    is_temp = true;
                    break;
                }
            }

            step->combine_type = COMBINE_TYPE_SAME;
            step->exec_nodes = exec_nodes;
            step->sql_statement = pstrdup(sql_statement);
            step->force_autocommit = false;
            step->exec_type = EXEC_ON_DATANODES;
            step->is_temp = is_temp;
            ExecRemoteUtility_ParallelDDLMode(step, FirstExecNode);
            pfree_ext(step->sql_statement);
            pfree_ext(step);
        } else {
            bool is_temp = false;
            RemoteQuery* step = makeNode(RemoteQuery);
            ExecNodes* exec_nodes = NULL;

            /* Check un-allowed case where truncate tables from different node groups */
            if (!ObjectsInSameNodeGroup(stmt->relations, T_TruncateStmt)) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("NOT-SUPPORT: Not support TRUNCATE multiple objects different nodegroup")));
            }

            foreach (cell, stmt->relations) {
                Oid relid;
                RangeVar* rel = (RangeVar*)lfirst(cell);

                relid = RangeVarGetRelid(rel, NoLock, false);

                if (exec_nodes == NULL) {
                    exec_nodes = RelidGetExecNodes(relid);
                }

                if (IsTempTable(relid)) {
                    is_temp = true;
                    break;
                }
            }

            step->combine_type = COMBINE_TYPE_SAME;
            step->exec_nodes = exec_nodes;
            step->sql_statement = pstrdup(sql_statement);
            step->force_autocommit = false;
            step->exec_type = is_temp ? EXEC_ON_DATANODES : EXEC_ON_ALL_NODES;
            step->is_temp = is_temp;
            ExecRemoteUtility(step);
            pfree_ext(step->sql_statement);
            pfree_ext(step);
        }
    }
#endif

    /* Handle queued AFTER triggers */
    AfterTriggerEndQuery(estate);

#ifdef PGXC
    /* Need to reset start and end ctid of rel in rels_in_redis list */
    if (IS_PGXC_DATANODE) {
        foreach (cell, rels_in_redis) {
            Relation rel = (Relation)lfirst(cell);

            if (!RELATION_IS_PARTITIONED(rel)) {
                ResetRelRedisCtidRelOptions(
                    rel, InvalidOid, RELOID, Natts_pg_class, Anum_pg_class_reloptions, RelationRelationId);
            } else {
                ResetPartsRedisCtidRelOptions(rel);
            }
            elog(LOG, "reset redis rel %s start and end ctid.", RelationGetRelationName(rel));
        }
    }
#endif

    /* We can clean up the EState now */
    FreeExecutorState(estate);

    /* And close the rels (can't do this while EState still holds refs) */
    foreach (cell, rels) {
        Relation rel = (Relation)lfirst(cell);

        /* Recode time of truancate relation. */
        recordRelationMTime(rel->rd_id, rel->rd_rel->relkind);

        heap_close(rel, NoLock);
    }
}

/*
 * Check that a given rel is safe to truncate.	Subroutine for ExecuteTruncate
 */
static void truncate_check_rel(Relation rel)
{
    AclResult aclresult;

    /* Only allow truncate on regular tables */
    /* @hdfs
     * Add error msg for a foreign table
     */
    if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE) {
        if (!isMOTFromTblOid(RelationGetRelid(rel)))
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("It is not supported to truncate foreign table \"%s\".", RelationGetRelationName(rel))));
    } else if (rel->rd_rel->relkind != RELKIND_RELATION) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("It is not supported to truncate non-table \"%s\"", RelationGetRelationName(rel))));
    }

    /* Permissions checks */
    aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(), ACL_TRUNCATE);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error(aclresult, ACL_KIND_CLASS, RelationGetRelationName(rel));

    if (!g_instance.attr.attr_common.allowSystemTableMods && !u_sess->attr.attr_common.IsInplaceUpgrade &&
        IsSystemRelation(rel) && !WLMRelationCanTruncate(rel))
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                errmsg("permission denied: \"%s\" is a system catalog", RelationGetRelationName(rel))));

    /*
     * Don't allow truncate on temp tables of other backends ... their local
     * buffer manager is not going to cope.
     */
    if (RELATION_IS_OTHER_TEMP(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot truncate temporary tables of other sessions")));
    }

    /*
     * Also check for active uses of the relation in the current transaction,
     * including open scans and pending AFTER trigger events.
     */
    CheckTableNotInUse(rel, "TRUNCATE");
}

/*
 * storage_name
 *	  returns the name corresponding to a typstorage/attstorage enum value
 */
static const char* storage_name(char c)
{
    switch (c) {
        case 'p':
            return "PLAIN";
        case 'm':
            return "MAIN";
        case 'x':
            return "EXTENDED";
        case 'e':
            return "EXTERNAL";
        default:
            return "???";
    }
}

/* ----------
 * MergeAttributes
 *		Returns new schema given initial schema and superclasses.
 *
 * Input arguments:
 * 'schema' is the column/attribute definition for the table. (It's a list
 *		of ColumnDef's.) It is destructively changed.
 * 'supers' is a list of names (as RangeVar nodes) of parent relations.
 * 'relpersistence' is a persistence type of the table.
 *
 * Output arguments:
 * 'supOids' receives a list of the OIDs of the parent relations.
 * 'supconstr' receives a list of constraints belonging to the parents,
 *		updated as necessary to be valid for the child.
 * 'supOidCount' is set to the number of parents that have OID columns.
 *
 * Return value:
 * Completed schema list.
 *
 * Notes:
 *	  The order in which the attributes are inherited is very important.
 *	  Intuitively, the inherited attributes should come first. If a table
 *	  inherits from multiple parents, the order of those attributes are
 *	  according to the order of the parents specified in CREATE TABLE.
 *
 *	  Here's an example:
 *
 *		create table person (name text, age int4, location point);
 *		create table emp (salary int4, manager text) inherits(person);
 *		create table student (gpa float8) inherits (person);
 *		create table stud_emp (percent int4) inherits (emp, student);
 *
 *	  The order of the attributes of stud_emp is:
 *
 *							person {1:name, 2:age, 3:location}
 *							/	 \
 *			   {6:gpa}	student   emp {4:salary, 5:manager}
 *							\	 /
 *						   stud_emp {7:percent}
 *
 *	   If the same attribute name appears multiple times, then it appears
 *	   in the result table in the proper location for its first appearance.
 *
 *	   Constraints (including NOT NULL constraints) for the child table
 *	   are the union of all relevant constraints, from both the child schema
 *	   and parent tables.
 *
 *	   The default value for a child column is defined as:
 *		(1) If the child schema specifies a default, that value is used.
 *		(2) If neither the child nor any parent specifies a default, then
 *			the column will not have a default.
 *		(3) If conflicting defaults are inherited from different parents
 *			(and not overridden by the child), an error is raised.
 *		(4) Otherwise the inherited default is used.
 *		Rule (3) is new in Postgres 7.1; in earlier releases you got a
 *		rather arbitrary choice of which parent default to use.
 * ----------
 */
static List* MergeAttributes(
    List* schema, List* supers, char relpersistence, List** supOids, List** supconstr, int* supOidCount)
{
    ListCell* entry = NULL;
    List* inhSchema = NIL;
    List* parentOids = NIL;
    List* constraints = NIL;
    int parentsWithOids = 0;
    bool have_bogus_defaults = false;
    int child_attno;

    /*
     * Check for and reject tables with too many columns. We perform this
     * check relatively early for two reasons: (a) we don't run the risk of
     * overflowing an AttrNumber in subsequent code (b) an O(n^2) algorithm is
     * okay if we're processing <= 1600 columns, but could take minutes to
     * execute if the user attempts to create a table with hundreds of
     * thousands of columns.
     *
     * Note that we also need to check that any we do not exceed this figure
     * after including columns from inherited relations.
     */
    if (list_length(schema) > MaxHeapAttributeNumber) {
        ereport(ERROR,
            (errcode(ERRCODE_TOO_MANY_COLUMNS), errmsg("tables can have at most %d columns", MaxHeapAttributeNumber)));
    }

    /*
     * Check for duplicate names in the explicit list of attributes.
     *
     * Although we might consider merging such entries in the same way that we
     * handle name conflicts for inherited attributes, it seems to make more
     * sense to assume such conflicts are errors.
     */
    foreach (entry, schema) {
        ColumnDef* coldef = (ColumnDef*)lfirst(entry);
        ListCell* rest = lnext(entry);
        ListCell* prev = entry;
        if (u_sess->attr.attr_sql.enable_cluster_resize && coldef->dropped_attr != NULL) {
            continue;
        }
        if (coldef->typname == NULL) {

            /*
             * Typed table column option that does not belong to a column from
             * the type.  This works because the columns from the type come
             * first in the list.
             */
            ereport(
                ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("column \"%s\" does not exist", coldef->colname)));
        }

        while (rest != NULL) {
            ColumnDef* restdef = (ColumnDef*)lfirst(rest);
            ListCell* next = lnext(rest); /* need to save it in case we delete it */
            if (strcmp(coldef->colname, restdef->colname) == 0) {
                if (coldef->is_from_type) {
                    /*
                     * merge the column options into the column from the type
                     */
                    coldef->is_not_null = restdef->is_not_null;
                    coldef->raw_default = restdef->raw_default;
                    coldef->cooked_default = restdef->cooked_default;
                    coldef->constraints = restdef->constraints;
                    coldef->is_from_type = false;
                    coldef->kvtype = restdef->kvtype;
                    coldef->cmprs_mode = restdef->cmprs_mode;
                    list_delete_cell(schema, rest, prev);
                } else {
                    ereport(ERROR,
                        (errcode(ERRCODE_DUPLICATE_COLUMN),
                            errmsg("column \"%s\" specified more than once", coldef->colname)));
                }
            }
            prev = rest;
            rest = next;
        }
    }

    /*
     * Scan the parents left-to-right, and merge their attributes to form a
     * list of inherited attributes (inhSchema).  Also check to see if we need
     * to inherit an OID column.
     */
    child_attno = 0;
    foreach (entry, supers) {
        RangeVar* parent = (RangeVar*)lfirst(entry);
        Relation relation;
        TupleDesc tupleDesc;
        TupleConstr* constr = NULL;
        AttrNumber* newattno = NULL;
        AttrNumber parent_attno;

        /*
         * A self-exclusive lock is needed here.  If two backends attempt to
         * add children to the same parent simultaneously, and that parent has
         * no pre-existing children, then both will attempt to update the
         * parent's relhassubclass field, leading to a "tuple concurrently
         * updated" error.  Also, this interlocks against a concurrent ANALYZE
         * on the parent table, which might otherwise be attempting to clear
         * the parent's relhassubclass field, if its previous children were
         * recently dropped.
         */
        relation = heap_openrv(parent, ShareUpdateExclusiveLock);

        if (relation->rd_rel->relkind != RELKIND_RELATION) {
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("inherited relation \"%s\" is not a table", parent->relname)));
        }

        /* Permanent rels cannot inherit from temporary ones */
        if (relpersistence != RELPERSISTENCE_TEMP && relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP) {
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("cannot inherit from temporary relation \"%s\"", parent->relname)));
        }

        /* If existing rel is temp, it must belong to this session */
        if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP && !RelationIsLocalTemp(relation)) {
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("cannot inherit from temporary relation of another session")));
        }

        /*
         * We should have an UNDER permission flag for this, but for now,
         * demand that creator of a child table own the parent.
         */
        if (!pg_class_ownercheck(RelationGetRelid(relation), GetUserId())) {
            aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, RelationGetRelationName(relation));
        }

        /*
         * Reject duplications in the list of parents.
         */
        if (list_member_oid(parentOids, RelationGetRelid(relation))) {
            ereport(ERROR,
                (errcode(ERRCODE_DUPLICATE_TABLE),
                    errmsg("relation \"%s\" would be inherited from more than once", parent->relname)));
        }

        parentOids = lappend_oid(parentOids, RelationGetRelid(relation));

        if (relation->rd_rel->relhasoids) {
            parentsWithOids++;
        }

        tupleDesc = RelationGetDescr(relation);
        constr = tupleDesc->constr;

        /*
         * newattno[] will contain the child-table attribute numbers for the
         * attributes of this parent table.  (They are not the same for
         * parents after the first one, nor if we have dropped columns.)
         */
        newattno = (AttrNumber*)palloc0(tupleDesc->natts * sizeof(AttrNumber));

        for (parent_attno = 1; parent_attno <= tupleDesc->natts; parent_attno++) {
            Form_pg_attribute attribute = tupleDesc->attrs[parent_attno - 1];
            char* attributeName = NameStr(attribute->attname);
            int exist_attno;
            ColumnDef* def = NULL;

            /*
             * Ignore dropped columns in the parent.
             */
            if (attribute->attisdropped) {
                continue; /* leave newattno entry as zero */
            }

            /*
             * Does it conflict with some previously inherited column?
             */
            exist_attno = findAttrByName(attributeName, inhSchema);
            if (exist_attno > 0) {
                Oid defTypeId;
                int32 deftypmod;
                Oid defCollId;

                /*
                 * Yes, try to merge the two column definitions. They must
                 * have the same type, typmod, and collation.
                 */
                ereport(NOTICE, (errmsg("merging multiple inherited definitions of column \"%s\"", attributeName)));
                def = (ColumnDef*)list_nth(inhSchema, exist_attno - 1);
                typenameTypeIdAndMod(NULL, def->typname, &defTypeId, &deftypmod);
                if (defTypeId != attribute->atttypid || deftypmod != attribute->atttypmod) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                            errmsg("inherited column \"%s\" has a type conflict", attributeName),
                            errdetail(
                                "%s versus %s", TypeNameToString(def->typname), format_type_be(attribute->atttypid))));
                }
                defCollId = GetColumnDefCollation(NULL, def, defTypeId);
                if (defCollId != attribute->attcollation) {
                    ereport(ERROR,
                        (errcode(ERRCODE_COLLATION_MISMATCH),
                            errmsg("inherited column \"%s\" has a collation conflict", attributeName),
                            errdetail("\"%s\" versus \"%s\"",
                                get_collation_name(defCollId),
                                get_collation_name(attribute->attcollation))));
                }

                /* Copy storage parameter */
                if (def->storage == 0) {
                    def->storage = attribute->attstorage;
                } else if (def->storage != attribute->attstorage) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                            errmsg("inherited column \"%s\" has a storage parameter conflict", attributeName),
                            errdetail(
                                "%s versus %s", storage_name(def->storage), storage_name(attribute->attstorage))));
                }

                def->inhcount++;
                /* Merge of NOT NULL constraints = OR 'em together */
                if (attribute->attnotnull) {
                    def->is_not_null = true;
                }
                if (def->kvtype == ATT_KV_UNDEFINED) {
                    def->kvtype = attribute->attkvtype;
                }
                if (def->cmprs_mode == ATT_CMPR_UNDEFINED) {
                    def->cmprs_mode = attribute->attcmprmode;
                }   
                /* Default and other constraints are handled below */
                newattno[parent_attno - 1] = exist_attno;
            } else {
                /*
                 * No, create a new inherited column
                 */
                def = makeNode(ColumnDef);
                def->colname = pstrdup(attributeName);
                def->typname = makeTypeNameFromOid(attribute->atttypid, attribute->atttypmod);
                def->inhcount = 1;
                def->is_local = false;
                def->is_not_null = attribute->attnotnull;
                def->is_from_type = false;
                def->storage = attribute->attstorage;
                def->kvtype = attribute->attkvtype;
                def->cmprs_mode = attribute->attcmprmode;
                def->raw_default = NULL;
                def->cooked_default = NULL;
                def->collClause = NULL;
                def->collOid = attribute->attcollation;
                def->constraints = NIL;
                inhSchema = lappend(inhSchema, def);
                newattno[parent_attno - 1] = ++child_attno;
            }

            /*
             * Copy default if any
             */
            if (attribute->atthasdef) {
                Node* this_default = NULL;
                AttrDefault* attrdef = NULL;
                int i;

                /* Find default in constraint structure */
                Assert(constr != NULL);
                attrdef = constr->defval;
                for (i = 0; i < constr->num_defval; i++) {
                    if (attrdef[i].adnum == parent_attno) {
                        this_default = (Node*)stringToNode_skip_extern_fields(attrdef[i].adbin);
                        break;
                    }
                }
                Assert(this_default != NULL);

                /*
                 * If default expr could contain any vars, we'd need to fix
                 * 'em, but it can't; so default is ready to apply to child.
                 *
                 * If we already had a default from some prior parent, check
                 * to see if they are the same.  If so, no problem; if not,
                 * mark the column as having a bogus default. Below, we will
                 * complain if the bogus default isn't overridden by the child
                 * schema.
                 */
                Assert(def->raw_default == NULL);
                if (def->cooked_default == NULL)
                    def->cooked_default = this_default;
                else if (!equal(def->cooked_default, this_default)) {
                    def->cooked_default = &u_sess->cmd_cxt.bogus_marker;
                    have_bogus_defaults = true;
                }
            }
        }

        /*
         * Now copy the CHECK constraints of this parent, adjusting attnos
         * using the completed newattno[] map.	Identically named constraints
         * are merged if possible, else we throw error.
         */
        if (constr != NULL && constr->num_check > 0) {
            ConstrCheck* check = constr->check;
            int i;

            for (i = 0; i < constr->num_check; i++) {
                char* name = check[i].ccname;
                Node* expr = NULL;
                bool found_whole_row = false;

                /* ignore if the constraint is non-inheritable */
                if (check[i].ccnoinherit)
                    continue;

                /* Adjust Vars to match new table's column numbering */
                expr = map_variable_attnos(
                    (Node*)stringToNode(check[i].ccbin), 1, 0, newattno, tupleDesc->natts, &found_whole_row);

                /*
                 * For the moment we have to reject whole-row variables.
                 * We could convert them, if we knew the new table's rowtype
                 * OID, but that hasn't been assigned yet.
                 */
                if (found_whole_row)
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("cannot convert whole-row table reference"),
                            errdetail("Constraint \"%s\" contains a whole-row reference to table \"%s\".",
                                name,
                                RelationGetRelationName(relation))));

                /* check for duplicate */
                if (!MergeCheckConstraint(constraints, name, expr)) {
                    /* nope, this is a new one */
                    CookedConstraint* cooked = NULL;

                    cooked = (CookedConstraint*)palloc(sizeof(CookedConstraint));
                    cooked->contype = CONSTR_CHECK;
                    cooked->name = pstrdup(name);
                    cooked->attnum = 0; /* not used for constraints */
                    cooked->expr = expr;
                    cooked->skip_validation = false;
                    cooked->is_local = false;
                    cooked->inhcount = 1;
                    cooked->is_no_inherit = false;
                    constraints = lappend(constraints, cooked);
                }
            }
        }

        pfree_ext(newattno);

        /*
         * Close the parent rel, but keep our AccessShareLock on it until xact
         * commit.	That will prevent someone else from deleting or ALTERing
         * the parent before the child is committed.
         */
        heap_close(relation, NoLock);
    }

    /*
     * If we had no inherited attributes, the result schema is just the
     * explicitly declared columns.  Otherwise, we need to merge the declared
     * columns into the inherited schema list.
     */
    if (inhSchema != NIL) {
        foreach (entry, schema) {
            ColumnDef* newdef = (ColumnDef*)lfirst(entry);
            char* attributeName = newdef->colname;
            int exist_attno;

            /*
             * Does it conflict with some previously inherited column?
             */
            exist_attno = findAttrByName(attributeName, inhSchema);
            if (exist_attno > 0) {
                Oid defTypeId, newTypeId;
                int32 deftypmod, newtypmod;
                Oid defcollid, newcollid;

                /*
                 * Yes, try to merge the two column definitions. They must
                 * have the same type, typmod, and collation.
                 */
                ereport(LOG,
                    (errmsg("%s: merging column \"%s\" with inherited definition",
                        g_instance.attr.attr_common.PGXCNodeName,
                        attributeName)));
                ereport(NOTICE, (errmsg("merging column \"%s\" with inherited definition", attributeName)));
                ColumnDef* def = (ColumnDef*)list_nth(inhSchema, exist_attno - 1);
                typenameTypeIdAndMod(NULL, def->typname, &defTypeId, &deftypmod);
                typenameTypeIdAndMod(NULL, newdef->typname, &newTypeId, &newtypmod);
                if (defTypeId != newTypeId || deftypmod != newtypmod)
                    ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                            errmsg("column \"%s\" has a type conflict", attributeName),
                            errdetail(
                                "%s versus %s", TypeNameToString(def->typname), TypeNameToString(newdef->typname))));
                defcollid = GetColumnDefCollation(NULL, def, defTypeId);
                newcollid = GetColumnDefCollation(NULL, newdef, newTypeId);
                if (defcollid != newcollid)
                    ereport(ERROR,
                        (errcode(ERRCODE_COLLATION_MISMATCH),
                            errmsg("column \"%s\" has a collation conflict", attributeName),
                            errdetail(
                                "\"%s\" versus \"%s\"", get_collation_name(defcollid), get_collation_name(newcollid))));

                /* Copy storage parameter */
                if (def->storage == 0)
                    def->storage = newdef->storage;
                else if (newdef->storage != 0 && def->storage != newdef->storage)
                    ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                            errmsg("column \"%s\" has a storage parameter conflict", attributeName),
                            errdetail("%s versus %s", storage_name(def->storage), storage_name(newdef->storage))));

                /* Mark the column as locally defined */
                def->is_local = true;
                /* Merge of NOT NULL constraints = OR 'em together */
                if (newdef->is_not_null)
                    def->is_not_null = true;
                /* Copy kv type parameter */
                if (def->kvtype == ATT_KV_UNDEFINED) {
                    def->kvtype = newdef->storage;
                } else if (newdef->kvtype != ATT_KV_UNDEFINED && def->kvtype != newdef->kvtype) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                            errmsg("column \"%s\" has a kvtype parameter conflict", attributeName),
                            errdetail("%s versus %s", storage_name(def->kvtype), storage_name(newdef->kvtype))));
                }
                if (def->cmprs_mode == ATT_CMPR_UNDEFINED)
                    def->cmprs_mode = newdef->cmprs_mode;
                /* If new def has a default, override previous default */
                if (newdef->raw_default != NULL) {
                    def->raw_default = newdef->raw_default;
                    def->cooked_default = newdef->cooked_default;
                }
            } else {
                /*
                 * No, attach new column to result schema
                 */
                inhSchema = lappend(inhSchema, newdef);
            }
        }

        schema = inhSchema;

        /*
         * Check that we haven't exceeded the legal # of columns after merging
         * in inherited columns.
         */
        if (list_length(schema) > MaxHeapAttributeNumber)
            ereport(ERROR,
                (errcode(ERRCODE_TOO_MANY_COLUMNS),
                    errmsg("tables can have at most %d columns", MaxHeapAttributeNumber)));
    }

    /*
     * If we found any conflicting parent default values, check to make sure
     * they were overridden by the child.
     */
    if (have_bogus_defaults) {
        foreach (entry, schema) {
            ColumnDef* def = (ColumnDef*)lfirst(entry);

            if (def->cooked_default == &u_sess->cmd_cxt.bogus_marker)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
                        errmsg("column \"%s\" inherits conflicting default values", def->colname),
                        errhint("To resolve the conflict, specify a default explicitly.")));
        }
    }

    *supOids = parentOids;
    *supconstr = constraints;
    *supOidCount = parentsWithOids;
    return schema;
}

/*
 * MergeCheckConstraint
 *		Try to merge an inherited CHECK constraint with previous ones
 *
 * If we inherit identically-named constraints from multiple parents, we must
 * merge them, or throw an error if they don't have identical definitions.
 *
 * constraints is a list of CookedConstraint structs for previous constraints.
 *
 * Returns TRUE if merged (constraint is a duplicate), or FALSE if it's
 * got a so-far-unique name, or throws error if conflict.
 */
static bool MergeCheckConstraint(List* constraints, char* name, Node* expr)
{
    ListCell* lc = NULL;

    foreach (lc, constraints) {
        CookedConstraint* ccon = (CookedConstraint*)lfirst(lc);

        Assert(ccon->contype == CONSTR_CHECK);

        /* Non-matching names never conflict */
        if (strcmp(ccon->name, name) != 0)
            continue;

        if (equal(expr, ccon->expr)) {
            /* OK to merge */
            ccon->inhcount++;
            return true;
        }

        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_OBJECT),
                errmsg("check constraint name \"%s\" appears multiple times but with different expressions", name)));
    }

    return false;
}

/*
 * StoreCatalogInheritance
 *		Updates the system catalogs with proper inheritance information.
 *
 * supers is a list of the OIDs of the new relation's direct ancestors.
 */
static void StoreCatalogInheritance(Oid relationId, List* supers)
{
    Relation relation;
    int16 seqNumber;
    ListCell* entry = NULL;

    /*
     * sanity checks
     */
    AssertArg(OidIsValid(relationId));

    if (supers == NIL)
        return;

    /*
     * Store INHERITS information in pg_inherits using direct ancestors only.
     * Also enter dependencies on the direct ancestors, and make sure they are
     * marked with relhassubclass = true.
     *
     * (Once upon a time, both direct and indirect ancestors were found here
     * and then entered into pg_ipl.  Since that catalog doesn't exist
     * anymore, there's no need to look for indirect ancestors.)
     */
    relation = heap_open(InheritsRelationId, RowExclusiveLock);

    seqNumber = 1;
    foreach (entry, supers) {
        Oid parentOid = lfirst_oid(entry);

        StoreCatalogInheritance1(relationId, parentOid, seqNumber, relation);
        seqNumber++;
    }

    heap_close(relation, RowExclusiveLock);
}

/*
 * Make catalog entries showing relationId as being an inheritance child
 * of parentOid.  inhRelation is the already-opened pg_inherits catalog.
 */
static void StoreCatalogInheritance1(Oid relationId, Oid parentOid, int16 seqNumber, Relation inhRelation)
{
    TupleDesc desc = RelationGetDescr(inhRelation);
    Datum values[Natts_pg_inherits];
    bool nulls[Natts_pg_inherits];
    ObjectAddress childobject, parentobject;
    HeapTuple tuple;
    errno_t rc = EOK;

    /*
     * Make the pg_inherits entry
     */
    values[Anum_pg_inherits_inhrelid - 1] = ObjectIdGetDatum(relationId);
    values[Anum_pg_inherits_inhparent - 1] = ObjectIdGetDatum(parentOid);
    values[Anum_pg_inherits_inhseqno - 1] = Int16GetDatum(seqNumber);

    rc = memset_s(nulls, sizeof(nulls), 0, sizeof(nulls));
    securec_check(rc, "\0", "\0");

    tuple = heap_form_tuple(desc, values, nulls);

    (void)simple_heap_insert(inhRelation, tuple);

    CatalogUpdateIndexes(inhRelation, tuple);

    heap_freetuple_ext(tuple);

    /*
     * Store a dependency too
     */
    parentobject.classId = RelationRelationId;
    parentobject.objectId = parentOid;
    parentobject.objectSubId = 0;
    childobject.classId = RelationRelationId;
    childobject.objectId = relationId;
    childobject.objectSubId = 0;

    recordDependencyOn(&childobject, &parentobject, DEPENDENCY_NORMAL);

    /*
     * Mark the parent as having subclasses.
     */
    SetRelationHasSubclass(parentOid, true);
}

/*
 * Look for an existing schema entry with the given name.
 *
 * Returns the index (starting with 1) if attribute already exists in schema,
 * 0 if it doesn't.
 */
static int findAttrByName(const char* attributeName, List* schema)
{
    ListCell* s = NULL;
    int i = 1;

    foreach (s, schema) {
        ColumnDef* def = (ColumnDef*)lfirst(s);

        if (strcmp(attributeName, def->colname) == 0)
            return i;

        i++;
    }
    return 0;
}

/*
 * SetRelationHasSubclass
 *		Set the value of the relation's relhassubclass field in pg_class.
 *
 * NOTE: caller must be holding an appropriate lock on the relation.
 * ShareUpdateExclusiveLock is sufficient.
 *
 * NOTE: an important side-effect of this operation is that an SI invalidation
 * message is sent out to all backends --- including me --- causing plans
 * referencing the relation to be rebuilt with the new list of children.
 * This must happen even if we find that no change is needed in the pg_class
 * row.
 */
void SetRelationHasSubclass(Oid relationId, bool relhassubclass)
{
    Relation relationRelation;
    HeapTuple tuple;
    Form_pg_class classtuple;

    /*
     * Fetch a modifiable copy of the tuple, modify it, update pg_class.
     */
    relationRelation = heap_open(RelationRelationId, RowExclusiveLock);
    tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relationId));
    if (!HeapTupleIsValid(tuple)) {
        ereport(
            ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", relationId)));
    }

    classtuple = (Form_pg_class)GETSTRUCT(tuple);
    if (classtuple->relhassubclass != relhassubclass) {
        classtuple->relhassubclass = relhassubclass;
        simple_heap_update(relationRelation, &tuple->t_self, tuple);

        /* keep the catalog indexes up to date */
        CatalogUpdateIndexes(relationRelation, tuple);
    } else {
        /* no need to change tuple, but force relcache rebuild anyway */
        CacheInvalidateRelcacheByTuple(tuple);
    }

    heap_freetuple_ext(tuple);
    heap_close(relationRelation, RowExclusiveLock);
}

/*
 *		renameatt_check			- basic sanity checks before attribute rename
 */
static void renameatt_check(Oid myrelid, Form_pg_class classform, bool recursing)
{
    char relkind = classform->relkind;

    if (classform->reloftype && !recursing) {
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("cannot rename column of typed table")));
    }

    /*
     * Renaming the columns of sequences or toast tables doesn't actually
     * break anything from the system's point of view, since internal
     * references are by attnum.  But it doesn't seem right to allow users to
     * change names that are hardcoded into the system, hence the following
     * restriction.
     */
    if (relkind != RELKIND_RELATION && relkind != RELKIND_VIEW && relkind != RELKIND_COMPOSITE_TYPE &&
        relkind != RELKIND_INDEX && relkind != RELKIND_FOREIGN_TABLE)
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("\"%s\" is not a table, view, composite type, index, or foreign table",
                    NameStr(classform->relname))));

    /*
     * permissions checking.  only the owner of a class can change its schema.
     */
    if (!pg_class_ownercheck(myrelid, GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, NameStr(classform->relname));
    if (!g_instance.attr.attr_common.allowSystemTableMods && !u_sess->attr.attr_common.IsInplaceUpgrade &&
        IsSystemClass(classform))
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                errmsg("permission denied: \"%s\" is a system catalog", NameStr(classform->relname))));
}

/*
 *		renameatt_internal		- workhorse for renameatt
 */
static void renameatt_internal(Oid myrelid, const char* oldattname, const char* newattname, bool recurse,
    bool recursing, int expected_parents, DropBehavior behavior)
{
    Relation targetrelation;
    Relation attrelation;
    HeapTuple atttup;
    Form_pg_attribute attform;
    int attnum;

    /*
     * Grab an exclusive lock on the target table, which we will NOT release
     * until end of transaction.
     */
    targetrelation = relation_open(myrelid, AccessExclusiveLock);
    renameatt_check(myrelid, RelationGetForm(targetrelation), recursing);

    /*
     * if the 'recurse' flag is set then we are supposed to rename this
     * attribute in all classes that inherit from 'relname' (as well as in
     * 'relname').
     *
     * any permissions or problems with duplicate attributes will cause the
     * whole transaction to abort, which is what we want -- all or nothing.
     */
    if (recurse) {
        List* child_oids = NIL;
        List* child_numparents = NIL;
        ListCell* lo = NULL;
        ListCell* li = NULL;

        /*
         * we need the number of parents for each child so that the recursive
         * calls to renameatt() can determine whether there are any parents
         * outside the inheritance hierarchy being processed.
         */
        child_oids = find_all_inheritors(myrelid, AccessExclusiveLock, &child_numparents);

        /*
         * find_all_inheritors does the recursive search of the inheritance
         * hierarchy, so all we have to do is process all of the relids in the
         * list that it returns.
         */
        forboth(lo, child_oids, li, child_numparents)
        {
            Oid childrelid = lfirst_oid(lo);
            int numparents = lfirst_int(li);

            if (childrelid == myrelid)
                continue;
            /* note we need not recurse again */
            renameatt_internal(childrelid, oldattname, newattname, false, true, numparents, behavior);
        }
    } else {
        /*
         * If we are told not to recurse, there had better not be any child
         * tables; else the rename would put them out of step.
         *
         * expected_parents will only be 0 if we are not already recursing.
         */
        if (expected_parents == 0 && find_inheritance_children(myrelid, NoLock) != NIL)
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                    errmsg("inherited column \"%s\" must be renamed in child tables too", oldattname)));
    }

    /* rename attributes in typed tables of composite type */
    if (targetrelation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE) {
        List* child_oids = NIL;
        ListCell* lo = NULL;

        child_oids = find_typed_table_dependencies(
            targetrelation->rd_rel->reltype, RelationGetRelationName(targetrelation), behavior);

        foreach (lo, child_oids)
            renameatt_internal(lfirst_oid(lo), oldattname, newattname, true, true, 0, behavior);
    }

    attrelation = heap_open(AttributeRelationId, RowExclusiveLock);
    atttup = SearchSysCacheCopyAttName(myrelid, oldattname);
    if (!HeapTupleIsValid(atttup)) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("column \"%s\" does not exist", oldattname)));
    }
    attform = (Form_pg_attribute)GETSTRUCT(atttup);
    attnum = attform->attnum;
    if (attnum <= 0) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot rename system column \"%s\"", oldattname)));
    }

    /*
     * if the attribute is inherited, forbid the renaming.	if this is a
     * top-level call to renameatt(), then expected_parents will be 0, so the
     * effect of this code will be to prohibit the renaming if the attribute
     * is inherited at all.  if this is a recursive call to renameatt(),
     * expected_parents will be the number of parents the current relation has
     * within the inheritance hierarchy being processed, so we'll prohibit the
     * renaming only if there are additional parents from elsewhere.
     */
    if (attform->attinhcount > expected_parents) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION), errmsg("cannot rename inherited column \"%s\"", oldattname)));
    }

    /* new name should not already exist */
    check_for_column_name_collision(targetrelation, newattname);

    /* apply the update */
    (void)namestrcpy(&(attform->attname), newattname);
    simple_heap_update(attrelation, &atttup->t_self, atttup);

    /* keep system catalog indexes current */
    CatalogUpdateIndexes(attrelation, atttup);
    heap_freetuple_ext(atttup);
    heap_close(attrelation, RowExclusiveLock);

    /* recurse rename cstore delta table column name */
    if (g_instance.attr.attr_storage.enable_delta_store && RelationIsCUFormat(targetrelation)) {
        List* child_oids = NIL;
        ListCell* child = NULL;
        child_oids = find_cstore_delta(targetrelation, AccessExclusiveLock);
        foreach (child, child_oids) {
            Oid childrelid = lfirst_oid(child);
            if (childrelid == myrelid)
                continue;
            renameatt_internal(childrelid, oldattname, newattname, false, true, 1, behavior);
        }
    }

    /* Recode time of reaname relation att. */
    recordRelationMTime(myrelid, targetrelation->rd_rel->relkind);
    relation_close(targetrelation, NoLock); /* close rel but keep lock */
}

/*
 * Perform permissions and integrity checks before acquiring a relation lock.
 */
static void RangeVarCallbackForRenameAttribute(
    const RangeVar* rv, Oid relid, Oid oldrelid, bool target_is_partition, void* arg)
{
    HeapTuple tuple;
    Form_pg_class form;

    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(tuple))
        return; /* concurrently dropped */
    form = (Form_pg_class)GETSTRUCT(tuple);
    renameatt_check(relid, form, false);
    ReleaseSysCache(tuple);
}

/*
 *		renameatt		- changes the name of a attribute in a relation
 */
void renameatt(RenameStmt* stmt)
{
    Oid relid;

    /* lock level taken here should match renameatt_internal */
    relid = RangeVarGetRelidExtended(stmt->relation,
        AccessExclusiveLock,
        stmt->missing_ok,
        false,
        false,
        false,
        RangeVarCallbackForRenameAttribute,
        NULL);

    if (!OidIsValid(relid)) {
        ereport(NOTICE, (errmsg("relation \"%s\" does not exist, skipping", stmt->relation->relname)));
        return;
    }

    // Check relations's internal mask
    Relation rel = relation_open(relid, AccessShareLock);
    if ((((uint32)RelationGetInternalMask(rel)) & INTERNAL_MASK_DALTER))
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Un-support feature"),
                errdetail("internal relation doesn't allow ALTER")));

    relation_close(rel, AccessShareLock);

    renameatt_internal(relid,
        stmt->subname,                              /* old att name */
        stmt->newname,                              /* new att name */
        interpretInhOption(stmt->relation->inhOpt), /* recursive? */
        false,                                      /* recursing? */
        0,                                          /* expected inhcount */
        stmt->behavior);
}

/*
 * same logic as renameatt_internal
 */
static void rename_constraint_internal(Oid myrelid, Oid mytypid, const char* oldconname, const char* newconname,
    bool recurse, bool recursing, int expected_parents)
{
    AssertArg(!myrelid || !mytypid);
    Relation targetrelation = NULL;
    Oid constraintOid;
    HeapTuple tuple;
    Form_pg_constraint con;

    if (mytypid) {
        constraintOid = get_domain_constraint_oid(mytypid, oldconname, false);
    } else {
        targetrelation = relation_open(myrelid, AccessExclusiveLock);
        /*
         * don't tell it whether we're recursing; we allow changing typed
         * tables here
         */
        renameatt_check(myrelid, RelationGetForm(targetrelation), false);
        constraintOid = get_relation_constraint_oid(myrelid, oldconname, false);
    }

    tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constraintOid));
    if (!HeapTupleIsValid(tuple)) {
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for constraint %u", constraintOid)));
    }
    con = (Form_pg_constraint)GETSTRUCT(tuple);
    if (myrelid && con->contype == CONSTRAINT_CHECK && !con->connoinherit) {
        if (recurse) {
            List* child_oids = NIL;
            List* child_numparents = NIL;
            ListCell* lo = NULL;
            ListCell* li = NULL;
            child_oids = find_all_inheritors(myrelid, AccessExclusiveLock, &child_numparents);
            forboth(lo, child_oids, li, child_numparents)
            {
                Oid childrelid = lfirst_oid(lo);
                int numparents = lfirst_int(li);
                if (childrelid == myrelid)
                    continue;

                rename_constraint_internal(childrelid, InvalidOid, oldconname, newconname, false, true, numparents);
            }
        } else {
            if (expected_parents == 0 && find_inheritance_children(myrelid, NoLock) != NIL)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg("inherited constraint \"%s\" must be renamed in child tables too", oldconname)));
        }

        if (con->coninhcount > expected_parents)
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                    errmsg("cannot rename inherited constraint \"%s\"", oldconname)));
    }

    if (con->conindid && (con->contype == CONSTRAINT_PRIMARY || con->contype == CONSTRAINT_UNIQUE ||
                             con->contype == CONSTRAINT_EXCLUSION))
        /* rename the index; this renames the constraint as well */
        RenameRelationInternal(con->conindid, newconname);
    else
        RenameConstraintById(constraintOid, newconname);

    ReleaseSysCache(tuple);
    if (targetrelation) {
        /* Recode time of rename relation constraint. */
        recordRelationMTime(targetrelation->rd_id, targetrelation->rd_rel->relkind);
        /* Invalidate relcache so as others can see the new constraint name. */
        CacheInvalidateRelcache(targetrelation);
        relation_close(targetrelation, NoLock); /* close rel but keep lock */
    }
}

void RenameConstraint(RenameStmt* stmt)
{
    Oid relid = InvalidOid;
    Oid typid = InvalidOid;

    if (stmt->relationType == OBJECT_DOMAIN) {
        Relation rel;
        HeapTuple tup;

        typid = typenameTypeId(NULL, makeTypeNameFromNameList(stmt->object));
        rel = heap_open(TypeRelationId, RowExclusiveLock);
        tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
        if (!HeapTupleIsValid(tup)) {
            ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for type %u", typid)));
        }
        checkDomainOwner(tup);
        ReleaseSysCache(tup);
        heap_close(rel, NoLock);
    } else {
        /* lock level taken here should match rename_constraint_internal */
        relid = RangeVarGetRelidExtended(stmt->relation,
            AccessExclusiveLock,
            stmt->missing_ok,
            false,
            false,
            false,
            RangeVarCallbackForRenameAttribute,
            NULL);
    }

    if (!OidIsValid(relid)) {
        ereport(NOTICE, (errmsg("relation \"%s\" does not exist, skipping", stmt->relation->relname)));
        return;
    }

    rename_constraint_internal(relid,
        typid,
        stmt->subname,
        stmt->newname,
        stmt->relation ? interpretInhOption(stmt->relation->inhOpt) : false, /* recursive? */
        false,                                                               /* recursing? */
        0 /* expected inhcount */);
}

/*
 * Execute ALTER TABLE/INDEX/SEQUENCE/VIEW/FOREIGN TABLE RENAME
 */
void RenameRelation(RenameStmt* stmt)
{
    Oid relid;

    /*
     * Grab an exclusive lock on the target table, index, sequence or view,
     * which we will NOT release until end of transaction.
     *
     * Lock level used here should match RenameRelationInternal, to avoid lock
     * escalation.
     */
    relid = RangeVarGetRelidExtended(stmt->relation,
        AccessExclusiveLock,
        stmt->missing_ok,
        false,
        false,
        false,
        RangeVarCallbackForAlterRelation,
        (void*)stmt);
    if (!OidIsValid(relid)) {
        ereport(NOTICE, (errmsg("relation \"%s\" does not exist, skipping", stmt->relation->relname)));
        return;
    }

    /* Do the work */
    RenameRelationInternal(relid, stmt->newname);
}

/*
 *		RenameRelationInternal - change the name of a relation
 *
 *		XXX - When renaming sequences, we don't bother to modify the
 *			  sequence name that is stored within the sequence itself
 *			  (this would cause problems with MVCC). In the future,
 *			  the sequence name should probably be removed from the
 *			  sequence, AFAIK there's no need for it to be there.
 */
void RenameRelationInternal(Oid myrelid, const char* newrelname)
{
    Relation targetrelation;
    Relation relrelation; /* for RELATION relation */
    HeapTuple reltup;
    Form_pg_class relform;
    Oid namespaceId;

    /*
     * Grab an exclusive lock on the target table, index, sequence or view,
     * which we will NOT release until end of transaction.
     */
    targetrelation = relation_open(myrelid, AccessExclusiveLock);

    if (targetrelation->rd_rel->reltype == OBJECT_SEQUENCE || targetrelation->rd_rel->relkind == RELKIND_SEQUENCE) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("RENAME SEQUENCE is not yet supported.")));
    }

    namespaceId = RelationGetNamespace(targetrelation);

    /*
     * Find relation's pg_class tuple, and make sure newrelname isn't in use.
     */
    relrelation = heap_open(RelationRelationId, RowExclusiveLock);

    reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(myrelid));
    if (!HeapTupleIsValid(reltup)) {
        /* shouldn't happen */
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", myrelid)));
    }

    relform = (Form_pg_class)GETSTRUCT(reltup);

    if (get_relname_relid(newrelname, namespaceId) != InvalidOid)
        ereport(ERROR, (errcode(ERRCODE_DUPLICATE_TABLE), errmsg("relation \"%s\" already exists", newrelname)));

    /*
     * Update pg_class tuple with new relname.	(Scribbling on reltup is OK
     * because it's a copy...)
     */
    (void)namestrcpy(&(relform->relname), newrelname);

    simple_heap_update(relrelation, &reltup->t_self, reltup);

    /* keep the system catalog indexes current */
    CatalogUpdateIndexes(relrelation, reltup);

    /* Recode time of rename relation. */
    recordRelationMTime(myrelid, targetrelation->rd_rel->relkind);

    /*
     * check rename's target is partitioned table
     */
    relform = (Form_pg_class)GETSTRUCT(reltup);
    if (relform->relkind == RELKIND_RELATION && relform->parttype == PARTTYPE_PARTITIONED_RELATION) {
        renamePartitionedTable(myrelid, newrelname);
    }

    heap_freetuple_ext(reltup);
    heap_close(relrelation, RowExclusiveLock);

    /*
     * Also rename the associated type, if any.
     */
    if (OidIsValid(targetrelation->rd_rel->reltype))
        RenameTypeInternal(targetrelation->rd_rel->reltype, newrelname, namespaceId);

    /*
     * Also rename the associated constraint, if any.
     */
    if (targetrelation->rd_rel->relkind == RELKIND_INDEX) {
        Oid constraintId = get_index_constraint(myrelid);
        if (OidIsValid(constraintId))
            RenameConstraintById(constraintId, newrelname);
    }

    /*
     * Close rel, but keep exclusive lock!
     */
    relation_close(targetrelation, NoLock);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: Change the name of partitioned table in pg_partition
 * Description	:
 * Notes		:
 */
void renamePartitionedTable(Oid partitionedTableOid, const char* partitionedTableNewName)
{
    Relation partitionRelRelation = NULL;
    HeapTuple partitionRelationTuple = NULL;
    Form_pg_partition relationForm = NULL;

    /* shouldn't happen */
    if (!OidIsValid(partitionedTableOid) || !PointerIsValid(partitionedTableNewName)) {
        ereport(ERROR, (errcode(ERRCODE_OPERATE_FAILED), errmsg("internal error, rename partitioned table failed")));
    }

    /*
     * Find relation's pg_partition tuple.
     */
    partitionRelRelation = relation_open(PartitionRelationId, RowExclusiveLock);
    partitionRelationTuple = searchPgPartitionByParentIdCopy(PART_OBJ_TYPE_PARTED_TABLE, partitionedTableOid);

    /* shouldn't happen */
    if (!HeapTupleIsValid(partitionRelationTuple)) {
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", partitionedTableOid)));
    }

    relationForm = (Form_pg_partition)GETSTRUCT(partitionRelationTuple);

    /*
     * Update pg_partition tuple with new relname.	(Scribbling on reltup is OK
     * because it's a copy...)
     */
    (void)namestrcpy(&(relationForm->relname), partitionedTableNewName);
    simple_heap_update(partitionRelRelation, &(partitionRelationTuple->t_self), partitionRelationTuple);

    /*
     * keep the system catalog indexes current
     */
    CatalogUpdateIndexes(partitionRelRelation, partitionRelationTuple);

    heap_freetuple_ext(partitionRelationTuple);

    relation_close(partitionRelRelation, RowExclusiveLock);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: Execute rename partition
 * Description	:
 * Notes		:
 */
void renamePartition(RenameStmt* stmt)
{
    Oid partitionedTableOid = InvalidOid;
    Oid partitionOid = InvalidOid;
    ParseState* pstate = NULL;
    RangePartitionDefState* rangePartDef = NULL;
    Relation rel = NULL;

    /* shouldn't happen */
    if (!PointerIsValid(stmt) || !PointerIsValid(stmt->newname)) {
        ereport(ERROR, (errcode(ERRCODE_OPERATE_FAILED), errmsg("internal error, rename partition failed")));
    }

    /* Get oid of target partitioned table.
     *
     * Grab a shared lock on the target partitioned table,
     * which we will NOT release until end of transaction.
     *
     * Lock level used here should match renamePartitonInternal, to avoid lock
     * escalation.
     */
    partitionedTableOid = RangeVarGetRelidExtended(stmt->relation,
        AccessShareLock,
        stmt->missing_ok,
        false,
        false,
        false,
        RangeVarCallbackForAlterRelation,
        (void*)stmt);

    if (!OidIsValid(partitionedTableOid)) {
        ereport(NOTICE, (errmsg("relation \"%s\" does not exist, skipping", stmt->relation->relname)));
        return;
    }

    rel = relation_open(partitionedTableOid, NoLock);

    /*
     * check relation is partitioned table
     */
    if (!PointerIsValid(rel->partMap)) {
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_TABLE),
                errmsg("relation \"%s\" is not partitioned table", stmt->relation->relname)));
    }

    /*
     * Get oid of target partition.
     * 1. If rename partition by name.
     */
    if (PointerIsValid(stmt->subname)) {
        partitionOid = partitionNameGetPartitionOid(partitionedTableOid,
            stmt->subname,
            PART_OBJ_TYPE_TABLE_PARTITION,
            AccessExclusiveLock,
            true,
            false,
            NULL,
            NULL,
            NoLock);

        if (!OidIsValid(partitionOid)) {
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg(
                        "partition \"%s\" of relation \"%s\" does not exist", stmt->subname, stmt->relation->relname)));
        }
    } else {
        /*
         * 2. If rename partition by partition key value.
         */
        pstate = make_parsestate(NULL);
        rangePartDef = makeNode(RangePartitionDefState);
        rangePartDef->boundary = stmt->object;
        transformRangePartitionValue(pstate, (Node*)rangePartDef, false);
        rangePartDef->boundary = transformConstIntoTargetType(
            rel->rd_att->attrs, ((RangePartitionMap*)rel->partMap)->partitionKey, rangePartDef->boundary);
        partitionOid =
            partitionValuesGetPartitionOid(rel, rangePartDef->boundary, AccessExclusiveLock, true, true, false);

        pfree_ext(pstate);
        list_free_deep(rangePartDef->boundary);
        pfree_ext(rangePartDef);

        if (!OidIsValid(partitionOid)) {
            ereport(
                ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("The partition number is invalid or out-of-range")));
        }
    }

    /* hold lock until committed */
    relation_close(rel, NoLock);

    /* check partition new name does not exist. */
    if (InvalidOid != GetSysCacheOid3(PARTPARTOID,
        NameGetDatum(stmt->newname),
        CharGetDatum(PART_OBJ_TYPE_TABLE_PARTITION),
        ObjectIdGetDatum(partitionedTableOid))) {
        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_TABLE),
                errmsg("partition \"%s\" of relation \"%s\" already exists", stmt->newname, stmt->relation->relname)));
    }

    /* Do the work */
    renamePartitionInternal(partitionedTableOid, partitionOid, stmt->newname);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: Execute rename partition index
 * Description	:
 * Notes		:
 */
void renamePartitionIndex(RenameStmt* stmt)
{
    Oid partitionedTableIndexOid = InvalidOid;
    Oid partitionIndexOid = InvalidOid;

    /* shouldn't happen */
    if (!PointerIsValid(stmt) || !PointerIsValid(stmt->subname) || !PointerIsValid(stmt->newname)) {
        ereport(
            ERROR, (errcode(ERRCODE_UNEXPECTED_NULL_VALUE), errmsg("internal error, rename partition index failed")));
    }

    /* Get oid of target partitioned table index.
     *
     * Grab an RowExclusive lock on the target partitioned table,
     * which we will NOT release until end of transaction.
     *
     * Lock level used here should match renamePartitonInternal, to avoid lock
     * escalation.
     */
    partitionedTableIndexOid = RangeVarGetRelidExtended(stmt->relation,
        AccessShareLock,
        stmt->missing_ok,
        false,
        false,
        false,
        RangeVarCallbackForAlterRelation,
        (void*)stmt);

    if (!OidIsValid(partitionedTableIndexOid)) {
        ereport(NOTICE, (errmsg("index \"%s\" does not exist, skipping", stmt->relation->relname)));

        return;
    }

    /* get partition index oid */
    partitionIndexOid = partitionNameGetPartitionOid(partitionedTableIndexOid,
        stmt->subname,
        PART_OBJ_TYPE_INDEX_PARTITION,
        AccessExclusiveLock,
        true,
        false,
        NULL,
        NULL,
        NoLock);
    if (InvalidOid == partitionIndexOid) {
        ereport(
            ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("partition index \"%s\" does not exist", stmt->subname)));
    }

    /*
     * check partition index new name does not exist.
     */
    if (InvalidOid != GetSysCacheOid3(PARTPARTOID,
                          NameGetDatum(stmt->newname),
                          CharGetDatum(PART_OBJ_TYPE_INDEX_PARTITION),
                          ObjectIdGetDatum(partitionedTableIndexOid))) {
        ereport(
            ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("partition index \"%s\" already exists", stmt->newname)));
    }

    /* Do the work */
    renamePartitionInternal(partitionedTableIndexOid, partitionIndexOid, stmt->newname);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: Change the name of partition object (partition/index) in pg_partition
 * Description	:
 * Notes		:
 */
void renamePartitionInternal(Oid partitionedTableOid, Oid partitionOid, const char* partitionNewName)
{
    Relation partitionRelRelation = NULL;
    HeapTuple partitionRelationTuple = NULL;
    Form_pg_partition relationForm = NULL;

    /* shouldn't happen */
    if (!OidIsValid(partitionedTableOid) || !OidIsValid(partitionOid) || !PointerIsValid(partitionNewName)) {
        ereport(ERROR, (errcode(ERRCODE_UNEXPECTED_NULL_VALUE), errmsg("internal error, rename partition failed")));
    }

    /*
     * Find relation's pg_partition tuple.
     */
    partitionRelRelation = relation_open(PartitionRelationId, RowExclusiveLock);
    partitionRelationTuple = SearchSysCacheCopy1(PARTRELID, ObjectIdGetDatum(partitionOid));

    /* shouldn't happen */
    if (!HeapTupleIsValid(partitionRelationTuple)) {
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("cache lookup failed for partition %u of relation %u", partitionOid, partitionedTableOid)));
    }

    relationForm = (Form_pg_partition)GETSTRUCT(partitionRelationTuple);

    /*
     * Update pg_partition tuple with new relname.	(Scribbling on reltup is OK
     * because it's a copy...)
     */
    (void)namestrcpy(&(relationForm->relname), partitionNewName);
    simple_heap_update(partitionRelRelation, &(partitionRelationTuple->t_self), partitionRelationTuple);

    /*
     * Keep the system catalog indexes current.
     */
    CatalogUpdateIndexes(partitionRelRelation, partitionRelationTuple);

    heap_freetuple_ext(partitionRelationTuple);

    relation_close(partitionRelRelation, RowExclusiveLock);
}

/*
 * Disallow ALTER TABLE (and similar commands) when the current backend has
 * any open reference to the target table besides the one just acquired by
 * the calling command; this implies there's an open cursor or active plan.
 * We need this check because our lock doesn't protect us against stomping
 * on our own foot, only other people's feet!
 *
 * For ALTER TABLE, the only case known to cause serious trouble is ALTER
 * COLUMN TYPE, and some changes are obviously pretty benign, so this could
 * possibly be relaxed to only error out for certain types of alterations.
 * But the use-case for allowing any of these things is not obvious, so we
 * won't work hard at it for now.
 *
 * We also reject these commands if there are any pending AFTER trigger events
 * for the rel.  This is certainly necessary for the rewriting variants of
 * ALTER TABLE, because they don't preserve tuple TIDs and so the pending
 * events would try to fetch the wrong tuples.	It might be overly cautious
 * in other cases, but again it seems better to err on the side of paranoia.
 *
 * REINDEX calls this with "rel" referencing the index to be rebuilt; here
 * we are worried about active indexscans on the index.  The trigger-event
 * check can be skipped, since we are doing no damage to the parent table.
 *
 * The statement name (eg, "ALTER TABLE") is passed for use in error messages.
 */
void CheckTableNotInUse(Relation rel, const char* stmt)
{
    int expected_refcnt;

    expected_refcnt = rel->rd_isnailed ? 2 : 1;
    if (rel->rd_refcnt != expected_refcnt)
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_IN_USE),
                /* translator: first %s is a SQL command, eg ALTER TABLE */
                errmsg("cannot %s \"%s\" because "
                       "it is being used by active queries in this session",
                    stmt,
                    RelationGetRelationName(rel))));

    if (rel->rd_rel->relkind != RELKIND_INDEX && AfterTriggerPendingOnRel(RelationGetRelid(rel)))
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_IN_USE),
                /* translator: first %s is a SQL command, eg ALTER TABLE */
                errmsg("cannot %s \"%s\" because "
                       "it has pending trigger events",
                    stmt,
                    RelationGetRelationName(rel))));
}

void CheckPartitionNotInUse(Partition part, const char* stmt)
{
    const int expected_refcnt = 1;

    if (part->pd_refcnt != expected_refcnt)
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_IN_USE),
                /* translator: first %s is a SQL command, eg ALTER TABLE */
                errmsg("cannot %s \"%s\" because "
                       "it is being used by active queries in this session",
                    stmt,
                    PartitionGetPartitionName(part))));
}
/*
 * AlterTableLookupRelation
 *		Look up, and lock, the OID for the relation named by an alter table
 *		statement.
 */
Oid AlterTableLookupRelation(AlterTableStmt* stmt, LOCKMODE lockmode, bool unlock)
{
    // treat unusable index and unusable index partition ops like drop index or dop index partition,
    // though we do not provide interface to drop index partition
    if ((stmt->relkind == OBJECT_INDEX) && (stmt->cmds->length == 1)) {
        AlterTableCmd* index_cmd = (AlterTableCmd*)lfirst(stmt->cmds->head);
        struct DropRelationCallbackState state = {0};
        state.concurrent = false;
        state.heapOid = InvalidOid;
        state.relkind = RELKIND_INDEX;
        
        if (index_cmd->subtype == AT_UnusableIndex) {
            Oid relid = InvalidOid;
            relid = RangeVarGetRelidExtended(stmt->relation,
                lockmode,
                stmt->missing_ok,
                false,
                false,  // not on index partition
                false,  // not support synonym
                RangeVarCallbackForDropRelation,
                (void*)&state);
            if (OidIsValid(state.heapOid) && u_sess->attr.attr_sql.enable_parallel_ddl && unlock) {
                UnlockRelationOid(state.heapOid, state.concurrent ? ShareUpdateExclusiveLock : AccessExclusiveLock);
            }
            return relid;
        } else if (index_cmd->subtype == AT_UnusableIndexPartition) {
            Oid relid = InvalidOid;
            relid = RangeVarGetRelidExtended(stmt->relation,
                lockmode,
                stmt->missing_ok,
                false,
                true,   // on index partition
                false,  // not support synonym
                RangeVarCallbackForDropRelation,
                (void*)&state);
            if (OidIsValid(state.heapOid) && u_sess->attr.attr_sql.enable_parallel_ddl && unlock) {
                UnlockRelationOid(state.heapOid, AccessShareLock);
            }
            return relid;
        }
    }

    if (stmt->relkind == OBJECT_TABLE) {
        ListCell* lcmd = NULL;
        foreach (lcmd, stmt->cmds) {
            AlterTableCmd* cmd = (AlterTableCmd*)lfirst(lcmd);
            if (AT_TruncatePartition == cmd->subtype) {
                return RangeVarGetRelidExtended(stmt->relation,
                    lockmode,
                    stmt->missing_ok,
                    false,
                    true,
                    false,  // not support synonym
                    RangeVarCallbackForAlterRelation,
                    (void*)stmt);
            }
        }
    }

    // all other ops on index and heap, goes on
    return RangeVarGetRelidExtended(stmt->relation,
        lockmode,
        stmt->missing_ok,
        false,
        false,
        false,  // not support synonym
        RangeVarCallbackForAlterRelation,
        (void*)stmt);
}

/* enum of alter-table-instantly
 * DEFAULT_NULL means no operation to pg_attribute.
 * DEFAULT_NOT_NULL_CONST means updating the attinitdefval of pg_attribute.
 * DEFAULT_OTHER  means rewriting the all tuples.
 */
typedef enum {
    DEFAULT_NULL,           /* attribute that has no default value or has null default value */
    DEFAULT_NOT_NULL_CONST, /* attribute that has const-expression default value */
    DEFAULT_OTHER           /* other value */
} AT_INSTANT_DEFAULT_VALUE;

/*
 * Estimate whether alter-table-instantly is effective.
 * The follow does not adapt to alter-table-instantly:
 * 1. unsupported data type - refer to supportType
 * 2. the default value expression include  volatile function
 * 3. the memory used by default value is more than 128 bytes.
 * 4. the default value is actually null
 */
static AT_INSTANT_DEFAULT_VALUE shouldUpdateAllTuples(
    Expr* defaultExpr, Oid typeOid, int attLen, bool attByVal, bytea** defaultVal)
{
    bool isNull = false;
    int i;
    bool tooLong = false;
    bytea* result = NULL;
    errno_t rc;

    /* it's difficult to handle all the datatypes, espicially complex datatypes.
     * so we just handle those datatypes which are normal and used most time.
     */
    const Oid supportType[] = {BOOLOID,
        BYTEAOID,
        CHAROID,
        INT8OID,
        INT2OID,
        INT4OID,
        TEXTOID,
        FLOAT4OID,
        FLOAT8OID,
        ABSTIMEOID,
        RELTIMEOID,
        TINTERVALOID,
        BPCHAROID,
        VARCHAROID,
        NUMERICOID,
        DATEOID,
        TIMEOID,
        TIMESTAMPOID,
        TIMESTAMPTZOID,
        INTERVALOID,
        TIMETZOID,
        INT1OID,
        SMALLDATETIMEOID};

    /* During inplace upgrade, we may allow extra column types. */
    const Oid extraSupportTypeInUpgrade[] = {OIDOID, NAMEOID, ACLITEMARRAYOID};

    /* check data type, if the data type is not supported, rewrite table */
    bool support = false;
    for (i = 0; i < int(sizeof(supportType) / sizeof(Oid)); ++i) {
        if (supportType[i] == typeOid) {
            support = true;
            break;
        }
    }

    /* check extra supported type during upgrade if needed */
    if (!support && u_sess->attr.attr_common.IsInplaceUpgrade) {
        for (i = 0; i < int(sizeof(extraSupportTypeInUpgrade) / sizeof(Oid)); ++i) {
            if (extraSupportTypeInUpgrade[i] == typeOid) {
                support = true;
                break;
            }
        }
    }

    if (!support) {
        return DEFAULT_OTHER;
    }

    /* alter-table-instantly does not support volatile default value. */
    if (contain_volatile_functions((Node*)defaultExpr))
        return DEFAULT_OTHER;

    EState* estate = CreateExecutorState();
    ExprState* exprstate = ExecInitExpr(expression_planner(defaultExpr), NULL);
    ExprContext* econtext = GetPerTupleExprContext(estate);

    MemoryContext newcxt = GetPerTupleMemoryContext(estate);
    MemoryContext oldcxt = MemoryContextSwitchTo(newcxt);
    Datum value = ExecEvalExpr(exprstate, econtext, &isNull, NULL);
    (void)MemoryContextSwitchTo(oldcxt);

    if (!isNull) {
        if (attByVal) {
            result = (bytea*)palloc(attLen + VARHDRSZ);
            SET_VARSIZE(result, attLen + VARHDRSZ);
            store_att_byval(VARDATA(result), value, attLen);
        } else {
            int len = att_addlength_datum(0, attLen, DatumGetPointer(value));
            if (len >= ATT_DEFAULT_LEN) {
                /* if the length of default value > 128, need rewrite table.
                 * this limitation ensure attinitdefval of relcache do not consume
                 * too many memory space.
                 */
                tooLong = true;
            } else {
                result = (bytea*)palloc(len + VARHDRSZ);
                SET_VARSIZE(result, len + VARHDRSZ);
                Assert((char*)result + VARHDRSZ == VARDATA(result));
                rc = memcpy_s((char*)result + VARHDRSZ, len, DatumGetPointer(value), len);
                securec_check(rc, "", "");
            }
        }
    }

    FreeExecutorState(estate);

    if (tooLong) {
        return DEFAULT_OTHER;
    }

    *defaultVal = result;

    return isNull ? DEFAULT_NULL : DEFAULT_NOT_NULL_CONST;
}

/* updateInitDefVal
 *
 * Update the attinitdefval field of pg_attribute for altering table instantly.
 */
static void updateInitDefVal(bytea* value, Relation rel, int16 attNum)
{
    Relation attRelation;
    HeapTuple tup;
    HeapTuple newTuple;
    Datum replVals[Natts_pg_attribute];
    bool replNulls[Natts_pg_attribute];
    bool replRepls[Natts_pg_attribute];
    errno_t rc;

    attRelation = heap_open(AttributeRelationId, RowExclusiveLock);

    tup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(RelationGetRelid(rel)), Int16GetDatum(attNum));
    if (!HeapTupleIsValid(tup)) {
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for attribute %d of relation %u", attNum, RelationGetRelid(rel))));
    }

    /* Build new tuple. */
    rc = memset_s(replNulls, sizeof(replNulls), false, sizeof(replNulls));
    securec_check(rc, "\0", "\0");

    rc = memset_s(replRepls, sizeof(replRepls), false, sizeof(replRepls));
    securec_check(rc, "\0", "\0");

    replVals[Anum_pg_attribute_attinitdefval - 1] = PointerGetDatum(value);
    replRepls[Anum_pg_attribute_attinitdefval - 1] = true;
    newTuple = heap_modify_tuple(tup, RelationGetDescr(attRelation), replVals, replNulls, replRepls);
    ReleaseSysCache(tup);

    /* Update system catalog. */
    simple_heap_update(attRelation, &newTuple->t_self, newTuple);
    CatalogUpdateIndexes(attRelation, newTuple);
    heap_freetuple_ext(newTuple);

    heap_close(attRelation, RowExclusiveLock);

    /* Make the attribute's catalog entry visible */
    CommandCounterIncrement();
}

/*
 * Clear all attrinitdefvals of relation with relid. It is called when rewrite
 * table is finished and all tuple attrnum are equal to attrnum of tupledesc.
 */
void clearAttrInitDefVal(Oid relid)
{
    Datum replVals[Natts_pg_attribute];
    bool replNulls[Natts_pg_attribute];
    bool replRepls[Natts_pg_attribute];
    errno_t rc;

    Relation rel = heap_open(relid, AccessExclusiveLock);

    if (rel->rd_att->initdefvals != NULL) {
        rc = memset_s(replNulls, sizeof(replNulls), false, sizeof(replNulls));
        securec_check(rc, "\0", "\0");
        rc = memset_s(replRepls, sizeof(replRepls), false, sizeof(replRepls));
        securec_check(rc, "\0", "\0");

        /* set the attribute *attinitdefval* null to clear its value */
        replVals[Anum_pg_attribute_attinitdefval - 1] = (Datum)0;
        replRepls[Anum_pg_attribute_attinitdefval - 1] = true;
        replNulls[Anum_pg_attribute_attinitdefval - 1] = true;

        int natts = rel->rd_att->natts;
        HeapTuple* tuples = (HeapTuple*)palloc0(natts * sizeof(HeapTuple));
        Relation attRelation = heap_open(AttributeRelationId, RowExclusiveLock);

        /* Clear all attrinitdefvals of relation */
        for (int attno = 0; attno < natts; ++attno) {
            if (rel->rd_att->initdefvals[attno].isNull)
                continue;

            /* step 1: Build new tuple. */
            HeapTuple tup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid), Int16GetDatum(attno + 1));
            if (!HeapTupleIsValid(tup)) {
                ereport(ERROR,
                    (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                        errmsg("cache lookup failed for attribute %d of relation %u", attno + 1, relid)));
            }

            tuples[attno] = heap_modify_tuple(tup, RelationGetDescr(attRelation), replVals, replNulls, replRepls);
            ReleaseSysCache(tup);

            /* step 2: Update system catalog. */
            simple_heap_update(attRelation, &tuples[attno]->t_self, tuples[attno]);
            CatalogUpdateIndexes(attRelation, tuples[attno]);

            /* free newtuple */
            heap_freetuple_ext(tuples[attno]);
            tuples[attno] = NULL;
        }

        pfree_ext(tuples);
        heap_close(attRelation, RowExclusiveLock);
    }

    heap_close(rel, AccessExclusiveLock);

    /* Make the attribute's catalog entry visible */
    CommandCounterIncrement();
}

/*
 * AlterTable
 *		Execute ALTER TABLE, which can be a list of subcommands
 *
 * ALTER TABLE is performed in three phases:
 *		1. Examine subcommands and perform pre-transformation checking.
 *		2. Update system catalogs.
 *		3. Scan table(s) to check new constraints, and optionally recopy
 *		   the data into new table(s).
 * Phase 3 is not performed unless one or more of the subcommands requires
 * it.	The intention of this design is to allow multiple independent
 * updates of the table schema to be performed with only one pass over the
 * data.
 *
 * ATPrepCmd performs phase 1.	A "work queue" entry is created for
 * each table to be affected (there may be multiple affected tables if the
 * commands traverse a table inheritance hierarchy).  Also we do preliminary
 * validation of the subcommands, including parse transformation of those
 * expressions that need to be evaluated with respect to the old table
 * schema.
 *
 * ATRewriteCatalogs performs phase 2 for each affected table.	(Note that
 * phases 2 and 3 normally do no explicit recursion, since phase 1 already
 * did it --- although some subcommands have to recurse in phase 2 instead.)
 * Certain subcommands need to be performed before others to avoid
 * unnecessary conflicts; for example, DROP COLUMN should come before
 * ADD COLUMN.	Therefore phase 1 divides the subcommands into multiple
 * lists, one for each logical "pass" of phase 2.
 *
 * ATRewriteTables performs phase 3 for those tables that need it.
 *
 * Thanks to the magic of MVCC, an error anywhere along the way rolls back
 * the whole operation; we don't have to do anything special to clean up.
 *
 * The caller must lock the relation, with an appropriate lock level
 * for the subcommands requested. Any subcommand that needs to rewrite
 * tuples in the table forces the whole command to be executed with
 * AccessExclusiveLock (actually, that is currently required always, but
 * we hope to relax it at some point).	We pass the lock level down
 * so that we can apply it recursively to inherited tables. Note that the
 * lock level we want as we recurse might well be higher than required for
 * that specific subcommand. So we pass down the overall lock requirement,
 * rather than reassess it at lower levels.
 *
 */
void AlterTable(Oid relid, LOCKMODE lockmode, AlterTableStmt* stmt)
{
    Relation rel;

    /* Caller is required to provide an adequate lock. */
    rel = relation_open(relid, lockmode);

    /* We allow to alter global temp table only this session use it */
    if (RELATION_IS_GLOBAL_TEMP(rel)) {
        if (is_other_backend_use_gtt(RelationGetRelid(rel))) {
            ereport(
                ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("can not alter table %s when other backend attached this global temp table",
                        RelationGetRelationName(rel))));
        }
    }

    CheckTableNotInUse(rel, "ALTER TABLE");

    /*
     * cmd list for 'ALTER FOREIGN TABLE ADD NODE'.
     * cmd list for 'ALTER TABLE DFSTbl ADD NODE'.
     */
    List* addNodeCmds = NIL;

    /* Unsupport ALTER statement for Foreign table */
    if (!stmt->fromCreate && stmt->relkind == OBJECT_FOREIGN_TABLE) {
        ListCell* lc = NULL;

        /* @hdfs
         * Check whether support alter foreign table. Many FDW types for foreign tables exists,
         * Each type foreign table has own mchanism, we use function pointor to realize. For example,
         * wheather support alter owner, support alter colomn type, etc.
         */
        FdwRoutine* fdwroutine = GetFdwRoutineByRelId(relid);
        stmt->relation->foreignOid = relid;

        if (NULL != fdwroutine->ValidateTableDef) {
            fdwroutine->ValidateTableDef((Node*)stmt);
        }

        foreach (lc, stmt->cmds) {
            AlterTableCmd* cmd = (AlterTableCmd*)lfirst(lc);

            if (AT_AddNodeList == cmd->subtype || AT_DeleteNodeList == cmd->subtype || AT_SubCluster == cmd->subtype) {
                /* append 'ALTER FOREIGN TABLE ADD NODE' cmd to ftAddNodeCmds */
                addNodeCmds = lappend(addNodeCmds, cmd);
            }
        }
    }

    /*
     * Process 'ALTER TABLE DELTA_TABLE ADD NODE' cmd.
     */
    if (!stmt->fromCreate && RelationIsDfsStore(rel)) {
        ListCell* lc = NULL;
        foreach (lc, stmt->cmds) {
            AlterTableCmd* cmd = (AlterTableCmd*)lfirst(lc);

            if (AT_AddNodeList == cmd->subtype || AT_SubCluster == cmd->subtype) {
                /* ALTER TABLE DELTA_TABLE ADD NODE' cmd. */
                addNodeCmds = lappend(addNodeCmds, cmd);
            }
        }
    }

    if (!stmt->fromCreate && (((uint32)RelationGetInternalMask(rel)) & INTERNAL_MASK_DALTER)) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                (errmsg("Un-support feature"), errdetail("internal relation doesn't allow ALTER"))));
    }

    // Unsupport ALTER statement for column store
    if (!stmt->fromCreate) {
        ListCell* lc = NULL;
        foreach (lc, stmt->cmds) {
            AlterTableCmd* cmd = (AlterTableCmd*)lfirst(lc);

            if (AT_AddOids == cmd->subtype) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        (errmsg("Un-support feature"),
                            errdetail("ALTER TABLE ... SET WITH OIDS is not yet supported."))));
            }

            if (AT_DropOids == cmd->subtype) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        (errmsg("Un-support feature"),
                            errdetail("ALTER TABLE ... SET WITHOUT OIDS is not yet supported."))));
            }

            if (RelationIsCUFormat(rel) && !CSTORE_SUPPORT_AT_CMD(cmd->subtype)) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("Un-support feature"),
                        errdetail("column-store relation doesn't support this ALTER yet")));
            }

            if (RelationIsTsStore(rel)) {
                at_timeseries_check(rel, cmd);
            }

            // We open up SetRelOptions for HDFS during online expansion so gs_redis could
            // set append_mode=read_only. Also we need to open up in CheckRedistributeOption.
            if (RelationIsPAXFormat(rel) &&
                !(DFS_SUPPORT_AT_CMD(cmd->subtype) ||
                    (u_sess->attr.attr_sql.enable_cluster_resize && AT_SetRelOptions == cmd->subtype))) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("Un-support feature"),
                        errdetail("It is not supported to alter a DFS table.")));
            }

            if (AT_SetTableSpace == cmd->subtype) {
                char* tblspcName = cmd->name;
                Oid tblspcOid = get_tablespace_oid(tblspcName, false);
                if (IsSpecifiedTblspc(tblspcOid, FILESYSTEM_HDFS)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("Un-support feature"),
                            errdetail("It is not supported to alter DFS tablespace.")));
                }
            }
        }
    }

    // Next version remove hack patch for 'ALTER FOREIGN TABLE ... ADD NODE'
    if (stmt->cmds != NIL) {
        /* process 'ALTER TABLE' cmd */
        ATController(rel, stmt->cmds, interpretInhOption(stmt->relation->inhOpt), lockmode);
    } else {
        /* if do not call ATController, close the relation in here, but keep lock until commit */
        relation_close(rel, NoLock);
    }

    if (addNodeCmds != NIL) {
        /*
         * Process 'ALTER TABLE DELTA_TABLE ADD NODE' cmd.
         */
        if (RelationIsDfsStore(rel)) {
            Oid deltaRelId = RelationGetDeltaRelId(rel);
            Assert(InvalidOid != deltaRelId);
            Relation deltaRel = relation_open(deltaRelId, lockmode);
            Assert(RelationIsValid(deltaRel));

            ATController(deltaRel, addNodeCmds, interpretInhOption(stmt->relation->inhOpt), lockmode);
        } else {

            /* process 'ALTER FOREIGN TABLE ... ADD NODE' cmd */
            DefElem* def = GetForeignTableOptionByName(relid, optErrorRel);
            if (def != NULL) {
                char* relname = strVal(def->arg);
                Oid errtableid = get_relname_relid(relname, get_rel_namespace(relid));

                /* open error table releation, closed in ATController */
                Relation errtablerel = relation_open(errtableid, lockmode);

                ATController(errtablerel, addNodeCmds, interpretInhOption(stmt->relation->inhOpt), lockmode);
            }
        }
    }
}

/*
 * AlterTableInternal
 *
 * ALTER TABLE with target specified by OID
 *
 * We do not reject if the relation is already open, because it's quite
 * likely that one or more layers of caller have it open.  That means it
 * is unsafe to use this entry point for alterations that could break
 * existing query plans.  On the assumption it's not used for such, we
 * don't have to reject pending AFTER triggers, either.
 */
void AlterTableInternal(Oid relid, List* cmds, bool recurse)
{
    Relation rel;
    LOCKMODE lockmode = AlterTableGetLockLevel(cmds);

    rel = relation_open(relid, lockmode);

    ATController(rel, cmds, recurse, lockmode);
}

/*
 * AlterTableGetLockLevel
 *
 * Sets the overall lock level required for the supplied list of subcommands.
 * Policy for doing this set according to needs of AlterTable(), see
 * comments there for overall explanation.
 *
 * Function is called before and after parsing, so it must give same
 * answer each time it is called. Some subcommands are transformed
 * into other subcommand types, so the transform must never be made to a
 * lower lock level than previously assigned. All transforms are noted below.
 *
 * Since this is called before we lock the table we cannot use table metadata
 * to influence the type of lock we acquire.
 *
 * There should be no lockmodes hardcoded into the subcommand functions. All
 * lockmode decisions for ALTER TABLE are made here only. The one exception is
 * ALTER TABLE RENAME which is treated as a different statement type T_RenameStmt
 * and does not travel through this section of code and cannot be combined with
 * any of the subcommands given here.
 */
LOCKMODE AlterTableGetLockLevel(List* cmds)
{
    /*
     * Late in 9.1 dev cycle a number of issues were uncovered with access to
     * catalog relations, leading to the decision to re-enforce all DDL at
     * AccessExclusiveLock level by default.
     *
     * The issues are that there is a pervasive assumption in the code that
     * the catalogs will not be read unless an AccessExclusiveLock is held. If
     * that rule is relaxed, we must protect against a number of potential
     * effects - infrequent, but proven possible with test cases where
     * multiple DDL operations occur in a stream against frequently accessed
     * tables.
     *
     * 1. Catalog tables are read using SnapshotNow, which has a race bug that
     * allows a scan to return no valid rows even when one is present in the
     * case of a commit of a concurrent update of the catalog table.
     * SnapshotNow also ignores transactions in progress, so takes the latest
     * committed version without waiting for the latest changes.
     *
     * 2. Relcache needs to be internally consistent, so unless we lock the
     * definition during reads we have no way to guarantee that.
     *
     * 3. Catcache access isn't coordinated at all so refreshes can occur at
     * any time.
     */
#ifdef REDUCED_ALTER_TABLE_LOCK_LEVELS
    ListCell* lcmd = NULL;
    LOCKMODE lockmode = ShareUpdateExclusiveLock;

    foreach (lcmd, cmds) {
        AlterTableCmd* cmd = (AlterTableCmd*)lfirst(lcmd);
        LOCKMODE cmd_lockmode = AccessExclusiveLock; /* default for compiler */

        switch (cmd->subtype) {
            case AT_UnusableIndex:
                cmd_lockmode = AccessExclusiveLock;
                break;
            case AT_AddPartition:
            case AT_DropPartition:
            case AT_ExchangePartition:
            case AT_MergePartition:
            case AT_UnusableIndexPartition:
            case AT_UnusableAllIndexOnPartition:
            case AT_SplitPartition:
                cmd_lockmode = AccessExclusiveLock;
                break;
            case AT_TruncatePartition:
                cmd_lockmode = AccessExclusiveLock;
                break;
                /*
                 * Need AccessExclusiveLock for these subcommands because they
                 * affect or potentially affect both read and write
                 * operations.
                 *
                 * New subcommand types should be added here by default.
                 */
            case AT_AddColumn:         /* may rewrite heap, in some cases and visible
                                        * to SELECT */
            case AT_DropColumn:        /* change visible to SELECT */
            case AT_AddColumnToView:   /* CREATE VIEW */
            case AT_AlterColumnType:   /* must rewrite heap */
            case AT_DropConstraint:    /* as DROP INDEX */
            case AT_AddOids:           /* must rewrite heap */
            case AT_DropOids:          /* calls AT_DropColumn */
            case AT_EnableAlwaysRule:  /* may change SELECT rules */
            case AT_EnableReplicaRule: /* may change SELECT rules */
            case AT_EnableRule:        /* may change SELECT rules */
            case AT_DisableRule:       /* may change SELECT rules */
            case AT_EnableRls:         /* may change SELECT|UPDATE|DELETE policies */
            case AT_DisableRls:        /* may change SELECT|UPDATE|DELETE policies */
            case AT_ForceRls:          /* may change SELECT|UPDATE|DELETE policies */
            case AT_NoForceRls:        /* may change SELECT|UPDATE|DELETE policies */
            case AT_ChangeOwner:       /* change visible to SELECT */
            case AT_SetTableSpace:     /* must rewrite heap */
            case AT_DropNotNull:       /* may change some SQL plans */
            case AT_SetNotNull:
            case AT_GenericOptions:
            case AT_SET_COMPRESS:
            case AT_AlterColumnGenericOptions:
            case AT_EnableRowMoveMent:
            case AT_DisableRowMoveMent:
                cmd_lockmode = AccessExclusiveLock;
                break;

            case AT_SetPartitionTableSpace:
                /* partitioned table lock: AccessShareLock
                 * partition lock: AccessExclusiveLock
                 */
                cmd_lockmode = AccessExclusiveLock;
                break;

#ifdef PGXC
            case AT_DistributeBy:   /* Changes table distribution type */
            case AT_SubCluster:     /* Changes node list of distribution */
            case AT_AddNodeList:    /* Adds nodes in distribution */
            case AT_DeleteNodeList: /* Deletes nodes in distribution */
                cmd_lockmode = ExclusiveLock;
                break;
#endif

                /*
                 * These subcommands affect write operations only.
                 */
            case AT_ColumnDefault:
            case AT_ProcessedConstraint:  /* becomes AT_AddConstraint */
            case AT_AddConstraintRecurse: /* becomes AT_AddConstraint */
            case AT_ReAddConstraint:      /* becomes AT_AddConstraint */
            case AT_EnableTrig:
            case AT_EnableAlwaysTrig:
            case AT_EnableReplicaTrig:
            case AT_EnableTrigAll:
            case AT_EnableTrigUser:
            case AT_DisableTrig:
            case AT_DisableTrigAll:
            case AT_DisableTrigUser:
            case AT_AddIndex: /* from ADD CONSTRAINT */
            case AT_AddIndexConstraint:
            case AT_ReplicaIdentity:
                cmd_lockmode = ShareRowExclusiveLock;
                break;

            case AT_AddConstraint:
                if (IsA(cmd->def, Constraint)) {
                    Constraint* con = (Constraint*)cmd->def;

                    switch (con->contype) {
                        case CONSTR_EXCLUSION:
                        case CONSTR_PRIMARY:
                        case CONSTR_UNIQUE:

                            /*
                             * Cases essentially the same as CREATE INDEX. We
                             * could reduce the lock strength to ShareLock if
                             * we can work out how to allow concurrent catalog
                             * updates.
                             */
                            cmd_lockmode = ShareRowExclusiveLock;
                            break;
                        case CONSTR_FOREIGN:

                            /*
                             * We add triggers to both tables when we add a
                             * Foreign Key, so the lock level must be at least
                             * as strong as CREATE TRIGGER.
                             */
                            cmd_lockmode = ShareRowExclusiveLock;
                            break;

                        default:
                            cmd_lockmode = ShareRowExclusiveLock;
                    }
                }
                break;

                /*
                 * These subcommands affect inheritance behaviour. Queries
                 * started before us will continue to see the old inheritance
                 * behaviour, while queries started after we commit will see
                 * new behaviour. No need to prevent reads or writes to the
                 * subtable while we hook it up though.
                 */
            case AT_AddInherit:
            case AT_DropInherit:
                cmd_lockmode = ShareUpdateExclusiveLock;
                break;

                /*
                 * These subcommands affect implicit row type conversion. They
                 * have affects similar to CREATE/DROP CAST on queries.  We
                 * don't provide for invalidating parse trees as a result of
                 * such changes.  Do avoid concurrent pg_class updates,
                 * though.
                 */
            case AT_AddOf:
            case AT_DropOf:
                cmd_lockmode = ShareUpdateExclusiveLock;

                /*
                 * These subcommands affect general strategies for performance
                 * and maintenance, though don't change the semantic results
                 * from normal data reads and writes. Delaying an ALTER TABLE
                 * behind currently active writes only delays the point where
                 * the new strategy begins to take effect, so there is no
                 * benefit in waiting. In this case the minimum restriction
                 * applies: we don't currently allow concurrent catalog
                 * updates.
                 */
            case AT_SetStatistics:
            case AT_ClusterOn:
            case AT_DropCluster:
            case AT_SetRelOptions:
            case AT_ResetRelOptions:
            case AT_ReplaceRelOptions:
            case AT_SetOptions:
            case AT_ResetOptions:
            case AT_SetStorage:
            case AT_ValidateConstraint:
                cmd_lockmode = ShareUpdateExclusiveLock;
                break;

            default: /* oops */
            {
                ereport(ERROR,
                    (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmsg("unrecognized alter table type: %d", (int)cmd->subtype)));
            } break;
        }

        /*
         * Take the greatest lockmode from any subcommand
         */
        if (cmd_lockmode > lockmode)
            lockmode = cmd_lockmode;
    }
#else
    ListCell* lcmd = NULL;

    /* default lock mode of DDL is the highest mode 8, even if commands list is empty */
    LOCKMODE lockmode = AccessExclusiveLock;

    if (list_length(cmds) > 0) {
        /* clear the default lock mode, so it's safe to compare with other lock modes. */
        lockmode = NoLock;

        foreach (lcmd, cmds) {
            LOCKMODE cmd_lockmode = AccessExclusiveLock;

            /* update with the higher lock mode */
            if (cmd_lockmode > lockmode) {
                lockmode = cmd_lockmode;
            }
        }
    }
#endif

    return lockmode;
}

static void ATController(Relation rel, List* cmds, bool recurse, LOCKMODE lockmode)
{
    List* wqueue = NIL;
    ListCell* lcmd = NULL;
#ifdef PGXC
    RedistribState* redistribState = NULL;
    bool doRedistribute = false;
#endif

    /* Phase 1: preliminary examination of commands, create work queue */
    foreach (lcmd, cmds) {
        AlterTableCmd* cmd = (AlterTableCmd*)lfirst(lcmd);

#ifdef PGXC
        /* Check restrictions of ALTER TABLE in cluster */
        ATCheckCmd(rel, cmd);
#endif

        ATPrepCmd(&wqueue, rel, cmd, recurse, false, lockmode);
    }

#ifdef PGXC
    /* Only check that on local Coordinator */
    if (IS_PGXC_COORDINATOR) {
        ListCell* ltab = NULL;

        /*
         * Redistribution is only applied to the parent table and not subsequent
         * children. It is also not applied in recursion. This needs to be done
         * once all the commands have been treated.
         */
        foreach (ltab, wqueue) {
            AlteredTableInfo* tab = (AlteredTableInfo*)lfirst(ltab);

            if (RelationGetRelid(rel) == tab->relid && list_length(tab->subcmds[AT_PASS_DISTRIB]) > 0) {
                /*
                 * Check if there are any commands incompatible
                 * with redistribution. For the time being no other commands
                 * are authorized.
                 */
                doRedistribute = true;
                if (!IsConnFromCoord()) {
                    if (list_length(tab->subcmds[AT_PASS_ADD_COL]) > 0 || list_length(tab->subcmds[AT_PASS_DROP]) > 0 ||
                        list_length(tab->subcmds[AT_PASS_ALTER_TYPE]) > 0 ||
                        list_length(tab->subcmds[AT_PASS_OLD_CONSTR]) > 0 ||
                        list_length(tab->subcmds[AT_PASS_COL_ATTRS]) > 0 ||
                        list_length(tab->subcmds[AT_PASS_ADD_INDEX]) > 0 ||
                        list_length(tab->subcmds[AT_PASS_ADD_CONSTR]) > 0 ||
                        list_length(tab->subcmds[AT_PASS_MISC]) > 0)
                        ereport(ERROR,
                            (errcode(ERRCODE_STATEMENT_TOO_COMPLEX),
                                errmsg("Incompatible operation with data redistribution")));

                    /* Scan redistribution commands and improve operation */
                    redistribState = BuildRedistribCommands(RelationGetRelid(rel), tab->subcmds[AT_PASS_DISTRIB]);
                }

                break;
            }
        }
    }
#endif

    /* Close the relation, but keep lock until commit */
    relation_close(rel, NoLock);

    /* Phase 2: update system catalogs */
    ATRewriteCatalogs(&wqueue, lockmode);

#ifdef PGXC
    /* Invalidate cache for redistributed relation */
    if (doRedistribute) {
        Relation rel2 = relation_open(RelationGetRelid(rel), NoLock);

        /* Invalidate all entries related to this relation */
        CacheInvalidateRelcache(rel2);

        /* Make sure locator info is rebuilt */
        RelationCacheInvalidateEntry(RelationGetRelid(rel));
        relation_close(rel2, NoLock);
    }

    if (redistribState != NULL)
        FreeRedistribState(redistribState);
#endif

    /* Phase 3: scan/rewrite tables as needed */
    ATRewriteTables(&wqueue, lockmode);
}

/*
 * ATPrepCmd
 *
 * Traffic cop for ALTER TABLE Phase 1 operations, including simple
 * recursion and permission checks.
 *
 * Caller must have acquired appropriate lock type on relation already.
 * This lock should be held until commit.
 */
static void ATPrepCmd(List** wqueue, Relation rel, AlterTableCmd* cmd, bool recurse, bool recursing, LOCKMODE lockmode)
{
    AlteredTableInfo* tab = NULL;
    int pass;

    /* Find or create work queue entry for this table */
    tab = ATGetQueueEntry(wqueue, rel);

    /*
     * Copy the original subcommand for each table.  This avoids conflicts
     * when different child tables need to make different parse
     * transformations (for example, the same column may have different column
     * numbers in different children).
     */
    cmd = (AlterTableCmd*)copyObject(cmd);

    /*
     * Do permissions checking, recursion to child tables if needed, and any
     * additional phase-1 processing needed.
     */
    switch (cmd->subtype) {
        case AT_AddColumn: /* ADD COLUMN */
            ATSimplePermissions(rel, ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE | ATT_SEQUENCE);
            ATPrepAddColumn(wqueue, rel, recurse, recursing, cmd, lockmode);
            /* Recursion occurs during execution phase */
            pass = AT_PASS_ADD_COL;
            break;
        case AT_AddPartition: /* ADD PARTITION */
            ATSimplePermissions(rel, ATT_TABLE);
            ATPrepAddPartition(rel);
            /* Recursion occurs during execution phase */
            pass = AT_PASS_ADD_PARTITION;
            break;
        case AT_AddColumnToView: /* add column via CREATE OR REPLACE VIEW */
            ATSimplePermissions(rel, ATT_VIEW);
            ATPrepAddColumn(wqueue, rel, recurse, recursing, cmd, lockmode);
            /* Recursion occurs during execution phase */
            pass = AT_PASS_ADD_COL;
            break;
        case AT_ColumnDefault: /* ALTER COLUMN DEFAULT */

            /*
             * We allow defaults on views so that INSERT into a view can have
             * default-ish behavior.  This works because the rewriter
             * substitutes default values into INSERTs before it expands
             * rules.
             */
            ATSimplePermissions(rel, ATT_TABLE | ATT_VIEW);
            ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode);
            ATPrepCheckDefault(cmd->def);
            /* No command-specific prep needed */
            pass = cmd->def ? AT_PASS_ADD_CONSTR : AT_PASS_DROP;
            break;
        case AT_DropNotNull: /* ALTER COLUMN DROP NOT NULL */
            ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
            ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode);
            /* No command-specific prep needed */
            pass = AT_PASS_DROP;
            break;
        case AT_SetNotNull: /* ALTER COLUMN SET NOT NULL */
            ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
            ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode);
            /* No command-specific prep needed */
            pass = AT_PASS_ADD_CONSTR;
            break;
        case AT_SetStatistics: /* ALTER COLUMN SET STATISTICS */
            ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode);
            /* Performs own permission checks */
            ATPrepSetStatistics(rel, cmd->name, cmd->def, lockmode);
            pass = AT_PASS_MISC;
            break;
        case AT_AddStatistics:    /* ADD STATISTICS */
        case AT_DeleteStatistics: /* DELETE STATISTICS */
            ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
            ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode);
            /* Performs own permission checks */
            ATPrepSetStatistics(rel, cmd->name, cmd->def, lockmode);
            es_check_alter_table_statistics(rel, cmd);
            pass = AT_PASS_MISC;
            break;
        case AT_SetOptions:   /* ALTER COLUMN SET ( options ) */
        case AT_ResetOptions: /* ALTER COLUMN RESET ( options ) */
            ATSimplePermissions(rel, ATT_TABLE | ATT_INDEX | ATT_FOREIGN_TABLE);
            /* This command never recurses */
            pass = AT_PASS_MISC;
            break;
        case AT_SetStorage: /* ALTER COLUMN SET STORAGE */
            ATSimplePermissions(rel, ATT_TABLE);
            ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode);
            /* No command-specific prep needed */
            pass = AT_PASS_MISC;
            break;
        case AT_DropColumn: /* DROP COLUMN */
            ATSimplePermissions(rel,
                ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE |
                    (u_sess->attr.attr_common.IsInplaceUpgrade ? ATT_VIEW : ATT_NULL));
            ATPrepDropColumn(wqueue, rel, recurse, recursing, cmd, lockmode);
            /* Recursion occurs during execution phase */
            pass = AT_PASS_DROP;
            break;
        case AT_DropPartition: /* DROP PARTITION */
            ATSimplePermissions(rel, ATT_TABLE);
            ATPrepDropPartition(rel);
            /* Recursion occurs during execution phase */
            pass = AT_PASS_DROP;
            break;
        case AT_UnusableIndexPartition: /* UNUSEABLE INDEX PARTITION */
            ATSimplePermissions(rel, ATT_INDEX);
            ATPrepUnusableIndexPartition(rel);
            /* Recursion occurs during execution phase */
            pass = AT_PASS_MISC;
            break;
        case AT_UnusableAllIndexOnPartition: /* UNUSEABLE ALL INDEX ON PARTITION */
            ATSimplePermissions(rel, ATT_TABLE | ATT_INDEX);
            ATPrepUnusableAllIndexOnPartition(rel);
            /* Recursion occurs during execution phase */
            pass = AT_PASS_MISC;
            break;
        case AT_AddIndex: /* ADD INDEX */
            ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
            /* This command never recurses */
            /* No command-specific prep needed */
            pass = AT_PASS_ADD_INDEX;
            break;
        case AT_AddConstraint: /* ADD CONSTRAINT */
            ATSimplePermissions(rel, ATT_TABLE);
            /* Recursion occurs during execution phase */
            /* No command-specific prep needed except saving recurse flag */
            if (recurse)
                cmd->subtype = AT_AddConstraintRecurse;
            pass = AT_PASS_ADD_CONSTR;
            break;
        case AT_AddIndexConstraint: /* ADD CONSTRAINT USING INDEX */
            ATSimplePermissions(rel, ATT_TABLE);
            /* This command never recurses */
            /* No command-specific prep needed */
            pass = AT_PASS_ADD_CONSTR;
            break;
        case AT_DropConstraint: /* DROP CONSTRAINT */
            /* @hdfs
             * ATSimplePermissions's second parameter is change from ATT_TABLE to
             * ATT_TABLE|ATT_FOREIGN_TABLE to suppert droping HDFS foreign table.
             */
            ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
            /* Recursion occurs during execution phase */
            /* No command-specific prep needed except saving recurse flag */
            if (recurse)
                cmd->subtype = AT_DropConstraintRecurse;
            pass = AT_PASS_DROP;
            break;
        case AT_AlterColumnType: /* ALTER COLUMN TYPE */
            ATSimplePermissions(rel, ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
            /* Performs own recursion */
            ATPrepAlterColumnType(wqueue, tab, rel, recurse, recursing, cmd, lockmode);
            pass = AT_PASS_ALTER_TYPE;
            break;
        case AT_AlterColumnGenericOptions:
            ATSimplePermissions(rel, ATT_FOREIGN_TABLE);
            /* This command never recurses */
            /* No command-specific prep needed */
            pass = AT_PASS_MISC;
            break;
        case AT_ChangeOwner: /* ALTER OWNER */
            /* This command never recurses */
            /* No command-specific prep needed */
            pass = AT_PASS_MISC;
            break;
        case AT_ClusterOn:   /* CLUSTER ON */
        case AT_DropCluster: /* SET WITHOUT CLUSTER */
            ATSimplePermissions(rel, ATT_TABLE);
            /* These commands never recurse */
            /* No command-specific prep needed */
            pass = AT_PASS_MISC;
            break;
        case AT_AddOids: /* SET WITH OIDS */

            /*
             * partitioned table can not be setted with or without oids
             */
            if (RELATION_IS_PARTITIONED(rel)) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot set with oids on partitioned table")));
            }

            ATSimplePermissions(rel, ATT_TABLE);
            if (!rel->rd_rel->relhasoids || recursing)
                ATPrepAddOids(wqueue, rel, recurse, cmd, lockmode);
            /* Recursion occurs during execution phase */
            pass = AT_PASS_ADD_COL;
            break;
        case AT_DropOids: /* SET WITHOUT OIDS */

            /*
             * partitioned table can not be setted with or without oids
             */
            if (RELATION_IS_PARTITIONED(rel)) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot set without oids on partitioned table")));
            }

            ATSimplePermissions(rel, ATT_TABLE);
            /* Performs own recursion */
            if (rel->rd_rel->relhasoids) {
                AlterTableCmd* dropCmd = makeNode(AlterTableCmd);

                dropCmd->subtype = AT_DropColumn;
                dropCmd->name = pstrdup("oid");
                dropCmd->behavior = cmd->behavior;
                ATPrepCmd(wqueue, rel, dropCmd, recurse, false, lockmode);
            }
            pass = AT_PASS_DROP;
            break;
        case AT_SetTableSpace: /* SET TABLESPACE */
        case AT_SetPartitionTableSpace:
            ATSimplePermissions(rel, ATT_TABLE | ATT_INDEX);
            /* This command never recurses */
            ATPrepSetTableSpace(tab, rel, cmd->name, lockmode);
            pass = AT_PASS_MISC; /* doesn't actually matter */
            break;
        case AT_UnusableIndex:
        case AT_SetRelOptions:     /* SET ... */
        case AT_ResetRelOptions:   /* RESET ... */
        case AT_ReplaceRelOptions: /* reset them all, then set just these */
            ATSimplePermissions(rel, ATT_TABLE | ATT_INDEX | ATT_VIEW);
            /* This command never recurses */
            /* No command-specific prep needed */
            pass = AT_PASS_MISC;
            break;
        case AT_AddInherit: /* INHERIT */
            ATSimplePermissions(rel, ATT_TABLE);
            /* This command never recurses */
            ATPrepAddInherit(rel);
            pass = AT_PASS_MISC;
            break;
        case AT_ValidateConstraint: /* VALIDATE CONSTRAINT */
            ATSimplePermissions(rel, ATT_TABLE);
            /* Recursion occurs during execution phase */
            /* No command-specific prep needed except saving recurse flag */
            if (recurse)
                cmd->subtype = AT_ValidateConstraintRecurse;
            pass = AT_PASS_MISC;
            break;
        case AT_ReplicaIdentity: /* REPLICA IDENTITY ... */
            ATSimplePermissions(rel, ATT_TABLE);
            pass = AT_PASS_MISC;
            /* This command never recurses */
            /* No command-specific prep needed */
            break;
        case AT_EnableTrig: /* ENABLE TRIGGER variants */
        case AT_EnableAlwaysTrig:
        case AT_EnableReplicaTrig:
        case AT_EnableTrigAll:
        case AT_EnableTrigUser:
        case AT_DisableTrig: /* DISABLE TRIGGER variants */
        case AT_DisableTrigAll:
        case AT_DisableTrigUser:
        case AT_EnableRule: /* ENABLE/DISABLE RULE variants */
        case AT_EnableAlwaysRule:
        case AT_EnableReplicaRule:
        case AT_DisableRule:
        case AT_EnableRls: /* ENABLE/DISABLE ROW LEVEL SECURITY */
        case AT_DisableRls:
        case AT_ForceRls: /* FORCE/NO-FORCE ROW LEVEL SECURITY */
        case AT_NoForceRls:
        case AT_DropInherit: /* NO INHERIT */
        case AT_AddOf:       /* OF */
        case AT_DropOf:      /* NOT OF */
            ATSimplePermissions(rel, ATT_TABLE);
            /* These commands never recurse */
            /* No command-specific prep needed */
            pass = AT_PASS_MISC;
            break;
        case AT_GenericOptions:
            ATSimplePermissions(rel, ATT_FOREIGN_TABLE);
            /* No command-specific prep needed */
            pass = AT_PASS_MISC;
            break;
        case AT_SET_COMPRESS:
            ATSimplePermissions(rel, ATT_TABLE);
            pass = AT_PASS_MISC;
            break;
        case AT_EnableRowMoveMent:
            ATSimplePermissions(rel, ATT_TABLE);
            ATPrepEnableRowMovement(rel);
            pass = AT_PASS_MISC;
            break;
        case AT_DisableRowMoveMent:
            ATSimplePermissions(rel, ATT_TABLE);
            ATPrepDisableRowMovement(rel);
            pass = AT_PASS_MISC;
            break;
        case AT_TruncatePartition:
            ATPrepTruncatePartition(rel);
            pass = AT_PASS_MISC;
            break;
        case AT_ExchangePartition:
            ATSimplePermissions(rel, ATT_TABLE);
            ATPrepExchangePartition(rel);
            pass = AT_PASS_MISC;
            break;
        case AT_MergePartition:
            ATSimplePermissions(rel, ATT_TABLE);
            ATPrepMergePartition(rel);
            pass = AT_PASS_MISC;
            break;
        case AT_SplitPartition:
            ATSimplePermissions(rel, ATT_TABLE);
            ATPrepSplitPartition(rel);
            pass = AT_PASS_MISC;
            break;

#ifdef PGXC
        case AT_DistributeBy:
        case AT_SubCluster:
            ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
            /* No command-specific prep needed */
            pass = AT_PASS_DISTRIB;
            break;

        /* @hdfs
         * The HDFS foreign table support 'ALTER FOREIGN TABLE ADD NODE/DELETE NODE' cmd.
         */
        case AT_AddNodeList:
        case AT_DeleteNodeList:
            ATSimplePermissions(rel, ATT_TABLE | ATT_FOREIGN_TABLE);
            /* No command-specific prep needed */
            pass = AT_PASS_DISTRIB;
            break;
#endif
        default: /* oops */
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized alter table type: %d", (int)cmd->subtype)));
            pass = 0; /* keep compiler quiet */
            break;
    }

    /* Add the subcommand to the appropriate list for phase 2 */
    tab->subcmds[pass] = lappend(tab->subcmds[pass], cmd);
}

/*
 * ATRewriteCatalogs
 *
 * Traffic cop for ALTER TABLE Phase 2 operations.	Subcommands are
 * dispatched in a "safe" execution order (designed to avoid unnecessary
 * conflicts).
 */
static void ATRewriteCatalogs(List** wqueue, LOCKMODE lockmode)
{
    int pass;
    ListCell* ltab = NULL;

    /*
     * We process all the tables "in parallel", one pass at a time.  This is
     * needed because we may have to propagate work from one table to another
     * (specifically, ALTER TYPE on a foreign key's PK has to dispatch the
     * re-adding of the foreign key constraint to the other table).  Work can
     * only be propagated into later passes, however.
     */
    for (pass = 0; pass < AT_NUM_PASSES; pass++) {
        /* Go through each table that needs to be processed */
        foreach (ltab, *wqueue) {
            AlteredTableInfo* tab = (AlteredTableInfo*)lfirst(ltab);
            List* subcmds = tab->subcmds[pass];
            Relation rel;
            ListCell* lcmd = NULL;

            if (subcmds == NIL)
                continue;

            /*
             * Appropriate lock was obtained by phase 1, needn't get it again
             */
            rel = relation_open(tab->relid, NoLock);

            foreach (lcmd, subcmds)
                ATExecCmd(wqueue, tab, rel, (AlterTableCmd*)lfirst(lcmd), lockmode);

            /*
             * After the ALTER TYPE pass, do cleanup work (this is not done in
             * ATExecAlterColumnType since it should be done only once if
             * multiple columns of a table are altered).
             */
            if (pass == AT_PASS_ALTER_TYPE)
                ATPostAlterTypeCleanup(wqueue, tab, lockmode);

            relation_close(rel, NoLock);
        }
    }

    /* Check to see if a toast table must be added. */
    foreach (ltab, *wqueue) {
        AlteredTableInfo* tab = (AlteredTableInfo*)lfirst(ltab);

        if (tab->relkind == RELKIND_RELATION)
            AlterTableCreateToastTable(tab->relid, (Datum)0);
    }
}

/*
 * ATExecCmd: dispatch a subcommand to appropriate execution routine
 */
static void ATExecCmd(List** wqueue, AlteredTableInfo* tab, Relation rel, AlterTableCmd* cmd, LOCKMODE lockmode)
{
    elog(ES_LOGLEVEL, "[ATExecCmd] cmd subtype: %d", cmd->subtype);

    switch (cmd->subtype) {
        case AT_AddColumn:       /* ADD COLUMN */
        case AT_AddColumnToView: /* add column via CREATE OR REPLACE VIEW */
            ATExecAddColumn(wqueue, tab, rel, (ColumnDef*)cmd->def, false, false, false, lockmode);
            break;
        case AT_AddColumnRecurse:
            ATExecAddColumn(wqueue, tab, rel, (ColumnDef*)cmd->def, false, true, false, lockmode);
            break;
        case AT_AddPartition: /* add partition */
            ATExecAddPartition(rel, (AddPartitionState*)cmd->def);
            break;
        case AT_ColumnDefault: /* ALTER COLUMN DEFAULT */
            ATExecColumnDefault(rel, cmd->name, cmd->def, lockmode);
            break;
        case AT_DropNotNull: /* ALTER COLUMN DROP NOT NULL */
            ATExecDropNotNull(rel, cmd->name, lockmode);
            break;
        case AT_SetNotNull: /* ALTER COLUMN SET NOT NULL */
            ATExecSetNotNull(tab, rel, cmd->name, lockmode);
            break;
        case AT_SetStatistics: /* ALTER COLUMN SET STATISTICS */
            ATExecSetStatistics(rel, cmd->name, cmd->def, cmd->additional_property, lockmode);
            break;
        case AT_AddStatistics: /* ADD STATISTICS */
            ATExecAddStatistics(rel, cmd->def, lockmode);
            break;
        case AT_DeleteStatistics: /* DELETE STATISTICS */
            ATExecDeleteStatistics(rel, cmd->def, lockmode);
            break;
        case AT_SetOptions: /* ALTER COLUMN SET ( options ) */
            ATExecSetOptions(rel, cmd->name, cmd->def, false, lockmode);
            break;
        case AT_ResetOptions: /* ALTER COLUMN RESET ( options ) */
            ATExecSetOptions(rel, cmd->name, cmd->def, true, lockmode);
            break;
        case AT_SetStorage: /* ALTER COLUMN SET STORAGE */
            ATExecSetStorage(rel, cmd->name, cmd->def, lockmode);
            break;
        case AT_DropColumn: /* DROP COLUMN */
            ATExecDropColumn(wqueue, rel, cmd->name, cmd->behavior, false, false, cmd->missing_ok, lockmode);
            break;
        case AT_DropColumnRecurse: /* DROP COLUMN with recursion */
            ATExecDropColumn(wqueue, rel, cmd->name, cmd->behavior, true, false, cmd->missing_ok, lockmode);
            break;
        case AT_DropPartition: /* drop partition */
            ATExecDropPartition(rel, cmd);
            break;
        case AT_UnusableIndexPartition: /* unusable index partition */
            ATExecUnusableIndexPartition(rel, cmd->name);
            break;
        case AT_UnusableAllIndexOnPartition: /* unusable all index on partition */
            ATExecUnusableAllIndexOnPartition(rel, cmd->name);
            break;
        case AT_UnusableIndex:
            ATExecUnusableIndex(rel);
            break;
        case AT_AddIndex: /* ADD INDEX */
            ATExecAddIndex(tab, rel, (IndexStmt*)cmd->def, false, lockmode);
            break;
        case AT_ReAddIndex: /* ADD INDEX */
            ATExecAddIndex(tab, rel, (IndexStmt*)cmd->def, true, lockmode);
            break;
        case AT_AddConstraint: /* ADD CONSTRAINT */
            ATExecAddConstraint(wqueue, tab, rel, (Constraint*)cmd->def, false, false, lockmode);
            break;
        case AT_AddConstraintRecurse: /* ADD CONSTRAINT with recursion */
            ATExecAddConstraint(wqueue, tab, rel, (Constraint*)cmd->def, true, false, lockmode);
            break;
        case AT_ReAddConstraint: /* Re-add pre-existing check constraint */
            ATExecAddConstraint(wqueue, tab, rel, (Constraint*)cmd->def, false, true, lockmode);
            break;
        case AT_AddIndexConstraint: /* ADD CONSTRAINT USING INDEX */
            ATExecAddIndexConstraint(tab, rel, (IndexStmt*)cmd->def, lockmode);
            break;
        case AT_ValidateConstraint: /* VALIDATE CONSTRAINT */
            ATExecValidateConstraint(rel, cmd->name, false, false, lockmode);
            break;
        case AT_ValidateConstraintRecurse: /* VALIDATE CONSTRAINT with
                                            * recursion */
            ATExecValidateConstraint(rel, cmd->name, true, false, lockmode);
            break;
        case AT_DropConstraint: /* DROP CONSTRAINT */
            ATExecDropConstraint(rel, cmd->name, cmd->behavior, false, false, cmd->missing_ok, lockmode);
            break;
        case AT_DropConstraintRecurse: /* DROP CONSTRAINT with recursion */
            ATExecDropConstraint(rel, cmd->name, cmd->behavior, true, false, cmd->missing_ok, lockmode);
            break;
        case AT_AlterColumnType: /* ALTER COLUMN TYPE */
            ATExecAlterColumnType(tab, rel, cmd, lockmode);
            break;
        case AT_AlterColumnGenericOptions: /* ALTER COLUMN OPTIONS */
            ATExecAlterColumnGenericOptions(rel, cmd->name, (List*)cmd->def, lockmode);
            break;
        case AT_ChangeOwner: /* ALTER OWNER */
            ATExecChangeOwner(RelationGetRelid(rel), get_role_oid(cmd->name, false), false, lockmode);
            break;
        case AT_ClusterOn: /* CLUSTER ON */
            ATExecClusterOn(rel, cmd->name, lockmode);
            break;
        case AT_DropCluster: /* SET WITHOUT CLUSTER */
            ATExecDropCluster(rel, lockmode);
            break;
        case AT_AddOids: /* SET WITH OIDS */
            /* Use the ADD COLUMN code, unless prep decided to do nothing */
            if (cmd->def != NULL)
                ATExecAddColumn(wqueue, tab, rel, (ColumnDef*)cmd->def, true, false, false, lockmode);
            break;
        case AT_AddOidsRecurse: /* SET WITH OIDS */
            /* Use the ADD COLUMN code, unless prep decided to do nothing */
            if (cmd->def != NULL)
                ATExecAddColumn(wqueue, tab, rel, (ColumnDef*)cmd->def, true, true, false, lockmode);
            break;
        case AT_DropOids: /* SET WITHOUT OIDS */

            /*
             * Nothing to do here; we'll have generated a DropColumn
             * subcommand to do the real work
             */
            break;
        case AT_SetTableSpace: /* SET TABLESPACE */

            /*
             * Nothing to do here; Phase 3 does the work
             */
            break;
        case AT_SetPartitionTableSpace:
            ATExecSetTableSpaceForPartitionP2(tab, rel, cmd->def);
            break;
        case AT_SetRelOptions:     /* SET ... */
        case AT_ResetRelOptions:   /* RESET ... */
        case AT_ReplaceRelOptions: /* replace entire option list */
            ATExecSetRelOptions(rel, (List*)cmd->def, cmd->subtype, lockmode);
            break;
        case AT_EnableTrig: /* ENABLE TRIGGER name */
            ATExecEnableDisableTrigger(rel, cmd->name, TRIGGER_FIRES_ON_ORIGIN, false, lockmode);
            break;
        case AT_EnableAlwaysTrig: /* ENABLE ALWAYS TRIGGER name */
            ATExecEnableDisableTrigger(rel, cmd->name, TRIGGER_FIRES_ALWAYS, false, lockmode);
            break;
        case AT_EnableReplicaTrig: /* ENABLE REPLICA TRIGGER name */
            ATExecEnableDisableTrigger(rel, cmd->name, TRIGGER_FIRES_ON_REPLICA, false, lockmode);
            break;
        case AT_DisableTrig: /* DISABLE TRIGGER name */
            ATExecEnableDisableTrigger(rel, cmd->name, TRIGGER_DISABLED, false, lockmode);
            break;
        case AT_EnableTrigAll: /* ENABLE TRIGGER ALL */
            ATExecEnableDisableTrigger(rel, NULL, TRIGGER_FIRES_ON_ORIGIN, false, lockmode);
            break;
        case AT_DisableTrigAll: /* DISABLE TRIGGER ALL */
            ATExecEnableDisableTrigger(rel, NULL, TRIGGER_DISABLED, false, lockmode);
            break;
        case AT_EnableTrigUser: /* ENABLE TRIGGER USER */
            ATExecEnableDisableTrigger(rel, NULL, TRIGGER_FIRES_ON_ORIGIN, true, lockmode);
            break;
        case AT_DisableTrigUser: /* DISABLE TRIGGER USER */
            ATExecEnableDisableTrigger(rel, NULL, TRIGGER_DISABLED, true, lockmode);
            break;

        case AT_EnableRule: /* ENABLE RULE name */
            ATExecEnableDisableRule(rel, cmd->name, RULE_FIRES_ON_ORIGIN, lockmode);
            break;
        case AT_EnableAlwaysRule: /* ENABLE ALWAYS RULE name */
            ATExecEnableDisableRule(rel, cmd->name, RULE_FIRES_ALWAYS, lockmode);
            break;
        case AT_EnableReplicaRule: /* ENABLE REPLICA RULE name */
            ATExecEnableDisableRule(rel, cmd->name, RULE_FIRES_ON_REPLICA, lockmode);
            break;
        case AT_DisableRule: /* DISABLE RULE name */
            ATExecEnableDisableRule(rel, cmd->name, RULE_DISABLED, lockmode);
            break;
        case AT_EnableRls: /* ENABLE ROW LEVEL SECURITY */
            ATExecEnableDisableRls(rel, RELATION_RLS_ENABLE, lockmode);
            break;
        case AT_DisableRls: /* DISABLE ROW LEVEL SECURITY */
            ATExecEnableDisableRls(rel, RELATION_RLS_DISABLE, lockmode);
            break;
        case AT_ForceRls: /* FORCE ROW LEVEL SECURITY */
            ATExecEnableDisableRls(rel, RELATION_RLS_FORCE_ENABLE, lockmode);
            break;
        case AT_NoForceRls: /* NO FORCE ROW LEVEL SECURITY */
            ATExecEnableDisableRls(rel, RELATION_RLS_FORCE_DISABLE, lockmode);
            break;
        case AT_AddInherit:
            ATExecAddInherit(rel, (RangeVar*)cmd->def, lockmode);
            break;
        case AT_DropInherit:
            ATExecDropInherit(rel, (RangeVar*)cmd->def, lockmode);
            break;
        case AT_AddOf:
            ATExecAddOf(rel, (TypeName*)cmd->def, lockmode);
            break;
        case AT_DropOf:
            ATExecDropOf(rel, lockmode);
            break;
        case AT_ReplicaIdentity:
            ATExecReplicaIdentity(rel, (ReplicaIdentityStmt*)cmd->def, lockmode);
            break;
        case AT_GenericOptions:
            ATExecGenericOptions(rel, (List*)cmd->def);
            break;
        case AT_SET_COMPRESS:
            ATExecSetCompress(rel, cmd->name);
            break;
        case AT_EnableRowMoveMent:
            ATExecModifyRowMovement(rel, true);
            break;
        case AT_DisableRowMoveMent:
            ATExecModifyRowMovement(rel, false);
            break;
        case AT_TruncatePartition:
            ATExecTruncatePartition(rel, cmd);
            break;
        case AT_ExchangePartition:
            ATExecExchangePartition(rel, cmd);
            break;
        case AT_MergePartition:
            ATExecMergePartition(rel, cmd);
            break;
        case AT_SplitPartition:
            ATExecSplitPartition(rel, cmd);
            break;

#ifdef PGXC
        case AT_DistributeBy:
            AtExecDistributeBy(rel, (DistributeBy*)cmd->def);
            break;
        case AT_SubCluster:
            AtExecSubCluster(rel, (PGXCSubCluster*)cmd->def);
            break;
        case AT_AddNodeList:
            AtExecAddNode(rel, (List*)cmd->def);
            break;
        case AT_DeleteNodeList:
            AtExecDeleteNode(rel, (List*)cmd->def);
            break;
#endif
        default: /* oops */
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized alter table type: %d", (int)cmd->subtype)));
            break;
    }

    /* Recode time of alter relation. */
    PgObjectType objectType = GetPgObjectTypePgClass(tab->relkind);
    if (objectType != OBJECT_TYPE_INVALID) {
        UpdatePgObjectMtime(tab->relid, objectType);
    }

    /*
     * Bump the command counter to ensure the next subcommand in the sequence
     * can see the changes so far
     */
    CommandCounterIncrement();
}

typedef void (*ExecRewriteFuncPtr)(AlteredTableInfo*, Oid, LOCKMODE);
typedef void (*ExecOnlyTestFuncPtr)(AlteredTableInfo*);
typedef void (*ExecChangeTabspcFuncPtr)(AlteredTableInfo*, LOCKMODE);

#define IDX_ROW_TBL 0
#define IDX_COL_TBL 1
#define IDX_DFS_TBL 2
#define IDX_ORDINARY_TBL 0
#define IDX_PARTITIONED_TBL 1

ExecRewriteFuncPtr ExecRewriteFuncPtrArray[2][2] = {
    {ExecRewriteRowTable, ExecRewriteRowPartitionedTable}, {ExecRewriteCStoreTable, ExecRewriteCStorePartitionedTable}};
ExecOnlyTestFuncPtr ExecOnlyTestFuncPtrArray[3][2] = {{ExecOnlyTestRowTable, ExecOnlyTestRowPartitionedTable},
    {ExecOnlyTestCStoreTable, ExecOnlyTestCStorePartitionedTable},
    {exec_only_test_dfs_table, NULL}};
ExecChangeTabspcFuncPtr ExecChangeTabspcFuncPtrArray[2][2] = {
    {ExecChangeTableSpaceForRowTable, ExecChangeTableSpaceForRowPartition},
    {ExecChangeTableSpaceForCStoreTable, ExecChangeTableSpaceForCStorePartition}};

/*
 * @Description: check this relation whether it's a temp table in current session
 * @Param[IN] topRelId: top relation OID
 * @See also: RELATION_IS_OTHER_TEMP()
 */
static void CheckTopRelationIsInMyTempSession(Oid topRelId)
{
    /* first reset it */
    u_sess->cmd_cxt.topRelatationIsInMyTempSession = false;

    Relation topRel = relation_open(topRelId, NoLock);
    u_sess->cmd_cxt.topRelatationIsInMyTempSession =
        /* check top realtion persistent */
        topRel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
        /* check top relation namespace */
        (u_sess->catalog_cxt.myTempNamespace == topRel->rd_rel->relnamespace ||
            u_sess->catalog_cxt.myTempToastNamespace == topRel->rd_rel->relnamespace);

    relation_close(topRel, NoLock);
}

/*
 * ATRewriteTables: ALTER TABLE phase 3
 */
static void ATRewriteTables(List** wqueue, LOCKMODE lockmode)
{
    ListCell* ltab = NULL;

    /* Go through each table that needs to be checked or rewritten */
    foreach (ltab, *wqueue) {
        AlteredTableInfo* tab = (AlteredTableInfo*)lfirst(ltab);
        int rel_format_idx = IDX_ROW_TBL;
        int idxPartitionedOrNot = IDX_ORDINARY_TBL;

#ifdef PGXC
        /* Forbid table rewrite operations with online data redistribution */
        if (tab->rewrite && list_length(tab->subcmds[AT_PASS_DISTRIB]) > 0 && IS_PGXC_COORDINATOR && !IsConnFromCoord())
            ereport(ERROR,
                (errcode(ERRCODE_STATEMENT_TOO_COMPLEX), errmsg("Incompatible operation with data redistribution")));
#endif

        /* Foreign tables have no storage. */
        if (tab->relkind == RELKIND_FOREIGN_TABLE)
            continue;

        if (tab->relkind == RELKIND_RELATION) {
            Relation temprel = heap_open(tab->relid, NoLock);
            rel_format_idx =
                RelationIsCUFormat(temprel) ? IDX_COL_TBL : (RelationIsPAXFormat(temprel) ? IDX_DFS_TBL : IDX_ROW_TBL);
            idxPartitionedOrNot = RELATION_IS_PARTITIONED(temprel) ? IDX_PARTITIONED_TBL : IDX_ORDINARY_TBL;
            heap_close(temprel, NoLock);
        } else if (tab->relkind == RELKIND_INDEX) {
            Relation temprel = index_open(tab->relid, NoLock);
            rel_format_idx = IDX_ROW_TBL; /* row relation */
            idxPartitionedOrNot = RelationIsPartitioned(temprel) ? IDX_PARTITIONED_TBL : IDX_ORDINARY_TBL;
            index_close(temprel, NoLock);
        }

        /*
         * If we change column data types or add/remove OIDs, the operation
         * has to be propagated to tables that use this table's rowtype as a
         * column type.  tab->newvals will also be non-NULL in the case where
         * we're adding a column with a default.  We choose to forbid that
         * case as well, since composite types might eventually support
         * defaults.
         *
         * (Eventually we'll probably need to check for composite type
         * dependencies even when we're just scanning the table without a
         * rewrite, but at the moment a composite type does not enforce any
         * constraints, so it's not necessary/appropriate to enforce them just
         * during ALTER.)
         */
        if (tab->newvals != NIL || tab->rewrite) {
            Relation rel;

            rel = heap_open(tab->relid, NoLock);
            find_composite_type_dependencies(rel->rd_rel->reltype, rel, NULL);
            heap_close(rel, NoLock);
        }

        /*
         * We only need to rewrite the table if at least one column needs to
         * be recomputed, or we are adding/removing the OID column.
         */
        if (tab->rewrite) {
            /* Build a temporary relation and copy data */
            Relation OldHeap;
            Oid NewTableSpace;

            OldHeap = heap_open(tab->relid, NoLock);

            /*
             * We don't support rewriting of system catalogs; there are too
             * many corner cases and too little benefit.  In particular this
             * is certainly not going to work for mapped catalogs.
             */
            if (IsSystemRelation(OldHeap))
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot rewrite system relation \"%s\"", RelationGetRelationName(OldHeap))));
            if (RelationIsUsedAsCatalogTable(OldHeap))
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg(
                            "cannot rewrite table \"%s\" used as a catalog table", RelationGetRelationName(OldHeap))));
            /*
             * Don't allow rewrite on temp tables of other backends ... their
             * local buffer manager is not going to cope.
             */
            if (RELATION_IS_OTHER_TEMP(OldHeap))
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot rewrite temporary tables of other sessions")));

            if (RELATION_IS_GLOBAL_TEMP(OldHeap)) {
                /* gtt may not attached, create it */
                if (!gtt_storage_attached(tab->relid)) {
                    ResultRelInfo *resultRelInfo;
                    MemoryContext oldcontext;
                    MemoryContext ctx_alter_gtt;
                    ctx_alter_gtt =
                        AllocSetContextCreate(CurrentMemoryContext, "gtt alter table", ALLOCSET_DEFAULT_SIZES);
                    oldcontext = MemoryContextSwitchTo(ctx_alter_gtt);
                    resultRelInfo = makeNode(ResultRelInfo);
                    InitResultRelInfo(resultRelInfo, OldHeap, 1, 0);
                    if (resultRelInfo->ri_RelationDesc->rd_rel->relhasindex &&
                        resultRelInfo->ri_IndexRelationDescs == NULL)
                        ExecOpenIndices(resultRelInfo);

                    init_gtt_storage(CMD_UTILITY, resultRelInfo);
                    ExecCloseIndices(resultRelInfo);
                    (void)MemoryContextSwitchTo(oldcontext);
                    MemoryContextDelete(ctx_alter_gtt);
                }
            }

            /*
             * Select destination tablespace (same as original unless user
             * requested a change)
             */
            if (tab->newTableSpace)
                NewTableSpace = tab->newTableSpace;
            else
                NewTableSpace = OldHeap->rd_rel->reltablespace;

            heap_close(OldHeap, NoLock);

            ExecRewriteFuncPtrArray[rel_format_idx][idxPartitionedOrNot](tab, NewTableSpace, lockmode);
        } else {
            /*
             * Test the current data within the table against new constraints
             * generated by ALTER TABLE commands, but don't rebuild data.
             */
            if (tab->constraints != NIL || tab->new_notnull) {
                ExecOnlyTestFuncPtrArray[rel_format_idx][idxPartitionedOrNot](tab);
            }

            /*
             * If we had SET TABLESPACE but no reason to reconstruct tuples,
             * just do a block-by-block copy.
             */
            if (tab->newTableSpace) {
                CheckTopRelationIsInMyTempSession(tab->relid);
                ExecChangeTabspcFuncPtrArray[rel_format_idx][idxPartitionedOrNot](tab, lockmode);
            }
        }
    }

#ifdef PGXC
    /*
     * In PGXC, do not check the FK constraints on the Coordinator, and just return
     * That is because a SELECT is generated whose plan will try and use
     * the Datanodes. We (currently) do not want to do that on the Coordinator,
     * when the command is passed down to the Datanodes it will
     * peform the check locally.
     * This issue was introduced when we added multi-step handling,
     * it caused foreign key constraints to fail.
     * issue for pg_catalog or any other cases?
     */
    if (IS_PGXC_COORDINATOR)
        return;
#endif
    /*
     * Foreign key constraints are checked in a final pass, since (a) it's
     * generally best to examine each one separately, and (b) it's at least
     * theoretically possible that we have changed both relations of the
     * foreign key, and we'd better have finished both rewrites before we try
     * to read the tables.
     */
    foreach (ltab, *wqueue) {
        AlteredTableInfo* tab = (AlteredTableInfo*)lfirst(ltab);
        Relation rel = NULL;
        ListCell* lcon = NULL;

        foreach (lcon, tab->constraints) {
            NewConstraint* con = (NewConstraint*)lfirst(lcon);

            if (con->contype == CONSTR_FOREIGN) {
                Constraint* fkconstraint = (Constraint*)con->qual;
                Relation refrel;

                if (rel == NULL) {
                    /* Long since locked, no need for another */
                    rel = heap_open(tab->relid, NoLock);
                }

                refrel = heap_open(con->refrelid, RowShareLock);

                validateForeignKeyConstraint(fkconstraint->conname, rel, refrel, con->refindid, con->conid);

                /*
                 * No need to mark the constraint row as validated, we did
                 * that when we inserted the row earlier.
                 */
                heap_close(refrel, NoLock);
            }
        }

        if (rel)
            heap_close(rel, NoLock);
    }
}

/*
 * change ATRewriteTable() input: oid->rel
 */
/*
 * ATRewriteTable: scan or rewrite one table
 *
 * oldrel is NULL if we don't need to rewrite
 */
static void ATRewriteTableInternal(AlteredTableInfo* tab, Relation oldrel, Relation newrel)
{
    TupleDesc oldTupDesc;
    TupleDesc newTupDesc;
    bool needscan = false;
    List* notnull_attrs = NIL;
    int i;
    ListCell* l = NULL;
    EState* estate = NULL;
    CommandId mycid;
    BulkInsertState bistate;
    uint32 hi_options;

    oldTupDesc = tab->oldDesc;
    newTupDesc = RelationGetDescr(oldrel); /* includes all mods */

    /*
     * Prepare a BulkInsertState and options for heap_insert. Because we're
     * building a new heap, we can skip WAL-logging and fsync it to disk at
     * the end instead (unless WAL-logging is required for archiving or
     * streaming replication). The FSM is empty too, so don't bother using it.
     */
    if (newrel) {
        mycid = GetCurrentCommandId(true);
        bistate = GetBulkInsertState();

        hi_options = HEAP_INSERT_SKIP_FSM;
        if (!XLogIsNeeded())
            hi_options |= HEAP_INSERT_SKIP_WAL;
    } else {
        /* keep compiler quiet about using these uninitialized */
        mycid = 0;
        bistate = NULL;
        hi_options = 0;
    }

    /*
     * Generate the constraint and default execution states
     */
    estate = CreateExecutorState();

    /* Build the needed expression execution states */
    foreach (l, tab->constraints) {
        NewConstraint* con = (NewConstraint*)lfirst(l);

        switch (con->contype) {
            case CONSTR_CHECK:
                needscan = true;
                con->qualstate = (List*)ExecPrepareExpr((Expr*)con->qual, estate);
                break;
            case CONSTR_FOREIGN:
                /* Nothing to do here */
                break;
            default: {
                ereport(ERROR,
                    (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmsg("unrecognized constraint type: %d", (int)con->contype)));
            } break;
        }
    }

    foreach (l, tab->newvals) {
        NewColumnValue* ex = (NewColumnValue*)lfirst(l);

        /* expr already planned */
        ex->exprstate = ExecInitExpr((Expr*)ex->expr, NULL);
    }

    notnull_attrs = NIL;
    if (newrel || tab->new_notnull) {
        /*
         * If we are rebuilding the tuples OR if we added any new NOT NULL
         * constraints, check all not-null constraints.  This is a bit of
         * overkill but it minimizes risk of bugs, and heap_attisnull is a
         * pretty cheap test anyway.
         */
        for (i = 0; i < newTupDesc->natts; i++) {
            if (newTupDesc->attrs[i]->attnotnull && !newTupDesc->attrs[i]->attisdropped)
                notnull_attrs = lappend_int(notnull_attrs, i);
        }
        if (notnull_attrs != NULL)
            needscan = true;
    }

    if (newrel || needscan) {
        ExprContext* econtext = NULL;
        Datum* values = NULL;
        bool* isnull = NULL;
        TupleTableSlot* oldslot = NULL;
        TupleTableSlot* newslot = NULL;
        HeapScanDesc scan;
        HeapTuple tuple;
        MemoryContext oldCxt;
        List* dropped_attrs = NIL;
        ListCell* lc = NULL;
        errno_t rc = EOK;

        if (newrel)
            ereport(DEBUG1, (errmsg("rewriting table \"%s\"", RelationGetRelationName(oldrel))));
        else
            ereport(DEBUG1, (errmsg("verifying table \"%s\"", RelationGetRelationName(oldrel))));

        if (newrel) {
            /*
             * All predicate locks on the tuples or pages are about to be made
             * invalid, because we move tuples around.	Promote them to
             * relation locks.
             */
            TransferPredicateLocksToHeapRelation(oldrel);
        }

        econtext = GetPerTupleExprContext(estate);

        /*
         * Make tuple slots for old and new tuples.  Note that even when the
         * tuples are the same, the tupDescs might not be (consider ADD COLUMN
         * without a default).
         */
        oldslot = MakeSingleTupleTableSlot(oldTupDesc);
        newslot = MakeSingleTupleTableSlot(newTupDesc);

        /* Preallocate values/isnull arrays */
        i = Max(newTupDesc->natts, oldTupDesc->natts);
        values = (Datum*)palloc(i * sizeof(Datum));
        isnull = (bool*)palloc(i * sizeof(bool));
        rc = memset_s(values, i * sizeof(Datum), 0, i * sizeof(Datum));
        securec_check(rc, "\0", "\0");
        rc = memset_s(isnull, i * sizeof(bool), true, i * sizeof(bool));
        securec_check(rc, "\0", "\0");

        /*
         * Any attributes that are dropped according to the new tuple
         * descriptor can be set to NULL. We precompute the list of dropped
         * attributes to avoid needing to do so in the per-tuple loop.
         */
        for (i = 0; i < newTupDesc->natts; i++) {
            if (newTupDesc->attrs[i]->attisdropped)
                dropped_attrs = lappend_int(dropped_attrs, i);
        }

        /*
         * here we don't care oldTupDesc->initdefvals, because it's
         * handled during deforming old tuple. new values for added
         * colums maybe is from *tab->newvals* list, or newTupDesc'
         * initdefvals list.
         */
        if (newTupDesc->initdefvals) {
            TupInitDefVal* defvals = newTupDesc->initdefvals;

            /* skip all the existing columns within this relation */
            for (i = oldTupDesc->natts; i < newTupDesc->natts; ++i) {
                if (!defvals[i].isNull) {
                    /* we assign both *isnull* and *values* here instead of
                     * scaning loop, because all these are constant and not
                     * dependent on each tuple.
                     */
                    isnull[i] = false;
                    values[i] = fetchatt(newTupDesc->attrs[i], defvals[i].datum);
                }
            }
        }

        /*
         * Scan through the rows, generating a new row if needed and then
         * checking all the constraints.
         */
        scan = heap_beginscan(oldrel, SnapshotNow, 0, NULL);

        /*
         * Switch to per-tuple memory context and reset it for each tuple
         * produced, so we don't leak memory.
         */
        oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

        // it is special that oldTupDesc must be used for deforming the heap tuple,
        // so that scan->rs_tupdesc is overwritten here.
        //
        scan->rs_tupdesc = oldTupDesc;
        while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
            if (tab->rewrite) {
                Oid tupOid = InvalidOid;

                /* Extract data from old tuple */
                heap_deform_tuple(tuple, oldTupDesc, values, isnull);
                if (oldTupDesc->tdhasoid)
                    tupOid = HeapTupleGetOid(tuple);

                /* Set dropped attributes to null in new tuple */
                foreach (lc, dropped_attrs)
                    isnull[lfirst_int(lc)] = true;

                /*
                 * Process supplied expressions to replace selected columns.
                 * Expression inputs come from the old tuple.
                 */
                (void)ExecStoreTuple(tuple, oldslot, InvalidBuffer, false);
                econtext->ecxt_scantuple = oldslot;

                foreach (l, tab->newvals) {
                    NewColumnValue* ex = (NewColumnValue*)lfirst(l);

                    values[ex->attnum - 1] = ExecEvalExpr(ex->exprstate, econtext, &isnull[ex->attnum - 1], NULL);
                }

                /*
                 * Form the new tuple. Note that we don't explicitly pfree it,
                 * since the per-tuple memory context will be reset shortly.
                 */
                tuple = heap_form_tuple(newTupDesc, values, isnull);

                /* Preserve OID, if any */
                if (newTupDesc->tdhasoid)
                    HeapTupleSetOid(tuple, tupOid);
            }

            /* Now check any constraints on the possibly-changed tuple */
            (void)ExecStoreTuple(tuple, newslot, InvalidBuffer, false);
            econtext->ecxt_scantuple = newslot;

            foreach (l, notnull_attrs) {
                int attn = lfirst_int(l);

                /* replace heap_attisnull with relationAttIsNull
                 * due to altering table instantly
                 */
                if (relationAttIsNull(tuple, attn + 1, newTupDesc))
                    ereport(ERROR,
                        (errcode(ERRCODE_NOT_NULL_VIOLATION),
                            errmsg("column \"%s\" contains null values", NameStr(newTupDesc->attrs[attn]->attname))));
            }

            foreach (l, tab->constraints) {
                NewConstraint* con = (NewConstraint*)lfirst(l);

                switch (con->contype) {
                    case CONSTR_CHECK:
                        if (!ExecQual(con->qualstate, econtext, true))
                            ereport(ERROR,
                                (errcode(ERRCODE_CHECK_VIOLATION),
                                    errmsg("check constraint \"%s\" is violated by some row", con->name)));
                        break;
                    case CONSTR_FOREIGN:
                        /* Nothing to do here */
                        break;
                    default: {
                        ereport(ERROR,
                            (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                                errmsg("unrecognized constraint type: %d", (int)con->contype)));
                    }
                }
            }

            /* Write the tuple out to the new relation */
            if (newrel)
                (void)heap_insert(newrel, tuple, mycid, hi_options, bistate);

            ResetExprContext(econtext);

            CHECK_FOR_INTERRUPTS();
        }

        (void)MemoryContextSwitchTo(oldCxt);
        heap_endscan(scan);

        ExecDropSingleTupleTableSlot(oldslot);
        ExecDropSingleTupleTableSlot(newslot);
    }

    FreeExecutorState(estate);

    if (newrel) {
        FreeBulkInsertState(bistate);

        /* If we skipped writing WAL, then we need to sync the heap. */
        if (((hi_options & HEAP_INSERT_SKIP_WAL) || enable_heap_bcm_data_replication()) &&
            !RelationIsBucket(newrel))
            heap_sync(newrel);
    }
}

static void ATRewriteTable(AlteredTableInfo* tab, Relation oldrel, Relation newrel)
{
    if (RELATION_CREATE_BUCKET(oldrel)) {
        oidvector* bucketlist = searchHashBucketByOid(oldrel->rd_bucketoid);

        for (int i = 0; i < bucketlist->dim1; i++) {
            Relation oldbucket = bucketGetRelation(oldrel, NULL, bucketlist->values[i]);
            Relation newbucket = NULL;
            if (newrel != NULL) {
                newbucket = bucketGetRelation(newrel, NULL, bucketlist->values[i]);
            }

            ATRewriteTableInternal(tab, oldbucket, newbucket);

            bucketCloseRelation(oldbucket);
            if (newbucket != NULL) {
                bucketCloseRelation(newbucket);
            }
        }
        
        if (newrel && (!XLogIsNeeded() || enable_heap_bcm_data_replication())) {
            heap_sync(newrel);
        }
    } else {
        ATRewriteTableInternal(tab, oldrel, newrel);
    }
}

/*
 * ATGetQueueEntry: find or create an entry in the ALTER TABLE work queue
 */
static AlteredTableInfo* ATGetQueueEntry(List** wqueue, Relation rel)
{
    Oid relid = RelationGetRelid(rel);
    AlteredTableInfo* tab = NULL;
    ListCell* ltab = NULL;

    foreach (ltab, *wqueue) {
        tab = (AlteredTableInfo*)lfirst(ltab);
        if (tab->relid == relid)
            return tab;
    }

    /*
     * Not there, so add it.  Note that we make a copy of the relation's
     * existing descriptor before anything interesting can happen to it.
     */
    tab = (AlteredTableInfo*)palloc0(sizeof(AlteredTableInfo));
    tab->relid = relid;
    tab->relkind = rel->rd_rel->relkind;
    tab->oldDesc = CreateTupleDescCopy(RelationGetDescr(rel));

    *wqueue = lappend(*wqueue, tab);

    return tab;
}

/*
 * ATSimplePermissions
 *
 * - Ensure that it is a relation (or possibly a view)
 * - Ensure this user is the owner
 * - Ensure that it is not a system table
 */
static void ATSimplePermissions(Relation rel, int allowed_targets)
{
    int actual_target;

    switch (rel->rd_rel->relkind) {
        case RELKIND_RELATION:
            actual_target = ATT_TABLE;
            break;
        case RELKIND_VIEW:
            actual_target = ATT_VIEW;
            break;
        case RELKIND_INDEX:
            actual_target = ATT_INDEX;
            break;
        case RELKIND_COMPOSITE_TYPE:
            actual_target = ATT_COMPOSITE_TYPE;
            break;
        case RELKIND_FOREIGN_TABLE:
            actual_target = ATT_FOREIGN_TABLE;
            break;
        case RELKIND_SEQUENCE: {
            if (u_sess->attr.attr_common.IsInplaceUpgrade) {
                actual_target = ATT_SEQUENCE;
            } else
                actual_target = ATT_NULL;
            break;
        }
        default:
            actual_target = 0;
            break;
    }

    /* Wrong target type? */
    if ((actual_target & allowed_targets) == 0)
        ATWrongRelkindError(rel, allowed_targets);

    /* Permissions checks */
    if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, RelationGetRelationName(rel));

    if (!g_instance.attr.attr_common.allowSystemTableMods && !u_sess->attr.attr_common.IsInplaceUpgrade &&
        IsSystemRelation(rel))
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                errmsg("permission denied: \"%s\" is a system catalog", RelationGetRelationName(rel))));
}

/*
 * ATWrongRelkindError
 *
 * Throw an error when a relation has been determined to be of the wrong
 * type.
 */
static void ATWrongRelkindError(Relation rel, int allowed_targets)
{
    char* msg = NULL;

    switch (allowed_targets) {
        case ATT_TABLE:
            msg = _("\"%s\" is not a table");
            break;
        case ATT_TABLE | ATT_INDEX:
            msg = _("\"%s\" is not a table or index");
            break;
        case ATT_TABLE | ATT_VIEW:
            msg = _("\"%s\" is not a table or view");
            break;
        case ATT_TABLE | ATT_FOREIGN_TABLE:
            msg = _("\"%s\" is not a table or foreign table");
            break;
        case ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE:
            msg = _("\"%s\" is not a table, composite type, or foreign table");
            break;
        case ATT_VIEW:
            msg = _("\"%s\" is not a view");
            break;
        case ATT_FOREIGN_TABLE:
            msg = _("\"%s\" is not a foreign table");
            break;
        default:
            /* shouldn't get here, add all necessary cases above */
            msg = _("\"%s\" is of the wrong type");
            break;
    }

    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg(msg, RelationGetRelationName(rel))));
}

/*
 * ATSimpleRecursion
 *
 * Simple table recursion sufficient for most ALTER TABLE operations.
 * All direct and indirect children are processed in an unspecified order.
 * Note that if a child inherits from the original table via multiple
 * inheritance paths, it will be visited just once.
 */
static void ATSimpleRecursion(List** wqueue, Relation rel, AlterTableCmd* cmd, bool recurse, LOCKMODE lockmode)
{
    /*
     * Propagate to children if desired.  Non-table relations never have
     * children, so no need to search in that case.
     */
    if (recurse && rel->rd_rel->relkind == RELKIND_RELATION) {
        Oid relid = RelationGetRelid(rel);
        ListCell* child = NULL;
        List* children = NIL;

        if (g_instance.attr.attr_storage.enable_delta_store && RelationIsCUFormat(rel))
            children = find_cstore_delta(rel, lockmode);
        else
            children = find_all_inheritors(relid, lockmode, NULL);

        /*
         * find_all_inheritors does the recursive search of the inheritance
         * hierarchy, so all we have to do is process all of the relids in the
         * list that it returns.
         */
        foreach (child, children) {
            Oid childrelid = lfirst_oid(child);
            Relation childrel;

            if (childrelid == relid)
                continue;
            /* find_all_inheritors already got lock */
            childrel = relation_open(childrelid, NoLock);
            CheckTableNotInUse(childrel, "ALTER TABLE");
            ATPrepCmd(wqueue, childrel, cmd, false, true, lockmode);
            relation_close(childrel, NoLock);
        }
    }
}

/*
 * ATTypedTableRecursion
 *
 * Propagate ALTER TYPE operations to the typed tables of that type.
 * Also check the RESTRICT/CASCADE behavior.  Given CASCADE, also permit
 * recursion to inheritance children of the typed tables.
 */
static void ATTypedTableRecursion(List** wqueue, Relation rel, AlterTableCmd* cmd, LOCKMODE lockmode)
{
    ListCell* child = NULL;
    List* children = NIL;

    Assert(rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE);

    children = find_typed_table_dependencies(rel->rd_rel->reltype, RelationGetRelationName(rel), cmd->behavior);

    foreach (child, children) {
        Oid childrelid = lfirst_oid(child);
        Relation childrel;

        childrel = relation_open(childrelid, lockmode);
        CheckTableNotInUse(childrel, "ALTER TABLE");
        ATPrepCmd(wqueue, childrel, cmd, true, true, lockmode);
        relation_close(childrel, NoLock);
    }
}

/*
 * find_composite_type_dependencies
 *
 * Check to see if a composite type is being used as a column in some
 * other table (possibly nested several levels deep in composite types!).
 * Eventually, we'd like to propagate the check or rewrite operation
 * into other such tables, but for now, just error out if we find any.
 *
 * Caller should provide either a table name or a type name (not both) to
 * report in the error message, if any.
 *
 * We assume that functions and views depending on the type are not reasons
 * to reject the ALTER.  (How safe is this really?)
 */
void find_composite_type_dependencies(Oid typeOid, Relation origRelation, const char* origTypeName)
{
    Relation depRel;
    ScanKeyData key[2];
    SysScanDesc depScan;
    HeapTuple depTup;
    Oid arrayOid;

    /*
     * We scan pg_depend to find those things that depend on the rowtype. (We
     * assume we can ignore refobjsubid for a rowtype.)
     */
    depRel = heap_open(DependRelationId, AccessShareLock);

    ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(TypeRelationId));
    ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(typeOid));

    depScan = systable_beginscan(depRel, DependReferenceIndexId, true, SnapshotNow, 2, key);

    while (HeapTupleIsValid(depTup = systable_getnext(depScan))) {
        Form_pg_depend pg_depend = (Form_pg_depend)GETSTRUCT(depTup);
        Relation rel;
        Form_pg_attribute att;

        /* Ignore dependees that aren't user columns of relations */
        /* (we assume system columns are never of rowtypes) */
        if (pg_depend->classid != RelationRelationId || pg_depend->objsubid <= 0)
            continue;

        rel = relation_open(pg_depend->objid, AccessShareLock);
        att = rel->rd_att->attrs[pg_depend->objsubid - 1];
        
        if (rel->rd_rel->relkind == RELKIND_RELATION) {
            if (origTypeName != NULL)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot alter type \"%s\" because column \"%s.%s\" uses it",
                            origTypeName,
                            RelationGetRelationName(rel),
                            NameStr(att->attname))));
            else if (origRelation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot alter type \"%s\" because column \"%s.%s\" uses it",
                            RelationGetRelationName(origRelation),
                            RelationGetRelationName(rel),
                            NameStr(att->attname))));
            else if (origRelation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot alter foreign table \"%s\" because column \"%s.%s\" uses its row type",
                            RelationGetRelationName(origRelation),
                            RelationGetRelationName(rel),
                            NameStr(att->attname))));
            else
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot alter table \"%s\" because column \"%s.%s\" uses its row type",
                            RelationGetRelationName(origRelation),
                            RelationGetRelationName(rel),
                            NameStr(att->attname))));
        } else if (OidIsValid(rel->rd_rel->reltype)) {
            /*
             * A view or composite type itself isn't a problem, but we must
             * recursively check for indirect dependencies via its rowtype.
             */
            find_composite_type_dependencies(rel->rd_rel->reltype, origRelation, origTypeName);
        }
        relation_close(rel, AccessShareLock);
    }

    systable_endscan(depScan);
    relation_close(depRel, AccessShareLock);

    /*
     * If there's an array type for the rowtype, must check for uses of it,
     * too.
     */
    arrayOid = get_array_type(typeOid);
    if (OidIsValid(arrayOid))
        find_composite_type_dependencies(arrayOid, origRelation, origTypeName);
}

/*
 * find_typed_table_dependencies
 *
 * Check to see if a composite type is being used as the type of a
 * typed table.  Abort if any are found and behavior is RESTRICT.
 * Else return the list of tables.
 */
static List* find_typed_table_dependencies(Oid typeOid, const char* typname, DropBehavior behavior)
{
    Relation classRel;
    ScanKeyData key[1];
    HeapScanDesc scan;
    HeapTuple tuple;
    List* result = NIL;

    classRel = heap_open(RelationRelationId, AccessShareLock);

    ScanKeyInit(&key[0], Anum_pg_class_reloftype, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(typeOid));

    scan = heap_beginscan(classRel, SnapshotNow, 1, key);

    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
        if (behavior == DROP_RESTRICT)
            ereport(ERROR,
                (errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
                    errmsg("cannot alter type \"%s\" because it is the type of a typed table", typname),
                    errhint("Use ALTER ... CASCADE to alter the typed tables too.")));
        else
            result = lappend_oid(result, HeapTupleGetOid(tuple));
    }

    heap_endscan(scan);
    heap_close(classRel, AccessShareLock);

    return result;
}

/*
 * check_of_type
 *
 * Check whether a type is suitable for CREATE TABLE OF/ALTER TABLE OF.  If it
 * isn't suitable, throw an error.  Currently, we require that the type
 * originated with CREATE TYPE AS.	We could support any row type, but doing so
 * would require handling a number of extra corner cases in the DDL commands.
 */
void check_of_type(HeapTuple typetuple)
{
    Form_pg_type typ = (Form_pg_type)GETSTRUCT(typetuple);
    bool typeOk = false;

    if (typ->typtype == TYPTYPE_COMPOSITE) {
        Relation typeRelation;

        Assert(OidIsValid(typ->typrelid));
        typeRelation = relation_open(typ->typrelid, AccessShareLock);
        typeOk = (typeRelation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE);

        /*
         * Close the parent rel, but keep our AccessShareLock on it until xact
         * commit.	That will prevent someone else from deleting or ALTERing
         * the type before the typed table creation/conversion commits.
         */
        relation_close(typeRelation, NoLock);
    }
    if (!typeOk)
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("type %s is not a composite type", format_type_be(HeapTupleGetOid(typetuple)))));
}

/*
 * ALTER TABLE ADD COLUMN
 *
 * Adds an additional attribute to a relation making the assumption that
 * CHECK, NOT NULL, and FOREIGN KEY constraints will be removed from the
 * AT_AddColumn AlterTableCmd by parse_utilcmd.c and added as independent
 * AlterTableCmd's.
 *
 * ADD COLUMN cannot use the normal ALTER TABLE recursion mechanism, because we
 * have to decide at runtime whether to recurse or not depending on whether we
 * actually add a column or merely merge with an existing column.  (We can't
 * check this in a static pre-pass because it won't handle multiple inheritance
 * situations correctly.)
 */
static void ATPrepAddColumn(
    List** wqueue, Relation rel, bool recurse, bool recursing, AlterTableCmd* cmd, LOCKMODE lockmode)
{
    if (rel->rd_rel->reloftype && !recursing)
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("cannot add column to typed table")));

    ColumnDef* colDef = (ColumnDef*)cmd->def;

    if (RelationIsColStore(rel)) {
        int32 typmod = 0;
        HeapTuple typeTuple = typenameType(NULL, colDef->typname, &typmod);
        Oid typeOid = HeapTupleGetOid(typeTuple);
        ReleaseSysCache(typeTuple);

        // check the supported data type and error report if needed.
        if (RelationIsCUFormat(rel) && !IsTypeSupportedByCStore(typeOid, typmod)) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("type \"%s\" is not supported in column store", format_type_with_typemod(typeOid, typmod))));
        }
        if (RelationIsPAXFormat(rel) && !IsTypeSupportedByORCRelation(typeOid)) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("type \"%s\" is not supported in DFS table.", format_type_with_typemod(typeOid, typmod))));
        }
    }

    if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
        ATTypedTableRecursion(wqueue, rel, cmd, lockmode);

    if (recurse)
        cmd->subtype = AT_AddColumnRecurse;
}

static bool contain_columndef_walker(Node* node, void* context)
{
    if (node == NULL)
        return false;

    if (IsA(node, ColumnRef))
        return true;

    return raw_expression_tree_walker(node, (bool (*)())contain_columndef_walker, context);
}

static void ATPrepCheckDefault(Node* node)
{
    if (contain_columndef_walker(node, NULL)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                (errmsg("default value cannot reference to a column"),
                    errhint("Perhaps the default value is enclosed in double quotes"))));
    }
}

static FORCE_INLINE void ATExecAppendDefValExpr(_in_ AttrNumber attnum, _in_ Expr* defval, _out_ AlteredTableInfo* tab)
{
    NewColumnValue* newval;

    newval = (NewColumnValue*)palloc0(sizeof(NewColumnValue));
    newval->attnum = attnum;
    newval->expr = expression_planner(defval);

    tab->newvals = lappend(tab->newvals, newval);
    tab->rewrite = true;
}

static void ATExecAddColumn(List** wqueue, AlteredTableInfo* tab, Relation rel, ColumnDef* colDef, bool isOid,
    bool recurse, bool recursing, LOCKMODE lockmode)
{
    Oid myrelid = RelationGetRelid(rel);
    Relation pgclass = NULL;
    Relation attrdesc = NULL;
    HeapTuple reltup = NULL;
    FormData_pg_attribute attribute;
    int newattnum;
    char relkind;
    HeapTuple typeTuple;
    Oid typeOid = InvalidOid;
    int32 typmod = -1;
    Oid collOid = InvalidOid;
    Form_pg_type tform = NULL;
    Expr* defval = NULL;
    List* children = NIL;
    ListCell* child = NULL;
    AclResult aclresult;
    bool isDfsTable = RelationIsPAXFormat(rel);

    /* At top level, permission check was done in ATPrepCmd, else do it */
    if (recursing)
        ATSimplePermissions(rel, ATT_TABLE);

    attrdesc = heap_open(AttributeRelationId, RowExclusiveLock);

    /*
     * Are we adding the column to a recursion child?  If so, check whether to
     * merge with an existing definition for the column.  If we do merge, we
     * must not recurse.  Children will already have the column, and recursing
     * into them would mess up attinhcount.
     */
    if (colDef->inhcount > 0) {
        HeapTuple tuple;

        /* Does child already have a column by this name? */
        tuple = SearchSysCacheCopyAttName(myrelid, colDef->colname);
        if (HeapTupleIsValid(tuple)) {
            Form_pg_attribute childatt = (Form_pg_attribute)GETSTRUCT(tuple);
            Oid ctypeId = InvalidOid;
            int32 ctypmod = -1;
            Oid ccollid = InvalidOid;

            /* Child column must match on type, typmod, and collation */
            typenameTypeIdAndMod(NULL, colDef->typname, &ctypeId, &ctypmod);
            if (ctypeId != childatt->atttypid || ctypmod != childatt->atttypmod)
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                        errmsg("child table \"%s\" has different type for column \"%s\"",
                            RelationGetRelationName(rel),
                            colDef->colname)));
            ccollid = GetColumnDefCollation(NULL, colDef, ctypeId);
            if (ccollid != childatt->attcollation)
                ereport(ERROR,
                    (errcode(ERRCODE_COLLATION_MISMATCH),
                        errmsg("child table \"%s\" has different collation for column \"%s\"",
                            RelationGetRelationName(rel),
                            colDef->colname),
                        errdetail("\"%s\" versus \"%s\"",
                            get_collation_name(ccollid),
                            get_collation_name(childatt->attcollation))));

            /* If it's OID, child column must actually be OID */
            if (isOid && childatt->attnum != ObjectIdAttributeNumber)
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                        errmsg("child table \"%s\" has a conflicting \"%s\" column",
                            RelationGetRelationName(rel),
                            colDef->colname)));

            /* Bump the existing child att's inhcount */
            childatt->attinhcount++;
            simple_heap_update(attrdesc, &tuple->t_self, tuple);
            CatalogUpdateIndexes(attrdesc, tuple);

            heap_freetuple_ext(tuple);

            /* Inform the user about the merge */
            ereport(NOTICE,
                (errmsg("merging definition of column \"%s\" for child \"%s\"",
                    colDef->colname,
                    RelationGetRelationName(rel))));

            heap_close(attrdesc, RowExclusiveLock);
            return;
        }
    }

    pgclass = heap_open(RelationRelationId, RowExclusiveLock);

    reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(myrelid));
    if (!HeapTupleIsValid(reltup)) {
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", myrelid)));
    }
    relkind = ((Form_pg_class)GETSTRUCT(reltup))->relkind;

    /* new name should not already exist */
    check_for_column_name_collision(rel, colDef->colname);

    /* Determine the new attribute's number */
    if (isOid) {
        newattnum = ObjectIdAttributeNumber;
    } else {
        newattnum = ((Form_pg_class)GETSTRUCT(reltup))->relnatts + 1;
        if (newattnum > MaxHeapAttributeNumber)
            ereport(ERROR,
                (errcode(ERRCODE_TOO_MANY_COLUMNS),
                    errmsg("tables can have at most %d columns", MaxHeapAttributeNumber)));
    }

    typeTuple = typenameType(NULL, colDef->typname, &typmod);
    tform = (Form_pg_type)GETSTRUCT(typeTuple);
    typeOid = HeapTupleGetOid(typeTuple);

    aclresult = pg_type_aclcheck(typeOid, GetUserId(), ACL_USAGE);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error_type(aclresult, typeOid);

    collOid = GetColumnDefCollation(NULL, colDef, typeOid);

    /* make sure datatype is legal for a column */
    CheckAttributeType(colDef->colname, typeOid, collOid, list_make1_oid(rel->rd_rel->reltype), false);

    /* construct new attribute's pg_attribute entry */
    attribute.attrelid = myrelid;
    (void)namestrcpy(&(attribute.attname), colDef->colname);
    attribute.atttypid = typeOid;
    attribute.attstattarget = (newattnum > 0) ? -1 : 0;
    attribute.attlen = tform->typlen;
    attribute.attcacheoff = -1;
    attribute.atttypmod = typmod;
    attribute.attnum = newattnum;
    attribute.attbyval = tform->typbyval;
    attribute.attndims = list_length(colDef->typname->arrayBounds);
    attribute.attstorage = tform->typstorage;
    attribute.attalign = tform->typalign;
    attribute.attnotnull = colDef->is_not_null;
    attribute.atthasdef = false;
    attribute.attisdropped = false;
    attribute.attislocal = colDef->is_local;
    attribute.attkvtype = colDef->kvtype;
    VerifyAttrCompressMode(colDef->cmprs_mode, attribute.attlen, colDef->colname);
    attribute.attcmprmode = colDef->cmprs_mode;
    attribute.attinhcount = colDef->inhcount;
    attribute.attcollation = collOid;
    /* attribute.attacl is handled by InsertPgAttributeTuple */
    ReleaseSysCache(typeTuple);

    InsertPgAttributeTuple(attrdesc, &attribute, NULL);

    heap_close(attrdesc, RowExclusiveLock);

    /*
     * Update pg_class tuple as appropriate
     */
    if (isOid)
        ((Form_pg_class)GETSTRUCT(reltup))->relhasoids = true;
    else
        ((Form_pg_class)GETSTRUCT(reltup))->relnatts = newattnum;

    simple_heap_update(pgclass, &reltup->t_self, reltup);

    /* keep catalog indexes current */
    CatalogUpdateIndexes(pgclass, reltup);

    heap_freetuple_ext(reltup);

    /* Post creation hook for new attribute */
    InvokeObjectAccessHook(OAT_POST_CREATE, RelationRelationId, myrelid, newattnum, NULL);

    heap_close(pgclass, RowExclusiveLock);

    /* Make the attribute's catalog entry visible */
    CommandCounterIncrement();

    /*
     * Store the DEFAULT, if any, in the catalogs
     */
    if (colDef->raw_default) {
        RawColumnDefault* rawEnt = NULL;

        if (relkind == RELKIND_FOREIGN_TABLE) {
            if (!isMOTFromTblOid(RelationGetRelid(rel)))
                ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                        errmsg("default values on foreign tables are not supported")));
        }

        rawEnt = (RawColumnDefault*)palloc(sizeof(RawColumnDefault));
        rawEnt->attnum = attribute.attnum;
        rawEnt->raw_default = (Node*)copyObject(colDef->raw_default);

        /*
         * This function is intended for CREATE TABLE, so it processes a
         * _list_ of defaults, but we just do one.
         */
        (void)AddRelationNewConstraints(rel, list_make1(rawEnt), NIL, false, true);

        /* Make the additional catalog changes visible */
        CommandCounterIncrement();
    }

    /*
     * Tell Phase 3 to fill in the default expression, if there is one.
     *
     * If there is no default, Phase 3 doesn't have to do anything, because
     * that effectively means that the default is NULL.  The heap tuple access
     * routines always check for attnum > # of attributes in tuple, and return
     * NULL if so, so without any modification of the tuple data we will get
     * the effect of NULL values in the new column.
     *
     * An exception occurs when the new column is of a domain type: the domain
     * might have a NOT NULL constraint, or a check constraint that indirectly
     * rejects nulls.  If there are any domain constraints then we construct
     * an explicit NULL default value that will be passed through
     * CoerceToDomain processing.  (This is a tad inefficient, since it causes
     * rewriting the table which we really don't have to do, but the present
     * design of domain processing doesn't offer any simple way of checking
     * the constraints more directly.)
     *
     * Note: we use build_column_default, and not just the cooked default
     * returned by AddRelationNewConstraints, so that the right thing happens
     * when a datatype's default applies.
     *
     * We skip this step completely for views and foreign tables.  For a view,
     * we can only get here from CREATE OR REPLACE VIEW, which historically
     * doesn't set up defaults, not even for domain-typed columns.  And in any
     * case we mustn't invoke Phase 3 on a view or foreign table, since they
     * have no storage.
     */
    if (relkind != RELKIND_VIEW && relkind != RELKIND_COMPOSITE_TYPE && relkind != RELKIND_FOREIGN_TABLE &&
        attribute.attnum > 0) {
        /* test whether new column is null or not */
        bool testNotNull = colDef->is_not_null;

        /*
         * Generally, relcache of nailed-in system catalogs should not be blowed away
         * for reliability consideration. Under such circumstances, visiting the
         * attdesc of the newly added system catalog column, which is necessary in building
         * its default, is impossible.
         * On the other hand, during inplace or online upgrade, we only prohibit invalidation
         * of pg_class's and pg_attribute's relcache. As a result, we do not support
         * non-NULL default values for new columns in pg_class, pg_attribute, pg_proc during
         * inplace or online upgrade at present.
         */
        if (u_sess->attr.attr_common.IsInplaceUpgrade &&
            (rel->rd_id == RelationRelationId || rel->rd_id == AttributeRelationId))
            defval = NULL;
        else
            defval = (Expr*)build_column_default(rel, attribute.attnum);

        if (defval == NULL && GetDomainConstraints(typeOid) != NIL) {
            Oid baseTypeId;
            int32 baseTypeMod;
            Oid baseTypeColl;

            baseTypeMod = typmod;
            baseTypeId = getBaseTypeAndTypmod(typeOid, &baseTypeMod);
            baseTypeColl = get_typcollation(baseTypeId);
            defval = (Expr*)makeNullConst(baseTypeId, baseTypeMod, baseTypeColl);
            defval = (Expr*)coerce_to_target_type(
                NULL, (Node*)defval, baseTypeId, typeOid, typmod, COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST, -1);
            if (defval == NULL) /* should not happen */
                ereport(
                    ERROR, (errcode(ERRCODE_UNEXPECTED_NULL_VALUE), errmsg("failed to coerce base type to domain")));
        }

        if (defval != NULL) {
            /*
             * We don't support alter table add column which default with nextval expression.
             */
            if (contain_specified_function((Node*)defval, NEXTVALFUNCOID))
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("It's not supported to alter table add column default with nextval expression.")));

            /* if now the row-storage table must be rewrited,
             * it isn't need to apply alter-table-instantly feature.
             * also exclude temp table and column table.
             */
            if (RelationIsCUFormat(rel) || tab->rewrite ||
                RelationUsesSpaceType(rel->rd_rel->relpersistence) == SP_TEMP) {
                ATExecAppendDefValExpr(attribute.attnum, defval, tab);
            } else {
                bytea* value = NULL;
                AT_INSTANT_DEFAULT_VALUE ret =
                    shouldUpdateAllTuples(defval, attribute.atttypid, attribute.attlen, attribute.attbyval, &value);
                if (ret == DEFAULT_NOT_NULL_CONST) {
                    Assert(value != NULL);
                    updateInitDefVal(value, rel, attribute.attnum);
                    pfree_ext(value);
                    /* new column has const default value,
                     * so Not-Null test is not necessary.
                     */
                    testNotNull = false;
                } else if (ret == DEFAULT_OTHER) {
                    if (isDfsTable) {
                        ereport(ERROR,
                            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                                (errmsg("It is not supported on DFS table. The detailed reasons are the"
                                        " followings:"),
                                    errdetail("1. the default value may be a volatile function.\n"
                                              "2. the storage length of default value may be greater than 127.\n"
                                              "3. the data type of new column is not supported."))));
                    }
                    ATExecAppendDefValExpr(attribute.attnum, defval, tab);
                }
                /* nothing to do if ret is DEFAULT_NULL */
            }
        }

        /*
         * If the new column is NOT NULL, tell Phase 3 it needs to test that.
         * (Note we don't do this for an OID column.  OID will be marked not
         * null, but since it's filled specially, there's no need to test
         * anything.)
         */
        if (testNotNull) {
            tab->new_notnull = true;
        }
    }

    /*
     * If we are adding an OID column, we have to tell Phase 3 to rewrite the
     * table to fix that.
     */
    if (isOid) {
        tab->rewrite = true;
    }

    if (RelationIsColStore(rel)) {
        /*
         * The DFS table do not rewrite data, only update the catalog for default value.
         */
        if (isDfsTable) {
            tab->rewrite = false;
        } else {
            tab->rewrite = true;
        }
    }

    /*
     * Add needed dependency entries for the new column.
     */
    add_column_datatype_dependency(myrelid, newattnum, attribute.atttypid);
    add_column_collation_dependency(myrelid, newattnum, attribute.attcollation);

    if (RelationIsPAXFormat(rel)) {
        /*
         * Add column for delta table.
         */
        children = lappend_oid(children, RelationGetDeltaRelId(rel));
        /*
         * Get the lock to synchronize against concurrent drop.
         */
        LockRelationOid(RelationGetDeltaRelId(rel), lockmode);
        elog(DEBUG1,
            "[GET LOCK] Get the lock %d successfully on delta table of %s for altering operator.",
            lockmode,
            RelationGetRelationName(rel));
    } else if (g_instance.attr.attr_storage.enable_delta_store && RelationIsCUFormat(rel)) {
        /*
         * add cstore relation delta table to recurse, if col support inherit feture
         * we also need call find_inheritance_children as below
         */
        children = find_cstore_delta(rel, lockmode);
    } else {
        /*
         * Propagate to children as appropriate.  Unlike most other ALTER
         * routines, we have to do this one level of recursion at a time; we can't
         * use find_all_inheritors to do it in one pass.
         */
        children = find_inheritance_children(RelationGetRelid(rel), lockmode);
    }

    /*
     * If we are told not to recurse, there had better not be any child
     * tables; else the addition would put them out of step.
     */
    if (children && !recurse)
        ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION), errmsg("column must be added to child tables too")));

    /* Children should see column as singly inherited, Cstore table and delta talbe are not inherited table */
    if (!recursing && !RelationIsCUFormat(rel)) {
        colDef = (ColumnDef*)copyObject(colDef);
        colDef->inhcount = 1;
        colDef->is_local = false;
    }

    foreach (child, children) {
        Oid childrelid = lfirst_oid(child);
        Relation childrel;
        AlteredTableInfo* childtab = NULL;

        /* find_inheritance_children already got lock */
        childrel = heap_open(childrelid, NoLock);
        CheckTableNotInUse(childrel, "ALTER TABLE");

        /* Find or create work queue entry for this table */
        childtab = ATGetQueueEntry(wqueue, childrel);

        /* Recurse to child */
        ATExecAddColumn(wqueue, childtab, childrel, colDef, isOid, recurse, true, lockmode);

        heap_close(childrel, NoLock);
    }
}

/*
 * If a new or renamed column will collide with the name of an existing
 * column, error out.
 */
static void check_for_column_name_collision(Relation rel, const char* colname)
{
    HeapTuple attTuple;
    int attnum;

    /*
     * this test is deliberately not attisdropped-aware, since if one tries to
     * add a column matching a dropped column name, it's gonna fail anyway.
     */
    attTuple = SearchSysCache2(ATTNAME, ObjectIdGetDatum(RelationGetRelid(rel)), PointerGetDatum(colname));
    if (!HeapTupleIsValid(attTuple))
        return;

    attnum = ((Form_pg_attribute)GETSTRUCT(attTuple))->attnum;
    ReleaseSysCache(attTuple);

    /*
     * We throw a different error message for conflicts with system column
     * names, since they are normally not shown and the user might otherwise
     * be confused about the reason for the conflict.
     */
    if (attnum <= 0)
        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_COLUMN),
                errmsg("column name \"%s\" conflicts with a system column name", colname)));
    else
        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_COLUMN),
                errmsg("column \"%s\" of relation \"%s\" already exists", colname, RelationGetRelationName(rel))));
}

/*
 * Install a column's dependency on its datatype.
 */
static void add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid)
{
    ObjectAddress myself, referenced;

    myself.classId = RelationRelationId;
    myself.objectId = relid;
    myself.objectSubId = attnum;
    referenced.classId = TypeRelationId;
    referenced.objectId = typid;
    referenced.objectSubId = 0;
    recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
}

/*
 * Install a column's dependency on its collation.
 */
static void add_column_collation_dependency(Oid relid, int32 attnum, Oid collid)
{
    ObjectAddress myself, referenced;

    /* We know the default collation is pinned, so don't bother recording it */
    if (OidIsValid(collid) && collid != DEFAULT_COLLATION_OID) {
        myself.classId = RelationRelationId;
        myself.objectId = relid;
        myself.objectSubId = attnum;
        referenced.classId = CollationRelationId;
        referenced.objectId = collid;
        referenced.objectSubId = 0;
        recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
    }
}

/*
 * ALTER TABLE SET WITH OIDS
 *
 * Basically this is an ADD COLUMN for the special OID column.	We have
 * to cons up a ColumnDef node because the ADD COLUMN code needs one.
 */
static void ATPrepAddOids(List** wqueue, Relation rel, bool recurse, AlterTableCmd* cmd, LOCKMODE lockmode)
{
    /* If we're recursing to a child table, the ColumnDef is already set up */
    if (cmd->def == NULL) {
        ColumnDef* cdef = makeNode(ColumnDef);

        cdef->colname = pstrdup("oid");
        cdef->typname = makeTypeNameFromOid(OIDOID, -1);
        cdef->inhcount = 0;
        cdef->is_local = true;
        cdef->is_not_null = true;
        cdef->storage = 0;
        // the best compression method for OID datatype is DELTA. but its data is stored
        // in header part of heap tuple, instead of data part. so don't compress it.
        //
        cdef->kvtype = ATT_KV_UNDEFINED;
        cdef->cmprs_mode = ATT_CMPR_NOCOMPRESS;
        cmd->def = (Node*)cdef;
    }
    ATPrepAddColumn(wqueue, rel, recurse, false, cmd, lockmode);

    if (recurse)
        cmd->subtype = AT_AddOidsRecurse;
}

/*
 * ALTER TABLE ALTER COLUMN DROP NOT NULL
 */
static void ATExecDropNotNull(Relation rel, const char* colName, LOCKMODE lockmode)
{
    HeapTuple tuple;
    AttrNumber attnum;
    Relation attr_rel;
    List* indexoidlist = NIL;
    ListCell* indexoidscan = NULL;

    /*
     * lookup the attribute
     */
    attr_rel = heap_open(AttributeRelationId, RowExclusiveLock);

    tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

    if (!HeapTupleIsValid(tuple))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_COLUMN),
                errmsg("column \"%s\" of relation \"%s\" does not exist", colName, RelationGetRelationName(rel))));

    attnum = ((Form_pg_attribute)GETSTRUCT(tuple))->attnum;

    /* Prevent them from altering a system attribute */
    if (attnum <= 0)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot alter system column \"%s\"", colName)));

    /*
     * Check that the attribute is not in a primary key
     *
     * Note: we'll throw error even if the pkey index is not valid.
     */
    /* Loop over all indexes on the relation */
    indexoidlist = RelationGetIndexList(rel);

    foreach (indexoidscan, indexoidlist) {
        Oid indexoid = lfirst_oid(indexoidscan);
        HeapTuple indexTuple;
        Form_pg_index indexStruct;
        int i;

        indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
        if (!HeapTupleIsValid(indexTuple)) {
            ereport(
                ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for index %u", indexoid)));
        }
        indexStruct = (Form_pg_index)GETSTRUCT(indexTuple);

        /* If the index is not a primary key, skip the check */
        if (indexStruct->indisprimary) {
            /*
             * Loop over each attribute in the primary key and see if it
             * matches the to-be-altered attribute
             */
            for (i = 0; i < indexStruct->indnatts; i++) {
                if (indexStruct->indkey.values[i] == attnum)
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("column \"%s\" is in a primary key", colName)));
            }
        }

        ReleaseSysCache(indexTuple);
    }

    list_free_ext(indexoidlist);

    /*
     * Okay, actually perform the catalog change ... if needed
     */
    if (((Form_pg_attribute)GETSTRUCT(tuple))->attnotnull) {
        ((Form_pg_attribute)GETSTRUCT(tuple))->attnotnull = FALSE;

        simple_heap_update(attr_rel, &tuple->t_self, tuple);

        /* keep the system catalog indexes current */
        CatalogUpdateIndexes(attr_rel, tuple);
    }

    heap_close(attr_rel, RowExclusiveLock);
}

/*
 * ALTER TABLE ALTER COLUMN SET NOT NULL
 */
static void ATExecSetNotNull(AlteredTableInfo* tab, Relation rel, const char* colName, LOCKMODE lockmode)
{
    HeapTuple tuple;
    AttrNumber attnum;
    Relation attr_rel;

    /*
     * lookup the attribute
     */
    attr_rel = heap_open(AttributeRelationId, RowExclusiveLock);

    tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

    if (!HeapTupleIsValid(tuple))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_COLUMN),
                errmsg("column \"%s\" of relation \"%s\" does not exist", colName, RelationGetRelationName(rel))));

    attnum = ((Form_pg_attribute)GETSTRUCT(tuple))->attnum;

    /* Prevent them from altering a system attribute */
    if (attnum <= 0)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot alter system column \"%s\"", colName)));

    /*
     * Okay, actually perform the catalog change ... if needed
     */
    if (!((Form_pg_attribute)GETSTRUCT(tuple))->attnotnull) {
        ((Form_pg_attribute)GETSTRUCT(tuple))->attnotnull = TRUE;

        simple_heap_update(attr_rel, &tuple->t_self, tuple);

        /* keep the system catalog indexes current */
        CatalogUpdateIndexes(attr_rel, tuple);

        /* Tell Phase 3 it needs to test the constraint */
        tab->new_notnull = true;
    }

    heap_close(attr_rel, RowExclusiveLock);
}

/*
 * ALTER TABLE ALTER COLUMN SET/DROP DEFAULT
 */
static void ATExecColumnDefault(Relation rel, const char* colName, Node* newDefault, LOCKMODE lockmode)
{
    AttrNumber attnum;

    /*
     * get the number of the attribute
     */
    attnum = get_attnum(RelationGetRelid(rel), colName);
    if (attnum == InvalidAttrNumber) {
        if (u_sess->attr.attr_common.IsInplaceUpgrade) {
            ereport(WARNING,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                    errmsg("column \"%s\" of relation \"%s\" does not exist while dropping default",
                        colName,
                        RelationGetRelationName(rel))));
            return;
        } else
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                    errmsg("column \"%s\" of relation \"%s\" does not exist", colName, RelationGetRelationName(rel))));
    }

    /* Prevent them from altering a system attribute */
    if (attnum <= 0)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot alter system column \"%s\"", colName)));

    /*
     * Remove any old default for the column.  We use RESTRICT here for
     * safety, but at present we do not expect anything to depend on the
     * default.
     *
     * We treat removing the existing default as an internal operation when it
     * is preparatory to adding a new default, but as a user-initiated
     * operation when the user asked for a drop.
     */
    RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false, newDefault == NULL ? false : true);

    if (newDefault != NULL) {
        /* SET DEFAULT */
        RawColumnDefault* rawEnt = NULL;

        rawEnt = (RawColumnDefault*)palloc(sizeof(RawColumnDefault));
        rawEnt->attnum = attnum;
        rawEnt->raw_default = newDefault;

        /*
         * This function is intended for CREATE TABLE, so it processes a
         * _list_ of defaults, but we just do one.
         */
        (void)AddRelationNewConstraints(rel, list_make1(rawEnt), NIL, false, true);
    }
}

/*
 * ALTER TABLE ALTER COLUMN SET STATISTICS
 */
static void ATPrepSetStatistics(Relation rel, const char* colName, Node* newValue, LOCKMODE lockmode)
{
    /*
     * We do our own permission checking because (a) we want to allow SET
     * STATISTICS on indexes (for expressional index columns), and (b) we want
     * to allow SET STATISTICS on system catalogs without requiring
     * allowSystemTableMods to be turned on.
     */
    if (rel->rd_rel->relkind != RELKIND_RELATION && rel->rd_rel->relkind != RELKIND_INDEX &&
        rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE)
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("\"%s\" is not a table, index, or foreign table", RelationGetRelationName(rel))));

    /* Permissions checks */
    if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, RelationGetRelationName(rel));
}

static void ATExecSetStatistics(
    Relation rel, const char* colName, Node* newValue, AlterTableStatProperty additional_property, LOCKMODE lockmode)
{
    int newtarget;
    Relation attrelation;
    HeapTuple tuple;
    Form_pg_attribute attrtuple;

    Assert(IsA(newValue, Integer));
    newtarget = intVal(newValue);

    /*
     * Limit target to a sane range
     */
    if (additional_property == AT_CMD_WithoutPercent) {
        if (newtarget < -1) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("statistics target %d is too low", newtarget)));
        } else if (newtarget > STATISTIC_WITHOUT_PERCENT_THRESHOLD) {
            newtarget = STATISTIC_WITHOUT_PERCENT_THRESHOLD;
            ereport(WARNING,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("lowering statistics target to %d", newtarget)));
        }
    } else {
        // Example:additional_property == AT_CMD_WithPercent
        if (newtarget < 0 || newtarget > STATISTIC_WITH_PERCENT_THRESHOLD) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("statistics percent valid value is between 0 and 100")));
        }

        newtarget = -1 * newtarget - 1;
    }
    attrelation = heap_open(AttributeRelationId, RowExclusiveLock);

    tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

    if (!HeapTupleIsValid(tuple))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_COLUMN),
                errmsg("column \"%s\" of relation \"%s\" does not exist", colName, RelationGetRelationName(rel))));
    attrtuple = (Form_pg_attribute)GETSTRUCT(tuple);

    if (attrtuple->attnum <= 0)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot alter system column \"%s\"", colName)));

    attrtuple->attstattarget = newtarget;

    simple_heap_update(attrelation, &tuple->t_self, tuple);

    /* keep system catalog indexes current */
    CatalogUpdateIndexes(attrelation, tuple);

    heap_freetuple_ext(tuple);

    heap_close(attrelation, RowExclusiveLock);
}

/*
 * ATExecAddStatistics
 *     to execute add statistics
 *
 * @param (in) rel:
 *     the relation
 * @param (in) def:
 *     the alter table column definition
 * @param (in) lockmode:
 *     lock mode
 */
static void ATExecAddStatistics(Relation rel, Node* def, LOCKMODE lockmode)
{
    Assert(IsA(def, List));

    Oid relid = rel->rd_id;
    if (is_sys_table(relid)) {
        ereport(ERROR,
            ((errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("System catalog is not supported by extended statistic."))));
    }

    char relkind = es_get_starelkind();
    bool inh = es_get_stainherit();

    /* A list of VacAttrStats */
    int array_length = 0;
    VacAttrStats** vacattrstats_array = es_build_vacattrstats_array(rel, (List*)def, true, &array_length, inh);

    if (array_length > 0) {
        update_attstats(relid, relkind, false, array_length, vacattrstats_array, RelationGetRelPersistence(rel));
        if (RelationIsDfsStore(rel)) {
            /* HDFS complex table */
            update_attstats(relid, relkind, true, array_length, vacattrstats_array, RelationGetRelPersistence(rel));

            /* HDFS delta table */
            Oid delta_relid = rel->rd_rel->reldeltarelid;
            Assert(OidIsValid(delta_relid));
            update_attstats(
                delta_relid, relkind, false, array_length, vacattrstats_array, RelationGetRelPersistence(rel));
        }
    }
}

/*
 * ATExecDeleteStatistics
 *     to execute delete statistics
 *
 * @param (in) rel:
 *     the relation
 * @param (in) def:
 *     the alter table column definition
 * @param (in) lockmode:
 *     lock mode
 */
static void ATExecDeleteStatistics(Relation rel, Node* def, LOCKMODE lockmode)
{
    Assert(IsA(def, List));

    Oid relid = rel->rd_id;
    if (is_sys_table(relid)) {
        ereport(ERROR,
            ((errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("System catalog is not supported by extended statistic."))));
    }

    char relkind = es_get_starelkind();
    bool inh = es_get_stainherit();

    /* A list of VacAttrStats */
    int array_length = 0;
    VacAttrStats** vacattrstats_array = es_build_vacattrstats_array(rel, (List*)def, false, &array_length, inh);

    if (array_length > 0) {
        delete_attstats(relid, relkind, false, array_length, vacattrstats_array, DELETE_STATS_MULTI);
        if (RelationIsDfsStore(rel)) {
            /* HDFS complex table */
            delete_attstats(relid, relkind, true, array_length, vacattrstats_array, DELETE_STATS_MULTI);

            /* HDFS delta table */
            Oid delta_relid = rel->rd_rel->reldeltarelid;
            Assert(OidIsValid(delta_relid));
            delete_attstats(delta_relid, relkind, false, array_length, vacattrstats_array, DELETE_STATS_MULTI);
        }
    }
}

static void ATExecSetOptions(Relation rel, const char* colName, Node* options, bool isReset, LOCKMODE lockmode)
{
    Relation attrelation;
    HeapTuple tuple, newtuple;
    Form_pg_attribute attrtuple;
    Datum datum, newOptions;
    bool isnull = false;
    Datum repl_val[Natts_pg_attribute];
    bool repl_null[Natts_pg_attribute];
    bool repl_repl[Natts_pg_attribute];
    errno_t rc = EOK;

    attrelation = heap_open(AttributeRelationId, RowExclusiveLock);

    tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);

    if (!HeapTupleIsValid(tuple))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_COLUMN),
                errmsg("column \"%s\" of relation \"%s\" does not exist", colName, RelationGetRelationName(rel))));
    attrtuple = (Form_pg_attribute)GETSTRUCT(tuple);

    if (attrtuple->attnum <= 0)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot alter system column \"%s\"", colName)));

    Assert(IsA(options, List));
    ForbidToSetOptionsForAttribute((List*)options);

    /* Generate new proposed attoptions (text array) */
    datum = SysCacheGetAttr(ATTNAME, tuple, Anum_pg_attribute_attoptions, &isnull);
    newOptions = transformRelOptions(isnull ? (Datum)0 : datum, (List*)options, NULL, NULL, false, isReset);
    /* Validate new options */
    (void)attribute_reloptions(newOptions, true);

    /* Build new tuple. */
    rc = memset_s(repl_null, sizeof(repl_null), false, sizeof(repl_null));
    securec_check(rc, "\0", "\0");
    rc = memset_s(repl_repl, sizeof(repl_repl), false, sizeof(repl_repl));
    securec_check(rc, "\0", "\0");
    if (newOptions != (Datum)0)
        repl_val[Anum_pg_attribute_attoptions - 1] = newOptions;
    else
        repl_null[Anum_pg_attribute_attoptions - 1] = true;
    repl_repl[Anum_pg_attribute_attoptions - 1] = true;
    newtuple = heap_modify_tuple(tuple, RelationGetDescr(attrelation), repl_val, repl_null, repl_repl);
    ReleaseSysCache(tuple);

    /* Update system catalog. */
    simple_heap_update(attrelation, &newtuple->t_self, newtuple);
    CatalogUpdateIndexes(attrelation, newtuple);
    heap_freetuple_ext(newtuple);

    heap_close(attrelation, RowExclusiveLock);
}

/*
 * ALTER TABLE ALTER COLUMN SET STORAGE
 */
static void ATExecSetStorage(Relation rel, const char* colName, Node* newValue, LOCKMODE lockmode)
{
    char* storagemode = NULL;
    char newstorage;
    Relation attrelation;
    HeapTuple tuple;
    Form_pg_attribute attrtuple;

    Assert(IsA(newValue, String));
    storagemode = strVal(newValue);

    if (RelationIsColStore(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Storage type \"%s\" is meaningless for column relation", storagemode)));
    }

    if (pg_strcasecmp(storagemode, "plain") == 0)
        newstorage = 'p';
    else if (pg_strcasecmp(storagemode, "external") == 0)
        newstorage = 'e';
    else if (pg_strcasecmp(storagemode, "extended") == 0)
        newstorage = 'x';
    else if (pg_strcasecmp(storagemode, "main") == 0)
        newstorage = 'm';
    else {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid storage type \"%s\"", storagemode)));
        newstorage = 0; /* keep compiler quiet */
    }

    attrelation = heap_open(AttributeRelationId, RowExclusiveLock);

    tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

    if (!HeapTupleIsValid(tuple))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_COLUMN),
                errmsg("column \"%s\" of relation \"%s\" does not exist", colName, RelationGetRelationName(rel))));
    attrtuple = (Form_pg_attribute)GETSTRUCT(tuple);

    if (attrtuple->attnum <= 0)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot alter system column \"%s\"", colName)));

    /*
     * safety check: do not allow toasted storage modes unless column datatype
     * is TOAST-aware.
     */
    if (newstorage == 'p' || TypeIsToastable(attrtuple->atttypid))
        attrtuple->attstorage = newstorage;
    else
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("column data type %s can only have storage PLAIN", format_type_be(attrtuple->atttypid))));

    simple_heap_update(attrelation, &tuple->t_self, tuple);

    /* keep system catalog indexes current */
    CatalogUpdateIndexes(attrelation, tuple);

    heap_freetuple_ext(tuple);

    heap_close(attrelation, RowExclusiveLock);
}

/*
 * ALTER TABLE DROP COLUMN
 *
 * DROP COLUMN cannot use the normal ALTER TABLE recursion mechanism,
 * because we have to decide at runtime whether to recurse or not depending
 * on whether attinhcount goes to zero or not.	(We can't check this in a
 * static pre-pass because it won't handle multiple inheritance situations
 * correctly.)
 */
static void ATPrepDropColumn(
    List** wqueue, Relation rel, bool recurse, bool recursing, AlterTableCmd* cmd, LOCKMODE lockmode)
{
    if (rel->rd_rel->reloftype && !recursing)
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("cannot drop column from typed table")));

    if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
        ATTypedTableRecursion(wqueue, rel, cmd, lockmode);

    if (recurse)
        cmd->subtype = AT_DropColumnRecurse;
}
/*
 * Brief        : Check if the input column is the last column of the input Relation
 * Input        : rel, the input relation
 *                attrnum, the input attribute number which is start from 1
 * Output       : None
 * Return Value : true if it's the last column of the input relation, false otherwise
 */
static bool CheckLastColumn(Relation rel, AttrNumber attrnum)
{
    for (int col = 0; col < rel->rd_att->natts; ++col) {
        if (rel->rd_att->attrs[col]->attisdropped)
            continue;
        if (col != (attrnum - 1)) {
            return false;
        }
    }
    return true;
}

static void ATExecDropColumn(List** wqueue, Relation rel, const char* colName, DropBehavior behavior, bool recurse,
    bool recursing, bool missing_ok, LOCKMODE lockmode)
{
    HeapTuple tuple;
    Form_pg_attribute targetatt;
    AttrNumber attnum;
    List* children = NIL;
    ObjectAddress object;

    /* At top level, permission check was done in ATPrepCmd, else do it */
    if (recursing)
        ATSimplePermissions(rel, ATT_TABLE);

    /*
     * get the number of the attribute
     */
    tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
    if (!HeapTupleIsValid(tuple)) {
        if (!missing_ok) {
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                    errmsg("column \"%s\" of relation \"%s\" does not exist", colName, RelationGetRelationName(rel))));
        } else {
            ereport(NOTICE,
                (errmsg("column \"%s\" of relation \"%s\" does not exist, skipping",
                    colName,
                    RelationGetRelationName(rel))));
            return;
        }
    }
    targetatt = (Form_pg_attribute)GETSTRUCT(tuple);

    attnum = targetatt->attnum;

    /*
     * column of a partitioned table's partition key can not be dropped
     */
    if (is_partition_column(rel, attnum)) {
        ereport(
            ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot drop partitioning column \"%s\"", colName)));
    }

    /* Can't drop a system attribute, except OID */
    if (attnum <= 0 && attnum != ObjectIdAttributeNumber)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot drop system column \"%s\"", colName)));

    /* Don't drop inherited columns */
    if (targetatt->attinhcount > 0 && !recursing)
        ereport(
            ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION), errmsg("cannot drop inherited column \"%s\"", colName)));

    ReleaseSysCache(tuple);

    /*
     * For a table, we don't allow to drop all the column.
     * We have to check if the drop column is the last column.
     * If it is, not allow to drop it.
     */
    if (GetLocatorType(rel->rd_id) != LOCATOR_TYPE_HASH) {
        bool lastColumn = CheckLastColumn(rel, attnum);
        if (lastColumn) {
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("must have at least one column")));
        }
    }

    /*
     * Propagate to children as appropriate.  Unlike most other ALTER
     * routines, we have to do this one level of recursion at a time; we can't
     * use find_all_inheritors to do it in one pass.
     */
    if (RelationIsPAXFormat(rel)) {
        /* Add column for delta table. */
        children = lappend_oid(children, RelationGetDeltaRelId(rel));

        /* Get the lock to synchronize against concurrent drop. */
        LockRelationOid(RelationGetDeltaRelId(rel), lockmode);
        elog(DEBUG1,
            "[GET LOCK] Get the lock %d successfully on delta table of %s for altering operator.",
            lockmode,
            RelationGetRelationName(rel));
    } else if (g_instance.attr.attr_storage.enable_delta_store && RelationIsCUFormat(rel)) {
        /*
         * add cstore relation delta table to recurse, if col support inherit feture
         * we also need call find_inheritance_children as below
         */
        children = find_cstore_delta(rel, lockmode);
    } else {
        children = find_inheritance_children(RelationGetRelid(rel), lockmode);
    }

    if (children != NULL) {
        Relation attr_rel;
        ListCell* child = NULL;

        attr_rel = heap_open(AttributeRelationId, RowExclusiveLock);
        foreach (child, children) {
            Oid childrelid = lfirst_oid(child);
            Relation childrel;
            Form_pg_attribute childatt;

            /* find_inheritance_children already got lock */
            childrel = heap_open(childrelid, NoLock);
            CheckTableNotInUse(childrel, "ALTER TABLE");

            tuple = SearchSysCacheCopyAttName(childrelid, colName);
            if (!HeapTupleIsValid(tuple)) {
                /* shouldn't happen */
                Assert(0);
                ereport(ERROR,
                    (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                        errmsg("cache lookup failed for attribute \"%s\" of relation %u", colName, childrelid)));
            }
            childatt = (Form_pg_attribute)GETSTRUCT(tuple);

            /*
             * The detal table is not inherit table.
             */
            if (!RelationIsPAXFormat(rel) && !RelationIsCUFormat(rel) &&
                childatt->attinhcount <= 0) /* shouldn't happen */
                ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("relation %u has non-inherited attribute \"%s\"", childrelid, colName)));

            if (recurse) {
                if (RelationIsPAXFormat(rel) || RelationIsCUFormat(rel)) {
                    /*
                     * Delete this column of the delta table.
                     */
                    ATExecDropColumn(wqueue, childrel, colName, behavior, true, true, false, lockmode);
                } else if (childatt->attinhcount == 1 && !childatt->attislocal) {
                    /*
                     * If the child column has other definition sources, just
                     * decrement its inheritance count; if not, recurse to delete it.
                     * 
                     * Time to delete this child column, too
                     */
                    ATExecDropColumn(wqueue, childrel, colName, behavior, true, true, false, lockmode);
                } else {
                    /* Child column must survive my deletion */
                    childatt->attinhcount--;

                    simple_heap_update(attr_rel, &tuple->t_self, tuple);

                    /* keep the system catalog indexes current */
                    CatalogUpdateIndexes(attr_rel, tuple);

                    /* Make update visible */
                    CommandCounterIncrement();
                }
            } else {
                /*
                 * If we were told to drop ONLY in this table (no recursion),
                 * we need to mark the inheritors' attributes as locally
                 * defined rather than inherited.
                 */
                childatt->attinhcount--;
                childatt->attislocal = true;

                simple_heap_update(attr_rel, &tuple->t_self, tuple);

                /* keep the system catalog indexes current */
                CatalogUpdateIndexes(attr_rel, tuple);

                /* Make update visible */
                CommandCounterIncrement();
            }

            heap_freetuple_ext(tuple);

            heap_close(childrel, NoLock);
        }
        heap_close(attr_rel, RowExclusiveLock);
    }

    /*
     * If the dropped column has partial cluster key, must to update
     * relhasclusterkey in pg_class.
     */
    if (rel->rd_rel->relhasclusterkey && colHasPartialClusterKey(rel, attnum)) {
        SetRelHasClusterKey(rel, false);
    }

    /*
     * Delete the dependent objects in order and update the rel catalog
     */
    object.classId = RelationRelationId;
    object.objectId = RelationGetRelid(rel);
    object.objectSubId = attnum;

    performDeletion(&object, behavior, 0);

    /*
     * If it's a column table, when we drop the column, we also need to delete the
     * column information from the cudesc, and put the column file into the pending delete.
     */
    if (RelationIsCUFormat(rel)) {
        CStoreRelDropColumn(rel, attnum, rel->rd_rel->relowner);
    }
    /*
     * If we dropped the OID column, must adjust pg_class.relhasoids and tell
     * Phase 3 to physically get rid of the column.  We formerly left the
     * column in place physically, but this caused subtle problems.  See
     * http://archives.postgresql.org/pgsql-hackers/2009-02/msg00363.php
     */
    if (attnum == ObjectIdAttributeNumber) {
        Relation class_rel;
        Form_pg_class tuple_class;
        AlteredTableInfo* tab = NULL;

        class_rel = heap_open(RelationRelationId, RowExclusiveLock);

        tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(RelationGetRelid(rel)));
        if (!HeapTupleIsValid(tuple)) {
            ereport(ERROR,
                (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                    errmsg("cache lookup failed for relation %u", RelationGetRelid(rel))));
        }
        tuple_class = (Form_pg_class)GETSTRUCT(tuple);

        tuple_class->relhasoids = false;
        simple_heap_update(class_rel, &tuple->t_self, tuple);

        /* Keep the catalog indexes up to date */
        CatalogUpdateIndexes(class_rel, tuple);

        heap_close(class_rel, RowExclusiveLock);

        /* Find or create work queue entry for this table */
        tab = ATGetQueueEntry(wqueue, rel);

        /* Tell Phase 3 to physically remove the OID column */
        tab->rewrite = true;
    }
}

/*
 * ALTER TABLE ADD INDEX
 *
 * There is no such command in the grammar, but parse_utilcmd.c converts
 * UNIQUE and PRIMARY KEY constraints into AT_AddIndex subcommands.  This lets
 * us schedule creation of the index at the appropriate time during ALTER.
 */
static void ATExecAddIndex(AlteredTableInfo* tab, Relation rel, IndexStmt* stmt, bool is_rebuild, LOCKMODE lockmode)
{
    bool check_rights = false;
    bool skip_build = false;
    bool quiet = false;
    Oid new_index;

    Assert(IsA(stmt, IndexStmt));
    Assert(!stmt->concurrent);

    /* suppress schema rights check when rebuilding existing index */
    check_rights = !is_rebuild;
    /* skip index build if phase 3 will do it or we're reusing an old one */
    skip_build = tab->rewrite || OidIsValid(stmt->oldNode);
    /* suppress notices when rebuilding existing index */
    quiet = is_rebuild;

    /* The IndexStmt has already been through transformIndexStmt */
    WaitState oldStatus = pgstat_report_waitstatus(STATE_CREATE_INDEX);
    new_index = DefineIndex(RelationGetRelid(rel),
        stmt,
        InvalidOid, /* no predefined OID */
        true,       /* is_alter_table */
        check_rights,
        skip_build,
        quiet);
    (void)pgstat_report_waitstatus(oldStatus);

    /*
     * If TryReuseIndex() stashed a relfilenode for us, we used it for the new
     * index instead of building from scratch.	The DROP of the old edition of
     * this index will have scheduled the storage for deletion at commit, so
     * cancel that pending deletion.
     */
    if (OidIsValid(stmt->oldNode)) {
        Relation irel = index_open(new_index, NoLock);

        if (!stmt->isPartitioned) {
            RelationPreserveStorage(irel->rd_node, true);
        } else {
            List* partOids = NIL;
            ListCell* cell = NULL;
            Partition partition = NULL;
            Oid partOid = InvalidOid;
            Oid partIndexOid = InvalidOid;

            partOids = relationGetPartitionOidList(rel);
            foreach (cell, partOids) {
                partOid = lfirst_oid(cell);
                partIndexOid = getPartitionIndexOid(RelationGetRelid(irel), partOid);

                partition = partitionOpen(irel, partIndexOid, NoLock);
                RelationPreserveStorage(partition->pd_node, true);
                partitionClose(irel, partition, NoLock);
            }
            releasePartitionOidList(&partOids);
        }

        index_close(irel, NoLock);
    }
}

/*
 * ALTER TABLE ADD CONSTRAINT USING INDEX
 */
static void ATExecAddIndexConstraint(AlteredTableInfo* tab, Relation rel, IndexStmt* stmt, LOCKMODE lockmode)
{
    Oid index_oid = stmt->indexOid;
    Relation indexRel;
    char* indexName = NULL;
    IndexInfo* indexInfo = NULL;
    char* constraintName = NULL;
    char constraintType;

    Assert(IsA(stmt, IndexStmt));
    Assert(OidIsValid(index_oid));
    Assert(stmt->isconstraint);

    indexRel = index_open(index_oid, AccessShareLock);

    indexName = pstrdup(RelationGetRelationName(indexRel));

    indexInfo = BuildIndexInfo(indexRel);

    /* this should have been checked at parse time */
    if (!indexInfo->ii_Unique)
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("index \"%s\" is not unique", indexName)));
    /*
     * Determine name to assign to constraint.	We require a constraint to
     * have the same name as the underlying index; therefore, use the index's
     * existing name as the default constraint name, and if the user
     * explicitly gives some other name for the constraint, rename the index
     * to match.
     */
    constraintName = stmt->idxname;
    if (constraintName == NULL)
        constraintName = indexName;
    else if (strcmp(constraintName, indexName) != 0) {
        ereport(NOTICE,
            (errmsg("ALTER TABLE / ADD CONSTRAINT USING INDEX will rename index \"%s\" to \"%s\"",
                indexName,
                constraintName)));
        RenameRelationInternal(index_oid, constraintName);
    }

    /* Extra checks needed if making primary key */
    if (stmt->primary)
        index_check_primary_key(rel, indexInfo, true);

    /* Note we currently don't support EXCLUSION constraints here */
    if (stmt->primary)
        constraintType = CONSTRAINT_PRIMARY;
    else
        constraintType = CONSTRAINT_UNIQUE;

    /* Create the catalog entries for the constraint */
    index_constraint_create(rel,
        index_oid,
        indexInfo,
        constraintName,
        constraintType,
        stmt->deferrable,
        stmt->initdeferred,
        stmt->primary,
        true, /* update pg_index */
        true, /* remove old dependencies */
        (g_instance.attr.attr_common.allowSystemTableMods || u_sess->attr.attr_common.IsInplaceUpgrade));

    index_close(indexRel, NoLock);
}

/*
 * ALTER TABLE ADD CONSTRAINT
 */
static void ATExecAddConstraint(List** wqueue, AlteredTableInfo* tab, Relation rel, Constraint* newConstraint,
    bool recurse, bool is_readd, LOCKMODE lockmode)
{
    Assert(IsA(newConstraint, Constraint));

    if (RelationIsColStore(rel) && !CSTORE_SUPPORT_CONSTRAINT(newConstraint->contype))
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("column store unsupport constraint \"%s\"", GetConstraintType(newConstraint->contype))));
    /*
     * Currently, we only expect to see CONSTR_CHECK and CONSTR_FOREIGN nodes
     * arriving here (see the preprocessing done in parse_utilcmd.c).  Use a
     * switch anyway to make it easier to add more code later.
     */
    switch (newConstraint->contype) {
        case CONSTR_CHECK:
            ATAddCheckConstraint(wqueue, tab, rel, newConstraint, recurse, false, is_readd, lockmode);
            break;

        case CONSTR_FOREIGN:

            /*
             * Note that we currently never recurse for FK constraints, so the
             * "recurse" flag is silently ignored.
             *
             * Assign or validate constraint name
             */
            if (newConstraint->conname) {
                if (ConstraintNameIsUsed(
                        CONSTRAINT_RELATION, RelationGetRelid(rel), RelationGetNamespace(rel), newConstraint->conname))
                    ereport(ERROR,
                        (errcode(ERRCODE_DUPLICATE_OBJECT),
                            errmsg("constraint \"%s\" for relation \"%s\" already exists",
                                newConstraint->conname,
                                RelationGetRelationName(rel))));
            } else
                newConstraint->conname = ChooseConstraintName(RelationGetRelationName(rel),
                    strVal(linitial(newConstraint->fk_attrs)),
                    "fkey",
                    RelationGetNamespace(rel),
                    NIL);

            ATAddForeignKeyConstraint(tab, rel, newConstraint, lockmode);
            break;
        case CONSTR_CLUSTER:
            if (rel->rd_rel->relhasclusterkey && !is_readd)
                ereport(ERROR,
                    (errcode(ERRCODE_DUPLICATE_OBJECT),
                        errmsg("cluster key for relation \"%s\" already exists", RelationGetRelationName(rel))));
            else
                AddRelClusterConstraints(rel, list_make1(newConstraint));

            break;
        default: {
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized constraint type: %d", (int)newConstraint->contype)));
        }
    }
}

/*
 * Add a check constraint to a single table and its children
 *
 * Subroutine for ATExecAddConstraint.
 *
 * We must recurse to child tables during execution, rather than using
 * ALTER TABLE's normal prep-time recursion.  The reason is that all the
 * constraints *must* be given the same name, else they won't be seen as
 * related later.  If the user didn't explicitly specify a name, then
 * AddRelationNewConstraints would normally assign different names to the
 * child constraints.  To fix that, we must capture the name assigned at
 * the parent table and pass that down.
 *
 * When re-adding a previously existing constraint (during ALTER COLUMN TYPE),
 * we don't need to recurse here, because recursion will be carried out at a
 * higher level; the constraint name issue doesn't apply because the names
 * have already been assigned and are just being re-used.  We need a separate
 * "is_readd" flag for that; just setting recurse=false would result in an
 * error if there are child tables.
 */
static void ATAddCheckConstraint(List** wqueue, AlteredTableInfo* tab, Relation rel, Constraint* constr, bool recurse,
    bool recursing, bool is_readd, LOCKMODE lockmode)
{
    List* newcons = NIL;
    ListCell* lcon = NULL;
    List* children = NIL;
    ListCell* child = NULL;

    /* At top level, permission check was done in ATPrepCmd, else do it */
    if (recursing)
        ATSimplePermissions(rel, ATT_TABLE);

    /*
     * Call AddRelationNewConstraints to do the work, making sure it works on
     * a copy of the Constraint so transformExpr can't modify the original. It
     * returns a list of cooked constraints.
     *
     * If the constraint ends up getting merged with a pre-existing one, it's
     * omitted from the returned list, which is what we want: we do not need
     * to do any validation work.  That can only happen at child tables,
     * though, since we disallow merging at the top level.
     */
    newcons = AddRelationNewConstraints(rel,
        NIL,
        list_make1(copyObject(constr)),
        recursing,   /* allow_merge */
        !recursing); /* is_local */

    /* Add each to-be-validated constraint to Phase 3's queue */
    foreach (lcon, newcons) {
        CookedConstraint* ccon = (CookedConstraint*)lfirst(lcon);

        if (!ccon->skip_validation) {
            NewConstraint* newcon = NULL;

            newcon = (NewConstraint*)palloc0(sizeof(NewConstraint));
            newcon->name = ccon->name;
            newcon->contype = ccon->contype;
            /* ExecQual wants implicit-AND format */
            newcon->qual = (Node*)make_ands_implicit((Expr*)ccon->expr);

            tab->constraints = lappend(tab->constraints, newcon);
        }

        /* Save the actually assigned name if it was defaulted */
        if (constr->conname == NULL)
            constr->conname = ccon->name;
    }

    /* At this point we must have a locked-down name to use */
    Assert(constr->conname != NULL);

    /* Advance command counter in case same table is visited multiple times */
    CommandCounterIncrement();

    /*
     * If the constraint got merged with an existing constraint, we're done.
     * We mustn't recurse to child tables in this case, because they've
     * already got the constraint, and visiting them again would lead to an
     * incorrect value for coninhcount.
     */
    if (newcons == NIL)
        return;

    /*
     * If adding a NO INHERIT constraint, no need to find our children.
     * Likewise, in a re-add operation, we don't need to recurse (that will be
     * handled at higher levels).
     */
    if (constr->is_no_inherit || is_readd)
        return;
    /*
     * Propagate to children as appropriate.  Unlike most other ALTER
     * routines, we have to do this one level of recursion at a time; we can't
     * use find_all_inheritors to do it in one pass.
     */
    children = find_inheritance_children(RelationGetRelid(rel), lockmode);

    /*
     * Check if ONLY was specified with ALTER TABLE.  If so, allow the
     * contraint creation only if there are no children currently.	Error out
     * otherwise.
     */
    if (!recurse && children != NIL)
        ereport(
            ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION), errmsg("constraint must be added to child tables too")));

    foreach (child, children) {
        Oid childrelid = lfirst_oid(child);
        Relation childrel;
        AlteredTableInfo* childtab = NULL;

        /* find_inheritance_children already got lock */
        childrel = heap_open(childrelid, NoLock);
        CheckTableNotInUse(childrel, "ALTER TABLE");

        /* Find or create work queue entry for this table */
        childtab = ATGetQueueEntry(wqueue, childrel);

        /* Recurse to child */
        ATAddCheckConstraint(wqueue, childtab, childrel, constr, recurse, true, is_readd, lockmode);

        heap_close(childrel, NoLock);
    }
}

/*
 * Add a foreign-key constraint to a single table
 *
 * Subroutine for ATExecAddConstraint.	Must already hold exclusive
 * lock on the rel, and have done appropriate validity checks for it.
 * We do permissions checks here, however.
 */
static void ATAddForeignKeyConstraint(AlteredTableInfo* tab, Relation rel, Constraint* fkconstraint, LOCKMODE lockmode)
{
    Relation pkrel;
    int16 pkattnum[INDEX_MAX_KEYS];
    int16 fkattnum[INDEX_MAX_KEYS];
    Oid pktypoid[INDEX_MAX_KEYS];
    Oid fktypoid[INDEX_MAX_KEYS];
    Oid opclasses[INDEX_MAX_KEYS];
    Oid pfeqoperators[INDEX_MAX_KEYS];
    Oid ppeqoperators[INDEX_MAX_KEYS];
    Oid ffeqoperators[INDEX_MAX_KEYS];
    int i;
    int numfks, numpks;
    Oid indexOid;
    Oid constrOid;
    bool old_check_ok = false;
    ListCell* old_pfeqop_item = list_head(fkconstraint->old_conpfeqop);

    /*
     * Grab an exclusive lock on the pk table, so that someone doesn't delete
     * rows out from under us. (Although a lesser lock would do for that
     * purpose, we'll need exclusive lock anyway to add triggers to the pk
     * table; trying to start with a lesser lock will just create a risk of
     * deadlock.)
     */
    if (OidIsValid(fkconstraint->old_pktable_oid))
        pkrel = heap_open(fkconstraint->old_pktable_oid, AccessExclusiveLock);
    else
        pkrel = heap_openrv(fkconstraint->pktable, AccessExclusiveLock);

    /*
     * Validity checks (permission checks wait till we have the column
     * numbers)
     */
    if (pkrel->rd_rel->relkind != RELKIND_RELATION)
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("referenced relation \"%s\" is not a table", RelationGetRelationName(pkrel))));

    if (!g_instance.attr.attr_common.allowSystemTableMods && !u_sess->attr.attr_common.IsInplaceUpgrade &&
        IsSystemRelation(pkrel))
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                errmsg("permission denied: \"%s\" is a system catalog", RelationGetRelationName(pkrel))));

    if (RELATION_IS_PARTITIONED(pkrel))
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("Invalid FOREIGN KEY constraints"),
                errdetail("Partitioned table cannot be referenced table")));

    /*
     * References from permanent or unlogged tables to temp tables, and from
     * permanent tables to unlogged tables, are disallowed because the
     * referenced data can vanish out from under us.  References from temp
     * tables to any other table type are also disallowed, because other
     * backends might need to run the RI triggers on the perm table, but they
     * can't reliably see tuples in the local buffers of other backends.
     */
    switch (rel->rd_rel->relpersistence) {
        case RELPERSISTENCE_PERMANENT:
            if (pkrel->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg("constraints on permanent tables may reference only permanent tables")));
            break;
        case RELPERSISTENCE_UNLOGGED:
            if (pkrel->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT &&
                pkrel->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg("constraints on unlogged tables may reference only permanent or unlogged tables")));
            break;
        case RELPERSISTENCE_TEMP:
            if (pkrel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg("constraints on temporary tables may reference only temporary tables")));
            if (!RelationIsLocalTemp(pkrel) || !RelationIsLocalTemp(rel))
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                        errmsg("constraints on temporary tables must involve temporary tables of this session")));
            break;
        case RELPERSISTENCE_GLOBAL_TEMP:
            if (pkrel->rd_rel->relpersistence != RELPERSISTENCE_GLOBAL_TEMP) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                         errmsg("constraints on global temporary tables may reference only global temporary tables")));
            }
            break;
        default:
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized table type: %d", (int)rel->rd_rel->relpersistence)));       
    }

    /*
     * Look up the referencing attributes to make sure they exist, and record
     * their attnums and type OIDs.
     */
    errno_t rc;
    rc = memset_s(pkattnum, sizeof(pkattnum), 0, sizeof(pkattnum));
    securec_check(rc, "", "");
    rc = memset_s(fkattnum, sizeof(fkattnum), 0, sizeof(fkattnum));
    securec_check(rc, "", "");
    rc = memset_s(pktypoid, sizeof(pktypoid), 0, sizeof(pktypoid));
    securec_check(rc, "", "");
    rc = memset_s(fktypoid, sizeof(fktypoid), 0, sizeof(fktypoid));
    securec_check(rc, "", "");
    rc = memset_s(opclasses, sizeof(opclasses), 0, sizeof(opclasses));
    securec_check(rc, "", "");
    rc = memset_s(pfeqoperators, sizeof(pfeqoperators), 0, sizeof(pfeqoperators));
    securec_check(rc, "", "");
    rc = memset_s(ppeqoperators, sizeof(ppeqoperators), 0, sizeof(ppeqoperators));
    securec_check(rc, "", "");
    rc = memset_s(ffeqoperators, sizeof(ffeqoperators), 0, sizeof(ffeqoperators));
    securec_check(rc, "", "");

    numfks = transformColumnNameList(RelationGetRelid(rel), fkconstraint->fk_attrs, fkattnum, fktypoid);

    /*
     * If the attribute list for the referenced table was omitted, lookup the
     * definition of the primary key and use it.  Otherwise, validate the
     * supplied attribute list.  In either case, discover the index OID and
     * index opclasses, and the attnums and type OIDs of the attributes.
     */
    if (fkconstraint->pk_attrs == NIL) {
        numpks = transformFkeyGetPrimaryKey(pkrel, &indexOid, &fkconstraint->pk_attrs, pkattnum, pktypoid, opclasses);
    } else {
        numpks = transformColumnNameList(RelationGetRelid(pkrel), fkconstraint->pk_attrs, pkattnum, pktypoid);
        /* Look for an index matching the column list */
        indexOid = transformFkeyCheckAttrs(pkrel, numpks, pkattnum, opclasses);
    }

    /*
     * Now we can check permissions.
     */
    checkFkeyPermissions(pkrel, pkattnum, numpks);
    checkFkeyPermissions(rel, fkattnum, numfks);

    /*
     * Look up the equality operators to use in the constraint.
     *
     * Note that we have to be careful about the difference between the actual
     * PK column type and the opclass' declared input type, which might be
     * only binary-compatible with it.	The declared opcintype is the right
     * thing to probe pg_amop with.
     */
    if (numfks != numpks)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_FOREIGN_KEY),
                errmsg("number of referencing and referenced columns for foreign key disagree")));

    /*
     * On the strength of a previous constraint, we might avoid scanning
     * tables to validate this one.  See below.
     */
    old_check_ok = (fkconstraint->old_conpfeqop != NIL);
    Assert(!old_check_ok || numfks == list_length(fkconstraint->old_conpfeqop));

    for (i = 0; i < numpks; i++) {
        Oid pktype = pktypoid[i];
        Oid fktype = fktypoid[i];
        Oid fktyped;
        HeapTuple cla_ht;
        Form_pg_opclass cla_tup;
        Oid amid;
        Oid opfamily;
        Oid opcintype;
        Oid pfeqop;
        Oid ppeqop;
        Oid ffeqop;
        int16 eqstrategy;
        Oid pfeqop_right;

        /* We need several fields out of the pg_opclass entry */
        cla_ht = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclasses[i]));
        if (!HeapTupleIsValid(cla_ht)) {
            ereport(ERROR,
                (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for opclass %u", opclasses[i])));
        }
        cla_tup = (Form_pg_opclass)GETSTRUCT(cla_ht);
        amid = cla_tup->opcmethod;
        opfamily = cla_tup->opcfamily;
        opcintype = cla_tup->opcintype;
        ReleaseSysCache(cla_ht);

        /*
         * Check it's a btree; currently this can never fail since no other
         * index AMs support unique indexes.  If we ever did have other types
         * of unique indexes, we'd need a way to determine which operator
         * strategy number is equality.  (Is it reasonable to insist that
         * every such index AM use btree's number for equality?)
         */
        if (amid != BTREE_AM_OID)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("only b-tree indexes are supported for foreign keys")));
        eqstrategy = BTEqualStrategyNumber;

        /*
         * There had better be a primary equality operator for the index.
         * We'll use it for PK = PK comparisons.
         */
        ppeqop = get_opfamily_member(opfamily, opcintype, opcintype, eqstrategy);

        if (!OidIsValid(ppeqop))
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("missing operator %d(%u,%u) in opfamily %u", eqstrategy, opcintype, opcintype, opfamily)));
        /*
         * Are there equality operators that take exactly the FK type? Assume
         * we should look through any domain here.
         */
        fktyped = getBaseType(fktype);

        pfeqop = get_opfamily_member(opfamily, opcintype, fktyped, eqstrategy);
        if (OidIsValid(pfeqop)) {
            pfeqop_right = fktyped;
            ffeqop = get_opfamily_member(opfamily, fktyped, fktyped, eqstrategy);
        } else {
            /* keep compiler quiet */
            pfeqop_right = InvalidOid;
            ffeqop = InvalidOid;
        }

        if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop))) {
            /*
             * Otherwise, look for an implicit cast from the FK type to the
             * opcintype, and if found, use the primary equality operator.
             * This is a bit tricky because opcintype might be a polymorphic
             * type such as ANYARRAY or ANYENUM; so what we have to test is
             * whether the two actual column types can be concurrently cast to
             * that type.  (Otherwise, we'd fail to reject combinations such
             * as int[] and point[].)
             */
            Oid input_typeids[2];
            Oid target_typeids[2];

            input_typeids[0] = pktype;
            input_typeids[1] = fktype;
            target_typeids[0] = opcintype;
            target_typeids[1] = opcintype;
            if (can_coerce_type(2, input_typeids, target_typeids, COERCION_IMPLICIT)) {
                pfeqop = ffeqop = ppeqop;
                pfeqop_right = opcintype;
            }
        }

        if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop)))
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("foreign key constraint \"%s\" "
                           "cannot be implemented",
                        fkconstraint->conname),
                    errdetail("Key columns \"%s\" and \"%s\" "
                              "are of incompatible types: %s and %s.",
                        strVal(list_nth(fkconstraint->fk_attrs, i)),
                        strVal(list_nth(fkconstraint->pk_attrs, i)),
                        format_type_be(fktype),
                        format_type_be(pktype))));

        if (old_check_ok) {
            /*
             * When a pfeqop changes, revalidate the constraint.  We could
             * permit intra-opfamily changes, but that adds subtle complexity
             * without any concrete benefit for core types.  We need not
             * assess ppeqop or ffeqop, which RI_Initial_Check() does not use.
             */
            old_check_ok = (pfeqop == lfirst_oid(old_pfeqop_item));
            old_pfeqop_item = lnext(old_pfeqop_item);
        }
        if (old_check_ok) {
            Oid old_fktype;
            Oid new_fktype;
            CoercionPathType old_pathtype;
            CoercionPathType new_pathtype;
            Oid old_castfunc;
            Oid new_castfunc;

            /*
             * Identify coercion pathways from each of the old and new FK-side
             * column types to the right (foreign) operand type of the pfeqop.
             * We may assume that pg_constraint.conkey is not changing.
             */
            old_fktype = tab->oldDesc->attrs[fkattnum[i] - 1]->atttypid;
            new_fktype = fktype;
            old_pathtype = findFkeyCast(pfeqop_right, old_fktype, &old_castfunc);
            new_pathtype = findFkeyCast(pfeqop_right, new_fktype, &new_castfunc);

            /*
             * Upon a change to the cast from the FK column to its pfeqop
             * operand, revalidate the constraint.	For this evaluation, a
             * binary coercion cast is equivalent to no cast at all.  While
             * type implementors should design implicit casts with an eye
             * toward consistency of operations like equality, we cannot
             * assume here that they have done so.
             *
             * A function with a polymorphic argument could change behavior
             * arbitrarily in response to get_fn_expr_argtype().  Therefore,
             * when the cast destination is polymorphic, we only avoid
             * revalidation if the input type has not changed at all.  Given
             * just the core data types and operator classes, this requirement
             * prevents no would-be optimizations.
             *
             * If the cast converts from a base type to a domain thereon, then
             * that domain type must be the opcintype of the unique index.
             * Necessarily, the primary key column must then be of the domain
             * type.  Since the constraint was previously valid, all values on
             * the foreign side necessarily exist on the primary side and in
             * turn conform to the domain.	Consequently, we need not treat
             * domains specially here.
             *
             * Since we require that all collations share the same notion of
             * equality (which they do, because texteq reduces to bitwise
             * equality), we don't compare collation here.
             *
             * We need not directly consider the PK type.  It's necessarily
             * binary coercible to the opcintype of the unique index column,
             * and ri_triggers.c will only deal with PK datums in terms of
             * that opcintype.	Changing the opcintype also changes pfeqop.
             */
            old_check_ok = (new_pathtype == old_pathtype && new_castfunc == old_castfunc &&
                            (!IsPolymorphicType(pfeqop_right) || new_fktype == old_fktype));
        }

        pfeqoperators[i] = pfeqop;
        ppeqoperators[i] = ppeqop;
        ffeqoperators[i] = ffeqop;
    }

#ifdef PGXC
    /* Check the shippability of this foreign key */
    if (IS_PGXC_COORDINATOR) {
        List* childRefs = NIL;
        List* parentRefs = NIL;

        /* Prepare call for shippability check */
        for (i = 0; i < numfks; i++)
            childRefs = lappend_int(childRefs, fkattnum[i]);
        for (i = 0; i < numpks; i++)
            parentRefs = lappend_int(parentRefs, pkattnum[i]);

        /* Now check shippability for this foreign key */
        if (!pgxc_check_fk_shippability(GetRelationLocInfo(RelationGetRelid(pkrel)),
                GetRelationLocInfo(RelationGetRelid(rel)),
                parentRefs,
                childRefs))
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("Cannot create foreign key whose evaluation cannot be enforced to remote nodes")));
    }
#endif

    /*
     * Record the FK constraint in pg_constraint.
     */
    constrOid = CreateConstraintEntry(fkconstraint->conname,
        RelationGetNamespace(rel),
        CONSTRAINT_FOREIGN,
        fkconstraint->deferrable,
        fkconstraint->initdeferred,
        fkconstraint->initially_valid,
        RelationGetRelid(rel),
        fkattnum,
        numfks,
        InvalidOid, /* not a domain constraint */
        indexOid,
        RelationGetRelid(pkrel),
        pkattnum,
        pfeqoperators,
        ppeqoperators,
        ffeqoperators,
        numpks,
        fkconstraint->fk_upd_action,
        fkconstraint->fk_del_action,
        fkconstraint->fk_matchtype,
        NULL, /* no exclusion constraint */
        NULL, /* no check constraint */
        NULL,
        NULL,
        true,                           /* islocal */
        0,                              /* inhcount */
        true,                           /* isnoinherit */
        fkconstraint->inforConstraint); /* @hdfs informational constraint */

    /*
     * Create the triggers that will enforce the constraint.
     */
    createForeignKeyTriggers(rel, RelationGetRelid(pkrel), fkconstraint, constrOid, indexOid);

    /*
     * Tell Phase 3 to check that the constraint is satisfied by existing
     * rows. We can skip this during table creation, when requested explicitly
     * by specifying NOT VALID in an ADD FOREIGN KEY command, and when we're
     * recreating a constraint following a SET DATA TYPE operation that did
     * not impugn its validity.
     */
    if (!old_check_ok && !fkconstraint->skip_validation) {
        NewConstraint* newcon = (NewConstraint*)palloc0(sizeof(NewConstraint));
        newcon->name = fkconstraint->conname;
        newcon->contype = CONSTR_FOREIGN;
        newcon->refrelid = RelationGetRelid(pkrel);
        newcon->refindid = indexOid;
        newcon->conid = constrOid;
        newcon->qual = (Node*)fkconstraint;

        tab->constraints = lappend(tab->constraints, newcon);
    }

    /*
     * Close pk table, but keep lock until we've committed.
     */
    heap_close(pkrel, NoLock);
}

/*
 * ALTER TABLE VALIDATE CONSTRAINT
 *
 * XXX The reason we handle recursion here rather than at Phase 1 is because
 * there's no good way to skip recursing when handling foreign keys: there is
 * no need to lock children in that case, yet we wouldn't be able to avoid
 * doing so at that level.
 */
static void ATExecValidateConstraint(Relation rel, char* constrName, bool recurse, bool recursing, LOCKMODE lockmode)
{
    Relation conrel;
    SysScanDesc scan;
    ScanKeyData key;
    HeapTuple tuple;
    Form_pg_constraint con = NULL;
    bool found = false;

    conrel = heap_open(ConstraintRelationId, RowExclusiveLock);

    /*
     * Find and check the target constraint
     */
    ScanKeyInit(
        &key, Anum_pg_constraint_conrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
    scan = systable_beginscan(conrel, ConstraintRelidIndexId, true, SnapshotNow, 1, &key);

    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        con = (Form_pg_constraint)GETSTRUCT(tuple);
        if (strcmp(NameStr(con->conname), constrName) == 0) {
            found = true;
            break;
        }
    }

    if (!found)
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg(
                    "constraint \"%s\" of relation \"%s\" does not exist", constrName, RelationGetRelationName(rel))));

    if (con->contype != CONSTRAINT_FOREIGN && con->contype != CONSTRAINT_CHECK)
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("constraint \"%s\" of relation \"%s\" is not a foreign key or check constraint",
                    constrName,
                    RelationGetRelationName(rel))));

    if (!con->convalidated) {
        HeapTuple copyTuple;
        Form_pg_constraint copy_con;

        if (con->contype == CONSTRAINT_FOREIGN) {
            Oid conid = HeapTupleGetOid(tuple);
            Relation refrel;

            /*
             * Triggers are already in place on both tables, so a concurrent
             * write that alters the result here is not possible. Normally we
             * can run a query here to do the validation, which would only
             * require AccessShareLock. In some cases, it is possible that we
             * might need to fire triggers to perform the check, so we take a
             * lock at RowShareLock level just in case.
             */
            refrel = heap_open(con->confrelid, RowShareLock);

            validateForeignKeyConstraint(constrName, rel, refrel, con->conindid, conid);
            heap_close(refrel, NoLock);

            /*
             * Foreign keys do not inherit, so we purposely ignore the
             * recursion bit here
             */
        } else if (con->contype == CONSTRAINT_CHECK) {
            List* children = NIL;
            ListCell* child = NULL;

            /*
             * If we're recursing, the parent has already done this, so skip
             * it.
             */
            if (!recursing)
                children = find_all_inheritors(RelationGetRelid(rel), lockmode, NULL);

            /*
             * For CHECK constraints, we must ensure that we only mark the
             * constraint as validated on the parent if it's already validated
             * on the children.
             *
             * We recurse before validating on the parent, to reduce risk of
             * deadlocks.
             */
            foreach (child, children) {
                Oid childoid = lfirst_oid(child);
                Relation childrel;

                if (childoid == RelationGetRelid(rel))
                    continue;

                /*
                 * If we are told not to recurse, there had better not be any
                 * child tables; else the addition would put them out of step.
                 */
                if (!recurse)
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                            errmsg("constraint must be validated on child tables too")));

                /* find_all_inheritors already got lock */
                childrel = heap_open(childoid, NoLock);

                ATExecValidateConstraint(childrel, constrName, false, true, lockmode);
                heap_close(childrel, NoLock);
            }

            if (!RelationIsPartitioned(rel)) {
                if (RELATION_CREATE_BUCKET(rel)) {
                    /* validate constraint for every buckets */
                    validateCheckConstraintForBucket(rel, NULL, tuple);
                } else {
                    validateCheckConstraint(rel, tuple);
                }
            } else {
                List* partitions = NIL;
                ListCell* cell = NULL;
                Partition partition = NULL;
                Relation partRel = NULL;

                partitions = relationGetPartitionList(rel, lockmode);
                foreach (cell, partitions) {
                    partition = (Partition)lfirst(cell);
                    if (RELATION_OWN_BUCKETKEY(rel)) {
                        /* validate constraint for every buckets */
                        validateCheckConstraintForBucket(rel, partition, tuple);
                    } else {
                        partRel = partitionGetRelation(rel, partition);

                        validateCheckConstraint(partRel, tuple);

                        releaseDummyRelation(&partRel);
                    }
                }
                releasePartitionList(rel, &partitions, lockmode);
            }

            /*
             * Invalidate relcache so that others see the new validated
             * constraint.
             */
            CacheInvalidateRelcache(rel);
        }

        /*
         * Now update the catalog, while we have the door open.
         */
        copyTuple = heap_copytuple(tuple);
        copy_con = (Form_pg_constraint)GETSTRUCT(copyTuple);
        copy_con->convalidated = true;
        simple_heap_update(conrel, &copyTuple->t_self, copyTuple);
        CatalogUpdateIndexes(conrel, copyTuple);
        heap_freetuple_ext(copyTuple);
    }

    systable_endscan(scan);

    heap_close(conrel, RowExclusiveLock);
}

/*
 * transformColumnNameList - transform list of column names
 *
 * Lookup each name and return its attnum and type OID
 */
static int transformColumnNameList(Oid relId, List* colList, int16* attnums, Oid* atttypids)
{
    ListCell* l = NULL;
    int attnum;

    attnum = 0;
    foreach (l, colList) {
        char* attname = strVal(lfirst(l));
        HeapTuple atttuple;

        atttuple = SearchSysCacheAttName(relId, attname);
        if (!HeapTupleIsValid(atttuple))
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                    errmsg("column \"%s\" referenced in foreign key constraint does not exist", attname)));
        if (attnum >= INDEX_MAX_KEYS)
            ereport(ERROR,
                (errcode(ERRCODE_TOO_MANY_COLUMNS),
                    errmsg("cannot have more than %d keys in a foreign key", INDEX_MAX_KEYS)));
        attnums[attnum] = ((Form_pg_attribute)GETSTRUCT(atttuple))->attnum;
        atttypids[attnum] = ((Form_pg_attribute)GETSTRUCT(atttuple))->atttypid;
        ReleaseSysCache(atttuple);
        attnum++;
    }

    return attnum;
}

/*
 * transformFkeyGetPrimaryKey -
 *
 *	Look up the names, attnums, and types of the primary key attributes
 *	for the pkrel.	Also return the index OID and index opclasses of the
 *	index supporting the primary key.
 *
 *	All parameters except pkrel are output parameters.	Also, the function
 *	return value is the number of attributes in the primary key.
 *
 *	Used when the column list in the REFERENCES specification is omitted.
 */
static int transformFkeyGetPrimaryKey(
    Relation pkrel, Oid* indexOid, List** attnamelist, int16* attnums, Oid* atttypids, Oid* opclasses)
{
    List* indexoidlist = NIL;
    ListCell* indexoidscan = NULL;
    HeapTuple indexTuple = NULL;
    Form_pg_index indexStruct = NULL;
    Datum indclassDatum;
    bool isnull = false;
    oidvector* indclass = NULL;
    int i;

    /*
     * Get the list of index OIDs for the table from the relcache, and look up
     * each one in the pg_index syscache until we find one marked primary key
     * (hopefully there isn't more than one such).  Insist it's valid, too.
     */
    *indexOid = InvalidOid;

    indexoidlist = RelationGetIndexList(pkrel);

    foreach (indexoidscan, indexoidlist) {
        Oid indexoid = lfirst_oid(indexoidscan);

        indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
        if (!HeapTupleIsValid(indexTuple)) {
            ereport(
                ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for index %u", indexoid)));
        }

        indexStruct = (Form_pg_index)GETSTRUCT(indexTuple);
        if (indexStruct->indisprimary && IndexIsValid(indexStruct)) {
            /*
             * Refuse to use a deferrable primary key.	This is per SQL spec,
             * and there would be a lot of interesting semantic problems if we
             * tried to allow it.
             */
            if (!indexStruct->indimmediate)
                ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("cannot use a deferrable primary key for referenced table \"%s\"",
                            RelationGetRelationName(pkrel))));

            *indexOid = indexoid;
            break;
        }
        ReleaseSysCache(indexTuple);
    }

    list_free_ext(indexoidlist);

    /*
     * Check that we found it
     */
    if (!OidIsValid(*indexOid))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("there is no primary key for referenced table \"%s\"", RelationGetRelationName(pkrel))));

    /* Must get indclass the hard way */
    indclassDatum = SysCacheGetAttr(INDEXRELID, indexTuple, Anum_pg_index_indclass, &isnull);
    Assert(!isnull);
    indclass = (oidvector*)DatumGetPointer(indclassDatum);

    /*
     * Now build the list of PK attributes from the indkey definition (we
     * assume a primary key cannot have expressional elements)
     */
    *attnamelist = NIL;
    for (i = 0; i < indexStruct->indnatts; i++) {
        int pkattno = indexStruct->indkey.values[i];

        attnums[i] = pkattno;
        atttypids[i] = attnumTypeId(pkrel, pkattno);
        opclasses[i] = indclass->values[i];
        *attnamelist = lappend(*attnamelist, makeString(pstrdup(NameStr(*attnumAttName(pkrel, pkattno)))));
    }

    ReleaseSysCache(indexTuple);

    return i;
}

/*
 * transformFkeyCheckAttrs -
 *
 *	Make sure that the attributes of a referenced table belong to a unique
 *	(or primary key) constraint.  Return the OID of the index supporting
 *	the constraint, as well as the opclasses associated with the index
 *	columns.
 */
static Oid transformFkeyCheckAttrs(Relation pkrel, int numattrs, int16* attnums, Oid* opclasses) /* output parameter */
{
    Oid indexoid = InvalidOid;
    bool found = false;
    bool found_deferrable = false;
    List* indexoidlist = NIL;
    ListCell* indexoidscan = NULL;

    /*
     * Get the list of index OIDs for the table from the relcache, and look up
     * each one in the pg_index syscache, and match unique indexes to the list
     * of attnums we are given.
     */
    indexoidlist = RelationGetIndexList(pkrel);

    foreach (indexoidscan, indexoidlist) {
        HeapTuple indexTuple;
        Form_pg_index indexStruct;
        int i, j;

        indexoid = lfirst_oid(indexoidscan);
        indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
        if (!HeapTupleIsValid(indexTuple)) {
            ereport(
                ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for index %u", indexoid)));
        }
        indexStruct = (Form_pg_index)GETSTRUCT(indexTuple);

        /*
         * Must have the right number of columns; must be unique and not a
         * partial index; forget it if there are any expressions, too. Invalid
         * indexes are out as well.
         */
        if (indexStruct->indnatts == numattrs && indexStruct->indisunique && IndexIsValid(indexStruct) &&
            heap_attisnull(indexTuple, Anum_pg_index_indpred, NULL) &&
            heap_attisnull(indexTuple, Anum_pg_index_indexprs, NULL)) {
            /* Must get indclass the hard way */
            Datum indclassDatum;
            bool isnull = false;
            oidvector* indclass = NULL;
            indclassDatum = SysCacheGetAttr(INDEXRELID, indexTuple, Anum_pg_index_indclass, &isnull);
            Assert(!isnull);
            indclass = (oidvector*)DatumGetPointer(indclassDatum);
            /*
             * The given attnum list may match the index columns in any order.
             * Check that each list is a subset of the other.
             */
            for (i = 0; i < numattrs; i++) {
                found = false;
                for (j = 0; j < numattrs; j++) {
                    if (attnums[i] == indexStruct->indkey.values[j]) {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    break;
            }
            if (found) {
                for (i = 0; i < numattrs; i++) {
                    found = false;
                    for (j = 0; j < numattrs; j++) {
                        if (attnums[j] == indexStruct->indkey.values[i]) {
                            opclasses[j] = indclass->values[i];
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        break;
                }
            }

            /*
             * Refuse to use a deferrable unique/primary key.  This is per SQL
             * spec, and there would be a lot of interesting semantic problems
             * if we tried to allow it.
             */
            if (found && !indexStruct->indimmediate) {
                /*
                 * Remember that we found an otherwise matching index, so that
                 * we can generate a more appropriate error message.
                 */
                found_deferrable = true;
                found = false;
            }
        }
        ReleaseSysCache(indexTuple);
        if (found)
            break;
    }

    if (!found) {
        if (found_deferrable)
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                    errmsg("cannot use a deferrable unique constraint for referenced table \"%s\"",
                        RelationGetRelationName(pkrel))));
        else
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_FOREIGN_KEY),
                    errmsg("there is no unique constraint matching given keys for referenced table \"%s\"",
                        RelationGetRelationName(pkrel))));
    }

    list_free_ext(indexoidlist);

    return indexoid;
}

/*
 * findFkeyCast -
 *
 *	Wrapper around find_coercion_pathway() for ATAddForeignKeyConstraint().
 *	Caller has equal regard for binary coercibility and for an exact match.
 */
static CoercionPathType findFkeyCast(Oid targetTypeId, Oid sourceTypeId, Oid* funcid)
{
    CoercionPathType ret;

    if (targetTypeId == sourceTypeId) {
        ret = COERCION_PATH_RELABELTYPE;
        *funcid = InvalidOid;
    } else {
        ret = find_coercion_pathway(targetTypeId, sourceTypeId, COERCION_IMPLICIT, funcid);
        if (ret == COERCION_PATH_NONE)
            /* A previously-relied-upon cast is now gone. */
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_CHARACTER_VALUE_FOR_CAST),
                    errmsg("could not find cast from %u to %u", sourceTypeId, targetTypeId)));
    }

    return ret;
}

/* Permissions checks for ADD FOREIGN KEY */
static void checkFkeyPermissions(Relation rel, int16* attnums, int natts)
{
    Oid roleid = GetUserId();
    AclResult aclresult;
    int i;

    /* Okay if we have relation-level REFERENCES permission */
    aclresult = pg_class_aclcheck(RelationGetRelid(rel), roleid, ACL_REFERENCES);
    if (aclresult == ACLCHECK_OK)
        return;
    /* Else we must have REFERENCES on each column */
    for (i = 0; i < natts; i++) {
        aclresult = pg_attribute_aclcheck(RelationGetRelid(rel), attnums[i], roleid, ACL_REFERENCES);
        if (aclresult != ACLCHECK_OK)
            aclcheck_error(aclresult, ACL_KIND_CLASS, RelationGetRelationName(rel));
    }
}

/*
 * Scan the existing rows in a table to verify they meet a proposed
 * CHECK constraint.
 *
 * The caller must have opened and locked the relation appropriately.
 */
static void validateCheckConstraint(Relation rel, HeapTuple constrtup)
{
    Datum val;
    HeapTuple tuple = NULL;
    bool isnull = false;

    Form_pg_constraint constrForm = (Form_pg_constraint)GETSTRUCT(constrtup);
    EState* estate = CreateExecutorState();

    /*
     * XXX this tuple doesn't really come from a syscache, but this doesn't
     * matter to SysCacheGetAttr, because it only wants to be able to fetch
     * the tupdesc
     */
    val = SysCacheGetAttr(CONSTROID, constrtup, Anum_pg_constraint_conbin, &isnull);
    if (isnull)
        ereport(ERROR,
            (errcode(ERRCODE_UNEXPECTED_NULL_VALUE),
                errmsg("null conbin for constraint %u", HeapTupleGetOid(constrtup))));
    char* conbin = TextDatumGetCString(val);
    Expr* origexpr = (Expr*)stringToNode(conbin);
    List* exprstate = (List*)ExecPrepareExpr((Expr*)make_ands_implicit(origexpr), estate);
    ExprContext* econtext = GetPerTupleExprContext(estate);
    TupleDesc tupdesc = RelationGetDescr(rel);
    TupleTableSlot* slot = MakeSingleTupleTableSlot(tupdesc);

    econtext->ecxt_scantuple = slot;

    HeapScanDesc scan = heap_beginscan(rel, SnapshotNow, 0, NULL);

    /*
     * Switch to per-tuple memory context and reset it for each tuple
     * produced, so we don't leak memory.
     */
    MemoryContext oldcxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
        (void)ExecStoreTuple(tuple, slot, InvalidBuffer, false);

        if (!ExecQual(exprstate, econtext, true))
            ereport(ERROR,
                (errcode(ERRCODE_CHECK_VIOLATION),
                    errmsg("check constraint \"%s\" is violated by some row", NameStr(constrForm->conname))));

        ResetExprContext(econtext);
    }

    (void)MemoryContextSwitchTo(oldcxt);
    heap_endscan(scan);
    ExecDropSingleTupleTableSlot(slot);
    FreeExecutorState(estate);
}

/*
 * Validate check constraint for a list of bucket.
 *
 * The caller must have opened and locked the relation and(or) partition appropriately.
 */
static void validateCheckConstraintForBucket(Relation rel, Partition part, HeapTuple constrtup)
{
    Relation bucketRel = NULL;
    oidvector* bucketlist = searchHashBucketByOid(rel->rd_bucketoid);

    for (int i = 0; i < bucketlist->dim1; i++) {
        /* Open the bucket and do the real validate */
        bucketRel = bucketGetRelation(rel, part, bucketlist->values[i]);

        validateCheckConstraint(bucketRel, constrtup);

        bucketCloseRelation(bucketRel);
    }
}

/*
 * Scan the existing rows in a table to verify they meet a proposed FK
 * constraint.
 *
 * Caller must have opened and locked both relations appropriately.
 */
static void validateForeignKeyConstraint(char* conname, Relation rel, Relation pkrel, Oid pkindOid, Oid constraintOid)
{
    HeapScanDesc scan;
    HeapTuple tuple;
    Trigger trig;
    errno_t rc = EOK;

    ereport(DEBUG1, (errmsg("validating foreign key constraint \"%s\"", conname)));

    /*
     * Build a trigger call structure; we'll need it either way.
     */
    rc = memset_s(&trig, sizeof(trig), 0, sizeof(trig));
    securec_check(rc, "\0", "\0");
    trig.tgoid = InvalidOid;
    trig.tgname = conname;
    trig.tgenabled = TRIGGER_FIRES_ON_ORIGIN;
    trig.tgisinternal = TRUE;
    trig.tgconstrrelid = RelationGetRelid(pkrel);
    trig.tgconstrindid = pkindOid;
    trig.tgconstraint = constraintOid;
    trig.tgdeferrable = FALSE;
    trig.tginitdeferred = FALSE;
    /* we needn't fill in tgargs or tgqual */
    /*
     * See if we can do it with a single LEFT JOIN query.  A FALSE result
     * indicates we must proceed with the fire-the-trigger method.
     */
    if (RI_Initial_Check(&trig, rel, pkrel))
        return;

    /*
     * Scan through each tuple, calling RI_FKey_check_ins (insert trigger) as
     * if that tuple had just been inserted.  If any of those fail, it should
     * ereport(ERROR) and that's that.
     */
    scan = heap_beginscan(rel, SnapshotNow, 0, NULL);

    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
        FunctionCallInfoData fcinfo;
        TriggerData trigdata;

        /*
         * Make a call to the trigger function
         *
         * No parameters are passed, but we do set a context
         */
        rc = memset_s(&fcinfo, sizeof(fcinfo), 0, sizeof(fcinfo));
        securec_check(rc, "\0", "\0");
        rc = memset_s(&trigdata, sizeof(trigdata), 0, sizeof(trigdata));
        securec_check(rc, "\0", "\0");
        /*
         * We assume RI_FKey_check_ins won't look at flinfo...
         */
        trigdata.type = T_TriggerData;
        trigdata.tg_event = TRIGGER_EVENT_INSERT | TRIGGER_EVENT_ROW;
        trigdata.tg_relation = rel;
        trigdata.tg_trigtuple = tuple;
        trigdata.tg_newtuple = NULL;
        trigdata.tg_trigger = &trig;
        trigdata.tg_trigtuplebuf = scan->rs_cbuf;
        trigdata.tg_newtuplebuf = InvalidBuffer;

        fcinfo.context = (Node*)&trigdata;

        RI_FKey_check_ins(&fcinfo);
    }

    heap_endscan(scan);
}

static void CreateFKCheckTrigger(
    Oid myRelOid, Oid refRelOid, Constraint* fkconstraint, Oid constraintOid, Oid indexOid, bool on_insert)
{
    CreateTrigStmt* fk_trigger = NULL;

    /*
     * Note: for a self-referential FK (referencing and referenced tables are
     * the same), it is important that the ON UPDATE action fires before the
     * CHECK action, since both triggers will fire on the same row during an
     * UPDATE event; otherwise the CHECK trigger will be checking a non-final
     * state of the row.  Triggers fire in name order, so we ensure this by
     * using names like "RI_ConstraintTrigger_a_NNNN" for the action triggers
     * and "RI_ConstraintTrigger_c_NNNN" for the check triggers.
     */
    fk_trigger = makeNode(CreateTrigStmt);
    fk_trigger->trigname = "RI_ConstraintTrigger_c";
    fk_trigger->relation = NULL;
    fk_trigger->row = true;
    fk_trigger->timing = TRIGGER_TYPE_AFTER;

    /* Either ON INSERT or ON UPDATE */
    if (on_insert) {
        fk_trigger->funcname = SystemFuncName("RI_FKey_check_ins");
        fk_trigger->events = TRIGGER_TYPE_INSERT;
    } else {
        fk_trigger->funcname = SystemFuncName("RI_FKey_check_upd");
        fk_trigger->events = TRIGGER_TYPE_UPDATE;
    }

    fk_trigger->columns = NIL;
    fk_trigger->whenClause = NULL;
    fk_trigger->isconstraint = true;
    fk_trigger->deferrable = fkconstraint->deferrable;
    fk_trigger->initdeferred = fkconstraint->initdeferred;
    fk_trigger->constrrel = NULL;
    fk_trigger->args = NIL;

    (void)CreateTrigger(fk_trigger, NULL, myRelOid, refRelOid, constraintOid, indexOid, true);

    /* Make changes-so-far visible */
    CommandCounterIncrement();
}

/*
 * Create the triggers that implement an FK constraint.
 */
static void createForeignKeyTriggers(
    Relation rel, Oid refRelOid, Constraint* fkconstraint, Oid constraintOid, Oid indexOid)
{
    Oid myRelOid;
    CreateTrigStmt* fk_trigger = NULL;

    myRelOid = RelationGetRelid(rel);

    /* Make changes-so-far visible */
    CommandCounterIncrement();

    /*
     * Build and execute a CREATE CONSTRAINT TRIGGER statement for the ON
     * DELETE action on the referenced table.
     */
    fk_trigger = makeNode(CreateTrigStmt);
    fk_trigger->trigname = "RI_ConstraintTrigger_a";
    fk_trigger->relation = NULL;
    fk_trigger->row = true;
    fk_trigger->timing = TRIGGER_TYPE_AFTER;
    fk_trigger->events = TRIGGER_TYPE_DELETE;
    fk_trigger->columns = NIL;
    fk_trigger->whenClause = NULL;
    fk_trigger->isconstraint = true;
    fk_trigger->constrrel = NULL;
    switch (fkconstraint->fk_del_action) {
        case FKCONSTR_ACTION_NOACTION:
            fk_trigger->deferrable = fkconstraint->deferrable;
            fk_trigger->initdeferred = fkconstraint->initdeferred;
            fk_trigger->funcname = SystemFuncName("RI_FKey_noaction_del");
            break;
        case FKCONSTR_ACTION_RESTRICT:
            fk_trigger->deferrable = false;
            fk_trigger->initdeferred = false;
            fk_trigger->funcname = SystemFuncName("RI_FKey_restrict_del");
            break;
        case FKCONSTR_ACTION_CASCADE:
            fk_trigger->deferrable = false;
            fk_trigger->initdeferred = false;
            fk_trigger->funcname = SystemFuncName("RI_FKey_cascade_del");
            break;
        case FKCONSTR_ACTION_SETNULL:
            fk_trigger->deferrable = false;
            fk_trigger->initdeferred = false;
            fk_trigger->funcname = SystemFuncName("RI_FKey_setnull_del");
            break;
        case FKCONSTR_ACTION_SETDEFAULT:
            fk_trigger->deferrable = false;
            fk_trigger->initdeferred = false;
            fk_trigger->funcname = SystemFuncName("RI_FKey_setdefault_del");
            break;
        default:
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized FK action type: %d", (int)fkconstraint->fk_del_action)));
            break;
    }
    fk_trigger->args = NIL;

    (void)CreateTrigger(fk_trigger, NULL, refRelOid, myRelOid, constraintOid, indexOid, true);

    /* Make changes-so-far visible */
    CommandCounterIncrement();

    /*
     * Build and execute a CREATE CONSTRAINT TRIGGER statement for the ON
     * UPDATE action on the referenced table.
     */
    fk_trigger = makeNode(CreateTrigStmt);
    fk_trigger->trigname = "RI_ConstraintTrigger_a";
    fk_trigger->relation = NULL;
    fk_trigger->row = true;
    fk_trigger->timing = TRIGGER_TYPE_AFTER;
    fk_trigger->events = TRIGGER_TYPE_UPDATE;
    fk_trigger->columns = NIL;
    fk_trigger->whenClause = NULL;
    fk_trigger->isconstraint = true;
    fk_trigger->constrrel = NULL;
    switch (fkconstraint->fk_upd_action) {
        case FKCONSTR_ACTION_NOACTION:
            fk_trigger->deferrable = fkconstraint->deferrable;
            fk_trigger->initdeferred = fkconstraint->initdeferred;
            fk_trigger->funcname = SystemFuncName("RI_FKey_noaction_upd");
            break;
        case FKCONSTR_ACTION_RESTRICT:
            fk_trigger->deferrable = false;
            fk_trigger->initdeferred = false;
            fk_trigger->funcname = SystemFuncName("RI_FKey_restrict_upd");
            break;
        case FKCONSTR_ACTION_CASCADE:
            fk_trigger->deferrable = false;
            fk_trigger->initdeferred = false;
            fk_trigger->funcname = SystemFuncName("RI_FKey_cascade_upd");
            break;
        case FKCONSTR_ACTION_SETNULL:
            fk_trigger->deferrable = false;
            fk_trigger->initdeferred = false;
            fk_trigger->funcname = SystemFuncName("RI_FKey_setnull_upd");
            break;
        case FKCONSTR_ACTION_SETDEFAULT:
            fk_trigger->deferrable = false;
            fk_trigger->initdeferred = false;
            fk_trigger->funcname = SystemFuncName("RI_FKey_setdefault_upd");
            break;
        default:
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized FK action type: %d", (int)fkconstraint->fk_upd_action)));
            break;
    }
    fk_trigger->args = NIL;

    (void)CreateTrigger(fk_trigger, NULL, refRelOid, myRelOid, constraintOid, indexOid, true);

    /* Make changes-so-far visible */
    CommandCounterIncrement();

    /*
     * Build and execute CREATE CONSTRAINT TRIGGER statements for the CHECK
     * action for both INSERTs and UPDATEs on the referencing table.
     */
    CreateFKCheckTrigger(myRelOid, refRelOid, fkconstraint, constraintOid, indexOid, true);
    CreateFKCheckTrigger(myRelOid, refRelOid, fkconstraint, constraintOid, indexOid, false);
}

/*
 * ALTER TABLE DROP CONSTRAINT
 *
 * Like DROP COLUMN, we can't use the normal ALTER TABLE recursion mechanism.
 */
static void ATExecDropConstraint(Relation rel, const char* constrName, DropBehavior behavior, bool recurse,
    bool recursing, bool missing_ok, LOCKMODE lockmode)
{
    List* children = NIL;
    ListCell* child = NULL;
    Relation conrel;
    Form_pg_constraint con;
    SysScanDesc scan;
    ScanKeyData key;
    HeapTuple tuple;
    bool found = false;
    bool is_no_inherit_constraint = false;

    /* At top level, permission check was done in ATPrepCmd, else do it */
    if (recursing)
        ATSimplePermissions(rel, ATT_TABLE);

    conrel = heap_open(ConstraintRelationId, RowExclusiveLock);

    /*
     * Find and drop the target constraint
     */
    ScanKeyInit(
        &key, Anum_pg_constraint_conrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
    scan = systable_beginscan(conrel, ConstraintRelidIndexId, true, SnapshotNow, 1, &key);

    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        ObjectAddress conobj;

        con = (Form_pg_constraint)GETSTRUCT(tuple);

        if (strcmp(NameStr(con->conname), constrName) != 0)
            continue;

        /* Don't drop inherited constraints */
        if (con->coninhcount > 0 && !recursing)
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                    errmsg("cannot drop inherited constraint \"%s\" of relation \"%s\"",
                        constrName,
                        RelationGetRelationName(rel))));

        is_no_inherit_constraint = con->connoinherit;

        /*
         * XXX as a special hack, we turn on no-inherit here unconditionally
         * except for CHECK constraints.  This is because 9.2 until beta2
         * contained a bug that marked it false for all constraints, even
         * though it was only supported false for CHECK constraints.
         * See bug #6712.
         */
        if (con->contype != CONSTRAINT_CHECK)
            is_no_inherit_constraint = true;

        /* drop partial cluster key */
        if (con->contype == CONSTRAINT_CLUSTER) {
            SetRelHasClusterKey(rel, false);
        }

        /*
         * If it's a foreign-key constraint, we'd better lock the referenced
         * table and check that that's not in use, just as we've already done
         * for the constrained table (else we might, eg, be dropping a trigger
         * that has unfired events).  But we can/must skip that in the
         * self-referential case.
         */
        if (con->contype == CONSTRAINT_FOREIGN &&
            con->confrelid != RelationGetRelid(rel)) {
            Relation frel;

            /* Must match lock taken by RemoveTriggerById: */
            frel = heap_open(con->confrelid, AccessExclusiveLock);
            CheckTableNotInUse(frel, "ALTER TABLE");
            heap_close(frel, NoLock);
        }

        /*
         * Perform the actual constraint deletion
         */
        conobj.classId = ConstraintRelationId;
        conobj.objectId = HeapTupleGetOid(tuple);
        conobj.objectSubId = 0;

        performDeletion(&conobj, behavior, 0);

        found = true;

        /* constraint found and dropped -- no need to keep looping */
        break;
    }

    systable_endscan(scan);

    if (!found) {
        if (!missing_ok) {
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("constraint \"%s\" of relation \"%s\" does not exist",
                        constrName,
                        RelationGetRelationName(rel))));
        } else {
            ereport(NOTICE,
                (errmsg("constraint \"%s\" of relation \"%s\" does not exist, skipping",
                    constrName,
                    RelationGetRelationName(rel))));
            heap_close(conrel, RowExclusiveLock);
            return;
        }
    }

    /*
     * Propagate to children as appropriate.  Unlike most other ALTER
     * routines, we have to do this one level of recursion at a time; we can't
     * use find_all_inheritors to do it in one pass.
     */
    if (!is_no_inherit_constraint)
        children = find_inheritance_children(RelationGetRelid(rel), lockmode);
    else
        children = NIL;

    foreach (child, children) {
        Oid childrelid = lfirst_oid(child);
        Relation childrel;
        HeapTuple copy_tuple;

        /* find_inheritance_children already got lock */
        childrel = heap_open(childrelid, NoLock);
        CheckTableNotInUse(childrel, "ALTER TABLE");

        ScanKeyInit(&key, Anum_pg_constraint_conrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(childrelid));
        scan = systable_beginscan(conrel, ConstraintRelidIndexId, true, SnapshotNow, 1, &key);

        /* scan for matching tuple - there should only be one */
        while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
            con = (Form_pg_constraint)GETSTRUCT(tuple);

            /* Right now only CHECK constraints can be inherited */
            if (con->contype != CONSTRAINT_CHECK)
                continue;

            if (strcmp(NameStr(con->conname), constrName) == 0)
                break;
        }

        if (!HeapTupleIsValid(tuple))
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("constraint \"%s\" of relation \"%s\" does not exist",
                        constrName,
                        RelationGetRelationName(childrel))));

        copy_tuple = heap_copytuple(tuple);

        systable_endscan(scan);

        con = (Form_pg_constraint)GETSTRUCT(copy_tuple);

        if (con->coninhcount <= 0) /* shouldn't happen */
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                    errmsg("relation %u has non-inherited constraint \"%s\"", childrelid, constrName)));

        if (recurse) {
            /*
             * If the child constraint has other definition sources, just
             * decrement its inheritance count; if not, recurse to delete it.
             */
            if (con->coninhcount == 1 && !con->conislocal) {
                /* Time to delete this child constraint, too */
                ATExecDropConstraint(childrel, constrName, behavior, true, true, false, lockmode);
            } else {
                /* Child constraint must survive my deletion */
                con->coninhcount--;
                simple_heap_update(conrel, &copy_tuple->t_self, copy_tuple);
                CatalogUpdateIndexes(conrel, copy_tuple);

                /* Make update visible */
                CommandCounterIncrement();
            }
        } else {
            /*
             * If we were told to drop ONLY in this table (no recursion), we
             * need to mark the inheritors' constraints as locally defined
             * rather than inherited.
             */
            con->coninhcount--;
            con->conislocal = true;

            simple_heap_update(conrel, &copy_tuple->t_self, copy_tuple);
            CatalogUpdateIndexes(conrel, copy_tuple);

            /* Make update visible */
            CommandCounterIncrement();
        }

        heap_freetuple_ext(copy_tuple);

        heap_close(childrel, NoLock);
    }

    heap_close(conrel, RowExclusiveLock);
}

/*
 * ALTER COLUMN TYPE
 */
static void ATPrepAlterColumnType(List** wqueue, AlteredTableInfo* tab, Relation rel, bool recurse, bool recursing,
    AlterTableCmd* cmd, LOCKMODE lockmode)
{
    char* colName = cmd->name;
    ColumnDef* def = (ColumnDef*)cmd->def;
    TypeName* typname = def->typname;
    Node* transform = def->raw_default;
    HeapTuple tuple;
    Form_pg_attribute attTup;
    AttrNumber attnum;
    Oid targettype = InvalidOid;
    int32 targettypmod = -1;
    Oid targetcollid = InvalidOid;
    NewColumnValue* newval = NULL;
    ParseState* pstate = make_parsestate(NULL);
    AclResult aclresult;

    if (rel->rd_rel->reloftype && !recursing)
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("cannot alter column type of typed table")));

    /* lookup the attribute so we can check inheritance status */
    tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
    if (!HeapTupleIsValid(tuple))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_COLUMN),
                errmsg("column \"%s\" of relation \"%s\" does not exist", colName, RelationGetRelationName(rel))));
    attTup = (Form_pg_attribute)GETSTRUCT(tuple);
    attnum = attTup->attnum;

    /* Can't alter a system attribute */
    if (attnum <= 0)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot alter system column \"%s\"", colName)));

    /* Don't alter inherited columns */
    if (attTup->attinhcount > 0 && !recursing)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION), errmsg("cannot alter inherited column \"%s\"", colName)));
    if (typname && list_length(typname->names) == 1 && !typname->pct_type) {
        char* tname = strVal(linitial(typname->names));

        if (strcmp(tname, "smallserial") == 0 || strcmp(tname, "serial2") == 0 || strcmp(tname, "serial") == 0 ||
            strcmp(tname, "serial4") == 0 || strcmp(tname, "bigserial") == 0 || strcmp(tname, "serial8") == 0)
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION), errmsg("cannot alter column type to \"%s\"", tname)));
    }

    /* Look up the target type */
    typenameTypeIdAndMod(NULL, typname, &targettype, &targettypmod);

    // check the unsupported datatype.
    if (RelationIsColStore(rel) && !IsTypeSupportedByCStore(targettype, targettypmod)) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("type \"%s\" is not supported in column store",
                    format_type_with_typemod(targettype, targettypmod))));
    }

    aclresult = pg_type_aclcheck(targettype, GetUserId(), ACL_USAGE);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error_type(aclresult, targettype);

    /* And the collation */
    targetcollid = GetColumnDefCollation(NULL, def, targettype);

    /* make sure datatype is legal for a column */
    CheckAttributeType(colName, targettype, targetcollid, list_make1_oid(rel->rd_rel->reltype), false);

    if (tab->relkind == RELKIND_RELATION) {
        if (IS_PGXC_COORDINATOR && !IsSystemRelation(rel) && !IsCStoreNamespace(rel->rd_rel->relnamespace)) {
            HeapTuple tup = SearchSysCache(PGXCCLASSRELID, ObjectIdGetDatum(RelationGetRelid(rel)), 0, 0, 0);

            if (!HeapTupleIsValid(tup)) /* should not happen */
                ereport(ERROR,
                    (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                        errmsg("cache lookup failed for pgxc_class %u", RelationGetRelid(rel))));

            Form_pgxc_class pgxc_class = (Form_pgxc_class)GETSTRUCT(tup);
            if ((pgxc_class->pclocatortype == 'H' || pgxc_class->pclocatortype == 'M') &&
                IsDistribColumn(RelationGetRelid(rel), attnum))
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot alter data type of distribute column")));

            ReleaseSysCache(tup);
        }
        /*
         * Set up an expression to transform the old data value to the new
         * type. If a USING option was given, transform and use that
         * expression, else just take the old value and try to coerce it.  We
         * do this first so that type incompatibility can be detected before
         * we waste effort, and because we need the expression to be parsed
         * against the original table row type.
         */
        if (transform != NULL) {
            /* Expression must be able to access vars of old table */
            RangeTblEntry* rte = addRangeTableEntryForRelation(pstate, rel, NULL, false, true);

            addRTEtoQuery(pstate, rte, false, true, true);

            transform = transformExpr(pstate, transform);

            if (RelationIsColStore(rel)) {
                Bitmapset* attrs_referred = NULL;
                /* Collect all the attributes refered in the expression. */
                pull_varattnos(transform, 1, &attrs_referred);
                if (attrs_referred != NULL) {
                    attrs_referred =
                        bms_del_member(attrs_referred, attnum - FirstLowInvalidHeapAttributeNumber);  // remove itself
                    if (!bms_is_empty(attrs_referred)) {
                        bms_free_ext(attrs_referred);
                        ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                errmsg(
                                    "cannot refer to other columns in transform expression for column store table")));
                    }
                    bms_free_ext(attrs_referred);
                }
            }

            /* It can't return a set */
            if (expression_returns_set(transform))
                ereport(
                    ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("transform expression must not return a set")));

            /* No subplans or aggregates, either... */
            if (pstate->p_hasSubLinks)
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot use subquery in transform expression")));
            if (pstate->p_hasAggs)
                ereport(ERROR,
                    (errcode(ERRCODE_GROUPING_ERROR), errmsg("cannot use aggregate function in transform expression")));
            if (pstate->p_hasWindowFuncs)
                ereport(ERROR,
                    (errcode(ERRCODE_WINDOWING_ERROR), errmsg("cannot use window function in transform expression")));
        } else {
            transform = (Node*)makeVar(1, attnum, attTup->atttypid, attTup->atttypmod, attTup->attcollation, 0);
        }

        transform = coerce_to_target_type(pstate,
            transform,
            exprType(transform),
            targettype,
            targettypmod,
            COERCION_ASSIGNMENT,
            COERCE_IMPLICIT_CAST,
            -1);
        if (transform == NULL)
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg(
                        "column \"%s\" cannot be cast automatically to type %s", colName, format_type_be(targettype)),
                    errhint("Specify a USING expression to perform the conversion.")));

        /* Fix collations after all else */
        assign_expr_collations(pstate, transform);

        /* Plan the expr now so we can accurately assess the need to rewrite. */
        transform = (Node*)expression_planner((Expr*)transform);

        /*
         * Add a work queue item to make ATRewriteTable update the column
         * contents.
         */
        newval = (NewColumnValue*)palloc0(sizeof(NewColumnValue));
        newval->attnum = attnum;
        newval->expr = (Expr*)transform;

        tab->newvals = lappend(tab->newvals, newval);
        if (ATColumnChangeRequiresRewrite(transform, attnum))
            tab->rewrite = true;
    } else if (transform != NULL)
        ereport(
            ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a table", RelationGetRelationName(rel))));

    if (tab->relkind == RELKIND_COMPOSITE_TYPE || tab->relkind == RELKIND_FOREIGN_TABLE) {
        /*
         * For composite types, do this check now.	Tables will check it later
         * when the table is being rewritten.
         */
        find_composite_type_dependencies(rel->rd_rel->reltype, rel, NULL);
    }

    ReleaseSysCache(tuple);

    /*
     * The recursion case is handled by ATSimpleRecursion.	However, if we are
     * told not to recurse, there had better not be any child tables; else the
     * alter would put them out of step.
     */
    if (recurse)
        ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode);
    else if (!recursing && find_inheritance_children(RelationGetRelid(rel), NoLock) != NIL)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("type of inherited column \"%s\" must be changed in child tables too", colName)));

    if (tab->relkind == RELKIND_COMPOSITE_TYPE)
        ATTypedTableRecursion(wqueue, rel, cmd, lockmode);
}

/*
 * When the data type of a column is changed, a rewrite might not be required
 * if the new type is sufficiently identical to the old one, and the USING
 * clause isn't trying to insert some other value.  It's safe to skip the
 * rewrite if the old type is binary coercible to the new type, or if the
 * new type is an unconstrained domain over the old type.  In the case of a
 * constrained domain, we could get by with scanning the table and checking
 * the constraint rather than actually rewriting it, but we don't currently
 * try to do that.
 */
static bool ATColumnChangeRequiresRewrite(Node* expr, AttrNumber varattno)
{
    Assert(expr != NULL);

    for (;;) {
        /* only one varno, so no need to check that */
        if (IsA(expr, Var) && ((Var*)expr)->varattno == varattno)
            return false;
        else if (IsA(expr, RelabelType))
            expr = (Node*)((RelabelType*)expr)->arg;
        else if (IsA(expr, CoerceToDomain)) {
            CoerceToDomain* d = (CoerceToDomain*)expr;

            if (GetDomainConstraints(d->resulttype) != NIL)
                return true;
            expr = (Node*)d->arg;
        } else
            return true;
    }
}

static void ATExecAlterColumnType(AlteredTableInfo* tab, Relation rel, AlterTableCmd* cmd, LOCKMODE lockmode)
{
    char* colName = cmd->name;
    ColumnDef* def = (ColumnDef*)cmd->def;
    TypeName* typname = def->typname;
    HeapTuple heapTup;
    Form_pg_attribute attTup;
    AttrNumber attnum;
    HeapTuple typeTuple;
    Form_pg_type tform;
    Oid targettype = InvalidOid;
    int32 targettypmod = -1;
    Oid targetcollid = InvalidOid;
    Node* defaultexpr = NULL;
    Relation attrelation;
    Relation depRel;
    ScanKeyData key[3];
    SysScanDesc scan;
    HeapTuple depTup;

    attrelation = heap_open(AttributeRelationId, RowExclusiveLock);

    /* Look up the target column */
    heapTup = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
    if (!HeapTupleIsValid(heapTup)) /* shouldn't happen */
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_COLUMN),
                errmsg("column \"%s\" of relation \"%s\" does not exist", colName, RelationGetRelationName(rel))));
    attTup = (Form_pg_attribute)GETSTRUCT(heapTup);
    attnum = attTup->attnum;

    /*
     * data type of a partitioned table's partition key can not be changed
     */
    if (RELATION_IS_PARTITIONED(rel)) {
        int2vector* partKey = ((RangePartitionMap*)rel->partMap)->partitionKey;
        int i = 0;

        for (; i < partKey->dim1; i++) {
            if (attnum == partKey->values[i]) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot alter data type of partitioning column \"%s\"", colName)));
            }
        }
    }

    /* Check for multiple ALTER TYPE on same column --- can't cope */
    if (attTup->atttypid != tab->oldDesc->attrs[attnum - 1]->atttypid ||
        attTup->atttypmod != tab->oldDesc->attrs[attnum - 1]->atttypmod)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot alter type of column \"%s\" twice", colName)));

    /* Look up the target type (should not fail, since prep found it) */
    typeTuple = typenameType(NULL, typname, &targettypmod);
    tform = (Form_pg_type)GETSTRUCT(typeTuple);
    targettype = HeapTupleGetOid(typeTuple);
    /* And the collation */
    targetcollid = GetColumnDefCollation(NULL, def, targettype);

    /*
     * If there is a default expression for the column, get it and ensure we
     * can coerce it to the new datatype.  (We must do this before changing
     * the column type, because build_column_default itself will try to
     * coerce, and will not issue the error message we want if it fails.)
     *
     * We remove any implicit coercion steps at the top level of the old
     * default expression; this has been agreed to satisfy the principle of
     * least surprise.	(The conversion to the new column type should act like
     * it started from what the user sees as the stored expression, and the
     * implicit coercions aren't going to be shown.)
     */
    if (attTup->atthasdef) {
        defaultexpr = build_column_default(rel, attnum);
        Assert(defaultexpr);
        defaultexpr = strip_implicit_coercions(defaultexpr);
        defaultexpr = coerce_to_target_type(NULL, /* no UNKNOWN params */
            defaultexpr,
            exprType(defaultexpr),
            targettype,
            targettypmod,
            COERCION_ASSIGNMENT,
            COERCE_IMPLICIT_CAST,
            -1);
        if (defaultexpr == NULL)
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("default for column \"%s\" cannot be cast automatically to type %s",
                        colName,
                        format_type_be(targettype))));
    } else
        defaultexpr = NULL;

    /*
     * Find everything that depends on the column (constraints, indexes, etc),
     * and record enough information to let us recreate the objects.
     *
     * The actual recreation does not happen here, but only after we have
     * performed all the individual ALTER TYPE operations.	We have to save
     * the info before executing ALTER TYPE, though, else the deparser will
     * get confused.
     *
     * There could be multiple entries for the same object, so we must check
     * to ensure we process each one only once.  Note: we assume that an index
     * that implements a constraint will not show a direct dependency on the
     * column.
     */
    depRel = heap_open(DependRelationId, RowExclusiveLock);

    ScanKeyInit(
        &key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
    ScanKeyInit(
        &key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
    ScanKeyInit(&key[2], Anum_pg_depend_refobjsubid, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum((int32)attnum));

    scan = systable_beginscan(depRel, DependReferenceIndexId, true, SnapshotNow, 3, key);

    while (HeapTupleIsValid(depTup = systable_getnext(scan))) {
        Form_pg_depend foundDep = (Form_pg_depend)GETSTRUCT(depTup);
        ObjectAddress foundObject;

        /* We don't expect any PIN dependencies on columns */
        if (foundDep->deptype == DEPENDENCY_PIN)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot alter type of a pinned column")));

        foundObject.classId = foundDep->classid;
        foundObject.objectId = foundDep->objid;
        foundObject.objectSubId = foundDep->objsubid;

        switch (getObjectClass(&foundObject)) {
            case OCLASS_CLASS: {
                char relKind = get_rel_relkind(foundObject.objectId);

                if (relKind == RELKIND_INDEX) {
                    Assert(foundObject.objectSubId == 0);
                    if (!list_member_oid(tab->changedIndexOids, foundObject.objectId)) {
                        /*
                         * Question: alter table set datatype and table index execute concurrently, data inconsistency
                         * occurs. The index file is deleted and metadata is left. Because the data type is not locked
                         * after modification, which ultimately leads to could not open file. Alter table column set
                         * datatype maybe trigger index operation but index is not locked. When the index data is
                         * inconsistent, we can use"reindex index" to repair the index.
                         * Solution: we should lock index at the beginning.The AccessExclusiveLock for index is used
                         * because we think AccessExclusiveLock for data table will block any operation and index
                         * will be not used to query data. This operation will block individual index operations,
                         * such as reindex index\set index tablespace.
                         * Testcase: alter table row_table alter column col_varchar set data type text,alter column
                         * col_smallint set data type bigint + alter index idx set tablespace.
                         */
                        LockRelationOid(foundObject.objectId, AccessExclusiveLock);
                        tab->changedIndexOids = lappend_oid(tab->changedIndexOids, foundObject.objectId);
                        tab->changedIndexDefs =
                            lappend(tab->changedIndexDefs, pg_get_indexdef_string(foundObject.objectId));
                    }
                } else if (relKind == RELKIND_SEQUENCE) {
                    /*
                     * This must be a SERIAL column's sequence.  We need
                     * not do anything to it.
                     */
                    Assert(foundObject.objectSubId == 0);
                } else {
                    /* Not expecting any other direct dependencies... */
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("unexpected object depending on column: %s", getObjectDescription(&foundObject))));
                }
                break;
            }

            case OCLASS_CONSTRAINT:
                Assert(foundObject.objectSubId == 0);
                if (!list_member_oid(tab->changedConstraintOids, foundObject.objectId)) {
                    char* defstring = pg_get_constraintdef_string(foundObject.objectId);

                    /*
                     * Put NORMAL dependencies at the front of the list and
                     * AUTO dependencies at the back.  This makes sure that
                     * foreign-key constraints depending on this column will
                     * be dropped before unique or primary-key constraints of
                     * the column; which we must have because the FK
                     * constraints depend on the indexes belonging to the
                     * unique constraints.
                     */
                    if (foundDep->deptype == DEPENDENCY_NORMAL) {
                        tab->changedConstraintOids = lcons_oid(foundObject.objectId, tab->changedConstraintOids);
                        tab->changedConstraintDefs = lcons(defstring, tab->changedConstraintDefs);
                    } else {
                        tab->changedConstraintOids = lappend_oid(tab->changedConstraintOids, foundObject.objectId);
                        tab->changedConstraintDefs = lappend(tab->changedConstraintDefs, defstring);
                    }
                }
                break;

            case OCLASS_REWRITE:
                /* XXX someday see if we can cope with revising views */
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot alter type of a column used by a view or rule"),
                        errdetail("%s depends on column \"%s\"", getObjectDescription(&foundObject), colName)));
                break;

            case OCLASS_TRIGGER:

                /*
                 * A trigger can depend on a column because the column is
                 * specified as an update target, or because the column is
                 * used in the trigger's WHEN condition.  The first case would
                 * not require any extra work, but the second case would
                 * require updating the WHEN expression, which will take a
                 * significant amount of new code.	Since we can't easily tell
                 * which case applies, we punt for both.
                 */
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot alter type of a column used in a trigger definition"),
                        errdetail("%s depends on column \"%s\"", getObjectDescription(&foundObject), colName)));
                break;

            case OCLASS_DEFAULT:

                /*
                 * Ignore the column's default expression, since we will fix
                 * it below.
                 */
                Assert(defaultexpr);
                break;

            case OCLASS_PROC:
            case OCLASS_TYPE:
            case OCLASS_CAST:
            case OCLASS_COLLATION:
            case OCLASS_CONVERSION:
            case OCLASS_LANGUAGE:
            case OCLASS_LARGEOBJECT:
            case OCLASS_OPERATOR:
            case OCLASS_OPCLASS:
            case OCLASS_OPFAMILY:
            case OCLASS_AMOP:
            case OCLASS_AMPROC:
            case OCLASS_SCHEMA:
            case OCLASS_TSPARSER:
            case OCLASS_TSDICT:
            case OCLASS_TSTEMPLATE:
            case OCLASS_TSCONFIG:
            case OCLASS_ROLE:
            case OCLASS_DATABASE:
            case OCLASS_TBLSPACE:
            case OCLASS_FDW:
            case OCLASS_FOREIGN_SERVER:
            case OCLASS_USER_MAPPING:
            case OCLASS_DEFACL:
            case OCLASS_EXTENSION:
            case OCLASS_DATA_SOURCE:

                /*
                 * We don't expect any of these sorts of objects to depend on
                 * a column.
                 */
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("unexpected object depending on column: %s", getObjectDescription(&foundObject))));
                break;

            default:
                ereport(ERROR,
                    (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmsg("unrecognized object class: %u", foundObject.classId)));
        }
    }

    systable_endscan(scan);

    /*
     * Now scan for dependencies of this column on other things.  The only
     * thing we should find is the dependency on the column datatype, which we
     * want to remove, and possibly a collation dependency.
     */
    ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
    ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
    ScanKeyInit(&key[2], Anum_pg_depend_objsubid, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum((int32)attnum));

    scan = systable_beginscan(depRel, DependDependerIndexId, true, SnapshotNow, 3, key);

    while (HeapTupleIsValid(depTup = systable_getnext(scan))) {
        Form_pg_depend foundDep = (Form_pg_depend)GETSTRUCT(depTup);

        if (foundDep->deptype != DEPENDENCY_NORMAL)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("found unexpected dependency type '%c'", foundDep->deptype)));
        if (!(foundDep->refclassid == TypeRelationId && foundDep->refobjid == attTup->atttypid) &&
            !(foundDep->refclassid == CollationRelationId && foundDep->refobjid == attTup->attcollation))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("found unexpected dependency for column")));

        simple_heap_delete(depRel, &depTup->t_self);
    }

    systable_endscan(scan);

    heap_close(depRel, RowExclusiveLock);

    /*
     * Here we go --- change the recorded column type and collation.  (Note
     * heapTup is a copy of the syscache entry, so okay to scribble on.)
     */
    attTup->atttypid = targettype;
    attTup->atttypmod = targettypmod;
    attTup->attcollation = targetcollid;
    attTup->attndims = list_length(typname->arrayBounds);
    attTup->attlen = tform->typlen;
    attTup->attbyval = tform->typbyval;
    attTup->attalign = tform->typalign;
    attTup->attstorage = tform->typstorage;

    ReleaseSysCache(typeTuple);

    simple_heap_update(attrelation, &heapTup->t_self, heapTup);

    /* keep system catalog indexes current */
    CatalogUpdateIndexes(attrelation, heapTup);

    heap_close(attrelation, RowExclusiveLock);

    /* Install dependencies on new datatype and collation */
    add_column_datatype_dependency(RelationGetRelid(rel), attnum, targettype);
    add_column_collation_dependency(RelationGetRelid(rel), attnum, targetcollid);

    /*
     * Drop any pg_statistic entry for the column, since it's now wrong type
     */
    RemoveStatistics<'c'>(RelationGetRelid(rel), attnum);

    /*
     * Update the default, if present, by brute force --- remove and re-add
     * the default.  Probably unsafe to take shortcuts, since the new version
     * may well have additional dependencies.  (It's okay to do this now,
     * rather than after other ALTER TYPE commands, since the default won't
     * depend on other column types.)
     */
    if (defaultexpr != NULL) {
        /* Must make new row visible since it will be updated again */
        CommandCounterIncrement();

        /*
         * We use RESTRICT here for safety, but at present we do not expect
         * anything to depend on the default.
         */
        RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, true, true);

        StoreAttrDefault(rel, attnum, defaultexpr);
    }

    /* Cleanup */
    heap_freetuple_ext(heapTup);
}

static void ATExecAlterColumnGenericOptions(Relation rel, const char* colName, List* options, LOCKMODE lockmode)
{
    Relation ftrel;
    Relation attrel;
    ForeignServer* server = NULL;
    ForeignDataWrapper* fdw = NULL;
    HeapTuple tuple;
    HeapTuple newtuple;
    bool isnull = false;
    Datum repl_val[Natts_pg_attribute];
    bool repl_null[Natts_pg_attribute];
    bool repl_repl[Natts_pg_attribute];
    Datum datum;
    Form_pg_foreign_table fttableform;
    Form_pg_attribute atttableform;
    errno_t rc = EOK;

    if (options == NIL)
        return;

    /* First, determine FDW validator associated to the foreign table. */
    ftrel = heap_open(ForeignTableRelationId, AccessShareLock);
    tuple = SearchSysCache1(FOREIGNTABLEREL, rel->rd_id);
    if (!HeapTupleIsValid(tuple))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("foreign table \"%s\" does not exist", RelationGetRelationName(rel))));
    fttableform = (Form_pg_foreign_table)GETSTRUCT(tuple);
    server = GetForeignServer(fttableform->ftserver);
    fdw = GetForeignDataWrapper(server->fdwid);

    heap_close(ftrel, AccessShareLock);
    ReleaseSysCache(tuple);

    attrel = heap_open(AttributeRelationId, RowExclusiveLock);
    tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
    if (!HeapTupleIsValid(tuple))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_COLUMN),
                errmsg("column \"%s\" of relation \"%s\" does not exist", colName, RelationGetRelationName(rel))));

    /* Prevent them from altering a system attribute */
    atttableform = (Form_pg_attribute)GETSTRUCT(tuple);
    if (atttableform->attnum <= 0)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot alter system column \"%s\"", colName)));

    /* Initialize buffers for new tuple values */
    rc = memset_s(repl_val, sizeof(repl_val), 0, sizeof(repl_val));
    securec_check(rc, "\0", "\0");
    rc = memset_s(repl_null, sizeof(repl_null), false, sizeof(repl_null));
    securec_check(rc, "\0", "\0");
    rc = memset_s(repl_repl, sizeof(repl_repl), false, sizeof(repl_repl));
    securec_check(rc, "\0", "\0");

    /* Extract the current options */
    datum = SysCacheGetAttr(ATTNAME, tuple, Anum_pg_attribute_attfdwoptions, &isnull);
    if (isnull)
        datum = PointerGetDatum(NULL);

    /* Transform the options */
    datum = transformGenericOptions(AttributeRelationId, datum, options, fdw->fdwvalidator);

    if (PointerIsValid(DatumGetPointer(datum)))
        repl_val[Anum_pg_attribute_attfdwoptions - 1] = datum;
    else
        repl_null[Anum_pg_attribute_attfdwoptions - 1] = true;
    repl_repl[Anum_pg_attribute_attfdwoptions - 1] = true;

    /* Everything looks good - update the tuple */
    newtuple = heap_modify_tuple(tuple, RelationGetDescr(attrel), repl_val, repl_null, repl_repl);
    ReleaseSysCache(tuple);
    simple_heap_update(attrel, &newtuple->t_self, newtuple);
    CatalogUpdateIndexes(attrel, newtuple);
    heap_close(attrel, RowExclusiveLock);
    heap_freetuple_ext(newtuple);
}

// delete record about psort oid and index oid from pg_depend,
// cut off their dependency and make psort independent.
// *oldIndexPassList* depends on AT_PASS_OLD_INDEX list.
static void ATCutOffPSortDependency(List* oldIndexPassList, LOCKMODE lockmode)
{
    ListCell* lcell = NULL;
    long ndeleted = 0;
    long nupdated = 0;

    foreach (lcell, oldIndexPassList) {
        AlterTableCmd* atCmd = (AlterTableCmd*)lfirst(lcell);
        Assert(atCmd);
        if (atCmd->subtype != AT_ReAddIndex)
            continue;

        IndexStmt* istmt = (IndexStmt*)atCmd->def;
        Assert(istmt);

        // valid oldPSortOid implies that we want to reuse the
        // existing psort index data, so we have to cut off their
        // dependency and keep it from performing deleting.
        if (!OidIsValid(istmt->oldPSortOid))
            continue;

        long num = deleteDependencyRecordsForClass(RelationRelationId,  // class id
            istmt->oldPSortOid,                                         // depender oid
            RelationRelationId,                                         // referenece class id
            DEPENDENCY_INTERNAL);                                       // dependency type
        if (num != 1) {
            Assert(false);
            ereport(ERROR,
                (errcode(ERRCODE_OPERATE_RESULT_NOT_EXPECTED),
                    errmsg("PSort %u should depend on only one index relation but not %ld.", istmt->oldPSortOid, num)));
        }
        ++ndeleted;

        // if the index is a partition table, handle all the
        // psort info for each partition.
        if (istmt->isPartitioned) {
            Relation hrel = relation_openrv(istmt->relation, lockmode);
            Relation irel = index_open(istmt->indexOid, lockmode);
            List* partHeapOids = relationGetPartitionOidList(hrel);

            ListCell* cell = NULL;
            Relation pg_part = heap_open(PartitionRelationId, RowExclusiveLock);
            foreach (cell, partHeapOids) {
                // step 1: find each part oid of this index tuple.
                Oid partHeapOid = lfirst_oid(cell);
                Oid partIdxOid = getPartitionIndexOid(istmt->indexOid, partHeapOid);

                // step 2: lookup the right index tuple according to part oid.
                HeapTuple partIdxTup = SearchSysCacheCopy1(PARTRELID, ObjectIdGetDatum(partIdxOid));
                if (!HeapTupleIsValid(partIdxTup)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                            errmsg("Partition cache lookup failed for index partition %u", partIdxOid)));
                }

                // step 3: reset psort oid to be 0.
                // so psort index cannot be droppend later.
                // refer to index_drop  and heapDropPartitionIndex.
                Form_pg_partition partForm = (Form_pg_partition)GETSTRUCT(partIdxTup);
                partForm->relcudescrelid = InvalidOid;

                // step 4: update pg_partition relation and its index info.
                simple_heap_update(pg_part, &partIdxTup->t_self, partIdxTup);
                CatalogUpdateIndexes(pg_part, partIdxTup);

                heap_freetuple_ext(partIdxTup);
                ++nupdated;
            }
            heap_close(pg_part, RowExclusiveLock);

            releasePartitionOidList(&partHeapOids);
            index_close(irel, NoLock);
            heap_close(hrel, NoLock);
        }
    }

    // Make deletion of dependency record visible
    if ((ndeleted + nupdated) > 0) {
        CommandCounterIncrement();
    }
}

/*
 * Cleanup after we've finished all the ALTER TYPE operations for a
 * particular relation.  We have to drop and recreate all the indexes
 * and constraints that depend on the altered columns.
 */
static void ATPostAlterTypeCleanup(List** wqueue, AlteredTableInfo* tab, LOCKMODE lockmode)
{
    ObjectAddress obj;
    ListCell* def_item = NULL;
    ListCell* oid_item = NULL;

    /*
     * Re-parse the index and constraint definitions, and attach them to the
     * appropriate work queue entries.	We do this before dropping because in
     * the case of a FOREIGN KEY constraint, we might not yet have exclusive
     * lock on the table the constraint is attached to, and we need to get
     * that before dropping.  It's safe because the parser won't actually look
     * at the catalogs to detect the existing entry.
     *
     * We can't rely on the output of deparsing to tell us which relation
     * to operate on, because concurrent activity might have made the name
     * resolve differently.  Instead, we've got to use the OID of the
     * constraint or index we're processing to figure out which relation
     * to operate on.
     */
    forboth(oid_item, tab->changedConstraintOids, def_item, tab->changedConstraintDefs)
    {
        Oid oldId = lfirst_oid(oid_item);
        Oid relid;
        Oid confrelid;

        get_constraint_relation_oids(oldId, &relid, &confrelid);
        ATPostAlterTypeParse(oldId, relid, confrelid, (char*)lfirst(def_item), wqueue, lockmode, tab->rewrite);
    }
    forboth(oid_item, tab->changedIndexOids, def_item, tab->changedIndexDefs)
    {
        Oid oldId = lfirst_oid(oid_item);
        Oid relid;

        relid = IndexGetRelation(oldId, false);
        ATPostAlterTypeParse(oldId, relid, InvalidOid, (char*)lfirst(def_item), wqueue, lockmode, tab->rewrite);
    }

    /*
     * Now we can drop the existing constraints and indexes --- constraints
     * first, since some of them might depend on the indexes.  In fact, we
     * have to delete FOREIGN KEY constraints before UNIQUE constraints, but
     * we already ordered the constraint list to ensure that would happen. It
     * should be okay to use DROP_RESTRICT here, since nothing else should be
     * depending on these objects.
     */
    foreach (oid_item, tab->changedConstraintOids) {
        obj.classId = ConstraintRelationId;
        obj.objectId = lfirst_oid(oid_item);
        obj.objectSubId = 0;
        performDeletion(&obj, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
    }

    if (!tab->rewrite && tab->subcmds[AT_PASS_OLD_INDEX]) {
        /*
         * existing indexes will be dropped. but rewrite is false, that
         * means existing indexes should be reserved and reused. so we
         * should remove the dependency between psort and its reference
         * index relation.
         */
        ATCutOffPSortDependency(tab->subcmds[AT_PASS_OLD_INDEX], lockmode);
    }

    foreach (oid_item, tab->changedIndexOids) {
        obj.classId = RelationRelationId;
        obj.objectId = lfirst_oid(oid_item);
        obj.objectSubId = 0;
        performDeletion(&obj, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
    }

    /*
     * The objects will get recreated during subsequent passes over the work
     * queue.
     */
}

static void ATPostAlterTypeParse(
    Oid oldId, Oid oldRelId, Oid refRelId, const char* cmd_str, List** wqueue, LOCKMODE lockmode, bool rewrite)
{
    ListCell* list_item = NULL;
    List* raw_parsetree_list = raw_parser(cmd_str);
    List* querytree_list = NIL;
    Relation rel;

    /*
     * We expect that we will get only ALTER TABLE and CREATE INDEX
     * statements. Hence, there is no need to pass them through
     * parse_analyze() or the rewriter, but instead we need to pass them
     * through parse_utilcmd.c to make them ready for execution.
     */
    foreach (list_item, raw_parsetree_list) {
        Node* stmt = (Node*)lfirst(list_item);

        if (IsA(stmt, IndexStmt)) {
            querytree_list = lappend(querytree_list, transformIndexStmt(oldRelId, (IndexStmt*)stmt, cmd_str));
        } else if (IsA(stmt, AlterTableStmt)) {
            querytree_list = list_concat(querytree_list, 
                transformAlterTableStmt(oldRelId, (AlterTableStmt*)stmt, cmd_str));
        } else {
            querytree_list = lappend(querytree_list, stmt);
        }
    }

    /* Caller should already have acquired whatever lock we need. */
    rel = relation_open(oldRelId, NoLock);

    /*
     * Attach each generated command to the proper place in the work queue.
     * Note this could result in creation of entirely new work-queue entries.
     *
     * Also note that we have to tweak the command subtypes, because it turns
     * out that re-creation of indexes and constraints has to act a bit
     * differently from initial creation.
     */
    foreach (list_item, querytree_list) {
        Node* stm = (Node*)lfirst(list_item);
        AlteredTableInfo* tab = NULL;

        switch (nodeTag(stm)) {
            case T_IndexStmt: {
                IndexStmt* stmt = (IndexStmt*)stm;
                AlterTableCmd* newcmd = NULL;

                if (!rewrite) {
                    if (!stmt->isPartitioned) {
                        TryReuseIndex(oldId, stmt);
                    } else {
                        tryReusePartedIndex(oldId, stmt, rel);
                    }
                }

                tab = ATGetQueueEntry(wqueue, rel);
                newcmd = makeNode(AlterTableCmd);
                newcmd->subtype = AT_ReAddIndex;
                newcmd->def = (Node*)stmt;
                tab->subcmds[AT_PASS_OLD_INDEX] = lappend(tab->subcmds[AT_PASS_OLD_INDEX], newcmd);
                break;
            }
            case T_AlterTableStmt: {
                AlterTableStmt* stmt = (AlterTableStmt*)stm;
                ListCell* lcmd = NULL;

                tab = ATGetQueueEntry(wqueue, rel);
                foreach (lcmd, stmt->cmds) {
                    AlterTableCmd* cmd = (AlterTableCmd*)lfirst(lcmd);
                    Constraint* con = NULL;

                    switch (cmd->subtype) {
                        case AT_AddIndex:
                            Assert(IsA(cmd->def, IndexStmt));
                            if (!rewrite) {
                                if (RelationIsPartitioned(rel)) {
                                    tryReusePartedIndex(get_constraint_index(oldId), (IndexStmt*)cmd->def, rel);
                                } else {
                                    TryReuseIndex(get_constraint_index(oldId), (IndexStmt*)cmd->def);
                                }
                            }

                            cmd->subtype = AT_ReAddIndex;
                            tab->subcmds[AT_PASS_OLD_INDEX] = lappend(tab->subcmds[AT_PASS_OLD_INDEX], cmd);
                            break;
                        case AT_AddConstraint:
                            Assert(IsA(cmd->def, Constraint));
                            con = (Constraint*)cmd->def;
                            con->old_pktable_oid = refRelId;
                            /* rewriting neither side of a FK */
                            if (con->contype == CONSTR_FOREIGN && !rewrite && !tab->rewrite)
                                TryReuseForeignKey(oldId, con);
                            cmd->subtype = AT_ReAddConstraint;
                            tab->subcmds[AT_PASS_OLD_CONSTR] = lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);
                            break;
                        default:
                            ereport(ERROR,
                                (errcode(ERRCODE_UNDEFINED_OBJECT),
                                    errmsg("unexpected statement type: %d", (int)cmd->subtype)));
                    }
                }
                break;
            }
            default: {
                ereport(ERROR,
                    (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmsg("unexpected statement type: %d", (int)nodeTag(stm))));
            }
        }
    }

    relation_close(rel, NoLock);
}

/*
 * Subroutine for ATPostAlterTypeParse().  Calls out to CheckIndexCompatible()
 * for the real analysis, then mutates the IndexStmt based on that verdict.
 */
void TryReuseIndex(Oid oldId, IndexStmt* stmt)
{
    if (CheckIndexCompatible(oldId, stmt->accessMethod, stmt->indexParams, stmt->excludeOpNames)) {
        Relation irel = index_open(oldId, NoLock);

        /*
         * try to reuse existing index storage. for row relation, only need *oldNode*.
         *for column relation, also remember the oid of psort index.
         */
        stmt->oldNode = irel->rd_node.relNode;
        stmt->oldPSortOid = irel->rd_rel->relcudescrelid;
        stmt->indexOid = oldId;
        index_close(irel, NoLock);
    }
}

/*
 * @@GaussDB@@
 * Target       : data partition
 * Brief        : Subroutine for ATPostAlterTypeParse().  Calls out to CheckIndexCompatible()
 *                for the real analysis, then mutates the IndexStmt based on that verdict.
 * Description  :
 * Notes        :
 * Input        :
 * Output       :
 */
void tryReusePartedIndex(Oid oldId, IndexStmt* stmt, Relation rel)
{
    List* partOids = NIL;
    ListCell* cell = NULL;
    Oid partOid = InvalidOid;
    Oid partIndexOid = InvalidOid;
    Partition partition = NULL;

    if (CheckIndexCompatible(oldId, stmt->accessMethod, stmt->indexParams, stmt->excludeOpNames)) {
        Relation irel = index_open(oldId, NoLock);

        /*
         * try to reuse existing index storage. for row relation, only need 'oldNode'.
         * for column relation, also remember the oid of psort index. index partition
         * relation oid is needed for each partition.
         */
        stmt->oldNode = irel->rd_node.relNode;
        stmt->oldPSortOid = irel->rd_rel->relcudescrelid;
        stmt->indexOid = oldId;

        partOids = relationGetPartitionOidList(rel);
        foreach (cell, partOids) {
            partOid = lfirst_oid(cell);
            partIndexOid = getPartitionIndexOid(oldId, partOid);

            partition = partitionOpen(irel, partIndexOid, NoLock);
            /* remember the old relfilenodes and psort oids. */
            stmt->partIndexOldNodes = lappend_oid(stmt->partIndexOldNodes, partition->pd_part->relfilenode);
            stmt->partIndexOldPSortOid = lappend_oid(stmt->partIndexOldPSortOid, partition->pd_part->relcudescrelid);
            partitionClose(irel, partition, NoLock);
        }
        releasePartitionOidList(&partOids);

        index_close(irel, NoLock);
    }
}

/*
 * Subroutine for ATPostAlterTypeParse().
 *
 * Stash the old P-F equality operator into the Constraint node, for possible
 * use by ATAddForeignKeyConstraint() in determining whether revalidation of
 * this constraint can be skipped.
 */
static void TryReuseForeignKey(Oid oldId, Constraint* con)
{
    HeapTuple tup;
    Datum adatum;
    bool isNull = false;
    ArrayType* arr = NULL;
    Oid* rawarr = NULL;
    int numkeys;
    int i;

    Assert(con->contype == CONSTR_FOREIGN);
    Assert(con->old_conpfeqop == NIL); /* already prepared this node */

    tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(oldId));
    if (!HeapTupleIsValid(tup)) {
        /* should not happen */
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for constraint %u", oldId)));
    }

    adatum = SysCacheGetAttr(CONSTROID, tup, Anum_pg_constraint_conpfeqop, &isNull);
    if (isNull)
        ereport(ERROR, (errcode(ERRCODE_UNEXPECTED_NULL_VALUE), errmsg("null conpfeqop for constraint %u", oldId)));
    arr = DatumGetArrayTypeP(adatum); /* ensure not toasted */
    numkeys = ARR_DIMS(arr)[0];
    /* test follows the one in ri_FetchConstraintInfo() */
    if (ARR_NDIM(arr) != 1 || ARR_HASNULL(arr) || ARR_ELEMTYPE(arr) != OIDOID)
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("conpfeqop is not a 1-D Oid array")));
    rawarr = (Oid*)ARR_DATA_PTR(arr);

    /* stash a List of the operator Oids in our Constraint node */
    for (i = 0; i < numkeys; i++)
        con->old_conpfeqop = lcons_oid(rawarr[i], con->old_conpfeqop);

    ReleaseSysCache(tup);
}

/*
 * ALTER TABLE OWNER
 *
 * recursing is true if we are recursing from a table to its indexes,
 * sequences, or toast table.  We don't allow the ownership of those things to
 * be changed separately from the parent table.  Also, we can skip permission
 * checks (this is necessary not just an optimization, else we'd fail to
 * handle toast tables properly).
 *
 * recursing is also true if ALTER TYPE OWNER is calling us to fix up a
 * free-standing composite type.
 */
void ATExecChangeOwner(Oid relationOid, Oid newOwnerId, bool recursing, LOCKMODE lockmode)
{
    Relation target_rel;
    Relation class_rel;
    HeapTuple tuple;
    Form_pg_class tuple_class;

    /*
     * Get exclusive lock till end of transaction on the target table. Use
     * relation_open so that we can work on indexes and sequences.
     */
    target_rel = relation_open(relationOid, lockmode);

    /* Get its pg_class tuple, too */
    class_rel = heap_open(RelationRelationId, RowExclusiveLock);

    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relationOid));
    if (!HeapTupleIsValid(tuple))
        ereport(
            ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", relationOid)));
    tuple_class = (Form_pg_class)GETSTRUCT(tuple);

    /* Can we change the ownership of this tuple? */
    switch (tuple_class->relkind) {
        case RELKIND_RELATION:
        case RELKIND_VIEW:
        case RELKIND_FOREIGN_TABLE:
            /* ok to change owner */
            if (IS_PGXC_COORDINATOR && in_logic_cluster()) {
                Oid new_group_oid, group_oid;
                bool is_admin = systemDBA_arg(newOwnerId) || superuser_arg(newOwnerId);

                /* check whether the group_oid of the two owners are same.
                 *  Only allow changing owner to other group's role in redistribution.
                 * Or allow changing owner to sysadmin users.
                 */
                group_oid = get_pgxc_logic_groupoid(tuple_class->relowner);
                if (tuple_class->relkind != RELKIND_VIEW && !OidIsValid(group_oid) &&
                    is_pgxc_class_table(relationOid)) {
                    group_oid = get_pgxc_class_groupoid(relationOid);
                }

                new_group_oid = get_pgxc_logic_groupoid(newOwnerId);

                if (group_oid != new_group_oid && OidIsValid(group_oid) && !is_admin) {
                    char in_redis_new = 'n';
                    char in_redis = get_pgxc_group_redistributionstatus(group_oid);
                    if (OidIsValid(new_group_oid)) {
                        in_redis_new = get_pgxc_group_redistributionstatus(new_group_oid);
                    }
                    if (in_redis != 'y' && in_redis != 't' && in_redis_new != 'y' && in_redis_new != 't') {
                        /* Old group and new group are not redistribution group */
                        ereport(ERROR,
                            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                                errmsg("Can not change owner of \"%s\" to other logic cluster users.",
                                    NameStr(tuple_class->relname))));
                    }
                }
            }
            break;
        case RELKIND_INDEX:
            if (!recursing) {
                /*
                 * Because ALTER INDEX OWNER used to be allowed, and in fact
                 * is generated by old versions of pg_dump, we give a warning
                 * and do nothing rather than erroring out.  Also, to avoid
                 * unnecessary chatter while restoring those old dumps, say
                 * nothing at all if the command would be a no-op anyway.
                 */
                if (tuple_class->relowner != newOwnerId)
                    ereport(WARNING,
                        (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                            errmsg("cannot change owner of index \"%s\"", NameStr(tuple_class->relname)),
                            errhint("Change the ownership of the index's table, instead.")));
                /* quick hack to exit via the no-op path */
                newOwnerId = tuple_class->relowner;
            }
            break;
        case RELKIND_SEQUENCE:
            if (!recursing && tuple_class->relowner != newOwnerId) {
                /* if it's an owned sequence, disallow changing it by itself */
                Oid tableId;
                int32 colId;

                if (sequenceIsOwned(relationOid, &tableId, &colId))
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("cannot change owner of sequence \"%s\"", NameStr(tuple_class->relname)),
                            errdetail("Sequence \"%s\" is linked to table \"%s\".",
                                NameStr(tuple_class->relname),
                                get_rel_name(tableId))));
            }
            break;
        case RELKIND_COMPOSITE_TYPE:
            if (recursing)
                break;
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("\"%s\" is a composite type", NameStr(tuple_class->relname)),
                    errhint("Use ALTER TYPE instead.")));
            break;
        case RELKIND_TOASTVALUE:
            if (recursing)
                break;
            /* lint -fallthrough */
        default:
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("\"%s\" is not a table, view, sequence, or foreign table", NameStr(tuple_class->relname))));
    }

    /*
     * If the new owner is the same as the existing owner, consider the
     * command to have succeeded.  This is for dump restoration purposes.
     */
    if (tuple_class->relowner != newOwnerId) {
        Datum repl_val[Natts_pg_class];
        bool repl_null[Natts_pg_class];
        bool repl_repl[Natts_pg_class];
        Acl* newAcl = NULL;
        Datum aclDatum;
        bool isNull = false;
        HeapTuple newtuple;
        errno_t rc = EOK;

        /* skip permission checks when recursing to index or toast table */
        if (!recursing) {
            /* Superusers can always do it, except manipulate independent role's objects. */
            if (!superuser() || is_role_independent(tuple_class->relowner)) {
                Oid namespaceOid = tuple_class->relnamespace;
                AclResult aclresult;

                /* Otherwise, must be owner or has privileges of the existing object's owner. */
                if (!has_privs_of_role(GetUserId(), tuple_class->relowner))
                    aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, RelationGetRelationName(target_rel));

                /* Must be able to become new owner */
                check_is_member_of_role(GetUserId(), newOwnerId);

                /* New owner must have CREATE privilege on namespace */
                aclresult = pg_namespace_aclcheck(namespaceOid, newOwnerId, ACL_CREATE);
                if (aclresult != ACLCHECK_OK)
                    aclcheck_error(aclresult, ACL_KIND_NAMESPACE, get_namespace_name(namespaceOid, true));
            }

            /* check whether newOwner has enough space on CN, or translation tableSize from oldOwner to newOwner on DN */
            if (g_instance.attr.attr_resource.enable_perm_space && !IS_SINGLE_NODE) {
                UserData* newUserdata = GetUserDataFromHTab(newOwnerId, false);

                FunctionCallInfo fcinfo =
                    (FunctionCallInfo)palloc0(sizeof(FunctionCallInfoData) + sizeof(bool) + sizeof(Datum));

                fcinfo->nargs = 1;
                fcinfo->argnull = (bool*)((char*)fcinfo + sizeof(FunctionCallInfoData));
                fcinfo->argnull[0] = false;
                fcinfo->arg = (Datum*)((char*)fcinfo->argnull + sizeof(bool));
                fcinfo->arg[0] = UInt32GetDatum(relationOid);

                uint64 tableSize = DatumGetUInt64(pg_total_relation_size(fcinfo));

                pfree_ext(fcinfo);

                DataSpaceType type = RelationUsesSpaceType(target_rel->rd_rel->relpersistence);

                if (IS_PGXC_COORDINATOR) {
                    if (type == SP_PERM) {
                        if (newUserdata->spacelimit > 0 &&
                            (newUserdata->totalspace + tableSize > newUserdata->spacelimit)) {
                            ereport(ERROR,
                                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                                    errmsg("no perm space is available for the targeted owner")));
                        }
                        if (newUserdata->parent && newUserdata->parent->spacelimit > 0 &&
                            (newUserdata->parent->totalspace + tableSize > newUserdata->parent->spacelimit)) {
                            ereport(ERROR,
                                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                                    errmsg("no perm space is available for the targeted user group")));
                        }
                    } else {
                        if (newUserdata->tmpSpaceLimit > 0 &&
                            (newUserdata->tmpSpace + tableSize > newUserdata->tmpSpaceLimit)) {
                            ereport(ERROR,
                                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                                    errmsg("no temp space is available for the targeted owner")));
                        }
                        if (newUserdata->parent && newUserdata->parent->tmpSpaceLimit > 0 &&
                            (newUserdata->parent->tmpSpace + tableSize > newUserdata->parent->tmpSpaceLimit)) {
                            ereport(ERROR,
                                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                                    errmsg("no temp space is available for the targeted user group")));
                        }
                    }
                }

                if (IS_PGXC_DATANODE) {
                    if (newOwnerId != BOOTSTRAP_SUPERUSERID) {
                        perm_space_increase(newOwnerId, tableSize, type);
                    }
                    perm_space_decrease(tuple_class->relowner, tableSize, type);
                }
            }
        }

        rc = memset_s(repl_null, sizeof(repl_null), false, sizeof(repl_null));
        securec_check(rc, "\0", "\0");
        rc = memset_s(repl_repl, sizeof(repl_repl), false, sizeof(repl_repl));
        securec_check(rc, "\0", "\0");

        repl_repl[Anum_pg_class_relowner - 1] = true;
        repl_val[Anum_pg_class_relowner - 1] = ObjectIdGetDatum(newOwnerId);

        /*
         * Determine the modified ACL for the new owner.  This is only
         * necessary when the ACL is non-null.
         */
        aclDatum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relacl, &isNull);
        if (!isNull) {
            newAcl = aclnewowner(DatumGetAclP(aclDatum), tuple_class->relowner, newOwnerId);
            repl_repl[Anum_pg_class_relacl - 1] = true;
            repl_val[Anum_pg_class_relacl - 1] = PointerGetDatum(newAcl);
        }

        newtuple = heap_modify_tuple(tuple, RelationGetDescr(class_rel), repl_val, repl_null, repl_repl);

        simple_heap_update(class_rel, &newtuple->t_self, newtuple);
        CatalogUpdateIndexes(class_rel, newtuple);

        heap_freetuple_ext(newtuple);

        /*
         * We must similarly update any per-column ACLs to reflect the new
         * owner; for neatness reasons that's split out as a subroutine.
         */
        change_owner_fix_column_acls(relationOid, tuple_class->relowner, newOwnerId);

        /*
         * Update owner dependency reference, if any.  A composite type has
         * none, because it's tracked for the pg_type entry instead of here;
         * indexes and TOAST tables don't have their own entries either.
         * the table in CSTORE_NAMESPACE such like pg_cudesc_xxx, pg_delta_xxx
         * don't have their own entries either.
         */
        if (tuple_class->relkind != RELKIND_COMPOSITE_TYPE && tuple_class->relkind != RELKIND_INDEX &&
            tuple_class->relkind != RELKIND_TOASTVALUE && tuple_class->relnamespace != CSTORE_NAMESPACE)
            changeDependencyOnOwner(RelationRelationId, relationOid, newOwnerId);

        /*
         * Also change the ownership of the table's row type, if it has one
         */
        if (tuple_class->relkind != RELKIND_INDEX)
            AlterTypeOwnerInternal(tuple_class->reltype, newOwnerId, tuple_class->relkind == RELKIND_COMPOSITE_TYPE);

        /*
         * If we are operating on a table, also change the ownership of any
         * indexes and sequences that belong to the table, as well as the
         * table's toast table (if it has one)
         */
        if (tuple_class->relkind == RELKIND_RELATION || tuple_class->relkind == RELKIND_TOASTVALUE) {
            List* index_oid_list = NIL;
            ListCell* i = NULL;

            /* Find all the indexes belonging to this relation */
            index_oid_list = RelationGetIndexList(target_rel);

            /* For each index, recursively change its ownership */
            foreach (i, index_oid_list)
                ATExecChangeOwner(lfirst_oid(i), newOwnerId, true, lockmode);

            list_free_ext(index_oid_list);
        }

        if (tuple_class->relkind == RELKIND_RELATION) {
            /* If it has a toast table, recurse to change its ownership */
            if (tuple_class->reltoastrelid != InvalidOid)
                ATExecChangeOwner(tuple_class->reltoastrelid, newOwnerId, true, lockmode);

            /* If it has a cudesc table, recurse to change its ownership */
            if (tuple_class->relcudescrelid != InvalidOid)
                ATExecChangeOwner(tuple_class->relcudescrelid, newOwnerId, true, lockmode);

            /* If it has a delta table, recurse to change its ownership */
            if (tuple_class->reldeltarelid != InvalidOid)
                ATExecChangeOwner(tuple_class->reldeltarelid, newOwnerId, true, lockmode);

            /* If it has dependent sequences, recurse to change them too */
            change_owner_recurse_to_sequences(relationOid, newOwnerId, lockmode);
        }

        if (tuple_class->relkind == RELKIND_INDEX && tuple_class->relam == PSORT_AM_OID) {
            /* if it is PSORT index,  recurse to change PSORT releateion's ownership */
            if (tuple_class->relcudescrelid != InvalidOid)
                ATExecChangeOwner(tuple_class->relcudescrelid, newOwnerId, true, lockmode);
        }

        if (tuple_class->relkind == RELKIND_FOREIGN_TABLE) {
            // if it is a foreign talbe, should change its errortable's ownership
            DefElem* def = GetForeignTableOptionByName(relationOid, optErrorRel);

            if (def != NULL) {
                char* relname = strVal(def->arg);
                Oid errorOid = get_relname_relid(relname, get_rel_namespace(relationOid));
                ATExecChangeOwner(errorOid, newOwnerId, true, lockmode);
            }
        }

        if (tuple_class->parttype == PARTTYPE_PARTITIONED_RELATION) {
            List* partCacheList = NIL;
            ListCell* cell = NULL;
            HeapTuple partTuple = NULL;
            Form_pg_partition partForm = NULL;

            if (tuple_class->relkind == RELKIND_RELATION) {
                partCacheList = searchPgPartitionByParentId(PART_OBJ_TYPE_TABLE_PARTITION, relationOid);
            } else if (tuple_class->relkind == RELKIND_INDEX) {
                partCacheList = searchPgPartitionByParentId(PART_OBJ_TYPE_INDEX_PARTITION, relationOid);
            } else {
                partCacheList = NIL;
            }
            if (partCacheList != NIL) {
                foreach (cell, partCacheList) {
                    partTuple = (HeapTuple)lfirst(cell);
                    if (PointerIsValid(partTuple)) {
                        partForm = (Form_pg_partition)GETSTRUCT(partTuple);
                        /* If it has a toast table, recurse to change its ownership */
                        if (partForm->reltoastrelid != InvalidOid)
                            ATExecChangeOwner(partForm->reltoastrelid, newOwnerId, true, lockmode);
                        /* If it has a cudesc table, recurse to change its ownership */
                        if (partForm->relcudescrelid != InvalidOid)
                            ATExecChangeOwner(partForm->relcudescrelid, newOwnerId, true, lockmode);
                        /* If it has a delta table, recurse to change its ownership */
                        if (partForm->reldeltarelid != InvalidOid)
                            ATExecChangeOwner(partForm->reldeltarelid, newOwnerId, true, lockmode);
                    }
                }
                freePartList(partCacheList);
            }
        }
    }

    ReleaseSysCache(tuple);
    heap_close(class_rel, RowExclusiveLock);
    relation_close(target_rel, NoLock);
}

/*
 * change_owner_fix_column_acls
 *
 * Helper function for ATExecChangeOwner.  Scan the columns of the table
 * and fix any non-null column ACLs to reflect the new owner.
 */
static void change_owner_fix_column_acls(Oid relationOid, Oid oldOwnerId, Oid newOwnerId)
{
    Relation attRelation;
    SysScanDesc scan;
    ScanKeyData key[1];
    HeapTuple attributeTuple;

    attRelation = heap_open(AttributeRelationId, RowExclusiveLock);
    ScanKeyInit(&key[0], Anum_pg_attribute_attrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relationOid));
    scan = systable_beginscan(attRelation, AttributeRelidNumIndexId, true, SnapshotNow, 1, key);
    while (HeapTupleIsValid(attributeTuple = systable_getnext(scan))) {
        Form_pg_attribute att = (Form_pg_attribute)GETSTRUCT(attributeTuple);
        Datum repl_val[Natts_pg_attribute];
        bool repl_null[Natts_pg_attribute];
        bool repl_repl[Natts_pg_attribute];
        Acl* newAcl = NULL;
        Datum aclDatum;
        bool isNull = false;
        HeapTuple newtuple;
        errno_t rc = EOK;

        /* Ignore dropped columns */
        if (att->attisdropped)
            continue;

        aclDatum = heap_getattr(attributeTuple, Anum_pg_attribute_attacl, RelationGetDescr(attRelation), &isNull);
        /* Null ACLs do not require changes */
        if (isNull)
            continue;

        rc = memset_s(repl_null, sizeof(repl_null), false, sizeof(repl_null));
        securec_check(rc, "\0", "\0");
        rc = memset_s(repl_repl, sizeof(repl_repl), false, sizeof(repl_repl));
        securec_check(rc, "\0", "\0");

        newAcl = aclnewowner(DatumGetAclP(aclDatum), oldOwnerId, newOwnerId);
        repl_repl[Anum_pg_attribute_attacl - 1] = true;
        repl_val[Anum_pg_attribute_attacl - 1] = PointerGetDatum(newAcl);

        newtuple = heap_modify_tuple(attributeTuple, RelationGetDescr(attRelation), repl_val, repl_null, repl_repl);

        simple_heap_update(attRelation, &newtuple->t_self, newtuple);
        CatalogUpdateIndexes(attRelation, newtuple);

        heap_freetuple_ext(newtuple);
    }
    systable_endscan(scan);
    heap_close(attRelation, RowExclusiveLock);
}

/*
 * change_owner_recurse_to_sequences
 *
 * Helper function for ATExecChangeOwner.  Examines pg_depend searching
 * for sequences that are dependent on serial columns, and changes their
 * ownership.
 */
static void change_owner_recurse_to_sequences(Oid relationOid, Oid newOwnerId, LOCKMODE lockmode)
{
    Relation depRel;
    SysScanDesc scan;
    ScanKeyData key[2];
    HeapTuple tup;

    /*
     * SERIAL sequences are those having an auto dependency on one of the
     * table's columns (we don't care *which* column, exactly).
     */
    depRel = heap_open(DependRelationId, AccessShareLock);

    ScanKeyInit(
        &key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
    ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relationOid));
    /* we leave refobjsubid unspecified */
    scan = systable_beginscan(depRel, DependReferenceIndexId, true, SnapshotNow, 2, key);

    while (HeapTupleIsValid(tup = systable_getnext(scan))) {
        Form_pg_depend depForm = (Form_pg_depend)GETSTRUCT(tup);
        Relation seqRel;

        /* skip dependencies other than auto dependencies on columns */
        if (depForm->refobjsubid == 0 || depForm->classid != RelationRelationId || depForm->objsubid != 0 ||
            depForm->deptype != DEPENDENCY_AUTO)
            continue;

        /* Use relation_open just in case it's an index */
        seqRel = relation_open(depForm->objid, lockmode);

        /* skip non-sequence relations */
        if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE) {
            /* No need to keep the lock */
            relation_close(seqRel, lockmode);
            continue;
        }

        /* We don't need to close the sequence while we alter it. */
        ATExecChangeOwner(depForm->objid, newOwnerId, true, lockmode);

        /* Now we can close it.  Keep the lock till end of transaction. */
        relation_close(seqRel, NoLock);
    }

    systable_endscan(scan);

    relation_close(depRel, AccessShareLock);
}

/*
 * ALTER TABLE CLUSTER ON
 *
 * The only thing we have to do is to change the indisclustered bits.
 */
static void ATExecClusterOn(Relation rel, const char* indexName, LOCKMODE lockmode)
{
    Oid indexOid;

    indexOid = get_relname_relid(indexName, rel->rd_rel->relnamespace);

    if (!OidIsValid(indexOid))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("index \"%s\" for table \"%s\" does not exist", indexName, RelationGetRelationName(rel))));

    /* Check index is valid to cluster on */
    check_index_is_clusterable(rel, indexOid, false, lockmode);

    /* And do the work */
    mark_index_clustered(rel, indexOid);
}

/*
 * ALTER TABLE SET WITHOUT CLUSTER
 *
 * We have to find any indexes on the table that have indisclustered bit
 * set and turn it off.
 */
static void ATExecDropCluster(Relation rel, LOCKMODE lockmode)
{
    mark_index_clustered(rel, InvalidOid);
}

/*
 * ALTER TABLE SET TABLESPACE
 */
static void ATPrepSetTableSpace(AlteredTableInfo* tab, Relation rel, const char* tablespacename, LOCKMODE lockmode)
{
    Oid tablespaceId;
    AclResult aclresult;

    /* Check that the tablespace exists */
    tablespaceId = get_tablespace_oid(tablespacename, false);

    /* Check its permissions */
    aclresult = pg_tablespace_aclcheck(tablespaceId, GetUserId(), ACL_CREATE);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error(aclresult, ACL_KIND_TABLESPACE, tablespacename);

    /* Save info for Phase 3 to do the real work */
    if (OidIsValid(tab->newTableSpace))
        ereport(ERROR, (errcode(ERRCODE_INVALID_OPERATION), errmsg("cannot have multiple SET TABLESPACE subcommands")));
    tab->newTableSpace = tablespaceId;
}

// psort tuple is fetched from pg_class either normal or partition relation.
// for normal relation, the tuple oid is stored in FormData_pg_class.relcudescrelid,
// while partition relation, tuple oid is stored in Form_pg_partition.relcudescrelid.
//
static void ATExecSetPSortOption(Oid psortOid, List* defList, AlterTableType operation, LOCKMODE lockmode)
{
    HeapTuple newtuple;
    Datum datum;
    bool isnull = false;
    Datum newOptions;
    Datum repl_val[Natts_pg_class];
    bool repl_null[Natts_pg_class];
    bool repl_repl[Natts_pg_class];

    Assert(InvalidOid != psortOid);
    Relation pgclass = heap_open(RelationRelationId, RowExclusiveLock);
    Relation psortRel = heap_open(psortOid, lockmode);

    HeapTuple psortTup = SearchSysCache1(RELOID, ObjectIdGetDatum(psortOid));
    if (!HeapTupleIsValid(psortTup)) {
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", psortOid)));
    }

    // check that it's a Column-Store heap relation.
    if (!RelationIsColStore(psortRel)) {
        if (RELKIND_RELATION != psortRel->rd_rel->relkind) {
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("relkind of psort tuple shouldn't be '%c'.", psortRel->rd_rel->relkind),
                    errdetail("psort tuple's relkind must be '%c'.", RELKIND_RELATION)));
        } else {
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("psort tuple doesn't have the correct ORIENTATION value."),
                    errdetail("ORIENTATION value within psort tuple must be COLUMN.")));
        }
    }

    ForbidUserToSetDefinedOptions(defList);
    ForbidToSetOptionsForPSort(defList);

    if (operation == AT_ReplaceRelOptions) {
        datum = (Datum)0;
        isnull = true;
    } else {
        datum = SysCacheGetAttr(RELOID, psortTup, Anum_pg_class_reloptions, &isnull);
    }

    newOptions =
        transformRelOptions(isnull ? (Datum)0 : datum, defList, NULL, NULL, false, operation == AT_ResetRelOptions);

    bytea* heapRelOpt = heap_reloptions(RELKIND_RELATION, newOptions, true);
    Assert(heapRelOpt != NULL);
    CheckCStoreRelOption((StdRdOptions*)heapRelOpt);

    errno_t rc = 0;
    rc = memset_s(repl_val, sizeof(repl_val), 0, sizeof(repl_val));
    securec_check_c(rc, "\0", "\0");
    rc = memset_s(repl_null, sizeof(repl_null), false, sizeof(repl_null));
    securec_check_c(rc, "\0", "\0");
    rc = memset_s(repl_repl, sizeof(repl_repl), false, sizeof(repl_repl));
    securec_check_c(rc, "\0", "\0");

    if (newOptions != (Datum)0)
        repl_val[Anum_pg_class_reloptions - 1] = newOptions;
    else
        repl_null[Anum_pg_class_reloptions - 1] = true;
    repl_repl[Anum_pg_class_reloptions - 1] = true;

    newtuple = heap_modify_tuple(psortTup, RelationGetDescr(pgclass), repl_val, repl_null, repl_repl);
    simple_heap_update(pgclass, &newtuple->t_self, newtuple);
    CatalogUpdateIndexes(pgclass, newtuple);
    heap_freetuple_ext(newtuple);

    ReleaseSysCache(psortTup);

    heap_close(psortRel, NoLock);
    heap_close(pgclass, RowExclusiveLock);
}

static void ATExecSetRelOptionsToast(Oid toastid, List* defList, AlterTableType operation, LOCKMODE lockmode)
{
    Relation pgclass;
    HeapTuple tuple;
    HeapTuple newtuple;
    Datum datum;
    bool isnull = false;
    Datum newOptions;
    Datum repl_val[Natts_pg_class];
    bool repl_null[Natts_pg_class];
    bool repl_repl[Natts_pg_class];
    static const char* const validnsps[] = HEAP_RELOPT_NAMESPACES;
    errno_t rc = EOK;

    Relation toastrel;

    pgclass = heap_open(RelationRelationId, RowExclusiveLock);

    toastrel = heap_open(toastid, lockmode);

    /* Fetch heap tuple */
    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(toastid));
    if (!HeapTupleIsValid(tuple)) {
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", toastid)));
    }

    if (operation == AT_ReplaceRelOptions) {
        /*
         * If we're supposed to replace the reloptions list, we just
         * pretend there were none before.
         */
        datum = (Datum)0;
        isnull = true;
    } else {
        /* Get the old reloptions */
        datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);
    }

    newOptions = transformRelOptions(
        isnull ? (Datum)0 : datum, defList, "toast", validnsps, false, operation == AT_ResetRelOptions);

    (void)heap_reloptions(RELKIND_TOASTVALUE, newOptions, true);

    rc = memset_s(repl_val, sizeof(repl_val), 0, sizeof(repl_val));
    securec_check(rc, "\0", "\0");
    rc = memset_s(repl_null, sizeof(repl_null), false, sizeof(repl_null));
    securec_check(rc, "\0", "\0");
    rc = memset_s(repl_repl, sizeof(repl_repl), false, sizeof(repl_repl));
    securec_check(rc, "\0", "\0");

    if (newOptions != (Datum)0)
        repl_val[Anum_pg_class_reloptions - 1] = newOptions;
    else
        repl_null[Anum_pg_class_reloptions - 1] = true;

    repl_repl[Anum_pg_class_reloptions - 1] = true;

    newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass), repl_val, repl_null, repl_repl);

    simple_heap_update(pgclass, &newtuple->t_self, newtuple);

    CatalogUpdateIndexes(pgclass, newtuple);

    heap_freetuple_ext(newtuple);

    ReleaseSysCache(tuple);

    heap_close(toastrel, NoLock);
    heap_close(pgclass, RowExclusiveLock);
}

/*
 * Set, reset, or replace reloptions.
 */
static void ATExecSetRelOptions(Relation rel, List* defList, AlterTableType operation, LOCKMODE lockmode)
{
    Oid relid;
    Relation pgclass;
    HeapTuple tuple;
    HeapTuple newtuple;
    Datum datum;
    bool isnull = false;
    Datum newOptions;
    Datum repl_val[Natts_pg_class];
    bool repl_null[Natts_pg_class];
    bool repl_repl[Natts_pg_class];
    static const char* const validnsps[] = HEAP_RELOPT_NAMESPACES;
    List* partCacheList = NIL;
    ListCell* cell = NULL;
    Oid toastOid = InvalidOid;
    HeapTuple partTuple = NULL;
    Form_pg_partition partForm = NULL;
    errno_t rc = EOK;
    Oid rel_cn_oid = InvalidOid;
    RedisHtlAction redis_action = REDIS_REL_INVALID;
    char* merge_list = NULL;

    if (defList == NIL && operation != AT_ReplaceRelOptions)
        return; /* nothing to do */

    if (GttOncommitOption(defList) != ONCOMMIT_NOOP) {
        ereport(ERROR, 
            (errcode(ERRCODE_INVALID_OPERATION), 
                errmsg("table cannot add or modify on commit parameter by ALTER TABLE command.")));
    }

    /* forbid user to set or change inner options */
    ForbidOutUsersToSetInnerOptions(defList);

    CheckRedistributeOption(defList, &rel_cn_oid, &redis_action, merge_list, rel);

    pgclass = heap_open(RelationRelationId, RowExclusiveLock);

    /* Fetch heap tuple */
    relid = RelationGetRelid(rel);
    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(tuple)) {
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", relid)));
    }

    // we have to handle psort tuple's options if this is an index relation using PSORT method.
    // it's identifyed by access method whose oid is PSORT_AM_OID.
    // and the psort tuple id is saved in index relation's relcudescrelid field.
    //
    bool needSetPsortOpt = false;
    Oid psortTid = InvalidOid;
    Oid indexAmId = DatumGetObjectId(SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relam, &isnull));
    if (!isnull && indexAmId == PSORT_AM_OID) {
        needSetPsortOpt = true;
        Assert(RelationIsIndex(rel));
        psortTid = DatumGetObjectId(SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relcudescrelid, &isnull));
    }

    if (operation == AT_ReplaceRelOptions) {
        /*
         * If we're supposed to replace the reloptions list, we just pretend
         * there were none before.
         */
        datum = (Datum)0;
        isnull = true;
    } else {
        /* Get the old reloptions */
        datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);
    }

    /* remove the redis reloptions. */
    if (redis_action == REDIS_REL_NORMAL) {
        List* old_reloptions = untransformRelOptions(datum);
        RemoveRedisRelOptionsFromList(&old_reloptions);
        RemoveRedisRelOptionsFromList(&defList);

        /* defList should keep unchanged. */
        if (old_reloptions != NIL) {
            defList = list_concat(defList, old_reloptions);
        }
        /* Generate new proposed reloptions (text array) */
        newOptions = transformRelOptions((Datum)0, defList, NULL, validnsps, false, operation == AT_ResetRelOptions);

        if (old_reloptions != NIL && defList != NULL) {
            defList = list_truncate(defList, list_length(defList) - list_length(old_reloptions));
        }

        list_free_ext(old_reloptions);
    } else if (redis_action == REDIS_REL_READ_ONLY) {
        List* redis_reloptions = AlterTableSetRedistribute(rel, redis_action, merge_list);
        List* old_reloptions = untransformRelOptions(datum);
        RemoveRedisRelOptionsFromList(&old_reloptions);

        if (old_reloptions != NIL) {
            redis_reloptions = list_concat(redis_reloptions, old_reloptions);
        }

        if (redis_reloptions != NIL) {
            defList = list_concat(defList, redis_reloptions);
        }

        newOptions = transformRelOptions((Datum)0, defList, NULL, validnsps, false, operation == AT_ResetRelOptions);

        if (redis_reloptions != NIL) {
            defList = list_truncate(defList, list_length(defList) - list_length(redis_reloptions));
        }

        list_free_ext(redis_reloptions);
    } else {
        /* Generate new proposed reloptions (text array) */
        List* redis_reloptions = AlterTableSetRedistribute(rel, redis_action, merge_list);

        if (redis_reloptions != NIL) {
            defList = list_concat(defList, redis_reloptions);
        }

        newOptions = transformRelOptions(
            isnull ? (Datum)0 : datum, defList, NULL, validnsps, false, operation == AT_ResetRelOptions);
        if (redis_reloptions != NIL) {
            defList = list_truncate(defList, list_length(defList) - list_length(redis_reloptions));
        }

        list_free_ext(redis_reloptions);
    }

    /* Validate */
    switch (rel->rd_rel->relkind) {
        case RELKIND_RELATION: {
            /* this options only can be used when define a new relation.
             * forbid to change or reset these options.
             */
            ForbidUserToSetDefinedOptions(defList);

            bytea* heapRelOpt = heap_reloptions(rel->rd_rel->relkind, newOptions, true);
            if (RelationIsColStore(rel)) {
                /* un-supported options. dont care its values */
                ForbidToSetOptionsForColTbl(defList);

                if (NULL != heapRelOpt) {
                    /* validate the values of these options */
                    CheckCStoreRelOption((StdRdOptions*)heapRelOpt);
                }
            } else if (RelationIsTsStore(rel)) {
                forbid_to_set_options_for_timeseries_tbl(defList);
            } else {
                /* un-supported options. dont care its values */
                ForbidToSetOptionsForRowTbl(defList);
            }

            /* validate the values of ttl and period for partition manager */
            if (NULL != heapRelOpt) {
                check_partion_policy_rel_option(defList, (StdRdOptions*)heapRelOpt);
            }
            break;
        }
        case RELKIND_TOASTVALUE:
        case RELKIND_VIEW: {
            (void)heap_reloptions(rel->rd_rel->relkind, newOptions, true);
            break;
        }
        case RELKIND_INDEX:
            (void)index_reloptions(rel->rd_am->amoptions, newOptions, true);
            break;
        default:
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("\"%s\" is not a table, index, or TOAST table", RelationGetRelationName(rel))));
            break;
    }

    /*
     * All we need do here is update the pg_class row; the new options will be
     * propagated into relcaches during post-commit cache inval.
     */
    rc = memset_s(repl_val, sizeof(repl_val), 0, sizeof(repl_val));
    securec_check(rc, "\0", "\0");
    rc = memset_s(repl_null, sizeof(repl_null), false, sizeof(repl_null));
    securec_check(rc, "\0", "\0");
    rc = memset_s(repl_repl, sizeof(repl_repl), false, sizeof(repl_repl));
    securec_check(rc, "\0", "\0");

    if (newOptions != (Datum)0)
        repl_val[Anum_pg_class_reloptions - 1] = newOptions;
    else
        repl_null[Anum_pg_class_reloptions - 1] = true;

    repl_repl[Anum_pg_class_reloptions - 1] = true;

    newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass), repl_val, repl_null, repl_repl);

    simple_heap_update(pgclass, &newtuple->t_self, newtuple);

    CatalogUpdateIndexes(pgclass, newtuple);

    heap_freetuple_ext(newtuple);

    ReleaseSysCache(tuple);

    /* repeat the whole exercise for the toast table, if there's one */
    if (RELATION_IS_PARTITIONED(rel)) {
        partCacheList = searchPgPartitionByParentId(PART_OBJ_TYPE_TABLE_PARTITION, relid);
        foreach (cell, partCacheList) {
            partTuple = (HeapTuple)lfirst(cell);

            if (PointerIsValid(partTuple)) {
                partForm = (Form_pg_partition)GETSTRUCT(partTuple);
                toastOid = partForm->reltoastrelid;
                if (OidIsValid(toastOid)) {
                    ATExecSetRelOptionsToast(toastOid, defList, operation, lockmode);
                }
            }
        }
    } else if (RelationIsPartitioned(rel) && needSetPsortOpt) {
        // fetch index-partition tuples from pg_partition table, which has the same parentid,
        // then set the relation-options for psort tuple in pg_class.
        //
        partCacheList = searchPgPartitionByParentId(PART_OBJ_TYPE_INDEX_PARTITION, relid);
        foreach (cell, partCacheList) {
            partTuple = (HeapTuple)lfirst(cell);
            if (PointerIsValid(partTuple)) {
                partForm = (Form_pg_partition)GETSTRUCT(partTuple);
                ATExecSetPSortOption(partForm->relcudescrelid, defList, operation, lockmode);
            }
        }
    } else {
        if (OidIsValid(rel->rd_rel->reltoastrelid)) {
            ATExecSetRelOptionsToast(rel->rd_rel->reltoastrelid, defList, operation, lockmode);
        }
        if (needSetPsortOpt) {
            ATExecSetPSortOption(psortTid, defList, operation, lockmode);
        }
    }
    heap_close(pgclass, RowExclusiveLock);

    if (RELATION_IS_PARTITIONED(rel)) {
        AlterTableSetPartRelOptions(rel, defList, operation, lockmode, merge_list, redis_action);
        if (IS_MAIN_COORDINATOR) {
            alter_partition_policy_if_needed(rel, defList);
        }
    }
    if (merge_list != NULL) {
        pfree(merge_list);
        merge_list = NULL;
    }
}

/*
 * Target		: data partition
 * Brief		:
 * Description	: get oid of target partition from Node *partition, and
 *				: save it in AlteredTableInfo->partid
 * Notes		:
 */
static void ATExecSetTableSpaceForPartitionP2(AlteredTableInfo* tab, Relation rel, Node* partition)
{
    RangePartitionDefState* rangePartDef = NULL;
    Oid partOid = InvalidOid;

    if (!RelationIsPartitioned(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_OPERATION),
                errmsg(
                    "\"%s\" must be a partitioned table for 'MOVE PARTITION CLAUSE'", RelationGetRelationName(rel))));
    }

    if (!RelationIsRelation(rel) && !RelationIsIndex(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_OPERATION),
                errmsg("can not set tablespace for partition of neither table nor index")));
    }

    if (OidIsValid(tab->partid)) {
        ereport(
            ERROR, (errcode(ERRCODE_INVALID_OPERATION), errmsg("cannot have multiple MOVE TABLESPACE subcommands")));
    }

    /* check partition node type, it can be RangeVar or RangePartitionDefState */
    switch (nodeTag(partition)) {
        case T_RangeVar: {
            char objectType = RelationIsRelation(rel) ? PART_OBJ_TYPE_TABLE_PARTITION : PART_OBJ_TYPE_INDEX_PARTITION;

            partOid = partitionNameGetPartitionOid(rel->rd_id,
                ((RangeVar*)partition)->relname,
                objectType,
                AccessExclusiveLock,
                false,
                false,
                NULL,
                NULL,
                NoLock);
            break;
        }
        case T_RangePartitionDefState: {
            if (RelationIsIndex(rel)) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("can not specify 'PARTITION FOR (value,,,)' for 'MOVE PARTITION CLAUSE'")));
            }
            rangePartDef = (RangePartitionDefState*)partition;
            transformRangePartitionValue(make_parsestate(NULL), (Node*)rangePartDef, false);
            rangePartDef->boundary = transformConstIntoTargetType(rel->rd_att->attrs,
                ((RangePartitionMap*)rel->partMap)->partitionKey,
                rangePartDef->boundary);
            partOid =
                partitionValuesGetPartitionOid(rel, rangePartDef->boundary, AccessExclusiveLock, true, false, false);
            break;
        }
        default: {
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("invalid partition node type in 'MOVE PARTITION CLAUSE'")));
            break;
        }
    }

    /* cehck oid validity of found partition */
    if (!OidIsValid(partOid)) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("The partition number is invalid or out-of-range")));
    }
    tab->partid = partOid;
}

/*
 * Target		: data partition
 * Brief		:
 * Description	: move a partition to new tablespace
 * Notes		:
 */
static void ATExecSetTableSpaceForPartitionP3(Oid tableOid, Oid partOid, Oid newTableSpace, LOCKMODE lockmode)
{
    Relation rel;
    Relation partRel;
    Partition part;
    Oid oldTableSpace;
    Oid reltoastrelid;
    Oid reltoastidxid;
    Oid newrelfilenode;
    Relation pg_partition;
    HeapTuple tuple;
    Form_pg_partition pd_part;

    /*
     * Need lock here in case we are recursing to toast table or index
     */
    rel = relation_open(tableOid, NoLock);
    part = partitionOpen(rel, partOid, lockmode);

    /*
     * No work if no change in tablespace.
     */
    oldTableSpace = part->pd_part->reltablespace;
    if (newTableSpace == oldTableSpace ||
        (newTableSpace == u_sess->proc_cxt.MyDatabaseTableSpace && oldTableSpace == 0)) {
        partitionClose(rel, part, NoLock);
        relation_close(rel, NoLock);
        return;
    }

    /* Can't move a non-shared relation into pg_global */
    if (newTableSpace == GLOBALTABLESPACE_OID)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("only shared relations can be placed in pg_global tablespace")));

    reltoastrelid = part->pd_part->reltoastrelid;
    reltoastidxid = part->pd_part->reltoastidxid;

    /* Get a modifiable copy of the relation's pg_class row */
    pg_partition = heap_open(PartitionRelationId, RowExclusiveLock);

    tuple = SearchSysCacheCopy1(PARTRELID, ObjectIdGetDatum(partOid));
    if (!HeapTupleIsValid(tuple))
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for partition %u", partOid)));

    pd_part = (Form_pg_partition)GETSTRUCT(tuple);

    /*
     * Relfilenodes are not unique across tablespaces, so we need to allocate
     * a new one in the new tablespace.
     */
    newrelfilenode = GetNewRelFileNode(newTableSpace, NULL, rel->rd_rel->relpersistence);
    partRel = partitionGetRelation(rel, part);
    atexecset_table_space(partRel, newTableSpace, newrelfilenode);

    elog(LOG,
        "Row Partition: %s(%u) tblspc %u/%u/%u => %u/%u/%u",
        RelationGetRelationName(partRel),
        RelationGetRelid(partRel),
        partRel->rd_node.spcNode,
        partRel->rd_node.dbNode,
        partRel->rd_node.relNode,
        newTableSpace,
        partRel->rd_node.dbNode,
        newrelfilenode);

    /* update the pg_class row */
    pd_part->reltablespace = ConvertToPgclassRelTablespaceOid(newTableSpace);
    pd_part->relfilenode = newrelfilenode;

    simple_heap_update(pg_partition, &tuple->t_self, tuple);
    CatalogUpdateIndexes(pg_partition, tuple);

    heap_freetuple_ext(tuple);

    heap_close(pg_partition, RowExclusiveLock);

    partitionClose(rel, part, NoLock);
    releaseDummyRelation(&partRel);
    relation_close(rel, NoLock);

    /* Make sure the reltablespace change is visible */
    CommandCounterIncrement();

    /* Move associated toast relation and/or index, too */
    if (OidIsValid(reltoastrelid))
        ATExecSetTableSpace(reltoastrelid, newTableSpace, lockmode);
    if (OidIsValid(reltoastidxid))
        ATExecSetTableSpace(reltoastidxid, newTableSpace, lockmode);
}

static void atexecset_table_space_internal(Relation rel, Oid newTableSpace, Oid newrelfilenode)
{
    ForkNumber forkNum;
    RelFileNode newrnode;
    SMgrRelation dstrel;

    Assert(!RELATION_OWN_BUCKET(rel));
    /*
     * Since we copy the file directly without looking at the shared buffers,
     * we'd better first flush out any pages of the source relation that are
     * in shared buffers.  We assume no new changes will be made while we are
     * holding exclusive lock on the rel.
     */
    FlushRelationBuffers(rel);

    /* Open old and new relation */
    newrnode = rel->rd_node;
    newrnode.relNode = newrelfilenode;
    newrnode.spcNode = newTableSpace;

    dstrel = smgropen(newrnode, rel->rd_backend);
    RelationOpenSmgr(rel);

    /*
     * Create and copy all forks of the relation, and schedule unlinking of
     * old physical files.
     *
     * NOTE: any conflict in relfilenode value will be caught in
     * RelationCreateStorage function.
     */
    RelationCreateStorage(newrnode, rel->rd_rel->relpersistence, rel->rd_rel->relowner, rel->rd_bucketoid, rel);

    /* copy main fork */
    copy_relation_data(rel, &dstrel, MAIN_FORKNUM, rel->rd_rel->relpersistence);

    /* copy those extra forks that exist */
    for (forkNum = (ForkNumber)(MAIN_FORKNUM + 1); forkNum <= MAX_FORKNUM; forkNum = (ForkNumber)(forkNum + 1)) {
        /* it's meaningless to copy BCM files, so ignore it */
        if ((forkNum != BCM_FORKNUM) && smgrexists(rel->rd_smgr, forkNum)) {
            smgrcreate(dstrel, forkNum, false);

            /*
             * WAL log creation if the relation is persistent, or this is the
             * init fork of an unlogged relation.
             */
            if (RelationNeedsWAL(rel) ||
                (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED && forkNum == INIT_FORKNUM))
                log_smgrcreate(&newrnode, forkNum);

            copy_relation_data(rel, &dstrel, forkNum, rel->rd_rel->relpersistence);
        }
    }

    // We must open smgr again, because once deal with invalid syscache msg,
    // smgr maybe is closed and removed from smgr hash table, thus dst and
    // rel->smgr are dangling pointer. If this memory area are reused, it is very
    // dangerous if we still use dst and rel->smgr.
    //
    RelationOpenSmgr(rel);

    /* drop old relation, and close new one */
    RelationDropStorage(rel);
    smgrclose(dstrel);
}

static void atexecset_table_space(Relation rel, Oid newTableSpace, Oid newrelfilenode)
{
    if (RELATION_CREATE_BUCKET(rel)) {
        oidvector* bucketlist = searchHashBucketByOid(rel->rd_bucketoid);

        for (int i = 0; i < bucketlist->dim1; i++) {
            Relation bucket = bucketGetRelation(rel, NULL, bucketlist->values[i]);

            atexecset_table_space_internal(bucket, newTableSpace, newrelfilenode);

            bucketCloseRelation(bucket);
        }
    } else {
        atexecset_table_space_internal(rel, newTableSpace, newrelfilenode);
    }
}

static bool NeedToSetTableSpace(Relation oldRelation, Oid targetTableSpace)
{
    /*
     * No work if no change in tablespace.
     */
    Oid oldTableSpace = oldRelation->rd_rel->reltablespace;
    if (targetTableSpace == oldTableSpace ||
        (targetTableSpace == u_sess->proc_cxt.MyDatabaseTableSpace && oldTableSpace == InvalidOid)) {
        return false;
    }

    /*
     * We cannot support moving mapped relations into different tablespaces.
     * (In particular this eliminates all shared catalogs.)
     */
    if (RelationIsMapped(oldRelation))
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("cannot move system relation \"%s\"", RelationGetRelationName(oldRelation))));

    /* Can't move a non-shared relation into pg_global */
    if (targetTableSpace == GLOBALTABLESPACE_OID)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("only shared relations can be placed in pg_global tablespace")));

    /*
     * Don't allow moving temp tables of other backends ... their local buffer
     * manager is not going to cope.
     * If the top relation is a temp relation in this session, then we treat its
     * all subordinate relations as temp.
     */
    if (!u_sess->cmd_cxt.topRelatationIsInMyTempSession && RELATION_IS_OTHER_TEMP(oldRelation))
        ereport(
            ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("cannot move temporary tables of other sessions")));

    return true;
}

/*
 * Execute ALTER TABLE SET TABLESPACE for cases where there is no tuple
 * rewriting to be done, so we just want to copy the data as fast as possible.
 */
static void ATExecSetTableSpace(Oid tableOid, Oid newTableSpace, LOCKMODE lockmode)
{
    Relation rel;
    Oid reltoastrelid;
    Oid reltoastidxid;
    Oid newrelfilenode;
    Relation pg_class;
    HeapTuple tuple;
    Form_pg_class rd_rel;

    /* require that newTableSpace is valid */
    Assert(OidIsValid(newTableSpace));

    /*
     * Need lock here in case we are recursing to toast table or index
     */
    rel = relation_open(tableOid, lockmode);

    if (RELATION_IS_GLOBAL_TEMP(rel)) {
        ereport(ERROR, 
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), 
                errmsg("not support alter table set tablespace on global temp table.")));
    }
    /*
     * No work if no change in tablespace.
     */
    if (!NeedToSetTableSpace(rel, newTableSpace)) {
        relation_close(rel, NoLock);
        return;
    }

    reltoastrelid = rel->rd_rel->reltoastrelid;
    reltoastidxid = rel->rd_rel->reltoastidxid;

    /* Get a modifiable copy of the relation's pg_class row */
    pg_class = heap_open(RelationRelationId, RowExclusiveLock);

    tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(tableOid));
    if (!HeapTupleIsValid(tuple)) {
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", tableOid)));
    }
    rd_rel = (Form_pg_class)GETSTRUCT(tuple);

    /*
     * Relfilenodes are not unique across tablespaces, so we need to allocate
     * a new one in the new tablespace.
     */
    newrelfilenode = GetNewRelFileNode(newTableSpace, NULL, rel->rd_rel->relpersistence);
    atexecset_table_space(rel, newTableSpace, newrelfilenode);

    elog(LOG,
        "Row Relation: %s(%u) tblspc %u/%u/%u => %u/%u/%u",
        RelationGetRelationName(rel),
        RelationGetRelid(rel),
        rel->rd_node.spcNode,
        rel->rd_node.dbNode,
        rel->rd_node.relNode,
        newTableSpace,
        rel->rd_node.dbNode,
        newrelfilenode);

    /* update the pg_class row */
    rd_rel->reltablespace = ConvertToPgclassRelTablespaceOid(newTableSpace);
    rd_rel->relfilenode = newrelfilenode;
    simple_heap_update(pg_class, &tuple->t_self, tuple);
    CatalogUpdateIndexes(pg_class, tuple);

    heap_freetuple_ext(tuple);

    heap_close(pg_class, RowExclusiveLock);

    relation_close(rel, NoLock);

    /* Make sure the reltablespace change is visible */
    CommandCounterIncrement();

    /* Move associated toast relation and/or index, too */
    if (OidIsValid(reltoastrelid))
        ATExecSetTableSpace(reltoastrelid, newTableSpace, lockmode);
    if (OidIsValid(reltoastidxid))
        ATExecSetTableSpace(reltoastidxid, newTableSpace, lockmode);
}

/*
 * Copy data, block by block
 */
static void copy_relation_data(Relation rel, SMgrRelation* dstptr, ForkNumber forkNum, char relpersistence)
{
    char* buf = NULL;
    Page page;
    bool use_wal = false;
    bool copying_initfork = false;
    BlockNumber nblocks;
    BlockNumber blkno;
    SMgrRelation src = rel->rd_smgr;
    SMgrRelation dst = *dstptr;
    RelFileNode newFileNode = dst->smgr_rnode.node;
    BackendId backendId = dst->smgr_rnode.backend;

    Assert(!RELATION_OWN_BUCKET(rel));
    /*
     * palloc the buffer so that it's MAXALIGN'd.  If it were just a local
     * char[] array, the compiler might align it on any byte boundary, which
     * can seriously hurt transfer speed to and from the kernel; not to
     * mention possibly making log_newpage's accesses to the page header fail.
     */
    ADIO_RUN()
    {
        buf = (char*)adio_align_alloc(BLCKSZ);
    }
    ADIO_ELSE()
    {
        buf = (char*)palloc(BLCKSZ);
    }
    ADIO_END();
    page = (Page)buf;

    /*
     * The init fork for an unlogged relation in many respects has to be
     * treated the same as normal relation, changes need to be WAL logged and
     * it needs to be synced to disk.
     */
    copying_initfork = relpersistence == RELPERSISTENCE_UNLOGGED && forkNum == INIT_FORKNUM;

    /*
     * We need to log the copied data in WAL iff WAL archiving/streaming is
     * enabled AND it's a permanent relation.
     */
    use_wal = XLogIsNeeded() && (relpersistence == RELPERSISTENCE_PERMANENT || copying_initfork ||
                                    ((relpersistence == RELPERSISTENCE_TEMP) && STMT_RETRY_ENABLED));

    nblocks = smgrnblocks(src, forkNum);

    // check tablespace size limitation when extending new relation file.
    TableSpaceUsageManager::IsExceedMaxsize(newFileNode.spcNode, ((uint64)BLCKSZ) * nblocks);

    // We must open smgr again, because once deal with invalid syscache msg,
    // smgr maybe is closed and removed from smgr hash table, thus dst and
    // rel->smgr are dangling pointer. If this memory area are reused, it is very
    // dangerous if we still use dst and rel->smgr.
    //
    RelationOpenSmgr(rel);
    *dstptr = dst = smgropen(newFileNode, backendId);
    src = rel->rd_smgr;

    /* maybe can add prefetch here */
    for (blkno = 0; blkno < nblocks; blkno++) {
        /* If we got a cancel signal during the copy of the data, quit */
        CHECK_FOR_INTERRUPTS();

        smgrread(src, forkNum, blkno, buf);

        if (!PageIsVerified(page, blkno)) {
            addBadBlockStat(&src->smgr_rnode.node, forkNum);

            if (RelationNeedsWAL(rel) && CanRemoteRead()) {
                ereport(WARNING,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                        errmsg("invalid page in block %u of relation %s, try to remote read",
                            blkno,
                            relpath(src->smgr_rnode, forkNum)),
                        handle_in_client(true)));

                RemoteReadBlock(src->smgr_rnode, forkNum, blkno, buf);

                if (PageIsVerified(page, blkno))
                    smgrwrite(src, forkNum, blkno, buf, false);
                else
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                            (errmsg("fail to remote read page, data corrupted in network"))));
            } else {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                        errmsg("invalid page in block %u of relation %s",
                            blkno,
                            relpathbackend(src->smgr_rnode.node, src->smgr_rnode.backend, forkNum))));
            }
        }

        PageDataDecryptIfNeed(page);

        /*
         * WAL-log the copied page. Unfortunately we don't know what kind of a
         * page this is, so we have to log the full page including any unused
         * space.
         */
        if (use_wal)
            log_newpage(&dst->smgr_rnode.node, forkNum, blkno, page, false);

        char* bufToWrite = PageDataEncryptIfNeed(page);

        PageSetChecksumInplace((Page)bufToWrite, blkno);

        smgrextend(dst, forkNum, blkno, bufToWrite, true);
    }

    ADIO_RUN()
    {
        adio_align_free(buf);
    }
    ADIO_ELSE()
    {
        pfree_ext(buf);
    }
    ADIO_END();

    /*
     * If the rel isn't temp, we must fsync it down to disk before it's safe
     * to commit the transaction.  (For a temp rel we don't care since the rel
     * will be uninteresting after a crash anyway.)
     *
     * It's obvious that we must do this when not WAL-logging the copy. It's
     * less obvious that we have to do it even if we did WAL-log the copied
     * pages. The reason is that since we're copying outside shared buffers, a
     * CHECKPOINT occurring during the copy has no way to flush the previously
     * written data to disk (indeed it won't know the new rel even exists).  A
     * crash later on would replay WAL from the checkpoint, therefore it
     * wouldn't replay our earlier WAL entries. If we do not fsync those pages
     * here, they might still not be on disk when the crash occurs.
     */
    if (relpersistence == RELPERSISTENCE_PERMANENT || copying_initfork ||
        ((relpersistence == RELPERSISTENCE_TEMP) && STMT_RETRY_ENABLED))
        smgrimmedsync(dst, forkNum);
}

static void mergeHeapBlock(Relation src, Relation dest, ForkNumber forkNum, char relpersistence, BlockNumber srcBlocks,
    BlockNumber destBlocks, TupleDesc srcTupleDesc, Oid srcToastOid, Oid destToastOid, HTAB* chunkIdHashTable,
    bool destHasFSM)
{
    char* buf = NULL;
    Page page = NULL;
    bool use_wal = false;
    BlockNumber src_blkno = 0;
    BlockNumber dest_blkno = 0;
    HeapTupleData tuple;
    errno_t rc = EOK;

    if (srcBlocks == 0) {
        return;
    }

    // check tablespace size limitation when extending new files.
    STORAGE_SPACE_OPERATION(dest, ((uint64)BLCKSZ) * srcBlocks);

    /*
     * palloc the buffer so that it's MAXALIGN'd.  If it were just a local
     * char[] array, the compiler might align it on any byte boundary, which
     * can seriously hurt transfer speed to and from the kernel; not to
     * mention possibly making log_newpage's accesses to the page header fail.
     */
    ADIO_RUN()
    {
        buf = (char*)adio_align_alloc(BLCKSZ);
    }
    ADIO_ELSE()
    {
        buf = (char*)palloc(BLCKSZ);
    }
    ADIO_END();
    page = (Page)buf;

    /*
     * We need to log the copied data in WAL iff WAL archiving/streaming is
     * enabled AND it's a permanent relation.
     */
    use_wal = XLogIsNeeded() && (relpersistence == RELPERSISTENCE_PERMANENT ||
                                    ((relpersistence == RELPERSISTENCE_TEMP) && STMT_RETRY_ENABLED));

    for (src_blkno = 0; src_blkno < srcBlocks; src_blkno++) {
        OffsetNumber tupleNum = 0;
        OffsetNumber tupleNo = 0;

        /* If we got a cancel signal during the copy of the data, quit */
        CHECK_FOR_INTERRUPTS();

        RelationOpenSmgr(src);
        smgrread(src->rd_smgr, forkNum, src_blkno, buf);

        if (!PageIsVerified(page, src_blkno)) {
            addBadBlockStat(&src->rd_smgr->smgr_rnode.node, forkNum);

            if (RelationNeedsWAL(src) && CanRemoteRead()) {
                ereport(WARNING,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                        errmsg("invalid page in block %u of relation %s, try to remote read",
                            src_blkno,
                            relpath(src->rd_smgr->smgr_rnode, forkNum)),
                        handle_in_client(true)));

                RemoteReadBlock(src->rd_smgr->smgr_rnode, forkNum, src_blkno, buf);

                if (PageIsVerified(page, src_blkno))
                    smgrwrite(src->rd_smgr, forkNum, src_blkno, buf, false);
                else
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                            (errmsg("fail to remote read page, data corrupted in network"))));
            } else {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                        errmsg("invalid page in block %u of relation %s",
                            src_blkno,
                            relpathbackend(src->rd_smgr->smgr_rnode.node, src->rd_smgr->smgr_rnode.backend, forkNum))));
            }
        }

        PageDataDecryptIfNeed(page);

        dest_blkno = destBlocks + src_blkno;
        tupleNum = PageGetMaxOffsetNumber(page);

        for (tupleNo = FirstOffsetNumber; tupleNo <= tupleNum; tupleNo++) {
            HeapTupleHeader tupleHeader = NULL;
            ItemId tupleItem = NULL;

            tupleItem = PageGetItemId(page, tupleNo);
            if (!ItemIdHasStorage(tupleItem)) {
                continue;
            }

            tupleHeader = (HeapTupleHeader)PageGetItem((Page)page, tupleItem);

            /* set block number */
            ItemPointerSetBlockNumber(
                &(tupleHeader->t_ctid), destBlocks + ItemPointerGetBlockNumber(&(tupleHeader->t_ctid)));

            /* set freeze options for rows in merging file */
            if (u_sess->cmd_cxt.mergePartition_Freeze) {
                HeapTupleCopyBaseFromPage(&tuple, page);
                tuple.t_data = tupleHeader;
                (void)heap_freeze_tuple(&tuple, u_sess->cmd_cxt.mergePartition_FreezeXid);
            }

            /* If toast storage, modify va_toastrelid and va_valueid. */
            if (OidIsValid(destToastOid)) {
                int numAttrs = srcTupleDesc->natts;
                Datum values[numAttrs];
                bool isNull[numAttrs];
                int i = 0;
                ChunkIdHashKey hashkey;
                OldToNewChunkIdMapping mapping = NULL;

                tuple.t_data = tupleHeader;
                tuple.t_len = ItemIdGetLength(tupleItem);
                tuple.t_self = tupleHeader->t_ctid;

                if (!HEAP_TUPLE_IS_COMPRESSED(tuple.t_data)) {
                    heap_deform_tuple(&tuple, srcTupleDesc, values, isNull);
                } else {
                    Assert(page != NULL);
                    Assert(PageIsCompressed(page));
                    heap_deform_cmprs_tuple(&tuple, srcTupleDesc, values, isNull, (char*)getPageDict(page));
                }

                for (i = 0; i < numAttrs; i++) {
                    struct varlena* value = NULL;

                    value = (struct varlena*)DatumGetPointer(values[i]);

                    if (srcTupleDesc->attrs[i]->attlen == -1 && !isNull[i] && VARATT_IS_EXTERNAL(value)) {
                        struct varatt_external* toastPointer = NULL;

                        toastPointer = (varatt_external*)(VARDATA_EXTERNAL((varattrib_1b_e*)(value)));
                        toastPointer->va_toastrelid = destToastOid;

                        rc = memset_s(&hashkey, sizeof(hashkey), 0, sizeof(hashkey));
                        securec_check(rc, "\0", "\0");
                        hashkey.toastTableOid = srcToastOid;
                        hashkey.oldChunkId = toastPointer->va_valueid;

                        mapping = (OldToNewChunkIdMapping)hash_search(chunkIdHashTable, &hashkey, HASH_FIND, NULL);

                        if (PointerIsValid(mapping)) {
                            toastPointer->va_valueid = mapping->newChunkId;
                        }
                    }
                }
            }
        }

        /* merge FSM */
        if (destHasFSM) {
            Size freespace = PageGetHeapFreeSpace(page);

            RecordPageWithFreeSpace(dest, dest_blkno, freespace);
        }

        /*
         * XLOG stuff
         * Retry to open smgr in case it is cloesd when we process SI messages
         */
        RelationOpenSmgr(dest);
        if (use_wal) {
            log_newpage(&dest->rd_smgr->smgr_rnode.node, forkNum, dest_blkno, page, true);
        }

        char* bufToWrite = PageDataEncryptIfNeed(page);

        /* heap block mix in the block number to checksum. need recalculate */
        PageSetChecksumInplace((Page)bufToWrite, dest_blkno);

        smgrextend(dest->rd_smgr, forkNum, dest_blkno, bufToWrite, true);
    }

    ADIO_RUN()
    {
        adio_align_free(buf);
    }
    ADIO_ELSE()
    {
        pfree_ext(buf);
    }
    ADIO_END();

    /*
     * If the rel isn't temp, we must fsync it down to disk before it's safe
     * to commit the transaction.  (For a temp rel we don't care since the rel
     * will be uninteresting after a crash anyway.)
     *
     * It's obvious that we must do this when not WAL-logging the copy. It's
     * less obvious that we have to do it even if we did WAL-log the copied
     * pages. The reason is that since we're copying outside shared buffers, a
     * CHECKPOINT occurring during the copy has no way to flush the previously
     * written data to disk (indeed it won't know the new rel even exists).  A
     * crash later on would replay WAL from the checkpoint, therefore it
     * wouldn't replay our earlier WAL entries. If we do not fsync those pages
     * here, they might still not be on disk when the crash occurs.
     */
    if (relpersistence == RELPERSISTENCE_PERMANENT || ((relpersistence == RELPERSISTENCE_TEMP) && STMT_RETRY_ENABLED))
        smgrimmedsync(dest->rd_smgr, forkNum);
}

static void mergeVMBlock(Relation src, Relation dest, BlockNumber srcHeapBlocks, BlockNumber destHeapBlocks)
{
    Buffer srcVMBuffer = InvalidBuffer;
    Buffer destVMBuffer = InvalidBuffer;
    BlockNumber srcBlockNum = 0;

    for (srcBlockNum = 0; srcBlockNum < srcHeapBlocks; srcBlockNum++) {
        bool VMValue = false;

        visibilitymap_pin(dest, destHeapBlocks + srcBlockNum, &destVMBuffer);
        VMValue = visibilitymap_test(src, srcBlockNum, &srcVMBuffer);

        if (VMValue) {
            visibilitymap_set(dest,
                destHeapBlocks + srcBlockNum,
                InvalidBuffer,
                InvalidXLogRecPtr,
                destVMBuffer,
                InvalidTransactionId,
                false);
        }
    }

    if (BufferIsValid(srcVMBuffer)) {
        ReleaseBuffer(srcVMBuffer);
    }
    if (BufferIsValid(destVMBuffer)) {
        ReleaseBuffer(destVMBuffer);
    }
}

/*
 * ALTER TABLE ENABLE/DISABLE TRIGGER
 *
 * We just pass this off to trigger.c.
 */
static void ATExecEnableDisableTrigger(
    Relation rel, const char* trigname, char fires_when, bool skip_system, LOCKMODE lockmode)
{
    EnableDisableTrigger(rel, trigname, fires_when, skip_system);
}

/*
 * ALTER TABLE ENABLE/DISABLE RULE
 *
 * We just pass this off to rewriteDefine.c.
 */
static void ATExecEnableDisableRule(Relation rel, const char* trigname, char fires_when, LOCKMODE lockmode)
{
    EnableDisableRule(rel, trigname, fires_when);
}

/*
 * ATExecEnableDisableRls
 *
 *     ALTER TABLE table_name ENABLE/DISABLE ROW LEVEL SECURITY.
 *     ALTER TABLE table_name FORCE/NO FORCE ROW LEVEL SECURITY.
 *
 * @param (in) rel: the relation information
 * @param (in) changeType: ENABLE, DISABLE, FORCE, NO FORCE R.L.S
 * @param (in) lockmode: Lock mode for relation open
 * @return: NULL
 */
void ATExecEnableDisableRls(Relation rel, RelationRlsStatus changeType, LOCKMODE lockmode)
{
    /* Check whether need to change on current node */
    if (SupportRlsOnCurrentNode() == false) {
        return;
    }
    /* Check license whether support this feature */
    LicenseSupportRls();

    /* Check whether support RLS for this relation */
    SupportRlsForRel(rel);

    List* roption = NULL;
    /* Enable or Disable Rls */
    switch (changeType) {
        case RELATION_RLS_ENABLE:
            /* update the column data when RLS is disabled */
            roption = list_make1(makeDefElem("enable_rowsecurity", (Node*)makeString("true")));
            ATExecSetRelOptions(rel, roption, AT_SetRelOptions, lockmode);
            break;
        case RELATION_RLS_DISABLE:
            /* update the column data when RLS is disabled */
            roption = list_make1(makeDefElem("enable_rowsecurity", NULL));
            ATExecSetRelOptions(rel, roption, AT_ResetRelOptions, lockmode);
            break;
        case RELATION_RLS_FORCE_ENABLE:
            /* update the column data when RLS is not forced */
            roption = list_make1(makeDefElem("force_rowsecurity", (Node*)makeString("true")));
            ATExecSetRelOptions(rel, roption, AT_SetRelOptions, lockmode);
            break;
        case RELATION_RLS_FORCE_DISABLE:
            /* only update the column data when RLS is forced */
            roption = list_make1(makeDefElem("force_rowsecurity", NULL));
            ATExecSetRelOptions(rel, roption, AT_ResetRelOptions, lockmode);
            break;
        default:
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("unknown type %u for ALTER TABLE ROW LEVEL SECURITY", changeType)));
    }
}

/*
 * ALTER TABLE INHERIT
 *
 * Add a parent to the child's parents. This verifies that all the columns and
 * check constraints of the parent appear in the child and that they have the
 * same data types and expressions.
 */
static void ATPrepAddInherit(Relation child_rel)
{
    if (child_rel->rd_rel->reloftype)
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("cannot change inheritance of typed table")));
}

static void ATExecAddInherit(Relation child_rel, RangeVar* parent, LOCKMODE lockmode)
{
    Relation parent_rel, catalogRelation;
    SysScanDesc scan;
    ScanKeyData key;
    HeapTuple inheritsTuple;
    int32 inhseqno;
    List* children = NIL;

    if (RELATION_IS_PARTITIONED(child_rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("relation \"%s\" is a partitioned table", RelationGetRelationName(child_rel)),
                errdetail("can not add inheritance for a partitioned table")));
    }

    /*
     * A self-exclusive lock is needed here.  See the similar case in
     * MergeAttributes() for a full explanation.
     */
    parent_rel = heap_openrv(parent, ShareUpdateExclusiveLock);

    if (RELATION_IS_PARTITIONED(parent_rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("inherited relation \"%s\" is a partitioned table", parent->relname),
                errdetail("can not inherit from partitioned table")));
    }

    /*
     * Must be owner of both parent and child -- child was checked by
     * ATSimplePermissions call in ATPrepCmd
     */
    ATSimplePermissions(parent_rel, ATT_TABLE);

    /* Permanent rels cannot inherit from temporary ones */
    if (parent_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
        child_rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("cannot inherit from temporary relation \"%s\"", RelationGetRelationName(parent_rel))));

    /* If parent rel is temp, it must belong to this session */
    if (parent_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP && !RelationIsLocalTemp(parent_rel))
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("cannot inherit from temporary relation of another session")));

    /* Ditto for the child */
    if (child_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP && !RelationIsLocalTemp(child_rel))
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("cannot inherit to temporary relation of another session")));

    /*
     * Check for duplicates in the list of parents, and determine the highest
     * inhseqno already present; we'll use the next one for the new parent.
     * (Note: get RowExclusiveLock because we will write pg_inherits below.)
     *
     * Note: we do not reject the case where the child already inherits from
     * the parent indirectly; CREATE TABLE doesn't reject comparable cases.
     */
    catalogRelation = heap_open(InheritsRelationId, RowExclusiveLock);
    ScanKeyInit(
        &key, Anum_pg_inherits_inhrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(child_rel)));
    scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId, true, SnapshotNow, 1, &key);

    /* inhseqno sequences start at 1 */
    inhseqno = 0;
    while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan))) {
        Form_pg_inherits inh = (Form_pg_inherits)GETSTRUCT(inheritsTuple);

        if (inh->inhparent == RelationGetRelid(parent_rel))
            ereport(ERROR,
                (errcode(ERRCODE_DUPLICATE_TABLE),
                    errmsg("relation \"%s\" would be inherited from more than once",
                        RelationGetRelationName(parent_rel))));
        if (inh->inhseqno > inhseqno)
            inhseqno = inh->inhseqno;
    }
    systable_endscan(scan);

    /*
     * Prevent circularity by seeing if proposed parent inherits from child.
     * (In particular, this disallows making a rel inherit from itself.)
     *
     * This is not completely bulletproof because of race conditions: in
     * multi-level inheritance trees, someone else could concurrently be
     * making another inheritance link that closes the loop but does not join
     * either of the rels we have locked.  Preventing that seems to require
     * exclusive locks on the entire inheritance tree, which is a cure worse
     * than the disease.  find_all_inheritors() will cope with circularity
     * anyway, so don't sweat it too much.
     *
     * We use weakest lock we can on child's children, namely AccessShareLock.
     */
    children = find_all_inheritors(RelationGetRelid(child_rel), AccessShareLock, NULL);

    if (list_member_oid(children, RelationGetRelid(parent_rel)))
        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_TABLE),
                errmsg("circular inheritance not allowed"),
                errdetail(
                    "\"%s\" is already a child of \"%s\".", parent->relname, RelationGetRelationName(child_rel))));

    /* If parent has OIDs then child must have OIDs */
    if (parent_rel->rd_rel->relhasoids && !child_rel->rd_rel->relhasoids)
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("table \"%s\" without OIDs cannot inherit from table \"%s\" with OIDs",
                    RelationGetRelationName(child_rel),
                    RelationGetRelationName(parent_rel))));

    /* Match up the columns and bump attinhcount as needed */
    MergeAttributesIntoExisting(child_rel, parent_rel);

    /* Match up the constraints and bump coninhcount as needed */
    MergeConstraintsIntoExisting(child_rel, parent_rel);

    /*
     * OK, it looks valid.	Make the catalog entries that show inheritance.
     */
    StoreCatalogInheritance1(RelationGetRelid(child_rel), RelationGetRelid(parent_rel), inhseqno + 1, catalogRelation);

    /* Now we're done with pg_inherits */
    heap_close(catalogRelation, RowExclusiveLock);

    /* keep our lock on the parent relation until commit */
    heap_close(parent_rel, NoLock);
}

/*
 * Obtain the source-text form of the constraint expression for a check
 * constraint, given its pg_constraint tuple
 */
static char* decompile_conbin(HeapTuple contup, TupleDesc tupdesc)
{
    Form_pg_constraint con;
    bool isnull = false;
    Datum attr;
    Datum expr;

    con = (Form_pg_constraint)GETSTRUCT(contup);
    attr = heap_getattr(contup, Anum_pg_constraint_conbin, tupdesc, &isnull);
    if (isnull)
        ereport(ERROR,
            (errcode(ERRCODE_UNEXPECTED_NULL_VALUE), errmsg("null conbin for constraint %u", HeapTupleGetOid(contup))));

    expr = DirectFunctionCall2(pg_get_expr, attr, ObjectIdGetDatum(con->conrelid));
    return TextDatumGetCString(expr);
}

/*
 * Determine whether two check constraints are functionally equivalent
 *
 * The test we apply is to see whether they reverse-compile to the same
 * source string.  This insulates us from issues like whether attributes
 * have the same physical column numbers in parent and child relations.
 */
static bool constraints_equivalent(HeapTuple a, HeapTuple b, TupleDesc tupleDesc)
{
    Form_pg_constraint acon = (Form_pg_constraint)GETSTRUCT(a);
    Form_pg_constraint bcon = (Form_pg_constraint)GETSTRUCT(b);

    if (acon->condeferrable != bcon->condeferrable || acon->condeferred != bcon->condeferred ||
        strcmp(decompile_conbin(a, tupleDesc), decompile_conbin(b, tupleDesc)) != 0)
        return false;
    else
        return true;
}

/*
 * Check columns in child table match up with columns in parent, and increment
 * their attinhcount.
 *
 * Called by ATExecAddInherit
 *
 * Currently all parent columns must be found in child. Missing columns are an
 * error.  One day we might consider creating new columns like CREATE TABLE
 * does.  However, that is widely unpopular --- in the common use case of
 * partitioned tables it's a foot-gun.
 *
 * The data type must match exactly. If the parent column is NOT NULL then
 * the child must be as well. Defaults are not compared, however.
 */
static void MergeAttributesIntoExisting(Relation child_rel, Relation parent_rel)
{
    Relation attrrel;
    AttrNumber parent_attno;
    int parent_natts;
    TupleDesc tupleDesc;
    HeapTuple tuple;

    attrrel = heap_open(AttributeRelationId, RowExclusiveLock);

    tupleDesc = RelationGetDescr(parent_rel);
    parent_natts = tupleDesc->natts;

    for (parent_attno = 1; parent_attno <= parent_natts; parent_attno++) {
        Form_pg_attribute attribute = tupleDesc->attrs[parent_attno - 1];
        char* attributeName = NameStr(attribute->attname);

        /* Ignore dropped columns in the parent. */
        if (attribute->attisdropped)
            continue;

        /* Find same column in child (matching on column name). */
        tuple = SearchSysCacheCopyAttName(RelationGetRelid(child_rel), attributeName);
        if (HeapTupleIsValid(tuple)) {
            /* Check they are same type, typmod, and collation */
            Form_pg_attribute childatt = (Form_pg_attribute)GETSTRUCT(tuple);

            if (attribute->atttypid != childatt->atttypid || attribute->atttypmod != childatt->atttypmod)
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                        errmsg("child table \"%s\" has different type for column \"%s\"",
                            RelationGetRelationName(child_rel),
                            attributeName)));

            if (attribute->attcollation != childatt->attcollation)
                ereport(ERROR,
                    (errcode(ERRCODE_COLLATION_MISMATCH),
                        errmsg("child table \"%s\" has different collation for column \"%s\"",
                            RelationGetRelationName(child_rel),
                            attributeName)));

            /*
             * Check child doesn't discard NOT NULL property.  (Other
             * constraints are checked elsewhere.)
             */
            if (attribute->attnotnull && !childatt->attnotnull)
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                        errmsg("column \"%s\" in child table must be marked NOT NULL", attributeName)));

            /*
             * OK, bump the child column's inheritance count.  (If we fail
             * later on, this change will just roll back.)
             */
            childatt->attinhcount++;
            simple_heap_update(attrrel, &tuple->t_self, tuple);
            CatalogUpdateIndexes(attrrel, tuple);
            heap_freetuple_ext(tuple);
        } else {
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("child table is missing column \"%s\"", attributeName)));
        }
    }

    heap_close(attrrel, RowExclusiveLock);
}

/*
 * Check constraints in child table match up with constraints in parent,
 * and increment their coninhcount.
 *
 * Constraints that are marked ONLY in the parent are ignored.
 *
 * Called by ATExecAddInherit
 *
 * Currently all constraints in parent must be present in the child. One day we
 * may consider adding new constraints like CREATE TABLE does.
 *
 * XXX This is O(N^2) which may be an issue with tables with hundreds of
 * constraints. As long as tables have more like 10 constraints it shouldn't be
 * a problem though. Even 100 constraints ought not be the end of the world.
 *
 * XXX See MergeWithExistingConstraint too if you change this code.
 */
static void MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel)
{
    Relation catalog_relation;
    TupleDesc tuple_desc;
    SysScanDesc parent_scan;
    ScanKeyData parent_key;
    HeapTuple parent_tuple;

    catalog_relation = heap_open(ConstraintRelationId, RowExclusiveLock);
    tuple_desc = RelationGetDescr(catalog_relation);

    /* Outer loop scans through the parent's constraint definitions */
    ScanKeyInit(&parent_key,
        Anum_pg_constraint_conrelid,
        BTEqualStrategyNumber,
        F_OIDEQ,
        ObjectIdGetDatum(RelationGetRelid(parent_rel)));
    parent_scan = systable_beginscan(catalog_relation, ConstraintRelidIndexId, true, SnapshotNow, 1, &parent_key);

    while (HeapTupleIsValid(parent_tuple = systable_getnext(parent_scan))) {
        Form_pg_constraint parent_con = (Form_pg_constraint)GETSTRUCT(parent_tuple);
        SysScanDesc child_scan;
        ScanKeyData child_key;
        HeapTuple child_tuple;
        bool found = false;

        if (parent_con->contype != CONSTRAINT_CHECK)
            continue;

        /* if the parent's constraint is marked NO INHERIT, it's not inherited */
        if (parent_con->connoinherit)
            continue;

        /* Search for a child constraint matching this one */
        ScanKeyInit(&child_key,
            Anum_pg_constraint_conrelid,
            BTEqualStrategyNumber,
            F_OIDEQ,
            ObjectIdGetDatum(RelationGetRelid(child_rel)));
        child_scan = systable_beginscan(catalog_relation, ConstraintRelidIndexId, true, SnapshotNow, 1, &child_key);

        while (HeapTupleIsValid(child_tuple = systable_getnext(child_scan))) {
            Form_pg_constraint child_con = (Form_pg_constraint)GETSTRUCT(child_tuple);
            HeapTuple child_copy;

            if (child_con->contype != CONSTRAINT_CHECK)
                continue;

            if (strcmp(NameStr(parent_con->conname), NameStr(child_con->conname)) != 0)
                continue;

            if (!constraints_equivalent(parent_tuple, child_tuple, tuple_desc))
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                        errmsg("child table \"%s\" has different definition for check constraint \"%s\"",
                            RelationGetRelationName(child_rel),
                            NameStr(parent_con->conname))));

            /* If the constraint is "no inherit" then cannot merge */
            if (child_con->connoinherit)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                        errmsg("constraint \"%s\" conflicts with non-inherited constraint on child table \"%s\"",
                            NameStr(child_con->conname),
                            RelationGetRelationName(child_rel))));

            /*
             * OK, bump the child constraint's inheritance count.  (If we fail
             * later on, this change will just roll back.)
             */
            child_copy = heap_copytuple(child_tuple);
            child_con = (Form_pg_constraint)GETSTRUCT(child_copy);
            child_con->coninhcount++;
            simple_heap_update(catalog_relation, &child_copy->t_self, child_copy);
            CatalogUpdateIndexes(catalog_relation, child_copy);
            heap_freetuple_ext(child_copy);

            found = true;
            break;
        }

        systable_endscan(child_scan);

        if (!found)
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("child table is missing constraint \"%s\"", NameStr(parent_con->conname))));
    }

    systable_endscan(parent_scan);
    heap_close(catalog_relation, RowExclusiveLock);
}

/*
 * ALTER TABLE NO INHERIT
 *
 * Drop a parent from the child's parents. This just adjusts the attinhcount
 * and attislocal of the columns and removes the pg_inherit and pg_depend
 * entries.
 *
 * If attinhcount goes to 0 then attislocal gets set to true. If it goes back
 * up attislocal stays true, which means if a child is ever removed from a
 * parent then its columns will never be automatically dropped which may
 * surprise. But at least we'll never surprise by dropping columns someone
 * isn't expecting to be dropped which would actually mean data loss.
 *
 * coninhcount and conislocal for inherited constraints are adjusted in
 * exactly the same way.
 */
static void ATExecDropInherit(Relation rel, RangeVar* parent, LOCKMODE lockmode)
{
    Relation parent_rel;
    Relation catalogRelation;
    SysScanDesc scan;
    ScanKeyData key[3];
    HeapTuple inheritsTuple, attributeTuple, constraintTuple;
    List* connames = NIL;
    bool found = false;

    /*
     * AccessShareLock on the parent is probably enough, seeing that DROP
     * TABLE doesn't lock parent tables at all.  We need some lock since we'll
     * be inspecting the parent's schema.
     */
    parent_rel = heap_openrv(parent, AccessShareLock);

    /*
     * We don't bother to check ownership of the parent table --- ownership of
     * the child is presumed enough rights.
     */
    /*
     * Find and destroy the pg_inherits entry linking the two, or error out if
     * there is none.
     */
    catalogRelation = heap_open(InheritsRelationId, RowExclusiveLock);
    ScanKeyInit(
        &key[0], Anum_pg_inherits_inhrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
    scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId, true, SnapshotNow, 1, key);

    while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan))) {
        Oid inhparent;

        inhparent = ((Form_pg_inherits)GETSTRUCT(inheritsTuple))->inhparent;
        if (inhparent == RelationGetRelid(parent_rel)) {
            simple_heap_delete(catalogRelation, &inheritsTuple->t_self);
            found = true;
            break;
        }
    }

    systable_endscan(scan);
    heap_close(catalogRelation, RowExclusiveLock);

    if (!found)
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_TABLE),
                errmsg("relation \"%s\" is not a parent of relation \"%s\"",
                    RelationGetRelationName(parent_rel),
                    RelationGetRelationName(rel))));

    /*
     * Search through child columns looking for ones matching parent rel
     */
    catalogRelation = heap_open(AttributeRelationId, RowExclusiveLock);
    ScanKeyInit(
        &key[0], Anum_pg_attribute_attrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
    scan = systable_beginscan(catalogRelation, AttributeRelidNumIndexId, true, SnapshotNow, 1, key);
    while (HeapTupleIsValid(attributeTuple = systable_getnext(scan))) {
        Form_pg_attribute att = (Form_pg_attribute)GETSTRUCT(attributeTuple);

        /* Ignore if dropped or not inherited */
        if (att->attisdropped)
            continue;
        if (att->attinhcount <= 0)
            continue;

        if (SearchSysCacheExistsAttName(RelationGetRelid(parent_rel), NameStr(att->attname))) {
            /* Decrement inhcount and possibly set islocal to true */
            HeapTuple copyTuple = heap_copytuple(attributeTuple);
            Form_pg_attribute copy_att = (Form_pg_attribute)GETSTRUCT(copyTuple);

            copy_att->attinhcount--;
            if (copy_att->attinhcount == 0)
                copy_att->attislocal = true;

            simple_heap_update(catalogRelation, &copyTuple->t_self, copyTuple);
            CatalogUpdateIndexes(catalogRelation, copyTuple);
            heap_freetuple_ext(copyTuple);
        }
    }
    systable_endscan(scan);
    heap_close(catalogRelation, RowExclusiveLock);

    /*
     * Likewise, find inherited check constraints and disinherit them. To do
     * this, we first need a list of the names of the parent's check
     * constraints.  (We cheat a bit by only checking for name matches,
     * assuming that the expressions will match.)
     */
    catalogRelation = heap_open(ConstraintRelationId, RowExclusiveLock);
    ScanKeyInit(&key[0],
        Anum_pg_constraint_conrelid,
        BTEqualStrategyNumber,
        F_OIDEQ,
        ObjectIdGetDatum(RelationGetRelid(parent_rel)));
    scan = systable_beginscan(catalogRelation, ConstraintRelidIndexId, true, SnapshotNow, 1, key);

    connames = NIL;

    while (HeapTupleIsValid(constraintTuple = systable_getnext(scan))) {
        Form_pg_constraint con = (Form_pg_constraint)GETSTRUCT(constraintTuple);

        if (con->contype == CONSTRAINT_CHECK)
            connames = lappend(connames, pstrdup(NameStr(con->conname)));
    }

    systable_endscan(scan);

    /* Now scan the child's constraints */
    ScanKeyInit(
        &key[0], Anum_pg_constraint_conrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
    scan = systable_beginscan(catalogRelation, ConstraintRelidIndexId, true, SnapshotNow, 1, key);

    while (HeapTupleIsValid(constraintTuple = systable_getnext(scan))) {
        Form_pg_constraint con = (Form_pg_constraint)GETSTRUCT(constraintTuple);

        if (con->contype != CONSTRAINT_CHECK)
            continue;

        bool match = false;
        ListCell* lc = NULL;

        foreach (lc, connames) {
            if (strcmp(NameStr(con->conname), (char*)lfirst(lc)) == 0) {
                match = true;
                break;
            }
        }

        if (match) {
            /* Decrement inhcount and possibly set islocal to true */
            HeapTuple copyTuple = heap_copytuple(constraintTuple);
            Form_pg_constraint copy_con = (Form_pg_constraint)GETSTRUCT(copyTuple);

            if (copy_con->coninhcount <= 0) /* shouldn't happen */
                ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("relation %u has non-inherited constraint \"%s\"",
                            RelationGetRelid(rel),
                            NameStr(copy_con->conname))));

            copy_con->coninhcount--;
            if (copy_con->coninhcount == 0)
                copy_con->conislocal = true;

            simple_heap_update(catalogRelation, &copyTuple->t_self, copyTuple);
            CatalogUpdateIndexes(catalogRelation, copyTuple);
            heap_freetuple_ext(copyTuple);
        }
    }

    systable_endscan(scan);
    heap_close(catalogRelation, RowExclusiveLock);

    drop_parent_dependency(RelationGetRelid(rel), RelationRelationId, RelationGetRelid(parent_rel));

    /* keep our lock on the parent relation until commit */
    heap_close(parent_rel, NoLock);
}

/*
 * Drop the dependency created by StoreCatalogInheritance1 (CREATE TABLE
 * INHERITS/ALTER TABLE INHERIT -- refclassid will be RelationRelationId) or
 * heap_create_with_catalog (CREATE TABLE OF/ALTER TABLE OF -- refclassid will
 * be TypeRelationId).	There's no convenient way to do this, so go trawling
 * through pg_depend.
 */
static void drop_parent_dependency(Oid relid, Oid refclassid, Oid refobjid)
{
    Relation catalogRelation;
    SysScanDesc scan;
    ScanKeyData key[3];
    HeapTuple depTuple;

    catalogRelation = heap_open(DependRelationId, RowExclusiveLock);

    ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
    ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
    ScanKeyInit(&key[2], Anum_pg_depend_objsubid, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));

    scan = systable_beginscan(catalogRelation, DependDependerIndexId, true, SnapshotNow, 3, key);

    while (HeapTupleIsValid(depTuple = systable_getnext(scan))) {
        Form_pg_depend dep = (Form_pg_depend)GETSTRUCT(depTuple);

        if (dep->refclassid == refclassid && dep->refobjid == refobjid && dep->refobjsubid == 0 &&
            dep->deptype == DEPENDENCY_NORMAL)
            simple_heap_delete(catalogRelation, &depTuple->t_self);
    }

    systable_endscan(scan);
    heap_close(catalogRelation, RowExclusiveLock);
}

/*
 * ALTER TABLE OF
 *
 * Attach a table to a composite type, as though it had been created with CREATE
 * TABLE OF.  All attname, atttypid, atttypmod and attcollation must match.  The
 * subject table must not have inheritance parents.  These restrictions ensure
 * that you cannot create a configuration impossible with CREATE TABLE OF alone.
 */
static void ATExecAddOf(Relation rel, const TypeName* ofTypename, LOCKMODE lockmode)
{
    Oid relid = RelationGetRelid(rel);
    Type typetuple;
    Oid typid;
    Relation inheritsRelation, relationRelation;
    SysScanDesc scan;
    ScanKeyData key;
    AttrNumber table_attno, type_attno;
    TupleDesc typeTupleDesc, tableTupleDesc;
    ObjectAddress tableobj, typeobj;
    HeapTuple classtuple;

    if (RELATION_IS_PARTITIONED(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("relation \"%s\" is a partitioned table", RelationGetRelationName(rel)),
                errdetail("can not add of_type for partitioned table")));
    }

    /* Validate the type. */
    typetuple = typenameType(NULL, ofTypename, NULL);
    check_of_type(typetuple);
    typid = HeapTupleGetOid(typetuple);

    /* Fail if the table has any inheritance parents. */
    inheritsRelation = heap_open(InheritsRelationId, AccessShareLock);
    ScanKeyInit(&key, Anum_pg_inherits_inhrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
    scan = systable_beginscan(inheritsRelation, InheritsRelidSeqnoIndexId, true, SnapshotNow, 1, &key);
    if (HeapTupleIsValid(systable_getnext(scan)))
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("typed tables cannot inherit")));
    systable_endscan(scan);
    heap_close(inheritsRelation, AccessShareLock);

    /*
     * Check the tuple descriptors for compatibility.  Unlike inheritance, we
     * require that the order also match.  However, attnotnull need not match.
     * Also unlike inheritance, we do not require matching relhasoids.
     */
    typeTupleDesc = lookup_rowtype_tupdesc(typid, -1);
    tableTupleDesc = RelationGetDescr(rel);
    table_attno = 1;
    for (type_attno = 1; type_attno <= typeTupleDesc->natts; type_attno++) {
        Form_pg_attribute type_attr, table_attr;
        const char* type_attname = NULL;
        const char* table_attname = NULL;

        /* Get the next non-dropped type attribute. */
        type_attr = typeTupleDesc->attrs[type_attno - 1];
        if (type_attr->attisdropped)
            continue;
        type_attname = NameStr(type_attr->attname);

        /* Get the next non-dropped table attribute. */
        do {
            if (table_attno > tableTupleDesc->natts)
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("table is missing column \"%s\"", type_attname)));
            table_attr = tableTupleDesc->attrs[table_attno++ - 1];
        } while (table_attr->attisdropped);
        table_attname = NameStr(table_attr->attname);

        /* Compare name. */
        if (strncmp(table_attname, type_attname, NAMEDATALEN) != 0)
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("table has column \"%s\" where type requires \"%s\"", table_attname, type_attname)));

        /* Compare type. */
        if (table_attr->atttypid != type_attr->atttypid || table_attr->atttypmod != type_attr->atttypmod ||
            table_attr->attcollation != type_attr->attcollation)
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("table \"%s\" has different type for column \"%s\"",
                        RelationGetRelationName(rel),
                        type_attname)));
    }
    DecrTupleDescRefCount(typeTupleDesc);

    /* Any remaining columns at the end of the table had better be dropped. */
    for (; table_attno <= tableTupleDesc->natts; table_attno++) {
        Form_pg_attribute table_attr = tableTupleDesc->attrs[table_attno - 1];

        if (!table_attr->attisdropped)
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("table has extra column \"%s\"", NameStr(table_attr->attname))));
    }

    /* If the table was already typed, drop the existing dependency. */
    if (rel->rd_rel->reloftype)
        drop_parent_dependency(relid, TypeRelationId, rel->rd_rel->reloftype);

    /* Record a dependency on the new type. */
    tableobj.classId = RelationRelationId;
    tableobj.objectId = relid;
    tableobj.objectSubId = 0;
    typeobj.classId = TypeRelationId;
    typeobj.objectId = typid;
    typeobj.objectSubId = 0;
    recordDependencyOn(&tableobj, &typeobj, DEPENDENCY_NORMAL);

    /* Update pg_class.reloftype */
    relationRelation = heap_open(RelationRelationId, RowExclusiveLock);
    classtuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(classtuple)) {
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", relid)));
    }
    ((Form_pg_class)GETSTRUCT(classtuple))->reloftype = typid;
    simple_heap_update(relationRelation, &classtuple->t_self, classtuple);
    CatalogUpdateIndexes(relationRelation, classtuple);
    heap_freetuple_ext(classtuple);
    heap_close(relationRelation, RowExclusiveLock);

    ReleaseSysCache(typetuple);
}

/*
 * ALTER TABLE NOT OF
 *
 * Detach a typed table from its originating type.	Just clear reloftype and
 * remove the dependency.
 */
static void ATExecDropOf(Relation rel, LOCKMODE lockmode)
{
    Oid relid = RelationGetRelid(rel);
    Relation relationRelation;
    HeapTuple tuple;

    if (!OidIsValid(rel->rd_rel->reloftype))
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a typed table", RelationGetRelationName(rel))));

    if (RELATION_IS_PARTITIONED(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("relation \"%s\" is a partitioned table", RelationGetRelationName(rel)),
                errdetail("drop of_type for partitioned table, this is a could not happening event")));
    }

    /*
     * We don't bother to check ownership of the type --- ownership of the
     * table is presumed enough rights.  No lock required on the type, either.
     */
    drop_parent_dependency(relid, TypeRelationId, rel->rd_rel->reloftype);

    /* Clear pg_class.reloftype */
    relationRelation = heap_open(RelationRelationId, RowExclusiveLock);
    tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(tuple)) {
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", relid)));
    }

    ((Form_pg_class)GETSTRUCT(tuple))->reloftype = InvalidOid;
    simple_heap_update(relationRelation, &tuple->t_self, tuple);
    CatalogUpdateIndexes(relationRelation, tuple);
    heap_freetuple_ext(tuple);
    heap_close(relationRelation, RowExclusiveLock);
}
/*
 * relation_mark_replica_identity: Update a table's replica identity
 *
 * Iff ri_type = REPLICA_IDENTITY_INDEX, indexOid must be the Oid of a suitable
 * index. Otherwise, it should be InvalidOid.
 */
static void relation_mark_replica_identity(Relation rel, char ri_type, Oid indexOid, bool is_internal)
{
    Relation pg_index;
    Relation pg_class;
    HeapTuple pg_class_tuple;
    HeapTuple pg_index_tuple;
    Form_pg_index pg_index_form;
    ListCell* index = NULL;
    bool isNull = false;
    Datum replident;
    Datum indisreplident;
    char relreplident = '\0';
    bool isreplident = false;

    /*
     * Check whether relreplident has changed, and update it if so.
     */
    pg_class = heap_open(RelationRelationId, RowExclusiveLock);
    pg_class_tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(RelationGetRelid(rel)));
    if (!HeapTupleIsValid(pg_class_tuple))
        ereport(ERROR,
            ((errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for relation \"%s\"", RelationGetRelationName(rel)))));
    replident = heap_getattr(pg_class_tuple, Anum_pg_class_relreplident, RelationGetDescr(pg_class), &isNull);
    if (!isNull)
        relreplident = CharGetDatum(replident);

    if (relreplident == '\0' || relreplident != ri_type) {
        HeapTuple newctup = NULL;
        Datum values[Natts_pg_class];
        bool nulls[Natts_pg_class];
        bool replaces[Natts_pg_class];
        errno_t rc;
        rc = memset_s(values, sizeof(values), 0, sizeof(values));
        securec_check(rc, "\0", "\0");
        rc = memset_s(nulls, sizeof(nulls), false, sizeof(nulls));
        securec_check(rc, "\0", "\0");
        rc = memset_s(replaces, sizeof(replaces), false, sizeof(replaces));
        securec_check(rc, "\0", "\0");

        replaces[Anum_pg_class_relreplident - 1] = true;
        values[Anum_pg_class_relreplident - 1] = ri_type;

        newctup = heap_modify_tuple(pg_class_tuple, RelationGetDescr(pg_class), values, nulls, replaces);

        simple_heap_update(pg_class, &pg_class_tuple->t_self, newctup);
        CatalogUpdateIndexes(pg_class, newctup);
        heap_freetuple_ext(newctup);
    }
    heap_close(pg_class, RowExclusiveLock);
    heap_freetuple_ext(pg_class_tuple);

    /*
     * Check whether the correct index is marked indisreplident; if so, we're
     * done.
     */
    if (OidIsValid(indexOid)) {
        Assert(ri_type == REPLICA_IDENTITY_INDEX);

        Relation reltmp = heap_open(IndexRelationId, AccessShareLock);
        pg_index_tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexOid));
        if (!HeapTupleIsValid(pg_index_tuple)) {
            ereport(
                ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for index %u", indexOid)));
        }
        pg_index_form = (Form_pg_index)GETSTRUCT(pg_index_tuple);
        isNull = false;
        isreplident = false;
        indisreplident = heap_getattr(pg_index_tuple, Anum_pg_index_indisreplident, RelationGetDescr(reltmp), &isNull);
        heap_close(reltmp, AccessShareLock);

        if (!isNull)
            isreplident = BoolGetDatum(indisreplident);

        if (isreplident) {
            ReleaseSysCache(pg_index_tuple);
            return;
        }
        ReleaseSysCache(pg_index_tuple);
    }

    /*
     * Clear the indisreplident flag from any index that had it previously, and
     * set it for any index that should have it now.
     */
    pg_index = heap_open(IndexRelationId, RowExclusiveLock);
    foreach (index, RelationGetIndexList(rel)) {
        Oid thisIndexOid = lfirst_oid(index);
        bool dirty = false;

        pg_index_tuple = SearchSysCacheCopy1(INDEXRELID, ObjectIdGetDatum(thisIndexOid));
        if (!HeapTupleIsValid(pg_index_tuple)) {
            ereport(ERROR,
                (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for index %u", thisIndexOid)));
        }
        pg_index_form = (Form_pg_index)GETSTRUCT(pg_index_tuple);
        isNull = false;
        isreplident = false;
        indisreplident =
            heap_getattr(pg_index_tuple, Anum_pg_index_indisreplident, RelationGetDescr(pg_index), &isNull);
        if (!isNull)
            isreplident = BoolGetDatum(indisreplident);
        /*
         * Unset the bit if set.  We know it's wrong because we checked this
         * earlier.
         */
        if (isreplident) {
            dirty = true;
            isreplident = false;
        } else if (thisIndexOid == indexOid) {
            dirty = true;
            isreplident = true;
        }

        if (dirty) {
            HeapTuple newitup = NULL;
            Datum values[Natts_pg_class];
            bool nulls[Natts_pg_class];
            bool replaces[Natts_pg_class];
            errno_t rc;
            rc = memset_s(values, sizeof(values), 0, sizeof(values));
            securec_check(rc, "\0", "\0");
            rc = memset_s(nulls, sizeof(nulls), false, sizeof(nulls));
            securec_check(rc, "\0", "\0");
            rc = memset_s(replaces, sizeof(replaces), false, sizeof(replaces));
            securec_check(rc, "\0", "\0");

            replaces[Anum_pg_index_indisreplident - 1] = true;
            values[Anum_pg_index_indisreplident - 1] = isreplident;
            newitup = heap_modify_tuple(pg_index_tuple, RelationGetDescr(pg_index), values, nulls, replaces);
            simple_heap_update(pg_index, &pg_index_tuple->t_self, newitup);
            CatalogUpdateIndexes(pg_index, newitup);
            heap_freetuple_ext(newitup);
        }
        heap_freetuple_ext(pg_index_tuple);
    }

    heap_close(pg_index, RowExclusiveLock);
}

/*
 * ALTER TABLE <name> REPLICA IDENTITY ...
 */
static void ATExecReplicaIdentity(Relation rel, ReplicaIdentityStmt* stmt, LOCKMODE lockmode)
{
    Oid indexOid;
    Relation indexRel;
    int key;

    if (stmt->identity_type == REPLICA_IDENTITY_DEFAULT) {
        relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
        return;
    } else if (stmt->identity_type == REPLICA_IDENTITY_FULL) {
        relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
        return;
    } else if (stmt->identity_type == REPLICA_IDENTITY_NOTHING) {
        relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
        return;
    } else if (stmt->identity_type == REPLICA_IDENTITY_INDEX) {
        /* fallthrough */;
    } else
        ereport(ERROR,
            ((errcode(ERRCODE_UNEXPECTED_NODE_STATE),
                errmsg("unexpected identity type %u", (uint)stmt->identity_type))));

    /* Check that the index exists */
    indexOid = get_relname_relid(stmt->name, rel->rd_rel->relnamespace);
    if (!OidIsValid(indexOid))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("index \"%s\" for table \"%s\" does not exist", stmt->name, RelationGetRelationName(rel))));

    indexRel = index_open(indexOid, ShareLock);

    /* Check that the index is on the relation we're altering. */
    if (indexRel->rd_index == NULL || indexRel->rd_index->indrelid != RelationGetRelid(rel))
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("\"%s\" is not an index for table \"%s\"",
                    RelationGetRelationName(indexRel),
                    RelationGetRelationName(rel))));
    /* The AM must support uniqueness, and the index must in fact be unique. */
    if (!indexRel->rd_am->amcanunique || !indexRel->rd_index->indisunique)
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("cannot use non-unique index \"%s\" as replica identity", RelationGetRelationName(indexRel))));
    /* Deferred indexes are not guaranteed to be always unique. */
    if (!indexRel->rd_index->indimmediate)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg(
                    "cannot use non-immediate index \"%s\" as replica identity", RelationGetRelationName(indexRel))));
    /* Expression indexes aren't supported. */
    if (RelationGetIndexExpressions(indexRel) != NIL)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("cannot use expression index \"%s\" as replica identity", RelationGetRelationName(indexRel))));
    /* Predicate indexes aren't supported. */
    if (RelationGetIndexPredicate(indexRel) != NIL)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("cannot use partial index \"%s\" as replica identity", RelationGetRelationName(indexRel))));
    /* And neither are invalid indexes. */
    if (!IndexIsValid(indexRel->rd_index))
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("cannot use invalid index \"%s\" as replica identity", RelationGetRelationName(indexRel))));

    /* Check index for nullable columns. */
    for (key = 0; key < indexRel->rd_index->indnatts; key++) {
        int16 attno = indexRel->rd_index->indkey.values[key];
        Form_pg_attribute attr;

        /* Of the system columns, only oid is indexable. */
        if (attno <= 0 && attno != ObjectIdAttributeNumber)
            ereport(ERROR,
                ((errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                    errmsg("internal column %d in unique index \"%s\"", attno, RelationGetRelationName(indexRel)))));

        attr = rel->rd_att->attrs[attno - 1];
        if (!attr->attnotnull)
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("index \"%s\" cannot be used as replica identity because column \"%s\" is nullable",
                        RelationGetRelationName(indexRel),
                        NameStr(attr->attname))));
    }

    /* This index is suitable for use as a replica identity. Mark it. */
    relation_mark_replica_identity(rel, stmt->identity_type, indexOid, true);

    index_close(indexRel, NoLock);
}

/*
 * ALTER FOREIGN TABLE <name> OPTIONS (...)
 */
static void ATExecGenericOptions(Relation rel, List* options)
{
    Relation ftrel;
    ForeignServer* server = NULL;
    ForeignDataWrapper* fdw = NULL;
    HeapTuple tuple;
    bool isnull = false;
    Datum repl_val[Natts_pg_foreign_table];
    bool repl_null[Natts_pg_foreign_table];
    bool repl_repl[Natts_pg_foreign_table];
    Datum datum;
    Form_pg_foreign_table tableform;
    errno_t rc;
    DefElemAction actions;
    double num_rows = 0.0;
    char* total_rows = NULL;

    if (options == NIL)
        return;

    ftrel = heap_open(ForeignTableRelationId, RowExclusiveLock);

    tuple = SearchSysCacheCopy1(FOREIGNTABLEREL, rel->rd_id);
    if (!HeapTupleIsValid(tuple))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("foreign table \"%s\" does not exist", RelationGetRelationName(rel))));
    tableform = (Form_pg_foreign_table)GETSTRUCT(tuple);
    server = GetForeignServer(tableform->ftserver);
    fdw = GetForeignDataWrapper(server->fdwid);

    rc = memset_s(repl_val, sizeof(repl_val), 0, sizeof(repl_val));
    securec_check(rc, "\0", "\0");
    rc = memset_s(repl_null, sizeof(repl_null), false, sizeof(repl_null));
    securec_check(rc, "\0", "\0");
    rc = memset_s(repl_repl, sizeof(repl_repl), false, sizeof(repl_repl));
    securec_check(rc, "\0", "\0");

    /* Extract the current options */
    datum = SysCacheGetAttr(FOREIGNTABLEREL, tuple, Anum_pg_foreign_table_ftoptions, &isnull);
    if (isnull)
        datum = PointerGetDatum(NULL);

    /* add write_only to options */
    if (tableform->ftwriteonly) {
        List* resultOptions = untransformRelOptions(datum);
        Node* val = (Node*)makeString(pstrdup("true"));
        resultOptions = lappend(resultOptions, makeDefElem(pstrdup("write_only"), val));
        datum = optionListToArray(resultOptions);
    }

    options = regularizeObsLocationInfo(options);

    /*
     * we insert type information into option in order to distinguish server type
     * hdfs_fdw_validator function.
     */
    char* optValue = getServerOptionValue(server->serverid, "type");
    DefElem* defElem = NULL;
    if (NULL != optValue) {
        defElem = makeDefElem("type", (Node*)makeString(optValue));
        options = lappend(options, defElem);
    }

    /* Transform the options */
    datum = transformGenericOptions(ForeignTableRelationId, datum, options, fdw->fdwvalidator);
    // update obs foreign table options totalrows in pg_class
    //
    if (IS_PGXC_COORDINATOR) {
        /* As for DEFELEM_DROP status, we do not deal with toltal rows in pg_class. */
        actions = getFTAlterAction(options, "totalrows");
        if ((actions == DEFELEM_SET) || (actions == DEFELEM_ADD)) {
            total_rows = getFTOptionValue(options, "totalrows");
            num_rows = convertFTOptionValue(total_rows);
            updateTotalRows(rel->rd_id, num_rows);
        }
    }

    List* resultOptions = untransformRelOptions(datum);

    if (NULL != optValue) {
        options = list_delete(options, defElem);
        resultOptions = list_delete(resultOptions, defElem);
        pfree_ext(defElem);
    }

    /* remove write_only from datum */
    if (tableform->ftwriteonly) {
        bool found = false;
        resultOptions = FindOrRemoveForeignTableOption(resultOptions, "write_only", true, &found);
        Assert(found);
    }

    datum = optionListToArray(resultOptions);

    if (PointerIsValid(DatumGetPointer(datum)))
        repl_val[Anum_pg_foreign_table_ftoptions - 1] = datum;
    else
        repl_null[Anum_pg_foreign_table_ftoptions - 1] = true;

    repl_repl[Anum_pg_foreign_table_ftoptions - 1] = true;

    /* Everything looks good - update the tuple */
    tuple = heap_modify_tuple(tuple, RelationGetDescr(ftrel), repl_val, repl_null, repl_repl);

    simple_heap_update(ftrel, &tuple->t_self, tuple);
    CatalogUpdateIndexes(ftrel, tuple);

    heap_close(ftrel, RowExclusiveLock);

    heap_freetuple_ext(tuple);
}

#ifdef PGXC
/*
 * ALTER TABLE <name> DISTRIBUTE BY ...
 */
static void AtExecDistributeBy(Relation rel, DistributeBy* options)
{
    Oid relid;
    char locatortype;
    int hashalgorithm = 0;
    int hashbuckets = 0;
    AttrNumber* attnum = NULL;
    int distributeKeyNum = 0;

    /* Nothing to do on Datanodes */
    if (IS_PGXC_DATANODE || options == NULL)
        return;

    relid = RelationGetRelid(rel);

    if (options->colname) {
        distributeKeyNum = list_length(options->colname);
        attnum = (int2*)palloc(distributeKeyNum * sizeof(AttrNumber));
    } else {
        distributeKeyNum = 1;
        attnum = (int2*)palloc(1 * sizeof(AttrNumber));
    }

    /* Get necessary distribution information */
    GetRelationDistributionItems(
        relid, options, RelationGetDescr(rel), &locatortype, &hashalgorithm, &hashbuckets, attnum);

    /*
     * It is not checked if the distribution type list is the same as the old one,
     * user might define a different sub-cluster at the same time.
     */
    /* Update pgxc_class entry */
    PgxcClassAlter(relid,
        locatortype,
        attnum,
        distributeKeyNum,
        hashalgorithm,
        hashbuckets,
        0,
        NULL,
        '\0',
        PGXC_CLASS_ALTER_DISTRIBUTION,
        NULL);
    pfree_ext(attnum);

    /* Make the additional catalog changes visible */
    CommandCounterIncrement();
}

/*
 * ALTER TABLE <name> TO [ NODE nodelist | GROUP groupname ]
 */
static void AtExecSubCluster(Relation rel, PGXCSubCluster* options)
{
    Oid* nodeoids = NULL;
    int numnodes;
    ListCell* lc = NULL;
    char* group_name = NULL;

    /* Nothing to do on Datanodes */
    if (IS_PGXC_DATANODE || options == NULL)
        return;

    /* Get node group name for ALTER TABLE ... TO Group */
    if (options->clustertype == SUBCLUSTER_GROUP) {
        Assert(list_length(options->members) == 1);

        foreach (lc, options->members) {
            group_name = strVal(lfirst(lc));
        }
    }

    /*
     * It is not checked if the new subcluster list is the same as the old one,
     * user might define a different distribution type.
     */
    /* Obtain new node information */
    nodeoids = GetRelationDistributionNodes(options, &numnodes);

    /* Update pgxc_class entry */
    PgxcClassAlter(
        RelationGetRelid(rel), '\0', NULL, 0, 0, 0, numnodes, nodeoids, 'y', PGXC_CLASS_ALTER_NODES, group_name);

    /* Make the additional catalog changes visible */
    CommandCounterIncrement();
}

/*
 * ALTER TABLE <name> ADD NODE nodelist
 */
static void AtExecAddNode(Relation rel, List* options)
{
    Oid* add_oids = NULL;
    Oid* old_oids = NULL;
    int add_num, old_num;

    /* Nothing to do on Datanodes */
    if (IS_PGXC_DATANODE || options == NIL)
        return;

    /*
     * Build a new array of sorted node Oids given the list of name nodes
     * to be added.
     */
    add_oids = BuildRelationDistributionNodes(options, &add_num);

    /*
     * Then check if nodes to be added are not in existing node
     * list and build updated list of nodes.
     */
    old_num = get_pgxc_classnodes(RelationGetRelid(rel), &old_oids);

    /* Add elements to array */
    old_oids = add_node_list(old_oids, old_num, add_oids, add_num, &old_num);

    /* Sort once again the newly-created array of node Oids to maintain consistency */
    old_oids = SortRelationDistributionNodes(old_oids, old_num);

    /* Update pgxc_class entry */
    PgxcClassAlter(RelationGetRelid(rel), '\0', NULL, 0, 0, 0, old_num, old_oids, 'y', PGXC_CLASS_ALTER_NODES, NULL);

    /* Make the additional catalog changes visible */
    CommandCounterIncrement();
}

/*
 * ALTER TABLE <name> DELETE NODE nodelist
 */
static void AtExecDeleteNode(Relation rel, List* options)
{
    Oid* del_oids = NULL;
    Oid* old_oids = NULL;
    int del_num;
    int old_num;

    /* Nothing to do on Datanodes */
    if (IS_PGXC_DATANODE || options == NIL)
        return;

    /*
     * Build a new array of sorted node Oids given the list of name nodes
     * to be deleted.
     */
    del_oids = BuildRelationDistributionNodes(options, &del_num);

    /*
     * Check if nodes to be deleted are really included in existing
     * node list and get updated list of nodes.
     */
    old_num = get_pgxc_classnodes(RelationGetRelid(rel), &old_oids);

    /* Delete elements on array */
    old_oids = delete_node_list(old_oids, old_num, del_oids, del_num, &old_num);

    /* Update pgxc_class entry */
    PgxcClassAlter(RelationGetRelid(rel), '\0', NULL, 0, 0, 0, old_num, old_oids, '\0', PGXC_CLASS_ALTER_NODES, NULL);

    /* Make the additional catalog changes visible */
    CommandCounterIncrement();
}

/*
 * ATCheckCmd
 *
 * Check ALTER TABLE restrictions in Postgres-XC
 */
static void ATCheckCmd(Relation rel, AlterTableCmd* cmd)
{
    /* Do nothing in the case of a remote node */
    if (u_sess->attr.attr_sql.enable_parallel_ddl) {
        if (IS_PGXC_DATANODE)
            return;
    } else {
        if (IS_PGXC_DATANODE || IsConnFromCoord())
            return;
    }

    switch (cmd->subtype) {
        case AT_DropColumn: {
            AttrNumber attnum = get_attnum(RelationGetRelid(rel), cmd->name);

            /* Distribution column cannot be dropped */
            if (IsDistribColumn(RelationGetRelid(rel), attnum))
                ereport(
                    ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Distribution column cannot be dropped")));

            break;
        }
        case AT_DistributeBy:
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Distribution mode cannot be altered")));
            break;
        case AT_SubCluster:
            break;
        default:
            break;
    }
}

/*
 * BuildRedistribCommands
 * Evaluate new and old distribution and build the list of operations
 * necessary to perform table redistribution.
 */
static RedistribState* BuildRedistribCommands(Oid relid, List* subCmds)
{
    RedistribState* redistribState = makeRedistribState(relid);
    RelationLocInfo* oldLocInfo = NULL;
    RelationLocInfo* newLocInfo = NULL; /* Former locator info */
    Relation rel;
    Oid* new_oid_array = NULL; /* Modified list of Oids */
    int new_num, i;            /* Modified number of Oids */
    ListCell* item = NULL;

    /* Get necessary information about relation */
    rel = relation_open(redistribState->relid, NoLock);
    oldLocInfo = RelationGetLocInfo(rel);
    Assert(oldLocInfo);

    /*
     * Get a copy of the locator information that will be modified by
     * successive ALTER TABLE commands.
     */
    newLocInfo = CopyRelationLocInfo(oldLocInfo);
    /* The node list of this locator information will be rebuilt after command scan */
    list_free_ext(newLocInfo->nodeList);
    list_free_ext(newLocInfo->partAttrNum);
    newLocInfo->nodeList = NULL;
    newLocInfo->partAttrNum = NULL;

    /* Get the list to be modified */
    new_num = get_pgxc_classnodes(RelationGetRelid(rel), &new_oid_array);

    foreach (item, subCmds) {
        AlterTableCmd* cmd = (AlterTableCmd*)lfirst(item);
        DistributeBy* distributeby = NULL;
        AttrNumber* attnum = NULL;
        int distributeKeyNum;

        switch (cmd->subtype) {
            case AT_DistributeBy:
                /*
                 * Get necessary distribution information and update to new
                 * distribution type.
                 */
                distributeby = (DistributeBy*)cmd->def;
                if (distributeby != NULL && distributeby->colname != NIL) {
                    distributeKeyNum = list_length(distributeby->colname);
                    attnum = (AttrNumber*)palloc(distributeKeyNum * sizeof(AttrNumber));
                } else {
                    distributeKeyNum = 1;
                    attnum = (AttrNumber*)palloc(1 * sizeof(AttrNumber));
                }
                GetRelationDistributionItems(redistribState->relid,
                    (DistributeBy*)cmd->def,
                    RelationGetDescr(rel),
                    &(newLocInfo->locatorType),
                    NULL,
                    NULL,
                    attnum);
                for (i = 0; i < distributeKeyNum; i++)
                    newLocInfo->partAttrNum = lappend_int(newLocInfo->partAttrNum, attnum[i]);
                pfree_ext(attnum);
                break;
            case AT_SubCluster:
                /* Update new list of nodes */
                new_oid_array = GetRelationDistributionNodes((PGXCSubCluster*)cmd->def, &new_num);
                break;
            case AT_AddNodeList: {
                Oid* add_oids = NULL;
                int add_num;
                add_oids = BuildRelationDistributionNodes((List*)cmd->def, &add_num);
                /* Add elements to array */
                new_oid_array = add_node_list(new_oid_array, new_num, add_oids, add_num, &new_num);
            } break;
            case AT_DeleteNodeList: {
                Oid* del_oids = NULL;
                int del_num;
                del_oids = BuildRelationDistributionNodes((List*)cmd->def, &del_num);
                /* Delete elements from array */
                new_oid_array = delete_node_list(new_oid_array, new_num, del_oids, del_num, &new_num);
            } break;
            default:
                Assert(0); /* Should not happen */
        }
    }

    /* Build relation node list for new locator info */
    for (i = 0; i < new_num; i++)
        newLocInfo->nodeList =
            lappend_int(newLocInfo->nodeList, PGXCNodeGetNodeId(new_oid_array[i], PGXC_NODE_DATANODE));

    /* Build the command tree for table redistribution */
    PGXCRedistribCreateCommandList(redistribState, newLocInfo);

    /*
     * Using the new locator info already available, check if constraints on
     * relation are compatible with the new distribution.
     */
    foreach (item, RelationGetIndexList(rel)) {
        Oid indid = lfirst_oid(item);
        Relation indexRel = index_open(indid, AccessShareLock);
        List* indexColNums = NIL;
        int2vector* colIdsPtr = &indexRel->rd_index->indkey;

        /*
         * Prepare call to shippability check. Attributes set to 0 correspond
         * to index expressions and are evaluated internally, so they are not
         * appended in given list.
         */
        for (i = 0; i < colIdsPtr->dim1; i++) {
            if (colIdsPtr->values[i] > 0)
                indexColNums = lappend_int(indexColNums, colIdsPtr->values[i]);
        }

        if (!pgxc_check_index_shippability(newLocInfo,
            indexRel->rd_index->indisprimary,
            indexRel->rd_index->indisunique,
            indexRel->rd_index->indisexclusion,
            indexColNums,
            indexRel->rd_indexprs))
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("Cannot alter table to distribution incompatible "
                           "with existing constraints")));

        index_close(indexRel, AccessShareLock);
    }

    /* Clean up */
    FreeRelationLocInfo(newLocInfo);
    pfree_ext(new_oid_array);
    relation_close(rel, NoLock);

    return redistribState;
}

/*
 * Delete from given Oid array old_oids the given oid list del_oids
 * and build a new one.
 */
static Oid* delete_node_list(Oid* old_oids, int old_num, Oid* del_oids, int del_num, int* new_num)
{
    /* Allocate former array and data */
    Oid* new_oids = old_oids;
    int loc_new_num = old_num;
    int i;

    /*
     * Delete from existing node Oid array the elements to be removed.
     * An error is returned if an element to be deleted is not in existing array.
     * It is not necessary to sort once again the result array of node Oids
     * as here only a deletion of elements is done.
     */
    for (i = 0; i < del_num; i++) {
        Oid nodeoid = del_oids[i];
        int j, position;
        bool is_listed = false;
        NameData nodename = {{0}};
        position = 0;

        for (j = 0; j < loc_new_num; j++) {
            /* Check if element can be removed */
            if (PgxcNodeCheckDnMatric(nodeoid, new_oids[j])) {
                is_listed = true;
                position = j;
            }
        }

        /* Move all the elements from [j+1, n-1] to [j, n-2] */
        if (is_listed) {
            for (j = position + 1; j < loc_new_num; j++)
                new_oids[j - 1] = new_oids[j];

            loc_new_num--;

            /* Not possible to have an empty list */
            if (loc_new_num == 0)
                ereport(ERROR,
                    (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("Node list is empty: one node at least is mandatory")));

            new_oids = (Oid*)repalloc(new_oids, loc_new_num * sizeof(Oid));
        } else
            ereport(ERROR,
                (errcode(ERRCODE_DUPLICATE_OBJECT),
                    errmsg("PGXC Node %s: object not in relation node list", get_pgxc_nodename(nodeoid, &nodename))));
    }

    /* Save new number of nodes */
    *new_num = loc_new_num;
    return new_oids;
}

/*
 * Add to given Oid array old_oids the given oid list add_oids
 * and build a new one.
 */
static Oid* add_node_list(Oid* old_oids, int old_num, Oid* add_oids, int add_num, int* new_num)
{
    /* Allocate former array and data */
    Oid* new_oids = old_oids;
    int loc_new_num = old_num;
    int i;

    /*
     * Build new Oid list, both addition and old list are already sorted.
     * The idea here is to go through the list of nodes to be added and
     * add the elements one-by-one on the existing list.
     * An error is returned if an element to be added already exists
     * in relation node array.
     * Here we do O(n^2) scan to avoid a dependency with the way
     * oids are sorted by heap APIs. They are sorted once again once
     * the addition operation is completed.
     */
    for (i = 0; i < add_num; i++) {
        Oid nodeoid = add_oids[i];
        int j;
        NameData nodename = {{0}};

        /* Check if element is already a part of array */
        for (j = 0; j < loc_new_num; j++) {
            /* Item is already in node list */
            if (PgxcNodeCheckDnMatric(nodeoid, new_oids[j]))
                ereport(ERROR,
                    (errcode(ERRCODE_DUPLICATE_OBJECT),
                        errmsg("PGXC Node %s: object already in relation node list",
                            get_pgxc_nodename(nodeoid, &nodename))));
        }

        /* If we are here, element can be added safely in node array */
        loc_new_num++;
        new_oids = (Oid*)repalloc(new_oids, loc_new_num * sizeof(Oid));
        new_oids[loc_new_num - 1] = nodeoid;
    }

    /* Sort once again the newly-created array of node Oids to maintain consistency */
    new_oids = SortRelationDistributionNodes(new_oids, loc_new_num);

    /* Save new number of nodes */
    *new_num = loc_new_num;
    return new_oids;
}
#endif

/*
 * Execute ALTER TABLE SET SCHEMA
 */
void AlterTableNamespace(AlterObjectSchemaStmt* stmt)
{
    Relation rel;
    Oid relid;
    Oid oldNspOid;
    Oid nspOid;
    RangeVar* newrv = NULL;
    ObjectAddresses* objsMoved = NULL;

    relid = RangeVarGetRelidExtended(stmt->relation,
        AccessExclusiveLock,
        stmt->missing_ok,
        false,
        false,
        false,
        RangeVarCallbackForAlterRelation,
        (void*)stmt);

    if (!OidIsValid(relid)) {
        ereport(NOTICE, (errmsg("relation \"%s\" does not exist, skipping", stmt->relation->relname)));
        return;
    }

    rel = relation_open(relid, NoLock);

    oldNspOid = RelationGetNamespace(rel);

    /* If it's an owned sequence, disallow moving it by itself. */
    if (rel->rd_rel->relkind == RELKIND_SEQUENCE) {
        ereport(
            ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("ALTER SEQUENCE SET SCHEMA is not yet supported.")));

        Oid tableId;
        int32 colId;

        if (sequenceIsOwned(relid, &tableId, &colId))
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("cannot move an owned sequence into another schema"),
                    errdetail("Sequence \"%s\" is linked to table \"%s\".",
                        RelationGetRelationName(rel),
                        get_rel_name(tableId))));
    }

    /* Get and lock schema OID and check its permissions. */
    newrv = makeRangeVar(stmt->newschema, RelationGetRelationName(rel), -1);
    nspOid = RangeVarGetAndCheckCreationNamespace(newrv, NoLock, NULL);

    /* common checks on switching namespaces */
    CheckSetNamespace(oldNspOid, nspOid, RelationRelationId, relid);

    objsMoved = new_object_addresses();
    AlterTableNamespaceInternal(rel, oldNspOid, nspOid, objsMoved);
    free_object_addresses(objsMoved);

    /* close rel, but keep lock until commit */
    relation_close(rel, NoLock);
}

/*
 * The guts of relocating a table to another namespace: besides moving
 * the table itself, its dependent objects are relocated to the new schema.
 */
void AlterTableNamespaceInternal(Relation rel, Oid oldNspOid, Oid nspOid, ObjectAddresses* objsMoved)
{
    Relation classRel;

    Assert(objsMoved != NULL);

    /* OK, modify the pg_class row and pg_depend entry */
    classRel = heap_open(RelationRelationId, RowExclusiveLock);

    AlterRelationNamespaceInternal(classRel, RelationGetRelid(rel), oldNspOid, nspOid, true, objsMoved);

    /* Fix the table's row type too */
    AlterTypeNamespaceInternal(rel->rd_rel->reltype, nspOid, false, false, objsMoved);

    /* Fix other dependent stuff */
    if (rel->rd_rel->relkind == RELKIND_RELATION) {
        AlterIndexNamespaces(classRel, rel, oldNspOid, nspOid, objsMoved);
        AlterSeqNamespaces(classRel, rel, oldNspOid, nspOid, objsMoved, AccessExclusiveLock);
        AlterConstraintNamespaces(RelationGetRelid(rel), oldNspOid, nspOid, false, objsMoved);
    }

    heap_close(classRel, RowExclusiveLock);
}

/*
 * The guts of relocating a relation to another namespace: fix the pg_class
 * entry, and the pg_depend entry if any.  Caller must already have
 * opened and write-locked pg_class.
 */
void AlterRelationNamespaceInternal(
    Relation classRel, Oid relOid, Oid oldNspOid, Oid newNspOid, bool hasDependEntry, ObjectAddresses* objsMoved)
{
    HeapTuple classTup;
    Form_pg_class classForm;
    ObjectAddress thisobj;

    classTup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relOid));
    if (!HeapTupleIsValid(classTup)) {
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", relOid)));
    }
    classForm = (Form_pg_class)GETSTRUCT(classTup);

    Assert(classForm->relnamespace == oldNspOid);

    thisobj.classId = RelationRelationId;
    thisobj.objectId = relOid;
    thisobj.objectSubId = 0;

    /*
     * Do nothing when there's nothing to do.
     */
    if (!object_address_present(&thisobj, objsMoved)) {
        /* check for duplicate name (more friendly than unique-index failure) */
        if (get_relname_relid(NameStr(classForm->relname), newNspOid) != InvalidOid)
            ereport(ERROR,
                (errcode(ERRCODE_DUPLICATE_TABLE),
                    errmsg("relation \"%s\" already exists in schema \"%s\"",
                        NameStr(classForm->relname),
                        get_namespace_name(newNspOid, true))));

        /* classTup is a copy, so OK to scribble on */
        classForm->relnamespace = newNspOid;

        simple_heap_update(classRel, &classTup->t_self, classTup);
        CatalogUpdateIndexes(classRel, classTup);

        /* Update dependency on schema if caller said so */
        if (hasDependEntry &&
            changeDependencyFor(RelationRelationId, relOid, NamespaceRelationId, oldNspOid, newNspOid) != 1)
            ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                    errmsg("failed to change schema dependency for relation \"%s\"", NameStr(classForm->relname))));
        add_exact_object_address(&thisobj, objsMoved);

        /* Recode time of alter relation namespace. */
        PgObjectType objectType = GetPgObjectTypePgClass(classForm->relkind);
        if (objectType != OBJECT_TYPE_INVALID) {
            UpdatePgObjectMtime(relOid, objectType);
        }
    }

    heap_freetuple_ext(classTup);
}

/*
 * Move all indexes for the specified relation to another namespace.
 *
 * Note: we assume adequate permission checking was done by the caller,
 * and that the caller has a suitable lock on the owning relation.
 */
static void AlterIndexNamespaces(
    Relation classRel, Relation rel, Oid oldNspOid, Oid newNspOid, ObjectAddresses* objsMoved)
{
    List* indexList = NIL;
    ListCell* l = NULL;

    indexList = RelationGetIndexList(rel);

    foreach (l, indexList) {
        Oid indexOid = lfirst_oid(l);
        ObjectAddress thisobj;

        thisobj.classId = RelationRelationId;
        thisobj.objectId = indexOid;
        thisobj.objectSubId = 0;

        /*
         * Note: currently, the index will not have its own dependency on the
         * namespace, so we don't need to do changeDependencyFor(). There's no
         * row type in pg_type, either.
         *
         * XXX this objsMoved test may be pointless -- surely we have a single
         * dependency link from a relation to each index?
         */
        if (!object_address_present(&thisobj, objsMoved)) {
            AlterRelationNamespaceInternal(classRel, indexOid, oldNspOid, newNspOid, false, objsMoved);
            add_exact_object_address(&thisobj, objsMoved);
        }
    }

    list_free_ext(indexList);
}

/*
 * Move all SERIAL-column sequences of the specified relation to another
 * namespace.
 *
 * Note: we assume adequate permission checking was done by the caller,
 * and that the caller has a suitable lock on the owning relation.
 */
static void AlterSeqNamespaces(
    Relation classRel, Relation rel, Oid oldNspOid, Oid newNspOid, ObjectAddresses* objsMoved, LOCKMODE lockmode)
{
    Relation depRel;
    SysScanDesc scan;
    ScanKeyData key[2];
    HeapTuple tup;

    /*
     * SERIAL sequences are those having an auto dependency on one of the
     * table's columns (we don't care *which* column, exactly).
     */
    depRel = heap_open(DependRelationId, AccessShareLock);

    ScanKeyInit(
        &key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
    ScanKeyInit(
        &key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(rel)));
    /* we leave refobjsubid unspecified */
    scan = systable_beginscan(depRel, DependReferenceIndexId, true, SnapshotNow, 2, key);

    while (HeapTupleIsValid(tup = systable_getnext(scan))) {
        Form_pg_depend depForm = (Form_pg_depend)GETSTRUCT(tup);
        Relation seqRel;

        /* skip dependencies other than auto dependencies on columns */
        if (depForm->refobjsubid == 0 || depForm->classid != RelationRelationId || depForm->objsubid != 0 ||
            depForm->deptype != DEPENDENCY_AUTO)
            continue;

        /* Use relation_open just in case it's an index */
        seqRel = relation_open(depForm->objid, lockmode);

        /* skip non-sequence relations */
        if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE) {
            /* No need to keep the lock */
            relation_close(seqRel, lockmode);
            continue;
        } else {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("There's dependent sequence, but ALTER SEQUENCE SET SCHEMA is not yet supported.")));
        }
        /* Fix the pg_class and pg_depend entries */
        AlterRelationNamespaceInternal(classRel, depForm->objid, oldNspOid, newNspOid, true, objsMoved);

        /*
         * Sequences have entries in pg_type. We need to be careful to move
         * them to the new namespace, too.
         */
        (void)AlterTypeNamespaceInternal(RelationGetForm(seqRel)->reltype, newNspOid, false, false, objsMoved);

        /* Now we can close it.  Keep the lock till end of transaction. */
        relation_close(seqRel, NoLock);
    }

    systable_endscan(scan);

    relation_close(depRel, AccessShareLock);
}

/*
 * This code supports
 *	CREATE TEMP TABLE ... ON COMMIT { DROP | PRESERVE ROWS | DELETE ROWS }
 *
 * Because we only support this for TEMP tables, it's sufficient to remember
 * the state in a backend-local data structure.
 */
/*
 * Register a newly-created relation's ON COMMIT action.
 */
void register_on_commit_action(Oid relid, OnCommitAction action)
{
    OnCommitItem* oc = NULL;
    MemoryContext oldcxt;

    /*
     * We needn't bother registering the relation unless there is an ON COMMIT
     * action we need to take.
     */
    if (action == ONCOMMIT_NOOP || action == ONCOMMIT_PRESERVE_ROWS)
        return;

    oldcxt = MemoryContextSwitchTo(u_sess->cache_mem_cxt);

    oc = (OnCommitItem*)palloc(sizeof(OnCommitItem));
    oc->relid = relid;
    oc->oncommit = action;
    oc->creating_subid = GetCurrentSubTransactionId();
    oc->deleting_subid = InvalidSubTransactionId;

    u_sess->cmd_cxt.on_commits = lcons(oc, u_sess->cmd_cxt.on_commits);

    (void)MemoryContextSwitchTo(oldcxt);
}

/*
 * Unregister any ON COMMIT action when a relation is deleted.
 *
 * Actually, we only mark the OnCommitItem entry as to be deleted after commit.
 */
void remove_on_commit_action(Oid relid)
{
    ListCell* l = NULL;

    foreach (l, u_sess->cmd_cxt.on_commits) {
        OnCommitItem* oc = (OnCommitItem*)lfirst(l);

        if (oc->relid == relid) {
            oc->deleting_subid = GetCurrentSubTransactionId();
            break;
        }
    }
}

/*
 * Perform ON COMMIT actions.
 *
 * This is invoked just before actually committing, since it's possible
 * to encounter errors.
 */
void PreCommit_on_commit_actions(void)
{
    ListCell* l = NULL;
    List* oids_to_truncate = NIL;

    foreach (l, u_sess->cmd_cxt.on_commits) {
        OnCommitItem* oc = (OnCommitItem*)lfirst(l);

        /* Ignore entry if already dropped in this xact */
        if (oc->deleting_subid != InvalidSubTransactionId)
            continue;

        switch (oc->oncommit) {
            case ONCOMMIT_NOOP:
            case ONCOMMIT_PRESERVE_ROWS:
                /* Do nothing (there shouldn't be such entries, actually) */
                break;
            case ONCOMMIT_DELETE_ROWS:
                oids_to_truncate = lappend_oid(oids_to_truncate, oc->relid);
                break;
            case ONCOMMIT_DROP: {
                ObjectAddress object;

                object.classId = RelationRelationId;
                object.objectId = oc->relid;
                object.objectSubId = 0;

                /*
                 * Since this is an automatic drop, rather than one
                 * directly initiated by the user, we pass the
                 * PERFORM_DELETION_INTERNAL flag.
                 */
                performDeletion(&object, DROP_CASCADE, PERFORM_DELETION_INTERNAL);

                /*
                 * Note that table deletion will call
                 * remove_on_commit_action, so the entry should get marked
                 * as deleted.
                 */
                Assert(oc->deleting_subid != InvalidSubTransactionId);
                break;
            }
        }
    }
    if (oids_to_truncate != NIL) {
        heap_truncate(oids_to_truncate);
        CommandCounterIncrement(); /* XXX needed? */
    }
}

/*
 * Post-commit or post-abort cleanup for ON COMMIT management.
 *
 * All we do here is remove no-longer-needed OnCommitItem entries.
 *
 * During commit, remove entries that were deleted during this transaction;
 * during abort, remove those created during this transaction.
 */
void AtEOXact_on_commit_actions(bool isCommit)
{
    ListCell* cur_item = NULL;
    ListCell* prev_item = NULL;

    prev_item = NULL;
    cur_item = list_head(u_sess->cmd_cxt.on_commits);

    while (cur_item != NULL) {
        OnCommitItem* oc = (OnCommitItem*)lfirst(cur_item);

        if (isCommit ? oc->deleting_subid != InvalidSubTransactionId : oc->creating_subid != InvalidSubTransactionId) {
            /* cur_item must be removed */
            u_sess->cmd_cxt.on_commits = list_delete_cell(u_sess->cmd_cxt.on_commits, cur_item, prev_item);
            pfree_ext(oc);
            if (prev_item != NULL)
                cur_item = lnext(prev_item);
            else
                cur_item = list_head(u_sess->cmd_cxt.on_commits);
        } else {
            /* cur_item must be preserved */
            oc->creating_subid = InvalidSubTransactionId;
            oc->deleting_subid = InvalidSubTransactionId;
            prev_item = cur_item;
            cur_item = lnext(prev_item);
        }
    }
}

/*
 * Post-subcommit or post-subabort cleanup for ON COMMIT management.
 *
 * During subabort, we can immediately remove entries created during this
 * subtransaction.	During subcommit, just relabel entries marked during
 * this subtransaction as being the parent's responsibility.
 */
void AtEOSubXact_on_commit_actions(bool isCommit, SubTransactionId mySubid, SubTransactionId parentSubid)
{
    ListCell* cur_item = NULL;
    ListCell* prev_item = NULL;

    prev_item = NULL;
    cur_item = list_head(u_sess->cmd_cxt.on_commits);

    while (cur_item != NULL) {
        OnCommitItem* oc = (OnCommitItem*)lfirst(cur_item);

        if (!isCommit && oc->creating_subid == mySubid) {
            /* cur_item must be removed */
            u_sess->cmd_cxt.on_commits = list_delete_cell(u_sess->cmd_cxt.on_commits, cur_item, prev_item);
            pfree_ext(oc);
            if (prev_item != NULL)
                cur_item = lnext(prev_item);
            else
                cur_item = list_head(u_sess->cmd_cxt.on_commits);
        } else {
            /* cur_item must be preserved */
            if (oc->creating_subid == mySubid)
                oc->creating_subid = parentSubid;
            if (oc->deleting_subid == mySubid)
                oc->deleting_subid = isCommit ? parentSubid : InvalidSubTransactionId;
            prev_item = cur_item;
            cur_item = lnext(prev_item);
        }
    }
}

/*
 * This is intended as a callback for RangeVarGetRelidExtended().  It allows
 * the table to be locked only if (1) it's a plain table or TOAST table and
 * (2) the current user is the owner (or the superuser).  This meets the
 * permission-checking needs of both CLUSTER and REINDEX TABLE; we expose it
 * here so that it can be used by both.
 */
void RangeVarCallbackOwnsTable(const RangeVar* relation, Oid relId, Oid oldRelId, bool target_is_partition, void* arg)
{
    char relkind;

    /* Nothing to do if the relation was not found. */
    if (!OidIsValid(relId)) {
        return;
    }

    /*
     * If the relation does exist, check whether it's an index.  But note that
     * the relation might have been dropped between the time we did the name
     * lookup and now.	In that case, there's nothing to do.
     */
    relkind = get_rel_relkind(relId);
    if (!relkind) {
        return;
    }
    if (relkind != RELKIND_RELATION && relkind != RELKIND_TOASTVALUE) {
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a table", relation->relname)));
    }
    /* Check permissions */
    if (!pg_class_ownercheck(relId, GetUserId())) {
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, relation->relname);
    }
}

/*
 * Callback to RangeVarGetRelidExtended(), similar to
 * RangeVarCallbackOwnsTable() but without checks on the type of the relation.
 */
void RangeVarCallbackOwnsRelation(
    const RangeVar* relation, Oid relId, Oid oldRelId, bool target_is_partition, void* arg)
{
    HeapTuple tuple;

    /* Nothing to do if the relation was not found. */
    if (!OidIsValid(relId)) {
        return;
    }

    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relId));
    if (!HeapTupleIsValid(tuple)) {
        /* should not happen */
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", relId)));
    }

    if (!pg_class_ownercheck(relId, GetUserId())) {
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, relation->relname);
    }
    if (!g_instance.attr.attr_common.allowSystemTableMods && !u_sess->attr.attr_common.IsInplaceUpgrade &&
        IsSystemClass((Form_pg_class)GETSTRUCT(tuple))) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                errmsg("permission denied: \"%s\" is a system catalog", relation->relname)));
    }

    ReleaseSysCache(tuple);
}

/*
 * Common RangeVarGetRelid callback for rename, set schema, and alter table
 * processing.
 */
static void RangeVarCallbackForAlterRelation(
    const RangeVar* rv, Oid relid, Oid oldrelid, bool target_is_partition, void* arg)
{
    Node* stmt = (Node*)arg;
    ObjectType reltype;
    HeapTuple tuple;
    Form_pg_class classform;
    AclResult aclresult;
    char relkind;

    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(tuple)) {
        return; /* concurrently dropped */
    }
    classform = (Form_pg_class)GETSTRUCT(tuple);
    relkind = classform->relkind;

    /* Must own relation. But if we are truncating a partition, we will not check owner. */
    if (!target_is_partition && !pg_class_ownercheck(relid, GetUserId())) {
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, rv->relname);
    }

    /* No system table modifications unless explicitly allowed or during inplace upgrade. */
    if (!g_instance.attr.attr_common.allowSystemTableMods && !u_sess->attr.attr_common.IsInplaceUpgrade &&
        IsSystemClass(classform)) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                errmsg("permission denied: \"%s\" is a system catalog", rv->relname)));
    }

    switch (relid) {
        case DatabaseRelationId:
        case AuthIdRelationId:
        case AuthMemRelationId:
        case RelationRelationId:
        case AttributeRelationId:
        case ProcedureRelationId:
        case TypeRelationId:
        case UserStatusRelationId:
            /*
             * Schema change of these nailed-in system catalogs is very dangerous!!!
             * Later, we may need an exclusive GUC variable to enable such change.
             */
            if (!u_sess->attr.attr_common.IsInplaceUpgrade)
                ereport(ERROR,
                    (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                        errmsg("permission denied: system catalog \"%s\" can not be altered", rv->relname)));
            break;
        default:
            break;
    }
    /*
     * Extract the specified relation type from the statement parse tree.
     *
     * Also, for ALTER .. RENAME, check permissions: the user must (still)
     * have CREATE rights on the containing namespace.
     */
    if (IsA(stmt, RenameStmt)) {
        aclresult = pg_namespace_aclcheck(classform->relnamespace, GetUserId(), ACL_CREATE);
        if (aclresult != ACLCHECK_OK) {
            aclcheck_error(aclresult, ACL_KIND_NAMESPACE, get_namespace_name(classform->relnamespace, true));
        }
        reltype = ((RenameStmt*)stmt)->renameType;
    } else if (IsA(stmt, AlterObjectSchemaStmt)) {
        reltype = ((AlterObjectSchemaStmt*)stmt)->objectType;
    } else if (IsA(stmt, AlterTableStmt)) {
        reltype = ((AlterTableStmt*)stmt)->relkind;
    } else {
        reltype = OBJECT_TABLE; /* placate compiler */
        ereport(
            ERROR, (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE), errmsg("unrecognized node type: %d", (int)nodeTag(stmt))));
    }

    /*
     * For compatibility with prior releases, we allow ALTER TABLE to be used
     * with most other types of relations (but not composite types). We allow
     * similar flexibility for ALTER INDEX in the case of RENAME, but not
     * otherwise.  Otherwise, the user must select the correct form of the
     * command for the relation at issue.
     */
    if (reltype == OBJECT_SEQUENCE && relkind != RELKIND_SEQUENCE) {
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sequence", rv->relname)));
    }

    if (reltype == OBJECT_VIEW && relkind != RELKIND_VIEW) {
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a view", rv->relname)));
    }

    if (reltype == OBJECT_FOREIGN_TABLE && relkind != RELKIND_FOREIGN_TABLE) {
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a foreign table", rv->relname)));
    }

    if (reltype == OBJECT_TYPE && relkind != RELKIND_COMPOSITE_TYPE) {
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a composite type", rv->relname)));
    }

    if (reltype == OBJECT_INDEX && relkind != RELKIND_INDEX && !IsA(stmt, RenameStmt)) {
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not an index", rv->relname)));
    }
    /*
     * Don't allow ALTER TABLE on composite types. We want people to use ALTER
     * TYPE for that.
     */
    if (reltype != OBJECT_TYPE && relkind == RELKIND_COMPOSITE_TYPE) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("\"%s\" is a composite type", rv->relname),
                errhint("Use ALTER TYPE instead.")));
    }

    if (reltype != OBJECT_FOREIGN_TABLE && relkind == RELKIND_FOREIGN_TABLE) {
        if (isMOTFromTblOid(relid)) {
            ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("\"%s\" is a mot, which does not support alter table.", rv->relname))); 
        } else {
            ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("\"%s\" is a foreign table, which does not support column constraints.", rv->relname)));
        }
    }

    /*
     * Don't allow ALTER TABLE .. SET SCHEMA on relations that can't be moved
     * to a different schema, such as indexes and TOAST tables.
     */
    if (IsA(stmt, AlterObjectSchemaStmt) && relkind != RELKIND_RELATION && relkind != RELKIND_VIEW &&
        relkind != RELKIND_SEQUENCE && relkind != RELKIND_FOREIGN_TABLE) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("\"%s\" is not a table, view, sequence, or foreign table", rv->relname)));
    }

    ReleaseSysCache(tuple);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: check if the partition is in use
 * Description	:
 * Notes		:
 */
void checkPartNotInUse(Partition part, const char* stmt)
{
    const int expected_refcnt = 1;

    if (part->pd_refcnt != expected_refcnt) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_IN_USE),
                /* translator: first %s is a SQL command, eg ALTER TABLE */
                errmsg("cannot %s \"%s\" because it is in use", stmt, PartitionGetPartitionName(part))));
    }
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: get paritionkey's sequence in column definition
 * Description	:
 * Notes		: invoker to free the return list
 */
List* GetPartitionkeyPos(List* partitionkeys, List* schema)
{
    ListCell* partitionkey_cell = NULL;
    ListCell* schema_cell = NULL;
    ColumnRef* partitionkey_ref = NULL;
    ColumnDef* schema_def = NULL;
    int column_count = 0;
    bool* is_exist = NULL;
    char* partitonkey_name = NULL;
    List* pos = NULL;
    int len = -1;

    if (schema == NIL) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_OPERATION), errmsg("there is no column for a partitioned table!")));
    }
    len = schema->length;
    if (partitionkeys == NIL) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_OPERATION), errmsg("there is no partition key!")));
    }
    is_exist = (bool*)palloc(len * sizeof(bool));

    errno_t rc = EOK;
    rc = memset_s(is_exist, len * sizeof(bool), 0, len * sizeof(bool));
    securec_check(rc, "\0", "\0");

    foreach (partitionkey_cell, partitionkeys) {
        partitionkey_ref = (ColumnRef*)lfirst(partitionkey_cell);
        partitonkey_name = ((Value*)linitial(partitionkey_ref->fields))->val.str;

        foreach (schema_cell, schema) {
            schema_def = (ColumnDef*)lfirst(schema_cell);

            /* find the column that has the same name as the partitionkey */
            if (!strcmp(partitonkey_name, schema_def->colname)) {
                /* duplicate partitionkey name */
                if (is_exist != NULL && is_exist[column_count]) {
                    pfree_ext(is_exist);
                    ereport(ERROR,
                        (errcode(ERRCODE_DUPLICATE_COLUMN), errmsg("duplicate partition key: %s", partitonkey_name)));
                }

                /* recoed attribute info when the partitionkey is unique */
                if (is_exist != NULL)
                    is_exist[column_count] = true;
                break;
            }
            column_count++;
        }

        /* fail to find the partitionkey in column definition */
        if (column_count >= len) {
            pfree_ext(is_exist);
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                    errmsg("undefined column\"%s\" is used as a partitioning column", partitonkey_name)));
        }

        pos = lappend_int(pos, column_count);
        column_count = 0;
        partitonkey_name = NULL;
    }
    pfree_ext(is_exist);
    return pos;
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: check the partitionkey's datatype
 * Description	:
 * Notes		:
 */
static void CheckRangePartitionKeyType(Form_pg_attribute* attrs, List* pos)
{
    int location = 0;
    ListCell* cell = NULL;
    Oid typoid = InvalidOid;
    foreach (cell, pos) {
        bool result = false;
        location = lfirst_int(cell);
        typoid = attrs[location]->atttypid;
        /* check datatype for range partitionkey */
        result = CheckRangePartitionKeyType(typoid);

        if (!result) {
            list_free_ext(pos);
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("column %s cannot serve as a range partitioning column because of its datatype",
                        NameStr(attrs[location]->attname))));
        }
    }
}

static void CheckIntervalPartitionKeyType(Form_pg_attribute* attrs, List* pos)
{
    /* must be one partitionkey for interval partition, have checked before */
    Assert(pos->length == 1);

    ListCell* cell = list_head(pos);
    int location = lfirst_int(cell);
    Oid typoid = attrs[location]->atttypid;
    if (typoid != TIMESTAMPOID && typoid != TIMESTAMPTZOID) {
        list_free_ext(pos);
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
                errmsg("column %s cannot serve as a interval partitioning column because of its datatype",
                    NameStr(attrs[location]->attname))));
    }
}

/*
 * @@GaussDB@@
 * Target		: value-partition type check
 * Brief		: check the value partitionkey's datatype
 * Description	:
 * Notes		:
 */
void CheckValuePartitionKeyType(Form_pg_attribute* attrs, List* pos)
{
    int location = 0;
    ListCell* cell = NULL;
    Oid typoid = InvalidOid;

    foreach (cell, pos) {
        location = lfirst_int(cell);
        typoid = attrs[location]->atttypid;
        /*
         * Check datatype for partitionkey NOTE: currently we reuse distribution
         * key's restriction as value-based parition is equal-evaluated we can't
         * the same criteria with Range-Partition Key
         */
        /* We just error-out first partition-column with invalid datatype */
        if (!(typoid == INT8OID || typoid == INT1OID || typoid == INT2OID || typoid == INT4OID ||
                typoid == NUMERICOID || typoid == CHAROID || typoid == BPCHAROID || typoid == VARCHAROID ||
                typoid == NVARCHAR2OID || typoid == DATEOID || typoid == TIMEOID || typoid == TIMESTAMPOID ||
                typoid == TIMESTAMPTZOID || typoid == INTERVALOID || typoid == TIMETZOID ||
                typoid == SMALLDATETIMEOID || typoid == TEXTOID)) {
            list_free_ext(pos);
            ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("column \"%s\" cannot be served as a value-partitioning column because of its datatype [%s]",
                        NameStr(attrs[location]->attname),
                        format_type_with_typemod(attrs[location]->atttypid, attrs[location]->atttypmod))));
        }
    }
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: check datatype for range partitionkey
 * Description	:
 * Notes		:
 */
static bool CheckRangePartitionKeyType(Oid typoid)
{
    bool result = true;

    switch (typoid) {
        case INT2OID:
            result = true;
            break;

        case INT4OID:
            result = true;
            break;

        case INT8OID:
            result = true;
            break;

        case DATEOID:
            result = true;
            break;

        case TIMESTAMPOID:
            result = true;
            break;

        case TIMESTAMPTZOID:
            result = true;
            break;

        case NUMERICOID:
            result = true;
            break;

        case TEXTOID:
            result = true;
            break;

        case CHAROID:
            result = true;
            break;

        case BPCHAROID:
            result = true;
            break;

        case VARCHAROID:
            result = true;
            break;

        case NVARCHAR2OID:
            result = true;
            break;

        case FLOAT4OID:
            result = true;
            break;

        case FLOAT8OID:
            result = true;
            break;

        case NAMEOID:
            result = true;
            break;

        default:
            result = false;
            break;
    }

    return result;
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: check tablespace permission for partition
 * Description	:
 * Notes		: maybe the owner of the partition is not the current user
 */
static void CheckPartitionTablespace(const char* spcname, Oid owner)
{
    Oid spcoid = InvalidOid;

    if (spcname == NULL || !OidIsValid(owner))
        return;

    spcoid = get_tablespace_oid(spcname, false);

    /* Check permissions except when using database's default */
    if (OidIsValid(spcoid) && u_sess->proc_cxt.MyDatabaseTableSpace != spcoid) {
        AclResult aclresult;

        aclresult = pg_tablespace_aclcheck(spcoid, GetUserId(), ACL_CREATE);
        if (aclresult != ACLCHECK_OK)
            aclcheck_error(aclresult, ACL_KIND_TABLESPACE, get_tablespace_name(spcoid));
    }

    /* In all cases disallow placing user relations in pg_global */
    if (spcoid == GLOBALTABLESPACE_OID) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("only shared relations can be placed in pg_global tablespace")));
    }
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: transform the partition value as an arry
 * Description	:
 * Notes		: the invoker should free the arry
 */
Const* GetPartitionValue(List* pos, Form_pg_attribute* attrs, List* value, bool isinterval)
{
    Const* result = NULL;
    Const* cell = NULL;
    ListCell* pos_cell = NULL;
    ListCell* value_cell = NULL;
    int valuepos = 0;
    int count = 0;
    Const* target_expr = NULL;

    /* lack of partitionkey value */
    if (pos->length > value->length) {
        list_free_ext(pos);
        ereport(ERROR, (errcode(ERRCODE_INVALID_OPERATION), errmsg("partition bound list contains too few elements")));
    }
    if (pos->length < value->length) {
        list_free_ext(pos);
        ereport(ERROR, (errcode(ERRCODE_INVALID_OPERATION), errmsg("partition bound list contains too many elements")));
    }
    result = (Const*)palloc(pos->length * sizeof(Const));
    errno_t rc = memset_s(result, pos->length * sizeof(Const), 0, pos->length * sizeof(Const));
    securec_check(rc, "", "");
    forboth(pos_cell, pos, value_cell, value)
    {
        valuepos = lfirst_int(pos_cell);
        cell = (Const*)lfirst(value_cell);

        /* del with maxvalue  */
        if (cell->ismaxvalue) {
            result[count].xpr.type = T_Const;
            result[count].ismaxvalue = cell->ismaxvalue;
            count++;
            continue;
        }

        /* transform the const to target datatype */
        target_expr = (Const*)GetTargetValue(attrs[valuepos], cell, isinterval);
        if (target_expr == NULL) {
            pfree_ext(result);
            list_free_ext(pos);
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_OPERATION),
                    errmsg("partition key value must be const or const-evaluable expression")));
        }

        result[count] = *target_expr;
        result[count].constcollid = attrs[valuepos]->attcollation;
        count++;
    }
    Assert(count == pos->length);
    return result;
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
Node* GetTargetValue(Form_pg_attribute attrs, Const* src, bool isinterval)
{
    Oid target_oid = InvalidOid;
    int target_mod = -1;
    Node* expr = NULL;
    Node* target_expr = NULL;

    Assert(src);

    /* transform the const to target datatype */
    if (!ConfirmTypeInfo(&target_oid, &target_mod, src, attrs, isinterval)) {
        return NULL;
    }
    expr = (Node*)coerce_to_target_type(
        NULL, (Node*)src, exprType((Node*)src), target_oid, target_mod, COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST, -1);
    if (expr == NULL) {
        return NULL;
    }
    switch (nodeTag(expr)) {
        case T_Const:
            target_expr = expr;
            break;

        /* get return value for function expression */
        case T_FuncExpr: {
            FuncExpr* funcexpr = (FuncExpr*)expr;
            expr = (Node*)evaluate_expr(
                (Expr*)funcexpr, exprType((Node*)funcexpr), exprTypmod((Node*)funcexpr), funcexpr->funccollid);
            if (T_Const == nodeTag((Node*)expr)) {
                target_expr = expr;
            }
        } break;

        default:
            target_expr = NULL;
            break;
    }

    return target_expr;
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static bool ConfirmTypeInfo(Oid* target_oid, int* target_mod, Const* src, Form_pg_attribute attrs, bool isinterval)
{
    Assert(src && target_oid && target_mod);

    *target_oid = attrs->atttypid;
    *target_mod = attrs->atttypmod;

    if (isinterval) {
        Oid srcid = src->consttype;

        if ((*target_oid == DATEOID) || (*target_oid == TIMESTAMPOID) || (*target_oid == TIMESTAMPTZOID)) {
            if (srcid == INTERVALOID) {
                *target_oid = INTERVALOID;
                *target_mod = src->consttypmod;
            } else if (srcid == UNKNOWNOID) {
                *target_oid = INTERVALOID;
                *target_mod = -1;
            } else {
                return false;
            }
        }
    }

    return true;
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static void ComparePartitionValue(List* pos, Form_pg_attribute* attrs, PartitionState* partdef)
{
    Const* pre_value = NULL;
    Const* cur_value = NULL;
    Const** pre = NULL;
    Const** cur = NULL;
    ListCell* cell = NULL;
    List* value = NIL;
    List* partitionList = NIL;
    bool is_intreval = false;
    int result = 0;
    int counter = 0;
    errno_t rc = EOK;

    if (pos == NULL || attrs == NULL || partdef == NULL) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_OPERATION), errmsg("invalid range partiiton table definition")));
    }
    partitionList = partdef->partitionList;
    /* no partitionvalues */
    if (!pos->length) {
        list_free_ext(pos);
        ereport(ERROR, (errcode(ERRCODE_INVALID_OPERATION), errmsg("there is no partition key")));
    }
    pre = (Const**)palloc0(pos->length * sizeof(Const*));
    cur = (Const**)palloc0(pos->length * sizeof(Const*));

    foreach (cell, partitionList) {
        value = ((RangePartitionDefState*)lfirst(cell))->boundary;

        if (pre_value == NULL) {
            pre_value = GetPartitionValue(pos, attrs, value, is_intreval);
            for (counter = 0; counter < pos->length; counter++) {
                pre[counter] = pre_value + counter;
            }
        } else {
            cur_value = GetPartitionValue(pos, attrs, value, is_intreval);
            for (counter = 0; counter < pos->length; counter++) {
                cur[counter] = cur_value + counter;
            }
            result = partitonKeyCompare(cur, pre, pos->length);

            /* compare partition value */
            if (result <= 0) {
                pfree_ext(pre);
                pfree_ext(cur);
                pfree_ext(pre_value);
                pfree_ext(cur_value);
                list_free_ext(pos);
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_OPERATION),
                        errmsg("partition bound of partition \"%s\" is too low",
                            ((RangePartitionDefState*)lfirst(cell))->partitionName)));
            }
            rc = memcpy_s(pre, pos->length * sizeof(Const*), cur, pos->length * sizeof(Const*));
            securec_check(rc, "\0", "\0");
            pfree_ext(pre_value);
            pre_value = cur_value;
            cur_value = NULL;
        }
    }
    pfree_ext(pre);
    pfree_ext(cur);
    pfree_ext(pre_value);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static void ATPrepAddPartition(Relation rel)
{
    if (!RELATION_IS_PARTITIONED(rel)) {
        ereport(
            ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("can not add partition against NON-PARTITIONED table")));
    }

    if (rel->partMap->type == PART_TYPE_INTERVAL) {
        ereport(ERROR, (errcode(ERRCODE_OPERATE_NOT_SUPPORTED),
            errmsg("can not add partition against interval partitioned table")));
    }
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static void ATPrepDropPartition(Relation rel)
{
    if (!RELATION_IS_PARTITIONED(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("can not drop partition against NON-PARTITIONED table")));
    }
}

static void ATPrepUnusableIndexPartition(Relation rel)
{
    if (!RelationIsPartitioned(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("can not set unusable index partition against NON-PARTITIONED index")));
    }
}

static void ATPrepUnusableAllIndexOnPartition(Relation rel)
{
    if (!RELATION_IS_PARTITIONED(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("can not set all index unusable on one partition against NON-PARTITIONED table")));
    }
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static void ATPrepEnableRowMovement(Relation rel)
{
    if (!RELATION_IS_PARTITIONED(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("can not enable row movement against NON-PARTITIONED table")));
    }
}
/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static void ATPrepDisableRowMovement(Relation rel)
{
    if (!RELATION_IS_PARTITIONED(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("can not disable row movement against NON-PARTITIONED table")));
    }
    if (RelationIsColStore(rel)) {
        ereport(NOTICE,
            (errmsg("disable row movement is invalid for column stored tables. They always enable row movement between "
                    "partitions.")));
    }
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static void ATPrepTruncatePartition(Relation rel)
{
    AclResult aclresult;
    if (!RELATION_IS_PARTITIONED(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("can not truncate partition against NON-PARTITIONED table")));
    }

    aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(), ACL_TRUNCATE);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error(aclresult, ACL_KIND_CLASS, RelationGetRelationName(rel));
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: ALTER TABLE EXCHANGE PARTITION WITH TABLE
 * Description	: check change with table which is or not ordinary table
 * Notes		:
 */
static void ATPrepExchangePartition(Relation rel)
{
    if (!RELATION_IS_PARTITIONED(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("can not exchange partition against NON-PARTITIONED table")));
    }
}

static void ATPrepMergePartition(Relation rel)
{
    if (!RELATION_IS_PARTITIONED(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("can not merge partition against NON-PARTITIONED table")));
    }

    if (rel->partMap->type == PART_TYPE_INTERVAL) {
        ereport(ERROR, (errcode(ERRCODE_OPERATE_NOT_SUPPORTED),
            errmsg("can not merge partition against interval partitioned table")));
    }
}

static void ATPrepSplitPartition(Relation rel)
{
    if (!RELATION_IS_PARTITIONED(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("can not split partition against NON-PARTITIONED table")));
    }

    if (rel->partMap->type == PART_TYPE_INTERVAL) {
        ereport(ERROR, (errcode(ERRCODE_OPERATE_NOT_SUPPORTED),
            errmsg("can not split partition against interval partitioned table")));
    }
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static void ATExecAddPartition(Relation rel, AddPartitionState* partState)
{
    Relation pgPartRel = NULL;
    Oid existingPartOid = InvalidOid;
    Oid newPartOid = InvalidOid;
    List* partKeyValueList = NULL;
    Datum new_reloptions;
    Datum rel_reloptions;
    HeapTuple tuple;
    bool isnull = false;
    List* old_reloptions = NIL;
    ListCell* cell = NULL;
    Oid bucketOid;

    RangePartitionDefState* partDef = NULL;

    /* check tablespace privileges */
    foreach (cell, partState->partitionList) {
        partDef = (RangePartitionDefState*)lfirst(cell);
        if (PointerIsValid(partDef->tablespacename))
            CheckPartitionTablespace(partDef->tablespacename, rel->rd_rel->relowner);
    }
    /* check 2: can not add more partition, because more enough */
    if ((getNumberOfPartitions(rel) + partState->partitionList->length) > MAX_PARTITION_NUM) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("too many partitions for partitioned table"),
                errhint("Number of partitions can not be more than %d", MAX_PARTITION_NUM)));
    }

    /* check 3: name conflict check */
    foreach (cell, partState->partitionList) {
        partDef = (RangePartitionDefState*)lfirst(cell);
        existingPartOid = partitionNameGetPartitionOid(rel->rd_id,
            partDef->partitionName,
            PART_OBJ_TYPE_TABLE_PARTITION,
            AccessExclusiveLock,
            true,
            false,
            NULL,
            NULL,
            NoLock);
        if (OidIsValid(existingPartOid)) {
            ereport(ERROR,
                ((errcode(ERRCODE_DUPLICATE_OBJECT),
                    errmsg(
                        "adding partition name conflict with existing partitions: \"%s\".", partDef->partitionName))));
        }
    }

    /* check 4: new adding partitions behind the last partition */
    partDef = (RangePartitionDefState*)linitial(partState->partitionList);

    int partNum = getNumberOfPartitions(rel);
    Const* curBound = (Const*)copyObject(((RangePartitionMap*)rel->partMap)->rangeElements[partNum - 1].boundary[0]);
    Const* curStartVal = partDef->curStartVal;
    if (!curBound->ismaxvalue && curStartVal != NULL && partitonKeyCompare(&curStartVal, &curBound, 1) != 0) {
        if (partDef->partitionInitName != NULL) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                    errmsg("start value of partition \"%s\" NOT EQUAL up-boundary of last partition.",
                        partDef->partitionInitName)));
        } else {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                    errmsg("start value of partition \"%s\" NOT EQUAL up-boundary of last partition.",
                        partDef->partitionName)));
        }
    }

    partKeyValueList = transformConstIntoTargetType(
        rel->rd_att->attrs, ((RangePartitionMap*)rel->partMap)->partitionKey, partDef->boundary);
    existingPartOid = partitionValuesGetPartitionOid(rel, partKeyValueList, AccessExclusiveLock, false, true, false);
    if (OidIsValid(existingPartOid)) {
        list_free_deep(partKeyValueList);
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("upper boundary of adding partition MUST overtop last existing partition")));
    }

    /* check 5: whether has the unusable local index */
    if (!checkRelationLocalIndexesUsable(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                errmsg("can't add partition bacause the relation %s has unusable local index",
                    NameStr(rel->rd_rel->relname)),
                errhint("please reindex the unusable index first.")));
    }

    bool* isTimestamptz = check_partkey_has_timestampwithzone(rel);

    pgPartRel = relation_open(PartitionRelationId, RowExclusiveLock);

    /* step 2: add new partition entry in pg_partition */
    /* TRANSFORM into target first */
    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(rel->rd_id));
    rel_reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);

    old_reloptions = untransformRelOptions(rel_reloptions);
    RemoveRedisRelOptionsFromList(&old_reloptions);
    new_reloptions = transformRelOptions((Datum)0, old_reloptions, NULL, NULL, false, false);
    ReleaseSysCache(tuple);

    if (old_reloptions != NIL)
        list_free_ext(old_reloptions);

    bucketOid = RelationGetBucketOid(rel);
    foreach (cell, partState->partitionList) {
        partDef = (RangePartitionDefState*)lfirst(cell);
        newPartOid = heapAddRangePartition(pgPartRel,
            rel->rd_id,
            InvalidOid,
            rel->rd_rel->reltablespace,
            bucketOid,
            partDef,
            rel->rd_rel->relowner,
            (Datum)new_reloptions,
            isTimestamptz);

        /* step 3: no need to update number of partitions in pg_partition */
        /*
         * We must bump the command counter to make the newly-created partition
         * tuple visible for opening.
         */
        CommandCounterIncrement();

        if (RelationIsColStore(rel)) {
            addCudescTableForNewPartition(rel, newPartOid);
            addDeltaTableForNewPartition(rel, newPartOid);
        }

        addIndexForPartition(rel, newPartOid);

        addToastTableForNewPartition(rel, newPartOid);
        /* step 4: invalidate relation */
        CacheInvalidateRelcache(rel);
    }

    /* close relation, done */
    relation_close(pgPartRel, NoLock);
    list_free_deep(partKeyValueList);
    pfree_ext(isTimestamptz);
}

/* Assume the caller has already hold RowExclusiveLock on the pg_partition. */
static void UpdateIntervalPartToRange(Relation relPartition, Oid partOid, const char* stmt)
{
    bool dirty = false;
    /* Fetch a copy of the tuple to scribble on */
    HeapTuple parttup = SearchSysCacheCopy1(PARTRELID, ObjectIdGetDatum(partOid));
    if (!HeapTupleIsValid(parttup)) {
        ereport(ERROR,
            (errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
                errmsg("pg_partition entry for partid %u vanished during %s.", partOid, stmt)));
    }
    Form_pg_partition partform = (Form_pg_partition)GETSTRUCT(parttup);

    /* Apply required updates, if any, to copied tuple */
    if (partform->partstrategy == PART_STRATEGY_INTERVAL) {
        partform->partstrategy = PART_STRATEGY_RANGE;
        dirty = true;
    } else {
        ereport(LOG,
            (errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
                errmsg("pg_partition entry for partid %u is not a interval "
                       "partition when execute %s .",
                    partOid,
                    stmt)));
    }

    /* If anything changed, write out the tuple. */
    if (dirty) {
        heap_inplace_update(relPartition, parttup);
    }
}

/* assume caller already hold AccessExclusiveLock on the partition being dropped
 * if the intervalPartOid is not InvalidOid, the interval partition which is specificed by it 
 * need to be changed to normal range partition.
 */
void fastDropPartition(Relation rel, Oid partOid, const char* stmt, Oid intervalPartOid)
{
    Partition part = NULL;
    Relation pg_partition = NULL;
    pg_partition = relation_open(PartitionRelationId, RowExclusiveLock);

    /* step 2: drop the targeting partition entry in pg_partition */
    part = partitionOpenWithRetry(rel, partOid, AccessExclusiveLock, stmt);
    if (!part) {
        ereport(ERROR,
            (errcode(ERRCODE_LOCK_WAIT_TIMEOUT),
                errmsg("could not acquire AccessExclusiveLock on dest table partition \"%s\", %s failed",
                    getPartitionName(partOid, false),
                    stmt)));
    }

    /* drop toast table, index, and finally the partition iteselt */
    dropIndexForPartition(partOid);
    dropToastTableOnPartition(partOid);
    if (RelationIsColStore(rel)) {
        dropCuDescTableOnPartition(partOid);
        dropDeltaTableOnPartition(partOid);
    }
    heapDropPartition(rel, part);

    if (intervalPartOid) {
        UpdateIntervalPartToRange(pg_partition, intervalPartOid, stmt);
    }

    /* step 3: no need to update number of partitions in pg_partition */
    /* step 4: invalidate relation */
    CacheInvalidateRelcache(rel);

    /* make the dropping partition invisible, fresh partition map for the new partition */
    relation_close(pg_partition, RowExclusiveLock);

    /*
     * We must bump the command counter to make the newly-droped partition
     * tuple visible.
     */
    CommandCounterIncrement();
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static void ATExecDropPartition(Relation rel, AlterTableCmd* cmd)
{
    Oid partOid = InvalidOid;
    RangePartitionDefState* rangePartDef = NULL;

    /* getting the dropping partition's oid */
    /* FIRST IS the DROP PARTITION PART_NAME branch */
    if (PointerIsValid(cmd->name)) {
        partOid = partitionNameGetPartitionOid(rel->rd_id,
            cmd->name,
            PART_OBJ_TYPE_TABLE_PARTITION,
            AccessExclusiveLock,
            false,
            false,
            NULL,
            NULL,
            NoLock);
    } else {
        /* next IS the DROP PARTITION FOR (MAXVALUELIST) branch */
        rangePartDef = (RangePartitionDefState*)cmd->def;
        rangePartDef->boundary = transformConstIntoTargetType(rel->rd_att->attrs,
            ((RangePartitionMap*)rel->partMap)->partitionKey, rangePartDef->boundary);
        partOid = partitionValuesGetPartitionOid(rel,
            rangePartDef->boundary,
            AccessExclusiveLock,
            true,
            true, /* will check validity of partition oid next step */
            false);
    }

    /* check 1: check validity of partition oid */
    if (!OidIsValid(partOid)) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("The partition number is invalid or out-of-range")));
    }

    /* check 2: can not drop the last existing partition */
    if (getNumberOfPartitions(rel) == 1) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_OPERATION), errmsg("Cannot drop the only partition of a partitioned table")));
    }

    Oid changeToRangePartOid = GetNeedDegradToRangePartOid(rel, partOid);
    fastDropPartition(rel, partOid, "DROP PARTITION", changeToRangePartOid);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
void ATExecSetIndexUsableState(Oid objclassOid, Oid objOid, bool newState)
{
    bool dirty = false;
    Relation sys_table = NULL;
    int sysCacheId = 0;
    HeapTuple sys_tuple = NULL;

    if (objclassOid != IndexRelationId && objclassOid != PartitionRelationId)
        return;

    sys_table = relation_open(objclassOid, RowExclusiveLock);

    /* drop toast table, index, and finally the partition iteselt */
    /* get the corresponding tuple for partOid */
    if (objclassOid == PartitionRelationId)
        sysCacheId = PARTRELID;
    else if (objclassOid == IndexRelationId)
        sysCacheId = INDEXRELID;

    // update the indisusable field
    sys_tuple = SearchSysCacheCopy1(sysCacheId, ObjectIdGetDatum(objOid));
    if (sys_tuple) {
        if (objclassOid == PartitionRelationId) {
            if (((Form_pg_partition)GETSTRUCT(sys_tuple))->indisusable != newState) {
                ((Form_pg_partition)GETSTRUCT(sys_tuple))->indisusable = newState;
                dirty = true;
            }
        } else if (objclassOid == IndexRelationId) {
            if (((Form_pg_index)GETSTRUCT(sys_tuple))->indisusable != newState) {
                ((Form_pg_index)GETSTRUCT(sys_tuple))->indisusable = newState;
                dirty = true;
            }
        }

        /* Keep the system catalog indexes current. */
        if (dirty) {
            simple_heap_update(sys_table, &(sys_tuple->t_self), sys_tuple);
            CatalogUpdateIndexes(sys_table, sys_tuple);
        }
        heap_freetuple_ext(sys_tuple);
    }
    relation_close(sys_table, RowExclusiveLock);

    if (dirty) {
        CommandCounterIncrement();
    }
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static void ATExecUnusableIndexPartition(Relation rel, const char* partition_name)
{
    Oid indexPartOid = InvalidOid;
    Oid heapPartOid = InvalidOid;

    if (partition_name == NULL)
        return;

    if (OidIsValid(rel->rd_rel->relcudescrelid) || rel->rd_rel->relam == CBTREE_AM_OID ||
        rel->rd_rel->relam == CGIN_AM_OID) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Un-support feature"),
                errdetail("column-store index doesn't support this ALTER yet")));
    }

    /* the AccessShareLock lock on heap relation is held by AlterTableLookupRelation(). */
    /* getting the partition's oid, lock it the same time */
    indexPartOid = partitionNameGetPartitionOid(rel->rd_id,
        partition_name,
        PART_OBJ_TYPE_INDEX_PARTITION,
        AccessExclusiveLock,  // lock on index partition
        false,
        false,
        PartitionNameCallbackForIndexPartition,
        (void*)&heapPartOid,
        AccessExclusiveLock);  // lock on heap partition
    // call the internal function
    ATExecSetIndexUsableState(PartitionRelationId, indexPartOid, false);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static void ATExecUnusableAllIndexOnPartition(Relation rel, const char* partition_name)
{
    Oid partOid = InvalidOid;
    Relation pg_partition = NULL;
    List* partIndexlist = NIL;
    ListCell* lc = NULL;

    if (partition_name == NULL)
        return;

    if (RelationIsColStore(rel)) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Un-support feature"),
                errdetail("column-store relation doesn't support this ALTER yet")));
    }

    /* getting the partition's oid, lock it the same time */
    partOid = partitionNameGetPartitionOid(rel->rd_id,
        partition_name,
        PART_OBJ_TYPE_TABLE_PARTITION,
        AccessExclusiveLock,
        false,
        false,
        NULL,
        NULL,
        NoLock);

    /* first get the list of index partition on targeting table partition */
    partIndexlist = searchPartitionIndexesByblid(partOid);
    if (!PointerIsValid(partIndexlist)) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("no local index defined on partition %u", partOid)));
    }

    // open pg_partition
    pg_partition = relation_open(PartitionRelationId, RowExclusiveLock);
    // for each
    foreach (lc, partIndexlist) {
        HeapTuple partIndexTuple = NULL;
        Oid partIndId = InvalidOid;
        Oid parentIndId = InvalidOid;
        Partition indexPart = NULL;
        Relation parentIndex = NULL;

        partIndexTuple = (HeapTuple)lfirst(lc);
        parentIndId = (((Form_pg_partition)GETSTRUCT(partIndexTuple)))->parentid;
        partIndId = HeapTupleGetOid(partIndexTuple);

        // open index and it's partition
        parentIndex = index_open(parentIndId, AccessShareLock);
        indexPart = partitionOpen(parentIndex, partIndId, AccessExclusiveLock);

        // update the indisusable field , by calling the internal function
        ATExecSetIndexUsableState(PartitionRelationId, partIndId, false);

        // close index and it's partition
        partitionClose(parentIndex, indexPart, NoLock);
        index_close(parentIndex, NoLock);
    }

    freePartList(partIndexlist);

    // update the indisusable field , by calling the internal function
    ATExecSetIndexUsableState(PartitionRelationId, partOid, false);

    // close pg_partition
    relation_close(pg_partition, RowExclusiveLock);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static void ATExecUnusableIndex(Relation rel)
{
    List* indexPartitionTupleList = NULL;
    ListCell* cell = NULL;
    Oid heapOid = InvalidOid;
    Relation heapRelation = NULL;

    if (!RelationIsIndex(rel))
        ereport(ERROR,
            (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                errmsg("can not set unusable index for relation %s , as it is not a index",
                    RelationGetRelationName(rel))));

    // cstore relation doesn't support this feature now.
    if (OidIsValid(rel->rd_rel->relcudescrelid) || rel->rd_rel->relam == CBTREE_AM_OID ||
        rel->rd_rel->relam == CGIN_AM_OID) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Un-support feature"),
                errdetail("column-store index doesn't support this ALTER yet")));
    }

    heapOid = IndexGetRelation(rel->rd_id, false);
    // the index is already lock by AccessExclusive lock, do not lock again.
    // AccessExclusiveLock on heap already held by call AlterTableLookupRelation().
    heapRelation = relation_open(heapOid, NoLock);
    // call the internal function, update pg_index system table
    ATExecSetIndexUsableState(IndexRelationId, rel->rd_id, false);

    // if partitioned index, do extra work: set local index unusable
    if (RelationIsPartitioned(rel)) {
        // update pg_partition system table
        indexPartitionTupleList = searchPgPartitionByParentId(PART_OBJ_TYPE_INDEX_PARTITION, rel->rd_id);
        foreach (cell, indexPartitionTupleList) {
            Partition indexPartition = NULL;
            Partition heapPartition = NULL;
            Oid indexPartOid = HeapTupleGetOid((HeapTuple)lfirst(cell));
            Oid heapPartOid = indexPartGetHeapPart(indexPartOid, false);

            // lock heap partition
            heapPartition = partitionOpen(heapRelation, heapPartOid, AccessExclusiveLock);
            // lock index partition
            indexPartition = partitionOpen(rel, indexPartOid, AccessExclusiveLock);
            // update the indisusable field , by calling the internal function
            ATExecSetIndexUsableState(PartitionRelationId, indexPartOid, false);
            // close heap part and index part
            partitionClose(heapRelation, heapPartition, NoLock);
            partitionClose(rel, indexPartition, NoLock);
        }
        freePartList(indexPartitionTupleList);
    }
    // close heap relation but maintain the lock.
    relation_close(heapRelation, NoLock);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static void ATExecModifyRowMovement(Relation rel, bool rowMovement)
{
    HeapTuple tuple = NULL;
    Oid relid = rel->rd_id;
    Relation pg_class;
    Form_pg_class rd_rel;
    bool dirty = false;

    pg_class = heap_open(RelationRelationId, RowExclusiveLock);

    /* get the tuple of partitioned table */
    tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(tuple)) {
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("could not find tuple for relation %u", relid)));
    }

    rd_rel = (Form_pg_class)GETSTRUCT(tuple);

    /* modify the tuple */
    if (rd_rel->relrowmovement != rowMovement) {
        rd_rel->relrowmovement = rowMovement;
        dirty = true;
    }

    /* update pg_class */
    if (dirty) {
        simple_heap_update(pg_class, &tuple->t_self, tuple);
        CatalogUpdateIndexes(pg_class, tuple);
        /* the above sends a cache inval message */
    } else {
        /* no need to change tuple, but force relcache inval anyway */
        CacheInvalidateRelcacheByTuple(tuple);
    }

    heap_freetuple_ext(tuple);
    heap_close(pg_class, RowExclusiveLock);

    CommandCounterIncrement();
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
static void ATExecTruncatePartition(Relation rel, AlterTableCmd* cmd)
{
    List* oidList = NULL;
    List* relid = lappend_oid(NULL, rel->rd_id);
    RangePartitionDefState* rangePartDef = NULL;
    Oid partOid = InvalidOid;
    Oid newPartOid = InvalidOid;
    Relation newTableRel = NULL;

    oidList = heap_truncate_find_FKs(relid);
    if (PointerIsValid(oidList)) {
        ereport(ERROR,
            (errcode(ERRCODE_UNEXPECTED_NULL_VALUE),
                errmsg("cannot truncate a partition owned by partitioned table which is referenced in a foreign key "
                       "constraint")));
    }

#ifdef PGXC
    /*
     * If parent rel is in redistribution, we need to truncate the same
     * partition in its new table rel.
     */
    if (RelationInClusterResizing(rel) && !RelationInClusterResizingReadOnly(rel)) {
        /*
         * If the target table is under online extension, it should always trigger
         * redis cancel, even though currently no lock confict yet, later it will still
         * meet, because redistribute the target table need to lock all the partition.
         * So we can trigger it now.
         */
        if (IS_PGXC_COORDINATOR)
            u_sess->catalog_cxt.redistribution_cancelable = true;

        newTableRel = GetAndOpenNewTableRel(rel, AccessExclusiveLock);

        if (IS_PGXC_COORDINATOR)
            u_sess->catalog_cxt.redistribution_cancelable = false;
    }
#endif
    /* this maybe useless, consider to remove */
    if (IS_PGXC_COORDINATOR)
        u_sess->exec_cxt.could_cancel_redistribution = true;

    /*
     * Get the partition oid
     * 1. Get partition oid from part_name cluase
     * 2. Get partition oid values clause
     */
    if (PointerIsValid(cmd->name)) {
        partOid = partitionNameGetPartitionOid(rel->rd_id,
            cmd->name,
            PART_OBJ_TYPE_TABLE_PARTITION,
            AccessExclusiveLock,
            false,
            false,
            NULL,
            NULL,
            NoLock);
        if (newTableRel) {
            newPartOid = partitionNameGetPartitionOid(newTableRel->rd_id,
                cmd->name,
                PART_OBJ_TYPE_TABLE_PARTITION,
                AccessExclusiveLock,
                false,
                false,
                NULL,
                NULL,
                NoLock);
        }
    } else {
        rangePartDef = (RangePartitionDefState*)cmd->def;
        rangePartDef->boundary = transformConstIntoTargetType(rel->rd_att->attrs,
            ((RangePartitionMap*)rel->partMap)->partitionKey,
            rangePartDef->boundary);
        partOid = partitionValuesGetPartitionOid(rel,
            rangePartDef->boundary,
            AccessExclusiveLock,
            true,
            true, /* will check validity of partition oid next step */
            false);
        if (newTableRel) {
            newPartOid = partitionValuesGetPartitionOid(newTableRel,
                rangePartDef->boundary,
                AccessExclusiveLock,
                true,
                true, /* will check validity of partition oid next step */
                false);
        }
    }

    if (!OidIsValid(partOid)) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("The partition number is invalid or out-of-range")));
    }

    heap_truncate_one_part(rel, partOid);
    pgstat_report_truncate(partOid, rel->rd_id, rel->rd_rel->relisshared);

    /* If newTableRel is not NULL, the parent rel must be in redistribution */
    if (newTableRel) {
        heap_truncate_one_part(newTableRel, newPartOid);

        /*  delete partOid related rows in delete delta table  */
        simple_delete_redis_tuples(rel, partOid);
        ResetOnePartRedisCtidRelOptions(rel, partOid);

        /* clean up */
        heap_close(newTableRel, AccessExclusiveLock);
        pgstat_report_truncate(newPartOid, newTableRel->rd_id, newTableRel->rd_rel->relisshared);
    }
}

/*
 * - Brief: delete tuples in a redis delete delta table for partition table
 *          being redistributed
 * - Parameter:
 *      @deltaRel: redis delete delta relation
 *      @partOid: partition oid
 * - Return:
 *      @void:
 */
static void delete_delta_table_tuples(Relation deltaRel, Oid partOid)
{
    ScanKeyData key;
    int indexPartOid;
    HeapScanDesc scan;
    HeapTuple tup;

    Assert(deltaRel != NULL);

    /*
     * Delete partOid related tuples in delete delta table.
     * Column index of partitionoid is always the last 3rd one.
     */
    indexPartOid = deltaRel->rd_att->natts - 2;
    Assert(indexPartOid > 0);

    ScanKeyInit(&key, indexPartOid, BTEqualStrategyNumber, F_INT4EQ, ObjectIdGetDatum(partOid));

    scan = heap_beginscan(deltaRel, SnapshotNow, 1, &key);

    while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection))) {
        simple_heap_delete(deltaRel, &tup->t_self);
    }

    heap_endscan(scan);
}

/*
 * - Brief: delete tuples in redis related delete delta tables for partition table
 *          being redistributed
 * - Parameter:
 *      @rel: relation being redistributed
 *      @partOid: partition oid
 * - Return:
 *      @void:
 */
static void simple_delete_redis_tuples(Relation rel, Oid partOid)
{
    Relation deltaRel;

    /*
     * First delete the multi catchup delta table tuples.
     * Always keep the order consistent by operating on multi catchup delete delta first and then the delete delta.
     */
    deltaRel = GetAndOpenDeleteDeltaRel(rel, RowExclusiveLock, true);
    if (deltaRel) {
        delete_delta_table_tuples(deltaRel, partOid);
        heap_close(deltaRel, RowExclusiveLock);
    }

    deltaRel = GetAndOpenDeleteDeltaRel(rel, RowExclusiveLock, false);
    if (deltaRel) {
        delete_delta_table_tuples(deltaRel, partOid);
        heap_close(deltaRel, RowExclusiveLock);
    }
}

// find each index partition corresponding to srcPartOids,
// under clonedIndexRelation.
// then, add the fake relation for local index to end of merging_btrees_list.
static List* generateMergeingIndexes(
    Relation destIndexRelation, Relation clonedIndexRelation, int2 bucketId, List* heapPartOids, List** partList)
{
    ListCell* cell = NULL;
    List* merging_btrees_list = NIL;
    List* merging_part_list = NIL;

    if (clonedIndexRelation->rd_rel->relam != BTREE_AM_OID)
        return merging_btrees_list;

    merging_btrees_list = lappend(merging_btrees_list, destIndexRelation);

    foreach (cell, heapPartOids) {
        Oid heapPartOid = InvalidOid;
        Oid indexPartOid = InvalidOid;
        Partition indexPart = NULL;
        Relation indexPartRel = NULL;

        heapPartOid = lfirst_oid(cell);
        indexPartOid = getPartitionIndexOid(clonedIndexRelation->rd_id, heapPartOid);
        // the index partition already locked by checkPartitionLocalIndexesUsable()
        indexPart = partitionOpen(clonedIndexRelation, indexPartOid, NoLock, bucketId);
        if (!indexPart->pd_part->indisusable) {
            ereport(ERROR,
                (errcode(ERRCODE_UNEXPECTED_NODE_STATE),
                    errmsg("can not merge index partition %s bacause it is unusable local index",
                        PartitionGetPartitionName(indexPart))));
        }
        indexPartRel = partitionGetRelation(clonedIndexRelation, indexPart);

        // append indexPartRel
        merging_btrees_list = lappend(merging_btrees_list, indexPartRel);
        merging_part_list = lappend(merging_part_list, indexPart);
    }

    if (partList != NULL)
        *partList = merging_part_list;

    return merging_btrees_list;
}

static void destroyMergeingIndexes(Relation srcIndexRelation, List* merging_btrees_list, List* merging_part_list)
{
    ListCell* cell1 = NULL;
    ListCell* cell2 = NULL;
    int i = 0;

    cell2 = list_head(merging_part_list);

    foreach (cell1, merging_btrees_list) {
        Relation indexRelation = (Relation)lfirst(cell1);
        Partition indexPartition = NULL;

        if (i == 0) {
            relation_close(indexRelation, NoLock);
        } else if (!OidIsValid(indexRelation->parentId)) {
            relation_close(indexRelation, NoLock);
        } else {

            indexPartition = (Partition)lfirst(cell2);
            partitionClose(srcIndexRelation, indexPartition, NoLock);
            releaseDummyRelation(&indexRelation);
            cell2 = lnext(cell2);
        }
        i++;
    }
    list_free_ext(merging_btrees_list);
    list_free_ext(merging_part_list);
}

static void mergePartitionIndexSwap(List* indexRel, List* indexDestPartOid, List* indexDestOid, TransactionId FreezeXid)
{
    ListCell* cell1 = NULL;
    ListCell* cell2 = NULL;
    ListCell* cell3 = NULL;

    forthree(cell1, indexRel, cell2, indexDestPartOid, cell3, indexDestOid)
    {
        Relation currentIndex = (Relation)lfirst(cell1);
        Oid dstPartOid = lfirst_oid(cell2);
        Oid clonedIndexRelationId = lfirst_oid(cell3);
        Partition dstPart;

        /* before swap refilenode, promote lock on index partition from ExclusiveLock to AccessExclusiveLock */
        dstPart = partitionOpenWithRetry(currentIndex, dstPartOid, AccessExclusiveLock, "MERGE PARTITIONS");
        if (!dstPart) {
            ereport(ERROR,
                (errcode(ERRCODE_LOCK_WAIT_TIMEOUT),
                    errmsg(
                        "could not acquire AccessExclusiveLock on dest index partition \"%s\", MERGE PARTITIONS failed",
                        getPartitionName(dstPartOid, false))));
        }
        /* swap relfilenode between temp index relation and dest index partition */
        finishPartitionHeapSwap(dstPartOid, clonedIndexRelationId, false, FreezeXid);
        partitionClose(currentIndex, dstPart, NoLock);
    }
}

static void mergePartitionHeapSwap(Relation partTableRel, Oid destPartOid, Oid tempTableOid, TransactionId FreezeXid)
{
    Partition destPart;

    /* before swap refilenode, promote lock on heap partition from ExclusiveLock to AccessExclusiveLock */
    destPart = partitionOpenWithRetry(partTableRel, destPartOid, AccessExclusiveLock, "MERGE PARTITIONS");

    /* step 4: swap relfilenode and delete temp table */
    if (!destPart) {
        ereport(ERROR,
            (errcode(ERRCODE_LOCK_WAIT_TIMEOUT),
                errmsg("could not acquire AccessExclusiveLock on dest table partition \"%s\", MERGE PARTITIONS failed",
                    getPartitionName(destPartOid, false))));
    }

    finishPartitionHeapSwap(destPartOid, tempTableOid, false, FreezeXid);
    partitionClose(partTableRel, destPart, NoLock);
}

static void mergePartitionHeapData(Relation partTableRel, Relation tempTableRel, List* srcPartOids, List* indexRel_list,
    List* indexDestOid_list, int2 bucketId, TransactionId* freezexid)
{
    TransactionId FreezeXid = InvalidTransactionId;
    HTAB* chunkIdHashTable = NULL;
    ListCell* cell1 = NULL;
    ListCell* cell2 = NULL;
    List* mergeToastIndexes = NIL;
    List* srcPartToastMergeOffset = NIL;
    List* srcPartMergeOffset = NIL;
    bool hasToast = false;
    Relation tempTableToastRel = NULL;
    Relation tempTableToastIndexRel = NULL;
    BlockNumber mergeHeapBlocks = 0;
    BlockNumber mergeToastBlocks = 0;
    int partNum = 0;
    int iterator = 0;
    bool* srcPartsHasVM = NULL;
    bool hasVM = false;
    bool hasFSM = false;

    partNum = srcPartOids->length;

    if (OidIsValid(tempTableRel->rd_rel->reltoastrelid)) {
        hasToast = true;
        tempTableToastRel = relation_open(tempTableRel->rd_rel->reltoastrelid, AccessExclusiveLock, bucketId);
        tempTableToastIndexRel = index_open(tempTableToastRel->rd_rel->reltoastidxid, AccessExclusiveLock, bucketId);
        mergeToastIndexes = lappend(mergeToastIndexes, tempTableToastIndexRel);
    }

    /* step 3: merge each src partition's tuple into the temp table */
    mergeHeapBlocks = 0;
    mergeToastBlocks = 0;

    /*
     *  3.1 check chunk_id of toast table not repeat
     */
    if (hasToast) {
        HASHCTL hashCtl;
        List* srcPartToastRels = NIL;
        errno_t rc = EOK;

        /* Initialize hash tables */
        rc = memset_s(&hashCtl, sizeof(hashCtl), 0, sizeof(hashCtl));
        securec_check(rc, "\0", "\0");
        hashCtl.keysize = sizeof(ChunkIdHashKey);
        hashCtl.entrysize = sizeof(OldToNewChunkIdMapping);
        hashCtl.hash = tag_hash;

        chunkIdHashTable =
            hash_create("Merge partition / Old to new chunkId map", 128, &hashCtl, HASH_ELEM | HASH_FUNCTION);

        foreach (cell1, srcPartOids) {
            Oid srcPartOid = lfirst_oid(cell1);
            Partition srcPartition = NULL;
            Relation srcPartToastRel = NULL;
            Relation srcPartToastIndexRel = NULL;

            srcPartition = partitionOpen(partTableRel, srcPartOid, NoLock);

            /* open toast table and it's index */
            srcPartToastRel = relation_open(srcPartition->pd_part->reltoastrelid, ExclusiveLock, bucketId);
            srcPartToastIndexRel = index_open(srcPartToastRel->rd_rel->reltoastidxid, ExclusiveLock, bucketId);

            srcPartToastRels = lappend(srcPartToastRels, srcPartToastRel);
            mergeToastIndexes = lappend(mergeToastIndexes, srcPartToastIndexRel);

            partitionClose(partTableRel, srcPartition, NoLock);
        }

        /* Find repeat chunkId in toast tables, and replace. */
        replaceRepeatChunkId(chunkIdHashTable, srcPartToastRels);

        foreach (cell1, srcPartToastRels) {
            Relation srcPartToastRel = (Relation)lfirst(cell1);
            relation_close(srcPartToastRel, NoLock);
        }

        list_free_ext(srcPartToastRels);
    }
    /*
     * 3.2 check VM and FSM of src partitions
     */
    srcPartsHasVM = (bool*)palloc0(partNum * sizeof(bool));

    iterator = 0;
    foreach (cell1, srcPartOids) {
        Oid srcPartOid = lfirst_oid(cell1);
        Partition srcPartition = NULL;

        srcPartition = partitionOpen(partTableRel, srcPartOid, NoLock, bucketId);
        PartitionOpenSmgr(srcPartition);

        if (smgrexists(srcPartition->pd_smgr, VISIBILITYMAP_FORKNUM)) {
            srcPartsHasVM[iterator] = true;
            hasVM = true;
        }

        if (smgrexists(srcPartition->pd_smgr, FSM_FORKNUM)) {
            hasFSM = true;
        }

        iterator++;

        PartitionCloseSmgr(srcPartition);
        partitionClose(partTableRel, srcPartition, NoLock);
    }

    /* create VM on temp table if need */
    if (hasVM) {
        /* Retry to open smgr in case it is cloesd when we process SI messages  */
        RelationOpenSmgr(tempTableRel);
        smgrcreate(tempTableRel->rd_smgr, VISIBILITYMAP_FORKNUM, false);
    }

    /* create FSM on temp table if need */
    if (hasFSM) {
        /* Retry to open smgr in case it is cloesd when we process SI messages  */
        RelationOpenSmgr(tempTableRel);
        smgrcreate(tempTableRel->rd_smgr, FSM_FORKNUM, false);
    }

    /*
     * 3.3 merge heap and toast, if any
     */
    iterator = 0;
    foreach (cell1, srcPartOids) {
        Oid srcPartOid = lfirst_oid(cell1);
        Partition srcPartition = NULL;
        Relation srcPartRel = NULL;
        char persistency;
        BlockNumber srcPartHeapBlocks = 0;
        TransactionId relfrozenxid;

        /* already ExclusiveLock locked */
        srcPartition = partitionOpen(partTableRel, srcPartOid, ExclusiveLock, bucketId);
        srcPartRel = partitionGetRelation(partTableRel, srcPartition);
        PartitionOpenSmgr(srcPartition);

        relfrozenxid = getPartitionRelfrozenxid(srcPartRel);
        /* update final fronzenxid, we choose the least one */
        if (!TransactionIdIsValid(FreezeXid) || TransactionIdPrecedes(relfrozenxid, FreezeXid))
            FreezeXid = relfrozenxid;

        /* flush dirty pages to disk */
        FlushRelationBuffers(srcPartRel);
        smgrimmedsync(srcPartRel->rd_smgr, MAIN_FORKNUM);

        persistency = srcPartRel->rd_rel->relpersistence;

        srcPartHeapBlocks = smgrnblocks(srcPartRel->rd_smgr, MAIN_FORKNUM);

        /* merge heap */
        mergeHeapBlock(srcPartRel,
            tempTableRel,
            MAIN_FORKNUM,
            persistency,
            srcPartHeapBlocks,
            mergeHeapBlocks,
            srcPartRel->rd_att,
            srcPartRel->rd_rel->reltoastrelid,
            tempTableRel->rd_rel->reltoastrelid,
            hasToast ? chunkIdHashTable : NULL,
            hasFSM);

        /* merge toast table */
        if (hasToast) {
            Relation srcPartToastRel = NULL;
            char toastPersistency;
            BlockNumber srcPartToastBlocks = 0;

            srcPartToastRel = relation_open(srcPartition->pd_part->reltoastrelid, NoLock, bucketId);
            RelationOpenSmgr(srcPartToastRel);
            srcPartToastMergeOffset = lappend_int(srcPartToastMergeOffset, mergeToastBlocks);

            /* flush dirty pages to disk */
            FlushRelationBuffers(srcPartToastRel);
            smgrimmedsync(srcPartToastRel->rd_smgr, MAIN_FORKNUM);

            toastPersistency = srcPartToastRel->rd_rel->relpersistence;

            srcPartToastBlocks = smgrnblocks(srcPartToastRel->rd_smgr, MAIN_FORKNUM);

            mergeHeapBlock(srcPartToastRel,
                tempTableToastRel,
                MAIN_FORKNUM,
                toastPersistency,
                srcPartToastBlocks,
                mergeToastBlocks,
                NULL,
                InvalidOid,
                InvalidOid,
                NULL,
                false);
            mergeToastBlocks += srcPartToastBlocks;

            RelationCloseSmgr(srcPartToastRel);
            relation_close(srcPartToastRel, NoLock);
        }

        /* merge VM */
        if (hasVM && srcPartsHasVM[iterator]) {
            mergeVMBlock(srcPartRel, tempTableRel, srcPartHeapBlocks, mergeHeapBlocks);
        }

        PartitionCloseSmgr(srcPartition);
        partitionClose(partTableRel, srcPartition, NoLock);
        releaseDummyRelation(&srcPartRel);

        iterator++;
        srcPartMergeOffset = lappend_int(srcPartMergeOffset, mergeHeapBlocks);
        mergeHeapBlocks += srcPartHeapBlocks;
    }

    pfree_ext(srcPartsHasVM);

    if (freezexid != NULL)
        *freezexid = FreezeXid;
    /*
     * 3.4 merge toast indexes and destroy chunkId hash table
     */
    if (hasToast) {
        mergeBTreeIndexes(mergeToastIndexes, srcPartToastMergeOffset);
        destroyMergeingIndexes(NULL, mergeToastIndexes, NULL);
        RelationCloseSmgr(tempTableToastRel);
        heap_close(tempTableToastRel, NoLock);
        hash_destroy(chunkIdHashTable);
    }

    /*
     *	3.5 merge btree-indexes
     *
     */
    forboth(cell1, indexRel_list, cell2, indexDestOid_list)
    {
        Relation currentIndex = (Relation)lfirst(cell1);
        Oid clonedIndexRelationId = lfirst_oid(cell2);
        bool skip_build = false;
        Relation clonedIndexRelation;
        List* merging_btrees_list = NIL;
        List* merging_part_list = NIL;

        if (currentIndex->rd_rel->relam == BTREE_AM_OID)
            skip_build = true;
        else
            skip_build = false;

        /* merge several indexes together */
        if (skip_build) {
            /*
             * add the newly created index into merging_btrees_list
             * we now begin creating a list of index relation for merging
             */
            clonedIndexRelation = index_open(clonedIndexRelationId, AccessExclusiveLock, bucketId);
            merging_btrees_list =
                generateMergeingIndexes(clonedIndexRelation, currentIndex, bucketId, srcPartOids, &merging_part_list);

            /* merging indexes: using the merging_btrees_list as the source */
            mergeBTreeIndexes(merging_btrees_list, srcPartMergeOffset);

            /* destroy the merging indexes list */
            destroyMergeingIndexes(currentIndex, merging_btrees_list, merging_part_list);
        }
    }
}

/*
 * MERGE partitions p1, p2...pn into partition px
 * infact, pn is the same partition with px
 *  if px is deferent with pn, change pn's name to px
 */
static void ATExecMergePartition(Relation partTableRel, AlterTableCmd* cmd)
{
    List* srcPartitions = NIL;
    List* srcPartOids = NIL;
    List* index_list = NIL;
    List* indexRel_list = NIL;
    List* clonedIndexRelId_list = NIL;
    List* indexDestPartOid_list = NIL;
    ListCell* cell = NULL;
    char* destPartName = NULL;
    char* oldPartName = NULL;
    Oid destPartOid = InvalidOid;
    Partition destPart = NULL;
    Relation destPartRel = NULL;
    bool renameTargetPart = false;
    int curPartSeq = 0;
    int prevPartSeq = -1;
    int iterator = 0;
    int partNum = 0;
    Oid targetPartTablespaceOid = InvalidOid;
    TupleDesc partedTableHeapDesc;
    Datum partedTableRelOptions = 0;
    HeapTuple tuple = NULL;
    bool isNull = false;
    Oid tempTableOid = InvalidOid;
    Relation tempTableRel = NULL;
    ObjectAddress object;
    TransactionId FreezeXid;

    srcPartitions = (List*)cmd->def;
    destPartName = cmd->name;
    partNum = srcPartitions->length;

    /* Branch if we are dealing with column-store */
    if (RelationIsColStore(partTableRel)) {
        ATExecCStoreMergePartition(partTableRel, cmd);
        return;
    }

    /* the source partitions, must be at least 2, to merge into 1 partition  */
    if (partNum < 2) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("source partitions must be at least two partitions")));
    }
    if (partNum > MAX_MERGE_PARTITIONS) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("merge partitions of relation \"%s\", source partitions must be no more than %d partitions",
                    RelationGetRelationName(partTableRel),
                    MAX_MERGE_PARTITIONS)));
    }

    /*
     * step 1: lock src partitions, and check the continuity of srcPartitions.
     * for 1...nth partition, we use AccessExclusiveLock lockmode
     * althought merge is something like delete and insert.
     */
    foreach (cell, srcPartitions) {
        char* partName = NULL;
        Oid srcPartOid = InvalidOid;

        iterator++;
        partName = strVal(lfirst(cell));

        /* from name to partition oid */
        srcPartOid = partitionNameGetPartitionOid(partTableRel->rd_id,
            partName,
            PART_OBJ_TYPE_TABLE_PARTITION,
            ExclusiveLock,  // get ExclusiveLock lock on src partitions
            false,          // no missing
            false,          // wait
            NULL,
            NULL,
            NoLock);
        /* check local index 'usable' state */
        if (!checkPartitionLocalIndexesUsable(srcPartOid)) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                    errmsg("can't merge partition bacause partition %s has unusable local index", partName),
                    errhint("please reindex the unusable index first.")));
        }

        /* from partitionoid to partition sequence */
        curPartSeq = partOidGetPartSequence(partTableRel, srcPartOid);

        /* check the continuity of sequence, not the first round loop */
        if (iterator != 1) {
            if (curPartSeq - prevPartSeq != 1)
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("source partitions must be continuous and in ascending order of boundary")));
        }
        prevPartSeq = curPartSeq;

        /* save the last source partition name */
        if (iterator == partNum) {
            oldPartName = partName;
            destPartOid = srcPartOid;
        }

        /* save oid of src partition */
        srcPartOids = lappend_oid(srcPartOids, srcPartOid);
    }

    if (strcmp(oldPartName, destPartName) != 0) {
        /*  check partition new name does not exist. */
        if (InvalidOid != GetSysCacheOid3(PARTPARTOID,
            NameGetDatum(destPartName),
            CharGetDatum(PART_OBJ_TYPE_TABLE_PARTITION),
            ObjectIdGetDatum(partTableRel->rd_id))) {
            ereport(ERROR,
                (errcode(ERRCODE_DUPLICATE_OBJECT),
                    errmsg("target partition's name \"%s\" already exists", destPartName)));
        }

        renameTargetPart = true;
    }

    /*
     * step 2: create a temp table for merge
     * get desc of partitioned table
     */
    partedTableHeapDesc = RelationGetDescr(partTableRel);

    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(RelationGetRelid(partTableRel)));
    if (!HeapTupleIsValid(tuple)) {
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for relation %u", RelationGetRelid(partTableRel))));
    }
    partedTableRelOptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isNull);
    if (isNull) {
        partedTableRelOptions = (Datum)0;
    }

    /* open the dest partition, it was already locked by partitionNameGetPartitionOid() call */
    destPart = partitionOpen(partTableRel, destPartOid, NoLock);
    destPartRel = partitionGetRelation(partTableRel, destPart);

    /* check target partition tablespace */
    if (PointerIsValid(cmd->target_partition_tablespace)) {
        targetPartTablespaceOid = get_tablespace_oid(cmd->target_partition_tablespace, false);
    } else {
        targetPartTablespaceOid = destPartRel->rd_rel->reltablespace;
    }

    /* create temp table and open it */
    tempTableOid = makePartitionNewHeap(partTableRel,
        partedTableHeapDesc,
        partedTableRelOptions,
        destPartRel->rd_id,
        destPartRel->rd_rel->reltoastrelid,
        targetPartTablespaceOid);
    object.classId = RelationRelationId;
    object.objectId = tempTableOid;
    object.objectSubId = 0;

    ReleaseSysCache(tuple);
    partitionClose(partTableRel, destPart, NoLock);
    releaseDummyRelation(&destPartRel);

    /* open temp relation */
    tempTableRel = relation_open(tempTableOid, AccessExclusiveLock);
    RelationOpenSmgr(tempTableRel);

    /* lock the index relation on partitioned table and check the usability */
    index_list = RelationGetIndexList(partTableRel);
    foreach (cell, index_list) {
        Oid dstIndexPartTblspcOid;
        Oid clonedIndexRelationId;
        Oid indexDestPartOid;
        Oid indexId = lfirst_oid(cell);
        char tmp_idxname[NAMEDATALEN];
        Relation currentIndex;
        bool skip_build = false;
        errno_t rc = EOK;

        /* Open the index relation, use AccessShareLock */
        currentIndex = index_open(indexId, AccessShareLock);

        if (!IndexIsUsable(currentIndex->rd_index)) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                    errmsg("merge partitions cannot process inusable index relation \''%s\''",
                        RelationGetRelationName(currentIndex))));
        } else {
            indexRel_list = lappend(indexRel_list, currentIndex);
        }

        if (currentIndex->rd_rel->relam == BTREE_AM_OID)
            skip_build = true;
        else
            skip_build = false;

        /* build name for tmp index */
        rc = snprintf_s(tmp_idxname,
            sizeof(tmp_idxname),
            sizeof(tmp_idxname) - 1,
            "pg_tmp_%u_%u_index",
            destPartOid,
            currentIndex->rd_id);
        securec_check_ss_c(rc, "\0", "\0");

        /* get tablespace oid of this index partition */
        dstIndexPartTblspcOid = getPartitionIndexTblspcOid(currentIndex->rd_id, destPartOid);

        /* build the same index for tmp table */
        clonedIndexRelationId =
            generateClonedIndex(currentIndex, tempTableRel, tmp_idxname, dstIndexPartTblspcOid, skip_build, false);
        indexDestPartOid = getPartitionIndexOid(currentIndex->rd_id, destPartOid);

        clonedIndexRelId_list = lappend_oid(clonedIndexRelId_list, clonedIndexRelationId);
        indexDestPartOid_list = lappend_oid(indexDestPartOid_list, indexDestPartOid);
    }

    /*
     * 3.3 merge heap and toast, if any
     */
    if (ThereAreNoPriorRegisteredSnapshots() && ThereAreNoReadyPortals()) {
        u_sess->cmd_cxt.mergePartition_Freeze = true;
        vacuum_set_xid_limits(partTableRel,
            -1,
            -1,
            &u_sess->cmd_cxt.mergePartition_OldestXmin,
            &u_sess->cmd_cxt.mergePartition_FreezeXid,
            NULL);
    }

    if (OidIsValid(tempTableRel->rd_rel->reltoastrelid)) {
        /* set new empty filenode for toast index */
        Relation toastRel = relation_open(tempTableRel->rd_rel->reltoastrelid, AccessExclusiveLock);
        Relation toastIndexRel = index_open(toastRel->rd_rel->reltoastidxid, AccessExclusiveLock);
        RelationSetNewRelfilenode(toastIndexRel, InvalidTransactionId);
        relation_close(toastRel, NoLock);
        index_close(toastIndexRel, NoLock);
    }

    /* step3: merge internal */
    if (RELATION_OWN_BUCKETKEY(partTableRel)) {
        Relation tempbucketRel = NULL;
        oidvector* bucketlist = searchHashBucketByOid(partTableRel->rd_bucketoid);

        for (int i = 0; i < bucketlist->dim1; i++) {
            tempbucketRel = bucketGetRelation(tempTableRel, NULL, bucketlist->values[i]);
            mergePartitionHeapData(partTableRel,
                tempbucketRel,
                srcPartOids,
                indexRel_list,
                clonedIndexRelId_list,
                bucketlist->values[i],
                &FreezeXid);
            bucketCloseRelation(tempbucketRel);
        }
    } else {
        mergePartitionHeapData(
            partTableRel, tempTableRel, srcPartOids, indexRel_list, clonedIndexRelId_list, InvalidBktId, &FreezeXid);
    }

    /* close temp relation */
    RelationCloseSmgr(tempTableRel);
    heap_close(tempTableRel, NoLock);

    /*
     * renew the final frozenxid for partition: set it to mergePartition_FreezeXid if mergePartition_Freeze is true,
     * otherwise set it to the least one in all srcpartition
     */
    if (u_sess->cmd_cxt.mergePartition_Freeze &&
        TransactionIdPrecedes(FreezeXid, u_sess->cmd_cxt.mergePartition_FreezeXid))
        FreezeXid = u_sess->cmd_cxt.mergePartition_FreezeXid;

    /* swap the index relfilenode */
    mergePartitionIndexSwap(indexRel_list, indexDestPartOid_list, clonedIndexRelId_list, FreezeXid);

    /* swap the heap relfilenode */
    mergePartitionHeapSwap(partTableRel, destPartOid, tempTableOid, FreezeXid);

    /* free index list */
    list_free_ext(index_list);
    list_free_ext(clonedIndexRelId_list);
    list_free_ext(indexDestPartOid_list);

    /* ensure that preceding changes are all visible to the next deletion step. */
    CommandCounterIncrement();

    /* delete temp table */
    performDeletion(&object, DROP_CASCADE, PERFORM_DELETION_INTERNAL);

    /* close every index relation */
    foreach (cell, indexRel_list) {
        Relation currentIndex = (Relation)lfirst(cell);
        index_close(currentIndex, NoLock);
    }
    list_free_ext(indexRel_list);

    /* step 5: drop src partitions */
    foreach (cell, srcPartOids) {
        Oid srcPartOid = InvalidOid;
        srcPartOid = lfirst_oid(cell);
        if (destPartOid != srcPartOid) {
            fastDropPartition(partTableRel, srcPartOid, "MERGE PARTITIONS");
        }
    }

    /*
     * step 6: rename p(n) to p(target) if needed, the dest partition is now locked by swap refilenode processing step
     */
    if (renameTargetPart) {
        renamePartitionInternal(partTableRel->rd_id, destPartOid, destPartName);
    }
}

// When merge toast table, values of the first column may be repeat.
// So, we must replace these values, and record them in repeatChunkIdList.
// When merge heap, modify toast_pointer->va_valueid by repeatChunkIdList.
static void replaceRepeatChunkId(HTAB* chunkIdHashTable, List* srcPartToastRels)
{
    ListCell* cell = NULL;
    int i = 0;
    errno_t rc = EOK;

    foreach (cell, srcPartToastRels) {
        Relation srcPartToastRel = NULL;
        Relation toastIndexRel = NULL;
        HeapScanDesc scan = NULL;
        HeapTuple tuple = NULL;
        TupleDesc tupleDesc = NULL;
        int numAttrs = 0;

        srcPartToastRel = (Relation)lfirst(cell);
        toastIndexRel = index_open(srcPartToastRel->rd_rel->reltoastidxid, RowExclusiveLock);

        tupleDesc = srcPartToastRel->rd_att;
        numAttrs = tupleDesc->natts;

        scan = heap_beginscan(srcPartToastRel, SnapshotNow, 0, NULL);

        while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
            Datum values[numAttrs];
            bool isNull[numAttrs];
            Oid oldChunkId = InvalidOid;
            Oid newChunkId = InvalidOid;

            bool found = false;
            ChunkIdHashKey hashkey;
            OldToNewChunkIdMapping mapping = NULL;

            rc = memset_s(values, sizeof(values), 0, sizeof(values));
            securec_check(rc, "\0", "\0");

            rc = memset_s(isNull, numAttrs, 0, numAttrs);
            securec_check(rc, "\0", "\0");

            heap_deform_tuple(tuple, tupleDesc, values, isNull);

            oldChunkId = (Oid)values[0];

            rc = memset_s(&hashkey, sizeof(hashkey), 0, sizeof(hashkey));
            securec_check(rc, "\0", "\0");
            hashkey.toastTableOid = RelationGetRelid(srcPartToastRel);
            hashkey.oldChunkId = oldChunkId;

            mapping = (OldToNewChunkIdMapping)hash_search(chunkIdHashTable, &hashkey, HASH_FIND, NULL);

            // One data may be cut into several tuples.
            // These tuples have the same chunkId.
            // So we replace the same new value if need.
            if (PointerIsValid(mapping)) {
                HeapTuple copyTuple = NULL;

                values[0] = mapping->newChunkId;
                copyTuple = heap_form_tuple(tupleDesc, values, isNull);

                simple_heap_delete(srcPartToastRel, &tuple->t_self);
                (void)simple_heap_insert(srcPartToastRel, copyTuple);

                (void)index_insert(toastIndexRel,
                    values,
                    isNull,
                    &(copyTuple->t_self),
                    srcPartToastRel,
                    toastIndexRel->rd_index->indisunique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO);

                heap_freetuple_ext(copyTuple);

                continue;
            }

            if (checkChunkIdRepeat(srcPartToastRels, i, oldChunkId)) {
                HeapTuple copyTuple = NULL;

                // Get one new oid, and it is not repeat in other toast tables.
                do {
                    newChunkId = GetNewObjectId();
                } while (checkChunkIdRepeat(srcPartToastRels, -1, newChunkId));

                values[0] = newChunkId;
                copyTuple = heap_form_tuple(tupleDesc, values, isNull);

                simple_heap_delete(srcPartToastRel, &tuple->t_self);
                (void)simple_heap_insert(srcPartToastRel, copyTuple);

                (void)index_insert(toastIndexRel,
                    values,
                    isNull,
                    &(copyTuple->t_self),
                    srcPartToastRel,
                    toastIndexRel->rd_index->indisunique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO);

                heap_freetuple_ext(copyTuple);

                // Enter hash table
                mapping = (OldToNewChunkIdMapping)hash_search(chunkIdHashTable, &hashkey, HASH_ENTER, &found);

                Assert(!found);

                mapping->newChunkId = newChunkId;
            }
        }

        heap_endscan(scan);
        index_close(toastIndexRel, RowExclusiveLock);

        i++;
    }
}

// Check whether or not chunkId is repeat in the other toast tables.
static bool checkChunkIdRepeat(List* srcPartToastRels, int selfIndex, Oid chunkId)
{
    ListCell* cell = NULL;
    int i = 0;

    foreach (cell, srcPartToastRels) {
        Relation srcPartToastRel = (Relation)lfirst(cell);

        // skip self.
        if ((i++ == selfIndex)) {
            continue;
        }

        if (toastrel_valueid_exists(srcPartToastRel, chunkId)) {
            return true;
        }
    }

    return false;
}

// Description : Execute exchange
static void ATExecExchangePartition(Relation partTableRel, AlterTableCmd* cmd)
{
    Oid ordTableOid = InvalidOid;
    Oid partOid = InvalidOid;
    Relation ordTableRel = NULL;
    List* partIndexList = NIL;
    List* ordIndexList = NIL;
    TransactionId relfrozenxid = InvalidTransactionId;

    ordTableOid = RangeVarGetRelid(cmd->exchange_with_rel, AccessExclusiveLock, false);

    partOid = getPartitionOid(partTableRel, cmd->name, (RangePartitionDefState*)cmd->def);

    if (!OidIsValid(partOid)) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("Specified partition does not exist")));
    }

    Assert(OidIsValid(ordTableOid));

    ordTableRel = heap_open(ordTableOid, NoLock);

    if (ordTableRel->rd_rel->relkind != RELKIND_RELATION ||
        ordTableRel->rd_rel->parttype == PARTTYPE_PARTITIONED_RELATION ||
        ordTableRel->rd_rel->relpersistence == RELPERSISTENCE_TEMP ||
        ordTableRel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED) {
        ereport(
            ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("ALTER TABLE EXCHANGE requires an ordinary table")));
    }

    if (RELATION_HAS_BUCKET(ordTableRel) != RELATION_HAS_BUCKET(partTableRel)) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("ALTER TABLE EXCHANGE requires both ordinary table and partitioned table "
                       "to have the same hashbucket option(on or off)")));
    }

    // Check ordinary competence
    CheckTableNotInUse(ordTableRel, "ALTER TABLE");
    ATSimplePermissions(ordTableRel, ATT_TABLE);

    // Check compress
    checkCompressForExchange(partTableRel, ordTableRel);

    // Check number, type of column
    checkColumnForExchange(partTableRel, ordTableRel);

    // Check constraint of two tables
    checkConstraintForExchange(partTableRel, ordTableRel);

#ifdef PGXC
    // Check distribute of two tables only on coordinator
    if (IS_PGXC_COORDINATOR) {
        checkDistributeForExchange(partTableRel, ordTableRel);
    }
#endif

    // Check number, type of index
    checkIndexForExchange(partTableRel, partOid, ordTableRel, &partIndexList, &ordIndexList);

    // Check if the tables are colstore
    checkColStoreForExchange(partTableRel, ordTableRel);
    // Swap object of partition and ordinary table
    LockPartition(partTableRel->rd_id, partOid, AccessExclusiveLock, PARTITION_LOCK);

    // Check the value is valided for partition boundary
    if (cmd->check_validation) {
        checkValidationForExchange(partTableRel, ordTableRel, partOid, cmd->exchange_verbose);
    }
    if (RelationIsPartition(ordTableRel))
        relfrozenxid = getPartitionRelfrozenxid(ordTableRel);
    else
        relfrozenxid = getRelationRelfrozenxid(ordTableRel);
    // Swap relfilenode of table and toast table
    finishPartitionHeapSwap(partOid, ordTableRel->rd_id, false, relfrozenxid);

    // Swap relfilenode of index
    Assert(list_length(partIndexList) == list_length(ordIndexList));
    if (0 != list_length(partIndexList)) {
        finishIndexSwap(partIndexList, ordIndexList);
        list_free_ext(partIndexList);
        list_free_ext(ordIndexList);
    }

    heap_close(ordTableRel, NoLock);
}

static void checkColStoreForExchange(Relation partTableRel, Relation ordTableRel)
{
    if ((RelationIsColStore(partTableRel) && !RelationIsColStore(ordTableRel)) ||
        (!RelationIsColStore(partTableRel) && RelationIsColStore(ordTableRel)))
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("tables in ALTER TABLE EXCHANGE PARTITION must have the same column/row storage")));
}
// Description : Check compress
static void checkCompressForExchange(Relation partTableRel, Relation ordTableRel)
{
    if (partTableRel->rd_rel->relcmprs != ordTableRel->rd_rel->relcmprs) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("tables in ALTER TABLE EXCHANGE PARTITION must have the same type of compress")));
    }
}

// Description : Check number, type of column
static void checkColumnForExchange(Relation partTableRel, Relation ordTableRel)
{
    CatCList* partAttList = NULL;
    CatCList* ordAttList = NULL;
    HeapTuple partHeapTuple = NULL;
    HeapTuple ordHeapTuple = NULL;
    Form_pg_attribute partAttForm = NULL;
    Form_pg_attribute ordAttForm = NULL;
    int i = 0;
    Relation attrdefRel = NULL;

    // Get column list
    partAttList = SearchSysCacheList1(ATTNUM, ObjectIdGetDatum(partTableRel->rd_id));
    ordAttList = SearchSysCacheList1(ATTNUM, ObjectIdGetDatum(ordTableRel->rd_id));

    // Check column number
    if (partAttList->n_members != ordAttList->n_members) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("tables in ALTER TABLE EXCHANGE PARTITION must have the same number of columns")));
    }

    attrdefRel = heap_open(AttrDefaultRelationId, RowExclusiveLock);

    for (i = 0; i < partAttList->n_members; i++) {
        int j = 0;
        ScanKeyData scankeys[2];
        SysScanDesc partAttrdefScan = NULL;
        HeapTuple partAttrdefTuple = NULL;
        SysScanDesc ordAttrdefScan = NULL;
        HeapTuple ordAttrdefTuple = NULL;
        partHeapTuple = &partAttList->members[i]->tuple;
        partAttForm = (Form_pg_attribute)GETSTRUCT(partHeapTuple);
        for (j = 0; j < ordAttList->n_members; j++) {
            ordHeapTuple = &ordAttList->members[j]->tuple;
            ordAttForm = (Form_pg_attribute)GETSTRUCT(ordHeapTuple);
            if (ordAttForm->attnum == partAttForm->attnum) {
                break;
            }
        }

        // Check column name
        if (strcmp(NameStr(ordAttForm->attname), NameStr(partAttForm->attname)) != 0) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("column name mismatch in ALTER TABLE EXCHANGE PARTITION")));
        }

        // Check column type and length
        if (ordAttForm->atttypid != partAttForm->atttypid || ordAttForm->attlen != partAttForm->attlen ||
            ordAttForm->atttypmod != partAttForm->atttypmod) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("column type or size mismatch in ALTER TABLE EXCHANGE PARTITION")));
        }

        // Check column not null constraint
        if (ordAttForm->attnotnull != partAttForm->attnotnull) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("column not null constraint mismatch in ALTER TABLE EXCHANGE PARTITION")));
        }

        // Check column default constraint
        if (ordAttForm->atthasdef != partAttForm->atthasdef) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("column default constraint mismatch in ALTER TABLE EXCHANGE PARTITION")));
        }

        ScanKeyInit(&scankeys[0],
            Anum_pg_attrdef_adrelid,
            BTEqualStrategyNumber,
            F_OIDEQ,
            ObjectIdGetDatum(partTableRel->rd_id));
        ScanKeyInit(
            &scankeys[1], Anum_pg_attrdef_adnum, BTEqualStrategyNumber, F_INT2EQ, Int16GetDatum(partAttForm->attnum));

        partAttrdefScan = systable_beginscan(attrdefRel, AttrDefaultIndexId, true, SnapshotNow, 2, scankeys);
        partAttrdefTuple = systable_getnext(partAttrdefScan);

        ScanKeyInit(&scankeys[0],
            Anum_pg_attrdef_adrelid,
            BTEqualStrategyNumber,
            F_OIDEQ,
            ObjectIdGetDatum(ordTableRel->rd_id));
        ScanKeyInit(
            &scankeys[1], Anum_pg_attrdef_adnum, BTEqualStrategyNumber, F_INT2EQ, Int16GetDatum(ordAttForm->attnum));

        ordAttrdefScan = systable_beginscan(attrdefRel, AttrDefaultIndexId, true, SnapshotNow, 2, scankeys);
        ordAttrdefTuple = systable_getnext(ordAttrdefScan);

        if ((partAttrdefTuple != NULL) && (ordAttrdefTuple != NULL)) {
            bool isnull = false;
            Datum partAdsrc = (Datum)0;
            Datum ordAdsrc = (Datum)0;

            partAdsrc = heap_getattr(partAttrdefTuple, Anum_pg_attrdef_adsrc, attrdefRel->rd_att, &isnull);
            ordAdsrc = heap_getattr(ordAttrdefTuple, Anum_pg_attrdef_adsrc, attrdefRel->rd_att, &isnull);

            if (strcmp(TextDatumGetCString(partAdsrc), TextDatumGetCString(ordAdsrc)) != 0) {
                systable_endscan(partAttrdefScan);
                systable_endscan(ordAttrdefScan);

                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("column default constraint mismatch in ALTER TABLE EXCHANGE PARTITION")));
            }
        }

        systable_endscan(partAttrdefScan);
        systable_endscan(ordAttrdefScan);

        // Check column collation
        if (ordAttForm->attcollation != partAttForm->attcollation) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("column collation mismatch in ALTER TABLE EXCHANGE PARTITION")));
        }
        // Check column storage
        if (ordAttForm->attstorage != partAttForm->attstorage) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("column storage mismatch in ALTER TABLE EXCHANGE PARTITION")));
        }

        // Check the type of column compress
        if (ordAttForm->attcmprmode != partAttForm->attcmprmode) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("the type of column compress mismatch in ALTER TABLE EXCHANGE PARTITION")));
        }
        if (ordAttForm->attkvtype != partAttForm->attkvtype) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("the kv storage type of column mismatch in ALTER TABLE EXCHANGE PARTITION")));
        }
    }

    heap_close(attrdefRel, RowExclusiveLock);

    ReleaseCatCacheList(partAttList);
    ReleaseCatCacheList(ordAttList);
}

bool checkPartitionLocalIndexesUsable(Oid partitionOid)
{
    bool ret = true;
    Relation pg_part_rel = NULL;
    ScanKeyData scankeys[1];
    SysScanDesc partScan = NULL;
    HeapTuple partTuple = NULL;

    pg_part_rel = relation_open(PartitionRelationId, AccessShareLock);
    ScanKeyInit(
        &scankeys[0], Anum_pg_partition_indextblid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(partitionOid));
    partScan = systable_beginscan(pg_part_rel, PartitionIndexTableIdIndexId, true, SnapshotNow, 1, scankeys);
    while ((partTuple = systable_getnext(partScan)) != NULL) {
        Relation indexRel = NULL;
        Partition indexPart = NULL;
        Oid indexRelOid = ((Form_pg_partition)GETSTRUCT(partTuple))->parentid;
        Oid indexPartOid = HeapTupleGetOid(partTuple);

        // the index relation is already locked by upper caller function
        indexRel = index_open(indexRelOid, NoLock);
        // we will keep the lock on index partition.
        indexPart = partitionOpen(indexRel, indexPartOid, ExclusiveLock);
        if (!indexPart->pd_part->indisusable) {
            partitionClose(indexRel, indexPart, NoLock);
            index_close(indexRel, NoLock);
            ret = false;
            break;
        } else {
            partitionClose(indexRel, indexPart, NoLock);
            index_close(indexRel, NoLock);
            continue;
        }
    }

    systable_endscan(partScan);
    relation_close(pg_part_rel, AccessShareLock);
    return ret;
}

/*
 * @Description: check whether the partitioned relation has usable index.
 * @in relation: the partitioned relation.
 * @return: whether the partitioned relation has usable index.
 * notes: the invoker must check relation is partitioned first.
 */
bool checkRelationLocalIndexesUsable(Relation relation)
{
    bool ret = true;
    Relation indrel;
    SysScanDesc indscan;
    ScanKeyData skey;
    HeapTuple htup;

    /* Prepare to scan pg_index for entries having indrelid = this rel. */
    ScanKeyInit(
        &skey, Anum_pg_index_indrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(relation)));

    indrel = heap_open(IndexRelationId, AccessShareLock);
    indscan = systable_beginscan(indrel, IndexIndrelidIndexId, true, SnapshotNow, 1, &skey);

    while (HeapTupleIsValid(htup = systable_getnext(indscan))) {
        Form_pg_index index = (Form_pg_index)GETSTRUCT(htup);

        if (!IndexIsUsable(index)) {
            ret = false;
            break;
        }
    }

    systable_endscan(indscan);
    heap_close(indrel, AccessShareLock);

    return ret;
}

// Description : Check constraint of two tables
static void checkConstraintForExchange(Relation partTableRel, Relation ordTableRel)
{
    List* partConList = NIL;
    List* ordConList = NIL;
    Relation pgConstraint = NULL;
    ListCell* partCell = NULL;
    ListCell* ordCell = NULL;

    // Get constraint list
    partConList = getConstraintList(partTableRel->rd_id);
    ordConList = getConstraintList(ordTableRel->rd_id);

    // Check constraint number
    if (list_length(partConList) != list_length(ordConList)) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("constraint mismatch in ALTER TABLE EXCHANGE PARTITION")));
    }

    pgConstraint = heap_open(ConstraintRelationId, AccessShareLock);

    foreach (partCell, partConList) {
        Form_pg_constraint partConForm = NULL;
        HeapTuple partConTuple = NULL;
        bool isNull = false;
        Datum partConDatum;
        ArrayType* partConKeyArr = NULL;
        int16* partConKeyAttNums = NULL;
        int partConKeyNum = 0;
        bool isMatch = false;

        partConTuple = (HeapTuple)lfirst(partCell);
        partConForm = (Form_pg_constraint)GETSTRUCT(partConTuple);

        // Get column number and column index
        // of the constriant on partitioned table
        partConDatum = heap_getattr(partConTuple, Anum_pg_constraint_conkey, RelationGetDescr(pgConstraint), &isNull);
        partConKeyArr = DatumGetArrayTypeP(partConDatum);
        partConKeyNum = ARR_DIMS(partConKeyArr)[0];
        partConKeyAttNums = (int16*)ARR_DATA_PTR(partConKeyArr);

        foreach (ordCell, ordConList) {
            Form_pg_constraint ordConForm = NULL;
            HeapTuple ordConTuple = NULL;
            Datum ordConDatum;
            ArrayType* ordConKeyArr = NULL;
            int16* ordConKeyAttNums = NULL;
            int ordConKeyNum = 0;
            int i = 0;

            isMatch = false;

            ordConTuple = (HeapTuple)lfirst(ordCell);
            ordConForm = (Form_pg_constraint)GETSTRUCT(ordConTuple);

            // get column number and column index
            // of the constriant on partitioned table
            ordConDatum = heap_getattr(ordConTuple, Anum_pg_constraint_conkey, RelationGetDescr(pgConstraint), &isNull);
            ordConKeyArr = DatumGetArrayTypeP(ordConDatum);
            ordConKeyNum = ARR_DIMS(ordConKeyArr)[0];
            ordConKeyAttNums = (int16*)ARR_DATA_PTR(ordConKeyArr);

            if ((ordConForm->contype == partConForm->contype) && (ordConKeyNum == partConKeyNum)) {
                // If the constriant is check constriant,
                // check the check expression
                if (ordConForm->contype == 'c') {
                    if (constraints_equivalent(ordConTuple, partConTuple, pgConstraint->rd_att)) {
                        isMatch = true;
                    }
                } else {
                    isMatch = true;

                    for (i = 0; i < ordConKeyNum; i++) {
                        if (ordConKeyAttNums[i] != partConKeyAttNums[i]) {
                            isMatch = false;
                            break;
                        }
                    }
                }

                if (isMatch) {
                    break;
                }
            }
        }

        if (!isMatch) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("constraint mismatch in ALTER TABLE EXCHANGE PARTITION")));
        }
    }

    heap_close(pgConstraint, AccessShareLock);

    freeConstraintList(partConList);
    freeConstraintList(ordConList);
}

// Description : Get constraint tuple list of table
static List* getConstraintList(Oid relOid, char conType)
{
    List* result = NIL;
    Relation pgConstraint = NULL;
    HeapTuple tuple = NULL;
    SysScanDesc scan = NULL;
    ScanKeyData skey[2];
    int nkeys;

    pgConstraint = heap_open(ConstraintRelationId, AccessShareLock);

    ScanKeyInit(&skey[0], Anum_pg_constraint_conrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relOid));
    if (conType != CONSTRAINT_INVALID) {
        ScanKeyInit(&skey[1], Anum_pg_constraint_contype, BTEqualStrategyNumber, F_CHAREQ, CharGetDatum(conType));
        nkeys = 2;
        scan = systable_beginscan(pgConstraint, ConstraintRelidIndexId, false, SnapshotNow, nkeys, skey);

    } else {
        nkeys = 1;
        scan = systable_beginscan(pgConstraint, ConstraintRelidIndexId, true, SnapshotNow, nkeys, skey);
    }

    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        HeapTuple resultTuple = heap_copytuple(tuple);
        result = lappend(result, resultTuple);
    }

    systable_endscan(scan);
    heap_close(pgConstraint, AccessShareLock);

    return result;
}

// Description : Free constraint tuple list of table
static void freeConstraintList(List* list)
{
    ListCell* cell = NULL;
    HeapTuple tuple = NULL;

    foreach (cell, list) {
        tuple = (HeapTuple)lfirst(cell);
        if (HeapTupleIsValid(tuple)) {
            heap_freetuple_ext(tuple);
        }
    }

    list_free_ext(list);
}

static bool colHasPartialClusterKey(Relation rel, AttrNumber attNum)
{
    List* constraintList = getConstraintList(RelationGetRelid(rel), CONSTRAINT_CLUSTER);
    bool colHasCluster = false;
    ListCell* lc = NULL;
    Relation pgConstraint = NULL;
    bool isNull = false;
    Datum conKeyDatum;

    ArrayType* conKeyArr = NULL;
    int16* conKeyAttNums = NULL;
    int conKeyNum = 0;
    int i = 0;

    pgConstraint = heap_open(ConstraintRelationId, AccessShareLock);

    foreach (lc, constraintList) {
        HeapTuple constrTuple = (HeapTuple)lfirst(lc);

        conKeyDatum = heap_getattr(constrTuple, Anum_pg_constraint_conkey, RelationGetDescr(pgConstraint), &isNull);
        conKeyArr = DatumGetArrayTypeP(conKeyDatum);
        conKeyNum = ARR_DIMS(conKeyArr)[0];
        conKeyAttNums = (int16*)ARR_DATA_PTR(conKeyArr);

        for (i = 0; i < conKeyNum; i++) {
            if (attNum == conKeyAttNums[i]) {
                colHasCluster = true;
                break;
            }
        }
    }

    heap_close(pgConstraint, AccessShareLock);

    freeConstraintList(constraintList);

    return colHasCluster;
}

// Description : Check distribute attribute of two tables
static void checkDistributeForExchange(Relation partTableRel, Relation ordTableRel)
{
    Relation pgxcClass = NULL;
    HeapTuple partTuple = NULL;
    HeapTuple ordTuple = NULL;
    Form_pgxc_class partForm = NULL;
    Form_pgxc_class ordForm = NULL;
    bool isMatch = false;
    oidvector* partNodes = NULL;
    oidvector* ordNodes = NULL;
    bool isNull = false;

    pgxcClass = heap_open(PgxcClassRelationId, RowExclusiveLock);

    partTuple = SearchSysCache1(PGXCCLASSRELID, ObjectIdGetDatum(partTableRel->rd_id));
    if (!HeapTupleIsValid(partTuple))
        ereport(ERROR,
            ((errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for relaton %u", partTableRel->rd_id))));

    Datum part_nodes_datum = heap_getattr(partTuple, Anum_pgxc_class_nodes, RelationGetDescr(pgxcClass), &isNull);

    if (isNull)
        elog(PANIC, "Can't get nodeoid for relation %s", RelationGetRelationName(partTableRel));
    partNodes = (oidvector*)PG_DETOAST_DATUM(part_nodes_datum);

    partForm = (Form_pgxc_class)GETSTRUCT(partTuple);

    /* Get ord information */
    ordTuple = SearchSysCache1(PGXCCLASSRELID, ObjectIdGetDatum(ordTableRel->rd_id));

    Datum ordnodes_datum = heap_getattr(ordTuple, Anum_pgxc_class_nodes, RelationGetDescr(pgxcClass), &isNull);
    if (isNull)
        ereport(PANIC,
            ((errcode(ERRCODE_UNEXPECTED_NULL_VALUE),
                errmsg("Can't get nodeoid for relation %s", RelationGetRelationName(ordTableRel)))));

    ordNodes = (oidvector*)PG_DETOAST_DATUM(ordnodes_datum);

    ordForm = (Form_pgxc_class)GETSTRUCT(ordTuple);

    if (partForm->pclocatortype == ordForm->pclocatortype && partForm->pchashalgorithm == ordForm->pchashalgorithm &&
        partNodes->dim1 == ordNodes->dim1 &&
        DatumGetBool(DirectFunctionCall2(
            int2vectoreq, PointerGetDatum(&partForm->pcattnum), PointerGetDatum(&ordForm->pcattnum)))) {
        int i = 0;

        isMatch = true;

        for (i = 0; i < partNodes->dim1; i++) {
            if (partNodes->values[i] != ordNodes->values[i]) {
                isMatch = false;
                break;
            }
        }
    }

    if ((oidvector*)DatumGetPointer(part_nodes_datum) != partNodes)
        pfree_ext(partNodes);

    if ((oidvector*)DatumGetPointer(ordnodes_datum) != ordNodes)
        pfree_ext(ordNodes);

    if (!isMatch) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("distribute mismatch for tables in ALTER TABLE EXCHANGE PARTITION")));
    }

    ReleaseSysCache(partTuple);
    ReleaseSysCache(ordTuple);

    heap_close(pgxcClass, RowExclusiveLock);
}

// Description : Check index of two tables
static void checkIndexForExchange(
    Relation partTableRel, Oid partOid, Relation ordTableRel, List** partIndexList, List** ordIndexList)
{
    ListCell* oidCell = NULL;
    ListCell* tupleCell = NULL;
    List* partTableIndexOidList = NIL;
    List* ordTableIndexOidList = NIL;
    HeapTuple ordTableIndexTuple = NULL;
    List* ordTableIndexTupleList = NIL;
    bool* matchFlag = NULL;
    partTableIndexOidList = RelationGetIndexList(partTableRel);
    ordTableIndexOidList = RelationGetIndexList(ordTableRel);
    if (list_length(partTableIndexOidList) == 0 && list_length(ordTableIndexOidList) == 0) {
        return;
    }
    if (list_length(partTableIndexOidList) != list_length(ordTableIndexOidList)) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("tables in ALTER TABLE EXCHANGE PARTITION must have the same number of indexs")));
    }
    foreach (oidCell, ordTableIndexOidList) {
        Oid ordTableIndexOid = lfirst_oid(oidCell);
        ordTableIndexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(ordTableIndexOid));
        if (!HeapTupleIsValid(ordTableIndexTuple)) {
            ereport(ERROR,
                (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for index %u", ordTableIndexOid)));
        }
        ordTableIndexTupleList = lappend(ordTableIndexTupleList, ordTableIndexTuple);
    }

    matchFlag = (bool*)palloc0(list_length(partTableIndexOidList) * sizeof(bool));

    foreach (oidCell, partTableIndexOidList) {
        Oid partTableIndexOid = lfirst_oid(oidCell);
        Oid partIndexOid = InvalidOid;
        HeapTuple indexPartTuple = NULL;
        Form_pg_partition indexPartForm = NULL;
        HeapTuple partTalbeIndexTuple = NULL;
        Form_pg_index partTalbeIndexForm = NULL;
        Form_pg_index ordTalbeIndexForm = NULL;
        bool isMatch = false;
        bool partTableIndexPartUsable = true;
        int matchFlagIndex = -1;
        // step 1: check index 'indisusable' state
        partTalbeIndexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(partTableIndexOid));
        partTalbeIndexForm = (Form_pg_index)GETSTRUCT(partTalbeIndexTuple);
        if (!IndexIsValid(partTalbeIndexForm)) {
            partTableIndexPartUsable = false;
        }
        partIndexOid = getPartitionIndexOid(partTableIndexOid, partOid);
        // step 2: check index partition 'indisusable' state
        if (partTableIndexPartUsable) {
            indexPartTuple = SearchSysCache1(PARTRELID, ObjectIdGetDatum(partIndexOid));
            indexPartForm = (Form_pg_partition)GETSTRUCT(indexPartTuple);
            if (!indexPartForm->indisusable) {
                partTableIndexPartUsable = false;
            }
            ReleaseSysCache(indexPartTuple);
        }
        // step 3: check whether 2 index are matching
        foreach (tupleCell, ordTableIndexTupleList) {
            Oid ordTableIndexOid = InvalidOid;
            HeapTuple partTableIndexClassTuple = NULL;
            HeapTuple ordTableIndexClassTuple = NULL;
            Form_pg_class partTableIndexClassForm = NULL;
            Form_pg_class ordTableIndexClassForm = NULL;
            bool ordTableIndexUsable = true;
            isMatch = false;
            matchFlagIndex++;
            if (matchFlag[matchFlagIndex]) {
                continue;
            }
            ordTableIndexTuple = (HeapTuple)lfirst(tupleCell);
            ordTalbeIndexForm = (Form_pg_index)GETSTRUCT(ordTableIndexTuple);

            ordTableIndexOid = ordTalbeIndexForm->indexrelid;
            if (!IndexIsValid(ordTalbeIndexForm)) {
                ordTableIndexUsable = false;
            }
            if (partTableIndexPartUsable != ordTableIndexUsable) {
                continue;
            }
            partTableIndexClassTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(partTableIndexOid));
            ordTableIndexClassTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(ordTableIndexOid));
            partTableIndexClassForm = (Form_pg_class)GETSTRUCT(partTableIndexClassTuple);
            ordTableIndexClassForm = (Form_pg_class)GETSTRUCT(ordTableIndexClassTuple);

            // index access method
            if (partTableIndexClassForm->relam != ordTableIndexClassForm->relam) {
                ReleaseSysCache(partTableIndexClassTuple);
                ReleaseSysCache(ordTableIndexClassTuple);
                continue;
            }
            ReleaseSysCache(partTableIndexClassTuple);
            ReleaseSysCache(ordTableIndexClassTuple);
            if ((ordTalbeIndexForm->indnatts == partTalbeIndexForm->indnatts) &&
                (ordTalbeIndexForm->indisunique == partTalbeIndexForm->indisunique) &&
                (ordTalbeIndexForm->indisprimary == partTalbeIndexForm->indisprimary)) {
                int i = 0;
                isMatch = true;
                for (i = 0; i < partTalbeIndexForm->indkey.dim1; i++) {
                    if (ordTalbeIndexForm->indkey.values[i] != partTalbeIndexForm->indkey.values[i]) {
                        isMatch = false;
                        break;
                    }
                }
            }
            if (isMatch) {
                *partIndexList = lappend_oid(*partIndexList, partIndexOid);
                *ordIndexList = lappend_oid(*ordIndexList, ordTableIndexOid);
                matchFlag[matchFlagIndex] = true;
                break;
            }
        }
        ReleaseSysCache(partTalbeIndexTuple);
        if (!isMatch) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("index mismatch for tables in ALTER TABLE EXCHANGE PARTITION")));
        }
    }
    foreach (tupleCell, ordTableIndexTupleList) {
        ordTableIndexTuple = (HeapTuple)lfirst(tupleCell);
        ReleaseSysCache(ordTableIndexTuple);
    }
    list_free_ext(partTableIndexOidList);
    list_free_ext(ordTableIndexOidList);
    list_free_ext(ordTableIndexTupleList);
    pfree_ext(matchFlag);
}

// Description : Check all tuples of ordinary whether locate the partition
template <bool exchangeVerbose>
static void checkValidationForExchangeTable(Relation partTableRel, Relation ordTableRel, int partSeq)
{
    AbsTblScanDesc scan = NULL;
    HeapTuple tuple = NULL;
    TupleDesc tupleDesc = NULL;
    HTAB* partRelHTAB = NULL;
    List* indexList = NIL;
    List* indexRelList = NIL;
    List* indexInfoList = NIL;
    ListCell* cell = NULL;
    ListCell* cell1 = NULL;
    EState* estate = NULL;
    TupleTableSlot* slot = NULL;

    tupleDesc = ordTableRel->rd_att;

    if (exchangeVerbose) {
        indexList = RelationGetIndexList(partTableRel);

        foreach (cell, indexList) {
            Oid indexOid = lfirst_oid(cell);
            Relation indexRel = relation_open(indexOid, RowExclusiveLock);
            IndexInfo* indexInfo = BuildIndexInfo(indexRel);

            indexRelList = lappend(indexRelList, indexRel);
            indexInfoList = lappend(indexInfoList, indexInfo);
        }

        if (PointerIsValid(indexRelList)) {
            estate = CreateExecutorState();
            slot = MakeSingleTupleTableSlot(RelationGetDescr(partTableRel));
        }
    }

    scan = abs_tbl_beginscan(ordTableRel, SnapshotNow, 0, NULL);
    while ((tuple = abs_tbl_getnext(scan, ForwardScanDirection)) != NULL) {
        if (!tupleLocateThePartition(partTableRel, partSeq, tupleDesc, tuple)) {
            if (!exchangeVerbose) {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("some rows in table do not qualify for specified partition")));
            } else {
                Oid targetPartOid = InvalidOid;
                Relation partRel = NULL;
                Partition part = NULL;
                HeapTuple copyTuple = NULL;
                int2 bucketId = InvalidBktId;

                // get right partition oid for the tuple
                targetPartOid = heapTupleGetPartitionId(partTableRel, tuple);
                searchFakeReationForPartitionOid(
                    partRelHTAB, CurrentMemoryContext, partTableRel, targetPartOid, partRel, part, RowExclusiveLock);

                if (RELATION_HAS_BUCKET(partTableRel)) {
                    // Get the target bucket.
                    bucketId = computeTupleBucketId(partTableRel, tuple);
                    searchHBucketFakeRelation(partRelHTAB, CurrentMemoryContext, partRel, bucketId, partRel);
                }

                copyTuple = heap_copytuple(tuple);

                // insert the tuple into right partition
                (void)simple_heap_insert(partRel, copyTuple);

                // insert the index tuple
                if (PointerIsValid(indexRelList)) {
                    (void)ExecStoreTuple(copyTuple, slot, InvalidBuffer, false);
                }

                forboth(cell, indexRelList, cell1, indexInfoList)
                {
                    Relation indexRel = (Relation)lfirst(cell);
                    IndexInfo* indexInfo = (IndexInfo*)lfirst(cell1);
                    Oid indexOid = RelationGetRelid(indexRel);
                    Oid partIndexOid = InvalidOid;
                    Relation partIndexRel = NULL;
                    Partition partIndex = NULL;

                    Datum values[tupleDesc->natts];
                    bool isNull[tupleDesc->natts];
                    bool estateIsNotNull = false;

                    partIndexOid = getPartitionIndexOid(indexOid, targetPartOid);
                    searchFakeReationForPartitionOid(partRelHTAB,
                        CurrentMemoryContext,
                        indexRel,
                        partIndexOid,
                        partIndexRel,
                        partIndex,
                        RowExclusiveLock);

                    if (RELATION_HAS_BUCKET(indexRel)) {
                        searchHBucketFakeRelation(
                            partRelHTAB, CurrentMemoryContext, partIndexRel, bucketId, partIndexRel);
                    }

                    if (indexInfo->ii_Expressions != NIL || indexInfo->ii_ExclusionOps != NULL) {
                        ExprContext* econtext = GetPerTupleExprContext(estate);
                        econtext->ecxt_scantuple = slot;
                        estateIsNotNull = true;
                    }

                    FormIndexDatum(indexInfo, slot, estateIsNotNull ? estate : NULL, values, isNull);

                    (void)index_insert(partIndexRel,
                        values,
                        isNull,
                        &(copyTuple->t_self),
                        partRel,
                        indexRel->rd_index->indisunique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO);
                }

                heap_freetuple_ext(copyTuple);

                // delete the tuple from ordinary table
                simple_heap_delete(GetHeapScanDesc(scan)->rs_rd, &tuple->t_self);
            }
        }
    }
    abs_tbl_endscan(scan);

    if (PointerIsValid(partRelHTAB)) {
        FakeRelationCacheDestroy(partRelHTAB);
    }

    if (exchangeVerbose) {
        foreach (cell, indexRelList) {
            Relation indexRel = (Relation)lfirst(cell);

            relation_close(indexRel, RowExclusiveLock);
        }

        list_free_ext(indexRelList);
        list_free_ext(indexInfoList);

        if (PointerIsValid(estate)) {
            FreeExecutorState(estate);
        }

        if (PointerIsValid(slot)) {
            ExecDropSingleTupleTableSlot(slot);
        }
    }
}

template <bool exchangeVerbose>
static void checkValidationForExchangeCStore(Relation partTableRel, Relation ordTableRel, int partSeq)
{
    RangePartitionMap* partMap = (RangePartitionMap*)(partTableRel->partMap);
    int2vector* partkeyColumns = partMap->partitionKey;
    int partkeyColumnNum = partkeyColumns->dim1;

    AttrNumber* scanAttrNumbers = NULL;
    int scanColNum = 0;

    CStoreScanDesc scanstate;
    VectorBatch* vecScanBatch = NULL;
    ScalarVector* pVec = NULL;
    ScalarValue* pVals = NULL;
    Datum* values = NULL;
    bool* nulls = NULL;
    Form_pg_attribute* attrs = ordTableRel->rd_att->attrs;

    Const consts[RANGE_PARTKEYMAXNUM];
    Const* partKeyValues[RANGE_PARTKEYMAXNUM];
    bool isInPart = false;

    const int tididx = 1;       // junk column for cstore delete
    const int tableoidIdx = 2;  // junk column(tableoid) for cstore delete
    int countNotInPart = 0;
    EState* estate = NULL;
    VectorBatch* pBatchNotInPart = NULL;
    VectorBatch* pBatchForDelete = NULL;
    ScalarVector* pValNotInPart = NULL;
    CStoreDelete* ordTabelDelete = NULL;
    ResultRelInfo* resultRelInfo = NULL;
    CStorePartitionInsert* partionInsert = NULL;

    // perpare for cstore scan
    if (exchangeVerbose) {
        // use all columns and ctid for scan key
        scanColNum = ordTableRel->rd_att->natts + 2;
        scanAttrNumbers = (AttrNumber*)palloc(sizeof(AttrNumber) * scanColNum);

        for (int i = 0; i < (scanColNum - 2); i++) {
            scanAttrNumbers[i] = attrs[i]->attnum;
        }

        // ctid for delete
        scanAttrNumbers[scanColNum - 2] = SelfItemPointerAttributeNumber;
        scanAttrNumbers[scanColNum - 1] = TableOidAttributeNumber;

        // init cstore partition insert
        resultRelInfo = makeNode(ResultRelInfo);
        InitResultRelInfo(resultRelInfo, partTableRel, 1, 0);
        ExecOpenIndices(resultRelInfo);
        resultRelInfo->ri_junkFilter = makeNode(JunkFilter);
        resultRelInfo->ri_junkFilter->jf_junkAttNo = tididx;
        resultRelInfo->ri_junkFilter->jf_xc_part_id = tableoidIdx;
        partionInsert =
            New(CurrentMemoryContext) CStorePartitionInsert(partTableRel, resultRelInfo, TUPLE_SORT, false, NULL, NULL);

        // init  cstore delete
        estate = CreateExecutorState();
        ordTabelDelete = New(CurrentMemoryContext) CStoreDelete(ordTableRel, estate, false, NULL, NULL);
        ordTabelDelete->InitSortState();
    } else {
        // use parition key for scan key
        scanColNum = partkeyColumnNum;
        scanAttrNumbers = (AttrNumber*)palloc(sizeof(AttrNumber) * scanColNum);

        for (int i = 0; i < scanColNum; i++) {
            scanAttrNumbers[i] = partkeyColumns->values[i];
        }
    }

    // datum values for scan partition key values
    values = (Datum*)palloc(sizeof(Datum) * partkeyColumnNum);
    nulls = (bool*)palloc(sizeof(bool) * partkeyColumnNum);

    // scan columnar table
    scanstate = CStoreBeginScan(ordTableRel, scanColNum, scanAttrNumbers, SnapshotNow, true);

    if (exchangeVerbose) {
        // perpare move data which not in exchange partition
        pBatchNotInPart = New(CurrentMemoryContext) VectorBatch(CurrentMemoryContext, scanstate->m_pScanBatch);
        TupleDesc tupdesc = CreateTemplateTupleDesc(2, false);
        TupleDescInitEntry(tupdesc, (AttrNumber)1, "ctid", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)2, "tableoid", INT8OID, -1, 0);
        pBatchForDelete = New(CurrentMemoryContext) VectorBatch(CurrentMemoryContext, tupdesc);
        pBatchNotInPart->CreateSysColContainer(CurrentMemoryContext, scanstate->ps.ps_ProjInfo->pi_sysAttrList);
    }

    do {
        vecScanBatch = CStoreGetNextBatch(scanstate);
        if (!BatchIsNull(vecScanBatch)) {
            for (int row = 0; row < vecScanBatch->m_rows; row++) {
                for (int partkeyIdx = 0; partkeyIdx < partkeyColumnNum; partkeyIdx++) {
                    // transform VectorBatch to partition key values
                    int col = partkeyColumns->values[partkeyIdx] - 1;
                    pVec = vecScanBatch->m_arr + col;
                    if (pVec->IsNull(row)) {
                        nulls[partkeyIdx] = true;
                        values[partkeyIdx] = (Datum)0;
                    } else {
                        nulls[partkeyIdx] = false;
                        pVals = pVec->m_vals;
                        if (pVec->m_desc.encoded == false)
                            values[partkeyIdx] = pVals[row];
                        else {
                            Assert(attrs[col]->attlen < 0 || attrs[col]->attlen > 8);
                            Datum v = ScalarVector::Decode(pVals[row]);
                            values[partkeyIdx] =
                                (attrs[col]->attlen < 0) ? v : PointerGetDatum((char*)v + VARHDRSZ_SHORT);
                        }
                    }

                    partKeyValues[partkeyIdx] = transformDatum2Const(partTableRel->rd_att,
                        partkeyColumns->values[partkeyIdx],
                        values[partkeyIdx],
                        nulls[partkeyIdx],
                        &consts[partkeyIdx]);
                }

                // is this row in the exchange partition
                isInPart = isPartKeyValuesInPartition(partMap, partKeyValues, partkeyColumnNum, partSeq);

                if (!isInPart) {
                    if (!exchangeVerbose) {
                        ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                errmsg("some rows in table do not qualify for specified partition")));
                    } else {
                        // all column
                        for (int col = 0; col < vecScanBatch->m_cols; col++) {
                            pVec = vecScanBatch->m_arr + col;
                            pValNotInPart = pBatchNotInPart->m_arr + col;

                            //  shallow copy
                            pValNotInPart->m_vals[countNotInPart] = pVec->m_vals[row];
                            pValNotInPart->m_flag[countNotInPart] = pVec->m_flag[row];
                            pValNotInPart->m_rows++;
                        }

                        // ctid
                        pVec = vecScanBatch->GetSysVector(SelfItemPointerAttributeNumber);
                        pValNotInPart = pBatchNotInPart->GetSysVector(SelfItemPointerAttributeNumber);
                        pValNotInPart->m_vals[countNotInPart] = pVec->m_vals[row];
                        pValNotInPart->m_flag[countNotInPart] = pVec->m_flag[row];
                        pValNotInPart->m_rows++;

                        // tableoid
                        pVec = vecScanBatch->GetSysVector(TableOidAttributeNumber);
                        pValNotInPart = pBatchNotInPart->GetSysVector(TableOidAttributeNumber);
                        pValNotInPart->m_vals[countNotInPart] = pVec->m_vals[row];
                        pValNotInPart->m_flag[countNotInPart] = pVec->m_flag[row];
                        pValNotInPart->m_rows++;

                        countNotInPart++;
                    }
                }
            }

            if (exchangeVerbose && (countNotInPart != 0)) {
                // routing to the right partition and insert
                Assert(countNotInPart <= vecScanBatch->m_rows);
                pBatchNotInPart->m_rows = countNotInPart;

                // insert to right partition
                partionInsert->BatchInsert(pBatchNotInPart, HEAP_INSERT_FROZEN);

                // delete from old table
                pBatchForDelete->m_rows = countNotInPart;
                pBatchForDelete->m_arr[tididx - 1].copy(pBatchNotInPart->GetSysVector(SelfItemPointerAttributeNumber));
                pBatchForDelete->m_arr[tableoidIdx - 1].copy(pBatchNotInPart->GetSysVector(TableOidAttributeNumber));
                ordTabelDelete->PutDeleteBatch(pBatchForDelete, resultRelInfo->ri_junkFilter);

                // reset batch and count
                pBatchNotInPart->Reset();
                pBatchForDelete->Reset();
                countNotInPart = 0;
            }
        }
    } while (!CStoreIsEndScan(scanstate));

    CStoreEndScan(scanstate);

    if (exchangeVerbose) {
        partionInsert->EndBatchInsert();
        DELETE_EX(partionInsert);

        (void)ordTabelDelete->ExecDelete();
        DELETE_EX(ordTabelDelete);

        ExecCloseIndices(resultRelInfo);
    }

    pfree_ext(scanAttrNumbers);
    pfree_ext(values);
    pfree_ext(nulls);
}

static void checkValidationForExchange(Relation partTableRel, Relation ordTableRel, Oid partOid, bool exchangeVerbose)
{
    Assert(partTableRel && partTableRel->partMap);

    RangePartitionMap* partMap = (RangePartitionMap*)(partTableRel->partMap);
    int partSeq = 0;

    // the premise is that partitioned table was locked by AccessExclusiveLock
    for (int i = 0; i < partMap->rangeElementsNum; i++) {
        if (partOid == partMap->rangeElements[i].partitionOid) {
            partSeq = i;
            break;
        }
    }

    if (RelationIsColStore(ordTableRel)) {
        if (exchangeVerbose)
            checkValidationForExchangeCStore<true>(partTableRel, ordTableRel, partSeq);
        else
            checkValidationForExchangeCStore<false>(partTableRel, ordTableRel, partSeq);
    } else {
        if (exchangeVerbose)
            checkValidationForExchangeTable<true>(partTableRel, ordTableRel, partSeq);
        else
            checkValidationForExchangeTable<false>(partTableRel, ordTableRel, partSeq);
    }
}

// Description : Get partition oid by name or partition key values
static Oid getPartitionOid(Relation partTableRel, const char* partName, RangePartitionDefState* rangePartDef)
{
    Oid partOid = InvalidOid;

    if (PointerIsValid(partName)) {
        partOid = partitionNameGetPartitionOid(RelationGetRelid(partTableRel),
            partName,
            PART_OBJ_TYPE_TABLE_PARTITION,
            AccessExclusiveLock,
            true,
            false,
            NULL,
            NULL,
            NoLock);
    } else {
        rangePartDef->boundary = transformConstIntoTargetType(partTableRel->rd_att->attrs,
            ((RangePartitionMap*)partTableRel->partMap)->partitionKey,
            rangePartDef->boundary);
        partOid = partitionValuesGetPartitionOid(
            partTableRel, rangePartDef->boundary, AccessExclusiveLock, true, true, false);
    }

    return partOid;
}

// Description : Swap relfilenode of index
static void finishIndexSwap(List* partIndexList, List* ordIndexList)
{
    ListCell* cell1 = NULL;
    ListCell* cell2 = NULL;

    forboth(cell1, partIndexList, cell2, ordIndexList)
    {
        Oid partOid, ordOid;
        partOid = (Oid)lfirst_oid(cell1);
        ordOid = (Oid)lfirst_oid(cell2);

        finishPartitionHeapSwap(partOid, ordOid, true, u_sess->utils_cxt.RecentGlobalXmin);
    }
}

static void ATExecSplitPartition(Relation partTableRel, AlterTableCmd* cmd)
{
    SplitPartitionState* splitPart = NULL;
    List* destPartDefList = NIL;
    RangePartitionMap* partMap = NULL;
    Oid partTableOid = InvalidOid;
    Oid srcPartOid = InvalidOid;
    int srcPartSeq = -1;
    ListCell* cell = NULL;
    int currentPartNum = 0;
    int partKeyNum = 0;
    List* newPartOidList = NIL;
    List* destPartBoundaryList = NIL;
    List* listForFree = NIL;

    Partition part = NULL;
    Oid tempTableOid = InvalidOid;
    Relation tempTableRel = NULL;
    ObjectAddress object;

    splitPart = (SplitPartitionState*)cmd->def;
    destPartDefList = splitPart->dest_partition_define_list;
    partMap = (RangePartitionMap*)partTableRel->partMap;
    partKeyNum = partMap->partitionKey->dim1;
    partTableOid = RelationGetRelid(partTableRel);

    // check final partition num
    currentPartNum = getNumberOfPartitions(partTableRel);
    if ((currentPartNum + list_length(destPartDefList) - 1) > MAX_PARTITION_NUM) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("too many partitions for partitioned table"),
                errhint("Number of partitions can not be more than %d", MAX_PARTITION_NUM)));
    }

    // get src partition oid
    if (PointerIsValid(splitPart->src_partition_name)) {
        srcPartOid = partitionNameGetPartitionOid(RelationGetRelid(partTableRel),
            splitPart->src_partition_name,
            PART_OBJ_TYPE_TABLE_PARTITION,
            AccessExclusiveLock,
            true,
            false,
            NULL,
            NULL,
            NoLock);
    } else {
        splitPart->partition_for_values = transformConstIntoTargetType(
            partTableRel->rd_att->attrs, partMap->partitionKey, splitPart->partition_for_values);
        srcPartOid = partitionValuesGetPartitionOid(
            partTableRel, splitPart->partition_for_values, AccessExclusiveLock, true, true, false);
    }

    /* check src partition exists */
    if (!OidIsValid(srcPartOid)) {
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_TABLE),
                splitPart->src_partition_name != NULL
                    ? errmsg("split partition \"%s\" does not exist.", splitPart->src_partition_name)
                    : errmsg("split partition does not exist.")));
    }

    /* check local index 'usable' state */
    if (!checkRelationLocalIndexesUsable(partTableRel)) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                errmsg("can't split partition bacause relation %s has unusable local index",
                    NameStr(partTableRel->rd_rel->relname)),
                errhint("please reindex the unusable index first.")));
    }

    // check dest partitions name not existing
    checkDestPartitionNameForSplit(partTableOid, destPartDefList);

    // check dest partition tablespace
    foreach (cell, destPartDefList) {
        RangePartitionDefState* rangePartDef = (RangePartitionDefState*)lfirst(cell);

        CheckPartitionTablespace(rangePartDef->tablespacename, partTableRel->rd_rel->relowner);
    }

    // get src partition sequence
    srcPartSeq = partOidGetPartSequence(partTableRel, srcPartOid);
    if (srcPartSeq < 1) {
        Assert(false);
        ereport(ERROR,
            (errcode(ERRCODE_NO_DATA_FOUND),
                errmsg("the partition oid(%u) of partition name (%s) is not found in partitioned table(%u).",
                    srcPartOid,
                    splitPart->src_partition_name ? splitPart->src_partition_name : "NULL",
                    partTableRel->rd_id)));
    } else {
        --srcPartSeq;
    }

    // if split point
    if (PointerIsValid(splitPart->split_point)) {
        RangePartitionDefState* rangePartDef = NULL;

        // check split point value
        checkSplitPointForSplit(splitPart, partTableRel, srcPartSeq);

        Assert(list_length(destPartDefList) == 2);

        // set the two dest partitions boundary
        rangePartDef = (RangePartitionDefState*)list_nth(destPartDefList, 0);
        rangePartDef->boundary = splitPart->split_point;

        /*
         * generate boundary for the second partititon
         */
        rangePartDef = (RangePartitionDefState*)list_nth(destPartDefList, 1);
        rangePartDef->boundary = getPartitionBoundaryList(partTableRel, srcPartSeq);
    } else {
        // not split point
        int compare = 0;
        ListCell* otherCell = NULL;

        if (list_length(destPartDefList) < 2) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_OPERATION),
                    errmsg("the number of resulting partitions must be more than one")));
        }

        // transform partition key type
        destPartBoundaryList = getDestPartBoundaryList(partTableRel, destPartDefList, &listForFree);

        // check the first dest partition boundary
        if (srcPartSeq != 0) {
            compare = comparePartitionKey(partMap,
                partMap->rangeElements[srcPartSeq - 1].boundary,
                (Const**)lfirst(list_head(destPartBoundaryList)),
                partKeyNum);
            if (compare >= 0) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_OPERATION),
                        errmsg("the bound of the first resulting partition is too low")));
            }
        }

        // check the dest partitions boundary
        forboth(cell, destPartBoundaryList, otherCell, destPartDefList)
        {
            Const** currentBoundary = (Const**)lfirst(cell);
            RangePartitionDefState* rangePartDef = (RangePartitionDefState*)lfirst(otherCell);
            Const** nextBoudary = NULL;

            if (!PointerIsValid(cell->next)) {
                break;
            }

            nextBoudary = (Const**)lfirst(cell->next);
            rangePartDef = (RangePartitionDefState*)lfirst(otherCell->next);

            compare = comparePartitionKey(partMap, currentBoundary, nextBoudary, partKeyNum);

            if (compare >= 0) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_OPERATION),
                        errmsg("the bound of resulting partition \"%s\" is too low", rangePartDef->partitionName)));
            }
        }

        // check the last dest partition boundary equal the src partition boundary
        compare = comparePartitionKey(partMap,
            (Const**)lfirst(list_tail(destPartBoundaryList)),
            partMap->rangeElements[srcPartSeq].boundary,
            partKeyNum);
        if (compare != 0) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_OPERATION),
                    errmsg("the bound of the last resulting partition is not equal with specified partition bound")));
        }
    }

    // add dest partitions
    fastAddPartition(partTableRel, destPartDefList, &newPartOidList);
    freeDestPartBoundaryList(destPartBoundaryList, listForFree);

#ifdef PGXC
    if (IS_PGXC_DATANODE) {
#endif
        part = partitionOpen(partTableRel, srcPartOid, AccessExclusiveLock);

        // creat temp table and swap relfilenode with src partition
        tempTableOid = createTempTableForPartition(partTableRel, part);
        finishPartitionHeapSwap(srcPartOid, tempTableOid, false, u_sess->utils_cxt.RecentXmin);

        CommandCounterIncrement();

        partitionClose(partTableRel, part, NoLock);
#ifdef PGXC
    }
#endif

    // drop src partition
    fastDropPartition(partTableRel, srcPartOid, "SPLIT PARTITION");

#ifdef PGXC
    if (IS_PGXC_DATANODE) {
#endif
        tempTableRel = relation_open(tempTableOid, AccessExclusiveLock);

        // read temp table tuples and insert into partitioned table
        readTuplesAndInsert(tempTableRel, partTableRel);

        relation_close(tempTableRel, NoLock);

        // delete temp table
        object.classId = RelationRelationId;
        object.objectId = tempTableOid;
        object.objectSubId = 0;
        performDeletion(&object, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

        // reindex dest partitions
        foreach (cell, newPartOidList) {
            Oid partOid = lfirst_oid(cell);

            reindexPartition(partTableOid, partOid, REINDEX_REL_SUPPRESS_INDEX_USE, REINDEX_ALL_INDEX);
        }
#ifdef PGXC
    }
#endif

    list_free_ext(newPartOidList);
}

// check split point
static void checkSplitPointForSplit(SplitPartitionState* splitPart, Relation partTableRel, int srcPartSeq)
{
    RangePartitionMap* partMap = NULL;
    ParseState* pstate = NULL;
    ListCell* cell = NULL;
    Const* partKeyValueArr[RANGE_PARTKEYMAXNUM] = {NULL};
    int i = 0;
    int partKeyNum = 0;
    int compareSrcPart = 0;
    int comparePrePart = 0;

    // get partition key number
    partMap = (RangePartitionMap*)partTableRel->partMap;
    partKeyNum = partMap->partitionKey->dim1;

    // check split point length
    if (partKeyNum != list_length(splitPart->split_point)) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_OPERATION),
                errmsg("number of boundary items NOT EQUAL to number of partition keys")));
    }

    pstate = make_parsestate(NULL);
    splitPart->split_point = transformRangePartitionValueInternal(pstate, splitPart->split_point, true, true);
    pfree_ext(pstate);

    splitPart->split_point =
        transformConstIntoTargetType(partTableRel->rd_att->attrs, partMap->partitionKey, splitPart->split_point);

    foreach (cell, splitPart->split_point) {
        partKeyValueArr[i++] = (Const*)lfirst(cell);
    }

    // compare splint point with src partition
    compareSrcPart =
        comparePartitionKey(partMap, partKeyValueArr, partMap->rangeElements[srcPartSeq].boundary, partKeyNum);

    // compare splint point with the previous partition of src partition
    if (srcPartSeq == 0) {
        comparePrePart = 1;
    } else {
        comparePrePart =
            comparePartitionKey(partMap, partKeyValueArr, partMap->rangeElements[srcPartSeq - 1].boundary, partKeyNum);
    }

    // splint point should be between the previous partition and the src partition
    if (comparePrePart <= 0) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("split point is too low")));
    }

    if (compareSrcPart >= 0) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("split point is too high")));
    }
}

static void checkDestPartitionNameForSplit(Oid partTableOid, List* partDefList)
{
    ListCell* cell = NULL;

    // check dest partitions name with self
    checkPartitionName(partDefList);

    // check dest partitions name with existing partitions name
    foreach (cell, partDefList) {
        RangePartitionDefState* rangePartDef = (RangePartitionDefState*)lfirst(cell);

        if (InvalidOid != GetSysCacheOid3(PARTPARTOID,
            NameGetDatum(rangePartDef->partitionName),
            CharGetDatum(PART_OBJ_TYPE_TABLE_PARTITION),
            ObjectIdGetDatum(partTableOid))) {
            ereport(ERROR,
                (errcode(ERRCODE_DUPLICATE_OBJECT),
                    errmsg("resulting partition \"%s\" name conflicts with that of an existing partition",
                        rangePartDef->partitionName)));
        }
    }
}

static List* getDestPartBoundaryList(Relation partTableRel, List* destPartDefList, List** listForFree)
{
    ListCell* cell = NULL;
    List* result = NIL;

    foreach (cell, destPartDefList) {
        RangePartitionDefState* rangePartDef = (RangePartitionDefState*)lfirst(cell);
        List* partKeyValueList = NIL;
        ListCell* otherCell = NULL;
        Const** partKeyValueArr = (Const**)palloc0(sizeof(Const*) * RANGE_PARTKEYMAXNUM);
        int i = 0;

        partKeyValueList = transformConstIntoTargetType(partTableRel->rd_att->attrs,
            ((RangePartitionMap*)partTableRel->partMap)->partitionKey,
            rangePartDef->boundary);

        foreach (otherCell, partKeyValueList) {
            partKeyValueArr[i++] = (Const*)lfirst(otherCell);
        }

        result = lappend(result, partKeyValueArr);
        *listForFree = lappend(*listForFree, partKeyValueList);
    }

    return result;
}

static void freeDestPartBoundaryList(List* list1, List* list2)
{
    ListCell* cell = NULL;

    foreach (cell, list1) {
        Const** partKeyValues = (Const**)lfirst(cell);

        pfree_ext(partKeyValues);
    }

    list_free_ext(list1);

    foreach (cell, list2) {
        List* partKeyList = (List*)lfirst(cell);

        list_free_deep(partKeyList);
    }

    list_free_ext(list2);
}

static void fastAddPartition(Relation partTableRel, List* destPartDefList, List** newPartOidList)
{
    Relation pgPartRel = NULL;
    Oid newPartOid = InvalidOid;
    ListCell* cell = NULL;
    Oid bucketOid;

    bool* isTimestamptz = check_partkey_has_timestampwithzone(partTableRel);

    bucketOid = RelationGetBucketOid(partTableRel);

    pgPartRel = relation_open(PartitionRelationId, RowExclusiveLock);

    foreach (cell, destPartDefList) {
        RangePartitionDefState* partDef = (RangePartitionDefState*)lfirst(cell);

        newPartOid = heapAddRangePartition(pgPartRel,
            partTableRel->rd_id,
            InvalidOid,
            partTableRel->rd_rel->reltablespace,
            bucketOid,
            partDef,
            partTableRel->rd_rel->relowner,
            (Datum)0,
            isTimestamptz);

        *newPartOidList = lappend_oid(*newPartOidList, newPartOid);

        // We must bump the command counter to make the newly-created
        // partition tuple visible for opening.
        CommandCounterIncrement();

        addIndexForPartition(partTableRel, newPartOid);

        addToastTableForNewPartition(partTableRel, newPartOid);

        // invalidate relation
        CacheInvalidateRelcache(partTableRel);
    }

    pfree_ext(isTimestamptz);
    relation_close(pgPartRel, NoLock);
}

static Oid createTempTableForPartition(Relation partTableRel, Partition part)
{
    TupleDesc partTableHeapDesc;
    Datum partTableRelOptions = 0;
    HeapTuple tuple = NULL;
    bool isNull = false;
    Oid tempTableOid = InvalidOid;

    partTableHeapDesc = RelationGetDescr(partTableRel);

    /* get reloptions */
    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(RelationGetRelid(partTableRel)));
    if (!HeapTupleIsValid(tuple)) {
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for relation %u", RelationGetRelid(partTableRel))));
    }
    partTableRelOptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isNull);
    if (isNull) {
        partTableRelOptions = (Datum)0;
    }

    // create temp table
    tempTableOid = makePartitionNewHeap(partTableRel,
        partTableHeapDesc,
        partTableRelOptions,
        part->pd_id,
        part->pd_part->reltoastrelid,
        part->pd_part->reltablespace);

    ReleaseSysCache(tuple);

    return tempTableOid;
}

static void readTuplesAndInsertInternal(Relation tempTableRel, Relation partTableRel, int2 bucketId)
{
    HeapScanDesc scan = NULL;
    HeapTuple tuple = NULL;
    HTAB* partRelHTAB = NULL;

    scan = heap_beginscan(tempTableRel, SnapshotNow, 0, NULL);

    while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
        Oid targetPartOid = InvalidOid;
        Relation partRel = NULL;
        Partition part = NULL;
        HeapTuple copyTuple = NULL;

        copyTuple = heap_copytuple(tuple);

        targetPartOid = heapTupleGetPartitionId(partTableRel, tuple);
        searchFakeReationForPartitionOid(
            partRelHTAB, CurrentMemoryContext, partTableRel, targetPartOid, partRel, part, RowExclusiveLock);
        if (bucketId != InvalidBktId) {
            searchHBucketFakeRelation(partRelHTAB, CurrentMemoryContext, partRel, bucketId, partRel);
        }

        (void)simple_heap_insert(partRel, copyTuple);

        heap_freetuple_ext(copyTuple);
    }

    heap_endscan(scan);

    if (PointerIsValid(partRelHTAB)) {
        FakeRelationCacheDestroy(partRelHTAB);
    }
}

static void readTuplesAndInsert(Relation tempTableRel, Relation partTableRel)
{
    if (RELATION_CREATE_BUCKET(tempTableRel)) {
        Relation bucketRel = NULL;
        oidvector* bucketlist = searchHashBucketByOid(tempTableRel->rd_bucketoid);

        for (int i = 0; i < bucketlist->dim1; i++) {
            bucketRel = bucketGetRelation(tempTableRel, NULL, bucketlist->values[i]);
            readTuplesAndInsertInternal(bucketRel, partTableRel, bucketlist->values[i]);
            bucketCloseRelation(bucketRel);
        }
    } else {
        readTuplesAndInsertInternal(tempTableRel, partTableRel, InvalidBktId);
    }
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		:
 * Description	:
 * Notes		:
 */
List* transformConstIntoTargetType(Form_pg_attribute* attrs, int2vector* partitionKey, List* boundary)
{
    int counter = 0;
    int2 partKeyPos = 0;
    Node* nodeBoundaryItem = NULL;
    Const* constBoundaryItem = NULL;
    Const* targetConstBoundaryItem = NULL;
    ListCell* cell = NULL;
    List* newBoundaryList = NULL;

    if (partitionKey->dim1 != boundary->length) {
        ereport(ERROR,

            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("number of boundary items NOT EQUAL to number of partition keys")));
    }

    foreach (cell, boundary) {
        nodeBoundaryItem = (Node*)lfirst(cell);
        Assert(nodeTag(nodeBoundaryItem) == T_Const);
        constBoundaryItem = (Const*)nodeBoundaryItem;

        partKeyPos = partitionKey->values[counter++];

        if (constBoundaryItem->ismaxvalue) {
            targetConstBoundaryItem = constBoundaryItem;
        } else {
            targetConstBoundaryItem = (Const*)GetTargetValue(attrs[partKeyPos - 1], constBoundaryItem, false);
            if (!PointerIsValid(targetConstBoundaryItem)) {
                list_free_ext(newBoundaryList);
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_OPERATION),
                        errmsg("partition key value must be const or const-evaluable expression")));
            }
            if (!OidIsValid(targetConstBoundaryItem->constcollid) && OidIsValid(attrs[partKeyPos - 1]->attcollation)) {
                targetConstBoundaryItem->constcollid = attrs[partKeyPos - 1]->attcollation;
            }
        }
        Assert(nodeTag(targetConstBoundaryItem) == T_Const);
        newBoundaryList = lappend(newBoundaryList, targetConstBoundaryItem);
    }

    return newBoundaryList;
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: create a toast for a new partition
 * Description	:
 * Input		: relation: partitined table 's relation
 *			:
 * Output	:
 * Return		:
 * Notes		:
 */
void addToastTableForNewPartition(Relation relation, Oid newPartId)
{
    Oid firstPartitionId = InvalidOid;
    Oid firstPartitionToastId = InvalidOid;
    Partition firstPartition = NULL;
    Datum reloptions = (Datum)0;
    bool isnull = false;
    HeapTuple reltuple = NULL;

    /* create toast table */
    firstPartitionId = ((RangePartitionMap*)relation->partMap)->rangeElements[0].partitionOid;
    firstPartition = partitionOpen(relation, firstPartitionId, NoLock);
    firstPartitionToastId = firstPartition->pd_part->reltoastrelid;

    if (OidIsValid(firstPartitionToastId)) {
        reltuple = SearchSysCache1(RELOID, ObjectIdGetDatum(firstPartitionToastId));
        if (!PointerIsValid(reltuple)) {
            ereport(ERROR,
                (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                    errmsg("cache lookup failed for toast table: %u", firstPartitionToastId)));
        }
        reloptions = SysCacheGetAttr(RELOID, reltuple, Anum_pg_class_reloptions, &isnull);

        if (isnull) {
            reloptions = (Datum)0;
        }

        (void)createToastTableForPartition(relation->rd_id, newPartId, reloptions, NULL);
        if (PointerIsValid(reltuple)) {
            ReleaseSysCache(reltuple);
        }

        /* Make the changes visible */
        CommandCounterIncrement();
    }
    partitionClose(relation, firstPartition, NoLock);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: create a cuDesc table for a new partition
 * Description	:
 * Input		: relation: partitined table 's relation
 *			:
 * Output	:
 * Return		:
 * Notes		:
 */
static void addCudescTableForNewPartition(Relation relation, Oid newPartId)
{
    Oid firstPartitionId = InvalidOid;
    Oid firstPartitionCudescTableId = InvalidOid;
    Partition firstPartition = NULL;
    Datum reloptions = (Datum)0;
    bool isnull = false;
    HeapTuple reltuple = NULL;

    /* create toast table */
    firstPartitionId = ((RangePartitionMap*)relation->partMap)->rangeElements[0].partitionOid;
    firstPartition = partitionOpen(relation, firstPartitionId, NoLock);
    firstPartitionCudescTableId = firstPartition->pd_part->relcudescrelid;

    if (OidIsValid(firstPartitionCudescTableId)) {
        reltuple = SearchSysCache1(RELOID, ObjectIdGetDatum(firstPartitionCudescTableId));
        if (!PointerIsValid(reltuple)) {
            ereport(ERROR,
                (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                    errmsg("cache lookup failed for cuDesc table: %u", firstPartitionCudescTableId)));
        }
        reloptions = SysCacheGetAttr(RELOID, reltuple, Anum_pg_class_reloptions, &isnull);

        if (isnull) {
            reloptions = (Datum)0;
        }

        (void)createCUDescTableForPartition(relation->rd_id, newPartId, reloptions);
        if (PointerIsValid(reltuple)) {
            ReleaseSysCache(reltuple);
        }

        /* Make the changes visible */
        CommandCounterIncrement();
    }
    partitionClose(relation, firstPartition, NoLock);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: create a delta table for a new partition
 * Description	:
 * Input		: relation: partitined table 's relation
 *			:
 * Output	:
 * Return		:
 * Notes		:
 */
static void addDeltaTableForNewPartition(Relation relation, Oid newPartId)
{
    Oid firstPartitionId = InvalidOid;
    Oid firstPartitionDeltaTableId = InvalidOid;
    Partition firstPartition = NULL;
    Datum reloptions = (Datum)0;
    bool isnull = false;
    HeapTuple reltuple = NULL;

    /* create toast table */
    firstPartitionId = ((RangePartitionMap*)relation->partMap)->rangeElements[0].partitionOid;
    firstPartition = partitionOpen(relation, firstPartitionId, NoLock);
    firstPartitionDeltaTableId = firstPartition->pd_part->reldeltarelid;

    if (OidIsValid(firstPartitionDeltaTableId)) {
        reltuple = SearchSysCache1(RELOID, ObjectIdGetDatum(firstPartitionDeltaTableId));
        if (!PointerIsValid(reltuple)) {
            ereport(ERROR,
                (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                    errmsg("cache lookup failed for delta  table: %u", firstPartitionDeltaTableId)));
        }
        reloptions = SysCacheGetAttr(RELOID, reltuple, Anum_pg_class_reloptions, &isnull);

        if (isnull) {
            reloptions = (Datum)0;
        }

        (void)createDeltaTableForPartition(relation->rd_id, newPartId, reloptions, NULL);
        if (PointerIsValid(reltuple)) {
            ReleaseSysCache(reltuple);
        }

        /* Make the changes visible */
        CommandCounterIncrement();
    }
    partitionClose(relation, firstPartition, NoLock);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: create a partiton for a special SN(sequence number)
 * Description	:
 * Input		: relation: partitined table 's relation
 *			: seqnum: the sequnce number for interval partition to be created
 * Output	:
 * Return		: the oid of the interval partition to be created
 * Notes		:
 */
Oid addPartitionBySN(Relation relation, int seqnum)
{
    return InvalidOid;
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: get boundary for table partition
 * Description	:
 * Notes		:
 */
Datum caculateBoundary(Datum transpoint, Oid attrtypid, Datum intervalue, Oid intertypid, int seqnum)
{
    return (Datum)0;
}

#ifdef PGXC
/*
 * IsTempTable
 *
 * Check if given table Oid is temporary.
 */
bool IsTempTable(Oid relid)
{
    HeapTuple tuple = NULL;
    bool res = false;

    // if oid is invalid, it's not a table, and not a temp table.
    if (InvalidOid == relid)
        return false;

    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(tuple)) {
        return false;
    }
    Form_pg_class classForm = (Form_pg_class)GETSTRUCT(tuple);
    res = (classForm->relpersistence == RELPERSISTENCE_TEMP);
    ReleaseSysCache(tuple);

    return res;
}

/*
 * IsRelaionView
 *
 * Check if given object Oid is view.
 */
bool IsRelaionView(Oid relid)
{
    Relation rel;
    bool res = false;
    /*
     * Is it correct to open without locks?
     * we just check if this object is view.
     */
    rel = relation_open(relid, NoLock);
    res = (rel->rd_rel->relkind == RELKIND_VIEW);
    relation_close(rel, NoLock);
    return res;
}

/*
 * IsIndexUsingTemp
 *
 * Check if given index relation uses temporary tables.
 */
bool IsIndexUsingTempTable(Oid relid)
{
    bool res = false;
    HeapTuple tuple;
    Oid parent_id = InvalidOid;

    tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(relid));
    if (HeapTupleIsValid(tuple)) {
        Form_pg_index index = (Form_pg_index)GETSTRUCT(tuple);
        parent_id = index->indrelid;

        /* Release system cache BEFORE looking at the parent table */
        ReleaseSysCache(tuple);

        res = IsTempTable(parent_id);
    } else
        res = false; /* Default case */

    return res;
}

/*
 * IsOnCommitActions
 *
 * Check if there are any on-commit actions activated.
 */
bool IsOnCommitActions(void)
{
    return list_length(u_sess->cmd_cxt.on_commits) > 0;
}

/*
 * DropTableThrowErrorExternal
 *
 * Error interface for DROP when looking for execution node type.
 */
void DropTableThrowErrorExternal(RangeVar* relation, ObjectType removeType, bool missing_ok)
{
    char relkind;

    /* Determine required relkind */
    switch (removeType) {
        case OBJECT_TABLE:
            relkind = RELKIND_RELATION;
            break;

        case OBJECT_INDEX:
            relkind = RELKIND_INDEX;
            break;

        case OBJECT_SEQUENCE:
            relkind = RELKIND_SEQUENCE;
            break;

        case OBJECT_VIEW:
            relkind = RELKIND_VIEW;
            break;

        case OBJECT_FOREIGN_TABLE:
            relkind = RELKIND_FOREIGN_TABLE;
            break;

        default: {
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized drop object type: %d", (int)removeType)));

            relkind = 0; /* keep compiler quiet */
        } break;
    }

    DropErrorMsgNonExistent(relation->relname, relkind, missing_ok);
}
#endif

struct CompressMetaInfo {
    const char* id;
    const int8 type;
};

static const CompressMetaInfo s_CmprsMetaInfo[] = {
    {"CMPRS_NOT_SUPPORT", REL_CMPRS_NOT_SUPPORT},

    /* IMPORTANT: add compression type after this line */
    {"NOCOMPRESS", REL_CMPRS_PAGE_PLAIN},
    {"COMPRESS", REL_CMPRS_FIELDS_EXTRACT},

    /* IMPORTANT: add compression type before this line */
    {"CMPRS_MAX", REL_CMPRS_MAX_TYPE},
};

#define GET_FIRST_CMPRS_METAITEM() ((CompressMetaInfo*)(s_CmprsMetaInfo + 1))
#define MOVE_NEXT_CMPRS_METAITEM(item) ((item)++)
#define LAST_CMPRS_METAITEM(item) ((item)->type == REL_CMPRS_MAX_TYPE)

static int8 getCompressType(const char* id)
{
    const CompressMetaInfo* metaItem = NULL;

    metaItem = GET_FIRST_CMPRS_METAITEM();
    while (!LAST_CMPRS_METAITEM(metaItem)) {
        if (0 == pg_strcasecmp(metaItem->id, id)) {
            break;
        }

        MOVE_NEXT_CMPRS_METAITEM(metaItem);
    }

    return metaItem->type;
}

static List* ATExecReplaceRelOptionListCell(List* options, char* keyName, char* newValue)
{
    ListCell* opt = NULL;
    ListCell* prev = NULL;
    bool found = false;

    foreach (opt, options) {
        DefElem* optDef = (DefElem*)lfirst(opt);
        if (pg_strcasecmp(optDef->defname, keyName) == 0) {
            found = true;
            break;
        }
        prev = opt;
    }

    if (found) {
        /* first delete this list cell. */
        options = list_delete_cell(options, opt, prev);

        /* insert new value */
        DefElem* newElem = makeDefElem(keyName, (Node*)makeString(newValue));
        options = lappend(options, newElem);
    }

    return options;
}

static void ATExecSetCompress(Relation rel, const char* cmprsId)
{
    Oid relId = RelationGetRelid(rel);
    Relation pgclass;
    HeapTuple relTuple;
    const int8 cmprsType = getCompressType(cmprsId);

    Assert(!CHECK_CMPRS_NOT_SUPPORT(RELATION_GET_CMPRS_ATTR(rel)));
    Assert(CHECK_CMPRS_VALID(cmprsType));

    /* always overwrite relcmprs field and compression options for row relation */
    pgclass = heap_open(RelationRelationId, RowExclusiveLock);

    relTuple = SearchSysCacheCopy1((int)RELOID, ObjectIdGetDatum(relId));
    if (!HeapTupleIsValid(relTuple)) {
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", relId)));
    }

    Datum replVal[Natts_pg_class];
    bool replNull[Natts_pg_class];
    bool replChange[Natts_pg_class];

    errno_t rc = memset_s(replVal, sizeof(replVal), 0, sizeof(replVal));
    securec_check(rc, "\0", "\0");
    rc = memset_s(replNull, sizeof(replNull), false, sizeof(replNull));
    securec_check(rc, "\0", "\0");
    rc = memset_s(replChange, sizeof(replChange), false, sizeof(replChange));
    securec_check(rc, "\0", "\0");

    /* first change the value of field RELCMPRS in pg_class */
    replVal[ANUM_PG_CLASS_RELCMPRS - 1] = cmprsType;
    replChange[ANUM_PG_CLASS_RELCMPRS - 1] = true;

    /* then change the value of COMPRESSION in relation options field */
    bool isNull = false;
    Datum relOptions = heap_getattr(relTuple, Anum_pg_class_reloptions, RelationGetDescr(pgclass), &isNull);
    if (!isNull) {
        List* options = untransformRelOptions(relOptions);
        options = ATExecReplaceRelOptionListCell(options,
            "compression",
            IsCompressedByCmprsInPgclass((RelCompressType)cmprsType) ? pstrdup(COMPRESSION_YES)
                                                                     : pstrdup(COMPRESSION_NO));
        relOptions = transformRelOptions((Datum)0, options, NULL, NULL, false, false);
        list_free_deep(options);

        replVal[Anum_pg_class_reloptions - 1] = relOptions;
        replChange[Anum_pg_class_reloptions - 1] = true;
    }

    HeapTuple newTuple = heap_modify_tuple(relTuple, RelationGetDescr(pgclass), replVal, replNull, replChange);
    simple_heap_update(pgclass, &newTuple->t_self, newTuple);
    CatalogUpdateIndexes(pgclass, newTuple);
    heap_freetuple_ext(newTuple);
    heap_freetuple_ext(relTuple);
    heap_close(pgclass, RowExclusiveLock);
}

/* CStore Rewrite Table Methods */
#include "access/cstore_rewrite.h"

#define RelAttrName(__tupdesc, __attridx) (NameStr((__tupdesc)->attrs[(__attridx)]->attname))

// get all the attributes to be checked or rewrited.
//
static void ATCStoreGetRewriteAttrs(_in_ AlteredTableInfo* tab, _in_ TupleDesc oldTupDesc, _in_ TupleDesc newTupDesc,
    _out_ CStoreRewriteColumn** rewriteInfo, _out_ bool* rewriteFlags, _out_ int* nColsOfEachType)
{
    Assert(newTupDesc->natts >= oldTupDesc->natts);
    Assert(nColsOfEachType && rewriteFlags);

    for (int pass = 0; pass < AT_NUM_PASSES; ++pass) {
        if (tab->subcmds[pass] == NIL) {
            continue;
        }

        List* subcmds = tab->subcmds[pass];
        ListCell* cmdCell = NULL;

        foreach (cmdCell, subcmds) {
            AlterTableCmd* cmd = (AlterTableCmd*)lfirst(cmdCell);

            // ADD COLUMN.
            if (pass == AT_PASS_ADD_COL) {
                Assert(cmd->def && cmd->def->type == T_ColumnDef);
                ColumnDef* colDef = (ColumnDef*)cmd->def;
                Assert(colDef->colname && colDef->colname[0] != '\0');

                // search newTupDesc' attributes backward, so as to reduce loop as possible.
                for (int attrIdx = newTupDesc->natts - 1; attrIdx >= 0; --attrIdx) {
                    if (pg_strcasecmp(colDef->colname, RelAttrName(newTupDesc, attrIdx)) == 0) {
                        Assert(rewriteInfo[attrIdx] == NULL);
                        rewriteInfo[attrIdx] = CStoreRewriteColumn::CreateForAddColumn(attrIdx + 1);

                        // collect how many new columns will be added.
                        ++nColsOfEachType[CSRT_ADD_COL];
                        rewriteFlags[attrIdx] = true;
                        break;
                    }
                }
                continue;
            }

            // ALTER COLUMN DATA TYPE.
            if (pass == AT_PASS_ALTER_TYPE) {
                Assert(cmd->name && cmd->name[0] != '\0');
                for (int attrIdx = 0; attrIdx < oldTupDesc->natts; ++attrIdx) {
                    if (pg_strcasecmp(cmd->name, RelAttrName(oldTupDesc, attrIdx)) == 0) {
                        // forbit multiple ALTER TYPE on the same column.
                        Assert(rewriteInfo[attrIdx] == NULL);
                        rewriteInfo[attrIdx] = CStoreRewriteColumn::CreateForSetDataType(attrIdx + 1);

                        // collect how many existing columns will be changed data type.
                        ++nColsOfEachType[CSRT_SET_DATA_TYPE];
                        rewriteFlags[attrIdx] = true;
                        break;
                    }
                }
                continue;
            }
        }
    }
}

static void ATCStoreRewriteTable(AlteredTableInfo* tab, Relation oldHeapRel, LOCKMODE lockMode, Oid targetTblspc)
{
    bool tblspcChanged = NeedToSetTableSpace(oldHeapRel, targetTblspc);
    Oid newfilenode = InvalidOid;
    Oid cudescOid = InvalidOid;
    Oid cudescIdxOid = InvalidOid;
    Relation cudescRel = NULL;
    Relation pg_class = NULL;
    Relation CUReplicationRel = NULL;
    HeapTuple pgclass_tuple = NULL;
    Form_pg_class pgclass_form = NULL;
    CStoreRewriter* rewriter = NULL;
    errno_t rc;

    /*
     * Notice: old TupleDesc has been copied and saved in tab->oldDesc.
     *   now the new TupleDesc can be seen and found in oldHeapRel.
     */
    TupleDesc newTupDesc = RelationGetDescr(oldHeapRel);
    TupleDesc oldTupDesc = tab->oldDesc;
    Assert(newTupDesc->natts >= oldTupDesc->natts);

    /* unsupported table/column constraints: CHECK; FOREIGN EKY; */
    Assert(tab->constraints == NIL);

    int nColsOfEachType[CSRT_NUM];
    rc = memset_s(nColsOfEachType, sizeof(int) * CSRT_NUM, 0, sizeof(int) * CSRT_NUM);
    securec_check(rc, "", "");

    int maxCols = newTupDesc->natts;
    bool* rewriteFlags = (bool*)palloc0(sizeof(bool) * maxCols);
    CStoreRewriteColumn** rewriteInfo = (CStoreRewriteColumn**)palloc0(sizeof(void*) * maxCols);

    /* split out: ADD COLUMNs; SET DATA TYPE COLUMNs; the others */
    ATCStoreGetRewriteAttrs(tab, oldTupDesc, newTupDesc, rewriteInfo, rewriteFlags, nColsOfEachType);

    /* set recomputing expression for updated columns. */
    ListCell* l = NULL;
    foreach (l, tab->newvals) {
        NewColumnValue* ex = (NewColumnValue*)lfirst(l);
        ex->exprstate = ExecInitExpr((Expr*)ex->expr, NULL);

        /* we expect only one NewColumnValue for each attrubute. */
        Assert(rewriteInfo[ex->attnum - 1] != NULL);
        Assert(rewriteInfo[ex->attnum - 1]->newValue == NULL);

        ColumnNewValue* newValExp = (ColumnNewValue*)palloc(sizeof(ColumnNewValue));
        newValExp->expr = ex->expr;
        newValExp->exprstate = ex->exprstate;
        rewriteInfo[ex->attnum - 1]->newValue = newValExp;
    }

    /* set NOT NULL constraint for updated columns. */
    if (tab->rewrite || tab->new_notnull) {
        for (int i = 0; i < maxCols; ++i) {
            if (rewriteInfo[i] != NULL && !rewriteInfo[i]->isDropped && newTupDesc->attrs[i]->attnotnull) {
                rewriteInfo[i]->notNull = true;
            }
        }
    }

    /* rewrite the column-store table. */
    rewriter = New(CurrentMemoryContext) CStoreRewriter(oldHeapRel, oldTupDesc, newTupDesc);

    /* lock order:
     * 1. column relation
     * 2. Delta relation [ Delta Index relation ]
     * 3. Cudesc relation + Cudesc Index relation
     */
    if (OidIsValid(oldHeapRel->rd_rel->reldeltarelid)) {
        LockRelationOid(oldHeapRel->rd_rel->reldeltarelid, lockMode);
    }
    cudescOid = oldHeapRel->rd_rel->relcudescrelid;
    cudescRel = heap_open(cudescOid, lockMode);
    cudescIdxOid = cudescRel->rd_rel->relcudescidx;
    /* don't index_open(cudescIdxOid) becuase rewriter need to reindex Cudesc relation */
    LockRelationOid(cudescIdxOid, lockMode);

    if (tblspcChanged) {
        /* Handle Delta && Delta Index Relation
         * Now it's safe to copy the relation data by block directly,
         * because Delta Relation now is unusable and has no data.
         *
         * When Delta Relation is usable, all tuples in it must be scanned,
         * re-written and merged/appended into CU files
         */
        ChangeTableSpaceForDeltaRelation(oldHeapRel->rd_rel->reldeltarelid, targetTblspc, lockMode);

        /* Handle each column' data */
        /* Here it's safe to open pg_class relation, because:
         * 1. Cannot change tablespace of partitioned table;
         * 2. Cannot ADD COLUMN/SET DATA TYPE for one partition;
         * 3. Changing tablespace for one partition cannot hit this branch;
         * so it's a ordinary relation from pg_class.
         */
        pg_class = heap_open(RelationRelationId, RowExclusiveLock);

        /* Get a modifiable copy of the relation's pg_class row */
        pgclass_tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(tab->relid));
        if (!HeapTupleIsValid(pgclass_tuple)) {
            ereport(ERROR,
                (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", tab->relid)));
        }
        pgclass_form = (Form_pg_class)GETSTRUCT(pgclass_tuple);

        /*
         * Relfilenodes are not unique across tablespaces, so we need to allocate
         * a new one in the new tablespace.
         */
        newfilenode = GetNewRelFileNode(targetTblspc, NULL, oldHeapRel->rd_rel->relpersistence);

        RelFileNode CUReplicationFile = {
            ConvertToRelfilenodeTblspcOid(targetTblspc), oldHeapRel->rd_node.dbNode, newfilenode, InvalidBktId};
        CUReplicationRel = CreateCUReplicationRelation(CUReplicationFile,
            oldHeapRel->rd_backend,
            oldHeapRel->rd_rel->relpersistence,
            RelationGetRelationName(oldHeapRel));

        for (int i = 0; i < maxCols; ++i) {
            Form_pg_attribute thisattr = newTupDesc->attrs[i];

            /* skip the dropped and rewritted columns */
            if (!thisattr->attisdropped && !rewriteFlags[i]) {
                CStoreCopyColumnData(CUReplicationRel, oldHeapRel, thisattr->attnum);
            }
        }

        /* prepare to handle ADD COLUMNs + SET DATATYPE COLUMNs */
        rewriter->ChangeTableSpace(CUReplicationRel);
    }

    /* handle Cudesc relation + ADD COLUMNs + SET DATATYPE COLUMNs */
    rewriter->BeginRewriteCols(maxCols, rewriteInfo, nColsOfEachType, rewriteFlags);
    rewriter->RewriteColsData();
    rewriter->EndRewriteCols();
    DELETE_EX(rewriter);

    if (tblspcChanged) {
        CStoreCopyColumnDataEnd(oldHeapRel, targetTblspc, newfilenode);

        /* destroy fake relation */
        FreeFakeRelcacheEntry(CUReplicationRel);

        /* update the pg_class row */
        pgclass_form->reltablespace = ConvertToPgclassRelTablespaceOid(targetTblspc);
        pgclass_form->relfilenode = newfilenode;
        simple_heap_update(pg_class, &pgclass_tuple->t_self, pgclass_tuple);
        CatalogUpdateIndexes(pg_class, pgclass_tuple);

        heap_freetuple_ext(pgclass_tuple);
        heap_close(pg_class, RowExclusiveLock);

        /* Make sure the reltablespace change is visible */
        CommandCounterIncrement();

        /* Handle Cudesc Index Relation */
        ATExecSetTableSpace(cudescIdxOid, ConvertToRelfilenodeTblspcOid(targetTblspc), lockMode);
    }

    /* unlock until committed */
    heap_close(cudescRel, NoLock);

    /* clean up work at last. */
    for (int k = 0; k < maxCols; ++k) {
        if (rewriteInfo[k]) {
            CStoreRewriteColumn::Destroy(&rewriteInfo[k]);
        }
    }
    pfree_ext(rewriteInfo);
    pfree_ext(rewriteFlags);
}

static void ATCStoreRewritePartition(AlteredTableInfo* tab, LOCKMODE lockMode)
{
    Relation parentRel = NULL;
    Relation newPartitionRel = NULL;
    List* partitionsList = NULL;
    ListCell* partitionCell = NULL;

    // construct a AlteredTableInfo object for a partition.
    // when it's passed into ATCStoreRewriteTable(), we take it as
    // the normall heap relation. therefore relid && partid shouldn't be
    // used, and so reset them.
    AlteredTableInfo* partitionTabInfo = (AlteredTableInfo*)palloc(sizeof(AlteredTableInfo));
    *partitionTabInfo = *tab;
    partitionTabInfo->relid = InvalidOid;
    partitionTabInfo->partid = InvalidOid;

    parentRel = heap_open(tab->relid, AccessExclusiveLock);
    Assert(RELATION_IS_PARTITIONED(parentRel) == true);

    partitionsList = relationGetPartitionList(parentRel, AccessExclusiveLock);
    foreach (partitionCell, partitionsList) {
        newPartitionRel = partitionGetRelation(parentRel, (Partition)lfirst(partitionCell));

        // rewrite each partition as the normall relation.
        ATCStoreRewriteTable(partitionTabInfo, newPartitionRel, lockMode, newPartitionRel->rd_rel->reltablespace);

        releaseDummyRelation(&newPartitionRel);
    }

    releasePartitionList(parentRel, &partitionsList, AccessExclusiveLock);
    heap_close(parentRel, NoLock);

    pfree_ext(partitionTabInfo);
    partitionTabInfo = NULL;
}

/*
 * Brief        : when alter the Dfs table, the followings must be deal with.
                  1. add Delta table for Dfs table.
                  2. add DfsDesc table for Dfs table.
 * Input        : relOid, the main table table Oid.
 *                reloptions, the table options.
 *                mainTblStmt, CreateStmt struct about the main table.
 * Output       : None.
 * Return Value : None.
 * Notes        : 1. If the store format is CU, we create CUDesc table, create
 *                DfsDesc table otherwise.
 */
void AlterDfsCreateTables(Oid relOid, Datum reloptions, CreateStmt* mainTblStmt)
{
    Relation rel;

    /*
     * Grab an exclusive lock on the target table, since we'll update its
     * pg_class tuple. This is redundant for all present uses, since caller
     * will have such a lock already.  But the lock is needed to ensure that
     * concurrent readers of the pg_class tuple won't have visibility issues,
     * so let's be safe.
     */
    rel = heap_open(relOid, AccessExclusiveLock);
    if (!RelationIsDfsStore(rel)) {
        heap_close(rel, NoLock);
        return;
    }

#ifndef ENABLE_MULTIPLE_NODES
    ereport(ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("Un-supported feature"),
            errdetail("HDFS data source or servers are no longer supported.")));
#endif

    (void)CreateDeltaTable(rel, reloptions, false, mainTblStmt);
    createDfsDescTable(rel, reloptions);

    /* create hdfs directory just on primary CN */
    if (IS_PGXC_COORDINATOR)
        CreateDfsStorage(rel);

    /*
     * we will deal with partition Dfs table here when support the parttion Dfs table.
     */
    heap_close(rel, NoLock);
}

/*
 * @Description: forbidden to set tablespace for partitioned table
 * @Param[IN] tab: Alter Table Info
 * @See also:
 */
static void ForbidToChangeTableSpaceOfPartitionedTable(AlteredTableInfo* tab)
{
    if (OidIsValid(tab->newTableSpace) && !OidIsValid(tab->partid)) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("can not set tablespace for partitioned relation"),
                errdetail("set tablespace for partition instead")));
    }
}

/*
 * @Description: rewrite row relation data.
 * @Param[IN] lockmode: lock mode used during rewriting data
 * @Param[IN] NewTableSpace: new tablespace used by row relation
 * @Param[IN] tab: Alter Table Info
 * @See also:
 */
static void ExecRewriteRowTable(AlteredTableInfo* tab, Oid NewTableSpace, LOCKMODE lockmode)
{
    ForbidToRewriteOrTestCstoreIndex(tab);

    Oid OIDNewHeap = make_new_heap(tab->relid, NewTableSpace);

    /*
     * Copy the heap data into the new table with the desired
     * modifications, and test the current data within the table
     * against new constraints generated by ALTER TABLE commands.
     */
    Relation oldRel = heap_open(tab->relid, NoLock);
    Relation newRel = heap_open(OIDNewHeap, lockmode);
    ATRewriteTable(tab, oldRel, newRel);
    heap_close(oldRel, NoLock);
    heap_close(newRel, NoLock);

    /*
     * Swap the physical files of the old and new heaps, then rebuild
     * indexes and discard the old heap.  We can use RecentXmin for
     * the table's new relfrozenxid because we rewrote all the tuples
     * in ATRewriteTable, so no older Xid remains in the table.  Also,
     * we never try to swap toast tables by content, since we have no
     * interest in letting this code work on system catalogs.
     */
    finish_heap_swap(tab->relid, OIDNewHeap, false, false, true, u_sess->utils_cxt.RecentXmin);

    /* clear all attrinitdefval */
    clearAttrInitDefVal(tab->relid);
}

/*
 * @Description: rewrite column relation data.
 * @Param[IN] lockmode: lock mode used during rewriting data
 * @Param[IN] NewTableSpace: new tablespace used by column relation
 * @Param[IN] tab: Alter Table Info
 * @See also:
 */
static void ExecRewriteCStoreTable(AlteredTableInfo* tab, Oid NewTableSpace, LOCKMODE lockmode)
{
    Relation OldHeap = heap_open(tab->relid, NoLock);
    ATCStoreRewriteTable(tab, OldHeap, lockmode, NewTableSpace);
    heap_close(OldHeap, NoLock);

    /* then, rebuild its index. */
    (void)reindex_relation(
        tab->relid, REINDEX_REL_SUPPRESS_INDEX_USE | REINDEX_REL_CHECK_CONSTRAINTS, REINDEX_ALL_INDEX);
}

/*
 * @Description: rewrite row partitioned relation data.
 *    Take each partition as a ordinary table, and rewrite it.
 * @Param[IN] lockmode: lock mode used during rewriting each partition.
 * @Param[IN] NewTableSpace: new tablespace used by some partition.
 * @Param[IN] tab: Alter Table Info
 * @See also:
 */
static void ExecRewriteRowPartitionedTable(AlteredTableInfo* tab, Oid NewTableSpace, LOCKMODE lockmode)
{
    Relation partitionedTableRel = NULL;
    TupleDesc partTabHeapDesc = NULL;
    HeapTuple tuple = NULL;
    List* tempTableOidList = NIL;
    List* partitions = NULL;
    ListCell* cell = NULL;
    Oid tempTableOid = InvalidOid;
    Datum partTabRelOptions = 0;
    int reindexFlags = 0;
    bool isNull = false;

    ForbidToChangeTableSpaceOfPartitionedTable(tab);
    ForbidToRewriteOrTestCstoreIndex(tab);

    partitionedTableRel = heap_open(tab->relid, AccessExclusiveLock);
    partTabHeapDesc = RelationGetDescr(partitionedTableRel);

    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(tab->relid));
    if (!HeapTupleIsValid(tuple)) {
        ereport(
            ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", tab->relid)));
    }
    partTabRelOptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isNull);
    if (isNull) {
        partTabRelOptions = (Datum)0;
    }

    partitions = relationGetPartitionList(partitionedTableRel, AccessExclusiveLock);
    foreach (cell, partitions) {
        Partition partition = (Partition)lfirst(cell);
        Relation oldRel = partitionGetRelation(partitionedTableRel, partition);

        /* make a temp table for swapping partition */
        Oid OIDNewHeap = makePartitionNewHeap(partitionedTableRel,
            partTabHeapDesc,
            partTabRelOptions,
            oldRel->rd_id,
            oldRel->rd_rel->reltoastrelid,
            oldRel->rd_rel->reltablespace);

        Relation newRel = heap_open(OIDNewHeap, lockmode);
        /* rewrite the temp table by partition */
        ATRewriteTable(tab, oldRel, newRel);
        heap_close(newRel, NoLock);

        /* swap the temp table and partition */
        finishPartitionHeapSwap(oldRel->rd_id, OIDNewHeap, false, u_sess->utils_cxt.RecentXmin);

        /* record the temp table oid for dropping */
        tempTableOidList = lappend_oid(tempTableOidList, OIDNewHeap);

        releaseDummyRelation(&oldRel);
    }

    ReleaseSysCache(tuple);

    /* rebuild index of partitioned table */
    reindexFlags = REINDEX_REL_SUPPRESS_INDEX_USE | REINDEX_REL_CHECK_CONSTRAINTS;
    (void)reindex_relation(tab->relid, reindexFlags, REINDEX_ALL_INDEX);

    /* drop the temp tables for swapping */
    foreach (cell, tempTableOidList) {
        ObjectAddress object;

        tempTableOid = DatumGetObjectId(lfirst(cell));

        object.classId = RelationRelationId;
        object.objectId = tempTableOid;
        object.objectSubId = 0;

        performDeletion(&object, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
    }
    list_free_ext(tempTableOidList);

    releasePartitionList(partitionedTableRel, &partitions, AccessExclusiveLock);
    heap_close(partitionedTableRel, NoLock);

    /* clear all attrinitdefval */
    clearAttrInitDefVal(tab->relid);
}

/*
 * @Description: rewrite column partitioned relation data.
 *    Take each partition as a ordinary table, and rewrite it.
 * @Param[IN] lockmode: lock mode used during rewriting each partition.
 * @Param[IN] targetTableSpace: new tablespace used by some partition.
 * @Param[IN] tab: Alter Table Info
 * @See also:
 */
static void ExecRewriteCStorePartitionedTable(AlteredTableInfo* tab, Oid targetTableSpace, LOCKMODE lockmode)
{
    /* forbid to change tablespace for partitioned table.
     * so argument *targetTableSpace* is not used.
     */
    ForbidToChangeTableSpaceOfPartitionedTable(tab);

    Relation OldHeap = heap_open(tab->relid, NoLock);
    ATCStoreRewritePartition(tab, lockmode);
    heap_close(OldHeap, NoLock);

    /* then, rebuild its index. */
    (void)reindex_relation(
        tab->relid, REINDEX_REL_SUPPRESS_INDEX_USE | REINDEX_REL_CHECK_CONSTRAINTS, REINDEX_ALL_INDEX);
}

/*
 * @Description: Only check relation data becuase constraints changed
 * @Param[IN] tab: Alter Table Info
 * @See also:
 */
static void ExecOnlyTestRowTable(AlteredTableInfo* tab)
{
    ForbidToRewriteOrTestCstoreIndex(tab);

    Relation oldRel = heap_open(tab->relid, NoLock);
    ATRewriteTable(tab, oldRel, NULL);
    heap_close(oldRel, NoLock);
}

/*
 * @Description: Only check relation data becuase constraints changed
 * @Param[IN] tab: Alter Table Info
 * @See also:
 */
static void ExecOnlyTestCStoreTable(AlteredTableInfo* tab)
{
    ereport(ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("Un-support feature"),
            errdetail("column stored relation doesn't support this feature")));
}

/**
 * @Description: Build a list of all attributes. The attributes must has
 * "NOT NULL" constraint and is not dropped column.
 * @in tuple_desc, The tuple description.
 * @return
 */
List* make_not_null_attrs(TupleDesc tuple_desc)
{
    List* not_null_attrs = NIL;
    for (int i = 0; i < tuple_desc->natts; i++) {
        if (tuple_desc->attrs[i]->attnotnull && !tuple_desc->attrs[i]->attisdropped)
            not_null_attrs = lappend_int(not_null_attrs, i);
    }

    return not_null_attrs;
}

/**
 * @Description: Only check the validity of existing data because of some altering operators,
 * for example, the query "alter table ... add column col date type not null" contains
 * "NOT NULL" constaint, if the relation has no data, the query will be executed successfully,
 * otherwise get a fail result.
 * @in tab, The AlteredTableInfo struct.
 * @return None.
 */
static void exec_only_test_dfs_table(AlteredTableInfo* tab)
{
    if (IS_PGXC_DATANODE && tab->new_notnull) {
        Relation rel = RelationIdGetRelation(tab->relid);
        MemoryContext context = AllocSetContextCreate(CurrentMemoryContext,
            "AlterTableContext",
            ALLOCSET_DEFAULT_MINSIZE,
            ALLOCSET_DEFAULT_INITSIZE,
            ALLOCSET_DEFAULT_MAXSIZE);
        MemoryContext old_context = MemoryContextSwitchTo(context);
        TupleDesc tuple_desc = RelationGetDescr(rel);
        Datum* values = (Datum*)palloc0(rel->rd_att->natts * sizeof(Datum));
        bool* nulls = (bool*)palloc0(rel->rd_att->natts * sizeof(bool));
        List* not_null_attrs = make_not_null_attrs(tuple_desc);
        TupleTableSlot* scan_tuple_slot = NULL;
        HeapTuple tuple = NULL;
        ListCell* lc = NULL;
        /*
         * Create tuple slot for scanning.
         */
        scan_tuple_slot = MakeTupleTableSlot();
        scan_tuple_slot->tts_tupleDescriptor = tuple_desc;
        scan_tuple_slot->tts_values = values;
        scan_tuple_slot->tts_isnull = nulls;
        DfsScanState* scan = dfs::reader::DFSBeginScan(rel, NIL, 0, NULL);
        do {
            dfs::reader::DFSGetNextTuple(scan, scan_tuple_slot);

            if (!scan_tuple_slot->tts_isempty) {

                tuple = heap_form_tuple(tuple_desc, values, nulls);
                foreach (lc, not_null_attrs) {
                    int attn = lfirst_int(lc);

                    if (relationAttIsNull(tuple, attn + 1, tuple_desc)) {
                        dfs::reader::DFSEndScan(scan);
                        pfree_ext(scan_tuple_slot);

                        relation_close(rel, NoLock);
                        ereport(ERROR,
                            (errcode(ERRCODE_NOT_NULL_VIOLATION),
                                errmsg(
                                    "column \"%s\" contains null values", NameStr(tuple_desc->attrs[attn]->attname))));
                    }
                }
            } else {
                break;
            }

        } while (true);

        dfs::reader::DFSEndScan(scan);
        pfree_ext(scan_tuple_slot);
        (void)MemoryContextSwitchTo(old_context);
        MemoryContextDelete(context);

        relation_close(rel, NoLock);
    }
}

/*
 * @Description: Only check relation data becuase constraints changed
 * @Param[IN] tab:  Alter Table Info
 * @See also:
 */
static void ExecOnlyTestRowPartitionedTable(AlteredTableInfo* tab)
{
    Relation oldRel = NULL;
    Partition partition = NULL;
    ListCell* cell = NULL;
    Relation partitionedTableRel = NULL;
    List* partitions = NULL;

    ForbidToRewriteOrTestCstoreIndex(tab);

    /* get all partitions of target partitioned table */
    partitionedTableRel = heap_open(tab->relid, NoLock);
    partitions = relationGetPartitionList(partitionedTableRel, NoLock);

    foreach (cell, partitions) {
        partition = (Partition)lfirst(cell);
        oldRel = partitionGetRelation(partitionedTableRel, partition);
        /* check each partition */
        ATRewriteTable(tab, oldRel, NULL);
        releaseDummyRelation(&oldRel);
    }

    releasePartitionList(partitionedTableRel, &partitions, NoLock);
    heap_close(partitionedTableRel, NoLock);
}

/*
 * @Description: Only check relation data becuase constraints changed
 * @Param[IN] tab:  Alter Table Info
 * @See also:
 */
static void ExecOnlyTestCStorePartitionedTable(AlteredTableInfo* tab)
{
    ereport(ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("Un-support feature"),
            errdetail("column stored relation doesn't support this feature")));
}

/*
 * @Description: Only check PSort relation data.
 * @Param[IN] tab: Alter Table Info
 * @See also:
 */
static void ForbidToRewriteOrTestCstoreIndex(AlteredTableInfo* tab)
{
    if (tab->relkind == RELKIND_INDEX) {
        Relation rel = index_open(tab->relid, AccessShareLock);
        if (rel->rd_rel->relam == PSORT_AM_OID) {
            index_close(rel, AccessShareLock);

            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    (errmsg("Un-support feature"), errdetail("PSort relation doesn't support this feature"))));
        }

        if (rel->rd_rel->relam == CBTREE_AM_OID) {
            index_close(rel, AccessShareLock);

            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    (errmsg("Un-support feature"), errdetail("CBtree relation doesn't support this feature"))));
        }

        if (rel->rd_rel->relam == CGIN_AM_OID) {
            index_close(rel, AccessShareLock);

            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    (errmsg("Un-support feature"), errdetail("CGinBtree relation doesn't support this feature"))));
        }
        index_close(rel, AccessShareLock);
    }
}

/*
 * @Description: SET TABLESPACE for psort relation
 * @Param[IN] lockmode:  lock mode used during changing tablespace.
 * @Param[IN] newTableSpace: the new/target tablespace.
 * @Param[IN] psortOid: Oid of PSort relation
 * @See also:
 */
static void PSortChangeTableSpace(Oid psortOid, Oid newTableSpace, LOCKMODE lockmode)
{
    AlteredTableInfo* tabinfo = (AlteredTableInfo*)palloc0(sizeof(AlteredTableInfo));

    /* fill the needed info */
    Assert(OidIsValid(newTableSpace));
    tabinfo->relid = psortOid;
    tabinfo->relkind = RELKIND_RELATION;
    tabinfo->newTableSpace = newTableSpace;

    /* treat psort as a column table */
    ExecChangeTableSpaceForCStoreTable(tabinfo, lockmode);

    pfree_ext(tabinfo);
}

/*
 * @Description: change tablespace for row relation.
 *    PSort index is handled in this branch, because its oid is remembered here.
 * @Param[IN] lockmode: lock mode used during changing tablespace.
 * @Param[IN] tab: Alter Table Info
 * @See also: the comments of function ExecChangeTableSpaceForRowPartition()
 */
static void ExecChangeTableSpaceForRowTable(AlteredTableInfo* tab, LOCKMODE lockmode)
{
    ATExecSetTableSpace(tab->relid, tab->newTableSpace, lockmode);

    /* handle a special index type: PSORT index */
    if (tab->relkind == RELKIND_INDEX) {
        Relation rel = index_open(tab->relid, lockmode);
        if (rel->rd_rel->relam == PSORT_AM_OID) {
            PSortChangeTableSpace(rel->rd_rel->relcudescrelid, /* psort oid */
                tab->newTableSpace,
                lockmode);
        }
        index_close(rel, NoLock);
    }
}

/*
 * @Description: change tablespace for Delta relation.
 * @Param[IN] deltaOid: Oid of Delta relation
 * @Param[IN] lockmode: lock mode used during changing tablespace.
 * @Param[IN] targetTableSpace: the new tablespace.
 * @See also:
 */
static inline void ChangeTableSpaceForDeltaRelation(Oid deltaOid, Oid targetTableSpace, LOCKMODE lockmode)
{
    if (OidIsValid(deltaOid)) {
        /* ATExecSetTableSpace() requires that targetTableSpace is not InvalidOid */
        targetTableSpace = ConvertToRelfilenodeTblspcOid(targetTableSpace);
        Assert(OidIsValid(targetTableSpace));

        /* lock delta relation with lockmode */
        Relation deltaRel = heap_open(deltaOid, lockmode);

        /* change tablespace for Delta Relation */
        ATExecSetTableSpace(deltaOid, targetTableSpace, lockmode);

        /* unlock until committed */
        relation_close(deltaRel, NoLock);

        /* change tablespace for Delta Index Relation */
    }
}

/*
 * @Description: change tablespace for CUDesc and its index relation.
 * @Param[IN] cudescIdxOid: Oid of Cudesc Index relation
 * @Param[IN] cudescOid: Oid of Cudesc relation
 * @Param[IN] lockmode: lock mode used during changing tablespace
 * @Param[IN] targetTableSpace: the new tablespace
 * @See also:
 */
static inline void ChangeTableSpaceForCudescRelation(
    Oid cudescIdxOid, Oid cudescOid, Oid targetTableSpace, LOCKMODE lockmode)
{
    /* ATExecSetTableSpace() requires that targetTableSpace is valid */
    targetTableSpace = ConvertToRelfilenodeTblspcOid(targetTableSpace);
    Assert(OidIsValid(targetTableSpace));

    /* change tablespace for Cudesc Relation */
    Assert(OidIsValid(cudescOid));
    ATExecSetTableSpace(cudescOid, targetTableSpace, lockmode);

    /* change tablespace for Cudesc Index Relation */
    Assert(OidIsValid(cudescIdxOid));
    ATExecSetTableSpace(cudescIdxOid, targetTableSpace, lockmode);
}

/*
 * @Description: change tablespace for column relation.
 * @Param[IN] lockmode: lock mode used during changing tablespace
 * @Param[IN] tab: Alter Table Info
 * @See also:
 */
static void ExecChangeTableSpaceForCStoreTable(AlteredTableInfo* tab, LOCKMODE lockmode)
{
    Relation colRel = NULL;
    Relation cudescRel = NULL;
    Relation cudescIdxRel = NULL;
    Oid cudescOid = InvalidOid;
    Oid cudescIdxOid = InvalidOid;
    Oid targetTableSpace = tab->newTableSpace;
    Oid newrelfilenode = InvalidOid;

    /* here maybe open a heap relation or index relation, so call relation_open() */
    colRel = relation_open(tab->relid, lockmode);

    /* No work if no change in tablespace. */
    if (!NeedToSetTableSpace(colRel, targetTableSpace)) {
        relation_close(colRel, NoLock);
        return;
    }

    /* lock order:
     * 1. column relation
     * 2. Delta relation [ Delta Index relation ]
     * 3. Cudesc relation + Cudesc Index relation
     */
    if (OidIsValid(colRel->rd_rel->reldeltarelid)) {
        LockRelationOid(colRel->rd_rel->reldeltarelid, lockmode);
    }
    cudescOid = colRel->rd_rel->relcudescrelid;
    cudescRel = heap_open(cudescOid, lockmode);
    cudescIdxOid = cudescRel->rd_rel->relcudescidx;
    cudescIdxRel = index_open(cudescIdxOid, lockmode);

    /* 1. Handle Delta && Delta Index Relation */
    ChangeTableSpaceForDeltaRelation(colRel->rd_rel->reldeltarelid, targetTableSpace, lockmode);

    /* 2. Handle each column' data */
    Relation pg_class = heap_open(RelationRelationId, RowExclusiveLock);

    /* Get a modifiable copy of the relation's pg_class row */
    HeapTuple tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(tab->relid));
    if (!HeapTupleIsValid(tuple)) {
        ereport(
            ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for relation %u", tab->relid)));
    }
    Form_pg_class rd_rel = (Form_pg_class)GETSTRUCT(tuple);
    newrelfilenode = CStoreSetTableSpaceForColumnData(colRel, targetTableSpace);

    /* update the pg_class row */
    rd_rel->reltablespace = ConvertToPgclassRelTablespaceOid(targetTableSpace);
    rd_rel->relfilenode = newrelfilenode;
    simple_heap_update(pg_class, &tuple->t_self, tuple);
    CatalogUpdateIndexes(pg_class, tuple);

    heap_freetuple_ext(tuple);
    heap_close(pg_class, RowExclusiveLock);

    /* Make sure the reltablespace change is visible */
    CommandCounterIncrement();

    /* 3. Handle Cudesc && Index Relation */
    ChangeTableSpaceForCudescRelation(cudescIdxOid, cudescOid, targetTableSpace, lockmode);

    index_close(cudescIdxRel, NoLock);
    heap_close(cudescRel, NoLock);
    relation_close(colRel, NoLock);
}

/*
 * @Description: change tablespace for row partition.
 * first, forbid to SET TABLESPACE for partitioned table.
 * if it's a
 *   1) row heap partition,
 *   2) row index partition,
 *      the two are the same,
 *      step 1: copy data to new tablespace
 *      step 2: update pg_partition.reltablespace && pg_partition.relfilenode
 *      step 3: handle toast && toast index if necessary.
 *   3) psort index partition,
 *      it's a column table, so that
 *      step 1: update pg_partition.reltablespace (ATExecSetTableSpaceForPartitionP3)
 *      step 2: handle psort as an ordinary column table. (ExecChangeTableSpaceForCStoreTable)
 *
 * @Param[IN] lockmode: lock mode used during changing tablespace
 * @Param[IN] tab: Alter Table Info
 * @See also:
 */
static void ExecChangeTableSpaceForRowPartition(AlteredTableInfo* tab, LOCKMODE lockmode)
{
    ForbidToChangeTableSpaceOfPartitionedTable(tab);

    /* input lockmode is the lock mode of patitioned table, which is >= AccessShareLock.
     * Take and example, t1 is a row partitioned relation and under tblspc1 tablespace. now execute
     *        ALTER TABLE t1 MOVE PARTITION p1 TO pg_default, ADD COLUMN c_char2 char(5);
     * it will triggle Altering-Table-Instantly feature, and needn't rewrite all the tuple data.
     * so lock mode is 8, and SET TABLESPACE branch is entered into. So that input lockmode may
     * be not AccessShareLock.
     *
     * Here we should use the lock mode of partition, which is AccessExclusiveLock.
     * see also ATExecSetTableSpaceForPartitionP2().
     */
    const LOCKMODE partitionLock = AccessExclusiveLock;

    ATExecSetTableSpaceForPartitionP3(tab->relid, tab->partid, tab->newTableSpace, partitionLock);

    /* handle a special index type: PSORT index */
    if (tab->relkind == RELKIND_INDEX) {
        Relation rel = index_open(tab->relid, NoLock);
        if (rel->rd_rel->relam == PSORT_AM_OID) {
            Partition part = partitionOpen(rel, tab->partid, partitionLock);
            PSortChangeTableSpace(part->pd_part->relcudescrelid, /* psort oid */
                tab->newTableSpace,
                partitionLock);
            partitionClose(rel, part, NoLock);
        }
        index_close(rel, NoLock);
    }
}

/*
 * @Description: change tablespace for column partition
 * @Param[IN] lockmode: lock mode used during changing tablespace
 * @Param[IN] tab: Alter Table Info
 * @See also:
 */
static void ExecChangeTableSpaceForCStorePartition(AlteredTableInfo* tab, LOCKMODE lockmode)
{
    Relation parentRel = NULL;
    Partition partition = NULL;
    Relation partitionRel = NULL;
    Relation cudescRel = NULL;
    Relation cudescIdxRel = NULL;
    Oid partOid = tab->partid;
    Oid cudescOid = InvalidOid;
    Oid cudescIdxOid = InvalidOid;
    Oid targetTableSpace = tab->newTableSpace;
    Oid newrelfilenode = InvalidOid;

    ForbidToChangeTableSpaceOfPartitionedTable(tab);

    /* input lockmode is the lock mode of patitioned table, which is AccessShareLock.
     * Here we should use the lock mode of partition, which is AccessExclusiveLock.
     * see also ATExecSetTableSpaceForPartitionP2().
     */
    const LOCKMODE partitionLock = AccessExclusiveLock;

    /* here maybe open a heap relation or index relation, so call relation_open() */
    parentRel = relation_open(tab->relid, NoLock);
    partition = partitionOpen(parentRel, partOid, partitionLock);
    partitionRel = partitionGetRelation(parentRel, partition);

    /* No work if no change in tablespace. */
    if (!NeedToSetTableSpace(partitionRel, targetTableSpace)) {
        releaseDummyRelation(&partitionRel);
        partitionClose(parentRel, partition, NoLock);
        relation_close(parentRel, NoLock);
        return;
    }

    /* lock order:
     * 1. column relation
     * 2. Delta relation [ Delta Index relation ]
     * 3. Cudesc relation + Cudesc Index relation
     */
    if (OidIsValid(partitionRel->rd_rel->reldeltarelid)) {
        LockRelationOid(partitionRel->rd_rel->reldeltarelid, partitionLock);
    }
    cudescOid = partitionRel->rd_rel->relcudescrelid;
    cudescRel = heap_open(cudescOid, partitionLock);
    cudescIdxOid = cudescRel->rd_rel->relcudescidx;
    cudescIdxRel = index_open(cudescIdxOid, partitionLock);

    /* 1. Handle Delta && Delta Index Relation */
    ChangeTableSpaceForDeltaRelation(partitionRel->rd_rel->reldeltarelid, targetTableSpace, partitionLock);

    /* 2. Handle each column' data */
    Relation pg_partition = heap_open(PartitionRelationId, RowExclusiveLock);

    /* Get a modifiable copy of the relation's pg_partition row */
    HeapTuple tuple = SearchSysCacheCopy1(PARTRELID, ObjectIdGetDatum(partOid));
    if (!HeapTupleIsValid(tuple))
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for partition %u", partOid)));
    Form_pg_partition rd_rel = (Form_pg_partition)GETSTRUCT(tuple);

    newrelfilenode = CStoreSetTableSpaceForColumnData(partitionRel, targetTableSpace);

    /* update the pg_partition row */
    rd_rel->reltablespace = ConvertToPgclassRelTablespaceOid(targetTableSpace);
    rd_rel->relfilenode = newrelfilenode;
    simple_heap_update(pg_partition, &tuple->t_self, tuple);
    CatalogUpdateIndexes(pg_partition, tuple);

    heap_freetuple_ext(tuple);
    heap_close(pg_partition, RowExclusiveLock);

    /* Make sure the reltablespace change is visible */
    CommandCounterIncrement();

    /* 3. Handle Cudesc && Index Relation */
    ChangeTableSpaceForCudescRelation(cudescIdxOid, cudescOid, targetTableSpace, partitionLock);

    index_close(cudescIdxRel, NoLock);
    heap_close(cudescRel, NoLock);

    releaseDummyRelation(&partitionRel);
    partitionClose(parentRel, partition, NoLock);
    relation_close(parentRel, NoLock);
}

/**
 * @Description: Whether judge the column is partition column.
 * @in rel, A relation.
 * @in att_no, Attribute number.
 * @return If the the column is partition column, return true, otherwise return false.
 */
bool is_partition_column(Relation rel, AttrNumber att_no)
{
    bool is_part_col = false;

    if (RelationIsValuePartitioned(rel)) {
        List* part_col_list = ((ValuePartitionMap*)rel->partMap)->partList;
        ListCell* lc = NULL;
        foreach (lc, part_col_list) {
            if (att_no == lfirst_int(lc)) {
                is_part_col = true;
                break;
            }
        }
    } else if (RelationIsRangePartitioned(rel)) {
        int2vector* part_key = ((RangePartitionMap*)rel->partMap)->partitionKey;
        for (int i = 0; i < part_key->dim1; i++) {
            if (att_no == part_key->values[i]) {
                is_part_col = true;
                break;
            }
        }
    }

    return is_part_col;
}

/**
 * @Description: Reset every partition's start_ctid/end_ctid of rel
 * @in rel, parent relation of partitions.
 * @return void
 */
static void ResetPartsRedisCtidRelOptions(Relation rel)
{
    Relation pg_partition = NULL;
    ScanKeyData key[2];
    SysScanDesc scan = NULL;
    HeapTuple tuple = NULL;
    TupleDesc part_tupdesc = NULL;
    List* redis_reloptions = NIL;

    pg_partition = heap_open(PartitionRelationId, RowExclusiveLock);
    part_tupdesc = RelationGetDescr(pg_partition);
    ScanKeyInit(&key[0],
                Anum_pg_partition_parttype,
                BTEqualStrategyNumber,
                F_CHAREQ,
                CharGetDatum(PART_OBJ_TYPE_TABLE_PARTITION));
    ScanKeyInit(&key[1],
                Anum_pg_partition_parentid,
                BTEqualStrategyNumber,
                F_OIDEQ,
                ObjectIdGetDatum(RelationGetRelid(rel)));

    scan = systable_beginscan(pg_partition,
                              PartitionParentOidIndexId,
                              true, SnapshotNow, 2, key);
    redis_reloptions = AlterTableSetRedistribute(rel, REDIS_REL_RESET_CTID, NULL);
    while (HeapTupleIsValid(tuple = systable_getnext(scan))) {
        HeapTuple dtuple;
        Datum repl_val[Natts_pg_partition];
        bool repl_null[Natts_pg_partition];
        bool repl_repl[Natts_pg_partition];
        bool isNull = false;

        Datum newOptions = (Datum)0;
        errno_t errorno = EOK;

        errorno = memset_s(repl_null, sizeof(repl_null), false, sizeof(repl_null));
        securec_check_c(errorno, "\0", "\0");

        errorno = memset_s(repl_repl, sizeof(repl_repl), false, sizeof(repl_repl));
        securec_check_c(errorno, "\0", "\0");

        Datum dval = fastgetattr(tuple, 
                                 Anum_pg_partition_reloptions,
                                 part_tupdesc,    
                                 &isNull);
        /* reset redis reloptions info */
        newOptions = transformRelOptions(isNull ? (Datum)0 : dval, redis_reloptions, NULL, NULL, false, false);

        if (newOptions != (Datum)0) {
            repl_val[Anum_pg_partition_reloptions - 1] = newOptions;
            repl_null[Anum_pg_partition_reloptions - 1] = false;
        } else {
            repl_null[Anum_pg_partition_reloptions - 1] = true;
        }

        repl_repl[Anum_pg_partition_reloptions - 1] = true;

        dtuple = heap_modify_tuple(tuple, part_tupdesc, repl_val, repl_null, repl_repl);

        simple_heap_update(pg_partition, &dtuple->t_self, dtuple);
        CatalogUpdateIndexes(pg_partition, dtuple);

        heap_freetuple_ext(dtuple);
    }
    systable_endscan(scan);
    list_free_ext(redis_reloptions);
    heap_close(pg_partition, RowExclusiveLock);
}


/**
 * @Description: Reset one partition(oid = part_oid)'s ctid info of rel
 * @in rel, parent relation.
 * @in part_oid, oid of partition
 * @return void
 */
static void ResetOnePartRedisCtidRelOptions(Relation rel, Oid part_oid)
{
    Assert(rel != NULL && OidIsValid(part_oid));

    ResetRelRedisCtidRelOptions(
        rel, part_oid, PARTRELID, Natts_pg_partition, Anum_pg_partition_reloptions, PartitionRelationId);
}

/**
 * @Description: relation(parent relation when partition rel)
 * @in rel, relaton or partition's fakeRelation.
 * @part_oid, partition's oid
 * @in cat_id, catche id of pg_class or pg_partition
 * @in att_num, att_num of pgcat(pg_class or pg_partition)
 * @in att_inx, att_inx of reloptions
 * @Oid pgcat_oid, pg_class or pg_partition
 * @return void
 */
static void ResetRelRedisCtidRelOptions(Relation rel, Oid part_oid, int cat_id, int att_num, int att_inx, Oid pgcat_oid)
{
    Datum newOptions = (Datum)0;
    Datum oldOptions = (Datum)0;
    List* redis_reloptions = NIL;
    Datum* repl_val = NULL;
    bool* repl_null = NULL;
    bool* repl_repl = NULL;
    Relation pgcatrel;
    HeapTuple newtuple;
    HeapTuple tuple;
    bool isnull = false;

    repl_val = (Datum*)palloc0(att_num * sizeof(Datum));
    repl_null = (bool*)palloc0(att_num);
    repl_repl = (bool*)palloc0(att_num);

    if (OidIsValid(part_oid)) {
        tuple = SearchSysCache1(cat_id, ObjectIdGetDatum(part_oid));
    } else {
        tuple = SearchSysCache1(cat_id, ObjectIdGetDatum(rel->rd_id));
    }
    if (!HeapTupleIsValid(tuple)) {
        ereport(
            ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for partition %u", rel->rd_id)));
    }

    /* Get the old reloptions */
    oldOptions = SysCacheGetAttr(cat_id, tuple, att_inx, &isnull);

    /* reset redis reloptions info */
    redis_reloptions = AlterTableSetRedistribute(rel, REDIS_REL_RESET_CTID, NULL);
    newOptions = transformRelOptions(isnull ? (Datum)0 : oldOptions, redis_reloptions, NULL, NULL, false, false);
    list_free_ext(redis_reloptions);

    if (newOptions != (Datum)0) {
        repl_val[att_inx - 1] = newOptions;
        repl_null[att_inx - 1] = false;
    } else
        repl_null[att_inx - 1] = true;

    repl_repl[att_inx - 1] = true;

    pgcatrel = heap_open(pgcat_oid, RowExclusiveLock);
    newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgcatrel), repl_val, repl_null, repl_repl);

    simple_heap_update(pgcatrel, &newtuple->t_self, newtuple);
    CatalogUpdateIndexes(pgcatrel, newtuple);

    heap_freetuple_ext(newtuple);
    ReleaseSysCache(tuple);
    heap_close(pgcatrel, RowExclusiveLock);

    pfree_ext(repl_val);
    pfree_ext(repl_null);
    pfree_ext(repl_repl);
    return;
}
/**
 * WLM has 5 system tables, those tables store monitoring data on CN node, will influence node-restore && node-expand.
 * So, need to grant truncate permission of those 5 tables to user, to reduce influence.
 *
 */
static bool WLMRelationCanTruncate(Relation rel)
{
    const char* targetRelname = get_rel_name(rel->rd_id);
    if ((strcmp(targetRelname, WLM_USER_RESOURCE_HISTORY) == 0 || strcmp(targetRelname, WLM_INSTANCE_HISTORY) == 0 ||
            strcmp(targetRelname, WLM_EC_OPERATOR_INFO) == 0 || strcmp(targetRelname, WLM_OPERATOR_INFO) == 0 ||
            strcmp(targetRelname, WLM_SESSION_INFO) == 0) &&
        IsSystemNamespace(RelationGetNamespace(rel))) {
        return true;
    }
    return false;
}

/**
 * @Description: execute given sql directly.
 * @return void
 */
static void execute_sql_direct(const char* query_string)
{
    int16 format = 0;
    List* parsetree_list = NULL;
    ListCell* parsetree_item = NULL;
    parsetree_list = pg_parse_query(query_string, NULL);
    foreach (parsetree_item, parsetree_list) {
        Portal portal = NULL;
        DestReceiver* receiver = NULL;
        List* querytree_list = NULL;
        List* plantree_list = NULL;
        Node* parsetree = (Node*)lfirst(parsetree_item);
        const char* commandTag = CreateCommandTag(parsetree);

        querytree_list = pg_analyze_and_rewrite(parsetree, query_string, NULL, 0);
        plantree_list = pg_plan_queries(querytree_list, 0, NULL);

        portal = CreatePortal(query_string, true, true);
        portal->visible = false;
        PortalDefineQuery(portal, NULL, query_string, commandTag, plantree_list, NULL);

        receiver = CreateDestReceiver(DestNone);
        PortalStart(portal, NULL, 0, NULL);
        
        PortalSetResultFormat(portal, 1, &format);
        (void)PortalRun(portal, FETCH_ALL, true, receiver, receiver, NULL);
        PortalDrop(portal, false);
    }
}

/**
 * @Description: create partition management policy if defined ttl or period
 * @in stmt, CreateStmt in CreateCommand.
 * @return void
 */
void create_part_policy_if_needed(CreateStmt* stmt, char relkind)
{
    char* relname = stmt->relation->relname;
    char* ttl_str = NULL;
    char* period_str = NULL;
    char* orientation_str = NULL;
    static const char* const validnsps[] = HEAP_RELOPT_NAMESPACES;
    Datum reloptions;

    reloptions = transformRelOptions((Datum)0, stmt->options, NULL, validnsps, true, false);
    StdRdOptions* std_opt = (StdRdOptions*)heap_reloptions(relkind, reloptions, true);

    orientation_str = (char*)StdRdOptionsGetStringData(std_opt, orientation, ORIENTATION_ROW);

    if (0 != pg_strcasecmp(TIME_UNDEFINED, StdRdOptionsGetStringData(std_opt, ttl, TIME_UNDEFINED)))
        ttl_str = (char*)StdRdOptionsGetStringData(std_opt, ttl, TIME_UNDEFINED);

    if (0 != pg_strcasecmp(TIME_UNDEFINED, StdRdOptionsGetStringData(std_opt, period, TIME_UNDEFINED)))
        period_str = (char*)StdRdOptionsGetStringData(std_opt, period, TIME_UNDEFINED);

    /* For timeseries storage, the default partititon interval is TIME_ONE_DAY */
    if (period_str != NULL || ttl_str != NULL || 0 == strcmp(orientation_str, "timeseries")) {
        int rc = 0;
        char sql[MAX_SQL_LEN] = {0};

        if (!u_sess->attr.attr_common.enable_tsdb) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("Unable to create partition policy when enable_tsdb is off.")));
        }
        
        if (ttl_str != NULL) {
            rc = sprintf_s(sql, MAX_SQL_LEN,
                "CALL add_drop_partition_policy_v2('%s', interval '%s') ;",
                relname, ttl_str);
            securec_check_ss_c(rc, "\0", "\0");
        }

        if (period_str == NULL) {
            period_str = TIME_ONE_DAY;
        }
        rc = sprintf_s(sql + rc, MAX_SQL_LEN - rc,
            "CALL add_create_partition_policy_v2('%s', interval '%s',  interval '%s') ;",
            relname, period_str, period_str);
        securec_check_ss_c(rc, "\0", "\0");

        const char* query_string = sql;
        execute_sql_direct(query_string);
    }
}

/**
 * @Description: alter partition management policy
 * @if alter cmd contains ttl or period.
 * @return void
 */
static void alter_partition_policy_if_needed(Relation rel, List* defList)
{
    ListCell* def = NULL;
    char* relname = rel->rd_rel->relname.data;
    char* ttl_str = NULL;
    char* period_str = NULL;
    char sql[MAX_SQL_LEN] = {0};
    int rc = 0;
    int pos = 0;

    foreach (def, defList) {
        DefElem* defElem = (DefElem*)lfirst(def);
        if (strcmp(defElem->defname, "ttl") == 0) {
            ttl_str = ((Value*)defElem->arg)->val.str;
        } else if (strcmp(defElem->defname, "period") == 0) {
            period_str = ((Value*)defElem->arg)->val.str;
        }
    }

    if (period_str == NULL && ttl_str == NULL) {
        return;
    } else if (!u_sess->attr.attr_common.enable_tsdb) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Unable to alter partition policy when enable_tsdb is off.")));
    }

    if (ttl_str != NULL) {
        rc = sprintf_s(sql, MAX_SQL_LEN,
            "CALL remove_drop_partition_policy_v2('%s') ;",
            relname);
        securec_check_ss_c(rc, "\0", "\0");
        pos += rc;
        if (period_str != NULL) {
            rc = sprintf_s(sql + pos, MAX_SQL_LEN - pos,
                "CALL add_drop_partition_policy_v2('%s', interval '%s', interval '%s') ;",
                relname, ttl_str, period_str);
            securec_check_ss_c(rc, "\0", "\0");
        } else {
            rc = sprintf_s(sql + pos, MAX_SQL_LEN - pos,
                "CALL add_drop_partition_policy_v2('%s', interval '%s') ;",
                relname, ttl_str);
            securec_check_ss_c(rc, "\0", "\0");
        }
    }
    if (period_str != NULL) {
        rc = sprintf_s(sql + pos, MAX_SQL_LEN - pos,
            "CALL remove_create_partition_policy_v2('%s') ;", relname);
        securec_check_ss_c(rc, "\0", "\0");
        pos += rc;
        rc = sprintf_s(sql + pos, MAX_SQL_LEN - pos,
            "CALL add_create_partition_policy_v2('%s', interval '%s',  interval '%s') ;",
            relname, period_str, period_str);
        securec_check_ss_c(rc, "\0", "\0");
        pos += rc;
    }

    const char* query_string = sql;
    execute_sql_direct(query_string);
}

/**
 * @Description: check for alter table when relation is timeseries store
 * @in rel, relation which need alter table.
 * @in cmd, alter table command.
 * @return void
 */
static void at_timeseries_check(Relation rel, AlterTableCmd* cmd)
{
    if (!CSTORE_SUPPORT_AT_CMD(cmd->subtype)) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("Un-support feature"),
                errdetail("timeseries store relation doesn't support this ALTER yet")));
    }
    if (cmd->subtype == AT_AddColumn) {
        ColumnDef* def = (ColumnDef*)cmd->def;
        if (def->kvtype == ATT_KV_UNDEFINED) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("Un-support feature"),
                    errdetail("column kvtype should be defined for timeseries store relation")));
        }
        if (def->kvtype == ATT_KV_TIMETAG) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("Un-support feature"),
                    errdetail("column kvtype cannot defined as TSTIME")));
        }
    }
}

static OnCommitAction GttOncommitOption(const List *options)
{
    ListCell *listptr;
    OnCommitAction action = ONCOMMIT_NOOP;

    foreach(listptr, options) {
        DefElem *def = reinterpret_cast<DefElem *>(lfirst(listptr));
        if (strcmp(def->defname, "on_commit_delete_rows") == 0) {
            bool res = false;
            char *sval = defGetString(def);

            if (!parse_bool(sval, &res)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("parameter \"on_commit_delete_rows\" requires a Boolean value")));
            }

            if (res) {
                action = ONCOMMIT_DELETE_ROWS;
            } else {
                action = ONCOMMIT_PRESERVE_ROWS;
            }
            break;
        }
    }
    return action;
}


