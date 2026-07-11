#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace minicam {

struct FdReadyEvents {
  bool readable = false;
  bool writable = false;
  bool error = false;
  bool hangup = false;
};

// Callback invoked when a registered fd has readiness events.
using FdReadyCallback = std::function<void(int fd, FdReadyEvents events)>;

// Runtime-level epoll loop. Sessions should not each own their own event loop;
// they register their V4L2 fd here and expose hook methods for readiness.
class EpollEventLoop {
 public:
  EpollEventLoop();
  ~EpollEventLoop();

  EpollEventLoop(const EpollEventLoop&) = delete;
  EpollEventLoop& operator=(const EpollEventLoop&) = delete;

  bool start();
  void stop();

  bool register_fd(int fd, FdReadyCallback callback);
  bool rearm_fd(int fd);
  void unregister_fd(int fd);

  const std::string& last_error() const;

 private:
  void loop();
  void set_error(std::string error);

  std::mutex mutex_;
  std::unordered_map<int, FdReadyCallback> callbacks_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::string last_error_;
  [[maybe_unused]] int epoll_fd_ = -1;
  [[maybe_unused]] int wake_fd_ = -1;
};

}  // namespace minicam
