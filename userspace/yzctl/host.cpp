/* SPDX-License-Identifier: Apache-2.0 */
/*
 * YukiZygisk - Reusable standalone kernel control client.
 *
 * License: Apache-2.0
 *
 * Author: Anatdx
 */

#include "host.hpp"

#include "uapi/ksu_control.h"
#include "uapi/yukizygisk.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace yzctl {

namespace {

// Integrated (built-in) path: ask the host KernelSU to install the anonymous
// YukiZygisk control fd. Returns a valid fd or -1 when the kernel is not
// integrated (so the caller can report the prctl error instead).
int install_via_ksu() {
  int ksu_fd = -1;
  errno = 0;
  syscall(SYS_reboot, static_cast<unsigned long>(KSU_INSTALL_MAGIC1),
          static_cast<unsigned long>(KSU_INSTALL_MAGIC2), 0UL,
          reinterpret_cast<unsigned long>(&ksu_fd));
  if (ksu_fd < 0)
    return -1;

  int yz_fd = -1;
  const int ret = ioctl(ksu_fd, KSU_IOCTL_YZ_INSTALL_FD, &yz_fd);
  close(ksu_fd);
  if (ret != 0 || yz_fd < 0)
    return -1;
  return yz_fd;
}

} // namespace

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

  // Integrated (built-in) kernels exclude the LKM bootstrap, so the reusable
  // control-session prctl is not hooked; fall back to the KSU ioctl channel.
  if (delivered_fd < 0)
    delivered_fd = install_via_ksu();

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
