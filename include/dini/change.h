#ifndef DINI_CHANGE_H
#define DINI_CHANGE_H

#include <cstddef>
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

/**
 * @brief Associates one column handle with one runtime value.
 *
 * ColumnValue is used by inserts, item snapshots, and change records. The column
 * must belong to the container relevant to the surrounding operation.
 */
struct DINI_EXPORT ColumnValue {
    ColumnHandle column;
    Value value;
};

/**
 * @brief Captures the public state required to restore or report one item.
 *
 * ItemSnapshot is a value-level description of an entity row or list element. It
 * is suitable for ChangeSet payloads, undo/redo records, event data, and future
 * serialization, but does not expose implementation storage.
 */
struct DINI_EXPORT ItemSnapshot {
    ItemId id = 0;
    ContainerKind containerKind = ContainerKind::Table;
    ContainerId containerId = 0;
    std::optional<ItemId> parentId;
    std::optional<VariantHandle> variant;
    std::vector<ColumnValue> values;
    std::optional<Value> listAssociationValue;
    std::optional<std::size_t> listIndex;
};

/**
 * @brief Payload for an inserted table row or list element.
 *
 * The snapshot contains the assigned item ID, initial values, parent relation,
 * variant, and list position when applicable.
 */
struct DINI_EXPORT ItemInsertedChange {
    ItemSnapshot item;
};

/**
 * @brief Payload for a removed table row or list element.
 *
 * The snapshot contains enough public information for undo, event consumers, and
 * log serialization to understand the removed state.
 */
struct DINI_EXPORT ItemRemovedChange {
    ItemSnapshot item;
    bool cascade = false;
};

/**
 * @brief Payload for a column value update.
 *
 * Parent relation changes and list association changes are represented by this
 * same payload. When a list association becomes non-null, placement must be
 * supplied through associationOptions.
 */
struct DINI_EXPORT ColumnUpdatedChange {
    ItemId itemId = 0;
    ColumnHandle column;
    Value oldValue;
    Value newValue;
    AssociationUpdateOptions associationOptions;
    std::optional<std::size_t> oldListIndex;
};

/**
 * @brief Payload for a materialized computed column update.
 *
 * Computed column changes are recorded separately so observers can distinguish
 * user-requested updates from automatic dependency propagation.
 */
struct DINI_EXPORT ComputedColumnUpdatedChange {
    ItemId itemId = 0;
    ColumnHandle column;
    Value oldValue;
    Value newValue;
};

/**
 * @brief Payload for a cascade removal caused by composition semantics.
 *
 * Cascade removals are first-class ChangeSet entries so observers and undo logic
 * can explain why a descendant item disappeared.
 */
struct DINI_EXPORT CascadeRemovedChange {
    ItemSnapshot item;
    ItemId ancestorId = 0;
};

/**
 * @brief Payload for the first-class list insert operation.
 *
 * The association value and index identify the concrete list instance and
 * insertion position. Appending is represented by index == current length.
 */
struct DINI_EXPORT ListInsertedChange {
    ListHandle list;
    Value associationValue;
    std::size_t index = 0;
    ItemSnapshot item;
};

/**
 * @brief Payload for the first-class list remove operation.
 *
 * The snapshot captures the removed element and its previous list position.
 */
struct DINI_EXPORT ListRemovedChange {
    ListHandle list;
    Value associationValue;
    std::size_t index = 0;
    ItemSnapshot item;
};

/**
 * @brief Payload for the first-class list rotate operation.
 *
 * Rotation preserves list operation semantics instead of exposing only the final
 * order. The affected range is within a single association-value group.
 */
struct DINI_EXPORT ListRotatedChange {
    ListHandle list;
    Value associationValue;
    ListRotation rotation;
};

/**
 * @brief Describes that one operation was derived from another operation.
 *
 * Derived links allow hooks and computed columns to preserve causal information
 * without changing the ordered operation stream.
 */
struct DINI_EXPORT DerivedChangeLink {
    std::size_t sourceOperation = 0;
    std::size_t derivedOperation = 0;
};

/**
 * @brief One semantic operation inside a ChangeSet.
 *
 * ChangeOperation is variant-style and intentionally preserves operation meaning.
 * Implementations must not collapse list operations or cascade removals into only
 * final state summaries.
 */
class DINI_EXPORT ChangeOperation {
public:
    using Payload = std::variant<ItemInsertedChange,
                                 ItemRemovedChange,
                                 ColumnUpdatedChange,
                                 ComputedColumnUpdatedChange,
                                 CascadeRemovedChange,
                                 ListInsertedChange,
                                 ListRemovedChange,
                                 ListRotatedChange>;

