// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/scheduler.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace ovf::exec::detail {
namespace {

template <typename Id> bool HasDuplicates(std::vector<Id> values) {
  std::sort(values.begin(), values.end());
  return std::adjacent_find(values.begin(), values.end()) != values.end();
}

template <typename Id>
bool Available(const std::vector<Id>& requested,
               const std::unordered_map<Id, TransitionId>& owners) {
  return std::all_of(requested.begin(), requested.end(),
                     [&](Id id) { return !owners.contains(id); });
}

template <typename Id>
void Claim(const std::vector<Id>& requested, TransitionId owner,
           std::unordered_map<Id, TransitionId>& owners) {
  for (const auto id : requested) {
    owners.emplace(id, owner);
  }
}

template <typename Id>
void ReleaseOwned(TransitionId owner, std::unordered_map<Id, TransitionId>& owners) {
  for (auto item = owners.begin(); item != owners.end();) {
    if (item->second == owner) {
      item = owners.erase(item);
    } else {
      ++item;
    }
  }
}

} // namespace

class TransitionScheduler::State final {
public:
  Result<Lease> Acquire(TransitionId owner, TransitionResources resources, Deadline deadline) {
    if (!owner) {
      return MakeError(ErrorCode::invalid_identifier, "transition identifier zero is reserved");
    }
    if (resources.domains.empty()) {
      return MakeError(ErrorCode::invalid_argument,
                       "transition must lock at least one execution domain");
    }
    if (HasDuplicates(resources.domains) || HasDuplicates(resources.units) ||
        HasDuplicates(resources.exclusive_resources)) {
      return MakeError(ErrorCode::invalid_argument,
                       "transition resource request contains duplicate identifiers");
    }
    std::sort(resources.domains.begin(), resources.domains.end());
    std::sort(resources.units.begin(), resources.units.end());
    std::sort(resources.exclusive_resources.begin(), resources.exclusive_resources.end());

    std::unique_lock lock(mutex_);
    if (leases_.contains(owner)) {
      return MakeError(ErrorCode::already_exists, "transition already owns a scheduler lease");
    }
    const auto available = [&] {
      return Available(resources.domains, domains_) && Available(resources.units, units_) &&
             Available(resources.exclusive_resources, exclusive_resources_);
    };
    if (!condition_.wait_until(lock, deadline, available)) {
      return MakeError(ErrorCode::deadline_exceeded,
                       "transition could not acquire its resources before the deadline");
    }

    Claim(resources.domains, owner, domains_);
    Claim(resources.units, owner, units_);
    Claim(resources.exclusive_resources, owner, exclusive_resources_);
    leases_.emplace(owner, std::move(resources));
    return Lease(shared_from_this(), owner);
  }

  void Release(TransitionId owner) noexcept {
    std::lock_guard lock(mutex_);
    if (leases_.erase(owner) == 0U) {
      return;
    }
    ReleaseOwned(owner, domains_);
    ReleaseOwned(owner, units_);
    ReleaseOwned(owner, exclusive_resources_);
    condition_.notify_all();
  }

  std::size_t ActiveLeaseCount() const noexcept {
    std::lock_guard lock(mutex_);
    return leases_.size();
  }

  std::shared_ptr<State> shared_from_this() { return weak_.lock(); }
  void SetSelf(const std::shared_ptr<State>& self) { weak_ = self; }

private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::unordered_map<TransitionId, TransitionResources> leases_;
  std::unordered_map<DomainId, TransitionId> domains_;
  std::unordered_map<ExecutionUnitId, TransitionId> units_;
  std::unordered_map<ResourceId, TransitionId> exclusive_resources_;
  std::weak_ptr<State> weak_;
};

TransitionScheduler::Lease::Lease(std::shared_ptr<State> state, TransitionId owner)
    : state_(std::move(state)), owner_(owner) {}

TransitionScheduler::Lease::~Lease() { Reset(); }

TransitionScheduler::Lease::Lease(Lease&& other) noexcept
    : state_(std::move(other.state_)), owner_(std::exchange(other.owner_, {})) {}

TransitionScheduler::Lease&
TransitionScheduler::Lease::operator=(TransitionScheduler::Lease&& other) noexcept {
  if (this != &other) {
    Reset();
    state_ = std::move(other.state_);
    owner_ = std::exchange(other.owner_, {});
  }
  return *this;
}

TransitionScheduler::Lease::operator bool() const noexcept {
  return state_ != nullptr && static_cast<bool>(owner_);
}

TransitionId TransitionScheduler::Lease::owner() const noexcept { return owner_; }

void TransitionScheduler::Lease::Reset() noexcept {
  if (state_ && owner_) {
    state_->Release(owner_);
  }
  state_.reset();
  owner_ = {};
}

TransitionScheduler::TransitionScheduler() : state_(std::make_shared<State>()) {
  state_->SetSelf(state_);
}

TransitionScheduler::~TransitionScheduler() = default;
TransitionScheduler::TransitionScheduler(TransitionScheduler&&) noexcept = default;
TransitionScheduler& TransitionScheduler::operator=(TransitionScheduler&&) noexcept = default;

Result<TransitionScheduler::Lease>
TransitionScheduler::Acquire(TransitionId owner, TransitionResources resources, Deadline deadline) {
  if (!state_) {
    return MakeError(ErrorCode::invalid_transition, "scheduler has been moved from");
  }
  return state_->Acquire(owner, std::move(resources), deadline);
}

std::size_t TransitionScheduler::ActiveLeaseCount() const noexcept {
  return state_ ? state_->ActiveLeaseCount() : 0U;
}

} // namespace ovf::exec::detail
