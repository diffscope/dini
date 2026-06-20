#include "schema_p.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

#include <stdcorelib/pimpl.h>

#include <dini/errors.h>

namespace dini {

    namespace {

    std::atomic<SchemaId> nextSchemaId {1};
    void requireMutable(const SchemaBuilder::Impl *builder)
    {
        if (!builder) {
            throw SchemaError("schema builder is invalid");
        }
        if (builder->data.frozen) {
            throw SchemaError("schema builder is frozen");
        }
    }

    void validateAssociationTarget(const SchemaDefinitionData &data, const AssociationTarget &target)
    {
        std::visit(
            [&](const auto &handle) {
                if (!handle.isValid() || handle.schemaId() != data.schemaId ||
                    !findContainer(data, handle.containerId())) {
                    throw SchemaError("association target must belong to the same schema builder");
                }
            },
            target);
    }

    ContainerDefinitionRecord &requireContainer(SchemaBuilder::Impl *builder, ContainerId id)
    {
        requireMutable(builder);
        auto *container = findContainer(builder->data, id);
        if (!container) {
            throw SchemaError("container builder is invalid");
        }
        return *container;
    }

    void requireColumnDefinition(ValueType type)
    {
        if (type == ValueType::Null) {
            throw SchemaError("column type must not be null");
        }
    }

    ColumnHandle addColumnRecord(SchemaBuilder::Impl *builder,
                                 ContainerDefinitionRecord &container,
                                 ColumnDefinitionRecord record)
    {
        record.info.id = builder->data.nextColumnId++;
        record.info.containerId = container.info.id;
        auto handle = ColumnHandle(builder->data.schemaId, container.info.id, record.info.id, record.info.debugName);
        container.columns.push_back(std::move(record));
        return handle;
    }

