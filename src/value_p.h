#ifndef DINI_VALUE_P_H
#define DINI_VALUE_P_H

#include <utility>

#include <dini/value.h>

namespace dini {

struct Value::Impl : SharedData {
    using Decl = Value;

    Decl *_decl = nullptr;
    Storage storage;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
    Impl(Decl *decl, Storage storage) : _decl(decl), storage(std::move(storage)) {}
};

} // namespace dini

#endif // DINI_VALUE_P_H
