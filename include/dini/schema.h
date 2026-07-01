#ifndef DINI_SCHEMA_H
#define DINI_SCHEMA_H

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <dini/diniglobal.h>
#include <dini/handles.h>
#include <dini/support/shareddata.h>
#include <dini/types.h>
#include <dini/value.h>

namespace dini {

class ChangeSet;
class EngineSchema;
class ListBuilder;
class SchemaBuilder;
struct SchemaDefinitionData;
class TableBuilder;
class TransactionContext;

/**
 * @brief Optional aggregate maintenance hints for a column.
 *
 * Hinted aggregate indexes are maintained on writes and may be used by
 * aggregation views. Leaving all flags false preserves the default fallback
 * behavior with no additional write-time maintenance.
 */
struct DINI_EXPORT AggregateIndexOptions {
    bool sum = false;
    bool minMax = false;
    bool byParent = true;
};

/**
 * @brief Describes a normal stored column.
 *
 * ColumnDefinition is consumed during schema definition only. Once its owning
 * SchemaBuilder is frozen, the produced EngineSchema and column behavior become
 * immutable.
 */
struct DINI_EXPORT ColumnDefinition {
    std::string debugName;
    ValueType type = ValueType::Null;
    IndexKind index = IndexKind::None;
    std::optional<Value> defaultValue;
    bool nullable = true;
    bool participatesInUniqueWhenNull = false;
    AggregateIndexOptions aggregateIndex;
    std::function<bool(const Value &)> check;
};

/**
 * @brief Describes a computed materialized column.
 *
 * Computed columns may depend only on fields in the same item and on previously
 * ordered computed columns. Implementations must recompute and index them inside
 * the same ChangeSet as the triggering update.
 */
struct DINI_EXPORT ComputedColumnDefinition {
    std::string debugName;
    ValueType type = ValueType::Null;
    IndexKind index = IndexKind::None;
    bool nullable = true;
    AggregateIndexOptions aggregateIndex;
    std::vector<ColumnHandle> dependsOn;
    std::function<Value(const std::vector<Value> &)> compute;
};

/**
 * @brief Identifies the table or list that an association may reference.
 *
 * AssociationTarget is a schema-time descriptor. Runtime parent or list
 * membership is still stored as a normal nullable relation column value.
 */
using AssociationTarget = std::variant<TableHandle, ListHandle>;

/**
 * @brief Describes a schema-declared association or composition relation.
 *
 * All parent-child relations are composition relations. The relation column is
 * nullable, participates in cascade delete and cycle checks, and is updated using
 * the same transaction and ChangeSet semantics as a normal column.
 */
struct DINI_EXPORT AssociationDefinition {
    std::string debugName;
    AssociationTarget target;
};

/**
 * @brief Declares a maintained multi-column range index for one container.
 *
 * Range indexes are explicit because maintaining kd-tree data adds update cost.
 * Queries whose range predicates are covered by one declaration may use the
 * maintained range index; otherwise they fall back to scalar indexes.
 */
struct DINI_EXPORT RangeIndexDefinition {
    std::string debugName;
    std::vector<ColumnHandle> columns;
};

/**
 * @brief Describes a column that is valid only for one polymorphic variant.
 *
 * Reading or writing the column for an item of another variant is rejected by
 * runtime validation. Engine query predicates treat non-matching variants as
 * predicate failures for that item.
 */
struct DINI_EXPORT VariantColumnDefinition {
    std::string debugName;
    VariantHandle variant;
    ValueType type = ValueType::Null;
    IndexKind index = IndexKind::None;
    std::optional<Value> defaultValue;
    bool nullable = true;
    AggregateIndexOptions aggregateIndex;
    std::function<bool(const Value &)> check;
};

/**
 * @brief Describes one hook callback registered on a container.
 *
 * The callback receives a TransactionContext and a ChangeSet describing the
 * relevant operation or transaction. Only BeforeApply callbacks may issue further
 * updates through the context; other stages must be observation or validation.
 */
struct DINI_EXPORT HookDefinition {
    HookStage stage = HookStage::BeforeApply;
    std::function<void(TransactionContext &, const ChangeSet &)> callback;
};

/**
 * @brief Immutable public metadata for a container definition.
 *
 * ContainerInfo is returned by EngineSchema inspection methods. It is diagnostic
 * and validation-oriented and does not expose implementation storage.
 */
struct DINI_EXPORT ContainerInfo {
    ContainerKind kind = ContainerKind::Table;
    ContainerId id = 0;
    std::string debugName;
};

/**
 * @brief Immutable public metadata for a column definition.
 *
 * ColumnInfo describes the declared type and indexing policy of a field. Callers
 * can use it to validate query construction before executing a query.
 */
struct DINI_EXPORT ColumnInfo {
    ColumnId id = 0;
    ContainerId containerId = 0;
    std::string debugName;
    ValueType type = ValueType::Null;
    IndexKind index = IndexKind::None;
    bool computed = false;
    bool association = false;
    bool variantSpecific = false;
};

/**
 * @brief Immutable public metadata for a relation definition.
 *
 * RelationInfo links a relation handle to its storage column and target container
 * kind. It is intended for diagnostics and safe API validation.
 */
struct DINI_EXPORT RelationInfo {
    RelationId id = 0;
    ContainerId containerId = 0;
    ColumnHandle column;
    AssociationTarget target;
};

/**
 * @brief Immutable frozen schema shared by document engine instances.
 *
 * EngineSchema separates definition from runtime document state. It owns no
 * document data and may be shared by many DocumentEngine instances after it has
 * been produced by SchemaBuilder::freeze().
 */
class DINI_EXPORT EngineSchema {
public:
    struct Impl;

