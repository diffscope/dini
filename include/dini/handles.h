#ifndef DINI_HANDLES_H
#define DINI_HANDLES_H

#include <string>

#include <dini/diniglobal.h>
#include <dini/support/shareddata.h>
#include <dini/types.h>

namespace dini {

/**
 * @brief Lightweight handle for an unordered entity table definition.
 *
 * TableHandle is copyable and may be stored by application code. It identifies a
 * table only within the schema that created it; using it with another schema or
 * engine instance must be rejected by validation.
 */
class DINI_EXPORT TableHandle {
public:
    /**
     * @brief Creates an invalid table handle.
     *
     * @pre None.
     * @post isValid() returns false.
     */
    TableHandle() noexcept;

    /**
     * @brief Creates a table handle from stable schema and container identifiers.
     *
     * @param schemaId Owning schema identity.
     * @param containerId Stable table identity within the schema.
     * @param debugName Optional diagnostic name.
     * @pre containerId must refer to a table in schemaId when validated.
     * @post isValid() returns true if schemaId and containerId are non-zero.
     */
    TableHandle(SchemaId schemaId, ContainerId containerId, std::string debugName = {});

    /**
     * @brief Destroys the table handle wrapper.
     *
     * @pre No public precondition.
     * @post The shared private handle data is released when no handle references it.
     */
    ~TableHandle();

    /**
     * @brief Copies a table handle with implicit shared private data.
     *
     * @param other Handle to copy.
     * @pre other may be valid or invalid.
     * @post This handle identifies the same table handle value as other.
     */
    TableHandle(const TableHandle &other);

    /**
     * @brief Moves a table handle.
     *
     * @param other Handle to move from.
     * @pre other may be valid or invalid.
     * @post This handle receives other's private data and other remains valid.
     */
    TableHandle(TableHandle &&other) noexcept;

    /**
     * @brief Copy-assigns a table handle with implicit shared private data.
     *
     * @param other Handle to copy.
     * @pre other may be valid or invalid.
     * @post This handle identifies the same table handle value as other.
     */
    TableHandle &operator=(const TableHandle &other);

    /**
     * @brief Move-assigns a table handle.
     *
     * @param other Handle to move from.
     * @pre other may be valid or invalid.
     * @post This handle receives other's private data and other remains valid.
     */
    TableHandle &operator=(TableHandle &&other) noexcept;

    /**
     * @brief Tests whether the handle contains a non-empty identity.
     *
     * @pre None.
     * @post Does not validate existence in an EngineSchema.
     */
    bool isValid() const noexcept;

    /**
     * @brief Returns the owning schema identity.
     *
     * @pre isValid() should be true for semantic use.
     * @post The handle is not modified.
     */
    SchemaId schemaId() const noexcept;

    /**
     * @brief Returns the stable table identity within the schema.
     *
     * @pre isValid() should be true for semantic use.
     * @post The handle is not modified.
     */
    ContainerId containerId() const noexcept;

    /**
     * @brief Returns the optional diagnostic name.
     *
     * @pre None.
     * @post The returned name must not be used as a runtime identifier.
     */
    const std::string &debugName() const noexcept;

    /**
     * @brief Compares two table handles by schema, container, and diagnostic identity.
     *
     * @pre None.
     * @post Returns true when both handles identify the same table handle value.
     */
    DINI_EXPORT friend bool operator==(const TableHandle &lhs, const TableHandle &rhs);

private:
    struct Impl;
    SharedDataPointer<Impl> _impl;
};

/**
 * @brief Lightweight handle for an association-grouped ordered list definition.
 *
 * ListHandle identifies a first-class list container. List order is managed by
 * the engine per association value and is never exposed as a writable column.
 */
class DINI_EXPORT ListHandle {
public:
    /**
     * @brief Creates an invalid list handle.
     *
     * @pre None.
     * @post isValid() returns false.
     */
    ListHandle() noexcept;

    /**
     * @brief Creates a list handle from stable schema and container identifiers.
     *
     * @param schemaId Owning schema identity.
     * @param containerId Stable list identity within the schema.
     * @param debugName Optional diagnostic name.
     * @pre containerId must refer to a list in schemaId when validated.
     * @post isValid() returns true if schemaId and containerId are non-zero.
     */
    ListHandle(SchemaId schemaId, ContainerId containerId, std::string debugName = {});

