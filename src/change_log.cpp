#include "change_p.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include <dini/errors.h>

#include "schema_p.h"

namespace dini {

    namespace {

    constexpr std::uint8_t changeSetMagic[] = {0x7f, 'D', 'I', 'N', 'I', 'C', 'S', '2'};
    constexpr std::uint32_t changeSetFormatVersion = 2;

    void writeMagic(BinaryWriter &writer)
    {
        for (auto byte : changeSetMagic) {
            writer.writeByte(byte);
        }
    }

    void readMagic(BinaryReader &reader)
    {
        for (auto expected : changeSetMagic) {
            if (reader.readByte() != expected) {
                throw LogError("invalid change set magic");
            }
        }
    }

    void writeRotation(BinaryWriter &writer, const ListRotation &rotation)
    {
        writer.writeSize(rotation.startIndex);
        writer.writeSize(rotation.count);
        writer.writeInt64(static_cast<std::int64_t>(rotation.offset));
    }

    ListRotation readRotation(BinaryReader &reader)
    {
        return ListRotation {
            .startIndex = reader.readSize(),
            .count = reader.readSize(),
            .offset = static_cast<std::ptrdiff_t>(reader.readInt64()),
        };
    }

    void writeAssociationOptions(BinaryWriter &writer, const AssociationUpdateOptions &options)
    {
        writer.writeBool(options.targetIndex.has_value());
        if (options.targetIndex) {
            writer.writeSize(*options.targetIndex);
        }
    }

    AssociationUpdateOptions readAssociationOptions(BinaryReader &reader)
    {
        AssociationUpdateOptions options;
        if (reader.readBool()) {
            options.targetIndex = reader.readSize();
        }
        return options;
    }

    void writeItemSnapshot(BinaryWriter &writer, const ItemSnapshot &snapshot)
    {
        writer.writeUInt64(snapshot.id);
        writer.writeVarUInt32(snapshot.containerId);
        writer.writeVarUInt32(snapshot.variant ? snapshot.variant->variantId() : 0);
        writer.writeSize(snapshot.values.size());
        for (const auto &columnValue : snapshot.values) {
            writer.writeVarUInt32(columnValue.column.columnId());
            writeValue(writer, columnValue.value);
        }
        writer.writeBool(snapshot.listIndex.has_value());
        if (snapshot.listIndex) {
            writer.writeSize(*snapshot.listIndex);
        }
    }

    ItemSnapshot readItemSnapshot(BinaryReader &reader, SchemaHandleCache &handles)
    {
        ItemSnapshot snapshot;
        snapshot.id = reader.readUInt64();
        snapshot.containerId = reader.readVarUInt32();
        const auto &container = handles.container(snapshot.containerId);
        snapshot.containerKind = container.info.kind;
        const auto variantId = reader.readVarUInt32();
        if (variantId != 0) {
            snapshot.variant = handles.variant(variantId);
        }
        const auto valueCount = reader.readSize();
        if (valueCount > reader.remainingSize() / 2) {
            throw LogError("item column values are truncated");
        }
        snapshot.values.reserve(valueCount);
        for (std::size_t i = 0; i < valueCount; ++i) {
            snapshot.values.push_back(ColumnValue {
                .column = handles.column(reader.readVarUInt32()),
                .value = readValue(reader),
            });
        }
        if (reader.readBool()) {
            snapshot.listIndex = reader.readSize();
        }
        restoreItemRelationMetadata(container, snapshot);
        return snapshot;
    }

    template <typename Change>
    ChangeOperation readListOperation(BinaryReader &reader, SchemaHandleCache &handles)
    {
        auto item = readItemSnapshot(reader, handles);
        if (!item.listAssociationValue || !item.listIndex) {
            throw LogError("list change is missing its association or index");
        }
        return ChangeOperation(Change {
            .list = handles.list(item.containerId),
            .associationValue = *item.listAssociationValue,
            .index = *item.listIndex,
            .item = std::move(item),
        });
    }

