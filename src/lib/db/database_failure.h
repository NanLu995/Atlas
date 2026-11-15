#ifndef ATLAS_LIB_DB_DATABASE_FAILURE_H_
#define ATLAS_LIB_DB_DATABASE_FAILURE_H_

#include "db/idatabase.h"

namespace atlas {

template <typename R>
auto DatabaseFailure(const Error& error) -> R {
  if constexpr (requires(R result) {
                  result.HasValue();
                  result.Error();
                }) {
    return error;
  } else {
    R result;
    result.error = error.Message();
    if constexpr (requires { result.error_kind; }) {
      switch (error.Code()) {
        case ErrorCode::kRateLimited:
          result.error_kind = DatabaseErrorKind::kOverloaded;
          break;
        case ErrorCode::kReceiverGone:
          result.error_kind = DatabaseErrorKind::kStopping;
          break;
        case ErrorCode::kCommitUnknown:
          result.error_kind = DatabaseErrorKind::kCommitUnknown;
          break;
        default:
          break;
      }
    }
    return result;
  }
}

}  // namespace atlas

#endif  // ATLAS_LIB_DB_DATABASE_FAILURE_H_
