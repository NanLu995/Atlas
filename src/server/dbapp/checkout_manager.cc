#include "checkout_manager.h"

namespace atlas {

auto CheckoutManager::TryCheckout(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner)
    -> CheckoutResult {
  Key key{dbid, type_id};
  auto it = entries_.find(key);
  if (it != entries_.end()) {
    if (it->second.state == State::kConfirmed)
      return {CheckoutStatus::kAlreadyCheckedOut, it->second.owner};
    return {CheckoutStatus::kPendingCheckout, it->second.owner};
  }

  entries_[key] = Entry{new_owner, State::kChecking};
  return {CheckoutStatus::kSuccess, {}};
}

void CheckoutManager::ConfirmCheckout(DatabaseID dbid, uint16_t type_id,
                                      uint64_t ownership_version) {
  auto it = entries_.find({dbid, type_id});
  if (it != entries_.end()) {
    it->second.state = State::kConfirmed;
    it->second.owner.ownership_version = ownership_version;
  }
}

auto CheckoutManager::ConfirmCheckoutIfOwner(DatabaseID dbid, uint16_t type_id,
                                             const CheckoutInfo& owner, uint64_t ownership_version)
    -> bool {
  auto current = entries_.find({dbid, type_id});
  if (current == entries_.end() || current->second.state != State::kChecking) return false;
  const auto& held = current->second.owner;
  if (held.base_addr != owner.base_addr || held.app_id != owner.app_id ||
      held.entity_id != owner.entity_id || held.ownership_version != owner.ownership_version ||
      held.process_instance != owner.process_instance) {
    return false;
  }
  ConfirmCheckout(dbid, type_id, ownership_version);
  return true;
}

auto CheckoutManager::CheckinIfOwner(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& owner)
    -> bool {
  auto current = entries_.find({dbid, type_id});
  if (current == entries_.end()) return false;
  const auto& held = current->second.owner;
  if (held.base_addr != owner.base_addr || held.app_id != owner.app_id ||
      held.entity_id != owner.entity_id || held.ownership_version != owner.ownership_version ||
      held.process_instance != owner.process_instance) {
    return false;
  }
  entries_.erase(current);
  return true;
}

void CheckoutManager::ReleaseCheckout(DatabaseID dbid, uint16_t type_id) {
  entries_.erase({dbid, type_id});
}

void CheckoutManager::Checkin(DatabaseID dbid, uint16_t type_id) {
  entries_.erase({dbid, type_id});
}

auto CheckoutManager::GetOwner(DatabaseID dbid, uint16_t type_id) const
    -> std::optional<CheckoutInfo> {
  auto it = entries_.find({dbid, type_id});
  if (it == entries_.end()) return std::nullopt;
  return it->second.owner;
}

auto CheckoutManager::ClearAllFor(const Address& base_addr, uint64_t process_instance) -> int {
  int count = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.owner.base_addr == base_addr &&
        it->second.owner.process_instance == process_instance) {
      it = entries_.erase(it);
      ++count;
    } else {
      ++it;
    }
  }
  return count;
}

}  // namespace atlas
