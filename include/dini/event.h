#ifndef DINI_EVENT_H
#define DINI_EVENT_H

#include <functional>
#include <memory>

#include <dini/change.h>
#include <dini/diniglobal.h>
#include <dini/types.h>

namespace dini {

/**
 * @brief Event payload delivered by the pure C++ subscription API.
 *
 * EngineEvent carries the semantic ChangeSet that observers need to update
 * external caches or UI bridges. It has no Qt dependency and does not define
 * threading guarantees.
 */
struct DINI_EXPORT EngineEvent {
    EventKind kind = EventKind::AfterApply;
    EventOrigin origin = EventOrigin::Normal;
    ChangeSet changeSet;
};

/**
 * @brief Callback type used by DocumentEngine event subscriptions.
 *
 * The callback is invoked synchronously by the engine implementation. Exceptions
 * from callbacks follow the hook/event propagation rules documented by the
 * calling API.
 */
using EventCallback = std::function<void(const EngineEvent &)>;

/**
 * @brief RAII handle for a document engine event subscription.
 *
 * Subscription owns a registration token. Destroying or disconnecting it must
 * prevent future callbacks through that registration without requiring Qt or any
 * external observer framework.
 */
class DINI_EXPORT Subscription {
public:
    /**
     * @brief Creates an empty disconnected subscription.
     *
     * @pre None.
     * @post connected() returns false.
     */
    Subscription();

    /**
     * @brief Destroys the subscription and disconnects it if still connected.
     *
     * @pre No public precondition.
     * @post No future event is delivered through this subscription.
     */
    ~Subscription();

    /**
     * @brief Copy construction is disabled because a subscription owns one registration token.
     *
     * @pre None.
     * @post This overload is unavailable; use move construction to transfer ownership.
     */
    Subscription(const Subscription &other) = delete;

    /**
     * @brief Copy assignment is disabled because a subscription owns one registration token.
     *
     * @pre None.
     * @post This overload is unavailable; use move assignment to transfer ownership.
     */
    Subscription &operator=(const Subscription &other) = delete;

    /**
     * @brief Moves a subscription.
     *
     * @param other Subscription to move from.
     * @pre other may be connected or disconnected.
     * @post This object owns the previous registration and other is disconnected.
     */
    Subscription(Subscription &&other) noexcept;

    /**
     * @brief Move-assigns a subscription.
     *
     * @param other Subscription to move from.
     * @pre This subscription may be connected; it is disconnected before assignment.
     * @post This object owns the moved registration and other is disconnected.
     */
    Subscription &operator=(Subscription &&other) noexcept;

    /**
     * @brief Disconnects the subscription.
     *
     * @pre None.
     * @post connected() returns false and no future callback is delivered.
     */
    void disconnect() noexcept;

    /**
     * @brief Tests whether this subscription is currently connected.
     *
     * @pre None.
     * @post The subscription state is not modified.
     */
    bool connected() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

    explicit Subscription(std::unique_ptr<Impl> data);
    friend class DocumentEngine;
};

} // namespace dini

#endif // DINI_EVENT_H