    /**
     * @brief Creates an empty invalid schema wrapper.
     *
     * @pre None.
     * @post isValid() returns false.
     */
    EngineSchema();

    /**
     * @brief Destroys the schema wrapper.
     *
     * @pre No public precondition.
     * @post Shared immutable schema data is released when no wrappers remain.
     */
    ~EngineSchema();

    /**
     * @brief Copies a schema wrapper.
     *
     * @param other Schema wrapper to share.
     * @pre other may be valid or invalid.
     * @post Both wrappers refer to the same immutable schema data.
     */
    EngineSchema(const EngineSchema &other);

    /**
     * @brief Moves a schema wrapper.
     *
     * @param other Schema wrapper to move from.
     * @pre other may be valid or invalid.
     * @post This wrapper receives other's schema reference.
     */
    EngineSchema(EngineSchema &&other) noexcept;

    /**
     * @brief Assigns a shared schema wrapper.
     *
     * @param other Schema wrapper to share.
     * @pre other may be valid or invalid.
     * @post This wrapper refers to the same immutable schema data as other.
     */
    EngineSchema &operator=(const EngineSchema &other);

    /**
     * @brief Move-assigns a schema wrapper.
     *
     * @param other Schema wrapper to move from.
     * @pre other may be valid or invalid.
     * @post This wrapper receives other's schema reference.
     */
    EngineSchema &operator=(EngineSchema &&other) noexcept;

    /**
     * @brief Tests whether this wrapper contains a frozen schema.
     *
     * @pre None.
     * @post Does not validate any specific handle.
     */
    bool isValid() const noexcept;

    /**
     * @brief Returns the stable identity of this schema.
     *
     * @pre isValid() must be true.
     * @post The schema is not modified.
     * @throws SchemaError if the wrapper is invalid.
     */
    SchemaId schemaId() const;

    /**
     * @brief Returns metadata for a table handle.
     *
     * @param table Table handle to inspect.
     * @pre table must belong to this schema and identify a table.
     * @post The schema is not modified.
     * @throws HandleError if table is invalid or belongs to another schema.
     */
    ContainerInfo tableInfo(TableHandle table) const;

    /**
     * @brief Returns metadata for a list handle.
     *
     * @param list List handle to inspect.
     * @pre list must belong to this schema and identify a list.
     * @post The schema is not modified.
     * @throws HandleError if list is invalid or belongs to another schema.
     */
    ContainerInfo listInfo(ListHandle list) const;

    /**
     * @brief Returns metadata for a column handle.
     *
     * @param column Column handle to inspect.
     * @pre column must belong to this schema and identify a column.
     * @post The schema is not modified.
     * @throws HandleError if column is invalid or belongs to another schema.
     */
    ColumnInfo columnInfo(ColumnHandle column) const;

    /**
     * @brief Returns metadata for a relation handle.
     *
     * @param relation Relation handle to inspect.
     * @pre relation must belong to this schema and identify a relation.
     * @post The schema is not modified.
     * @throws HandleError if relation is invalid or belongs to another schema.
     */
    RelationInfo relationInfo(RelationHandle relation) const;

    /**
     * @brief Validates that a table handle belongs to this schema.
     *
     * @param table Table handle to validate.
     * @pre table must be a public table handle.
     * @post Returns normally only if table is usable with this schema.
     * @throws HandleError if validation fails.
     */
    void validate(TableHandle table) const;

