// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ovf/exec/internal/engine.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace ovf::exec::detail {

struct FileJournalOptions final {
  std::string path;
  std::size_t maximum_record_size{1024U * 1024U};
  bool synchronize{true};
};

[[nodiscard]] Result<std::unique_ptr<TransitionJournal>>
OpenFileTransitionJournal(FileJournalOptions options);

} // namespace ovf::exec::detail
