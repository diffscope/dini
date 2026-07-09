#ifndef DINI_ENGINE_H
#define DINI_ENGINE_H

#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <dini/change.h>
#include <dini/diniglobal.h>
#include <dini/event.h>
#include <dini/handles.h>
#include <dini/query.h>
#include <dini/schema.h>
#include <dini/transaction.h>
#include <dini/types.h>
#include <dini/value.h>
#include <dini/view.h>

namespace dini {

/**
 * @brief In-memory document storage engine instance for one document.
 *
 * DocumentEngine owns runtime document state, indexes, transactions, event
 * subscriptions, undo/redo stacks, ID generation state, and snapshot/log
 * serialization for one document. It does not own files, fsync policy, UI
 * bindings, multi-process access, or thread-level synchronization.
 */
class DINI_EXPORT DocumentEngine {
public:
    struct Impl;

    /**
     * @brief Creates a new empty document engine for a frozen schema.
     *
     * @param schema Immutable schema definition.
     * @pre schema must be a valid frozen EngineSchema.
     * @post The engine contains an empty document state and empty undo/redo history.
     * @throws SchemaError if schema is invalid or not usable for runtime instances.
     */
    explicit DocumentEngine(EngineSchema schema);

    /**
     * @brief Destroys the engine instance.
     *
     * @pre No write transaction may outlive the engine.
     * @post Runtime document state, subscriptions, and undo/redo history are released.
     */
    ~DocumentEngine();

    /**
     * @brief Copy construction is disabled because an engine owns one mutable document state.
     *
     * @pre None.
     * @post This overload is unavailable; create another engine from the same schema instead.
     */
    DocumentEngine(const DocumentEngine &other) = delete;

    /**
     * @brief Copy assignment is disabled because an engine owns one mutable document state.
     *
     * @pre None.
     * @post This overload is unavailable; use move assignment for ownership transfer.
     */
    DocumentEngine &operator=(const DocumentEngine &other) = delete;

    /**
     * @brief Moves an engine instance.
     *
     * @param other Engine to move from.
     * @pre other must not have active external transaction objects.
     * @post This engine owns the moved document state and other becomes invalid.
     */
    DocumentEngine(DocumentEngine &&other) noexcept;

    /**
     * @brief Move-assigns an engine instance.
     *
     * @param other Engine to move from.
     * @pre This engine must not have an active transaction; other must not have active external transaction objects.
     * @post This engine owns the moved document state and other becomes invalid.
     */
    DocumentEngine &operator=(DocumentEngine &&other) noexcept;

    /**
     * @brief Returns the immutable schema used by this engine.
     *
     * @pre The engine must be valid.
     * @post The engine state is not modified.
     * @throws SchemaError if the engine has been moved from.
     */
    EngineSchema schema() const;

    /**
     * @brief Starts a write transaction.
     *
     * @param options Transaction options; undoable defaults to true.
     * @pre No other write transaction may be active for this engine.
     * @post A Transaction object controls all writes until commit or rollback.
     * @throws TransactionError if another write transaction is active.
     */
    Transaction beginTransaction(TransactionOptions options = {});

    /**
     * @brief Reads a complete item snapshot by global ID.
     *
     * @param itemId Item to read.
     * @pre itemId must exist in current visible state.
     * @post The returned snapshot reflects current engine state at call time.
     * @throws QueryError if itemId does not exist.
     */
    ItemSnapshot read(ItemId itemId) const;

    /**
     * @brief Reads one column value by item ID and column handle.
     *
     * @param itemId Item to read.
     * @param column Column to read.
     * @pre itemId must exist and column must be legal for the item's container and variant.
     * @post The returned value reflects current engine state at call time.
     * @throws HandleError if column belongs to another schema; throws QueryError for invalid reads.
     */
    Value read(ItemId itemId, ColumnHandle column) const;

    std::optional<ItemSnapshot> previous(OrderedIndexHandle index,
                                         const ItemSnapshot &probe,
                                         const std::set<ItemId> &excludedIds = {}) const;

    std::optional<ItemSnapshot> next(OrderedIndexHandle index,
                                     const ItemSnapshot &probe,
                                     const std::set<ItemId> &excludedIds = {}) const;

    std::vector<ItemSnapshot> overlapping(IntervalIndexHandle index,
                                          const ItemSnapshot &probe,
                                          const std::set<ItemId> &excludedIds = {}) const;

    /**
     * @brief Tests whether an item currently exists.
     *
     * @param itemId Item ID to test.
     * @pre None.
     * @post Returns true only if itemId exists in current visible state.
     */
    bool contains(ItemId itemId) const;

    /**
     * @brief Returns the current length of one list instance.
     *
     * @param list List handle.
     * @param associationValue Association value identifying the list instance.
     * @pre list must belong to this engine schema.
     * @post The returned length reflects current engine state at call time.
     * @throws HandleError if list belongs to another schema.
     */
    std::size_t listLength(ListHandle list, const Value &associationValue) const;

