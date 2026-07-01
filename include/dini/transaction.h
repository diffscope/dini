#ifndef DINI_TRANSACTION_H
#define DINI_TRANSACTION_H

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include <dini/change.h>
#include <dini/diniglobal.h>
#include <dini/handles.h>
#include <dini/types.h>
#include <dini/value.h>

namespace dini {

class DocumentEngine;

/**
 * @brief Result of a successfully committed ordinary, undo, or redo transaction.
 *
 * CommitResult carries the public semantic change set and persistent log bytes.
 * For ordinary transactions, createdUndoStep indicates whether the committed
 * change was pushed to the undo stack.
 */
struct DINI_EXPORT CommitResult {
    ChangeSet changeSet;
    ByteArray commitLog;
    EventOrigin origin = EventOrigin::Normal;
    bool createdUndoStep = false;
    bool clearedRedoStack = false;
};

/**
 * @brief Mutation facade passed to schema hooks.
 *
 * TransactionContext exposes controlled write operations to hook callbacks. Only
 * BeforeApply hooks may call mutating methods; calls from other stages must throw
 * HookError and mark the surrounding transaction failed.
 */
class DINI_EXPORT TransactionContext {
public:
    struct Impl;

    /**
     * @brief Creates an invalid context.
     *
     * @pre None.
     * @post Mutating calls must fail until bound to an active transaction by the engine.
     */
    TransactionContext();

    /**
     * @brief Destroys the hook mutation context wrapper.
     *
     * @pre No public precondition.
     * @post The private context reference is released.
     */
    ~TransactionContext();

    TransactionContext(const TransactionContext &other) = delete;
    TransactionContext &operator=(const TransactionContext &other) = delete;

    /**
     * @brief Moves a hook mutation context wrapper.
     *
     * @param other Context to move from.
     * @pre other may be valid or invalid.
     * @post This context receives other's private state and other becomes invalid.
     */
    TransactionContext(TransactionContext &&other) noexcept;

    /**
     * @brief Move-assigns a hook mutation context wrapper.
     *
     * @param other Context to move from.
     * @pre other may be valid or invalid.
     * @post This context receives other's private state and other becomes invalid.
     */
    TransactionContext &operator=(TransactionContext &&other) noexcept;

    /**
     * @brief Returns the origin of the surrounding transaction.
     *
     * @pre The context must be bound to an active hook invocation.
     * @post The context is not modified.
     * @throws TransactionError if the context is invalid.
     */
    EventOrigin origin() const;

    /**
     * @brief Returns the document engine that owns the surrounding transaction.
     *
     * @pre The context must be bound to an active hook invocation.
     * @post The context is not modified.
     * @throws TransactionError if the context is invalid.
     */
    DocumentEngine &engine();

    /**
     * @brief Returns the document engine that owns the surrounding transaction.
     *
     * @pre The context must be bound to an active hook invocation.
     * @post The context is not modified.
     * @throws TransactionError if the context is invalid.
     */
    const DocumentEngine &engine() const;

    /**
     * @brief Inserts a table row from inside an allowed hook.
     *
     * @param table Target table.
     * @param values Initial column values.
     * @param variant Optional polymorphic variant for polymorphic tables.
     * @pre The context must be in a BeforeApply hook and table must belong to the active schema.
     * @post A new globally unique item is visible in the engine state.
     * @throws HookError if mutation is forbidden in the current hook stage.
     */
    ItemId insert(TableHandle table, std::vector<ColumnValue> values, std::optional<VariantHandle> variant = {});

    /**
     * @brief Inserts a list element from inside an allowed hook.
     *
     * @param list Target list.
     * @param associationValue Non-null association value identifying the list instance.
     * @param index Insertion index inside the association-value group.
     * @param values Initial column values.
     * @param variant Optional polymorphic variant for polymorphic lists.
     * @pre The context must be in a BeforeApply hook and index must be within the current list length.
     * @post A new globally unique list element is visible at the requested position.
     * @throws HookError if mutation is forbidden in the current hook stage.
     */
    ItemId insert(ListHandle list, Value associationValue, std::size_t index, std::vector<ColumnValue> values, std::optional<VariantHandle> variant = {});

