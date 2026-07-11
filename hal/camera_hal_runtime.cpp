#include "hal/camera_hal_runtime.h"

#include "hal/camera_device_session.h"

#include <utility>

namespace minicam {

CameraHalRuntime::CameraHalRuntime(CameraHalRuntimeConfig config)
    : config_(std::move(config)),
      executor_(config_.executor),
      dma_buf_pool_(config_.dma_heap_path) {}

CameraHalRuntime::~CameraHalRuntime() {
  stop();
}

bool CameraHalRuntime::start() {
  if (!executor_.start()) {
    last_error_ = "failed to start TaskExecutor";
    return false;
  }

  if (!event_loop_.start()) {
    last_error_ = event_loop_.last_error();
    executor_.stop();
    return false;
  }

  return true;
}

void CameraHalRuntime::stop() {
  event_loop_.stop();
  executor_.stop();
}

bool CameraHalRuntime::register_session(
    int session_id,
    std::weak_ptr<CameraDeviceSession> session,
    const std::vector<int>& driver_fds) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[session_id] = session;
  }

  std::vector<int> registered_fds;
  for (int driver_fd : driver_fds) {
    if (driver_fd < 0) {
      continue;
    }
    const bool registered = event_loop_.register_fd(
        driver_fd,
        [this, session](int fd, FdReadyEvents events) {
          on_driver_fd_ready(session, fd, events);
        });
    if (!registered) {
      for (int registered_fd : registered_fds) {
        event_loop_.unregister_fd(registered_fd);
      }
      std::lock_guard<std::mutex> lock(mutex_);
      sessions_.erase(session_id);
      last_error_ = event_loop_.last_error();
      return false;
    }
    registered_fds.push_back(driver_fd);
  }

  if (registered_fds.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(session_id);
    last_error_ = "driver exposed no event fd";
    return false;
  }
  return true;
}

void CameraHalRuntime::unregister_session(int session_id,
                                          const std::vector<int>& driver_fds) {
  for (int driver_fd : driver_fds) {
    if (driver_fd >= 0) {
      event_loop_.unregister_fd(driver_fd);
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.erase(session_id);
}

bool CameraHalRuntime::post_session_task(int session_id, RuntimeTask task) {
  return executor_.post(static_cast<uint64_t>(session_id), std::move(task));
}

EpollEventLoop& CameraHalRuntime::event_loop() {
  return event_loop_;
}

TaskExecutor& CameraHalRuntime::executor() {
  return executor_;
}

DmaBufPool& CameraHalRuntime::dma_buf_pool() {
  return dma_buf_pool_;
}

const std::string& CameraHalRuntime::last_error() const {
  return last_error_;
}

void CameraHalRuntime::on_driver_fd_ready(
    std::weak_ptr<CameraDeviceSession> session,
    int fd,
    FdReadyEvents events) {
  if (auto locked = session.lock()) {
    const int session_id = locked->session_id();
    const bool posted = post_session_task(
        session_id,
        [this, session = std::move(session), fd, events] {
          if (auto locked = session.lock()) {
            locked->on_driver_fd_ready(fd, events);
            (void)event_loop_.rearm_fd(fd);
          }
        });
    if (!posted) {
      last_error_ = "failed to post driver readiness task";
    }
  }
}

}  // namespace minicam
