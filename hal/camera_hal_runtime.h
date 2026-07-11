#pragma once

#include "hal/dma_buf_allocator.h"
#include "hal/epoll_event_loop.h"
#include "hal/task_executor.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace minicam {

class CameraDeviceSession;

struct CameraHalRuntimeConfig {
  TaskExecutorConfig executor;
  std::string dma_heap_path = "/dev/dma_heap/system";
};

// Process-level HAL runtime, analogous to a server runtime in a connection
// server. It owns shared execution/event resources:
// - one epoll event loop for driver fd readiness
// - one task executor for per-session serialized tasks
// - a registry of active sessions
//
// CameraDeviceSession owns per-session camera semantics.
class CameraHalRuntime {
 public:
  explicit CameraHalRuntime(CameraHalRuntimeConfig config = {});
  ~CameraHalRuntime();

  CameraHalRuntime(const CameraHalRuntime&) = delete;
  CameraHalRuntime& operator=(const CameraHalRuntime&) = delete;

  bool start();
  void stop();

  bool register_session(int session_id,
                        std::weak_ptr<CameraDeviceSession> session,
                        const std::vector<int>& driver_fds);
  void unregister_session(int session_id, const std::vector<int>& driver_fds);

  bool post_session_task(int session_id, RuntimeTask task);

  EpollEventLoop& event_loop();
  TaskExecutor& executor();
  DmaBufPool& dma_buf_pool();
  const std::string& last_error() const;

 private:
  void on_driver_fd_ready(std::weak_ptr<CameraDeviceSession> session,
                          int fd,
                          FdReadyEvents events);

  std::mutex mutex_;
  std::unordered_map<int, std::weak_ptr<CameraDeviceSession>> sessions_;
  CameraHalRuntimeConfig config_;
  EpollEventLoop event_loop_;
  TaskExecutor executor_;
  DmaBufPool dma_buf_pool_;
  std::string last_error_;
};

}  // namespace minicam
