#include "ipc.h"
#include "serializer.h"
#include "utils.h"

#include "third_party/doctest/doctest/doctest.h"

namespace {
// The absolute smallest partial payload we should send. This must be >0, ie, 1 is the
// minimum. Keep a reasonably high value so we don't send needlessly send tiny payloads.
const int kMinimumPartialPayloadSize = 128;

const int kBufferSize = 1024 * 1024 * 32;  // number of chars/bytes (32mb) in the message buffer.

// JSON-encoded message that is passed across shared memory.
//
// Messages are funky objects. They contain potentially variable amounts of
// data and are passed between processes. This means that they need to be
// fully relocatable, ie, it is possible to memmove them in memory to a
// completely different address.
struct JsonMessage {
  IpcId ipc_id;
  int partial_message_id;
  bool has_more_chunks;
  size_t payload_size;
  void* payload() {
    return reinterpret_cast<char*>(this) + sizeof(JsonMessage);
  }

  void Setup(IpcId ipc_id, int partial_message_id, bool has_more_chunks, size_t payload_size, const char* payload) {
    this->ipc_id = ipc_id;
    this->partial_message_id = partial_message_id;
    this->has_more_chunks = has_more_chunks;
    this->payload_size = payload_size;

    char* payload_dest = reinterpret_cast<char*>(this) + sizeof(JsonMessage);
    memcpy(payload_dest, payload, payload_size);
  }
};

std::string NameToServerName(const std::string& name) {
  return name + "server";
}

std::string NameToClientName(const std::string& name, int client_id) {
  return name + "client" + std::to_string(client_id);
}
}

IpcRegistry* IpcRegistry::instance_ = nullptr;

std::unique_ptr<IpcMessage> IpcRegistry::Allocate(IpcId id) {
  assert(allocators_);
  auto it = allocators_->find(id);
  assert(it != allocators_->end() && "No registered allocator for id");
  return std::unique_ptr<IpcMessage>(it->second());
}

struct IpcDirectionalChannel::MessageBuffer {
  MessageBuffer(void* buffer, size_t buffer_size, bool initialize) {
    real_buffer = buffer;
    real_buffer_size = buffer_size;
    if (initialize)
      new(real_buffer) Metadata();
  }

  // Pointer to the start of the actual buffer and the
  // amount of storage actually available.
  void* real_buffer;
  size_t real_buffer_size;

  template<typename T>
  T* Offset(size_t offset) const {
    return reinterpret_cast<T*>(static_cast<char*>(real_buffer) + offset);
  }

  struct Metadata {
    // The number of bytes that are currently used in the buffer minus the
    // size of this Metadata struct.
    size_t bytes_used = 0;
    int next_partial_message_id = 0;
    int num_outstanding_partial_messages = 0;
  };

  Metadata* metadata() const {
    return Offset<Metadata>(0);
  }

  size_t bytes_available() const {
    return real_buffer_size - sizeof(Metadata) - metadata()->bytes_used;
  }

  JsonMessage* message_at_offset(size_t offset) const {
    return Offset<JsonMessage>(sizeof(Metadata) + offset);
  }

  // First json message.
  JsonMessage* first_message() const {
    return message_at_offset(0);
  }

  // First free, writable json message. Make sure to increase *bytes_used()
  // by any written size.
  JsonMessage* free_message() const {
    return message_at_offset(metadata()->bytes_used);
  }

  struct Iterator {
    void* buffer;
    size_t remaining_bytes;

    Iterator(void* buffer, size_t remaining_bytes) : buffer(buffer), remaining_bytes(remaining_bytes) {}

    JsonMessage* get() const {
      assert(buffer);
      return reinterpret_cast<JsonMessage*>(buffer);
    }

    JsonMessage* operator*() const {
      return get();
    }

    JsonMessage* operator->() const {
      return get();
    }

    void operator++() {
      size_t next_message_offset = sizeof(JsonMessage) + get()->payload_size;
      if (next_message_offset >= remaining_bytes) {
        assert(next_message_offset == remaining_bytes);
        buffer = nullptr;
        remaining_bytes = 0;
        return;
      }

      buffer = (char*)buffer + next_message_offset;
      remaining_bytes -= next_message_offset;
    }

    bool operator==(const Iterator& other) const {
      return buffer == other.buffer && remaining_bytes == other.remaining_bytes;
    }
    bool operator!=(const Iterator& other) const {
      return !(*this == other);
    }
  };

  Iterator begin() const {
    if (metadata()->bytes_used == 0)
      return end();

    return Iterator(first_message(), metadata()->bytes_used);
  }
  Iterator end() const {
    return Iterator(nullptr, 0);
  }
};

struct IpcDirectionalChannel::ResizableBuffer {
  void* memory;
  size_t size;
  size_t capacity;

  ResizableBuffer() {
    memory = malloc(128);
    size = 0;
    capacity = 128;
  }

  ~ResizableBuffer() {
    free(memory);
    size = 0;
    capacity = 0;
  }

