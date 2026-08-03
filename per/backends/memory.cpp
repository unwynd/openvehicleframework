// SPDX-License-Identifier: Apache-2.0

#include "ovf/per/backend_abi.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
using Values = std::map<Bytes, Bytes>;

struct StoreData final {
  Values values;
  Values blobs;
  std::uint64_t generation{};
  std::uint64_t capacity{};
  std::uint32_t max_entries{};
  std::uint32_t max_key_size{};
  std::uint32_t max_value_size{};
  std::uint64_t max_blob_size{};
  bool writer_active{};
};

struct BlobOperation final {
  std::shared_ptr<StoreData> store;
  Bytes key;
  Bytes value;
  std::uint64_t generation{};
  std::uint64_t written{};
  ovf_per_durability_v1 durability{};
  bool write{};
  bool finished{};
};

struct StoreHandle final {
  std::shared_ptr<StoreData> store;
  ovf_per_access_v1 access{};
};

struct Transaction final {
  std::shared_ptr<StoreData> store;
  Values values;
  std::uint64_t base_generation{};
  ovf_per_durability_v1 durability{};
  bool write{};
  bool finished{};
};

struct MemoryBackend final {
  ovf_per_backend_v1 abi{};
  std::mutex mutex;
  std::unordered_map<std::string, std::shared_ptr<StoreData>> stores;
  std::unordered_map<ovf_per_handle_v1, StoreHandle> store_handles;
  std::unordered_map<ovf_per_handle_v1, Transaction> transactions;
  std::unordered_map<ovf_per_handle_v1, BlobOperation> blobs;
  ovf_per_handle_v1 next_handle{1};
  std::uint32_t max_stores{};
  std::uint32_t max_transactions{};
  bool running{};
  std::string error;
};

MemoryBackend* Self(ovf_per_backend_v1* backend) noexcept {
  return static_cast<MemoryBackend*>(backend->implementation);
}

void SetError(MemoryBackend& backend, std::string value) { backend.error = std::move(value); }

bool Valid(ovf_per_bytes_view_v1 value) noexcept {
  return value.data != nullptr || value.size == 0;
}

Bytes Copy(ovf_per_bytes_view_v1 value) {
  return value.size == 0 ? Bytes{} : Bytes(value.data, value.data + value.size);
}

std::uint64_t Size(const Values& values) noexcept {
  std::uint64_t total{};
  for (const auto& [key, value] : values) {
    total += key.size() + value.size();
  }
  return total;
}

ovf_per_status_v1 Start(ovf_per_backend_v1* backend) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  self.running = true;
  return OVF_PER_STATUS_OK;
}

void Stop(ovf_per_backend_v1* backend) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  self.running = false;
  for (auto& [handle, transaction] : self.transactions) {
    static_cast<void>(handle);
    if (transaction.write && !transaction.finished) {
      transaction.store->writer_active = false;
    }
  }
  self.transactions.clear();
  for (auto& [handle, blob] : self.blobs) {
    static_cast<void>(handle);
    if (blob.write && !blob.finished) {
      blob.store->writer_active = false;
    }
  }
  self.blobs.clear();
  self.store_handles.clear();
}

