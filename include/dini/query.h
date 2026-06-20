#ifndef DINI_QUERY_H
#define DINI_QUERY_H

#include <memory>
#include <optional>
#include <vector>

#include <dini/diniglobal.h>
#include <dini/handles.h>
#include <dini/support/shareddata.h>
#include <dini/types.h>
#include <dini/value.h>

namespace dini {

/**
 * @brief References one queryable field.
 *
 * FieldRef may refer to ID, parent relation, polymorphic variant, or an indexed
 * column. Creating a query with a non-indexed normal column must be rejected by
 * query validation.
 */
class DINI_EXPORT FieldRef {
public:
    struct Impl;

    /**
     * @brief Creates an invalid field reference.
     *
     * @pre None.
     * @post kind() returns FieldKind::Column and column() is invalid.
     */
    FieldRef();

    /**
     * @brief Creates a reference to the built-in item ID field.
     *
     * @pre None.
     * @post The returned field is queryable and sortable for every container.
     */
    static FieldRef id();

    /**
     * @brief Creates a reference to a parent or association relation field.
     *
     * @param relation Relation handle.
     * @pre relation must identify a schema-declared relation when validated.
     * @post The returned field compares relation values by ItemId or null.
     */
    static FieldRef parent(RelationHandle relation);

    /**
     * @brief Creates a reference to a polymorphic variant field.
     *
     * @param variant Variant handle used to identify the owning container.
     * @pre variant must belong to the queried container when validated.
     * @post The returned field can be used for variant filtering, sorting, and grouping.
     */
    static FieldRef variant(VariantHandle variant);

    /**
     * @brief Creates a reference to an indexed column.
     *
     * @param column Column handle.
     * @pre column must be indexed or otherwise specially queryable when validated.
     * @post The returned field compares values using the column's declared type.
     */
    static FieldRef column(ColumnHandle column);

    /**
     * @brief Destroys the field reference wrapper.
     *
     * @pre No public precondition.
     * @post The shared private field data is released when no FieldRef references it.
     */
    ~FieldRef();

    /**
     * @brief Copies a field reference with implicit shared private data.
     *
     * @param other Field reference to copy.
     * @pre other may be valid or invalid.
     * @post This field reference describes the same field as other.
     */
    FieldRef(const FieldRef &other);

    /**
     * @brief Moves a field reference.
     *
     * @param other Field reference to move from.
     * @pre other may be valid or invalid.
     * @post This field reference receives other's private data and other remains valid.
     */
    FieldRef(FieldRef &&other) noexcept;

    /**
     * @brief Copy-assigns a field reference with implicit shared private data.
     *
     * @param other Field reference to copy.
     * @pre other may be valid or invalid.
     * @post This field reference describes the same field as other.
     */
    FieldRef &operator=(const FieldRef &other);

    /**
     * @brief Move-assigns a field reference.
     *
     * @param other Field reference to move from.
     * @pre other may be valid or invalid.
     * @post This field reference receives other's private data and other remains valid.
     */
    FieldRef &operator=(FieldRef &&other) noexcept;

    /**
     * @brief Returns the category of this field reference.
     *
     * @pre None.
     * @post The returned kind determines which handle accessor is meaningful.
     */
    FieldKind kind() const noexcept;

    /**
     * @brief Returns the referenced column handle.
     *
     * @pre kind() must be FieldKind::Column.
     * @post The field reference is not modified.
     * @throws QueryError if this field is not a column reference.
     */
    ColumnHandle column() const;

    /**
     * @brief Returns the referenced relation handle.
     *
     * @pre kind() must be FieldKind::Parent.
     * @post The field reference is not modified.
     * @throws QueryError if this field is not a parent relation reference.
     */
    RelationHandle relation() const;

    /**
     * @brief Returns the referenced variant handle.
     *
     * @pre kind() must be FieldKind::Variant.
     * @post The field reference is not modified.
     * @throws QueryError if this field is not a variant reference.
     */
    VariantHandle variant() const;

private:
    SharedDataPointer<Impl> _impl;

    friend class DocumentEngine;
    friend class View;
};

/**
 * @brief Leaf comparison predicate in a query filter.
 *
 * Filter compares one FieldRef against one Value using a supported comparison
 * operator. Type compatibility and index legality are checked by the engine.
 */
class DINI_EXPORT Filter {
public:
    struct Impl;

    /**
     * @brief Creates an invalid equality filter placeholder.
     *
     * @pre None.
     * @post The filter must be replaced before query execution.
     */
    Filter();

    /**
     * @brief Creates a field comparison filter.
     *
     * @param field Queryable field.
     * @param op Comparison operator.
     * @param value Comparison value.
     * @pre field must be legal for the queried container when validated.
     * @post The filter stores the supplied comparison.
     */
    Filter(FieldRef field, ComparisonOperator op, Value value);

    /**
     * @brief Destroys the filter wrapper.
     *
     * @pre No public precondition.
     * @post The shared private filter data is released when no Filter references it.
     */
    ~Filter();

    /**
     * @brief Copies a filter with implicit shared private data.
     *
     * @param other Filter to copy.
     * @pre other may be valid or invalid.
     * @post This filter describes the same predicate as other.
     */
    Filter(const Filter &other);

    /**
     * @brief Moves a filter.
     *
     * @param other Filter to move from.
     * @pre other may be valid or invalid.
     * @post This filter receives other's private data and other remains valid.
     */
    Filter(Filter &&other) noexcept;

