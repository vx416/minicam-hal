#include "hal/epoll_event_loop.h"

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#endif

namespace minicam {

namespace {

#ifdef __linux__
FdReadyEvents to_fd_ready_events(uint32_t events) {
  return FdReadyEvents{
      .readable = (events & EPOLLIN) != 0,
      .writable = (events & EPOLLOUT) != 0,
      .error = (events & EPOLLERR) != 0,
      .hangup = (events & EPOLLHUP) != 0,
  };
}
#endif

}  // namespace

EpollEventLoop::EpollEventLoop() = default;

EpollEventLoop::~EpollEventLoop() {
  stop();
}

bool EpollEventLoop::start() {
#ifdef __linux__
  if (running_) {
    return true;
  }

  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    set_error(std::string("epoll_create1 failed: ") + std::strerror(errno));
    return false;
  }

  wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (wake_fd_ < 0) {
    set_error(std::string("eventfd failed: ") + std::strerror(errno));
    ::close(epoll_fd_);
    epoll_fd_ = -1;
    return false;
  }

  epoll_event event{};
  event.events = EPOLLIN;
  event.data.fd = wake_fd_;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &event) < 0) {
    set_error(std::string("epoll_ctl wake fd failed: ") + std::strerror(errno));
    ::close(wake_fd_);
    ::close(epoll_fd_);
    wake_fd_ = -1;
    epoll_fd_ = -1;
    return false;
  }

  running_ = true;
  thread_ = std::thread(&EpollEventLoop::loop, this);
  return true;
#else
  set_error("EpollEventLoop is only supported on Linux");
  return false;
#endif
}

void EpollEventLoop::stop() {
#ifdef __linux__
  if (!running_.exchange(false)) {
    return;
  }

  if (wake_fd_ >= 0) {
    uint64_t value = 1;
    (void)::write(wake_fd_, &value, sizeof(value));
  }

  if (thread_.joinable()) {
    thread_.join();
  }

  if (wake_fd_ >= 0) {
    ::close(wake_fd_);
    wake_fd_ = -1;
  }
  if (epoll_fd_ >= 0) {
    ::close(epoll_fd_);
    epoll_fd_ = -1;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  callbacks_.clear();
#else
  running_ = false;
#endif
}

bool EpollEventLoop::register_fd(int fd, FdReadyCallback callback) {
#ifdef __linux__
  if (!running_ || epoll_fd_ < 0 || fd < 0 || !callback) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_[fd] = std::move(callback);
  }

  epoll_event event{};
  event.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLONESHOT;
  event.data.fd = fd;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) < 0) {
    if (errno == EEXIST) {
      if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) == 0) {
        return true;
      }
    }

    set_error(std::string("epoll_ctl add/mod failed: ") +
              std::strerror(errno));
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.erase(fd);
    return false;
  }
  return true;
#else
  (void)fd;
  (void)callback;
  set_error("EpollEventLoop is only supported on Linux");
  return false;
#endif
}

bool EpollEventLoop::rearm_fd(int fd) {
#ifdef __linux__
  if (!running_ || epoll_fd_ < 0 || fd < 0) {
    return false;
  }

  epoll_event event{};
  event.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLONESHOT;
  event.data.fd = fd;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
    set_error(std::string("epoll_ctl rearm failed: ") + std::strerror(errno));
    return false;
  }
  return true;
#else
  (void)fd;
  set_error("EpollEventLoop is only supported on Linux");
  return false;
#endif
}

void EpollEventLoop::unregister_fd(int fd) {
#ifdef __linux__
  if (epoll_fd_ >= 0 && fd >= 0) {
    (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  }
#endif
  std::lock_guard<std::mutex> lock(mutex_);
  callbacks_.erase(fd);
}

const std::string& EpollEventLoop::last_error() const {
  return last_error_;
}

void EpollEventLoop::loop() {
#ifdef __linux__
  std::array<epoll_event, 32> events{};

  while (running_) {
    const int count = ::epoll_wait(
        epoll_fd_, events.data(), static_cast<int>(events.size()), -1);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      set_error(std::string("epoll_wait failed: ") + std::strerror(errno));
      break;
    }

    for (int i = 0; i < count; ++i) {
      const int fd = events[static_cast<size_t>(i)].data.fd;
      if (fd == wake_fd_) {
        uint64_t value = 0;
        while (::read(wake_fd_, &value, sizeof(value)) > 0) {
        }
        continue;
      }

      FdReadyCallback callback;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = callbacks_.find(fd);
        if (it != callbacks_.end()) {
          callback = it->second;
        }
      }

      if (callback) {
        callback(fd, to_fd_ready_events(events[static_cast<size_t>(i)].events));
      }
    }
  }
#endif
}

void EpollEventLoop::set_error(std::string error) {
  last_error_ = std::move(error);
}

}  // namespace minicam
