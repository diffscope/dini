#ifndef DINI_VIEW_H
#define DINI_VIEW_H

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include <dini/change.h>
#include <dini/diniglobal.h>
#include <dini/query.h>
#include <dini/support/shareddata.h>
#include <dini/types.h>

namespace dini {

class AggregationView;

/**
 * @brief Lazy live view over query results.
 *
 * View stores a container and query expression rather than a snapshot of items.
 * Iteration, count, and conversion operations evaluate against the current engine
 * state, including globally visible uncommitted transaction changes and rollbacks.
 */
class DINI_EXPORT View {
public:
    /**
     * @brief Creates an invalid empty view.
     *
     * @pre None.
     * @post empty() may return true and iteration produces no items.
     */
    View();

    /**
     * @brief Destroys the view wrapper.
     *
     * @pre No public precondition.
     * @post Shared view state is released when no wrappers remain.
     */
    ~View();

    /**
     * @brief Copies a view wrapper.
     *
     * @param other View to share.
     * @pre other may be valid or invalid.
     * @post Both views refer to the same live query state.
     */
    View(const View &other);

    /**
     * @brief Moves a view wrapper.
     *
     * @param other View to move from.
     * @pre other may be valid or invalid.
     * @post This view receives other's live query state.
     */
    View(View &&other) noexcept;

    /**
     * @brief Assigns a shared view wrapper.
     *
     * @param other View to share.
     * @pre other may be valid or invalid.
     * @post This view refers to the same live query state as other.
     */
    View &operator=(const View &other);

    /**
     * @brief Move-assigns a view wrapper.
     *
     * @param other View to move from.
     * @pre other may be valid or invalid.
     * @post This view receives other's live query state.
     */
    View &operator=(View &&other) noexcept;

    /**
     * @brief Returns a new view with an additional filter expression.
     *
     * @param expression Filter expression to combine with the existing query.
     * @pre expression must reference fields legal for this view's container.
     * @post The original view is unchanged; the returned view is still live.
     * @throws QueryError if the expression is invalid for the container.
     */
    View filter(const FilterExpression &expression) const;

    /**
     * @brief Returns a new view with the supplied sort order.
     *
     * @param keys Sort keys to apply.
     * @pre Each key must reference a sortable indexed or special field.
     * @post The original view is unchanged; the returned view is still live.
     * @throws QueryError if a sort key is invalid for the container.
     */
    View sort(std::vector<SortKey> keys) const;

    /**
     * @brief Creates a live aggregation view.
     *
     * @param spec Aggregation specification.
     * @pre spec fields must be legal for this view's container.
     * @post The returned aggregation evaluates lazily against current engine state.
     * @throws QueryError if the aggregation is invalid.
     */
    AggregationView aggregate(const AggregationSpec &spec) const;

    /**
     * @brief Materializes current view results into a vector.
     *
     * @pre The view must reference a live engine state.
     * @post The returned vector is a snapshot of the items observed during this call.
     * @throws QueryError if query execution fails.
     */
    std::vector<ItemSnapshot> toVector() const;

    /**
     * @brief Visits each current result item.
     *
     * @param visitor Callback invoked once per item in query order.
     * @pre visitor must be callable and must not invalidate the engine during iteration.
     * @post All currently matching items have been offered to visitor unless an exception was thrown.
     * @throws QueryError if query execution fails; propagates exceptions from visitor.
     */
    void forEach(const std::function<void(const ItemSnapshot &)> &visitor) const;

    /**
     * @brief Counts current view results.
     *
     * @pre The view must reference a live engine state.
     * @post The returned value reflects current engine state, not creation-time state.
     * @throws QueryError if query execution fails.
     */
    std::size_t count() const;

    /**
     * @brief Tests whether the view currently has no results.
     *
     * @pre The view must reference a live engine state.
     * @post The returned value reflects current engine state.
     * @throws QueryError if query execution fails.
     */
    bool empty() const;

private:
    struct Impl;
    SharedDataPointer<Impl> _impl;

    explicit View(SharedDataPointer<Impl> data);
    friend class DocumentEngine;
};

/**
 * @brief Lazy live view over aggregation results.
 *
 * AggregationView stores an aggregation plan and evaluates it against current
 * engine state when materialized or iterated.
 */
class DINI_EXPORT AggregationView {
public:
    /**
     * @brief Creates an invalid empty aggregation view.
     *
     * @pre None.
     * @post toVector() returns an empty result.
     */
    AggregationView();

    /**
     * @brief Destroys the aggregation view wrapper.
     *
     * @pre No public precondition.
     * @post Shared aggregation state is released when no wrappers remain.
     */
    ~AggregationView();

    /**
     * @brief Copies an aggregation view wrapper.
     *
     * @param other Aggregation view to share.
     * @pre other may be valid or invalid.
     * @post Both wrappers refer to the same live aggregation state.
     */
    AggregationView(const AggregationView &other);

    /**
     * @brief Moves an aggregation view wrapper.
     *
     * @param other Aggregation view to move from.
     * @pre other may be valid or invalid.
     * @post This wrapper receives other's aggregation state.
     */
    AggregationView(AggregationView &&other) noexcept;

    /**
     * @brief Assigns a shared aggregation view wrapper.
     *
     * @param other Aggregation view to share.
     * @pre other may be valid or invalid.
     * @post This wrapper refers to the same aggregation state as other.
     */
    AggregationView &operator=(const AggregationView &other);

    /**
     * @brief Move-assigns an aggregation view wrapper.
     *
     * @param other Aggregation view to move from.
     * @pre other may be valid or invalid.
     * @post This wrapper receives other's aggregation state.
     */
    AggregationView &operator=(AggregationView &&other) noexcept;

    /**
     * @brief Materializes current aggregation rows.
     *
     * @pre The aggregation view must reference a live engine state.
     * @post The returned vector is a snapshot of aggregation results observed during this call.
     * @throws QueryError if aggregation execution fails.
     */
    std::vector<AggregationResult> toVector() const;

    /**
     * @brief Visits each current aggregation row.
     *
     * @param visitor Callback invoked once per aggregation row.
     * @pre visitor must be callable and must not invalidate the engine during iteration.
     * @post All current aggregation rows have been offered to visitor unless an exception was thrown.
     * @throws QueryError if aggregation execution fails; propagates exceptions from visitor.
     */
    void forEach(const std::function<void(const AggregationResult &)> &visitor) const;

private:
    struct Impl;
    SharedDataPointer<Impl> _impl;

    explicit AggregationView(SharedDataPointer<Impl> data);
    friend class View;
};

} // namespace dini

#endif // DINI_VIEW_H
