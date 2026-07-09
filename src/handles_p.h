#ifndef DINI_HANDLES_P_H
#define DINI_HANDLES_P_H

#include <string>
#include <utility>

#include <dini/handles.h>

namespace dini {

struct HandleData : SharedData {
    SchemaId schemaId = 0;
    ContainerId containerId = 0;
    ColumnId columnId = 0;
    RelationId relationId = 0;
    VariantId variantId = 0;
    std::uint32_t indexId = 0;
    std::string debugName;
};

struct TableHandle::Impl : HandleData {
    using Decl = TableHandle;
    Decl *_decl = nullptr;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
};

struct ListHandle::Impl : HandleData {
    using Decl = ListHandle;
    Decl *_decl = nullptr;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
};

struct ColumnHandle::Impl : HandleData {
    using Decl = ColumnHandle;
    Decl *_decl = nullptr;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
};

struct RelationHandle::Impl : HandleData {
    using Decl = RelationHandle;
    Decl *_decl = nullptr;
    ColumnHandle column;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
};

struct VariantHandle::Impl : HandleData {
    using Decl = VariantHandle;
    Decl *_decl = nullptr;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
};

struct OrderedIndexHandle::Impl : HandleData {
    using Decl = OrderedIndexHandle;
    Decl *_decl = nullptr;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
};

struct IntervalIndexHandle::Impl : HandleData {
    using Decl = IntervalIndexHandle;
    Decl *_decl = nullptr;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
};

} // namespace dini

#endif // DINI_HANDLES_P_H