    /**
     * @brief Destroys the list handle wrapper.
     *
     * @pre No public precondition.
     * @post The shared private handle data is released when no handle references it.
     */
    ~ListHandle();

    /**
     * @brief Copies a list handle with implicit shared private data.
     *
     * @param other Handle to copy.
     * @pre other may be valid or invalid.
     * @post This handle identifies the same list handle value as other.
     */
    ListHandle(const ListHandle &other);

    /**
     * @brief Moves a list handle.
     *
     * @param other Handle to move from.
     * @pre other may be valid or invalid.
     * @post This handle receives other's private data and other remains valid.
     */
    ListHandle(ListHandle &&other) noexcept;

    /**
     * @brief Copy-assigns a list handle with implicit shared private data.
     *
     * @param other Handle to copy.
     * @pre other may be valid or invalid.
     * @post This handle identifies the same list handle value as other.
     */
    ListHandle &operator=(const ListHandle &other);

    /**
     * @brief Move-assigns a list handle.
     *
     * @param other Handle to move from.
     * @pre other may be valid or invalid.
     * @post This handle receives other's private data and other remains valid.
     */
    ListHandle &operator=(ListHandle &&other) noexcept;

    /**
     * @brief Tests whether the handle contains a non-empty identity.
     *
     * @pre None.
     * @post Does not validate existence in an EngineSchema.
     */
    bool isValid() const noexcept;

    /**
     * @brief Returns the owning schema identity.
     *
     * @pre isValid() should be true for semantic use.
     * @post The handle is not modified.
     */
    SchemaId schemaId() const noexcept;

    /**
     * @brief Returns the stable list identity within the schema.
     *
     * @pre isValid() should be true for semantic use.
     * @post The handle is not modified.
     */
    ContainerId containerId() const noexcept;

    /**
     * @brief Returns the optional diagnostic name.
     *
     * @pre None.
     * @post The returned name must not be used as a runtime identifier.
     */
    const std::string &debugName() const noexcept;

    /**
     * @brief Compares two list handles by schema, container, and diagnostic identity.
     *
     * @pre None.
     * @post Returns true when both handles identify the same list handle value.
     */
    DINI_EXPORT friend bool operator==(const ListHandle &lhs, const ListHandle &rhs);

private:
    struct Impl;
    SharedDataPointer<Impl> _impl;
};

/**
 * @brief Lightweight handle for a column-like field definition.
 *
 * ColumnHandle may identify normal, computed, variant-specific, association, or
 * polymorphic type columns. The owning container must be validated before use.
 */
class DINI_EXPORT ColumnHandle {
public:
    /**
     * @brief Creates an invalid column handle.
     *
     * @pre None.
     * @post isValid() returns false.
     */
    ColumnHandle() noexcept;

    /**
     * @brief Creates a column handle from stable schema, container, and column identifiers.
     *
     * @param schemaId Owning schema identity.
     * @param containerId Owning table or list identity.
     * @param columnId Stable column identity.
     * @param debugName Optional diagnostic name.
     * @pre columnId must belong to containerId in schemaId when validated.
     * @post isValid() returns true if all identifiers are non-zero.
     */
    ColumnHandle(SchemaId schemaId, ContainerId containerId, ColumnId columnId, std::string debugName = {});

    /**
     * @brief Destroys the column handle wrapper.
     *
     * @pre No public precondition.
     * @post The shared private handle data is released when no handle references it.
     */
    ~ColumnHandle();

    /**
     * @brief Copies a column handle with implicit shared private data.
     *
     * @param other Handle to copy.
     * @pre other may be valid or invalid.
     * @post This handle identifies the same column handle value as other.
     */
    ColumnHandle(const ColumnHandle &other);

    /**
     * @brief Moves a column handle.
     *
     * @param other Handle to move from.
     * @pre other may be valid or invalid.
     * @post This handle receives other's private data and other remains valid.
     */
    ColumnHandle(ColumnHandle &&other) noexcept;

    /**
     * @brief Copy-assigns a column handle with implicit shared private data.
     *
     * @param other Handle to copy.
     * @pre other may be valid or invalid.
     * @post This handle identifies the same column handle value as other.
     */
    ColumnHandle &operator=(const ColumnHandle &other);

    /**
     * @brief Move-assigns a column handle.
     *
     * @param other Handle to move from.
     * @pre other may be valid or invalid.
     * @post This handle receives other's private data and other remains valid.
     */
    ColumnHandle &operator=(ColumnHandle &&other) noexcept;