    /**
     * @brief Creates an empty inserted-item operation placeholder.
     *
     * @pre None.
     * @post kind() identifies the default payload alternative.
     */
    ChangeOperation();

    /**
     * @brief Creates an operation from a semantic payload.
     *
     * @param payload Operation payload.
     * @pre payload must describe one valid semantic change.
     * @post kind() matches the payload alternative.
     */
    explicit ChangeOperation(Payload payload);

    /**
     * @brief Destroys the change operation wrapper.
     *
     * @pre No public precondition.
     * @post The shared private operation data is released when no operation references it.
     */
    ~ChangeOperation();

    /**
     * @brief Copies a change operation with implicit shared private data.
     *
     * @param other Operation to copy.
     * @pre other may contain any supported operation payload.
     * @post This operation describes the same semantic change as other.
     */
    ChangeOperation(const ChangeOperation &other);

    /**
     * @brief Moves a change operation.
     *
     * @param other Operation to move from.
     * @pre other may contain any supported operation payload.
     * @post This operation receives other's private data and other remains valid.
     */
    ChangeOperation(ChangeOperation &&other) noexcept;

    /**
     * @brief Copy-assigns a change operation with implicit shared private data.
     *
     * @param other Operation to copy.
     * @pre other may contain any supported operation payload.
     * @post This operation describes the same semantic change as other.
     */
    ChangeOperation &operator=(const ChangeOperation &other);

    /**
     * @brief Move-assigns a change operation.
     *
     * @param other Operation to move from.
     * @pre other may contain any supported operation payload.
     * @post This operation receives other's private data and other remains valid.
     */
    ChangeOperation &operator=(ChangeOperation &&other) noexcept;

    /**
     * @brief Returns the semantic kind of this operation.
     *
     * @pre None.
     * @post The returned kind corresponds to payload().
     */
    ChangeOperationKind kind() const noexcept;

    /**
     * @brief Returns the stored operation payload.
     *
     * @pre None.
     * @post The returned reference remains valid while this operation is unchanged.
     */
    const Payload &payload() const noexcept;

private:
    struct Impl;
    SharedDataPointer<Impl> _impl;
};

/**
 * @brief Ordered semantic description of changes produced by updates or replay.
 *
 * ChangeSet is the central event, rollback, undo, redo, and logging payload. It
 * preserves operation order and first-class list semantics while allowing callers
 * to derive inverse and merged forms.
 */
class DINI_EXPORT ChangeSet {
public:
    /**
     * @brief Creates an empty change set.
     *
     * @pre None.
     * @post empty() returns true.
     */
    ChangeSet();

    /**
     * @brief Creates a change set from ordered operations.
     *
     * @param operations Semantic operation sequence.
     * @pre operations must be in application order.
     * @post operations() returns the supplied operation sequence.
     */
    explicit ChangeSet(std::vector<ChangeOperation> operations);

    /**
     * @brief Destroys the change set wrapper.
     *
     * @pre No public precondition.
     * @post The shared private change data is released when no ChangeSet references it.
     */
    ~ChangeSet();

    /**
     * @brief Copies a change set with implicit shared private data.
     *
     * @param other Change set to copy.
     * @pre other may be empty or non-empty.
     * @post This change set describes the same ordered operations as other.
     */
    ChangeSet(const ChangeSet &other);

    /**
     * @brief Moves a change set.
     *
     * @param other Change set to move from.
     * @pre other may be empty or non-empty.
     * @post This change set receives other's private data and other remains valid.
     */
    ChangeSet(ChangeSet &&other) noexcept;

    /**
     * @brief Copy-assigns a change set with implicit shared private data.
     *
     * @param other Change set to copy.
     * @pre other may be empty or non-empty.
     * @post This change set describes the same ordered operations as other.
     */
    ChangeSet &operator=(const ChangeSet &other);

    /**
     * @brief Move-assigns a change set.
     *
     * @param other Change set to move from.
     * @pre other may be empty or non-empty.
     * @post This change set receives other's private data and other remains valid.
     */
    ChangeSet &operator=(ChangeSet &&other) noexcept;

    /**
     * @brief Tests whether the change set contains no operations.
     *
     * @pre None.
     * @post Does not modify the change set.
     */
    bool empty() const noexcept;

