#include "change_p.h"

#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <stdcorelib/pimpl.h>

#include <dini/errors.h>

namespace dini {

    namespace {

    template <typename... T>
    struct Overloaded : T... {
        using T::operator()...;
    };

    template <typename... T>
    Overloaded(T...) -> Overloaded<T...>;

    ChangeOperationKind operationKind(const ChangeOperation::Payload &payload) noexcept
    {
        return std::visit(Overloaded {
                              [](const ItemInsertedChange &) { return ChangeOperationKind::ItemInserted; },
                              [](const ItemRemovedChange &) { return ChangeOperationKind::ItemRemoved; },
                              [](const ColumnUpdatedChange &) { return ChangeOperationKind::ColumnUpdated; },
                              [](const ComputedColumnUpdatedChange &) {
                                  return ChangeOperationKind::ComputedColumnUpdated;
                              },
                              [](const CascadeRemovedChange &) { return ChangeOperationKind::CascadeRemoved; },
                              [](const ListInsertedChange &) { return ChangeOperationKind::ListInserted; },
                              [](const ListRemovedChange &) { return ChangeOperationKind::ListRemoved; },
                              [](const ListRotatedChange &) { return ChangeOperationKind::ListRotated; },
                          },
                          payload);
    }

    bool operationIsVolatile(const ChangeOperation::Payload &payload) noexcept
    {
        return std::visit(Overloaded {
                              [](const ItemInsertedChange &change) { return change.volatileData; },
                              [](const ItemRemovedChange &change) { return change.volatileData; },
                              [](const ColumnUpdatedChange &change) { return change.volatileData; },
                              [](const ComputedColumnUpdatedChange &change) { return change.volatileData; },
                              [](const CascadeRemovedChange &change) { return change.volatileData; },
                              [](const ListInsertedChange &change) { return change.volatileData; },
                              [](const ListRemovedChange &change) { return change.volatileData; },
                              [](const ListRotatedChange &change) { return change.volatileData; },
                          },
                          payload);
    }