    /**
     * @brief Tests whether the handle contains a non-empty identity.
     *
     * @pre None.
     * @post Does not validate existence in an EngineSchema.
     */
    bool isValid() const noexcept;

    /**
     * @brief Returns the owning schema identity.
     *
     * @pre isValid() should be true for semantic use.
     * @post The handle is not modified.
     */
    SchemaId schemaId() const noexcept;

    /**
     * @brief Returns the owning container identity.
     *
     * @pre isValid() should be true for semantic use.
     * @post The handle is not modified.
     */
    ContainerId containerId() const noexcept;

    /**
     * @brief Returns the stable column identity.
     *
     * @pre isValid() should be true for semantic use.
     * @post The handle is not modified.
     */
    ColumnId columnId() const noexcept;

    /**
     * @brief Returns the optional diagnostic name.
     *
     * @pre None.
     * @post The returned name must not be used as a runtime identifier.
     */
    const std::string &debugName() const noexcept;

    /**
     * @brief Compares two column handles by schema, container, column, and diagnostic identity.
     *
     * @pre None.
     * @post Returns true when both handles identify the same column handle value.
     */
    DINI_EXPORT friend bool operator==(const ColumnHandle &lhs, const ColumnHandle &rhs);

private:
    struct Impl;
    SharedDataPointer<Impl> _impl;
};

/**
 * @brief Lightweight handle for a schema-declared association relation.
 *
 * RelationHandle identifies composition and list association columns at the
 * schema level. Runtime parent and list membership updates are still expressed as
 * column value changes.
 */
class DINI_EXPORT RelationHandle {
public:
    /**
     * @brief Creates an invalid relation handle.
     *
     * @pre None.
     * @post isValid() returns false.
     */
    RelationHandle() noexcept;

    /**
     * @brief Creates a relation handle from stable identifiers.
     *
     * @param schemaId Owning schema identity.
     * @param containerId Owning table or list identity.
     * @param relationId Stable relation identity.
     * @param column Column handle that stores the relation value.
     * @param debugName Optional diagnostic name.
     * @pre relationId and column must belong to containerId in schemaId when validated.
     * @post isValid() returns true if all identifiers are non-zero and column is valid.
     */
    RelationHandle(SchemaId schemaId, ContainerId containerId, RelationId relationId, ColumnHandle column, std::string debugName = {});

    /**
     * @brief Destroys the relation handle wrapper.
     *
     * @pre No public precondition.
     * @post The shared private handle data is released when no handle references it.
     */
    ~RelationHandle();

    /**
     * @brief Copies a relation handle with implicit shared private data.
     *
     * @param other Handle to copy.
     * @pre other may be valid or invalid.
     * @post This handle identifies the same relation handle value as other.
     */
    RelationHandle(const RelationHandle &other);

    /**
     * @brief Moves a relation handle.
     *
     * @param other Handle to move from.
     * @pre other may be valid or invalid.
     * @post This handle receives other's private data and other remains valid.
     */
    RelationHandle(RelationHandle &&other) noexcept;

    /**
     * @brief Copy-assigns a relation handle with implicit shared private data.
     *
     * @param other Handle to copy.
     * @pre other may be valid or invalid.
     * @post This handle identifies the same relation handle value as other.
     */
    RelationHandle &operator=(const RelationHandle &other);

    /**
     * @brief Move-assigns a relation handle.
     *
     * @param other Handle to move from.
     * @pre other may be valid or invalid.
     * @post This handle receives other's private data and other remains valid.
     */
    RelationHandle &operator=(RelationHandle &&other) noexcept;

    /**
     * @brief Tests whether the handle contains a non-empty identity.
     *
     * @pre None.
     * @post Does not validate existence in an EngineSchema.
     */
    bool isValid() const noexcept;

    /**
     * @brief Returns the owning schema identity.
     *
     * @pre isValid() should be true for semantic use.
     * @post The handle is not modified.
     */
    SchemaId schemaId() const noexcept;

    /**
     * @brief Returns the owning container identity.
     *
     * @pre isValid() should be true for semantic use.
     * @post The handle is not modified.
     */
    ContainerId containerId() const noexcept;

    /**
     * @brief Returns the stable relation identity.
     *
     * @pre isValid() should be true for semantic use.
     * @post The handle is not modified.
     */
    RelationId relationId() const noexcept;

