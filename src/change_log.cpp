#include "change_p.h"

#include <cstring>
#include <limits>

#include <dini/errors.h>

namespace dini {

    namespace {

    // Magic and version does not need to be changed during development, even if the format changes.
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
                    writeColumnHandle(writer, change.column);
                    writeValue(writer, change.oldValue);
                    writeValue(writer, change.newValue);
                    writeAssociationOptions(writer, change.associationOptions);
                    writer.writeBool(change.oldListIndex.has_value());
                    if (change.oldListIndex) {
                        writer.writeSize(*change.oldListIndex);
                    }
                } else if constexpr (std::is_same_v<T, ComputedColumnUpdatedChange>) {
                    writer.writeUInt64(change.itemId);
                    writeColumnHandle(writer, change.column);
                    writeValue(writer, change.oldValue);
                    writeValue(writer, change.newValue);
                } else if constexpr (std::is_same_v<T, CascadeRemovedChange>) {
                    writeItemSnapshot(writer, change.item);
                    writer.writeUInt64(change.ancestorId);
                } else if constexpr (std::is_same_v<T, ListInsertedChange>) {
                    writeListHandle(writer, change.list);
                    writeValue(writer, change.associationValue);
                    writer.writeSize(change.index);
                    writeItemSnapshot(writer, change.item);
                } else if constexpr (std::is_same_v<T, ListRemovedChange>) {
                    writeListHandle(writer, change.list);
                    writeValue(writer, change.associationValue);
                    writer.writeSize(change.index);
                    writeItemSnapshot(writer, change.item);
                } else if constexpr (std::is_same_v<T, ListRotatedChange>) {
                    writeListHandle(writer, change.list);
                    writeValue(writer, change.associationValue);
                    writeRotation(writer, change.rotation);
                }
            },
            operation.payload());
    }

    ChangeOperation readOperation(BinaryReader &reader)
    {
        const auto kind = static_cast<ChangeOperationKind>(reader.readByte());
        switch (kind) {
            case ChangeOperationKind::ItemInserted:
                return ChangeOperation(ItemInsertedChange {
                    .item = readItemSnapshot(reader),
                });
            case ChangeOperationKind::ItemRemoved:
                return ChangeOperation(ItemRemovedChange {
                    .item = readItemSnapshot(reader),
                    .cascade = reader.readBool(),
                });
            case ChangeOperationKind::ColumnUpdated:
            {
                ColumnUpdatedChange change {
                    .itemId = reader.readUInt64(),
                    .column = readColumnHandle(reader),
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
                    .column = readColumnHandle(reader),
                    .oldValue = readValue(reader),
                    .newValue = readValue(reader),
                });
            case ChangeOperationKind::CascadeRemoved:
                return ChangeOperation(CascadeRemovedChange {
                    .item = readItemSnapshot(reader),
                    .ancestorId = reader.readUInt64(),
                });
            case ChangeOperationKind::ListInserted:
                return ChangeOperation(ListInsertedChange {
                    .list = readListHandle(reader),
                    .associationValue = readValue(reader),
                    .index = reader.readSize(),
                    .item = readItemSnapshot(reader),
                });
            case ChangeOperationKind::ListRemoved:
                return ChangeOperation(ListRemovedChange {
                    .list = readListHandle(reader),
                    .associationValue = readValue(reader),
                    .index = reader.readSize(),
                    .item = readItemSnapshot(reader),
                });
            case ChangeOperationKind::ListRotated:
                return ChangeOperation(ListRotatedChange {
                    .list = readListHandle(reader),
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
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(readByte()) << (i * 8);
    }
    return value;
}

std::uint64_t BinaryReader::readUInt64()
{
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(readByte()) << (i * 8);
    }
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

void writeColumnHandle(BinaryWriter &writer, const ColumnHandle &handle)
{
    writer.writeUInt64(handle.schemaId());
    writer.writeUInt32(handle.containerId());
    writer.writeUInt32(handle.columnId());
    writer.writeString(handle.debugName());
}

ColumnHandle readColumnHandle(BinaryReader &reader)
{
    const auto schemaId = reader.readUInt64();
    const auto containerId = reader.readUInt32();
    const auto columnId = reader.readUInt32();
    auto debugName = reader.readString();
    return ColumnHandle(schemaId, containerId, columnId, std::move(debugName));
}

void writeListHandle(BinaryWriter &writer, const ListHandle &handle)
{
    writer.writeUInt64(handle.schemaId());
    writer.writeUInt32(handle.containerId());
    writer.writeString(handle.debugName());
}

ListHandle readListHandle(BinaryReader &reader)
{
    const auto schemaId = reader.readUInt64();
    const auto containerId = reader.readUInt32();
    auto debugName = reader.readString();
    return ListHandle(schemaId, containerId, std::move(debugName));
}

void writeVariantHandle(BinaryWriter &writer, const VariantHandle &handle)
{
    writer.writeUInt64(handle.schemaId());
    writer.writeUInt32(handle.containerId());
    writer.writeUInt32(handle.variantId());
    writer.writeString(handle.debugName());
}

VariantHandle readVariantHandle(BinaryReader &reader)
{
    const auto schemaId = reader.readUInt64();
    const auto containerId = reader.readUInt32();
    const auto variantId = reader.readUInt32();
    auto debugName = reader.readString();
    return VariantHandle(schemaId, containerId, variantId, std::move(debugName));
}

void writeItemSnapshot(BinaryWriter &writer, const ItemSnapshot &snapshot)
{
    writer.writeUInt64(snapshot.id);
    writer.writeByte(static_cast<std::uint8_t>(snapshot.containerKind));
    writer.writeUInt32(snapshot.containerId);
    writer.writeBool(snapshot.parentId.has_value());
    if (snapshot.parentId) {
        writer.writeUInt64(*snapshot.parentId);
    }
    writer.writeBool(snapshot.variant.has_value());
    if (snapshot.variant) {
        writeVariantHandle(writer, *snapshot.variant);
    }
    writer.writeSize(snapshot.values.size());
    for (const auto &columnValue : snapshot.values) {
        writeColumnHandle(writer, columnValue.column);
        writeValue(writer, columnValue.value);
    }
    writer.writeBool(snapshot.listAssociationValue.has_value());
    if (snapshot.listAssociationValue) {
        writeValue(writer, *snapshot.listAssociationValue);
    }
    writer.writeBool(snapshot.listIndex.has_value());
    if (snapshot.listIndex) {
        writer.writeSize(*snapshot.listIndex);
    }
}

ItemSnapshot readItemSnapshot(BinaryReader &reader)
{
    ItemSnapshot snapshot;
    snapshot.id = reader.readUInt64();
    snapshot.containerKind = static_cast<ContainerKind>(reader.readByte());
    snapshot.containerId = reader.readUInt32();
    if (reader.readBool()) {
        snapshot.parentId = reader.readUInt64();
    }
    if (reader.readBool()) {
        snapshot.variant = readVariantHandle(reader);
    }
    const auto valueCount = reader.readSize();
    snapshot.values.reserve(valueCount);
    for (std::size_t i = 0; i < valueCount; ++i) {
        snapshot.values.push_back(ColumnValue {
            .column = readColumnHandle(reader),
            .value = readValue(reader),
        });
    }
    if (reader.readBool()) {
        snapshot.listAssociationValue = readValue(reader);
    }
    if (reader.readBool()) {
        snapshot.listIndex = reader.readSize();
    }
    return snapshot;
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

ChangeSet deserializeChangeSet(const ByteArray &bytes)
{
    BinaryReader reader(bytes);
    readMagic(reader);
    const auto version = reader.readUInt32();
    if (version != changeSetFormatVersion) {
        throw LogError("unsupported change set version");
    }
    ChangeSet changeSet;
    const auto operationCount = reader.readSize();
    for (std::size_t i = 0; i < operationCount; ++i) {
        changeSet.append(readOperation(reader));
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
}

} // namespace dini
