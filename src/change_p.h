#ifndef DINI_CHANGE_P_H
#define DINI_CHANGE_P_H

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <dini/change.h>

namespace dini {

struct ContainerDefinitionRecord;
struct SchemaDefinitionData;

struct ChangeOperation::Impl : SharedData {
    using Decl = ChangeOperation;

    Decl *_decl = nullptr;
    Payload payload;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
    Impl(Decl *decl, Payload payload) : _decl(decl), payload(std::move(payload)) {}
};

struct ChangeSet::Impl : SharedData {
    using Decl = ChangeSet;

    Decl *_decl = nullptr;
    std::vector<ChangeOperation> operations;
    std::vector<DerivedChangeLink> derivedLinks;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
    Impl(Decl *decl, std::vector<ChangeOperation> operations) : _decl(decl), operations(std::move(operations)) {}
};

struct UndoStep::Impl : SharedData {
    using Decl = UndoStep;

    Decl *_decl = nullptr;
    ChangeSet changeSet;
    std::string label;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
    Impl(Decl *decl, ChangeSet changeSet, std::string label)
        : _decl(decl), changeSet(std::move(changeSet)), label(std::move(label))
    {
    }
};

ByteArray serializeChangeSet(const ChangeSet &changeSet);
ChangeSet deserializeChangeSet(const ByteArray &bytes, const EngineSchema &schema);

class SchemaHandleCache {
public:
    explicit SchemaHandleCache(const SchemaDefinitionData &schema);

    const ContainerDefinitionRecord &container(ContainerId id) const;
    const ListHandle &list(ContainerId id);
    const ColumnHandle &column(ColumnId id);
    const VariantHandle &variant(VariantId id);

private:
    const SchemaDefinitionData &_schema;
    std::vector<std::optional<ListHandle>> _lists;
    std::vector<std::optional<ColumnHandle>> _columns;
    std::vector<std::optional<VariantHandle>> _variants;
};

void restoreItemRelationMetadata(const ContainerDefinitionRecord &container, ItemSnapshot &snapshot);

struct BinaryWriter {
    ByteArray bytes;

    void writeByte(std::uint8_t value);
    void writeBool(bool value);
    void writeUInt32(std::uint32_t value);
    void writeVarUInt32(std::uint32_t value);
    void writeUInt64(std::uint64_t value);
    void writeInt64(std::int64_t value);
    void writeSize(std::size_t value);
    void writeDouble(double value);
    void writeString(const std::string &value);
    void writeBytes(const ByteArray &value);
};

class BinaryReader {
public:
    explicit BinaryReader(const ByteArray &bytes);

    bool atEnd() const noexcept;
    std::size_t remainingSize() const noexcept;
    std::uint8_t readByte();
    bool readBool();
    std::uint32_t readUInt32();
    std::uint32_t readVarUInt32();
    std::uint64_t readUInt64();
    std::int64_t readInt64();
    std::size_t readSize();
    double readDouble();
    std::string readString();
    ByteArray readBytes();

private:
    const ByteArray &_bytes;
    std::size_t _offset = 0;
};

void writeValue(BinaryWriter &writer, const Value &value);
Value readValue(BinaryReader &reader);

} // namespace dini

#endif // DINI_CHANGE_P_H