    void writeOperation(BinaryWriter &writer, const ChangeOperation &operation)
    {
        writer.writeByte(static_cast<std::uint8_t>(operation.kind()));
        std::visit(
            [&](const auto &change) {
                using T = std::decay_t<decltype(change)>;
                if constexpr (std::is_same_v<T, ItemInsertedChange>) {
                    writeItemSnapshot(writer, change.item);
                } else if constexpr (std::is_same_v<T, ItemRemovedChange>) {
                    writeItemSnapshot(writer, change.item);
                    writer.writeBool(change.cascade);
                } else if constexpr (std::is_same_v<T, ColumnUpdatedChange>) {
                    writer.writeUInt64(change.itemId);
                    writer.writeVarUInt32(change.column.columnId());
                    writeValue(writer, change.oldValue);
                    writeValue(writer, change.newValue);
                    writeAssociationOptions(writer, change.associationOptions);
                    writer.writeBool(change.oldListIndex.has_value());
                    if (change.oldListIndex) {
                        writer.writeSize(*change.oldListIndex);
                    }
                } else if constexpr (std::is_same_v<T, ComputedColumnUpdatedChange>) {
                    writer.writeUInt64(change.itemId);
                    writer.writeVarUInt32(change.column.columnId());
                    writeValue(writer, change.oldValue);
                    writeValue(writer, change.newValue);
                } else if constexpr (std::is_same_v<T, CascadeRemovedChange>) {
                    writeItemSnapshot(writer, change.item);
                    writer.writeUInt64(change.ancestorId);
                } else if constexpr (std::is_same_v<T, ListInsertedChange> || std::is_same_v<T, ListRemovedChange>) {
                    writeItemSnapshot(writer, change.item);
                } else if constexpr (std::is_same_v<T, ListRotatedChange>) {
                    writer.writeVarUInt32(change.list.containerId());
                    writeValue(writer, change.associationValue);
                    writeRotation(writer, change.rotation);
                }
            },
            operation.payload());
    }

    ChangeOperation readOperation(BinaryReader &reader, SchemaHandleCache &handles)
    {
        const auto kind = static_cast<ChangeOperationKind>(reader.readByte());
        switch (kind) {
            case ChangeOperationKind::ItemInserted:
                return ChangeOperation(ItemInsertedChange {
                    .item = readItemSnapshot(reader, handles),
                });
            case ChangeOperationKind::ItemRemoved:
                return ChangeOperation(ItemRemovedChange {
                    .item = readItemSnapshot(reader, handles),
                    .cascade = reader.readBool(),
                });
            case ChangeOperationKind::ColumnUpdated:
            {
                ColumnUpdatedChange change {
                    .itemId = reader.readUInt64(),
                    .column = handles.column(reader.readVarUInt32()),
                    .oldValue = readValue(reader),
                    .newValue = readValue(reader),
                    .associationOptions = readAssociationOptions(reader),
                };
                if (reader.readBool()) {
                    change.oldListIndex = reader.readSize();
                }
                return ChangeOperation(change);
            }
            case ChangeOperationKind::ComputedColumnUpdated:
                return ChangeOperation(ComputedColumnUpdatedChange {
                    .itemId = reader.readUInt64(),
                    .column = handles.column(reader.readVarUInt32()),
                    .oldValue = readValue(reader),
                    .newValue = readValue(reader),
                });
            case ChangeOperationKind::CascadeRemoved:
                return ChangeOperation(CascadeRemovedChange {
                    .item = readItemSnapshot(reader, handles),
                    .ancestorId = reader.readUInt64(),
                });
            case ChangeOperationKind::ListInserted:
                return readListOperation<ListInsertedChange>(reader, handles);
            case ChangeOperationKind::ListRemoved:
                return readListOperation<ListRemovedChange>(reader, handles);
            case ChangeOperationKind::ListRotated:
                return ChangeOperation(ListRotatedChange {
                    .list = handles.list(reader.readVarUInt32()),
                    .associationValue = readValue(reader),
                    .rotation = readRotation(reader),
                });
        }
        throw LogError("unknown change operation kind");
    }

    } // namespace

void BinaryWriter::writeByte(std::uint8_t value)
{
    bytes.push_back(value);
}

void BinaryWriter::writeBool(bool value)
{
    writeByte(value ? 1 : 0);
}

void BinaryWriter::writeUInt32(std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        writeByte(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffU));
    }
}

void BinaryWriter::writeVarUInt32(std::uint32_t value)
{
    while (value >= 0x80) {
        writeByte(static_cast<std::uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    writeByte(static_cast<std::uint8_t>(value));
}

void BinaryWriter::writeUInt64(std::uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        writeByte(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffU));
    }
}

