#ifndef DINI_QUERY_P_H
#define DINI_QUERY_P_H

#include <optional>
#include <utility>
#include <vector>

#include <dini/query.h>

namespace dini {

struct FieldRef::Impl : SharedData {
    using Decl = FieldRef;

    Decl *_decl = nullptr;
    FieldKind kind = FieldKind::Column;
    ColumnHandle column;
    RelationHandle relation;
    VariantHandle variant;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
};

struct Filter::Impl : SharedData {
    using Decl = Filter;

    Decl *_decl = nullptr;
    FieldRef field;
    ComparisonOperator comparisonOperator = ComparisonOperator::Equal;
    Value value;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
    Impl(Decl *decl, FieldRef field, ComparisonOperator comparisonOperator, Value value)
        : _decl(decl), field(std::move(field)), comparisonOperator(comparisonOperator), value(std::move(value))
    {
    }
};

struct FilterExpression::Impl : SharedData {
    using Decl = FilterExpression;

    Decl *_decl = nullptr;
    bool empty = true;
    std::optional<Filter> filter;
    std::optional<FilterOperator> op;
    std::vector<FilterExpression> children;

    Impl() = default;
    explicit Impl(Decl *decl) : _decl(decl) {}
};

const FilterExpression::Impl &filterExpressionImpl(const FilterExpression &expression);

} // namespace dini

#endif // DINI_QUERY_P_H