    /**
     * @brief Removes an item by ID from inside an allowed hook.
     *
     * @param itemId Item to remove.
     * @pre The context must be in a BeforeApply hook and itemId must exist.
     * @post The item and its composed descendants are removed from the visible state.
     * @throws HookError if mutation is forbidden in the current hook stage.
     */
    void remove(ItemId itemId);

    /**
     * @brief Updates a column value from inside an allowed hook.
     *
     * @param itemId Item to update.
     * @param column Column to update.
     * @param value New value.
     * @param options Optional list placement required for non-null list association updates.
     * @pre The context must be in a BeforeApply hook and column must be writable for the item.
     * @post The new value is visible and computed columns are updated as needed.
     * @throws HookError if mutation is forbidden in the current hook stage.
     */
    void update(ItemId itemId, ColumnHandle column, Value value, AssociationUpdateOptions options = {});

    /**
     * @brief Rotates a range inside one list instance from inside an allowed hook.
     *
     * @param list Target list.
     * @param associationValue Association value identifying the list instance.
     * @param rotation Range and offset to rotate.
     * @pre The context must be in a BeforeApply hook and the range must be valid.
     * @post The list instance keeps contiguous indexes with the range rotated.
     * @throws HookError if mutation is forbidden in the current hook stage.
     */
    void rotate(ListHandle list, Value associationValue, ListRotation rotation);

private:
    std::unique_ptr<Impl> _impl;

    explicit TransactionContext(std::unique_ptr<Impl> data);
    friend class DocumentEngine;
    friend class Transaction;
};

/**
 * @brief RAII write transaction for a DocumentEngine instance.
 *
 * Transaction is the only public write path for ordinary updates. Updates become
 * globally readable immediately after application, while commit produces logs and
 * undo history and rollback applies inverse changes.
 */
class DINI_EXPORT Transaction {
public:
    struct Impl;

    /**
     * @brief Creates an invalid inactive transaction.
     *
     * @pre None.
     * @post state() is not Active and mutating calls must throw.
     */
    Transaction();

    /**
     * @brief Rolls back any active transaction and destroys the wrapper.
     *
     * @pre No public precondition.
     * @post If the transaction was still active, rollback has been attempted without throwing from the destructor.
     */
    ~Transaction();

    /**
     * @brief Copy construction is disabled because a transaction has unique RAII ownership.
     *
     * @pre None.
     * @post This overload is unavailable; use move construction to transfer ownership.
     */
    Transaction(const Transaction &other) = delete;

    /**
     * @brief Copy assignment is disabled because a transaction has unique RAII ownership.
     *
     * @pre None.
     * @post This overload is unavailable; use move assignment to transfer ownership.
     */
    Transaction &operator=(const Transaction &other) = delete;

    /**
     * @brief Moves a transaction wrapper.
     *
     * @param other Transaction to move from.
     * @pre other may be active, completed, failed, or invalid.
     * @post This object owns the moved transaction state and other becomes invalid.
     */
    Transaction(Transaction &&other) noexcept;

    /**
     * @brief Move-assigns a transaction wrapper.
     *
     * @param other Transaction to move from.
     * @pre If this transaction is active, it is rolled back before assignment.
     * @post This object owns the moved transaction state and other becomes invalid.
     */
    Transaction &operator=(Transaction &&other) noexcept;

    /**
     * @brief Returns the current transaction lifecycle state.
     *
     * @pre None.
     * @post The transaction is not modified.
     */
    TransactionState state() const noexcept;

    /**
     * @brief Returns the document engine that owns this transaction.
     *
     * @pre The transaction wrapper must be valid.
     * @post The transaction is not modified.
     * @throws TransactionError if this wrapper is invalid.
     */
    DocumentEngine &engine();

    /**
     * @brief Returns the document engine that owns this transaction.
     *
     * @pre The transaction wrapper must be valid.
     * @post The transaction is not modified.
     * @throws TransactionError if this wrapper is invalid.
     */
    const DocumentEngine &engine() const;