void BinaryWriter::writeInt64(std::int64_t value)
{
    writeUInt64(static_cast<std::uint64_t>(value));
}

void BinaryWriter::writeSize(std::size_t value)
{
    writeUInt64(static_cast<std::uint64_t>(value));
}

void BinaryWriter::writeDouble(double value)
{
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(value));
    writeUInt64(bits);
}

void BinaryWriter::writeString(const std::string &value)
{
    writeSize(value.size());
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void BinaryWriter::writeBytes(const ByteArray &value)
{
    writeSize(value.size());
    bytes.insert(bytes.end(), value.begin(), value.end());
}

BinaryReader::BinaryReader(const ByteArray &bytes) : _bytes(bytes) {}

bool BinaryReader::atEnd() const noexcept
{
    return _offset == _bytes.size();
}

std::size_t BinaryReader::remainingSize() const noexcept
{
    return _bytes.size() - _offset;
}

std::uint8_t BinaryReader::readByte()
{
    if (_offset >= _bytes.size()) {
        throw LogError("unexpected end of bytes");
    }
    return _bytes[_offset++];
}

bool BinaryReader::readBool()
{
    const auto value = readByte();
    if (value > 1) {
        throw LogError("invalid boolean encoding");
    }
    return value != 0;
}

std::uint32_t BinaryReader::readUInt32()
{
    constexpr std::size_t width = sizeof(std::uint32_t);
    if (_bytes.size() - _offset < width) {
        throw LogError("unexpected end of bytes");
    }
    const auto value = static_cast<std::uint32_t>(_bytes[_offset]) |
                       (static_cast<std::uint32_t>(_bytes[_offset + 1]) << 8) |
                       (static_cast<std::uint32_t>(_bytes[_offset + 2]) << 16) |
                       (static_cast<std::uint32_t>(_bytes[_offset + 3]) << 24);
    _offset += width;
    return value;
}

std::uint32_t BinaryReader::readVarUInt32()
{
    std::uint32_t value = 0;
    for (int shift = 0; shift <= 28; shift += 7) {
        const auto byte = readByte();
        if (shift == 28 && byte > 0x0f) {
            break;
        }
        value |= static_cast<std::uint32_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return value;
        }
    }
    throw LogError("variable-length integer is out of range");
}

std::uint64_t BinaryReader::readUInt64()
{
    constexpr std::size_t width = sizeof(std::uint64_t);
    if (_bytes.size() - _offset < width) {
        throw LogError("unexpected end of bytes");
    }
    const auto value = static_cast<std::uint64_t>(_bytes[_offset]) |
                       (static_cast<std::uint64_t>(_bytes[_offset + 1]) << 8) |
                       (static_cast<std::uint64_t>(_bytes[_offset + 2]) << 16) |
                       (static_cast<std::uint64_t>(_bytes[_offset + 3]) << 24) |
                       (static_cast<std::uint64_t>(_bytes[_offset + 4]) << 32) |
                       (static_cast<std::uint64_t>(_bytes[_offset + 5]) << 40) |
                       (static_cast<std::uint64_t>(_bytes[_offset + 6]) << 48) |
                       (static_cast<std::uint64_t>(_bytes[_offset + 7]) << 56);
    _offset += width;
    return value;
}

std::int64_t BinaryReader::readInt64()
{
    return static_cast<std::int64_t>(readUInt64());
}

std::size_t BinaryReader::readSize()
{
    const auto value = readUInt64();
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw LogError("size value is too large");
    }
    return static_cast<std::size_t>(value);
}

double BinaryReader::readDouble()
{
    const auto bits = readUInt64();
    double value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string BinaryReader::readString()
{
    const auto size = readSize();
    if (_bytes.size() - _offset < size) {
        throw LogError("string payload is truncated");
    }
    std::string value(reinterpret_cast<const char *>(_bytes.data() + _offset), size);
    _offset += size;
    return value;
}

ByteArray BinaryReader::readBytes()
{
    const auto size = readSize();
    if (_bytes.size() - _offset < size) {
        throw LogError("binary payload is truncated");
    }
    ByteArray value(_bytes.begin() + static_cast<std::ptrdiff_t>(_offset),
                    _bytes.begin() + static_cast<std::ptrdiff_t>(_offset + size));
    _offset += size;
    return value;
}

void writeValue(BinaryWriter &writer, const Value &value)
{
    writer.writeByte(static_cast<std::uint8_t>(value.type()));
    std::visit(
        [&](const auto &payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, bool>) {
                writer.writeBool(payload);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                writer.writeInt64(payload);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                writer.writeUInt64(payload);
            } else if constexpr (std::is_same_v<T, double>) {
                writer.writeDouble(payload);
            } else if constexpr (std::is_same_v<T, std::string>) {
                writer.writeString(payload);
            } else if constexpr (std::is_same_v<T, ByteArray>) {
                writer.writeBytes(payload);
            }
        },
        value.storage());
}