  void Append(void* content, size_t content_size) {
    assert(capacity);

    // Grow memory if needed.
    if ((size + content_size) >= capacity) {
      size_t new_capacity = capacity * 2;
      while (new_capacity < size + content_size)
        new_capacity *= 2;
      void* new_memory = malloc(new_capacity);
      assert(size < capacity);
      memcpy(new_memory, memory, size);
      free(memory);
      memory = new_memory;
      capacity = new_capacity;
    }

    // Append new content into memory.
    memcpy((char*)memory + size, content, content_size);
    size += content_size;
  }

  void Reset() {
    size = 0;
  }
};

IpcDirectionalChannel::ResizableBuffer* IpcDirectionalChannel::CreateOrFindResizableBuffer(int id) {
  auto it = resizable_buffers.find(id);
  if (it != resizable_buffers.end())
    return it->second.get();
  return (resizable_buffers[id] = MakeUnique<ResizableBuffer>()).get();
}

void IpcDirectionalChannel::RemoveResizableBuffer(int id) {
  resizable_buffers.erase(id);
}

IpcDirectionalChannel::IpcDirectionalChannel(const std::string& name, bool initialize_shared_memory) {
  shared = CreatePlatformSharedMemory(name + "memory", kBufferSize);
  mutex = CreatePlatformMutex(name + "mutex");
  local = std::unique_ptr<char>(new char[kBufferSize]);

  // TODO: connecting a client will allocate reset shared state on the
  // buffer. We need to store if we "initialized".
  shared_buffer = MakeUnique<MessageBuffer>(shared->data, kBufferSize, initialize_shared_memory);
  local_buffer = MakeUnique<MessageBuffer>(local.get(), kBufferSize, true /*initialize*/);
}

IpcDirectionalChannel::~IpcDirectionalChannel() {}

enum class DispatchResult {
  RunAgain,
  Break
};

// Run |action| an arbitrary number of times.
void IpcDispatch(PlatformMutex* mutex, std::function<DispatchResult()> action) {
  bool first = true;
  int log_iteration_count = 0;
  int log_count = 0;
  while (true) {
    if (!first) {
      if (log_iteration_count > 1000) {
        log_iteration_count = 0;
        std::cerr << "[info]: shmem full, waiting (" << log_count++ << ")" << std::endl; // TODO: remove
      }
      ++log_iteration_count;
      // TODO: See if we can figure out a way to use condition variables cross-process.
      std::this_thread::sleep_for(std::chrono::microseconds(0));
    }
    first = false;

    std::unique_ptr<PlatformScopedMutexLock> lock = CreatePlatformScopedMutexLock(mutex);
    if (action() == DispatchResult::RunAgain)
      continue;
    break;
  }
}

void IpcDirectionalChannel::PushMessage(IpcMessage* message) {
  assert(message->ipc_id != IpcId::Invalid);
  assert(kBufferSize > sizeof(JsonMessage) + kMinimumPartialPayloadSize);

  rapidjson::StringBuffer output;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(output);
  writer.SetFormatOptions(
    rapidjson::PrettyFormatOptions::kFormatSingleLineArray);
  writer.SetIndent(' ', 2);
  message->Serialize(writer);

  //std::cerr << "Sending message with id " << message->runtime_id() << " (hash " << message->hashed_runtime_id() << ")" << std::endl;


  size_t payload_size = output.GetSize();
  const char* payload = output.GetString();
  if (payload_size == 0)
    return;

  int partial_message_id = 0;

  std::cerr << "Starting dispatch of payload with size " << payload_size << std::endl;
  int count = 0;

  IpcDispatch(mutex.get(), [&]() {
    assert(payload_size > 0);

    // We cannot find the entire payload in the buffer. We
    // have to send chunks of it over time.
    if ((sizeof(JsonMessage) + payload_size) > shared_buffer->bytes_available()) {
      if ((sizeof(JsonMessage) + kMinimumPartialPayloadSize) > shared_buffer->bytes_available())
        return DispatchResult::RunAgain;

      if (partial_message_id == 0)
        partial_message_id = ++shared_buffer->metadata()->next_partial_message_id; // note: pre-increment so we 1 as initial value

      size_t sent_payload_size = shared_buffer->bytes_available() - sizeof(JsonMessage);
      shared_buffer->free_message()->Setup(message->ipc_id, partial_message_id, true /*has_more_chunks*/, sent_payload_size, payload);
      shared_buffer->metadata()->bytes_used += sizeof(JsonMessage) + sent_payload_size;
      //shared_buffer->free_message()->ipc_id = IpcId::Invalid; // Note: free_message() may be past writable memory.

      if (count++ > 50) {
        std::cerr << "x50 Sending partial message with payload_size=" << sent_payload_size << std::endl;
        count = 0;
      }

      // Prepare for next time.
      payload_size -= sent_payload_size;
      payload += sent_payload_size;
      return DispatchResult::RunAgain;
    }
    // The entire payload fits. Send it all now.
    else {
      // Include partial message id, as there could have been previous parts of this payload.
      shared_buffer->free_message()->Setup(message->ipc_id, partial_message_id, false /*has_more_chunks*/, payload_size, payload);
      shared_buffer->metadata()->bytes_used += sizeof(JsonMessage) + payload_size;
      shared_buffer->free_message()->ipc_id = IpcId::Invalid;
      //std::cerr << "Sending full message with payload_size=" << payload_size << std::endl;

      return DispatchResult::Break;
    }
  });
}

