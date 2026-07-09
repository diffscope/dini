#ifndef DINI_SCHEMA_P_H
#define DINI_SCHEMA_P_H

#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <dini/schema.h>

namespace dini {

struct ColumnDefinitionRecord {
    ColumnInfo info;
    ColumnDefinition normalDefinition;
    ComputedColumnDefinition computedDefinition;
    VariantColumnDefinition variantDefinition;
    std::optional<VariantId> variantId;
};

struct RelationDefinitionRecord {
    RelationInfo info;
};

struct VariantDefinitionRecord {
    VariantId id = 0;
    ContainerId containerId = 0;
    std::string debugName;
};

struct RangeIndexDefinitionRecord {
    std::string debugName;
    std::vector<ColumnHandle> columns;
};

struct OrderedIndexDefinitionRecord {
    OrderedIndexHandle handle;
    std::string debugName;
    std::vector<ColumnHandle> groupBy;
    std::vector<ColumnHandle> orderBy;
    bool tieBreakById = true;
};

struct IntervalIndexDefinitionRecord {
    IntervalIndexHandle handle;
    std::string debugName;
    std::vector<ColumnHandle> groupBy;
    ColumnHandle start;
    ColumnHandle end;
};

struct ContainerDefinitionRecord {
    ContainerInfo info;
    std::vector<ColumnDefinitionRecord> columns;
    std::vector<RelationDefinitionRecord> relations;
    std::vector<VariantDefinitionRecord> variants;
    std::vector<RangeIndexDefinitionRecord> rangeIndexes;
    std::vector<OrderedIndexDefinitionRecord> orderedIndexes;
    std::vector<IntervalIndexDefinitionRecord> intervalIndexes;
    std::vector<HookDefinition> hooks;
    std::optional<RelationId> listAssociation;
};

struct SchemaDefinitionData : SharedData {
    SchemaId schemaId = 0;
    bool frozen = false;
    ContainerId nextContainerId = 1;
    ColumnId nextColumnId = 1;
    RelationId nextRelationId = 1;
    VariantId nextVariantId = 1;
    std::uint32_t nextOrderedIndexId = 1;
    std::uint32_t nextIntervalIndexId = 1;
    std::vector<ContainerDefinitionRecord> containers;
};

struct EngineSchema::Impl : SchemaDefinitionData {
    using Decl = EngineSchema;
    Decl *_decl = nullptr;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
    Impl(Decl *decl, const SchemaDefinitionData &data) : SchemaDefinitionData(data), _decl(decl) {}
};

struct SchemaBuilder::Impl {
    SchemaDefinitionData data;
};

struct TableBuilder::Impl {
    SchemaBuilder::Impl *builder = nullptr;
    ContainerId containerId = 0;
};

struct ListBuilder::Impl {
    SchemaBuilder::Impl *builder = nullptr;
    ContainerId containerId = 0;
};

ContainerDefinitionRecord *findContainer(SchemaDefinitionData &data, ContainerId id);
const ContainerDefinitionRecord *findContainer(const SchemaDefinitionData &data, ContainerId id);
ColumnDefinitionRecord *findColumn(SchemaDefinitionData &data, ContainerId containerId, ColumnId columnId);
const ColumnDefinitionRecord *findColumn(const SchemaDefinitionData &data, ContainerId containerId, ColumnId columnId);
RelationDefinitionRecord *findRelation(SchemaDefinitionData &data, ContainerId containerId, RelationId relationId);
const RelationDefinitionRecord *findRelation(const SchemaDefinitionData &data, ContainerId containerId, RelationId relationId);
RelationDefinitionRecord *findRelationByColumn(SchemaDefinitionData &data, ContainerId containerId, ColumnId columnId);
const RelationDefinitionRecord *findRelationByColumn(const SchemaDefinitionData &data, ContainerId containerId, ColumnId columnId);
OrderedIndexDefinitionRecord *findOrderedIndex(SchemaDefinitionData &data, ContainerId containerId, std::uint32_t indexId);
const OrderedIndexDefinitionRecord *findOrderedIndex(const SchemaDefinitionData &data, ContainerId containerId, std::uint32_t indexId);
IntervalIndexDefinitionRecord *findIntervalIndex(SchemaDefinitionData &data, ContainerId containerId, std::uint32_t indexId);
const IntervalIndexDefinitionRecord *findIntervalIndex(const SchemaDefinitionData &data, ContainerId containerId, std::uint32_t indexId);
const SchemaDefinitionData &schemaData(const EngineSchema &schema);

} // namespace dini

#endif // DINI_SCHEMA_P_H
