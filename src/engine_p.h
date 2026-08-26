#ifndef DINI_ENGINE_P_H
#define DINI_ENGINE_P_H

#include <algorithm>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <dini/engine.h>

#include "event_p.h"
#include "schema_p.h"
#include "support/runtime_index.h"

namespace dini {

struct RuntimeItem {
    ItemSnapshot snapshot;
};

class RollbackItemStore final {
public:
    using InsertedSet = std::pmr::unordered_set<ItemId>;
    using ExistingMap = std::pmr::unordered_map<ItemId, RuntimeItem>;

    RollbackItemStore() : _insertedItems(&_pool), _existingItems(&_pool) {}

    RollbackItemStore(const RollbackItemStore &other) : RollbackItemStore()
    {
        _insertedItems.reserve(other._insertedItems.size());
        _insertedItems.insert(other._insertedItems.begin(), other._insertedItems.end());
        _existingItems.reserve(other._existingItems.size());
        _existingItems.insert(other._existingItems.begin(), other._existingItems.end());
    }

    RollbackItemStore &operator=(const RollbackItemStore &other)
    {
        if (this != &other) {
            InsertedSet replacementInserted(&_pool);
            replacementInserted.reserve(other._insertedItems.size());
            replacementInserted.insert(other._insertedItems.begin(), other._insertedItems.end());
            ExistingMap replacementExisting(&_pool);
            replacementExisting.reserve(other._existingItems.size());
            replacementExisting.insert(other._existingItems.begin(), other._existingItems.end());
            _insertedItems = std::move(replacementInserted);
            _existingItems = std::move(replacementExisting);
        }
        return *this;
    }

    bool contains(ItemId itemId) const
    {
        return _insertedItems.find(itemId) != _insertedItems.end() ||
               _existingItems.find(itemId) != _existingItems.end();
    }

    void reserveInsertedForGrowth(std::size_t additional)
    {
        if (additional == 0) {
            return;
        }
        const auto required = _insertedItems.size() + additional;
        const auto currentCapacity = static_cast<std::size_t>(
            static_cast<double>(_insertedItems.bucket_count()) *
            static_cast<double>(_insertedItems.max_load_factor()));
        if (required <= currentCapacity) {
            return;
        }
        const auto geometricCapacity = _insertedItems.size() +
                                       std::max(_insertedItems.size() / 2, std::size_t {64});
        _insertedItems.reserve(std::max(required, geometricCapacity));
    }

    void recordInserted(ItemId itemId) { _insertedItems.emplace(itemId); }
    void recordExisting(ItemId itemId, const RuntimeItem &item) { _existingItems.emplace(itemId, item); }
    const InsertedSet &insertedItems() const noexcept { return _insertedItems; }
    const ExistingMap &existingItems() const noexcept { return _existingItems; }

private:
    std::pmr::unsynchronized_pool_resource _pool;
    InsertedSet _insertedItems;
    ExistingMap _existingItems;
};

struct DocumentEngine::Impl {
    EngineSchema schema;
    std::pmr::unsynchronized_pool_resource itemPool;
    std::pmr::unordered_map<ItemId, RuntimeItem> items {&itemPool};
    std::map<std::pair<ContainerId, std::string>, std::vector<ItemId>> listGroups;
    RuntimeIndexStore indexes;
    std::vector<UndoStep> undoStack;
    std::vector<UndoStep> redoStack;
    std::vector<std::weak_ptr<SubscriptionState>> subscriptions;
    std::uint64_t epochSeconds = 0;
    std::uint32_t currentElapsedSecond = 0;
    std::uint32_t currentSecondCounter = 0;
    bool activeTransaction = false;
    bool valid = true;
    std::optional<HookStage> hookStage;
    std::size_t hookDepth = 0;

    explicit Impl(EngineSchema schema) : schema(std::move(schema)) {}
};

struct Transaction::Impl {
    DocumentEngine *owner = nullptr;
    DocumentEngine::Impl *engine = nullptr;
    TransactionOptions options;
    TransactionState state = TransactionState::Failed;
    EventOrigin origin = EventOrigin::Normal;
    ChangeSet changeSet;
    ItemSnapshot *pendingBeforeApplySnapshot = nullptr;
    bool pendingBeforeApplyInsert = false;
    ColumnId pendingBeforeApplyPrimaryColumn = 0;
    std::vector<ColumnId> pendingBeforeApplyPrimaryColumns;
    RollbackItemStore rollbackItems;
    std::map<std::pair<ContainerId, std::string>, std::optional<std::vector<ItemId>>> rollbackListGroups;
    bool rollbackApplied = false;
};

struct TransactionContext::Impl {
    Transaction::Impl *transaction = nullptr;
    EventOrigin origin = EventOrigin::Normal;
    bool mutationAllowed = false;
};

} // namespace dini

#endif // DINI_ENGINE_P_H