    void validateFrozenSchema(const SchemaDefinitionData &data)
    {
        if (data.containers.empty()) {
            throw SchemaError("schema must contain at least one container");
        }
        for (const auto &container : data.containers) {
            if (container.info.kind == ContainerKind::List && !container.listAssociation) {
                throw SchemaError("list container must define an association");
            }
            for (std::size_t i = 0; i < container.columns.size(); ++i) {
                const auto &column = container.columns[i];
                if (column.info.computed) {
                    if (!column.computedDefinition.compute) {
                        throw SchemaError("computed column must define a compute callback");
                    }
                    for (const auto &dependency : column.computedDefinition.dependsOn) {
                        if (!dependency.isValid() || dependency.schemaId() != data.schemaId ||
                            dependency.containerId() != container.info.id) {
                            throw SchemaError("computed column dependency must belong to the same container");
                        }
                        const auto depIt = std::find_if(container.columns.begin(),
                                                        container.columns.begin() + static_cast<std::ptrdiff_t>(i),
                                                        [&](const auto &candidate) {
                                                            return candidate.info.id == dependency.columnId();
                                                        });
                        if (depIt == container.columns.begin() + static_cast<std::ptrdiff_t>(i)) {
                            throw SchemaError("computed column dependency must be declared before the computed column");
                        }
                    }
                }
                if (column.info.variantSpecific) {
                    const auto found = std::any_of(container.variants.begin(),
                                                   container.variants.end(),
                                                   [&](const auto &variant) {
                                                       return column.variantId && *column.variantId == variant.id;
                                                   });
                    if (!found) {
                        throw SchemaError("variant-specific column references an unknown variant");
                    }
                }
            }
            for (const auto &relation : container.relations) {
                validateAssociationTarget(data, relation.info.target);
                if (!findColumn(data, container.info.id, relation.info.column.columnId())) {
                    throw SchemaError("relation storage column does not belong to its container");
                }
            }
        }
    }

    } // namespace

ContainerDefinitionRecord *findContainer(SchemaDefinitionData &data, ContainerId id)
{
    auto it = std::find_if(data.containers.begin(), data.containers.end(), [id](const auto &container) {
        return container.info.id == id;
    });
    return it == data.containers.end() ? nullptr : &*it;
}

const ContainerDefinitionRecord *findContainer(const SchemaDefinitionData &data, ContainerId id)
{
    auto it = std::find_if(data.containers.begin(), data.containers.end(), [id](const auto &container) {
        return container.info.id == id;
    });
    return it == data.containers.end() ? nullptr : &*it;
}

ColumnDefinitionRecord *findColumn(SchemaDefinitionData &data, ContainerId containerId, ColumnId columnId)
{
    auto *container = findContainer(data, containerId);
    if (!container) {
        return nullptr;
    }
    auto it = std::find_if(container->columns.begin(), container->columns.end(), [columnId](const auto &column) {
        return column.info.id == columnId;
    });
    return it == container->columns.end() ? nullptr : &*it;
}

const ColumnDefinitionRecord *findColumn(const SchemaDefinitionData &data, ContainerId containerId, ColumnId columnId)
{
    const auto *container = findContainer(data, containerId);
    if (!container) {
        return nullptr;
    }
    auto it = std::find_if(container->columns.begin(), container->columns.end(), [columnId](const auto &column) {
        return column.info.id == columnId;
    });
    return it == container->columns.end() ? nullptr : &*it;
}

RelationDefinitionRecord *findRelation(SchemaDefinitionData &data, ContainerId containerId, RelationId relationId)
{
    auto *container = findContainer(data, containerId);
    if (!container) {
        return nullptr;
    }
    auto it = std::find_if(container->relations.begin(), container->relations.end(), [relationId](const auto &relation) {
        return relation.info.id == relationId;
    });
    return it == container->relations.end() ? nullptr : &*it;
}

const RelationDefinitionRecord *findRelation(const SchemaDefinitionData &data, ContainerId containerId, RelationId relationId)
{
    const auto *container = findContainer(data, containerId);
    if (!container) {
        return nullptr;
    }
    auto it = std::find_if(container->relations.begin(), container->relations.end(), [relationId](const auto &relation) {
        return relation.info.id == relationId;
    });
    return it == container->relations.end() ? nullptr : &*it;
}

RelationDefinitionRecord *findRelationByColumn(SchemaDefinitionData &data, ContainerId containerId, ColumnId columnId)
{
    auto *container = findContainer(data, containerId);
    if (!container) {
        return nullptr;
    }
    auto it = std::find_if(container->relations.begin(), container->relations.end(), [columnId](const auto &relation) {
        return relation.info.column.columnId() == columnId;
    });
    return it == container->relations.end() ? nullptr : &*it;
}

const RelationDefinitionRecord *findRelationByColumn(const SchemaDefinitionData &data,
                                                     ContainerId containerId,
                                                     ColumnId columnId)
{
    const auto *container = findContainer(data, containerId);
    if (!container) {
        return nullptr;
    }
    auto it = std::find_if(container->relations.begin(), container->relations.end(), [columnId](const auto &relation) {
        return relation.info.column.columnId() == columnId;
    });
    return it == container->relations.end() ? nullptr : &*it;
}

const SchemaDefinitionData &schemaData(const EngineSchema &schema)
{
    return *schema._impl.constData();
}

EngineSchema::EngineSchema() : _impl(new Impl(this)) {}
EngineSchema::EngineSchema(SharedDataPointer<Impl> data) : _impl(std::move(data)) {}
EngineSchema::~EngineSchema() = default;
EngineSchema::EngineSchema(const EngineSchema &other) = default;
EngineSchema::EngineSchema(EngineSchema &&other) noexcept = default;
EngineSchema &EngineSchema::operator=(const EngineSchema &other) = default;
EngineSchema &EngineSchema::operator=(EngineSchema &&other) noexcept = default;

bool EngineSchema::isValid() const noexcept
{
    __stdc_impl_t;
    return impl.frozen && impl.schemaId != 0;
}

SchemaId EngineSchema::schemaId() const
{
    __stdc_impl_t;
    if (!isValid()) {
        throw SchemaError("schema is invalid");
    }
    return impl.schemaId;
}

ContainerInfo EngineSchema::tableInfo(TableHandle table) const
{
    validate(table);
    __stdc_impl_t;
    return findContainer(impl, table.containerId())->info;
}

ContainerInfo EngineSchema::listInfo(ListHandle list) const
{
    validate(list);
    __stdc_impl_t;
    return findContainer(impl, list.containerId())->info;
}

ColumnInfo EngineSchema::columnInfo(ColumnHandle column) const
{
    validate(column);
    __stdc_impl_t;
    return findColumn(impl, column.containerId(), column.columnId())->info;
}

RelationInfo EngineSchema::relationInfo(RelationHandle relation) const
{
    validate(relation);
    __stdc_impl_t;
    return findRelation(impl, relation.containerId(), relation.relationId())->info;
}

void EngineSchema::validate(TableHandle table) const
{
    __stdc_impl_t;
    const auto *container = findContainer(impl, table.containerId());
    if (!isValid() || !table.isValid() || table.schemaId() != impl.schemaId || !container ||
        container->info.kind != ContainerKind::Table) {
        throw HandleError("invalid table handle");
    }
}

void EngineSchema::validate(ListHandle list) const
{
    __stdc_impl_t;
    const auto *container = findContainer(impl, list.containerId());
    if (!isValid() || !list.isValid() || list.schemaId() != impl.schemaId || !container ||
        container->info.kind != ContainerKind::List) {
        throw HandleError("invalid list handle");
    }
}

void EngineSchema::validate(ColumnHandle column) const
{
    __stdc_impl_t;
    if (!isValid() || !column.isValid() || column.schemaId() != impl.schemaId ||
        !findColumn(impl, column.containerId(), column.columnId())) {
        throw HandleError("invalid column handle");
    }
}

void EngineSchema::validate(RelationHandle relation) const
{
    __stdc_impl_t;
    if (!isValid() || !relation.isValid() || relation.schemaId() != impl.schemaId ||
        !findRelation(impl, relation.containerId(), relation.relationId())) {
        throw HandleError("invalid relation handle");
    }
}

SchemaBuilder::SchemaBuilder() : _impl(std::make_unique<Impl>())
{
    _impl->data.schemaId = nextSchemaId.fetch_add(1, std::memory_order_relaxed);
}

SchemaBuilder::~SchemaBuilder() = default;
SchemaBuilder::SchemaBuilder(SchemaBuilder &&other) noexcept = default;
SchemaBuilder &SchemaBuilder::operator=(SchemaBuilder &&other) noexcept = default;

TableBuilder SchemaBuilder::createTable(std::string debugName)
{
    requireMutable(_impl.get());
    ContainerDefinitionRecord record;
    record.info.kind = ContainerKind::Table;
    record.info.id = _impl->data.nextContainerId++;
    record.info.debugName = std::move(debugName);
    _impl->data.containers.push_back(std::move(record));

    auto builder = TableBuilder();
    builder._impl = std::make_unique<TableBuilder::Impl>();
    builder._impl->builder = _impl.get();
    builder._impl->containerId = _impl->data.containers.back().info.id;
    return builder;
}

ListBuilder SchemaBuilder::createList(std::string debugName)
{
    requireMutable(_impl.get());
    ContainerDefinitionRecord record;
    record.info.kind = ContainerKind::List;
    record.info.id = _impl->data.nextContainerId++;
    record.info.debugName = std::move(debugName);
    _impl->data.containers.push_back(std::move(record));

    auto builder = ListBuilder();
    builder._impl = std::make_unique<ListBuilder::Impl>();
    builder._impl->builder = _impl.get();
    builder._impl->containerId = _impl->data.containers.back().info.id;
    return builder;
}

EngineSchema SchemaBuilder::freeze()
{
    requireMutable(_impl.get());
    validateFrozenSchema(_impl->data);
    _impl->data.frozen = true;
    return EngineSchema(SharedDataPointer<EngineSchema::Impl>(new EngineSchema::Impl(nullptr, _impl->data)));
}

bool SchemaBuilder::isFrozen() const noexcept
{
    return !_impl || _impl->data.frozen;
}

TableBuilder::TableBuilder() = default;
TableBuilder::~TableBuilder() = default;
TableBuilder::TableBuilder(TableBuilder &&other) noexcept = default;
TableBuilder &TableBuilder::operator=(TableBuilder &&other) noexcept = default;

bool TableBuilder::isValid() const noexcept
{
    return _impl && _impl->builder && findContainer(_impl->builder->data, _impl->containerId);
}

TableHandle TableBuilder::handle() const
{
    if (!isValid()) {
        throw SchemaError("table builder is invalid");
    }
    const auto *container = findContainer(_impl->builder->data, _impl->containerId);
    return TableHandle(_impl->builder->data.schemaId, _impl->containerId, container->info.debugName);
}

void TableBuilder::setVolatile(bool enabled)
{
    requireContainer(_impl ? _impl->builder : nullptr, _impl ? _impl->containerId : 0).info.volatileData = enabled;
}

ColumnHandle TableBuilder::addColumn(const ColumnDefinition &definition)
{
    requireColumnDefinition(definition.type);
    auto &container = requireContainer(_impl ? _impl->builder : nullptr, _impl ? _impl->containerId : 0);
    ColumnDefinitionRecord record;
    record.info.debugName = definition.debugName;
    record.info.type = definition.type;
    record.info.index = definition.index;
    record.info.volatileData = definition.volatileData;
    record.normalDefinition = definition;
    return addColumnRecord(_impl->builder, container, std::move(record));
}

ColumnHandle TableBuilder::addComputedColumn(const ComputedColumnDefinition &definition)
{
    requireColumnDefinition(definition.type);
    auto &container = requireContainer(_impl ? _impl->builder : nullptr, _impl ? _impl->containerId : 0);
    ColumnDefinitionRecord record;
    record.info.debugName = definition.debugName;
    record.info.type = definition.type;
    record.info.index = definition.index;
    record.info.computed = true;
    record.info.volatileData = definition.volatileData;
    record.computedDefinition = definition;
    return addColumnRecord(_impl->builder, container, std::move(record));
}

RelationHandle TableBuilder::addAssociation(const AssociationDefinition &definition)
{
    auto &container = requireContainer(_impl ? _impl->builder : nullptr, _impl ? _impl->containerId : 0);
    validateAssociationTarget(_impl->builder->data, definition.target);
    ColumnDefinitionRecord columnRecord;
    columnRecord.info.debugName = definition.debugName;
    columnRecord.info.type = ValueType::UInt64;
    columnRecord.info.index = IndexKind::Normal;
    columnRecord.info.association = true;
    columnRecord.normalDefinition = ColumnDefinition {
        .debugName = definition.debugName,
        .type = ValueType::UInt64,
        .index = IndexKind::Normal,
        .nullable = true,
    };
    auto column = addColumnRecord(_impl->builder, container, std::move(columnRecord));

    RelationDefinitionRecord relation;
    relation.info.id = _impl->builder->data.nextRelationId++;
    relation.info.containerId = container.info.id;
    relation.info.column = column;
    relation.info.target = definition.target;
    container.relations.push_back(relation);
    return RelationHandle(_impl->builder->data.schemaId, container.info.id, relation.info.id, column, definition.debugName);
}

VariantHandle TableBuilder::addVariant(std::string debugName)
{
    auto &container = requireContainer(_impl ? _impl->builder : nullptr, _impl ? _impl->containerId : 0);
    VariantDefinitionRecord variant;
    variant.id = _impl->builder->data.nextVariantId++;
    variant.containerId = container.info.id;
    variant.debugName = std::move(debugName);
    container.variants.push_back(variant);
    return VariantHandle(_impl->builder->data.schemaId, container.info.id, variant.id, variant.debugName);
}

ColumnHandle TableBuilder::addVariantColumn(const VariantColumnDefinition &definition)
{
    requireColumnDefinition(definition.type);
    if (!definition.variant.isValid() || definition.variant.schemaId() != _impl->builder->data.schemaId ||
        definition.variant.containerId() != (_impl ? _impl->containerId : 0)) {
        throw SchemaError("variant column uses an invalid variant");
    }
    auto &container = requireContainer(_impl ? _impl->builder : nullptr, _impl ? _impl->containerId : 0);
    ColumnDefinitionRecord record;
    record.info.debugName = definition.debugName;
    record.info.type = definition.type;
    record.info.index = definition.index;
    record.info.variantSpecific = true;
    record.info.volatileData = definition.volatileData;
    record.variantDefinition = definition;
    record.variantId = definition.variant.variantId();
    return addColumnRecord(_impl->builder, container, std::move(record));
}

void TableBuilder::addHook(const HookDefinition &definition)
{
    if (!definition.callback) {
        throw SchemaError("hook callback is empty");
    }
    requireContainer(_impl ? _impl->builder : nullptr, _impl ? _impl->containerId : 0).hooks.push_back(definition);
}

ListBuilder::ListBuilder() = default;
ListBuilder::~ListBuilder() = default;
ListBuilder::ListBuilder(ListBuilder &&other) noexcept = default;
ListBuilder &ListBuilder::operator=(ListBuilder &&other) noexcept = default;

bool ListBuilder::isValid() const noexcept
{
    return _impl && _impl->builder && findContainer(_impl->builder->data, _impl->containerId);
}

ListHandle ListBuilder::handle() const
{
    if (!isValid()) {
        throw SchemaError("list builder is invalid");
    }
    const auto *container = findContainer(_impl->builder->data, _impl->containerId);
    return ListHandle(_impl->builder->data.schemaId, _impl->containerId, container->info.debugName);
}

void ListBuilder::setVolatile(bool enabled)
{
    requireContainer(_impl ? _impl->builder : nullptr, _impl ? _impl->containerId : 0).info.volatileData = enabled;
}

RelationHandle ListBuilder::setAssociation(const AssociationDefinition &definition)
{
    auto &container = requireContainer(_impl ? _impl->builder : nullptr, _impl ? _impl->containerId : 0);
    if (container.listAssociation) {
        throw SchemaError("list association has already been set");
    }
    TableBuilder tableBuilder;
    tableBuilder._impl = std::make_unique<TableBuilder::Impl>();
    tableBuilder._impl->builder = _impl->builder;
    tableBuilder._impl->containerId = _impl->containerId;
    auto relation = tableBuilder.addAssociation(definition);
    container.listAssociation = relation.relationId();
    return relation;
}

ColumnHandle ListBuilder::addColumn(const ColumnDefinition &definition)
{
    TableBuilder builder;
    builder._impl = std::make_unique<TableBuilder::Impl>();
    builder._impl->builder = _impl ? _impl->builder : nullptr;
    builder._impl->containerId = _impl ? _impl->containerId : 0;
    return builder.addColumn(definition);
}

ColumnHandle ListBuilder::addComputedColumn(const ComputedColumnDefinition &definition)
{
    TableBuilder builder;
    builder._impl = std::make_unique<TableBuilder::Impl>();
    builder._impl->builder = _impl ? _impl->builder : nullptr;
    builder._impl->containerId = _impl ? _impl->containerId : 0;
    return builder.addComputedColumn(definition);
}

VariantHandle ListBuilder::addVariant(std::string debugName)
{
    TableBuilder builder;
    builder._impl = std::make_unique<TableBuilder::Impl>();
    builder._impl->builder = _impl ? _impl->builder : nullptr;
    builder._impl->containerId = _impl ? _impl->containerId : 0;
    return builder.addVariant(std::move(debugName));
}

ColumnHandle ListBuilder::addVariantColumn(const VariantColumnDefinition &definition)
{
    TableBuilder builder;
    builder._impl = std::make_unique<TableBuilder::Impl>();
    builder._impl->builder = _impl ? _impl->builder : nullptr;
    builder._impl->containerId = _impl ? _impl->containerId : 0;
    return builder.addVariantColumn(definition);
}

void ListBuilder::addHook(const HookDefinition &definition)
{
    TableBuilder builder;
    builder._impl = std::make_unique<TableBuilder::Impl>();
    builder._impl->builder = _impl ? _impl->builder : nullptr;
    builder._impl->containerId = _impl ? _impl->containerId : 0;
    builder.addHook(definition);
}

} // namespace dini
