#ifndef DINI_CHANGE_P_H
#define DINI_CHANGE_P_H

#include <string>
#include <utility>
#include <vector>

#include <dini/change.h>

namespace dini {

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
ChangeSet deserializeChangeSet(const ByteArray &bytes);

struct BinaryWriter {
    ByteArray bytes;

    void writeByte(std::uint8_t value);
    void writeBool(bool value);
    void writeUInt32(std::uint32_t value);
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
    std::uint8_t readByte();
    bool readBool();
    std::uint32_t readUInt32();
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
void writeColumnHandle(BinaryWriter &writer, const ColumnHandle &handle);
ColumnHandle readColumnHandle(BinaryReader &reader);
void writeListHandle(BinaryWriter &writer, const ListHandle &handle);
ListHandle readListHandle(BinaryReader &reader);
void writeVariantHandle(BinaryWriter &writer, const VariantHandle &handle);
VariantHandle readVariantHandle(BinaryReader &reader);
void writeItemSnapshot(BinaryWriter &writer, const ItemSnapshot &snapshot);
ItemSnapshot readItemSnapshot(BinaryReader &reader);

} // namespace dini

#endif // DINI_CHANGE_P_H