    /**
     * @brief Returns the column handle that stores this relation value.
     *
     * @pre isValid() should be true for semantic use.
     * @post The returned handle has the same schema and container identity.
     */
    ColumnHandle column() const noexcept;

    /**
     * @brief Returns the optional diagnostic name.
     *
     * @pre None.
     * @post The returned name must not be used as a runtime identifier.
     */
    const std::string &debugName() const noexcept;

    /**
     * @brief Compares two relation handles by schema, container, relation, column, and diagnostic identity.
     *
     * @pre None.
     * @post Returns true when both handles identify the same relation handle value.
     */
    DINI_EXPORT friend bool operator==(const RelationHandle &lhs, const RelationHandle &rhs);

private:
    struct Impl;
    SharedDataPointer<Impl> _impl;
};

/**
 * @brief Lightweight handle for one polymorphic variant of a container.
 *
 * VariantHandle is assigned by schema definition and is used when inserting
 * polymorphic items, reading variant fields, and querying by variant.
 */
class DINI_EXPORT VariantHandle {
public:
    /**
     * @brief Creates an invalid variant handle.
     *
     * @pre None.
     * @post isValid() returns false.
     */
    VariantHandle() noexcept;

    /**
     * @brief Creates a variant handle from stable identifiers.
     *
     * @param schemaId Owning schema identity.
     * @param containerId Owning table or list identity.
     * @param variantId Stable variant identity.
     * @param debugName Optional diagnostic name.
     * @pre variantId must belong to containerId in schemaId when validated.
     * @post isValid() returns true if all identifiers are non-zero.
     */
    VariantHandle(SchemaId schemaId, ContainerId containerId, VariantId variantId, std::string debugName = {});

    /**
     * @brief Destroys the variant handle wrapper.
     *
     * @pre No public precondition.
     * @post The shared private handle data is released when no handle references it.
     */
    ~VariantHandle();

    /**
     * @brief Copies a variant handle with implicit shared private data.
     *
     * @param other Handle to copy.
     * @pre other may be valid or invalid.
     * @post This handle identifies the same variant handle value as other.
     */
    VariantHandle(const VariantHandle &other);

    /**
     * @brief Moves a variant handle.
     *
     * @param other Handle to move from.
     * @pre other may be valid or invalid.
     * @post This handle receives other's private data and other remains valid.
     */
    VariantHandle(VariantHandle &&other) noexcept;

    /**
     * @brief Copy-assigns a variant handle with implicit shared private data.
     *
     * @param other Handle to copy.
     * @pre other may be valid or invalid.
     * @post This handle identifies the same variant handle value as other.
     */
    VariantHandle &operator=(const VariantHandle &other);

    /**
     * @brief Move-assigns a variant handle.
     *
     * @param other Handle to move from.
     * @pre other may be valid or invalid.
     * @post This handle receives other's private data and other remains valid.
     */
    VariantHandle &operator=(VariantHandle &&other) noexcept;

    /**
     * @brief Tests whether the handle contains a non-empty identity.
     *
     * @pre None.
     * @post Does not validate existence in an EngineSchema.
     */
    bool isValid() const noexcept;

    /**
     * @brief Returns the owning schema identity.
     *
     * @pre isValid() should be true for semantic use.
     * @post The handle is not modified.
     */
    SchemaId schemaId() const noexcept;

    /**
     * @brief Returns the owning container identity.
     *
     * @pre isValid() should be true for semantic use.
     * @post The handle is not modified.
     */
    ContainerId containerId() const noexcept;

    /**
     * @brief Returns the stable variant identity.
     *
     * @pre isValid() should be true for semantic use.
     * @post The handle is not modified.
     */
    VariantId variantId() const noexcept;

    /**
     * @brief Returns the optional diagnostic name.
     *
     * @pre None.
     * @post The returned name must not be used as a runtime identifier.
     */
    const std::string &debugName() const noexcept;

    /**
     * @brief Compares two variant handles by schema, container, variant, and diagnostic identity.
     *
     * @pre None.
     * @post Returns true when both handles identify the same variant handle value.
     */
    DINI_EXPORT friend bool operator==(const VariantHandle &lhs, const VariantHandle &rhs);

private:
    struct Impl;
    SharedDataPointer<Impl> _impl;
};

} // namespace dini

#endif // DINI_HANDLES_H