    /**
     * @brief Validates that a list handle belongs to this schema.
     *
     * @param list List handle to validate.
     * @pre list must be a public list handle.
     * @post Returns normally only if list is usable with this schema.
     * @throws HandleError if validation fails.
     */
    void validate(ListHandle list) const;

    /**
     * @brief Validates that a column handle belongs to this schema.
     *
     * @param column Column handle to validate.
     * @pre column must be a public column handle.
     * @post Returns normally only if column is usable with this schema.
     * @throws HandleError if validation fails.
     */
    void validate(ColumnHandle column) const;

    /**
     * @brief Validates that a relation handle belongs to this schema.
     *
     * @param relation Relation handle to validate.
     * @pre relation must be a public relation handle.
     * @post Returns normally only if relation is usable with this schema.
     * @throws HandleError if validation fails.
     */
    void validate(RelationHandle relation) const;

private:
    SharedDataPointer<Impl> _impl;

    explicit EngineSchema(SharedDataPointer<Impl> data);
    friend const SchemaDefinitionData &schemaData(const EngineSchema &schema);
    friend class DocumentEngine;
    friend class Transaction;
    friend class TransactionContext;
    friend class View;
    friend class SchemaBuilder;
};

/**
 * @brief Builder for immutable EngineSchema objects.
 *
 * SchemaBuilder owns schema definition state until freeze() is called. Frozen
 * EngineSchema instances are immutable and may be shared by many DocumentEngine
 * instances.
 */
class DINI_EXPORT SchemaBuilder {
public:
    struct Impl;

    /**
     * @brief Creates an empty schema builder.
     *
     * @pre None.
     * @post The builder can create tables and lists until freeze().
     */
    SchemaBuilder();

    /**
     * @brief Destroys the schema builder.
     *
     * @pre No public precondition.
     * @post Unfrozen definition state owned only by this builder is released.
     */
    ~SchemaBuilder();

    /**
     * @brief Copy construction is disabled because builders own mutable definition state.
     *
     * @pre None.
     * @post This overload is unavailable; move the builder when ownership transfer is required.
     */
    SchemaBuilder(const SchemaBuilder &other) = delete;

    /**
     * @brief Copy assignment is disabled because builders own mutable definition state.
     *
     * @pre None.
     * @post This overload is unavailable; move the builder when ownership transfer is required.
     */
    SchemaBuilder &operator=(const SchemaBuilder &other) = delete;

    /**
     * @brief Moves a schema builder.
     *
     * @param other Builder to move from.
     * @pre other must not be used for further schema definition after the move.
     * @post This builder owns the moved definition state.
     */
    SchemaBuilder(SchemaBuilder &&other) noexcept;

    /**
     * @brief Move-assigns a schema builder.
     *
     * @param other Builder to move from.
     * @pre This builder must not have active dependent builder objects.
     * @post This builder owns the moved definition state.
     */
    SchemaBuilder &operator=(SchemaBuilder &&other) noexcept;

    /**
     * @brief Creates a table definition.
     *
     * @param debugName Optional diagnostic name.
     * @pre The builder must not be frozen.
     * @post Returns a TableBuilder for adding columns, variants, relations, and hooks.
     * @throws SchemaError if this builder is invalid or frozen.
     */
    TableBuilder createTable(std::string debugName);

    /**
     * @brief Creates a list definition.
     *
     * @param debugName Optional diagnostic name.
     * @pre The builder must not be frozen.
     * @post Returns a ListBuilder for adding columns, associations, variants, and hooks.
     * @throws SchemaError if this builder is invalid or frozen.
     */
    ListBuilder createList(std::string debugName);

    /**
     * @brief Freezes the schema definition into an EngineSchema.
     *
     * @pre All required container definitions must be complete.
     * @post The builder cannot be mutated further and the returned schema is immutable.
     * @throws SchemaError if definitions are incomplete or inconsistent.
     */
    EngineSchema freeze();

    /**
     * @brief Tests whether freeze() has already been called.
     *
     * @pre None.
     * @post Returns true after a successful freeze().
     */
    bool isFrozen() const noexcept;

private:
    std::unique_ptr<Impl> _impl;

    friend class ListBuilder;
};

/**
 * @brief Builder facade for a table container.
 *
 * TableBuilder is a lightweight view into SchemaBuilder-owned definition state.
 * It is valid only while the owning SchemaBuilder remains alive and unfrozen.
 */
class DINI_EXPORT TableBuilder {
public:
    struct Impl;

    /**
     * @brief Creates an invalid table builder.
     *
     * @pre None.
     * @post isValid() returns false.
     */
    TableBuilder();

