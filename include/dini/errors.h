#ifndef DINI_ERRORS_H
#define DINI_ERRORS_H

#include <stdexcept>
#include <string>

#include <dini/diniglobal.h>

namespace dini {

/**
 * @brief Base exception for all storage engine API failures.
 *
 * DiniError owns a diagnostic message and is intended to cross public API
 * boundaries. More specific subclasses identify the subsystem that rejected an
 * operation.
 */
class DINI_EXPORT DiniError : public std::runtime_error {
public:
    /**
     * @brief Creates an engine exception with a diagnostic message.
     *
     * @param message Human-readable error description.
     * @pre message should describe the violated contract.
     * @post what() returns the supplied diagnostic text.
     */
    explicit DiniError(const std::string &message);
};

/**
 * @brief Reports invalid schema definition, freezing, or schema compatibility.
 */
class DINI_EXPORT SchemaError : public DiniError {
public:
    /**
     * @brief Creates a schema error with a diagnostic message.
     *
     * @param message Human-readable schema failure description.
     * @pre message should identify the invalid schema operation.
     * @post The exception is classified as SchemaError.
     */
    explicit SchemaError(const std::string &message);
};

/**
 * @brief Reports invalid, stale, cross-schema, or cross-container handle use.
 */
class DINI_EXPORT HandleError : public DiniError {
public:
    /**
     * @brief Creates a handle validation error.
     *
     * @param message Human-readable handle failure description.
     * @pre message should identify the invalid handle or expected owner.
     * @post The exception is classified as HandleError.
     */
    explicit HandleError(const std::string &message);
};

/**
 * @brief Reports transaction lifecycle and single-writer contract violations.
 */
class DINI_EXPORT TransactionError : public DiniError {
public:
    /**
     * @brief Creates a transaction error.
     *
     * @param message Human-readable transaction failure description.
     * @pre message should identify the violated transaction contract.
     * @post The exception is classified as TransactionError.
     */
    explicit TransactionError(const std::string &message);
};

/**
 * @brief Reports schema constraint, type, uniqueness, or composition violations.
 */
class DINI_EXPORT ConstraintError : public DiniError {
public:
    /**
     * @brief Creates a constraint error.
     *
     * @param message Human-readable constraint failure description.
     * @pre message should identify the failed constraint.
     * @post The exception is classified as ConstraintError.
     */
    explicit ConstraintError(const std::string &message);
};

/**
 * @brief Reports hook callback failures or forbidden hook-time mutations.
 */
class DINI_EXPORT HookError : public DiniError {
public:
    /**
     * @brief Creates a hook error.
     *
     * @param message Human-readable hook failure description.
     * @pre message should identify the hook stage or callback failure.
     * @post The exception is classified as HookError.
     */
    explicit HookError(const std::string &message);
};

/**
 * @brief Reports invalid query expressions, non-indexed fields, or view failures.
 */
class DINI_EXPORT QueryError : public DiniError {
public:
    /**
     * @brief Creates a query error.
     *
     * @param message Human-readable query failure description.
     * @pre message should identify the invalid query contract.
     * @post The exception is classified as QueryError.
     */
    explicit QueryError(const std::string &message);
};

/**
 * @brief Reports change set serialization or deserialization failures.
 */
class DINI_EXPORT LogError : public DiniError {
public:
    /**
     * @brief Creates a serialization error.
     *
     * @param message Human-readable serialization failure description.
     * @pre message should identify the invalid change set operation or bytes.
     * @post The exception is classified as LogError.
     */
    explicit LogError(const std::string &message);
};

/**
 * @brief Reports snapshot restoration and recovery contract violations.
 */
class DINI_EXPORT RecoveryError : public DiniError {
public:
    /**
     * @brief Creates a recovery error.
     *
     * @param message Human-readable recovery failure description.
     * @pre message should identify the incompatible or corrupt recovery input.
     * @post The exception is classified as RecoveryError.
     */
    explicit RecoveryError(const std::string &message);
};

} // namespace dini

#endif // DINI_ERRORS_H
