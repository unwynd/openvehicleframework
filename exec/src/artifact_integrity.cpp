// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/artifact_integrity.hpp"

#include <json/json.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

namespace ovf::exec::detail {
namespace {

constexpr std::uintmax_t kMaximumArtifactSize = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumServiceFiles = 4096U;

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};

class Sha256 final {
public:
  void Update(std::string_view input) noexcept {
    for (const unsigned char byte : input) {
      buffer_[buffer_size_++] = byte;
      bit_count_ += 8U;
      if (buffer_size_ == buffer_.size()) {
        Transform();
        buffer_size_ = 0U;
      }
    }
  }

  std::string Finish() noexcept {
    const auto message_bits = bit_count_;
    buffer_[buffer_size_++] = 0x80U;
    if (buffer_size_ > 56U) {
      std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_), buffer_.end(), 0U);
      Transform();
      buffer_size_ = 0U;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_), buffer_.begin() + 56,
              0U);
    for (std::size_t index = 0; index < 8U; ++index) {
      buffer_[63U - index] =
          static_cast<std::uint8_t>(message_bits >> static_cast<unsigned>(index * 8U));
    }
    Transform();
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const auto word : state_) {
      encoded << std::setw(8) << word;
    }
    return encoded.str();
  }

private:
  void Transform() noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16U; ++index) {
      const auto offset = index * 4U;
      schedule[index] = (static_cast<std::uint32_t>(buffer_[offset]) << 24U) |
                        (static_cast<std::uint32_t>(buffer_[offset + 1U]) << 16U) |
                        (static_cast<std::uint32_t>(buffer_[offset + 2U]) << 8U) |
                        static_cast<std::uint32_t>(buffer_[offset + 3U]);
    }
    for (std::size_t index = 16U; index < schedule.size(); ++index) {
      const auto s0 = std::rotr(schedule[index - 15U], 7) ^ std::rotr(schedule[index - 15U], 18) ^
                      (schedule[index - 15U] >> 3U);
      const auto s1 = std::rotr(schedule[index - 2U], 17) ^ std::rotr(schedule[index - 2U], 19) ^
                      (schedule[index - 2U] >> 10U);
      schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
    }
    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t index = 0; index < schedule.size(); ++index) {
      const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto choice = (e & f) ^ (~e & g);
      const auto first = h + s1 + choice + kRoundConstants[index] + schedule[index];
      const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto second = s0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + first;
      d = c;
      c = b;
      b = a;
      a = first + second;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_size_{};
  std::uint64_t bit_count_{};
};

Result<std::string> ReadArtifact(const std::string& path) {
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
    return MakeError(ErrorCode::configuration_error,
                     "runtime artifact is not a regular file: " + path);
  }
  if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > kMaximumArtifactSize ||
      static_cast<std::uintmax_t>(status.st_size) >
          static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
    return MakeError(ErrorCode::resource_exhausted, "runtime artifact exceeds size limit");
  }
  std::ifstream input(path, std::ios::binary);
  std::string content(static_cast<std::size_t>(status.st_size), '\0');
  if (!content.empty()) {
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
  }
  if (!input) {
    return MakeError(ErrorCode::configuration_error, "runtime artifact cannot be read");
  }
  return content;
}

std::string_view BaseName(std::string_view path) noexcept {
  const auto separator = path.find_last_of('/');
  return separator == std::string_view::npos ? path : path.substr(separator + 1U);
}

Result<Json::Value> ParseManifest(std::string_view content) {
  Json::CharReaderBuilder builder;
  builder["allowComments"] = false;
  builder["allowTrailingCommas"] = false;
  builder["rejectDupKeys"] = true;
  builder["strictRoot"] = true;
  std::istringstream input(std::string{content});
  Json::Value value;
  std::string errors;
  if (!Json::parseFromStream(builder, input, &value, &errors) || !value.isObject()) {
    return MakeError(ErrorCode::configuration_error, "runtime artifact manifest is invalid");
  }
  return value;
}

std::string Digest(std::string_view content) {
  Sha256 sha;
  sha.Update(content);
  return sha.Finish();
}

bool Fingerprint(const Json::Value& value) {
  if (!value.isString() || value.asString().size() != 64U) {
    return false;
  }
  const auto text = value.asString();
  return std::all_of(text.begin(), text.end(), [](unsigned char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
  });
}