    /**
     * @brief Creates a live view over all items in a table.
     *
     * @param table Source table.
     * @pre table must belong to this engine schema.
     * @post The returned view evaluates lazily against current engine state.
     * @throws HandleError if table is invalid for this schema.
     */
    View view(TableHandle table) const;

    /**
     * @brief Creates a live view over all elements in a list container.
     *
     * @param list Source list.
     * @pre list must belong to this engine schema.
     * @post The returned view evaluates lazily across current list elements.
     * @throws HandleError if list is invalid for this schema.
     */
    View view(ListHandle list) const;

    /**
     * @brief Creates a live query view over a table.
     *
     * @param table Source table.
     * @param spec Query filter and sort specification.
     * @pre table and every field in spec must be valid for this schema.
     * @post The returned view evaluates lazily against current engine state.
     * @throws QueryError if spec uses non-queryable fields.
     */
    View query(TableHandle table, const QuerySpec &spec) const;

    /**
     * @brief Creates a live query view over a list container.
     *
     * @param list Source list.
     * @param spec Query filter and sort specification.
     * @pre list and every field in spec must be valid for this schema.
     * @post The returned view evaluates lazily against current engine state.
     * @throws QueryError if spec uses non-queryable fields.
     */
    View query(ListHandle list, const QuerySpec &spec) const;

    /**
     * @brief Serializes the current document state.
     *
     * @pre No schema mismatch exists and the engine state is internally consistent.
     * @post The returned bytes contain document data but no undo/redo history.
     * @throws LogError if snapshot serialization fails.
     */
    ByteArray createSnapshot() const;

    /**
     * @brief Restores document state from snapshot bytes.
     *
     * @param snapshot Snapshot bytes produced by a compatible engine schema.
     * @pre snapshot must match this engine schema and version.
     * @post Document state, indexes, list positions, parent relations, and ID generation state are restored; undo/redo history is cleared.
     * @throws RecoveryError if bytes are corrupt, incompatible, or violate constraints.
     */
    void restoreSnapshot(const ByteArray &snapshot);

    /**
     * @brief Replays one persistent commit log after a compatible snapshot.
     *
     * @param commitLog Commit log bytes produced by a compatible engine schema.
     * @pre commitLog must be ordered after the current restored snapshot state.
     * @post Document state is advanced and undo/redo history remains empty.
     * @throws RecoveryError if replay fails because of corruption, schema mismatch, or ID conflict.
     */
    void replayCommitLog(const ByteArray &commitLog);

    /**
     * @brief Tests whether an undo step is currently available.
     *
     * @pre None.
     * @post The engine state is not modified.
     */
    bool canUndo() const noexcept;

    /**
     * @brief Tests whether a redo step is currently available.
     *
     * @pre None.
     * @post The engine state is not modified.
     */
    bool canRedo() const noexcept;

    /**
     * @brief Executes one undo special transaction.
     *
     * @pre No write transaction may be active and canUndo() should be true.
     * @post The target UndoStep is moved to the redo stack, document state is reverted, and commit log bytes are produced.
     * @throws TransactionError if no undo step exists or a write transaction is active; propagates after-hook exceptions after state changes.
     */
    CommitResult undo();

    /**
     * @brief Executes one redo special transaction.
     *
     * @pre No write transaction may be active and canRedo() should be true.
     * @post The target UndoStep is moved to the undo stack, document state is reapplied, and commit log bytes are produced.
     * @throws TransactionError if no redo step exists or a write transaction is active; propagates after-hook exceptions after state changes.
     */
    CommitResult redo();

    /**
     * @brief Clears the current in-memory undo and redo stacks.
     *
     * @pre None.
     * @post Document data is unchanged, no commit log is generated, and no hooks or events are triggered.
     */
    void clearUndoHistory();

    /**
     * @brief Returns public descriptors for currently undoable steps.
     *
     * @pre None.
     * @post The returned vector is a snapshot of in-memory undo history and is not persistent.
     */
    std::vector<UndoStep> undoHistory() const;

    /**
     * @brief Returns public descriptors for currently redoable steps.
     *
     * @pre None.
     * @post The returned vector is a snapshot of in-memory redo history and is not persistent.
     */
    std::vector<UndoStep> redoHistory() const;

    /**
     * @brief Subscribes to engine events through a pure C++ callback.
     *
     * @param callback Event callback.
     * @pre callback must be callable and must remain safe to invoke synchronously.
     * @post The returned Subscription controls future delivery to callback.
     * @throws std::bad_alloc if subscription storage cannot be allocated.
     */
    Subscription subscribe(EventCallback callback);

private:
    std::unique_ptr<Impl> _impl;
};

} // namespace dini

#endif // DINI_ENGINE_H
