// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/application.hpp"
#include "ovf/exec/error.hpp"
#include "ovf/exec/types.hpp"

#include <memory>
#include <vector>

namespace ovf::exec::detail {

struct TransitionResources final {
  std::vector<DomainId> domains;
  std::vector<ApplicationId> applications;
  std::vector<ResourceId> exclusive_resources;
};

class TransitionScheduler final {
private:
  class State;

public:
  class Lease final {
  public:
    Lease() = default;
    ~Lease();
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] TransitionId owner() const noexcept;
    void Reset() noexcept;

  private:
    friend class TransitionScheduler;
    Lease(std::shared_ptr<State> state, TransitionId owner);

    std::shared_ptr<State> state_;
    TransitionId owner_;
  };

  TransitionScheduler();
  ~TransitionScheduler();
  TransitionScheduler(TransitionScheduler&&) noexcept;
  TransitionScheduler& operator=(TransitionScheduler&&) noexcept;
  TransitionScheduler(const TransitionScheduler&) = delete;
  TransitionScheduler& operator=(const TransitionScheduler&) = delete;

  [[nodiscard]] Result<Lease> Acquire(TransitionId owner, TransitionResources resources,
                                      Deadline deadline);
  [[nodiscard]] std::size_t ActiveLeaseCount() const noexcept;

private:
  std::shared_ptr<State> state_;
};

} // namespace ovf::exec::detail
