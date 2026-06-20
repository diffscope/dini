#include "event_p.h"

#include <utility>

namespace dini {

Subscription::Subscription() : _impl(std::make_unique<Impl>()) {}

Subscription::Subscription(std::unique_ptr<Impl> data) : _impl(std::move(data)) {}

Subscription::~Subscription()
{
    disconnect();
}

Subscription::Subscription(Subscription &&other) noexcept = default;

Subscription &Subscription::operator=(Subscription &&other) noexcept
{
    if (this != &other) {
        disconnect();
        _impl = std::move(other._impl);
    }
    return *this;
}

void Subscription::disconnect() noexcept
{
    if (_impl && _impl->state) {
        _impl->state->connected = false;
        _impl->state.reset();
    }
}

bool Subscription::connected() const noexcept
{
    return _impl && _impl->state && _impl->state->connected;
}

} // namespace dini
