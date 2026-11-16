#ifndef ATLAS_LIB_SERVER_AWAIT_MACHINED_QUERY_H_
#define ATLAS_LIB_SERVER_AWAIT_MACHINED_QUERY_H_

#include <coroutine>
#include <memory>
#include <utility>

#include "coro/cancellation.h"
#include "server/machined_client.h"

namespace atlas {

inline auto AwaitMachinedQuery(MachinedClient& client, ProcessType type,
                               CancellationToken token = {}) {
  struct State {
    MachinedClient::QueryResult result{Error{ErrorCode::kInternalError, "registry query pending"}};
    std::coroutine_handle<> continuation;
    bool suspending{true};
    bool completed{false};

    void Complete(MachinedClient::QueryResult value) {
      if (completed) return;
      result = std::move(value);
      completed = true;
      if (!suspending && continuation) continuation.resume();
    }
  };
  struct Awaiter {
    MachinedClient& client;
    ProcessType type;
    CancellationToken token;
    std::shared_ptr<State> state{std::make_shared<State>()};
    CancelRegistration cancellation{};

    ~Awaiter() { state->continuation = {}; }
    auto await_ready() const -> bool { return false; }
    auto await_suspend(std::coroutine_handle<> caller) -> bool {
      state->continuation = caller;
      cancellation = token.OnCancel([state = state] {
        state->Complete(Error{ErrorCode::kCancelled, "registry query cancelled"});
      });
      if (!state->completed)
        client.QueryAsync(type, [state = state](MachinedClient::QueryResult result) {
          state->Complete(std::move(result));
        });
      state->suspending = false;
      return !state->completed;
    }
    auto await_resume() -> MachinedClient::QueryResult { return std::move(state->result); }
  };
  return Awaiter{client, type, std::move(token)};
}

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_AWAIT_MACHINED_QUERY_H_
