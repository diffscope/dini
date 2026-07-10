#ifndef DINI_TYPES_H
#define DINI_TYPES_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <dini/diniglobal.h>

namespace dini {

/**
 * @brief Globally unique item identifier inside one document engine instance.
 *
 * ItemId is used for rows, list elements, parent references, change records, and
 * serialized change sets. Implementations must generate values automatically and must not
 * reuse an identifier that may have been observed during the lifetime of an
 * engine instance.
 */
using ItemId = std::uint64_t;

/**
 * @brief Binary byte sequence used for snapshots, schema structures, change sets, and binary values.
 *
 * ByteArray has no filesystem semantics. The caller owns storage and ordering of
 * byte streams produced by the engine.
 */
using ByteArray = std::vector<std::uint8_t>;

/**
 * @brief Stable schema identity token used by handles to detect cross-schema use.
 *
 * A SchemaId is assigned by the schema subsystem. Public code may compare it for
 * diagnostics, but must not construct semantic meaning from the numeric value.
 */
using SchemaId = std::uint64_t;

/**
 * @brief Stable container identity token within one schema.
 *
 * A ContainerId identifies a table or list definition. It is intended for handle
 * validation and serialization, not for user-facing persistence outside engine data.
 */
using ContainerId = std::uint32_t;

/**
 * @brief Stable column identity token within one schema.
 *
 * A ColumnId identifies normal, computed, variant-specific, association, parent,
 * and polymorphic type columns.
 */
using ColumnId = std::uint32_t;

/**
 * @brief Stable relation identity token within one schema.
 *
 * A RelationId identifies a schema-declared association or composition relation.
 */
using RelationId = std::uint32_t;

/**
 * @brief Stable polymorphic variant identity token within one schema.
 *
 * A VariantId identifies one allowed variant of a polymorphic container.
 */
using VariantId = std::uint32_t;

/**
 * @brief Built-in value type supported by the storage engine.
 *
 * The engine does not support custom runtime value types. Domain concepts such as
 * pitch, color, path, or tick must be represented by one of these scalar kinds.
 */
enum class ValueType {
    Null,
    Bool,
    Int64,
    UInt64,
    Double,
    String,
    Binary
};

/**
 * @brief Declares whether and how a column participates in indexed operations.
 *
 * Only indexed columns, ID, parent association, and polymorphic variant fields may
 * be used in query predicates, sorting, and aggregation grouping.
 */
enum class IndexKind {
    None,
    Normal,
    Unique
};

/**
 * @brief Identifies one of the four fixed hook stages.
 *
 * BeforeApply is the only stage where hooks may issue additional updates through
 * TransactionContext. Other stages are observation or validation phases.
 */
enum class HookStage {
    BeforeApply,
    AfterApply,
    BeforeCommit,
    AfterCommit
};

/**
 * @brief Describes why an event or committed change was produced.
 *
 * Normal is used for ordinary write transactions. Undo and Redo are used for the
 * special transactions that replay stored undo steps.
 */
enum class EventOrigin {
    Normal,
    Undo,
    Redo
};

/**
 * @brief Event category delivered to engine subscribers.
 *
 * Rollback events carry the inverse ChangeSet actually applied during rollback.
 * Apply and commit events use EventOrigin to distinguish normal, undo, and redo
 * sources.
 */
enum class EventKind {
    AfterApply,
    AfterCommit,
    Rollback
};

/**
 * @brief Describes the observable lifecycle state of a transaction object.
 */
enum class TransactionState {
    Active,
    Failed,
    Committed,
    RolledBack
};

/**
 * @brief Distinguishes unordered tables from ordered association-grouped lists.
 */
enum class ContainerKind {
    Table,
    List
};

/**
 * @brief Field category used by query expressions and aggregation grouping.
 */
enum class FieldKind {
    Id,
    Parent,
    Variant,
    Column
};

/**
 * @brief Comparison operation supported by indexed query predicates.
 */
enum class ComparisonOperator {
    Equal,
    NotEqual,
    Greater,
    Less,
    GreaterOrEqual,
    LessOrEqual
};

/**
 * @brief Sort direction for a query sort key.
 */
enum class SortDirection {
    Ascending,
    Descending
};

/**
 * @brief Boolean node kind used by filter expressions.
 */
enum class FilterOperator {
    And,
    Or,
    Not
};

/**
 * @brief Aggregation function supported by the query layer.
 */
enum class AggregateKind {
    Count,
    Sum,
    Minimum,
    Maximum
};

/**
 * @brief Semantic operation kind stored by ChangeOperation.
 */
enum class ChangeOperationKind {
    ItemInserted,
    ItemRemoved,
    ColumnUpdated,
    ComputedColumnUpdated,
    CascadeRemoved,
    ListInserted,
    ListRemoved,
    ListRotated
};

/**
 * @brief Options used when opening a write transaction.
 *
 * The default contract is undoable=true. A non-undoable transaction still commits
 * normally, but it does not create an UndoStep after commit.
 */
struct DINI_EXPORT TransactionOptions {
    bool undoable = true;
};

/**
 * @brief Explicit placement for association updates that attach an item to a list.
 *
 * When a list association changes from null or from another association value to a
 * non-null value, callers must supply targetIndex. The engine must not implicitly
 * append to the destination list group.
 */
struct DINI_EXPORT AssociationUpdateOptions {
    std::optional<std::size_t> targetIndex;
};

/**
 * @brief Parameters for the first-class list rotate operation.
 *
 * The operation is equivalent to std::rotate(first, first + normalizedOffset,
 * last) over the affected range, where first is startIndex, last is
 * startIndex + count, and normalizedOffset is offset modulo count in
 * [0, count). Positive offsets therefore rotate toward lower indexes; negative
 * offsets rotate toward higher indexes.
 */
struct DINI_EXPORT ListRotation {
    std::size_t startIndex = 0;
    std::size_t count = 0;
    std::ptrdiff_t offset = 0;
};

/**
 * @brief Diagnostic metadata shared by schema objects and handles.
 *
 * Names are optional and are not runtime identifiers. Querying, updating, serialization,
 * and recovery must use handles and stable numeric identities instead.
 */
struct DINI_EXPORT DiagnosticInfo {
    std::string debugName;
};

} // namespace dini

#endif // DINI_TYPES_H
