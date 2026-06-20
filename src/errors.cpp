#include <dini/errors.h>

namespace dini {

DiniError::DiniError(const std::string &message) : std::runtime_error(message) {}

SchemaError::SchemaError(const std::string &message) : DiniError(message) {}

HandleError::HandleError(const std::string &message) : DiniError(message) {}

TransactionError::TransactionError(const std::string &message) : DiniError(message) {}

ConstraintError::ConstraintError(const std::string &message) : DiniError(message) {}

HookError::HookError(const std::string &message) : DiniError(message) {}

QueryError::QueryError(const std::string &message) : DiniError(message) {}

LogError::LogError(const std::string &message) : DiniError(message) {}

RecoveryError::RecoveryError(const std::string &message) : DiniError(message) {}

} // namespace dini
