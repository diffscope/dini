#include "view_p.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>
#include <utility>

#include <stdcorelib/pimpl.h>

#include "query_p.h"

namespace dini {

    namespace {

    Value idValue(ItemId id)
    {
        return Value(static_cast<std::uint64_t>(id));
    }

    Value valueForColumn(const ItemSnapshot &item, ColumnId columnId)
    {
        auto it = std::find_if(item.values.begin(), item.values.end(), [columnId](const ColumnValue &columnValue) {
            return columnValue.column.columnId() == columnId;
        });
        return it == item.values.end() ? Value::null() : it->value;
    }

    Value fieldValue(const ItemSnapshot &item, const FieldRef &field)
    {
        switch (field.kind()) {
            case FieldKind::Id:
                return idValue(item.id);
            case FieldKind::Parent:
                return item.parentId ? idValue(*item.parentId) : Value::null();
            case FieldKind::Variant:
                return item.variant ? Value(static_cast<std::uint64_t>(item.variant->variantId())) : Value::null();
            case FieldKind::Column:
                return valueForColumn(item, field.column().columnId());
        }
        return Value::null();
    }

    int compareValues(const Value &lhs, const Value &rhs)
    {
        if (lhs.storage() == rhs.storage()) {
            return 0;
        }
        if (lhs.type() != rhs.type()) {
            return static_cast<int>(lhs.type()) < static_cast<int>(rhs.type()) ? -1 : 1;
        }
        return std::visit(
            [](const auto &a, const auto &b) -> int {
                using A = std::decay_t<decltype(a)>;
                using B = std::decay_t<decltype(b)>;
                if constexpr (!std::is_same_v<A, B>) {
                    return 0;
                } else if constexpr (std::is_same_v<A, std::monostate>) {
                    return 0;
                } else {
                    return a < b ? -1 : 1;
                }
            },
            lhs.storage(),
            rhs.storage());
    }

    bool comparePredicate(const Value &lhs, ComparisonOperator op, const Value &rhs)
    {
        const auto comparison = compareValues(lhs, rhs);
        switch (op) {
            case ComparisonOperator::Equal:
                return comparison == 0;
            case ComparisonOperator::NotEqual:
                return comparison != 0;
            case ComparisonOperator::Greater:
                return comparison > 0;
            case ComparisonOperator::Less:
                return comparison < 0;
            case ComparisonOperator::GreaterOrEqual:
                return comparison >= 0;
            case ComparisonOperator::LessOrEqual:
                return comparison <= 0;
        }
        return false;
    }

    bool matchesExpression(const ItemSnapshot &item, const FilterExpression &expression)
    {
        const auto &impl = filterExpressionImpl(expression);
        if (impl.empty) {
            return true;
        }
        if (impl.filter) {
            return comparePredicate(fieldValue(item, impl.filter->field()),
                                    impl.filter->comparisonOperator(),
                                    impl.filter->value());
        }
        if (impl.op == FilterOperator::And) {
            return std::all_of(impl.children.begin(), impl.children.end(), [&](const auto &child) {
                return matchesExpression(item, child);
            });
        }
        if (impl.op == FilterOperator::Or) {
            return std::any_of(impl.children.begin(), impl.children.end(), [&](const auto &child) {
                return matchesExpression(item, child);
            });
        }
        if (impl.op == FilterOperator::Not) {
            return impl.children.empty() || !matchesExpression(item, impl.children.front());
        }
        return true;
    }

    void sortItems(std::vector<ItemSnapshot> &items, const std::vector<SortKey> &keys)
    {
        for (auto sortIt = keys.rbegin(); sortIt != keys.rend(); ++sortIt) {
            std::stable_sort(items.begin(), items.end(), [&](const auto &lhs, const auto &rhs) {
                const auto comparison = compareValues(fieldValue(lhs, sortIt->field), fieldValue(rhs, sortIt->field));
                return sortIt->direction == SortDirection::Ascending ? comparison < 0 : comparison > 0;
            });
        }
    }

    double numericValue(const Value &value)
    {
        switch (value.type()) {
            case ValueType::Int64:
                return static_cast<double>(value.asInt64());
            case ValueType::UInt64:
                return static_cast<double>(value.asUInt64());
            case ValueType::Double:
                return value.asDouble();
            default:
                return 0.0;
        }
    }

