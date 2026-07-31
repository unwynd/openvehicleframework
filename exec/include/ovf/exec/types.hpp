// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ovf::exec {

template <typename Tag> class Identifier final {
public:
  using Value = std::uint64_t;

  constexpr Identifier() noexcept = default;
  explicit constexpr Identifier(Value value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Value value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0U; }
  [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }

  auto operator<=>(const Identifier&) const = default;

private:
  Value value_{};
};

struct ApplicationIdTag;
struct DomainIdTag;
struct ModeIdTag;
struct ResourceIdTag;
struct TransitionIdTag;

using ApplicationId = Identifier<ApplicationIdTag>;
using ExecutionUnitId = ApplicationId;
using DomainId = Identifier<DomainIdTag>;
using ModeId = Identifier<ModeIdTag>;
using ResourceId = Identifier<ResourceIdTag>;
using TransitionId = Identifier<TransitionIdTag>;

struct ModeRef final {
  DomainId domain;
  ModeId mode;

  auto operator<=>(const ModeRef&) const = default;
};

struct ModelGeneration final {
  std::uint64_t value{};

  [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }
  auto operator<=>(const ModelGeneration&) const = default;
};

enum class ApplicationState : std::uint8_t {
  unknown,
  starting,
  ready,
  stopping,
  stopped,
  failed,
  killed,
  unavailable
};

using ExecutionUnitState = ApplicationState;

enum class DomainStatus : std::uint8_t { unknown, stable, transitioning, degraded, recovering };

enum class TransitionPhase : std::uint8_t {
  received,
  validated,
  planned,
  stopping,
  starting,
  awaiting_readiness,
  committing,
  succeeded,
  rejected,
  failed,
  cancelled,
  superseded,
  deadline_exceeded,
  recovery_failed,
  recovering
};

enum class StopReason : std::uint8_t {
  none,
  mode_change,
  system_shutdown,
  restart,
  supervisor_request,
  dependency_failure,
  recovery,
  unknown
};

enum class ReplacementPolicy : std::uint8_t { reject_while_busy, queue, supersede_if_safe };

enum class FailureAction : std::uint8_t {
  hold_observed_configuration,
  enter_fallback_mode,
  rollback_if_declared_safe,
  stop_domain,
  request_system_recovery
};

enum class ReadinessPolicy : std::uint8_t {
  lifecycle_channel,
  process_started,
  supervisor_notification,
  successful_exit,
  socket_available,
  mount_present,
  required = lifecycle_channel
};

[[nodiscard]] std::string_view ToString(ApplicationState value) noexcept;
[[nodiscard]] std::string_view ToString(DomainStatus value) noexcept;
[[nodiscard]] std::string_view ToString(TransitionPhase value) noexcept;
[[nodiscard]] std::string_view ToString(StopReason value) noexcept;

} // namespace ovf::exec

namespace std {

template <typename Tag> struct hash<ovf::exec::Identifier<Tag>> {
  size_t operator()(const ovf::exec::Identifier<Tag>& id) const noexcept {
    return hash<typename ovf::exec::Identifier<Tag>::Value>{}(id.value());
  }
};

template <> struct hash<ovf::exec::ModeRef> {
  size_t operator()(const ovf::exec::ModeRef& value) const noexcept {
    const auto first = hash<ovf::exec::DomainId>{}(value.domain);
    const auto second = hash<ovf::exec::ModeId>{}(value.mode);
    return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
  }
};

} // namespace std