Result<void> VerifyFile(std::string_view content, const Json::Value& expected,
                        std::string_view description) {
  if (!Fingerprint(expected) || Digest(content) != expected.asString()) {
    return MakeError(ErrorCode::configuration_error,
                     std::string{description} + " failed integrity verification");
  }
  return {};
}

} // namespace

Result<void> VerifyRuntimeArtifacts(const RuntimeArtifactPaths& paths) {
  try {
    auto model = ReadArtifact(paths.execution_model);
    auto backend = ReadArtifact(paths.backend_configuration);
    auto manifest_content = ReadArtifact(paths.manifest);
    if (!model || !backend || !manifest_content) {
      return !model     ? Result<void>{model.error()}
             : !backend ? Result<void>{backend.error()}
                        : Result<void>{manifest_content.error()};
    }
    auto manifest = ParseManifest(manifest_content.value());
    if (!manifest || !manifest.value()["artifactVersion"].isIntegral() ||
        manifest.value()["artifactVersion"].asUInt64() != 2U ||
        !manifest.value()["applicationArtifacts"].isArray() ||
        !manifest.value()["backendConfiguration"].isString() ||
        !manifest.value()["servicesDirectory"].isString() ||
        manifest.value()["backendConfiguration"].asString() !=
            BaseName(paths.backend_configuration) ||
        manifest.value()["servicesDirectory"].asString() != BaseName(paths.services_directory) ||
        !manifest.value()["serviceFingerprints"].isObject()) {
      return MakeError(ErrorCode::configuration_error,
                       "runtime artifact manifest does not match deployment paths");
    }
    for (const auto& application : manifest.value()["applicationArtifacts"]) {
      if (!application.isObject() || !application["name"].isString() ||
          application["name"].asString().empty() || !application["bazelTarget"].isString() ||
          !application["installPath"].isString() || application["installPath"].asString().empty() ||
          application["installPath"].asString().front() == '/' ||
          !Fingerprint(application["sha256"])) {
        return MakeError(ErrorCode::configuration_error,
                         "runtime artifact manifest has an invalid application binding");
      }
    }
    auto verified_model = VerifyFile(
        model.value(), manifest.value()["executionModelArtifactFingerprint"], "execution model");
    auto verified_backend =
        VerifyFile(backend.value(), manifest.value()["backendConfigurationFingerprint"],
                   "backend configuration");
    if (!verified_model || !verified_backend) {
      return !verified_model ? verified_model : verified_backend;
    }
    struct stat directory_status{};
    if (::lstat(paths.services_directory.c_str(), &directory_status) != 0 ||
        !S_ISDIR(directory_status.st_mode)) {
      return MakeError(ErrorCode::configuration_error, "generated service directory is invalid");
    }
    std::vector<std::string> observed;
    struct Directory final {
      explicit Directory(DIR* value) : value(value) {}
      ~Directory() {
        if (value != nullptr) {
          ::closedir(value);
        }
      }
      DIR* value;
    } directory{::opendir(paths.services_directory.c_str())};
    if (directory.value == nullptr) {
      return MakeError(ErrorCode::configuration_error,
                       "generated service directory cannot be opened");
    }
    while (const auto* entry = ::readdir(directory.value)) {
      const std::string name{entry->d_name};
      if (name == "." || name == "..") {
        continue;
      }
      if (observed.size() >= kMaximumServiceFiles || name.find('/') != std::string::npos) {
        return MakeError(ErrorCode::configuration_error,
                         "generated service directory contains an invalid entry");
      }
      const auto& expected = manifest.value()["serviceFingerprints"][name];
      auto content = ReadArtifact(paths.services_directory + "/" + name);
      if (!content) {
        return content.error();
      }
      auto verified = VerifyFile(content.value(), expected, "service description");
      if (!verified) {
        return verified;
      }
      observed.push_back(name);
    }
    auto expected = manifest.value()["serviceFingerprints"].getMemberNames();
    std::sort(observed.begin(), observed.end());
    std::sort(expected.begin(), expected.end());
    if (observed != expected) {
      return MakeError(ErrorCode::configuration_error, "generated service directory is incomplete");
    }
    return {};
  } catch (...) {
    return MakeError(ErrorCode::resource_exhausted,
                     "runtime artifact verification exhausted resources");
  }
}

} // namespace ovf::exec::detail