    /**
     * @brief Destroys the table builder facade.
     *
     * @pre No public precondition.
     * @post The private builder facade state is released.
     */
    ~TableBuilder();

    /**
     * @brief Copy construction is disabled because the facade owns private builder state.
     *
     * @pre None.
     * @post This overload is unavailable; use move construction to transfer the facade.
     */
    TableBuilder(const TableBuilder &other) = delete;

    /**
     * @brief Copy assignment is disabled because the facade owns private builder state.
     *
     * @pre None.
     * @post This overload is unavailable; use move assignment to transfer the facade.
     */
    TableBuilder &operator=(const TableBuilder &other) = delete;

    /**
     * @brief Moves a table builder facade.
     *
     * @param other Builder facade to move from.
     * @pre other may be valid or invalid.
     * @post This builder receives other's private state and other becomes invalid.
     */
    TableBuilder(TableBuilder &&other) noexcept;

    /**
     * @brief Move-assigns a table builder facade.
     *
     * @param other Builder facade to move from.
     * @pre other may be valid or invalid.
     * @post This builder receives other's private state and other becomes invalid.
     */
    TableBuilder &operator=(TableBuilder &&other) noexcept;

    /**
     * @brief Tests whether this builder refers to a table definition.
     *
     * @pre None.
     * @post Does not validate that the owning SchemaBuilder is still mutable.
     */
    bool isValid() const noexcept;

    /**
     * @brief Returns the handle for the table being defined.
     *
     * @pre isValid() must be true.
     * @post The schema definition is not modified.
     * @throws SchemaError if this builder is invalid.
     */
    TableHandle handle() const;

    /**
     * @brief Adds a normal stored column to the table.
     *
     * @param definition Column declaration.
     * @pre definition.type must not be ValueType::Null.
     * @post Returns a handle for the newly declared column.
     * @throws SchemaError if the definition is invalid or the schema is frozen.
     */
    ColumnHandle addColumn(const ColumnDefinition &definition);

    /**
     * @brief Adds a computed materialized column to the table.
     *
     * @param definition Computed column declaration.
     * @pre Dependencies must belong to this table and compute must be callable.
     * @post Returns a handle for the newly declared computed column.
     * @throws SchemaError if dependencies are invalid or the schema is frozen.
     */
    ColumnHandle addComputedColumn(const ComputedColumnDefinition &definition);

    /**
     * @brief Adds an association or parent relation column to the table.
     *
     * @param definition Association declaration.
     * @pre The target container handle must belong to the same schema builder.
     * @post Returns the relation handle; its column() stores runtime parent values.
     * @throws SchemaError if the target is invalid or the schema is frozen.
     */
    RelationHandle addAssociation(const AssociationDefinition &definition);

    /**
     * @brief Adds one allowed polymorphic variant to the table.
     *
     * @param debugName Optional diagnostic variant name.
     * @pre The table must not already contain committed runtime data.
     * @post Returns a handle that may be used for inserts and variant columns.
     * @throws SchemaError if the schema is frozen.
     */
    VariantHandle addVariant(std::string debugName);

    /**
     * @brief Adds a column that is legal only for one variant.
     *
     * @param definition Variant-specific column declaration.
     * @pre definition.variant must belong to this table.
     * @post Returns a handle for the newly declared variant column.
     * @throws SchemaError if the variant or definition is invalid.
     */
    ColumnHandle addVariantColumn(const VariantColumnDefinition &definition);

    /**
     * @brief Declares a maintained multi-column range index for this table.
     *
     * @param definition Range index name and indexed columns.
     * @pre All columns must belong to this table, be indexed, and use numeric ordering.
     * @post Matching range AND queries may use the maintained range index.
     * @throws SchemaError if the declaration is invalid or the schema is frozen.
     */
    void addRangeIndex(const RangeIndexDefinition &definition);

    /**
     * @brief Registers a hook on this table.
     *
     * @param definition Hook stage and callback.
     * @pre definition.callback must be callable.
     * @post The hook participates in the declared transaction stage.
     * @throws SchemaError if the schema is frozen or the hook is invalid.
     */
    void addHook(const HookDefinition &definition);

private:
    std::unique_ptr<Impl> _impl;

    friend class SchemaBuilder;
    friend class ListBuilder;
};

/**
 * @brief Builder facade for a first-class ordered list container.
 *
 * ListBuilder is a lightweight view into SchemaBuilder-owned definition state.
 * List membership is determined by an association column and order is maintained
 * by the engine per association value.
 */
class DINI_EXPORT ListBuilder {
public:
    struct Impl;

