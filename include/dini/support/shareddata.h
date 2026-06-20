#ifndef DINI_SUPPORT_SHAREDDATA_H
#define DINI_SUPPORT_SHAREDDATA_H

#include <atomic>
#include <cstddef>
#include <utility>

#include <dini/diniglobal.h>

namespace dini {

/**
 * @brief Intrusive reference-counted base for implicitly shared public data classes.
 *
 * SharedData provides the minimal reference counter needed by SharedDataPointer.
 * It is intended for value-semantic API classes whose public object should behave
 * like an inexpensive copy while hiding its private representation.
 */
class DINI_EXPORT SharedData {
public:
    /**
     * @brief Creates a shared data block with one reference.
     *
     * @pre None.
     * @post refCount() returns 1.
     */
    SharedData() noexcept = default;

    /**
     * @brief Copies a shared data block payload while resetting the reference count.
     *
     * @pre None.
     * @post refCount() returns 1 for the copied block.
     */
    SharedData(const SharedData &) noexcept : _ref(1) {}

    /**
     * @brief Assigns shared data payload without copying the reference count.
     *
     * @pre None.
     * @post The target reference count is unchanged.
     */
    SharedData &operator=(const SharedData &) noexcept { return *this; }

    /**
     * @brief Destroys the shared data block.
     *
     * @pre The block must not be referenced by any SharedDataPointer.
     * @post Payload storage is released by the derived type destructor.
     */
    virtual ~SharedData() = default;

    /**
     * @brief Returns the current intrusive reference count.
     *
     * @pre None.
     * @post The reference count is not modified.
     */
    std::size_t refCount() const noexcept { return _ref.load(std::memory_order_relaxed); }

private:
    mutable std::atomic_size_t _ref {1};

    template <typename T>
    friend class SharedDataPointer;
};

/**
 * @brief Copy-on-write intrusive shared data pointer.
 *
 * SharedDataPointer is a small Qt-style value pimpl helper. Copying the pointer
 * shares the private data block; mutable access through data(), operator->(), or
 * operator*() detaches when the block is shared.
 *
 * @tparam T Implementation data type. T must derive from SharedData and be copy
 * constructible when detach() is used.
 */
template <typename T>
class SharedDataPointer {
public:
    /**
     * @brief Creates a null shared data pointer.
     *
     * @pre None.
     * @post isNull() returns true.
     */
    constexpr SharedDataPointer() noexcept = default;

    /**
     * @brief Takes ownership of a newly allocated data block.
     *
     * @param data Data block pointer or nullptr.
     * @pre data must either be nullptr or have a reference count of 1.
     * @post This pointer owns one reference to data.
     */
    explicit SharedDataPointer(T *data) noexcept : _data(data) {}

    /**
     * @brief Shares another data pointer.
     *
     * @param other Pointer to copy.
     * @pre None.
     * @post This pointer references the same block as other.
     */
    SharedDataPointer(const SharedDataPointer &other) noexcept : _data(other._data) { ref(); }

    /**
     * @brief Moves another data pointer.
     *
     * @param other Pointer to move from.
     * @pre None.
     * @post This pointer owns other's previous reference and other becomes null.
     */
    SharedDataPointer(SharedDataPointer &&other) noexcept : _data(std::exchange(other._data, nullptr)) {}

    /**
     * @brief Releases one reference.
     *
     * @pre None.
     * @post The data block is deleted when the last reference is released.
     */
    ~SharedDataPointer() { deref(); }

    /**
     * @brief Shares another data pointer.
     *
     * @param other Pointer to copy.
     * @pre None.
     * @post This pointer references the same block as other.
     */
    SharedDataPointer &operator=(const SharedDataPointer &other) noexcept
    {
        if (this != &other) {
            other.ref();
            deref();
            _data = other._data;
        }
        return *this;
    }

    /**
     * @brief Move-assigns another data pointer.
     *
     * @param other Pointer to move from.
     * @pre None.
     * @post This pointer owns other's previous reference and other becomes null.
     */
    SharedDataPointer &operator=(SharedDataPointer &&other) noexcept
    {
        if (this != &other) {
            deref();
            _data = std::exchange(other._data, nullptr);
        }
        return *this;
    }

    /**
     * @brief Returns true when no data block is referenced.
     *
     * @pre None.
     * @post The pointer is not modified.
     */
    bool isNull() const noexcept { return _data == nullptr; }

    /**
     * @brief Returns true when a data block is referenced.
     *
     * @pre None.
     * @post The pointer is not modified.
     */
    explicit operator bool() const noexcept { return _data != nullptr; }

    /**
     * @brief Returns the referenced data block without detaching.
     *
     * @pre None.
     * @post The pointer is not modified.
     */
    const T *constData() const noexcept { return _data; }

    /**
     * @brief Returns a mutable data block, detaching first if shared.
     *
     * @pre T must be copy constructible if the current block is shared.
     * @post If the pointer is non-null, the returned block is uniquely referenced.
     */
    T *data()
    {
        detach();
        return _data;
    }

    /**
     * @brief Returns a mutable data block, detaching first if shared.
     *
     * @pre T must be copy constructible if the current block is shared.
     * @post If the pointer is non-null, the returned block is uniquely referenced.
     */
    T *operator->() { return data(); }

    /**
     * @brief Returns the referenced data block without detaching.
     *
     * @pre _data must not be nullptr.
     * @post The pointer is not modified.
     */
    const T *operator->() const noexcept { return _data; }

    /**
     * @brief Returns a mutable data reference, detaching first if shared.
     *
     * @pre _data must not be nullptr.
     * @post The referenced block is uniquely owned by this pointer.
     */
    T &operator*() { return *data(); }

    /**
     * @brief Returns a const data reference without detaching.
     *
     * @pre _data must not be nullptr.
     * @post The pointer is not modified.
     */
    const T &operator*() const noexcept { return *_data; }

    /**
     * @brief Ensures this pointer has a unique copy of the referenced data block.
     *
     * @pre T must be copy constructible if the block is shared.
     * @post The pointer is null or references a block with refCount() == 1.
     */
    void detach()
    {
        if (!_data || _data->_ref.load(std::memory_order_acquire) == 1) {
            return;
        }
        T *copy = new T(*_data);
        deref();
        _data = copy;
    }

    /**
     * @brief Replaces the referenced block.
     *
     * @param data New data block pointer or nullptr.
     * @pre data must either be nullptr or have a reference count of 1.
     * @post This pointer owns one reference to data and the previous reference is released.
     */
    void reset(T *data = nullptr) noexcept
    {
        if (_data == data) {
            return;
        }
        deref();
        _data = data;
    }

private:
    void ref() const noexcept
    {
        if (_data) {
            _data->_ref.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void deref() noexcept
    {
        if (_data && _data->_ref.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete _data;
        }
    }

    T *_data = nullptr;
};

} // namespace dini

namespace stdc::pimpl::_private_ {

template <class T, class T1 = T>
inline const T *get_impl_helper(const dini::SharedDataPointer<T1> &data)
{
    return static_cast<const T *>(data.constData());
}

template <class T, class T1 = T>
inline T *get_impl_helper(dini::SharedDataPointer<T1> &data)
{
    return static_cast<T *>(data.data());
}

} // namespace stdc::pimpl::_private_

#endif // DINI_SUPPORT_SHAREDDATA_H
