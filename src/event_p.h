#ifndef DINI_EVENT_P_H
#define DINI_EVENT_P_H

#include <functional>
#include <memory>

#include <dini/event.h>

namespace dini {

struct SubscriptionState {
    bool connected = true;
    EventCallback callback;
};

struct Subscription::Impl {
    std::shared_ptr<SubscriptionState> state;
};

} // namespace dini

#endif // DINI_EVENT_P_H