Value readValue(BinaryReader &reader)
{
    const auto type = static_cast<ValueType>(reader.readByte());
    switch (type) {
        case ValueType::Null:
            return Value::null();
        case ValueType::Bool:
            return Value(reader.readBool());
        case ValueType::Int64:
            return Value(reader.readInt64());
        case ValueType::UInt64:
            return Value(reader.readUInt64());
        case ValueType::Double:
            return Value(reader.readDouble());
        case ValueType::String:
            return Value(reader.readString());
        case ValueType::Binary:
            return Value(reader.readBytes());
    }
    throw LogError("unknown value type");
}

SchemaHandleCache::SchemaHandleCache(const SchemaDefinitionData &schema)
    : _schema(schema),
      _lists(schema.nextContainerId),
      _columns(schema.nextColumnId),
      _variants(schema.nextVariantId)
{
    for (const auto &container : schema.containers) {
        for (const auto &variant : container.variants) {
            _variants[variant.id].emplace(schema.schemaId, variant.containerId, variant.id, variant.debugName);
        }
    }
}

const ContainerDefinitionRecord &SchemaHandleCache::container(ContainerId id) const
{
    return _schema.containers.at(static_cast<std::size_t>(id) - 1);
}

const ListHandle &SchemaHandleCache::list(ContainerId id)
{
    auto &handle = _lists.at(id);
    if (!handle) {
        handle.emplace(_schema.schemaId, id, container(id).info.debugName);
    }
    return *handle;
}

const ColumnHandle &SchemaHandleCache::column(ColumnId id)
{
    auto &handle = _columns.at(id);
    if (!handle) {
        const auto [containerId, position] = _schema.columnLocations.at(id);
        const auto &column = container(containerId).columns[position];
        handle.emplace(_schema.schemaId, containerId, id, column.info.debugName);
    }
    return *handle;
}

const VariantHandle &SchemaHandleCache::variant(VariantId id)
{
    return _variants.at(id).value();
}

ByteArray serializeChangeSet(const ChangeSet &changeSet)
{
    BinaryWriter writer;
    writeMagic(writer);
    writer.writeUInt32(changeSetFormatVersion);
    writer.writeSize(changeSet.operations().size());
    for (const auto &operation : changeSet.operations()) {
        writeOperation(writer, operation);
    }
    writer.writeSize(changeSet.derivedLinks().size());
    for (const auto &link : changeSet.derivedLinks()) {
        writer.writeSize(link.sourceOperation);
        writer.writeSize(link.derivedOperation);
    }
    return std::move(writer.bytes);
}

ChangeSet deserializeChangeSet(const ByteArray &bytes, const EngineSchema &schema)
{
    BinaryReader reader(bytes);
    readMagic(reader);
    const auto version = reader.readUInt32();
    if (version != changeSetFormatVersion) {
        throw LogError("unsupported change set version");
    }
    try {
        SchemaHandleCache handles(schemaData(schema));
        ChangeSet changeSet;
        const auto operationCount = reader.readSize();
        for (std::size_t i = 0; i < operationCount; ++i) {
            changeSet.append(readOperation(reader, handles));
        }
        const auto linkCount = reader.readSize();
        for (std::size_t i = 0; i < linkCount; ++i) {
            changeSet.addDerivedLink(DerivedChangeLink {
                .sourceOperation = reader.readSize(),
                .derivedOperation = reader.readSize(),
            });
        }
        if (!reader.atEnd()) {
            throw LogError("trailing bytes after change set");
        }
        return changeSet;
    } catch (const std::out_of_range &) {
        throw LogError("change set index is out of range");
    }
}

} // namespace dini