    /**
     * @brief Creates an invalid list builder.
     *
     * @pre None.
     * @post isValid() returns false.
     */
    ListBuilder();

    /**
     * @brief Destroys the list builder facade.
     *
     * @pre No public precondition.
     * @post The private builder facade state is released.
     */
    ~ListBuilder();

    /**
     * @brief Copy construction is disabled because the facade owns private builder state.
     *
     * @pre None.
     * @post This overload is unavailable; use move construction to transfer the facade.
     */
    ListBuilder(const ListBuilder &other) = delete;

    /**
     * @brief Copy assignment is disabled because the facade owns private builder state.
     *
     * @pre None.
     * @post This overload is unavailable; use move assignment to transfer the facade.
     */
    ListBuilder &operator=(const ListBuilder &other) = delete;

    /**
     * @brief Moves a list builder facade.
     *
     * @param other Builder facade to move from.
     * @pre other may be valid or invalid.
     * @post This builder receives other's private state and other becomes invalid.
     */
    ListBuilder(ListBuilder &&other) noexcept;

    /**
     * @brief Move-assigns a list builder facade.
     *
     * @param other Builder facade to move from.
     * @pre other may be valid or invalid.
     * @post This builder receives other's private state and other becomes invalid.
     */
    ListBuilder &operator=(ListBuilder &&other) noexcept;

    /**
     * @brief Tests whether this builder refers to a list definition.
     *
     * @pre None.
     * @post Does not validate that the owning SchemaBuilder is still mutable.
     */
    bool isValid() const noexcept;

    /**
     * @brief Returns the handle for the list being defined.
     *
     * @pre isValid() must be true.
     * @post The schema definition is not modified.
     * @throws SchemaError if this builder is invalid.
     */
    ListHandle handle() const;

    /**
     * @brief Adds the association column that groups list instances.
     *
     * @param definition Association declaration.
     * @pre The target container handle must belong to the same schema builder.
     * @post Returns the relation handle that determines list membership.
     * @throws SchemaError if the list already has an association or the schema is frozen.
     */
    RelationHandle setAssociation(const AssociationDefinition &definition);

    /**
     * @brief Adds a normal stored column to list elements.
     *
     * @param definition Column declaration.
     * @pre definition.type must not be ValueType::Null.
     * @post Returns a handle for the newly declared column.
     * @throws SchemaError if the definition is invalid or the schema is frozen.
     */
    ColumnHandle addColumn(const ColumnDefinition &definition);

    /**
     * @brief Adds a computed materialized column to list elements.
     *
     * @param definition Computed column declaration.
     * @pre Dependencies must belong to this list and compute must be callable.
     * @post Returns a handle for the newly declared computed column.
     * @throws SchemaError if dependencies are invalid or the schema is frozen.
     */
    ColumnHandle addComputedColumn(const ComputedColumnDefinition &definition);

    /**
     * @brief Adds one allowed polymorphic variant to this list.
     *
     * @param debugName Optional diagnostic variant name.
     * @pre The list must not already contain committed runtime data.
     * @post Returns a handle that may be used for inserts and variant columns.
     * @throws SchemaError if the schema is frozen.
     */
    VariantHandle addVariant(std::string debugName);

    /**
     * @brief Adds a column that is legal only for one list element variant.
     *
     * @param definition Variant-specific column declaration.
     * @pre definition.variant must belong to this list.
     * @post Returns a handle for the newly declared variant column.
     * @throws SchemaError if the variant or definition is invalid.
     */
    ColumnHandle addVariantColumn(const VariantColumnDefinition &definition);

    /**
     * @brief Declares a maintained multi-column range index for this list.
     *
     * @param definition Range index name and indexed columns.
     * @pre All columns must belong to this list, be indexed, and use numeric ordering.
     * @post Matching range AND queries may use the maintained range index.
     * @throws SchemaError if the declaration is invalid or the schema is frozen.
     */
    void addRangeIndex(const RangeIndexDefinition &definition);

    /**
     * @brief Registers a hook on this list.
     *
     * @param definition Hook stage and callback.
     * @pre definition.callback must be callable.
     * @post The hook participates in the declared transaction stage.
     * @throws SchemaError if the schema is frozen or the hook is invalid.
     */
    void addHook(const HookDefinition &definition);

private:
    std::unique_ptr<Impl> _impl;

    friend class SchemaBuilder;
    friend class TableBuilder;
};

} // namespace dini

#endif // DINI_SCHEMA_H