    /**
     * @brief Copy-assigns a filter with implicit shared private data.
     *
     * @param other Filter to copy.
     * @pre other may be valid or invalid.
     * @post This filter describes the same predicate as other.
     */
    Filter &operator=(const Filter &other);

    /**
     * @brief Move-assigns a filter.
     *
     * @param other Filter to move from.
     * @pre other may be valid or invalid.
     * @post This filter receives other's private data and other remains valid.
     */
    Filter &operator=(Filter &&other) noexcept;

    /**
     * @brief Returns the field being compared.
     *
     * @pre None.
     * @post The returned reference remains valid while this Filter is unchanged.
     */
    const FieldRef &field() const noexcept;

    /**
     * @brief Returns the comparison operator.
     *
     * @pre None.
     * @post The filter is not modified.
     */
    ComparisonOperator comparisonOperator() const noexcept;

    /**
     * @brief Returns the comparison value.
     *
     * @pre None.
     * @post The returned reference remains valid while this Filter is unchanged.
     */
    const Value &value() const noexcept;

private:
    SharedDataPointer<Impl> _impl;

    friend class DocumentEngine;
    friend class View;
};

/**
 * @brief Boolean query expression composed from filters.
 *
 * FilterExpression supports nested AND, OR, and NOT nodes. It is an immutable
 * expression value from the public API perspective and may be reused across
 * multiple compatible queries.
 */
class DINI_EXPORT FilterExpression {
public:
    struct Impl;

    /**
     * @brief Creates an empty match-all expression.
     *
     * @pre None.
     * @post isEmpty() returns true.
     */
    FilterExpression();

    /**
     * @brief Creates an expression from one leaf filter.
     *
     * @param filter Leaf predicate.
     * @pre filter must be valid before execution.
     * @post The expression represents exactly the supplied predicate.
     */
    explicit FilterExpression(Filter filter);

    /**
     * @brief Destroys the filter expression wrapper.
     *
     * @pre No public precondition.
     * @post The shared private expression data is released when no FilterExpression references it.
     */
    ~FilterExpression();

    /**
     * @brief Copies a filter expression with implicit shared private data.
     *
     * @param other Expression to copy.
     * @pre other may be valid or empty.
     * @post This expression describes the same predicate tree as other.
     */
    FilterExpression(const FilterExpression &other);

    /**
     * @brief Moves a filter expression.
     *
     * @param other Expression to move from.
     * @pre other may be valid or empty.
     * @post This expression receives other's private data and other remains valid.
     */
    FilterExpression(FilterExpression &&other) noexcept;

    /**
     * @brief Copy-assigns a filter expression with implicit shared private data.
     *
     * @param other Expression to copy.
     * @pre other may be valid or empty.
     * @post This expression describes the same predicate tree as other.
     */
    FilterExpression &operator=(const FilterExpression &other);

    /**
     * @brief Move-assigns a filter expression.
     *
     * @param other Expression to move from.
     * @pre other may be valid or empty.
     * @post This expression receives other's private data and other remains valid.
     */
    FilterExpression &operator=(FilterExpression &&other) noexcept;

    /**
     * @brief Creates an AND expression from child expressions.
     *
     * @param children Child expressions.
     * @pre children must not be empty for semantic use.
     * @post The returned expression matches only items matching every child.
     */
    static FilterExpression all(std::vector<FilterExpression> children);

    /**
     * @brief Creates an OR expression from child expressions.
     *
     * @param children Child expressions.
     * @pre children must not be empty for semantic use.
     * @post The returned expression matches items matching at least one child.
     */
    static FilterExpression any(std::vector<FilterExpression> children);

    /**
     * @brief Creates a NOT expression.
     *
     * @param child Expression to negate.
     * @pre child must be valid before execution.
     * @post The returned expression matches the complement of child.
     */
    static FilterExpression negate(FilterExpression child);

    /**
     * @brief Tests whether this expression contains no predicate.
     *
     * @pre None.
     * @post Empty expressions are interpreted as match-all by query APIs.
     */
    bool isEmpty() const noexcept;

private:
    SharedDataPointer<Impl> _impl;

    friend class DocumentEngine;
    friend class View;
    friend const Impl &filterExpressionImpl(const FilterExpression &expression);
};

/**
 * @brief Sort key used by QuerySpec.
 *
 * SortKey applies a direction to one sortable field. The engine must reject
 * sorting by non-indexed normal columns.
 */
struct DINI_EXPORT SortKey {
    FieldRef field;
    SortDirection direction = SortDirection::Ascending;
};

/**
 * @brief Complete query expression for a table or list view.
 *
 * QuerySpec captures filter and sort choices but does not execute them. Views are
 * live and evaluate this spec against the engine state at iteration time.
 */
struct DINI_EXPORT QuerySpec {
    FilterExpression filter;
    std::vector<SortKey> sortKeys;
};

/**
 * @brief Describes one aggregation over a live view.
 *
 * AggregationSpec supports count, sum, minimum, and maximum. Grouping is optional
 * and must use parent, variant, or an indexed column.
 */
struct DINI_EXPORT AggregationSpec {
    AggregateKind kind = AggregateKind::Count;
    std::optional<FieldRef> valueField;
    std::optional<FieldRef> groupBy;
};

/**
 * @brief One aggregation result row.
 *
 * The group key is nullopt for ungrouped aggregation. The value stores the
 * aggregate result using the built-in Value representation.
 */
struct DINI_EXPORT AggregationResult {
    std::optional<Value> groupKey;
    Value value;
};

} // namespace dini

#endif // DINI_QUERY_H