    /**
     * @brief Returns the semantic changes accumulated so far.
     *
     * @pre The transaction must be active, failed, or completed with accessible history.
     * @post The returned ChangeSet reflects operations applied by this transaction.
     * @throws TransactionError if this wrapper is invalid.
     */
    const ChangeSet &changeSet() const;

    /**
     * @brief Inserts a table row.
     *
     * @param table Target table.
     * @param values Initial column values.
     * @param variant Optional polymorphic variant for polymorphic tables.
     * @pre The transaction must be active and table must belong to the engine schema.
     * @post A new globally unique item is visible in the engine state.
     * @throws TransactionError if the transaction is inactive or failed.
     */
    ItemId insert(TableHandle table, std::vector<ColumnValue> values, std::optional<VariantHandle> variant = {});

    /**
     * @brief Inserts a list element at an explicit position.
     *
     * @param list Target list.
     * @param associationValue Non-null association value identifying the list instance.
     * @param index Insertion index inside the association-value group.
     * @param values Initial column values.
     * @param variant Optional polymorphic variant for polymorphic lists.
     * @pre The transaction must be active and index must be within the current list length.
     * @post A new globally unique list element is visible at the requested position.
     * @throws TransactionError if the transaction is inactive or failed.
     */
    ItemId insert(ListHandle list, Value associationValue, std::size_t index, std::vector<ColumnValue> values, std::optional<VariantHandle> variant = {});

    /**
     * @brief Removes an item by global ID.
     *
     * @param itemId Item to remove.
     * @pre The transaction must be active and itemId must exist.
     * @post The item and composed descendants are removed from visible state.
     * @throws TransactionError if the transaction is inactive or failed.
     */
    void remove(ItemId itemId);

    /**
     * @brief Removes a list element at a concrete list position.
     *
     * @param list Target list.
     * @param associationValue Association value identifying the list instance.
     * @param index Element index to remove.
     * @pre The transaction must be active and index must identify an existing element.
     * @post The element is removed and following indexes remain contiguous.
     * @throws TransactionError if the transaction is inactive or failed.
     */
    void removeAt(ListHandle list, Value associationValue, std::size_t index);

    /**
     * @brief Updates a writable column value.
     *
     * @param itemId Item to update.
     * @param column Column to update.
     * @param value New value.
     * @param options Optional list placement required for non-null list association updates.
     * @pre The transaction must be active and column must be legal for the item.
     * @post The new value is visible and dependent computed columns are updated.
     * @throws TransactionError if the transaction is inactive or failed.
     */
    void update(ItemId itemId, ColumnHandle column, Value value, AssociationUpdateOptions options = {});

    /**
     * @brief Rotates a range inside one list instance.
     *
     * @param list Target list.
     * @param associationValue Association value identifying the list instance.
     * @param rotation Range and offset to rotate.
     * @pre The transaction must be active and the rotation range must be valid.
     * @post The list instance keeps contiguous indexes with the range rotated.
     * @throws TransactionError if the transaction is inactive or failed.
     */
    void rotate(ListHandle list, Value associationValue, ListRotation rotation);

    /**
     * @brief Commits the active transaction.
     *
     * @pre The transaction must be active and all before-commit hooks must accept the final ChangeSet.
     * @post The transaction is completed, persistent log bytes are produced, and undo/redo stacks are updated.
     * @throws TransactionError if the transaction is failed or inactive; propagates hook and constraint exceptions.
     */
    CommitResult commit();

    /**
     * @brief Rolls back the active or failed transaction.
     *
     * @pre The transaction must not already be committed or rolled back.
     * @post Changes are reversed, rollback events are published, and the transaction is completed.
     * @throws TransactionError if rollback cannot be performed.
     */
    void rollback();

private:
    std::unique_ptr<Impl> _impl;

    explicit Transaction(std::unique_ptr<Impl> data);
    friend class DocumentEngine;
    friend class TransactionContext;
};

} // namespace dini

#endif // DINI_TRANSACTION_H
