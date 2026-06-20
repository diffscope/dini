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
    EngineSchema schema;
    std::optional<ContainerId> containerId;

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
