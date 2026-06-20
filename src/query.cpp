#include "query_p.h"

#include <utility>

#include <stdcorelib/pimpl.h>

#include <dini/errors.h>

namespace dini {

FieldRef::FieldRef() : _impl(new Impl(this)) {}

FieldRef FieldRef::id()
{
    FieldRef field;
    field._impl.data()->kind = FieldKind::Id;
    return field;
}

FieldRef FieldRef::parent(RelationHandle relation)
{
    FieldRef field;
    auto *impl = field._impl.data();
    impl->kind = FieldKind::Parent;
    impl->relation = std::move(relation);
    return field;
}

FieldRef FieldRef::variant(VariantHandle variant)
{
    FieldRef field;
    auto *impl = field._impl.data();
    impl->kind = FieldKind::Variant;
    impl->variant = std::move(variant);
    return field;
}

FieldRef FieldRef::column(ColumnHandle column)
{
    FieldRef field;
    auto *impl = field._impl.data();
    impl->kind = FieldKind::Column;
    impl->column = std::move(column);
    return field;
}

FieldRef::~FieldRef() = default;
FieldRef::FieldRef(const FieldRef &other) = default;
FieldRef::FieldRef(FieldRef &&other) noexcept = default;
FieldRef &FieldRef::operator=(const FieldRef &other) = default;
FieldRef &FieldRef::operator=(FieldRef &&other) noexcept = default;

FieldKind FieldRef::kind() const noexcept
{
    __stdc_impl_t;
    return impl.kind;
}

ColumnHandle FieldRef::column() const
{
    __stdc_impl_t;
    if (impl.kind != FieldKind::Column) {
        throw QueryError("field reference is not a column");
    }
    return impl.column;
}

RelationHandle FieldRef::relation() const
{
    __stdc_impl_t;
    if (impl.kind != FieldKind::Parent) {
        throw QueryError("field reference is not a relation");
    }
    return impl.relation;
}

VariantHandle FieldRef::variant() const
{
    __stdc_impl_t;
    if (impl.kind != FieldKind::Variant) {
        throw QueryError("field reference is not a variant");
    }
    return impl.variant;
}

Filter::Filter() : _impl(new Impl(this)) {}

Filter::Filter(FieldRef field, ComparisonOperator op, Value value)
    : _impl(new Impl(this, std::move(field), op, std::move(value)))
{
}

Filter::~Filter() = default;
Filter::Filter(const Filter &other) = default;
Filter::Filter(Filter &&other) noexcept = default;
Filter &Filter::operator=(const Filter &other) = default;
Filter &Filter::operator=(Filter &&other) noexcept = default;

const FieldRef &Filter::field() const noexcept
{
    __stdc_impl_t;
    return impl.field;
}

ComparisonOperator Filter::comparisonOperator() const noexcept
{
    __stdc_impl_t;
    return impl.comparisonOperator;
}

const Value &Filter::value() const noexcept
{
    __stdc_impl_t;
    return impl.value;
}

FilterExpression::FilterExpression() : _impl(new Impl(this)) {}

FilterExpression::FilterExpression(Filter filter) : _impl(new Impl(this))
{
    __stdc_impl_t;
    impl.empty = false;
    impl.filter = std::move(filter);
}

FilterExpression::~FilterExpression() = default;
FilterExpression::FilterExpression(const FilterExpression &other) = default;
FilterExpression::FilterExpression(FilterExpression &&other) noexcept = default;
FilterExpression &FilterExpression::operator=(const FilterExpression &other) = default;
FilterExpression &FilterExpression::operator=(FilterExpression &&other) noexcept = default;

FilterExpression FilterExpression::all(std::vector<FilterExpression> children)
{
    FilterExpression expression;
    auto *impl = expression._impl.data();
    impl->empty = false;
    impl->op = FilterOperator::And;
    impl->children = std::move(children);
    return expression;
}

FilterExpression FilterExpression::any(std::vector<FilterExpression> children)
{
    FilterExpression expression;
    auto *impl = expression._impl.data();
    impl->empty = false;
    impl->op = FilterOperator::Or;
    impl->children = std::move(children);
    return expression;
}

FilterExpression FilterExpression::negate(FilterExpression child)
{
    FilterExpression expression;
    auto *impl = expression._impl.data();
    impl->empty = false;
    impl->op = FilterOperator::Not;
    impl->children.push_back(std::move(child));
    return expression;
}

bool FilterExpression::isEmpty() const noexcept
{
    __stdc_impl_t;
    return impl.empty;
}

const FilterExpression::Impl &filterExpressionImpl(const FilterExpression &expression)
{
    return *expression._impl.constData();
}

} // namespace dini
