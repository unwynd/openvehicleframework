// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovf/com/generated.hpp"
#include "ovf/com/runtime.hpp"
#include "ovf/com/transport_abi.h"

namespace ovf::com {

struct RouteBinding {
  struct NativeElementMapping {
    Uuid element_id;
    std::string event;
    std::string method;
  };
  Uuid service_id;
  Uuid instance_id;
  std::uint64_t route_epoch;
  std::uint64_t max_payload_size{65536};
  std::uint32_t history_depth{8};
  std::vector<NativeElementMapping> native_elements;
  std::string provider;
  std::string native_service;
  std::int32_t priority{};
};

struct ServiceRoute {
  RouteBinding binding;
  [[nodiscard]] auto provider() const noexcept -> std::string_view { return binding.provider; }
  [[nodiscard]] auto instance_id() const noexcept -> Uuid const& { return binding.instance_id; }
  [[nodiscard]] auto route_epoch() const noexcept -> std::uint64_t { return binding.route_epoch; }
};

class DiscoveryWatch final {
public:
  using Callback = std::function<void(std::span<ServiceRoute const>)>;

  ~DiscoveryWatch();
  DiscoveryWatch(DiscoveryWatch const&) = delete;
  DiscoveryWatch& operator=(DiscoveryWatch const&) = delete;

  [[nodiscard]] auto routes() const -> std::vector<ServiceRoute>;
  [[nodiscard]] auto select() const -> std::optional<ServiceRoute>;
  auto on_change(Callback callback) -> void;
  auto close() noexcept -> void;

private:
  struct Impl;
  explicit DiscoveryWatch(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
  friend auto Discover(Runtime&, std::vector<RouteBinding>) -> std::shared_ptr<DiscoveryWatch>;
};

[[nodiscard]] auto Discover(Runtime& runtime, std::vector<RouteBinding> candidates)
    -> std::shared_ptr<DiscoveryWatch>;

class ProviderClientBinding final : public ClientBinding {
public:
  ProviderClientBinding(ovf_com_transport_v1& provider, RouteBinding route) noexcept;
  ~ProviderClientBinding() override;
  ProviderClientBinding(ProviderClientBinding const&) = delete;
  ProviderClientBinding& operator=(ProviderClientBinding const&) = delete;

  auto invoke(ElementDescriptor const&, std::span<const std::byte>, CallOptions)
      -> std::shared_ptr<RawOperation> override;
  auto subscribe(ElementDescriptor const&) -> std::shared_ptr<RawSubscription> override;

private:
  ovf_com_transport_v1* provider_;
  RouteBinding route_;
};

[[nodiscard]] auto Connect(Runtime& runtime, ServiceRoute route) -> std::shared_ptr<ClientBinding>;

[[nodiscard]] auto FindService(Runtime& runtime, RouteBinding candidate,
                               std::chrono::steady_clock::duration timeout)
    -> std::shared_ptr<ClientBinding>;

class ProviderServerBinding final : public ServerBinding {
public:
  ProviderServerBinding(ovf_com_transport_v1& provider, RouteBinding route) noexcept;
  ~ProviderServerBinding() override;
  ProviderServerBinding(ProviderServerBinding const&) = delete;
  ProviderServerBinding& operator=(ProviderServerBinding const&) = delete;

  auto add_method(ElementDescriptor const&, MethodHandler) -> bool override;
  auto add_event(ElementDescriptor const&) -> bool override;
  auto publish(ElementDescriptor const&, std::span<const std::byte>)
      -> std::optional<CommunicationError> override;
  auto close() noexcept -> void override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] auto Offer(Runtime& runtime, RouteBinding route) -> std::shared_ptr<ServerBinding>;

namespace detail {
class RuntimeAccess final {
public:
  static auto find(Runtime& runtime, std::string_view name) noexcept -> ovf_com_transport_v1*;
};
} // namespace detail

} // namespace ovf::com