void AddIpcMessageFromJsonMessage(std::vector<std::unique_ptr<IpcMessage>>& result, IpcId ipc_id, void* payload, size_t payload_size) {
  rapidjson::Document document;
  document.Parse(reinterpret_cast<const char*>(payload), payload_size);
  bool has_error = document.HasParseError();
  auto error = document.GetParseError();

  std::unique_ptr<IpcMessage> base_message = IpcRegistry::instance()->Allocate(ipc_id);
  base_message->Deserialize(document);
  result.emplace_back(std::move(base_message));
}

std::vector<std::unique_ptr<IpcMessage>> IpcDirectionalChannel::TakeMessages() {
  std::vector<std::unique_ptr<IpcMessage>> result;

  do {
    // Move data from shared memory into a local buffer. Do this
    // before parsing the blocks so that other processes can begin
    // posting data as soon as possible.
    {
      std::unique_ptr<PlatformScopedMutexLock> lock = CreatePlatformScopedMutexLock(mutex.get());
      assert(shared_buffer->metadata()->bytes_used <= kBufferSize);
      memcpy(local.get(), shared->data, sizeof(MessageBuffer::Metadata) + shared_buffer->metadata()->bytes_used);
      shared_buffer->metadata()->bytes_used = 0;
      shared_buffer->free_message()->ipc_id = IpcId::Invalid;
    }

    // Parse blocks from shared memory.
    for (JsonMessage* message : *local_buffer) {
      //std::cerr << "Got message with payload_size=" << message->payload_size << std::endl;

      if (message->partial_message_id != 0) {
        auto* buf = CreateOrFindResizableBuffer(message->partial_message_id);
        buf->Append(message->payload(), message->payload_size);
        if (!message->has_more_chunks) {
          AddIpcMessageFromJsonMessage(result, message->ipc_id, buf->memory, buf->size);
          RemoveResizableBuffer(message->partial_message_id);
        }
      }
      else {
        assert(!message->has_more_chunks);
        AddIpcMessageFromJsonMessage(result, message->ipc_id, message->payload(), message->payload_size);
      }
    }
    local_buffer->metadata()->bytes_used = 0;

    // Let other threads run. We still want to run as fast as possible, though.
    std::this_thread::sleep_for(std::chrono::microseconds(0));
  } while (resizable_buffers.size() > 0);

  return result;
}



IpcServer::IpcServer(const std::string& name, int num_clients)
  : server_(NameToServerName(name), true /*initialize_shared_memory*/) {

  for (int i = 0; i < num_clients; ++i) {
    clients_.push_back(MakeUnique<IpcDirectionalChannel>(NameToClientName(name, i), true /*initialize_shared_memory*/));
  }
}

void IpcServer::SendToClient(int client_id, IpcMessage* message) {
  clients_[client_id]->PushMessage(message);
}

std::vector<std::unique_ptr<IpcMessage>> IpcServer::TakeMessages() {
  return server_.TakeMessages();
}

IpcClient::IpcClient(const std::string& name, int client_id)
  : server_(NameToServerName(name), false /*initialize_shared_memory*/),
  client_(NameToClientName(name, client_id), false /*initialize_shared_memory*/) {}

void IpcClient::SendToServer(IpcMessage* message) {
  server_.PushMessage(message);
}

std::vector<std::unique_ptr<IpcMessage>> IpcClient::TakeMessages() {
  return client_.TakeMessages();
}





template<typename T>
struct TestIpcMessage : IpcMessage {
  T data;

  TestIpcMessage() : IpcMessage(IpcId::Test) {}
  ~TestIpcMessage() override {}

  // IpcMessage:
  void Serialize(Writer& writer) override {
    Reflect(writer, data);
  }
  void Deserialize(Reader& reader) override {
    Reflect(reader, data);
  }
};

#if false
TEST_CASE("foo") {
  IpcRegistry::instance()->Register<TestIpcMessage<std::string>>(IpcId::Test);

  IpcDirectionalChannel channel0("indexertestmemory", true /*initialize_shared_memory*/);
  IpcDirectionalChannel channel1("indexertestmemory", false /*initialize_shared_memory*/);

  TestIpcMessage<std::string> m;
  m.data = "hey there";

  channel0.PushMessage(&m);
  std::vector<std::unique_ptr<IpcMessage>> messages = channel1.TakeMessages();
  REQUIRE(messages.size() == 1);
  REQUIRE(messages[0]->ipc_id == m.ipc_id);
  REQUIRE(reinterpret_cast<TestIpcMessage<std::string>*>(messages[0].get())->data == m.data);
}
#endif