    /**
     * @brief Returns the ordered semantic operations.
     *
     * @pre None.
     * @post The returned reference remains valid while this ChangeSet is unchanged.
     */
    const std::vector<ChangeOperation> &operations() const noexcept;

    /**
     * @brief Returns derived-operation causal links.
     *
     * @pre None.
     * @post The returned reference remains valid while this ChangeSet is unchanged.
     */
    const std::vector<DerivedChangeLink> &derivedLinks() const noexcept;

    /**
     * @brief Appends one operation to the end of the change set.
     *
     * @param operation Operation to append.
     * @pre operation must be semantically valid for the surrounding transaction.
     * @post operations().back() is the appended operation.
     */
    void append(ChangeOperation operation);

    /**
     * @brief Records that one operation was derived from another operation.
     *
     * @param link Source and derived operation indexes.
     * @pre Both indexes must refer to existing operations.
     * @post derivedLinks() contains the supplied link.
     * @throws std::out_of_range if an index is outside the current operation list.
     */
    void addDerivedLink(DerivedChangeLink link);

    /**
     * @brief Produces the inverse change set needed to restore prior state.
     *
     * @pre The change set must contain enough old state for every invertible operation.
     * @post The returned ChangeSet applies inverse operations in a valid order.
     * @throws DiniError if an operation cannot be inverted from public payload data.
     */
    ChangeSet invert() const;

    /**
     * @brief Serializes this change set into commit-log bytes.
     *
     * @pre The change set must be compatible with the active schema.
     * @post The returned bytes can be passed to DocumentEngine::replayCommitLog for a compatible schema.
     * @throws LogError if serialization cannot represent an operation.
     */
    ByteArray serializeForLog() const;

    /**
     * @brief Merges multiple change sets while preserving their operation order.
     *
     * @param changes Change sets to concatenate.
     * @pre changes must be supplied in semantic application order.
     * @post The returned ChangeSet contains all operations and adjusted derived links.
     */
    static ChangeSet merge(const std::vector<ChangeSet> &changes);

private:
    struct Impl;
    SharedDataPointer<Impl> _impl;
};

/**
 * @brief Non-persistent undo history entry for one committed transaction.
 *
 * UndoStep records the committed ChangeSet and user-facing metadata needed by
 * the current memory session. It is not included in snapshots or logs.
 */
class DINI_EXPORT UndoStep {
public:
    /**
     * @brief Creates an empty undo step.
     *
     * @pre None.
     * @post changeSet().empty() returns true.
     */
    UndoStep();

    /**
     * @brief Creates an undo step from a committed change set.
     *
     * @param changeSet Committed change set.
     * @param label Optional diagnostic label.
     * @pre changeSet should describe a committed transaction.
     * @post changeSet() returns the supplied change set.
     */
    UndoStep(ChangeSet changeSet, std::string label = {});

    /**
     * @brief Destroys the undo step wrapper.
     *
     * @pre No public precondition.
     * @post The shared private undo step data is released when no UndoStep references it.
     */
    ~UndoStep();

    /**
     * @brief Copies an undo step with implicit shared private data.
     *
     * @param other Undo step to copy.
     * @pre other may be empty or non-empty.
     * @post This undo step describes the same undo metadata as other.
     */
    UndoStep(const UndoStep &other);

    /**
     * @brief Moves an undo step.
     *
     * @param other Undo step to move from.
     * @pre other may be empty or non-empty.
     * @post This undo step receives other's private data and other remains valid.
     */
    UndoStep(UndoStep &&other) noexcept;

    /**
     * @brief Copy-assigns an undo step with implicit shared private data.
     *
     * @param other Undo step to copy.
     * @pre other may be empty or non-empty.
     * @post This undo step describes the same undo metadata as other.
     */
    UndoStep &operator=(const UndoStep &other);

    /**
     * @brief Move-assigns an undo step.
     *
     * @param other Undo step to move from.
     * @pre other may be empty or non-empty.
     * @post This undo step receives other's private data and other remains valid.
     */
    UndoStep &operator=(UndoStep &&other) noexcept;

    /**
     * @brief Returns the forward change set represented by this step.
     *
     * @pre None.
     * @post The returned reference remains valid while this UndoStep is unchanged.
     */
    const ChangeSet &changeSet() const noexcept;

    /**
     * @brief Returns the optional diagnostic label.
     *
     * @pre None.
     * @post The returned label has no persistence semantics.
     */
    const std::string &label() const noexcept;

private:
    struct Impl;
    SharedDataPointer<Impl> _impl;
};

} // namespace dini

#endif // DINI_CHANGE_H
