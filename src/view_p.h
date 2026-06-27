#ifndef DINI_VIEW_P_H
#define DINI_VIEW_P_H

#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <dini/schema.h>
#include <dini/view.h>

namespace dini {

struct View::Impl : SharedData {
    using Decl = View;

    Decl *_decl = nullptr;
    std::function<std::vector<ItemSnapshot>()> evaluator;
    std::function<void(const std::function<bool(const ItemSnapshot &)> &,
                       std::size_t,
                       std::optional<std::size_t>)> streamer;
    std::function<std::optional<std::vector<AggregationResult>>(const AggregationSpec &,
                                                                 std::size_t,
                                                                 std::optional<std::size_t>)> aggregator;
    std::function<std::optional<std::size_t>(std::size_t, std::optional<std::size_t>)> counter;
    EngineSchema schema;
    std::optional<ContainerId> containerId;
    std::size_t offset = 0;
    std::optional<std::size_t> limit;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
    Impl(Decl *decl, std::function<std::vector<ItemSnapshot>()> evaluator)
        : _decl(decl), evaluator(std::move(evaluator))
    {
    }
    Impl(Decl *decl,
         std::function<std::vector<ItemSnapshot>()> evaluator,
         EngineSchema schema,
         std::optional<ContainerId> containerId)
        : _decl(decl),
          evaluator(std::move(evaluator)),
          schema(std::move(schema)),
          containerId(std::move(containerId))
    {
    }
};

struct AggregationView::Impl : SharedData {
    using Decl = AggregationView;

    Decl *_decl = nullptr;
    std::function<std::vector<AggregationResult>()> evaluator;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
    Impl(Decl *decl, std::function<std::vector<AggregationResult>()> evaluator)
        : _decl(decl), evaluator(std::move(evaluator))
    {
    }
};

} // namespace dini

#endif // DINI_VIEW_P_H
