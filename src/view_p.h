#ifndef DINI_VIEW_P_H
#define DINI_VIEW_P_H

#include <functional>
#include <utility>
#include <vector>

#include <dini/view.h>

namespace dini {

struct View::Impl : SharedData {
    using Decl = View;

    Decl *_decl = nullptr;
    std::function<std::vector<ItemSnapshot>()> evaluator;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
    Impl(Decl *decl, std::function<std::vector<ItemSnapshot>()> evaluator)
        : _decl(decl), evaluator(std::move(evaluator))
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