ovf_per_status_v1 Capabilities(ovf_per_backend_v1* backend, ovf_per_capabilities_v1* output) {
  if (output == nullptr || output->struct_size < sizeof(*output)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  const auto& self = *Self(backend);
  *output = {sizeof(*output),
             self.max_stores,
             self.max_transactions,
             UINT64_C(64) * 1024U * 1024U,
             4096,
             16U * 1024U * 1024U,
             UINT64_C(64) * 1024U * 1024U,
             OVF_PER_DURABILITY_BUFFERED,
             0,
             0,
             {}};
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 OpenStore(ovf_per_backend_v1* backend,
                            const ovf_per_store_descriptor_v1* descriptor,
                            ovf_per_handle_v1* output) {
  auto& self = *Self(backend);
  if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor) || output == nullptr ||
      descriptor->logical_name.data == nullptr || descriptor->logical_name.size == 0 ||
      descriptor->capacity_bytes == 0 || descriptor->max_entries == 0 ||
      descriptor->max_key_size == 0 || descriptor->max_value_size == 0 ||
      descriptor->max_blob_size == 0) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  if (!self.running) {
    return OVF_PER_STATUS_SHUTTING_DOWN;
  }
  if (descriptor->minimum_durability > OVF_PER_DURABILITY_BUFFERED) {
    SetError(self, "memory provider cannot satisfy requested durability");
    return OVF_PER_STATUS_UNSUPPORTED;
  }
  const std::string name(descriptor->logical_name.data, descriptor->logical_name.size);
  auto iterator = self.stores.find(name);
  if (iterator == self.stores.end()) {
    if (self.stores.size() >= self.max_stores) {
      SetError(self, "store limit reached");
      return OVF_PER_STATUS_RESOURCE_EXHAUSTED;
    }
    auto store = std::make_shared<StoreData>();
    store->capacity = descriptor->capacity_bytes;
    store->max_entries = descriptor->max_entries;
    store->max_key_size = descriptor->max_key_size;
    store->max_value_size = descriptor->max_value_size;
    store->max_blob_size = descriptor->max_blob_size;
    iterator = self.stores.emplace(name, std::move(store)).first;
  } else if (iterator->second->capacity != descriptor->capacity_bytes ||
             iterator->second->max_entries != descriptor->max_entries ||
             iterator->second->max_key_size != descriptor->max_key_size ||
             iterator->second->max_value_size != descriptor->max_value_size ||
             iterator->second->max_blob_size != descriptor->max_blob_size) {
    SetError(self, "store was reopened with incompatible limits");
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  const auto handle = self.next_handle++;
  self.store_handles.emplace(handle, StoreHandle{iterator->second, descriptor->access});
  *output = handle;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 CloseStore(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  return self.store_handles.erase(handle) == 1 ? OVF_PER_STATUS_OK : OVF_PER_STATUS_NOT_FOUND;
}

ovf_per_status_v1 Begin(ovf_per_backend_v1* backend, ovf_per_handle_v1 store_handle,
                        ovf_per_durability_v1 durability, bool write, ovf_per_handle_v1* output,
                        std::uint64_t* generation) {
  auto& self = *Self(backend);
  if (output == nullptr || generation == nullptr) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto store = self.store_handles.find(store_handle);
  if (store == self.store_handles.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  if (self.transactions.size() >= self.max_transactions) {
    SetError(self, "transaction limit reached");
    return OVF_PER_STATUS_RESOURCE_EXHAUSTED;
  }
  if (write && store->second.access != OVF_PER_ACCESS_READ_WRITE) {
    return OVF_PER_STATUS_PERMISSION_DENIED;
  }
  if (write && durability != OVF_PER_DURABILITY_BUFFERED) {
    SetError(self, "memory provider supports buffered durability only");
    return OVF_PER_STATUS_UNSUPPORTED;
  }
  if (write && store->second.store->writer_active) {
    return OVF_PER_STATUS_BUSY;
  }
  if (write) {
    store->second.store->writer_active = true;
  }
  const auto handle = self.next_handle++;
  const auto current_generation = store->second.store->generation;
  self.transactions.emplace(handle, Transaction{store->second.store, store->second.store->values,
                                                current_generation, durability, write, false});
  *output = handle;
  *generation = current_generation;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 BeginRead(ovf_per_backend_v1* backend, ovf_per_handle_v1 store,
                            ovf_per_handle_v1* output, std::uint64_t* generation) {
  return Begin(backend, store, OVF_PER_DURABILITY_BUFFERED, false, output, generation);
}

ovf_per_status_v1 BeginWrite(ovf_per_backend_v1* backend, ovf_per_handle_v1 store,
                             ovf_per_durability_v1 durability, ovf_per_handle_v1* output,
                             std::uint64_t* generation) {
  return Begin(backend, store, durability, true, output, generation);
}

ovf_per_status_v1 Get(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                      ovf_per_bytes_view_v1 key, ovf_per_mutable_bytes_v1* output) {
  auto& self = *Self(backend);
  if (!Valid(key) || key.size == 0 || output == nullptr) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto transaction = self.transactions.find(handle);
  if (transaction == self.transactions.end() || transaction->second.finished) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  const auto value = transaction->second.values.find(Copy(key));
  if (value == transaction->second.values.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  if (output->data == nullptr || output->size < value->second.size()) {
    output->size = value->second.size();
    return OVF_PER_STATUS_BUFFER_TOO_SMALL;
  }
  std::memcpy(output->data, value->second.data(), value->second.size());
  output->size = value->second.size();
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 Put(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                      ovf_per_bytes_view_v1 key, ovf_per_bytes_view_v1 value) {
  auto& self = *Self(backend);
  if (!Valid(key) || !Valid(value) || key.size == 0) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto transaction = self.transactions.find(handle);
  if (transaction == self.transactions.end() || !transaction->second.write ||
      transaction->second.finished) {
    return OVF_PER_STATUS_INVALID_STATE;
  }
  auto& store = *transaction->second.store;
  if (key.size > store.max_key_size || value.size > store.max_value_size) {
    SetError(self, "key or value exceeds its configured bound");
    return OVF_PER_STATUS_QUOTA_EXCEEDED;
  }
  auto candidate = transaction->second.values;
  candidate[Copy(key)] = Copy(value);
  if (candidate.size() + store.blobs.size() > store.max_entries ||
      Size(candidate) + Size(store.blobs) > store.capacity) {
    SetError(self, "transaction exceeds store quota");
    return OVF_PER_STATUS_QUOTA_EXCEEDED;
  }
  transaction->second.values = std::move(candidate);
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 Erase(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                        ovf_per_bytes_view_v1 key, std::uint8_t* erased) {
  auto& self = *Self(backend);
  if (!Valid(key) || key.size == 0 || erased == nullptr) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto transaction = self.transactions.find(handle);
  if (transaction == self.transactions.end() || !transaction->second.write ||
      transaction->second.finished) {
    return OVF_PER_STATUS_INVALID_STATE;
  }
  *erased = transaction->second.values.erase(Copy(key)) == 1 ? 1 : 0;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 Commit(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                         ovf_per_commit_result_v1* output) {
  auto& self = *Self(backend);
  if (output == nullptr || output->struct_size < sizeof(*output)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto transaction = self.transactions.find(handle);
  if (transaction == self.transactions.end() || !transaction->second.write ||
      transaction->second.finished) {
    return OVF_PER_STATUS_INVALID_STATE;
  }
  auto& item = transaction->second;
  if (item.store->generation != item.base_generation) {
    item.store->writer_active = false;
    item.finished = true;
    return OVF_PER_STATUS_CONFLICT;
  }
  item.store->values = item.values;
  ++item.store->generation;
  item.store->writer_active = false;
  item.finished = true;
  *output = {sizeof(*output), item.store->generation, OVF_PER_DURABILITY_BUFFERED};
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 Abort(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  const auto transaction = self.transactions.find(handle);
  if (transaction == self.transactions.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  if (transaction->second.write && !transaction->second.finished) {
    transaction->second.store->writer_active = false;
  }
  transaction->second.finished = true;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 CloseTransaction(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  const auto transaction = self.transactions.find(handle);
  if (transaction == self.transactions.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  if (transaction->second.write && !transaction->second.finished) {
    transaction->second.store->writer_active = false;
  }
  self.transactions.erase(transaction);
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 OpenBlobRead(ovf_per_backend_v1* backend, ovf_per_handle_v1 store_handle,
                               ovf_per_bytes_view_v1 key, ovf_per_handle_v1* output,
                               std::uint64_t* size, std::uint64_t* generation) {
  auto& self = *Self(backend);
  if (!Valid(key) || key.size == 0 || output == nullptr || size == nullptr ||
      generation == nullptr) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto handle = self.store_handles.find(store_handle);
  if (handle == self.store_handles.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  const auto found = handle->second.store->blobs.find(Copy(key));
  if (found == handle->second.store->blobs.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  const auto blob_handle = self.next_handle++;
  self.blobs.emplace(blob_handle,
                     BlobOperation{handle->second.store, Copy(key), found->second,
                                   handle->second.store->generation, found->second.size(),
                                   OVF_PER_DURABILITY_BUFFERED, false, false});
  *output = blob_handle;
  *size = found->second.size();
  *generation = handle->second.store->generation;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 BeginBlobReplace(ovf_per_backend_v1* backend, ovf_per_handle_v1 store_handle,
                                   ovf_per_bytes_view_v1 key, std::uint64_t size,
                                   ovf_per_durability_v1 durability, ovf_per_handle_v1* output,
                                   std::uint64_t* generation) {
  auto& self = *Self(backend);
  if (!Valid(key) || key.size == 0 || output == nullptr || generation == nullptr) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto handle = self.store_handles.find(store_handle);
  if (handle == self.store_handles.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  auto& store = *handle->second.store;
  if (handle->second.access != OVF_PER_ACCESS_READ_WRITE) {
    return OVF_PER_STATUS_PERMISSION_DENIED;
  }
  if (durability != OVF_PER_DURABILITY_BUFFERED) {
    return OVF_PER_STATUS_UNSUPPORTED;
  }
  if (store.writer_active) {
    return OVF_PER_STATUS_BUSY;
  }
  if (key.size > store.max_key_size || size > store.max_blob_size) {
    return OVF_PER_STATUS_QUOTA_EXCEEDED;
  }
  try {
    auto candidate = store.blobs;
    candidate[Copy(key)] = Bytes(static_cast<std::size_t>(size));
    if (candidate.size() + store.values.size() > store.max_entries ||
        Size(candidate) + Size(store.values) > store.capacity) {
      return OVF_PER_STATUS_QUOTA_EXCEEDED;
    }
    store.writer_active = true;
    const auto blob_handle = self.next_handle++;
    self.blobs.emplace(blob_handle, BlobOperation{handle->second.store, Copy(key), Bytes(size),
                                                  store.generation, 0, durability, true, false});
    *output = blob_handle;
    *generation = store.generation;
    return OVF_PER_STATUS_OK;
  } catch (...) {
    store.writer_active = false;
    return OVF_PER_STATUS_RESOURCE_EXHAUSTED;
  }
}

ovf_per_status_v1 ReadBlob(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                           std::uint64_t offset, ovf_per_mutable_bytes_v1* output) {
  auto& self = *Self(backend);
  if (output == nullptr || (output->data == nullptr && output->size != 0)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto blob = self.blobs.find(handle);
  if (blob == self.blobs.end() || blob->second.write || blob->second.finished ||
      offset > blob->second.value.size() || output->size > blob->second.value.size() - offset) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  if (output->size != 0) {
    std::memcpy(output->data, blob->second.value.data() + offset, output->size);
  }
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 WriteBlob(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                            std::uint64_t offset, ovf_per_bytes_view_v1 input) {
  auto& self = *Self(backend);
  if (!Valid(input)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto blob = self.blobs.find(handle);
  if (blob == self.blobs.end() || !blob->second.write || blob->second.finished ||
      offset != blob->second.written || input.size > blob->second.value.size() - offset) {
    return OVF_PER_STATUS_INVALID_STATE;
  }
  if (input.size != 0) {
    std::memcpy(blob->second.value.data() + offset, input.data, input.size);
  }
  blob->second.written += input.size;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 CommitBlob(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle,
                             ovf_per_commit_result_v1* output) {
  auto& self = *Self(backend);
  if (output == nullptr || output->struct_size < sizeof(*output)) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  const auto blob = self.blobs.find(handle);
  if (blob == self.blobs.end() || !blob->second.write || blob->second.finished ||
      blob->second.written != blob->second.value.size()) {
    return OVF_PER_STATUS_INVALID_STATE;
  }
  auto& item = blob->second;
  if (item.store->generation != item.generation) {
    item.store->writer_active = false;
    item.finished = true;
    return OVF_PER_STATUS_CONFLICT;
  }
  item.store->blobs[item.key] = item.value;
  ++item.store->generation;
  item.store->writer_active = false;
  item.finished = true;
  *output = {sizeof(*output), item.store->generation, item.durability};
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 AbortBlob(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  const auto blob = self.blobs.find(handle);
  if (blob == self.blobs.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  if (blob->second.write && !blob->second.finished) {
    blob->second.store->writer_active = false;
  }
  blob->second.finished = true;
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 CloseBlob(ovf_per_backend_v1* backend, ovf_per_handle_v1 handle) {
  auto& self = *Self(backend);
  std::scoped_lock lock(self.mutex);
  const auto blob = self.blobs.find(handle);
  if (blob == self.blobs.end()) {
    return OVF_PER_STATUS_NOT_FOUND;
  }
  if (blob->second.write && !blob->second.finished) {
    blob->second.store->writer_active = false;
  }
  self.blobs.erase(blob);
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 LastError(ovf_per_backend_v1* backend, ovf_per_mutable_bytes_v1* output) {
  auto& self = *Self(backend);
  if (output == nullptr) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  std::scoped_lock lock(self.mutex);
  if (output->data == nullptr || output->size < self.error.size()) {
    output->size = self.error.size();
    return OVF_PER_STATUS_BUFFER_TOO_SMALL;
  }
  std::memcpy(output->data, self.error.data(), self.error.size());
  output->size = self.error.size();
  return OVF_PER_STATUS_OK;
}

ovf_per_status_v1 Create(const ovf_per_backend_config_v1* config, ovf_per_backend_v1** output) {
  if (config == nullptr || config->struct_size < sizeof(*config) || output == nullptr ||
      config->max_stores == 0 || config->max_transactions == 0) {
    return OVF_PER_STATUS_INVALID_ARGUMENT;
  }
  try {
    auto backend = std::make_unique<MemoryBackend>();
    backend->max_stores = config->max_stores;
    backend->max_transactions = config->max_transactions;
    backend->abi = {sizeof(ovf_per_backend_v1),
                    OVF_PER_BACKEND_ABI_VERSION_1,
                    backend.get(),
                    Start,
                    Stop,
                    Capabilities,
                    OpenStore,
                    CloseStore,
                    BeginRead,
                    BeginWrite,
                    Get,
                    Put,
                    Erase,
                    Commit,
                    Abort,
                    CloseTransaction,
                    OpenBlobRead,
                    BeginBlobReplace,
                    ReadBlob,
                    WriteBlob,
                    CommitBlob,
                    AbortBlob,
                    CloseBlob,
                    LastError};
    *output = &backend.release()->abi;
    return OVF_PER_STATUS_OK;
  } catch (...) {
    return OVF_PER_STATUS_RESOURCE_EXHAUSTED;
  }
}

void Destroy(ovf_per_backend_v1* backend) { delete Self(backend); }

const ovf_per_backend_factory_v1 kFactory{sizeof(ovf_per_backend_factory_v1),
                                          OVF_PER_BACKEND_ABI_VERSION_1,
                                          {"memory", 6},
                                          Create,
                                          Destroy};

} // namespace

extern "C" const ovf_per_backend_factory_v1* ovf_per_backend_query_v1(void) { return &kFactory; }
