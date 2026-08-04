/* SPDX-License-Identifier: Apache-2.0 */
/*
 * YukiZygisk - Reusable standalone kernel control client.
 *
 * License: Apache-2.0
 *
 * Author: Anatdx
 */

#include "host.hpp"

#include "uapi/yukizygisk.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace yzctl {

Host::~Host() {
  if (fd_ >= 0)
    close(fd_);
}

bool Host::open(std::string *error) {
  if (fd_ >= 0)
    return true;

  int delivered_fd = -1;
  errno = 0;
  const long result =
      syscall(SYS_prctl, static_cast<unsigned long>(YZ_PRCTL_CONTROL_OPTION),
              static_cast<unsigned long>(YZ_PRCTL_CONTROL_MAGIC),
              reinterpret_cast<unsigned long>(&delivered_fd), 0UL, 0UL);
  const int saved_errno = errno;
  (void)result;

  if (delivered_fd < 0) {
    if (error != nullptr) {
      *error = "cannot open YukiZygisk kernel control session: ";
      *error += strerror(saved_errno);
    }
    errno = saved_errno;
    return false;
  }

  const int flags = fcntl(delivered_fd, F_GETFD);
  if (flags < 0 || fcntl(delivered_fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
    const int fd_errno = errno;
    close(delivered_fd);
    if (error != nullptr) {
      *error = "cannot secure YukiZygisk kernel control fd: ";
      *error += strerror(fd_errno);
    }
    errno = fd_errno;
    return false;
  }

  fd_ = delivered_fd;
  return true;
}

int Host::call(unsigned long request, void *arg) const {
  if (fd_ < 0) {
    errno = ENODEV;
    return -1;
  }
  return ioctl(fd_, request, arg);
}

bool Host::available() const { return fd_ >= 0; }

} // namespace yzctl
