#include "handles_p.h"

#include <tuple>
#include <utility>

#include <stdcorelib/pimpl.h>

namespace dini {

    namespace {

    template <typename Impl>
    bool validContainerHandle(const Impl &impl) noexcept
    {
        return impl.schemaId != 0 && impl.containerId != 0;
    }

    template <typename Impl>
    const std::string &debugNameOf(const Impl &impl) noexcept
    {
        return impl.debugName;
    }

    } // namespace

TableHandle::TableHandle() noexcept : _impl(new Impl(this)) {}

TableHandle::TableHandle(SchemaId schemaId, ContainerId containerId, std::string debugName) : _impl(new Impl(this))
{
    __stdc_impl_t;
    impl.schemaId = schemaId;
    impl.containerId = containerId;
    impl.debugName = std::move(debugName);
}

TableHandle::~TableHandle() = default;
TableHandle::TableHandle(const TableHandle &other) = default;
TableHandle::TableHandle(TableHandle &&other) noexcept = default;
TableHandle &TableHandle::operator=(const TableHandle &other) = default;
TableHandle &TableHandle::operator=(TableHandle &&other) noexcept = default;

bool TableHandle::isValid() const noexcept
{
    __stdc_impl_t;
    return validContainerHandle(impl);
}

SchemaId TableHandle::schemaId() const noexcept
{
    __stdc_impl_t;
    return impl.schemaId;
}

ContainerId TableHandle::containerId() const noexcept
{
    __stdc_impl_t;
    return impl.containerId;
}

const std::string &TableHandle::debugName() const noexcept
{
    __stdc_impl_t;
    return debugNameOf(impl);
}

bool operator==(const TableHandle &lhs, const TableHandle &rhs)
{
    return std::make_tuple(lhs.schemaId(), lhs.containerId(), lhs.debugName()) ==
           std::make_tuple(rhs.schemaId(), rhs.containerId(), rhs.debugName());
}

ListHandle::ListHandle() noexcept : _impl(new Impl(this)) {}

ListHandle::ListHandle(SchemaId schemaId, ContainerId containerId, std::string debugName) : _impl(new Impl(this))
{
    __stdc_impl_t;
    impl.schemaId = schemaId;
    impl.containerId = containerId;
    impl.debugName = std::move(debugName);
}

ListHandle::~ListHandle() = default;
ListHandle::ListHandle(const ListHandle &other) = default;
ListHandle::ListHandle(ListHandle &&other) noexcept = default;
ListHandle &ListHandle::operator=(const ListHandle &other) = default;
ListHandle &ListHandle::operator=(ListHandle &&other) noexcept = default;

bool ListHandle::isValid() const noexcept
{
    __stdc_impl_t;
    return validContainerHandle(impl);
}

SchemaId ListHandle::schemaId() const noexcept
{
    __stdc_impl_t;
    return impl.schemaId;
}

ContainerId ListHandle::containerId() const noexcept
{
    __stdc_impl_t;
    return impl.containerId;
}

const std::string &ListHandle::debugName() const noexcept
{
    __stdc_impl_t;
    return debugNameOf(impl);
}

bool operator==(const ListHandle &lhs, const ListHandle &rhs)
{
    return std::make_tuple(lhs.schemaId(), lhs.containerId(), lhs.debugName()) ==
           std::make_tuple(rhs.schemaId(), rhs.containerId(), rhs.debugName());
}

ColumnHandle::ColumnHandle() noexcept : _impl(new Impl(this)) {}

ColumnHandle::ColumnHandle(SchemaId schemaId, ContainerId containerId, ColumnId columnId, std::string debugName)
    : _impl(new Impl(this))
{
    __stdc_impl_t;
    impl.schemaId = schemaId;
    impl.containerId = containerId;
    impl.columnId = columnId;
    impl.debugName = std::move(debugName);
}

ColumnHandle::~ColumnHandle() = default;
ColumnHandle::ColumnHandle(const ColumnHandle &other) = default;
ColumnHandle::ColumnHandle(ColumnHandle &&other) noexcept = default;
ColumnHandle &ColumnHandle::operator=(const ColumnHandle &other) = default;
ColumnHandle &ColumnHandle::operator=(ColumnHandle &&other) noexcept = default;

bool ColumnHandle::isValid() const noexcept
{
    __stdc_impl_t;
    return validContainerHandle(impl) && impl.columnId != 0;
}

SchemaId ColumnHandle::schemaId() const noexcept
{
    __stdc_impl_t;
    return impl.schemaId;
}

ContainerId ColumnHandle::containerId() const noexcept
{
    __stdc_impl_t;
    return impl.containerId;
}

ColumnId ColumnHandle::columnId() const noexcept
{
    __stdc_impl_t;
    return impl.columnId;
}

const std::string &ColumnHandle::debugName() const noexcept
{
    __stdc_impl_t;
    return debugNameOf(impl);
}

bool operator==(const ColumnHandle &lhs, const ColumnHandle &rhs)
{
    return std::make_tuple(lhs.schemaId(), lhs.containerId(), lhs.columnId(), lhs.debugName()) ==
           std::make_tuple(rhs.schemaId(), rhs.containerId(), rhs.columnId(), rhs.debugName());
}

RelationHandle::RelationHandle() noexcept : _impl(new Impl(this)) {}

RelationHandle::RelationHandle(SchemaId schemaId,
                               ContainerId containerId,
                               RelationId relationId,
                               ColumnHandle column,
                               std::string debugName)
    : _impl(new Impl(this))
{
    __stdc_impl_t;
    impl.schemaId = schemaId;
    impl.containerId = containerId;
    impl.relationId = relationId;
    impl.column = std::move(column);
    impl.debugName = std::move(debugName);
}

RelationHandle::~RelationHandle() = default;
RelationHandle::RelationHandle(const RelationHandle &other) = default;
RelationHandle::RelationHandle(RelationHandle &&other) noexcept = default;
RelationHandle &RelationHandle::operator=(const RelationHandle &other) = default;
RelationHandle &RelationHandle::operator=(RelationHandle &&other) noexcept = default;

bool RelationHandle::isValid() const noexcept
{
    __stdc_impl_t;
    return validContainerHandle(impl) && impl.relationId != 0 && impl.column.isValid();
}

SchemaId RelationHandle::schemaId() const noexcept
{
    __stdc_impl_t;
    return impl.schemaId;
}

ContainerId RelationHandle::containerId() const noexcept
{
    __stdc_impl_t;
    return impl.containerId;
}

RelationId RelationHandle::relationId() const noexcept
{
    __stdc_impl_t;
    return impl.relationId;
}

ColumnHandle RelationHandle::column() const noexcept
{
    __stdc_impl_t;
    return impl.column;
}

const std::string &RelationHandle::debugName() const noexcept
{
    __stdc_impl_t;
    return debugNameOf(impl);
}

bool operator==(const RelationHandle &lhs, const RelationHandle &rhs)
{
    return std::make_tuple(lhs.schemaId(), lhs.containerId(), lhs.relationId(), lhs.column(), lhs.debugName()) ==
           std::make_tuple(rhs.schemaId(), rhs.containerId(), rhs.relationId(), rhs.column(), rhs.debugName());
}

VariantHandle::VariantHandle() noexcept : _impl(new Impl(this)) {}

VariantHandle::VariantHandle(SchemaId schemaId, ContainerId containerId, VariantId variantId, std::string debugName)
    : _impl(new Impl(this))
{
    __stdc_impl_t;
    impl.schemaId = schemaId;
    impl.containerId = containerId;
    impl.variantId = variantId;
    impl.debugName = std::move(debugName);
}

VariantHandle::~VariantHandle() = default;
VariantHandle::VariantHandle(const VariantHandle &other) = default;
VariantHandle::VariantHandle(VariantHandle &&other) noexcept = default;
VariantHandle &VariantHandle::operator=(const VariantHandle &other) = default;
VariantHandle &VariantHandle::operator=(VariantHandle &&other) noexcept = default;

bool VariantHandle::isValid() const noexcept
{
    __stdc_impl_t;
    return validContainerHandle(impl) && impl.variantId != 0;
}

SchemaId VariantHandle::schemaId() const noexcept
{
    __stdc_impl_t;
    return impl.schemaId;
}

ContainerId VariantHandle::containerId() const noexcept
{
    __stdc_impl_t;
    return impl.containerId;
}

VariantId VariantHandle::variantId() const noexcept
{
    __stdc_impl_t;
    return impl.variantId;
}

const std::string &VariantHandle::debugName() const noexcept
{
    __stdc_impl_t;
    return debugNameOf(impl);
}

bool operator==(const VariantHandle &lhs, const VariantHandle &rhs)
{
    return std::make_tuple(lhs.schemaId(), lhs.containerId(), lhs.variantId(), lhs.debugName()) ==
           std::make_tuple(rhs.schemaId(), rhs.containerId(), rhs.variantId(), rhs.debugName());
}

OrderedIndexHandle::OrderedIndexHandle() noexcept : _impl(new Impl(this)) {}

OrderedIndexHandle::OrderedIndexHandle(SchemaId schemaId,
                                       ContainerId containerId,
                                       std::uint32_t indexId,
                                       std::string debugName)
    : _impl(new Impl(this))
{
    __stdc_impl_t;
    impl.schemaId = schemaId;
    impl.containerId = containerId;
    impl.indexId = indexId;
    impl.debugName = std::move(debugName);
}

OrderedIndexHandle::~OrderedIndexHandle() = default;
OrderedIndexHandle::OrderedIndexHandle(const OrderedIndexHandle &other) = default;
OrderedIndexHandle::OrderedIndexHandle(OrderedIndexHandle &&other) noexcept = default;
OrderedIndexHandle &OrderedIndexHandle::operator=(const OrderedIndexHandle &other) = default;
OrderedIndexHandle &OrderedIndexHandle::operator=(OrderedIndexHandle &&other) noexcept = default;

bool OrderedIndexHandle::isValid() const noexcept
{
    __stdc_impl_t;
    return validContainerHandle(impl) && impl.indexId != 0;
}

SchemaId OrderedIndexHandle::schemaId() const noexcept
{
    __stdc_impl_t;
    return impl.schemaId;
}

ContainerId OrderedIndexHandle::containerId() const noexcept
{
    __stdc_impl_t;
    return impl.containerId;
}

std::uint32_t OrderedIndexHandle::indexId() const noexcept
{
    __stdc_impl_t;
    return impl.indexId;
}

const std::string &OrderedIndexHandle::debugName() const noexcept
{
    __stdc_impl_t;
    return debugNameOf(impl);
}

bool operator==(const OrderedIndexHandle &lhs, const OrderedIndexHandle &rhs)
{
    return std::make_tuple(lhs.schemaId(), lhs.containerId(), lhs.indexId(), lhs.debugName()) ==
           std::make_tuple(rhs.schemaId(), rhs.containerId(), rhs.indexId(), rhs.debugName());
}

IntervalIndexHandle::IntervalIndexHandle() noexcept : _impl(new Impl(this)) {}

IntervalIndexHandle::IntervalIndexHandle(SchemaId schemaId,
                                         ContainerId containerId,
                                         std::uint32_t indexId,
                                         std::string debugName)
    : _impl(new Impl(this))
{
    __stdc_impl_t;
    impl.schemaId = schemaId;
    impl.containerId = containerId;
    impl.indexId = indexId;
    impl.debugName = std::move(debugName);
}

IntervalIndexHandle::~IntervalIndexHandle() = default;
IntervalIndexHandle::IntervalIndexHandle(const IntervalIndexHandle &other) = default;
IntervalIndexHandle::IntervalIndexHandle(IntervalIndexHandle &&other) noexcept = default;
IntervalIndexHandle &IntervalIndexHandle::operator=(const IntervalIndexHandle &other) = default;
IntervalIndexHandle &IntervalIndexHandle::operator=(IntervalIndexHandle &&other) noexcept = default;

bool IntervalIndexHandle::isValid() const noexcept
{
    __stdc_impl_t;
    return validContainerHandle(impl) && impl.indexId != 0;
}

SchemaId IntervalIndexHandle::schemaId() const noexcept
{
    __stdc_impl_t;
    return impl.schemaId;
}

ContainerId IntervalIndexHandle::containerId() const noexcept
{
    __stdc_impl_t;
    return impl.containerId;
}

std::uint32_t IntervalIndexHandle::indexId() const noexcept
{
    __stdc_impl_t;
    return impl.indexId;
}

const std::string &IntervalIndexHandle::debugName() const noexcept
{
    __stdc_impl_t;
    return debugNameOf(impl);
}

bool operator==(const IntervalIndexHandle &lhs, const IntervalIndexHandle &rhs)
{
    return std::make_tuple(lhs.schemaId(), lhs.containerId(), lhs.indexId(), lhs.debugName()) ==
           std::make_tuple(rhs.schemaId(), rhs.containerId(), rhs.indexId(), rhs.debugName());
}

} // namespace dini
