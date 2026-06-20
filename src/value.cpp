#include "value_p.h"

#include <stdexcept>
#include <utility>

#include <stdcorelib/pimpl.h>

namespace dini {

Value::Value() : _impl(new Impl(this)) {}

Value::Value(bool value) : _impl(new Impl(this, value)) {}

Value::Value(std::int64_t value) : _impl(new Impl(this, value)) {}

Value::Value(std::uint64_t value) : _impl(new Impl(this, value)) {}

Value::Value(double value) : _impl(new Impl(this, value)) {}

Value::Value(std::string value) : _impl(new Impl(this, std::move(value))) {}

Value::Value(const char *value)
{
    if (!value) {
        throw std::invalid_argument("null string pointer");
    }
    _impl.reset(new Impl(this, std::string(value)));
}

Value::Value(Binary value) : _impl(new Impl(this, std::move(value))) {}

Value::~Value() = default;

Value::Value(const Value &other) = default;

Value::Value(Value &&other) noexcept = default;

Value &Value::operator=(const Value &other) = default;

Value &Value::operator=(Value &&other) noexcept = default;

Value Value::null()
{
    return Value();
}

ValueType Value::type() const noexcept
{
    __stdc_impl_t;
    switch (impl.storage.index()) {
        case 1:
            return ValueType::Bool;
        case 2:
            return ValueType::Int64;
        case 3:
            return ValueType::UInt64;
        case 4:
            return ValueType::Double;
        case 5:
            return ValueType::String;
        case 6:
            return ValueType::Binary;
        default:
            return ValueType::Null;
    }
}

bool Value::isNull() const noexcept
{
    return type() == ValueType::Null;
}

bool Value::asBool() const
{
    return std::get<bool>(storage());
}

std::int64_t Value::asInt64() const
{
    return std::get<std::int64_t>(storage());
}

std::uint64_t Value::asUInt64() const
{
    return std::get<std::uint64_t>(storage());
}

double Value::asDouble() const
{
    return std::get<double>(storage());
}

const std::string &Value::asString() const
{
    return std::get<std::string>(storage());
}

const Value::Binary &Value::asBinary() const
{
    return std::get<Binary>(storage());
}

const Value::Storage &Value::storage() const noexcept
{
    __stdc_impl_t;
    return impl.storage;
}

DINI_EXPORT bool operator==(const Value &lhs, const Value &rhs)
{
    return lhs.storage() == rhs.storage();
}

DINI_EXPORT bool operator!=(const Value &lhs, const Value &rhs)
{
    return !(lhs == rhs);
}

} // namespace dini