    ChangeOperation invertOperation(const ChangeOperation &operation)
    {
        return std::visit(Overloaded {
                              [](const ItemInsertedChange &change) -> ChangeOperation {
                                  return ChangeOperation(ItemRemovedChange {
                                      .item = change.item,
                                      .cascade = false,
                                      .volatileData = change.volatileData,
                                  });
                              },
                              [](const ItemRemovedChange &change) -> ChangeOperation {
                                  return ChangeOperation(ItemInsertedChange {
                                      .item = change.item,
                                      .volatileData = change.volatileData,
                                  });
                              },
                              [](const ColumnUpdatedChange &change) -> ChangeOperation {
                                  auto inverseOptions = change.associationOptions;
                                  if (!change.oldValue.isNull()) {
                                      inverseOptions.targetIndex = change.oldListIndex;
                                  } else {
                                      inverseOptions.targetIndex.reset();
                                  }
                                  return ChangeOperation(ColumnUpdatedChange {
                                      .itemId = change.itemId,
                                      .column = change.column,
                                      .oldValue = change.newValue,
                                      .newValue = change.oldValue,
                                      .associationOptions = inverseOptions,
                                      .volatileData = change.volatileData,
                                      .oldListIndex = change.associationOptions.targetIndex,
                                  });
                              },
                              [](const ComputedColumnUpdatedChange &change) -> ChangeOperation {
                                  return ChangeOperation(ComputedColumnUpdatedChange {
                                      .itemId = change.itemId,
                                      .column = change.column,
                                      .oldValue = change.newValue,
                                      .newValue = change.oldValue,
                                      .volatileData = change.volatileData,
                                  });
                              },
                              [](const CascadeRemovedChange &change) -> ChangeOperation {
                                  return ChangeOperation(ItemInsertedChange {
                                      .item = change.item,
                                      .volatileData = change.volatileData,
                                  });
                              },
                              [](const ListInsertedChange &change) -> ChangeOperation {
                                  return ChangeOperation(ListRemovedChange {
                                      .list = change.list,
                                      .associationValue = change.associationValue,
                                      .index = change.index,
                                      .item = change.item,
                                      .volatileData = change.volatileData,
                                  });
                              },
                              [](const ListRemovedChange &change) -> ChangeOperation {
                                  return ChangeOperation(ListInsertedChange {
                                      .list = change.list,
                                      .associationValue = change.associationValue,
                                      .index = change.index,
                                      .item = change.item,
                                      .volatileData = change.volatileData,
                                  });
                              },
                              [](const ListRotatedChange &change) -> ChangeOperation {
                                  auto rotation = change.rotation;
                                  rotation.offset = -rotation.offset;
                                  return ChangeOperation(ListRotatedChange {
                                      .list = change.list,
                                      .associationValue = change.associationValue,
                                      .rotation = rotation,
                                      .volatileData = change.volatileData,
                                  });
                              },
                          },
                          operation.payload());
    }

    } // namespace

ChangeOperation::ChangeOperation() : _impl(new Impl(this))
{
}

ChangeOperation::ChangeOperation(Payload payload) : _impl(new Impl(this, std::move(payload)))
{
}

ChangeOperation::~ChangeOperation() = default;

ChangeOperation::ChangeOperation(const ChangeOperation &other) = default;

ChangeOperation::ChangeOperation(ChangeOperation &&other) noexcept = default;

ChangeOperation &ChangeOperation::operator=(const ChangeOperation &other) = default;

ChangeOperation &ChangeOperation::operator=(ChangeOperation &&other) noexcept = default;

ChangeOperationKind ChangeOperation::kind() const noexcept
{
    __stdc_impl_t;
    return operationKind(impl.payload);
}

const ChangeOperation::Payload &ChangeOperation::payload() const noexcept
{
    __stdc_impl_t;
    return impl.payload;
}

bool ChangeOperation::isVolatile() const noexcept
{
    __stdc_impl_t;
    return operationIsVolatile(impl.payload);
}

ChangeSet::ChangeSet() : _impl(new Impl(this))
{
}

ChangeSet::ChangeSet(std::vector<ChangeOperation> operations) : _impl(new Impl(this, std::move(operations)))
{
}

ChangeSet::~ChangeSet() = default;

ChangeSet::ChangeSet(const ChangeSet &other) = default;

ChangeSet::ChangeSet(ChangeSet &&other) noexcept = default;

ChangeSet &ChangeSet::operator=(const ChangeSet &other) = default;

ChangeSet &ChangeSet::operator=(ChangeSet &&other) noexcept = default;

bool ChangeSet::empty() const noexcept
{
    __stdc_impl_t;
    return impl.operations.empty();
}

const std::vector<ChangeOperation> &ChangeSet::operations() const noexcept
{
    __stdc_impl_t;
    return impl.operations;
}

const std::vector<DerivedChangeLink> &ChangeSet::derivedLinks() const noexcept
{
    __stdc_impl_t;
    return impl.derivedLinks;
}

void ChangeSet::append(ChangeOperation operation)
{
    __stdc_impl_t;
    impl.operations.push_back(std::move(operation));
}

void ChangeSet::addDerivedLink(DerivedChangeLink link)
{
    __stdc_impl_t;
    if (link.sourceOperation >= impl.operations.size() || link.derivedOperation >= impl.operations.size()) {
        throw std::out_of_range("derived change link operation index is outside the change set");
    }
    impl.derivedLinks.push_back(link);
}

ChangeSet ChangeSet::invert() const
{
    __stdc_impl_t;

    std::vector<ChangeOperation> invertedOperations;
    invertedOperations.reserve(impl.operations.size());

    for (auto it = impl.operations.rbegin(); it != impl.operations.rend(); ++it) {
        invertedOperations.push_back(invertOperation(*it));
    }

    return ChangeSet(std::move(invertedOperations));
}

ChangeSet ChangeSet::filterVolatile() const
{
    __stdc_impl_t;

    ChangeSet filtered;
    std::vector<std::size_t> indexMap(impl.operations.size(), impl.operations.size());

    for (std::size_t i = 0; i < impl.operations.size(); ++i) {
        const auto &operation = impl.operations[i];
        if (!operation.isVolatile()) {
            indexMap[i] = filtered.operations().size();
            filtered.append(operation);
        }
    }

    for (const auto &link : impl.derivedLinks) {
        if (link.sourceOperation >= indexMap.size() || link.derivedOperation >= indexMap.size()) {
            continue;
        }

        const auto sourceOperation = indexMap[link.sourceOperation];
        const auto derivedOperation = indexMap[link.derivedOperation];
        if (sourceOperation != impl.operations.size() && derivedOperation != impl.operations.size()) {
            filtered.addDerivedLink(DerivedChangeLink {
                .sourceOperation = sourceOperation,
                .derivedOperation = derivedOperation,
            });
        }
    }

    return filtered;
}

ByteArray ChangeSet::serializeForLog() const
{
    return serializeChangeSetForLog(*this);
}

ChangeSet ChangeSet::merge(const std::vector<ChangeSet> &changes)
{
    ChangeSet merged;

    for (const auto &changeSet : changes) {
        const auto operationOffset = merged.operations().size();

        for (const auto &operation : changeSet.operations()) {
            merged.append(operation);
        }

        for (const auto &link : changeSet.derivedLinks()) {
            merged.addDerivedLink(DerivedChangeLink {
                .sourceOperation = operationOffset + link.sourceOperation,
                .derivedOperation = operationOffset + link.derivedOperation,
            });
        }
    }

    return merged;
}

UndoStep::UndoStep() : _impl(new Impl(this))
{
}

UndoStep::UndoStep(ChangeSet changeSet, std::string label)
    : _impl(new Impl(this, std::move(changeSet), std::move(label)))
{
}

UndoStep::~UndoStep() = default;

UndoStep::UndoStep(const UndoStep &other) = default;

UndoStep::UndoStep(UndoStep &&other) noexcept = default;

UndoStep &UndoStep::operator=(const UndoStep &other) = default;

UndoStep &UndoStep::operator=(UndoStep &&other) noexcept = default;

const ChangeSet &UndoStep::changeSet() const noexcept
{
    __stdc_impl_t;
    return impl.changeSet;
}

const std::string &UndoStep::label() const noexcept
{
    __stdc_impl_t;
    return impl.label;
}

} // namespace dini
