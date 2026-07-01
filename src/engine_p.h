#ifndef DINI_ENGINE_P_H
#define DINI_ENGINE_P_H

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
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

struct DocumentEngine::Impl {
    EngineSchema schema;
    std::unordered_map<ItemId, RuntimeItem> items;
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
    std::unordered_map<ItemId, RuntimeItem> rollbackItems;
    std::map<std::pair<ContainerId, std::string>, std::vector<ItemId>> rollbackListGroups;
    RuntimeIndexStore rollbackIndexes;
    bool rollbackApplied = false;
};

struct TransactionContext::Impl {
    Transaction::Impl *transaction = nullptr;
    EventOrigin origin = EventOrigin::Normal;
    bool mutationAllowed = false;
};

} // namespace dini

#endif // DINI_ENGINE_P_H
