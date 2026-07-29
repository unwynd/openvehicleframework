// SPDX-License-Identifier: Apache-2.0

#include "ovf/exec/internal/artifact_integrity.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <limits.h>
#include <string>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace ovf::exec;
using namespace ovf::exec::detail;

std::string RealPath(const char* path) {
  char resolved[PATH_MAX];
  return ::realpath(path, resolved) == nullptr ? std::string{} : std::string{resolved};
}

RuntimeArtifactPaths GeneratedArtifacts() {
  return {
      RealPath(std::getenv("OVF_TEST_EXECUTION_MODEL")),
      RealPath(std::getenv("OVF_TEST_EXECUTION_BACKEND")),
      RealPath(std::getenv("OVF_TEST_EXECUTION_SERVICES")),
      RealPath(std::getenv("OVF_TEST_EXECUTION_MANIFEST")),
  };
}

std::string BaseName(const std::string& path) {
  const auto separator = path.find_last_of('/');
  return separator == std::string::npos ? path : path.substr(separator + 1U);
}

void CopyFile(const std::string& source, const std::string& target) {
  std::ifstream input(source, std::ios::binary);
  std::ofstream output(target, std::ios::binary);
  output << input.rdbuf();
  ASSERT_TRUE(input);
  ASSERT_TRUE(output);
}

RuntimeArtifactPaths CopyArtifacts(const std::string& directory_name) {
  const auto source = GeneratedArtifacts();
  const std::string root = std::string{std::getenv("TEST_TMPDIR")} + "/" + directory_name;
  EXPECT_EQ(::mkdir(root.c_str(), 0700), 0);
  const auto model = root + "/" + BaseName(source.execution_model);
  const auto backend = root + "/" + BaseName(source.backend_configuration);
  const auto services = root + "/" + BaseName(source.services_directory);
  const auto manifest = root + "/" + BaseName(source.manifest);
  CopyFile(source.execution_model, model);
  CopyFile(source.backend_configuration, backend);
  EXPECT_EQ(::mkdir(services.c_str(), 0700), 0);
  DIR* directory = ::opendir(source.services_directory.c_str());
  EXPECT_NE(directory, nullptr);
  if (directory == nullptr) {
    return {};
  }
  while (const auto* entry = ::readdir(directory)) {
    const std::string name{entry->d_name};
    if (name != "." && name != "..") {
      CopyFile(source.services_directory + "/" + name, services + "/" + name);
    }
  }
  EXPECT_EQ(::closedir(directory), 0);
  CopyFile(source.manifest, manifest);
  return {model, backend, services, manifest};
}

TEST(ArtifactIntegrityTest, AcceptsExactGeneratedArtifactSet) {
  auto verified = VerifyRuntimeArtifacts(CopyArtifacts("valid-integrity"));
  ASSERT_TRUE(verified) << verified.error().message;
}

TEST(ArtifactIntegrityTest, RejectsModifiedArtifact) {
  const auto artifacts = CopyArtifacts("modified-integrity");
  {
    std::ofstream modified(artifacts.execution_model, std::ios::app);
    modified << ' ';
  }
  auto verified = VerifyRuntimeArtifacts(artifacts);
  ASSERT_FALSE(verified);
  EXPECT_EQ(verified.error().code, ErrorCode::configuration_error);
}

} // namespace
