#ifndef DINI_VALUE_H
#define DINI_VALUE_H

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <dini/diniglobal.h>
#include <dini/support/shareddata.h>
#include <dini/types.h>

namespace dini {

/**
 * @brief Variant-like value object for all engine-supported scalar values.
 *
 * Value is the only public runtime value carrier used by columns, query
 * predicates, snapshots, logs, and change records. It deliberately supports only
 * the built-in type set from the engine specification and does not provide custom
 * type registration.
 */
class DINI_EXPORT Value {
public:
    using Binary = ByteArray;
    using Storage = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, Binary>;

    /**
     * @brief Creates a null value.
     *
     * @pre None.
     * @post type() returns ValueType::Null and isNull() returns true.
     */
    Value();

    /**
     * @brief Creates a boolean value.
     *
     * @param value Boolean payload.
     * @pre None.
     * @post type() returns ValueType::Bool.
     */
    Value(bool value);

    /**
     * @brief Creates a signed integer value.
     *
     * @param value Signed 64-bit payload.
     * @pre None.
     * @post type() returns ValueType::Int64.
     */
    Value(std::int64_t value);

    /**
     * @brief Creates an unsigned integer value.
     *
     * @param value Unsigned 64-bit payload.
     * @pre None.
     * @post type() returns ValueType::UInt64.
     */
    Value(std::uint64_t value);

    /**
     * @brief Creates a floating-point value.
     *
     * @param value Double precision payload.
     * @pre value should be finite when used in indexed comparisons.
     * @post type() returns ValueType::Double.
     */
    Value(double value);

    /**
     * @brief Creates a string value.
     *
     * @param value UTF-8 or application-defined string payload.
     * @pre None.
     * @post type() returns ValueType::String.
     */
    Value(std::string value);

    /**
     * @brief Creates a string value from a null-terminated character sequence.
     *
     * @param value Null-terminated string pointer.
     * @pre value must not be nullptr.
     * @post type() returns ValueType::String.
     * @throws std::invalid_argument if value is nullptr.
     */
    Value(const char *value);

    /**
     * @brief Creates a binary value.
     *
     * @param value Binary payload.
     * @pre None.
     * @post type() returns ValueType::Binary.
     */
    Value(Binary value);

    /**
     * @brief Destroys the value wrapper.
     *
     * @pre No public precondition.
     * @post The shared private value data is released when no Value objects reference it.
     */
    ~Value();

    /**
     * @brief Copies a value wrapper with implicit sharing.
     *
     * @param other Value to copy.
     * @pre other may hold any supported value type.
     * @post This Value shares the same private payload until a mutable operation detaches it.
     */
    Value(const Value &other);

    /**
     * @brief Moves a value wrapper.
     *
     * @param other Value to move from.
     * @pre other may hold any supported value type.
     * @post This Value receives other's payload and other remains valid.
     */
    Value(Value &&other) noexcept;

    /**
     * @brief Copy-assigns a value wrapper with implicit sharing.
     *
     * @param other Value to copy.
     * @pre other may hold any supported value type.
     * @post This Value shares the same private payload until a mutable operation detaches it.
     */
    Value &operator=(const Value &other);

    /**
     * @brief Move-assigns a value wrapper.
     *
     * @param other Value to move from.
     * @pre other may hold any supported value type.
     * @post This Value receives other's payload and other remains valid.
     */
    Value &operator=(Value &&other) noexcept;

    /**
     * @brief Returns a null Value object.
     *
     * @pre None.
     * @post The returned value is null.
     */
    static Value null();

    /**
     * @brief Returns the active value kind.
     *
     * @pre None.
     * @post The returned type matches the active storage alternative.
     */
    ValueType type() const noexcept;

    /**
     * @brief Tests whether this value is null.
     *
     * @pre None.
     * @post Returns true only when type() is ValueType::Null.
     */
    bool isNull() const noexcept;

    /**
     * @brief Returns the boolean payload.
     *
     * @pre type() must be ValueType::Bool.
     * @post The stored value is not modified.
     * @throws std::bad_variant_access if the active type is not bool.
     */
    bool asBool() const;

    /**
     * @brief Returns the signed integer payload.
     *
     * @pre type() must be ValueType::Int64.
     * @post The stored value is not modified.
     * @throws std::bad_variant_access if the active type is not int64.
     */
    std::int64_t asInt64() const;

    /**
     * @brief Returns the unsigned integer payload.
     *
     * @pre type() must be ValueType::UInt64.
     * @post The stored value is not modified.
     * @throws std::bad_variant_access if the active type is not uint64.
     */
    std::uint64_t asUInt64() const;

    /**
     * @brief Returns the floating-point payload.
     *
     * @pre type() must be ValueType::Double.
     * @post The stored value is not modified.
     * @throws std::bad_variant_access if the active type is not double.
     */
    double asDouble() const;

    /**
     * @brief Returns the string payload.
     *
     * @pre type() must be ValueType::String.
     * @post The returned reference remains valid while this Value is unchanged.
     * @throws std::bad_variant_access if the active type is not string.
     */
    const std::string &asString() const;

    /**
     * @brief Returns the binary payload.
     *
     * @pre type() must be ValueType::Binary.
     * @post The returned reference remains valid while this Value is unchanged.
     * @throws std::bad_variant_access if the active type is not binary.
     */
    const Binary &asBinary() const;

    /**
     * @brief Returns the underlying storage variant.
     *
     * @pre None.
     * @post The returned reference exposes the active payload without modifying it.
     */
    const Storage &storage() const noexcept;

    /**
     * @brief Compares two values for exact equality.
     *
     * @pre Both values must be comparable by their active types.
     * @post Returns true when type and payload are equal.
     */
    friend bool operator==(const Value &lhs, const Value &rhs);

    /**
     * @brief Compares two values for exact inequality.
     *
     * @pre Both values must be comparable by their active types.
     * @post Returns true when type or payload differ.
     */
    friend bool operator!=(const Value &lhs, const Value &rhs);

private:
    struct Impl;
    SharedDataPointer<Impl> _impl;
};

} // namespace dini

#endif // DINI_VALUE_H