    std::string stableValueKey(const Value &value)
    {
        std::ostringstream stream;
        stream << static_cast<int>(value.type()) << ':';
        std::visit(
            [&](const auto &payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    stream << "null";
                } else if constexpr (std::is_same_v<T, bool>) {
                    stream << (payload ? "1" : "0");
                } else if constexpr (std::is_same_v<T, std::string>) {
                    stream << payload.size() << ':' << payload;
                } else if constexpr (std::is_same_v<T, ByteArray>) {
                    stream << payload.size() << ':';
                    for (auto byte : payload) {
                        stream << static_cast<unsigned int>(byte) << ',';
                    }
                } else {
                    stream << payload;
                }
            },
            value.storage());
        return stream.str();
    }

    } // namespace

View::View() : _impl(new Impl(this)) {}

View::View(SharedDataPointer<Impl> data) : _impl(std::move(data)) {}

View::~View() = default;
View::View(const View &other) = default;
View::View(View &&other) noexcept = default;
View &View::operator=(const View &other) = default;
View &View::operator=(View &&other) noexcept = default;

View View::filter(const FilterExpression &expression) const
{
    auto source = *this;
    return View(SharedDataPointer<View::Impl>(new View::Impl(nullptr, [source, expression]() {
        auto items = source.toVector();
        items.erase(std::remove_if(items.begin(),
                                   items.end(),
                                   [&](const auto &item) { return !matchesExpression(item, expression); }),
                    items.end());
        return items;
    })));
}

View View::sort(std::vector<SortKey> keys) const
{
    auto source = *this;
    return View(SharedDataPointer<View::Impl>(new View::Impl(nullptr, [source, keys = std::move(keys)]() {
        auto items = source.toVector();
        sortItems(items, keys);
        return items;
    })));
}

AggregationView View::aggregate(const AggregationSpec &spec) const
{
    auto source = *this;
    return AggregationView(SharedDataPointer<AggregationView::Impl>(new AggregationView::Impl(nullptr, [source, spec]() {
        auto items = source.toVector();
        std::map<std::string, AggregationResult> rows;

        auto updateRow = [&](AggregationResult &row, const ItemSnapshot &item) {
            Value value = spec.valueField ? fieldValue(item, *spec.valueField) : Value(static_cast<std::uint64_t>(1));
            switch (spec.kind) {
                case AggregateKind::Count:
                    row.value = Value(row.value.isNull() ? std::uint64_t {1} : row.value.asUInt64() + 1);
                    break;
                case AggregateKind::Sum: {
                    const double sum = (row.value.isNull() ? 0.0 : numericValue(row.value)) + numericValue(value);
                    row.value = Value(sum);
                    break;
                }
                case AggregateKind::Minimum:
                    if (row.value.isNull() || compareValues(value, row.value) < 0) {
                        row.value = value;
                    }
                    break;
                case AggregateKind::Maximum:
                    if (row.value.isNull() || compareValues(value, row.value) > 0) {
                        row.value = value;
                    }
                    break;
            }
        };

        if (spec.groupBy) {
            for (const auto &item : items) {
                auto groupValue = fieldValue(item, *spec.groupBy);
                auto key = stableValueKey(groupValue);
                auto &row = rows[key];
                row.groupKey = groupValue;
                updateRow(row, item);
            }
        } else {
            auto &row = rows[""];
            for (const auto &item : items) {
                updateRow(row, item);
            }
        }

        std::vector<AggregationResult> result;
        result.reserve(rows.size());
        for (auto &[key, row] : rows) {
            (void)key;
            result.push_back(std::move(row));
        }
        return result;
    })));
}

std::vector<ItemSnapshot> View::toVector() const
{
    __stdc_impl_t;
    return impl.evaluator ? impl.evaluator() : std::vector<ItemSnapshot> {};
}

void View::forEach(const std::function<void(const ItemSnapshot &)> &visitor) const
{
    for (const auto &item : toVector()) {
        visitor(item);
    }
}

std::size_t View::count() const
{
    return toVector().size();
}

bool View::empty() const
{
    return count() == 0;
}

AggregationView::AggregationView() : _impl(new Impl(this)) {}

AggregationView::AggregationView(SharedDataPointer<Impl> data) : _impl(std::move(data)) {}

AggregationView::~AggregationView() = default;
AggregationView::AggregationView(const AggregationView &other) = default;
AggregationView::AggregationView(AggregationView &&other) noexcept = default;
AggregationView &AggregationView::operator=(const AggregationView &other) = default;
AggregationView &AggregationView::operator=(AggregationView &&other) noexcept = default;

std::vector<AggregationResult> AggregationView::toVector() const
{
    __stdc_impl_t;
    return impl.evaluator ? impl.evaluator() : std::vector<AggregationResult> {};
}

void AggregationView::forEach(const std::function<void(const AggregationResult &)> &visitor) const
{
    for (const auto &row : toVector()) {
        visitor(row);
    }
}

} // namespace dini
