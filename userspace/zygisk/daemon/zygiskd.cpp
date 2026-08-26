/* SPDX-License-Identifier: Apache-2.0 */
/*
 * YukiZygisk - Standalone daemon.
 *
 * License: Apache-2.0
 *
 * Author: Anatdx
 */

#include "zygiskd.hpp"
#if defined(YUKIZYGISK_RUNTIME_LOG)
#include "log.hpp"
#endif
#include "native_modules.hpp"
#include "root_policy.hpp"
#include "uapi/ksu_control.h"
#include "uapi/yukizygisk.h"

#include "json.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/netlink.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits.h>
#include <string>
#include <utility>
#include <vector>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr int kZnApiVersion3 = 3;
constexpr int kZnApiVersion4 = 4;

#if defined(YUKIZYGISK_RUNTIME_LOG)
#define DLOGE(...)                                                             \
  zygiskd::logging::writef(zygiskd::LogLevel::Error,                           \
                           zygiskd::LogSource::Daemon, __VA_ARGS__)
#define DLOGI(...)                                                             \
  zygiskd::logging::writef(zygiskd::LogLevel::Info,                            \
                           zygiskd::LogSource::Daemon, __VA_ARGS__)
#else
[[gnu::format(printf, 1, 2)]] void dlog(const char *fmt, ...) {
  static int kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
  if (kmsg < 0)
    return;

  char buf[256];
  int n = snprintf(buf, sizeof(buf), "<6>zygiskd: ");
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  if (m < 0)
    return;

  size_t len = static_cast<size_t>(n) + static_cast<size_t>(m);
  if (len >= sizeof(buf))
    len = sizeof(buf) - 1;
  ssize_t w = write(kmsg, buf, len);
  (void)w;
}
#define DLOGE(...) dlog(__VA_ARGS__)
#define DLOGI(...) dlog(__VA_ARGS__)
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif // #ifndef MFD_CLOEXEC
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif // #ifndef MFD_ALLOW_SEALING

#if defined(__aarch64__)
constexpr char kAbi[] = "arm64-v8a";
constexpr char kLinkerPath[] = "/system/bin/linker64";
constexpr int kSetDlopenRequest = YZ_IOCTL_SET_DLOPEN;
#elif defined(__arm__)
constexpr char kAbi[] = "armeabi-v7a";
constexpr char kLinkerPath[] = "/system/bin/linker";
constexpr int kSetDlopenRequest = YZ_IOCTL_SET_DLOPEN32;
#elif defined(__x86_64__)
constexpr char kAbi[] = "x86_64";
#elif defined(__i386__)
constexpr char kAbi[] = "x86";
#else
constexpr char kAbi[] = "unknown";
#endif // #if defined(__aarch64__)

namespace yzhost {

constexpr char kDefaultModulesDir[] = "/data/adb/modules";
constexpr char kDefaultConfigPath[] = "/data/adb/yukizygisk/yzconfig.json";
constexpr char kSystemLibContext[] = "u:object_r:system_lib_file:s0";

std::string g_modules_dir = kDefaultModulesDir;
std::string g_config_path = kDefaultConfigPath;
uint64_t g_cookie_lo = 0;
uint64_t g_cookie_hi = 0;
int g_control_fd = -1;
// Set when the control fd was claimed via the integrated (built-in) KSU path
// rather than the LKM bootstrap. In integrated mode the kernel arms no
// bootstrap fail-closed guard, so the daemon-ready handshake is skipped.
bool g_integrated = false;

const std::string &modules_dir() { return g_modules_dir; }

const std::string &config_path() { return g_config_path; }

bool parse_u64(const char *s, uint64_t *out) {
  if (s == nullptr || *s == '\0')
    return false;
  errno = 0;
  char *end = nullptr;
  unsigned long long v = strtoull(s, &end, 0);
  if (errno != 0 || end == s || *end != '\0')
    return false;
  *out = static_cast<uint64_t>(v);
  return true;
}

bool parse_i32(const char *s, int *out) {
  if (s == nullptr || *s == '\0')
    return false;
  errno = 0;
  char *end = nullptr;
  long v = strtol(s, &end, 0);
  if (errno != 0 || end == s || *end != '\0' || v < INT32_MIN || v > INT32_MAX)
    return false;
  *out = static_cast<int>(v);
  return true;
}

void load_env() {
  const char *env = getenv("YUKIZYGISK_CONTROL_FD");
  if (env != nullptr && *env != '\0') {
    int inherited_fd = -1;
    if (parse_i32(env, &inherited_fd) && inherited_fd >= 0 &&
        fcntl(inherited_fd, F_GETFD) >= 0)
      g_control_fd = inherited_fd;
  }

  env = getenv("YUKIZYGISK_MODULES_DIR");
  if (env != nullptr && *env != '\0')
    g_modules_dir = env;

  env = getenv("YUKIZYGISK_CONFIG");
  if (env != nullptr && *env != '\0')
    g_config_path = env;

  env = getenv("YUKIZYGISK_BOOTSTRAP_COOKIE");
  if (env != nullptr && *env != '\0')
    parse_u64(env, &g_cookie_lo);

  env = getenv("YUKIZYGISK_BOOTSTRAP_COOKIE_LO");
  if (env != nullptr && *env != '\0')
    parse_u64(env, &g_cookie_lo);

  env = getenv("YUKIZYGISK_BOOTSTRAP_COOKIE_HI");
  if (env != nullptr && *env != '\0')
    parse_u64(env, &g_cookie_hi);
}

// Integrated (built-in) path: ask the host KernelSU to install the anonymous
// YukiZygisk control fd directly. Returns a valid fd or -1 when the running
// kernel is not integrated (so the caller can fall back to the LKM bootstrap).
int try_claim_via_ksu() {
  int ksu_fd = -1;
  errno = 0;
  syscall(SYS_reboot, static_cast<unsigned long>(KSU_INSTALL_MAGIC1),
          static_cast<unsigned long>(KSU_INSTALL_MAGIC2), 0ul,
          reinterpret_cast<unsigned long>(&ksu_fd));
  if (ksu_fd < 0) {
    DLOGI("ksu install fd unavailable: errno=%d (%s)", errno, strerror(errno));
    return -1;
  }

  int yz_fd = -1;
  errno = 0;
  int ret = ioctl(ksu_fd, KSU_IOCTL_YZ_INSTALL_FD, &yz_fd);
  int e = errno;
  close(ksu_fd);

  if (ret != 0 || yz_fd < 0) {
    DLOGI("KSU_IOCTL_YZ_INSTALL_FD not served: ret=%d errno=%d (%s)", ret, e,
          strerror(e));
    return -1;
  }
  return yz_fd;
}

int try_claim_via_bootstrap() {
  if (g_cookie_lo == 0 && g_cookie_hi == 0) {
    errno = EINVAL;
    DLOGI("no bootstrap cookie provided, cannot use LKM path");
    return -1;
  }

  int fd = -1;
  errno = 0;
  long ret =
      syscall(SYS_prctl, static_cast<unsigned long>(YZ_PRCTL_BOOTSTRAP_OPTION),
              static_cast<unsigned long>(YZ_PRCTL_BOOTSTRAP_MAGIC_YUKIHOOK),
              static_cast<unsigned long>(g_cookie_lo),
              static_cast<unsigned long>(g_cookie_hi),
              reinterpret_cast<unsigned long>(&fd));
  int saved_errno = errno;
  if (fd < 0) {
    DLOGE("bootstrap prctl failed: ret=%ld fd=%d errno=%d (%s)", ret, fd,
          saved_errno, strerror(saved_errno));
    return -1;
  }
  if (ret != 0) {
    DLOGI("bootstrap prctl returned ret=%ld errno=%d (%s), accepting delivered "
          "fd=%d",
          ret, saved_errno, strerror(saved_errno), fd);
  }
  return fd;
}

int claim_control_fd() {
  if (g_control_fd >= 0)
    return g_control_fd;

  // Prefer the integrated (built-in) kernel path. On an LKM kernel the KSU
  // install ioctl is not served and this returns -1, so we fall back to the
  // prctl+cookie bootstrap.
  int fd = try_claim_via_ksu();
  if (fd >= 0) {
    g_control_fd = fd;
    g_integrated = true;
    DLOGI("claimed integrated control fd via KSU");
    return g_control_fd;
  }
  DLOGI("integrated path unavailable, falling back to LKM bootstrap");

  fd = try_claim_via_bootstrap();
  if (fd >= 0) {
    g_control_fd = fd;
    DLOGI("claimed anonymous control fd via bootstrap");
    return g_control_fd;
  }

  DLOGE("failed to claim control fd (both integrated and bootstrap paths)");
  return -1;
}

int ctl(int request, void *arg) {
  if (g_control_fd < 0) {
    errno = ENODEV;
    return -1;
  }
  return ioctl(g_control_fd, request, arg);
}

bool get_root_status(yz_root_status_cmd *status) {
  if (status == nullptr)
    return false;
  *status = {};
  return ctl(YZ_IOCTL_GET_ROOT_STATUS, status) == 0;
}

bool uid_should_umount(uint32_t uid) {
  if (yzpolicy::active())
    (void)yzpolicy::refresh(false);
  yz_uid_policy_cmd cmd{};
  cmd.uid = uid;
  if (ctl(YZ_IOCTL_UID_SHOULD_UMOUNT, &cmd) == 0)
    return cmd.should_umount != 0;

  int saved_errno = errno;
  bool decision = false;
  if (saved_errno == EAGAIN && yzpolicy::query_uid(uid, &decision))
    return decision;
  DLOGE("UID policy unavailable uid=%u errno=%d (%s)", uid, saved_errno,
        strerror(saved_errno));
  return false;
}

bool lsetfilecon(const std::string &path, const char *context) {
  if (context == nullptr || *context == '\0')
    return false;
  return lsetxattr(path.c_str(), "security.selinux", context,
                   strlen(context) + 1, 0) == 0;
}

} // namespace yzhost

struct Module {
  std::string name;
  std::string lib_path; // <id>/zygisk/<abi>.so
};

std::vector<Module> g_modules;

using NativeModule = yukizygisk::native::NativeModule;

std::vector<NativeModule> g_native_modules;
std::vector<NativeModule> g_native_targets;

int consume_ready_fd() {
  const char *env = getenv("YUKIZYGISK_READY_FD");
  if (env == nullptr || *env == '\0')
    return -1;

  errno = 0;
  char *end = nullptr;
  long fd = strtol(env, &end, 10);
  unsetenv("YUKIZYGISK_READY_FD");
  if (errno || end == env || *end != '\0' || fd < 0 || fd > INT32_MAX)
    return -1;
  return static_cast<int>(fd);
}

void notify_ready(int fd, bool ok) {
  if (fd < 0)
    return;
  const char byte = ok ? '1' : '0';
  ssize_t w;
  do {
    w = write(fd, &byte, 1);
  } while (w < 0 && errno == EINTR);
  (void)w;
  close(fd);
}

#if defined(__LP64__)
void stop_compat_daemon(pid_t pid) {
  if (pid <= 0)
    return;
  (void)kill(pid, SIGKILL);
  while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
  }
}

pid_t spawn_compat_daemon() {
  char exe_path[PATH_MAX];
  ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (exe_len <= 0)
    return -1;
  exe_path[exe_len] = '\0';
  char *leaf = strrchr(exe_path, '/');
  if (leaf == nullptr ||
      static_cast<size_t>(leaf - exe_path) + sizeof("/zygiskd32") >
          sizeof(exe_path))
    return -1;
  memcpy(leaf, "/zygiskd32", sizeof("/zygiskd32"));

  int ready[2];
  if (pipe2(ready, O_CLOEXEC) != 0)
    return -1;
  pid_t pid = fork();
  if (pid == 0) {
    close(ready[0]);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() == 1)
      _exit(127);
    int control_fd = fcntl(yzhost::g_control_fd, F_DUPFD, 3);
    if (control_fd < 0 || fcntl(control_fd, F_SETFD, 0) != 0 ||
        fcntl(ready[1], F_SETFD, 0) != 0)
      _exit(127);
    char control_text[16];
    char ready_text[16];
    snprintf(control_text, sizeof(control_text), "%d", control_fd);
    snprintf(ready_text, sizeof(ready_text), "%d", ready[1]);
    setenv("YUKIZYGISK_CONTROL_FD", control_text, 1);
    setenv("YUKIZYGISK_READY_FD", ready_text, 1);
    setenv("YUKIZYGISK_MODULES_DIR", yzhost::modules_dir().c_str(), 1);
    setenv("YUKIZYGISK_CONFIG", yzhost::config_path().c_str(), 1);
    execl(exe_path, exe_path, static_cast<char *>(nullptr));
    _exit(127);
  }
  close(ready[1]);
  if (pid < 0) {
    close(ready[0]);
    return -1;
  }

  pollfd pfd{ready[0], POLLIN, 0};
  char result = '0';
  int poll_result;
  do {
    poll_result = poll(&pfd, 1, 5000);
  } while (poll_result < 0 && errno == EINTR);
  ssize_t received = -1;
  if (poll_result > 0 && (pfd.revents & POLLIN)) {
    do {
      received = read(ready[0], &result, 1);
    } while (received < 0 && errno == EINTR);
  }
  bool ok = received == 1 && result == '1';
  close(ready[0]);
  if (!ok) {
    stop_compat_daemon(pid);
    pid = -1;
  }
  DLOGI("zygiskd32 spawn pid=%d ready=%u", pid, ok ? 1U : 0U);
  return pid;
}
#endif // #if defined(__LP64__)

/* Enabled zygisk modules for this ABI. */
std::vector<Module> scan_modules() {
  std::vector<Module> mods;
  DIR *d = opendir(yzhost::modules_dir().c_str());
  if (d == nullptr)
    return mods;

  while (dirent *e = readdir(d)) {
    if (e->d_name[0] == '.')
      continue;
    std::string base = yzhost::modules_dir() + "/" + e->d_name;
    if (access((base + "/disable").c_str(), F_OK) == 0 ||
        access((base + "/remove").c_str(), F_OK) == 0)
      continue;
    std::string lib = base + "/zygisk/" + kAbi + ".so";
    if (access(lib.c_str(), F_OK) != 0)
      continue;
    mods.push_back(Module{e->d_name, std::move(lib)});
  }
  closedir(d);
  return mods;
}

bool read_text_file(const std::string &path, std::string *out) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;

  out->clear();
  char buffer[64 * 1024];
  bool ok = true;
  for (;;) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      out->append(buffer, static_cast<size_t>(count));
      continue;
    }
    if (count == 0)
      break;
    if (errno == EINTR)
      continue;
    ok = false;
    break;
  }
  close(fd);
  if (!ok)
    out->clear();
  return ok;
}

template <typename Fn>
void for_each_manifest_line(const std::string &text, const Fn &fn) {
  size_t begin = 0;
  while (begin < text.size()) {
    size_t end = text.find('\n', begin);
    const bool last = end == std::string::npos;
    if (last)
      end = text.size();
    fn(text.substr(begin, end - begin));
    begin = last ? text.size() : end + 1;
  }
}

std::vector<NativeModule> scan_native_modules() {
  std::vector<NativeModule> mods;
  DIR *d = opendir(yzhost::modules_dir().c_str());
  if (d == nullptr)
    return mods;

  while (dirent *e = readdir(d)) {
    if (e->d_name[0] == '.')
      continue;
    std::string module_id = e->d_name;
    std::string base = yzhost::modules_dir() + "/" + module_id;
    if (access((base + "/disable").c_str(), F_OK) == 0 ||
        access((base + "/remove").c_str(), F_OK) == 0)
      continue;

    std::string manifest;
    if (!read_text_file(base + "/zn_modules.txt", &manifest))
      continue;
    for_each_manifest_line(manifest, [&](const std::string &line) {
      NativeModule m{};
      if (yukizygisk::native::parse_native_module_line(module_id, base, line,
                                                       &m)) {
        if (!yzhost::lsetfilecon(m.lib_path, yzhost::kSystemLibContext))
          DLOGE("native module: failed to label lib=%s", m.lib_path.c_str());
        DLOGI("native module: id=%s target=%s%s lib=%s companion=%u",
              m.module_id.c_str(),
              m.target_type == YZ_NATIVE_TARGET_PATH ? "path=" : "name=",
              m.target.c_str(), m.lib_path.c_str(), m.has_companion ? 1 : 0);
        mods.push_back(std::move(m));
      } else if (!yukizygisk::native::trim_copy(line).empty() &&
                 yukizygisk::native::trim_copy(line)[0] != '#') {
        DLOGI("native module: ignored invalid line in %s: %s",
              module_id.c_str(), yukizygisk::native::trim_copy(line).c_str());
      }
    });
  }
  closedir(d);
  return mods;
}

int native_module_elf_class(const std::string &path) {
  unsigned char ident[EI_NIDENT]{};
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return ELFCLASSNONE;
  ssize_t n = read(fd, ident, sizeof(ident));
  close(fd);
  if (n != static_cast<ssize_t>(sizeof(ident)) ||
      memcmp(ident, ELFMAG, SELFMAG) != 0 || ident[EI_DATA] != ELFDATA2LSB)
    return ELFCLASSNONE;
  return ident[EI_CLASS];
}

#if defined(__LP64__)
bool needs_compat_daemon() {
  char zygote[PROP_VALUE_MAX]{};
  if (__system_property_get("ro.zygote", zygote) > 0 &&
      strstr(zygote, "32") != nullptr)
    return true;

  return std::any_of(g_native_targets.begin(), g_native_targets.end(),
                     [](const NativeModule &module) {
                       return native_module_elf_class(module.lib_path) ==
                              ELFCLASS32;
                     });
}
#endif // #if defined(__LP64__)

#if defined(__LP64__)
void publish_native_targets() {
  yz_native_targets_cmd cmd{};
  for (const auto &m : g_native_targets) {
    if (cmd.count >= YZ_NATIVE_TARGET_MAX)
      break;
    bool duplicate = false;
    for (uint32_t i = 0; i < cmd.count; ++i) {
      if (cmd.targets[i].type == m.target_type &&
          strcmp(cmd.targets[i].value, m.target.c_str()) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate)
      continue;
    yz_native_target &t = cmd.targets[cmd.count++];
    t.type = m.target_type;
    snprintf(t.value, sizeof(t.value), "%s", m.target.c_str());
  }
  int ret = yzhost::ctl(YZ_IOCTL_SET_NATIVE_TARGETS, &cmd);
  if (ret == 0) {
    DLOGI("native targets: %u module(s), kernel ret=0", cmd.count);
  } else {
    DLOGI("native targets: %u module(s), kernel ret=%d errno=%d (%s)",
          cmd.count, ret, errno, strerror(errno));
  }
}
#endif // #if defined(__LP64__)

void rescan_modules() {
  g_modules = scan_modules();
  std::vector<NativeModule> scanned = scan_native_modules();
  g_native_targets.clear();
  g_native_modules.clear();
  constexpr int kElfClass = sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32;
  for (const auto &module : scanned) {
    int module_class = native_module_elf_class(module.lib_path);
    if (module_class != ELFCLASS32 && module_class != ELFCLASS64)
      continue;
    g_native_targets.push_back(module);
    if (module_class == kElfClass)
      g_native_modules.push_back(module);
  }
#if defined(__LP64__)
  publish_native_targets();
#endif // #if defined(__LP64__)
  DLOGI("found %zu zygisk module(s), %zu native module(s) for %s",
        g_modules.size(), g_native_modules.size(), kAbi);
}

bool read_exact(int fd, void *buf, size_t n) {
  auto *p = static_cast<uint8_t *>(buf);
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r <= 0)
      return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

#if defined(YUKIZYGISK_RUNTIME_LOG)
class ClientReader {
public:
  explicit ClientReader(int fd)
      : fd_(fd), deadline_(Clock::now() + std::chrono::seconds(2)) {}

  bool read_exact(void *buffer, size_t size) const {
    auto *data = static_cast<uint8_t *>(buffer);
    while (size > 0) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ -
                                                                Clock::now());
      if (remaining.count() <= 0)
        return false;

      pollfd pfd{fd_, POLLIN, 0};
      const int timeout =
          static_cast<int>(std::max<int64_t>(1, remaining.count()));
      const int ready = poll(&pfd, 1, timeout);
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready <= 0 || (pfd.revents & POLLIN) == 0)
        return false;

      const ssize_t received = recv(fd_, data, size, MSG_DONTWAIT);
      if (received < 0 && (errno == EINTR || errno == EAGAIN))
        continue;
      if (received <= 0)
        return false;
      data += received;
      size -= static_cast<size_t>(received);
    }
    return true;
  }

private:
  using Clock = std::chrono::steady_clock;

  int fd_;
  Clock::time_point deadline_;
};
#endif

bool write_exact(int fd, const void *buf, size_t n) {
  const auto *p = static_cast<const uint8_t *>(buf);
  while (n > 0) {
    ssize_t r = write(fd, p, n);
    if (r <= 0)
      return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

/* Send one fd via SCM_RIGHTS. */
bool send_fd(int sock, int fd) {
  msghdr msg{};
  iovec io{};
  char dummy = '!';
  io.iov_base = &dummy;
  io.iov_len = 1;
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;

  char cbuf[CMSG_SPACE(sizeof(int))] = {};
  if (fd >= 0) {
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
  }
  return sendmsg(sock, &msg, MSG_NOSIGNAL) >
         0; // EPIPE not SIGPIPE on dead client
}

int copy_file_to_memfd(const std::string &path) {
  int src = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (src < 0) {
    DLOGE("module memfd: open failed path=%s err=%s", path.c_str(),
          strerror(errno));
    return -1;
  }

  struct stat st{};
  if (fstat(src, &st) != 0 || st.st_size <= 0 || !S_ISREG(st.st_mode)) {
    DLOGE("module memfd: invalid source path=%s err=%s", path.c_str(),
          strerror(errno));
    close(src);
    return -1;
  }

  int mfd = static_cast<int>(
      syscall(__NR_memfd_create, "", MFD_CLOEXEC | MFD_ALLOW_SEALING));
  if (mfd < 0) {
    DLOGE("module memfd: memfd_create failed path=%s err=%s", path.c_str(),
          strerror(errno));
    close(src);
    return -1;
  }

  if (ftruncate(mfd, st.st_size) != 0) {
    DLOGE("module memfd: ftruncate failed size=%lld err=%s",
          static_cast<long long>(st.st_size), strerror(errno));
    close(mfd);
    close(src);
    return -1;
  }

  std::vector<uint8_t> buf(64 * 1024);
  while (true) {
    ssize_t r = read(src, buf.data(), buf.size());
    if (r == 0)
      break;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      DLOGE("module memfd: read failed path=%s err=%s", path.c_str(),
            strerror(errno));
      close(mfd);
      close(src);
      return -1;
    }

    const uint8_t *p = buf.data();
    size_t left = static_cast<size_t>(r);
    while (left > 0) {
      ssize_t w = write(mfd, p, left);
      if (w < 0) {
        if (errno == EINTR)
          continue;
        DLOGE("module memfd: write failed path=%s err=%s", path.c_str(),
              strerror(errno));
        close(mfd);
        close(src);
        return -1;
      }
      if (w == 0) {
        DLOGE("module memfd: short write path=%s", path.c_str());
        close(mfd);
        close(src);
        return -1;
      }
      p += w;
      left -= static_cast<size_t>(w);
    }
  }

  close(src);
  if (lseek(mfd, 0, SEEK_SET) < 0) {
    DLOGE("module memfd: rewind failed err=%s", strerror(errno));
    close(mfd);
    return -1;
  }

  constexpr int kModuleSeals =
      F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL;
  if (fcntl(mfd, F_ADD_SEALS, kModuleSeals) != 0) {
    DLOGE("module memfd: seal failed path=%s err=%s", path.c_str(),
          strerror(errno));
    close(mfd);
    return -1;
  }

  // memfd_create() always returns an O_RDWR file description. SCM_RIGHTS
  // checks permissions from that description when the zygote receives it,
  // so even a sealed image would unnecessarily require tmpfs:file write.
  // Reopen the sealed inode read-only and expose only that description.
  char proc_fd[64];
  snprintf(proc_fd, sizeof(proc_fd), "/proc/self/fd/%d", mfd);
  int ro_fd = open(proc_fd, O_RDONLY | O_CLOEXEC);
  if (ro_fd < 0) {
    DLOGE("module memfd: reopen read-only failed path=%s err=%s", path.c_str(),
          strerror(errno));
    close(mfd);
    return -1;
  }
  close(mfd);

  DLOGI("module memfd: staged size=%lld fd=%d",
        static_cast<long long>(st.st_size), ro_fd);
  return ro_fd;
}

/* Receive one fd via SCM_RIGHTS. */
int recv_fd(int sock) {
  char data = 0;
  char cbuf[CMSG_SPACE(sizeof(int))] = {};
  iovec io{&data, 1};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = cbuf;
  msg.msg_controllen = sizeof(cbuf);
  if (recvmsg(sock, &msg, 0) <= 0)
    return -1;
  for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int fd = -1;
      memcpy(&fd, CMSG_DATA(c), sizeof(fd));
      return fd;
    }
  return -1;
}

using companion_entry_fn = void (*)(int);

struct CompanionJob {
  companion_entry_fn fn;
  int client;
};

void *companion_thread(void *p) {
  auto *job = static_cast<CompanionJob *>(p);
  job->fn(job->client);
  close(job->client);
  delete job;
  return nullptr;
}

[[noreturn]] void companion_main(const std::string &lib_path, int ctrl) {
  // Drop daemon fds.
  if (DIR *fdd = opendir("/proc/self/fd")) {
    int dfd = dirfd(fdd);
    while (dirent *e = readdir(fdd)) {
      int fd = atoi(e->d_name);
      if (fd > 2 && fd != ctrl && fd != dfd)
        close(fd);
    }
    closedir(fdd);
  }
  void *h = dlopen(lib_path.c_str(), RTLD_NOW);
  auto fn = h ? reinterpret_cast<companion_entry_fn>(
                    dlsym(h, "zygisk_companion_entry"))
              : nullptr;
  uint8_t ready = fn != nullptr ? 1 : 0;
  if (write(ctrl, &ready, 1) != 1)
    _exit(0);
  for (;;) {
    int client = recv_fd(ctrl);
    if (client < 0)
      _exit(0); // daemon gone
    if (fn == nullptr) {
      close(client);
      continue;
    }
    auto *job = new CompanionJob{fn, client};
    pthread_t t;
    if (pthread_create(&t, nullptr, companion_thread, job) == 0)
      pthread_detach(t);
    else {
      close(client);
      delete job;
    }
  }
}

struct Companion {
  pid_t pid = -1;
  int ctrl = -1;
  bool has_entry = false;
};
std::vector<Companion> g_companions; // indexed like g_modules, spawned lazily
constexpr int kCompanionReadyMs = 5000; // bound on a companion's startup

bool ensure_companion(uint32_t idx) {
  if (idx >= g_modules.size())
    return false;
  if (g_companions.size() != g_modules.size())
    g_companions.resize(g_modules.size());
  Companion &c = g_companions[idx];
  if (c.pid > 0)
    return c.has_entry;

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
    return false;
  pid_t pid = fork();
  if (pid < 0) {
    close(sv[0]);
    close(sv[1]);
    return false;
  }
  if (pid == 0) {
    close(sv[0]);
    companion_main(g_modules[idx].lib_path, sv[1]); // never returns
  }
  close(sv[1]);

  pollfd pfd{sv[0], POLLIN, 0};
  uint8_t ready = 0;
  if (poll(&pfd, 1, kCompanionReadyMs) != 1 || !(pfd.revents & POLLIN) ||
      !read_exact(sv[0], &ready, 1)) {
    DLOGE("companion for '%s' pid=%d not ready in %dms; killing",
          g_modules[idx].name.c_str(), pid, kCompanionReadyMs);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    close(sv[0]);
    return false;
  }

  c.pid = pid;
  c.ctrl = sv[0];
  c.has_entry = (ready == 1);
  DLOGI("companion for '%s' pid=%d entry=%d", g_modules[idx].name.c_str(), pid,
        c.has_entry);
  return c.has_entry;
}

struct ZygiskNextCompanionModule {
  int target_api_version;
  void (*onCompanionLoaded)();
  void (*onModuleConnected)(int fd);
};

struct NativeCompanionJob {
  void (*fn)(int);
  int client;
};

void *native_companion_thread(void *p) {
  auto *job = static_cast<NativeCompanionJob *>(p);
  job->fn(job->client);
  delete job;
  return nullptr;
}

[[noreturn]] void native_companion_main(const std::string &lib_path, int ctrl) {
  if (DIR *fdd = opendir("/proc/self/fd")) {
    int dfd = dirfd(fdd);
    while (dirent *e = readdir(fdd)) {
      int fd = atoi(e->d_name);
      if (fd > 2 && fd != ctrl && fd != dfd)
        close(fd);
    }
    closedir(fdd);
  }

  void *h = dlopen(lib_path.c_str(), RTLD_NOW);
  auto *mod = h ? reinterpret_cast<ZygiskNextCompanionModule *>(
                      dlsym(h, "zn_companion_module"))
                : nullptr;
  bool valid = mod != nullptr && (mod->target_api_version == kZnApiVersion3 ||
                                  mod->target_api_version == kZnApiVersion4);
  if (valid && mod->onCompanionLoaded != nullptr)
    mod->onCompanionLoaded();

  uint8_t ready = valid && mod->onModuleConnected != nullptr ? 1 : 0;
  if (write(ctrl, &ready, 1) != 1)
    _exit(0);
  for (;;) {
    int client = recv_fd(ctrl);
    if (client < 0)
      _exit(0);
    if (!ready) {
      close(client);
      continue;
    }
    auto *job = new NativeCompanionJob{mod->onModuleConnected, client};
    pthread_t t;
    if (pthread_create(&t, nullptr, native_companion_thread, job) == 0)
      pthread_detach(t);
    else {
      close(client);
      delete job;
    }
  }
}

std::vector<Companion> g_native_companions;

bool ensure_native_companion(uint32_t idx) {
  if (idx >= g_native_modules.size() || !g_native_modules[idx].has_companion)
    return false;
  if (g_native_companions.size() != g_native_modules.size())
    g_native_companions.resize(g_native_modules.size());
  Companion &c = g_native_companions[idx];
  if (c.pid > 0)
    return c.has_entry;

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
    return false;
  pid_t pid = fork();
  if (pid < 0) {
    close(sv[0]);
    close(sv[1]);
    return false;
  }
  if (pid == 0) {
    close(sv[0]);
    native_companion_main(g_native_modules[idx].lib_path, sv[1]);
  }
  close(sv[1]);

  pollfd pfd{sv[0], POLLIN, 0};
  uint8_t ready = 0;
  if (poll(&pfd, 1, kCompanionReadyMs) != 1 || !(pfd.revents & POLLIN) ||
      !read_exact(sv[0], &ready, 1)) {
    DLOGE("native companion for '%s' pid=%d not ready in %dms; killing",
          g_native_modules[idx].module_id.c_str(), pid, kCompanionReadyMs);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    close(sv[0]);
    return false;
  }

  c.pid = pid;
  c.ctrl = sv[0];
  c.has_entry = (ready == 1);
  DLOGI("native companion for '%s' pid=%d entry=%d",
        g_native_modules[idx].module_id.c_str(), pid, c.has_entry);
  return c.has_entry;
}

yz_config g_yz_config{1, 0, 0, 0};

uint32_t query_flags(uint32_t uid) {
  uint32_t flags = 0;
  if (g_yz_config.denylist_mode != 0 && yzhost::uid_should_umount(uid))
    flags |= 1u << 1;
  return flags;
}

void read_yzconfig() {
  yz_config cfg{1, 0, 0, 0};
  int fd = open(yzhost::config_path().c_str(), O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    std::string buf;
    char tmp[1024];
    for (ssize_t n; (n = read(fd, tmp, sizeof(tmp))) > 0;)
      buf.append(tmp, static_cast<size_t>(n));
    close(fd);
    json::Value root = json::parse(buf);
    if (root.type == json::Type::Object) {
      if (root.contains("yukilinker"))
        cfg.yukilinker = root.at("yukilinker").as_bool() ? 1 : 0;
      if (root.contains("denylist_mode")) {
        double mode = root.at("denylist_mode").as_number();
        if (mode == 0 || mode == 1 || mode == 2)
          cfg.denylist_mode = static_cast<__u8>(mode);
      }
      if (root.contains("dmesg_log"))
        cfg.dmesg_log = root.at("dmesg_log").as_bool() ? 1 : 0;
    }
  }
  g_yz_config = cfg;
#if defined(YUKIZYGISK_RUNTIME_LOG)
  zygiskd::logging::set_kernel_mirror(cfg.dmesg_log != 0);
#endif
  yz_yukilinker_cmd yc{};
  yc.enabled = cfg.yukilinker;
  yzhost::ctl(YZ_IOCTL_SET_YUKILINKER, &yc);
  DLOGI("yzconfig: yukilinker=%u denylist_mode=%u dmesg_log=%u", cfg.yukilinker,
        cfg.denylist_mode, cfg.dmesg_log);
}

#if defined(__LP64__)
constexpr uint8_t kRuntimeAbi = YZ_RUNTIME_ABI_64;
#else
constexpr uint8_t kRuntimeAbi = YZ_RUNTIME_ABI_32;
#endif

struct RuntimeSnapshot {
  std::vector<yz_runtime_record> records;
};

RuntimeSnapshot query_runtime_snapshot() {
  RuntimeSnapshot snapshot;
  snapshot.records.resize(YZ_RUNTIME_RECORD_MAX);

  yz_runtime_query_cmd cmd{};
  cmd.capacity = static_cast<uint32_t>(snapshot.records.size());
  cmd.entries = static_cast<__aligned_u64>(
      reinterpret_cast<uintptr_t>(snapshot.records.data()));
  if (yzhost::ctl(YZ_IOCTL_GET_RUNTIME, &cmd) != 0) {
    snapshot.records.clear();
    return snapshot;
  }

  if (cmd.count < snapshot.records.size())
    snapshot.records.resize(cmd.count);
  return snapshot;
}

bool report_runtime(pid_t pid, uint8_t kind, uint32_t generation,
                    const char *module_id = nullptr) {
  if (pid <= 0 || generation == 0)
    return false;

  yz_runtime_report_cmd cmd{};
  cmd.pid = static_cast<uint32_t>(pid);
  cmd.generation = generation;
  cmd.kind = kind;
  if (module_id != nullptr)
    snprintf(cmd.module_id, sizeof(cmd.module_id), "%s", module_id);
  return yzhost::ctl(YZ_IOCTL_REPORT_RUNTIME, &cmd) == 0;
}

uint32_t runtime_generation(pid_t pid, uint8_t kind) {
  if (pid <= 0 ||
      (kind != YZ_RUNTIME_KIND_ZYGOTE && kind != YZ_RUNTIME_KIND_NATIVE))
    return 0;

  const RuntimeSnapshot snapshot = query_runtime_snapshot();
  uint32_t generation = 0;
  for (const auto &record : snapshot.records) {
    if (record.pid != static_cast<uint32_t>(pid) || record.kind != kind ||
        record.abi != kRuntimeAbi || record.module_id[0] != '\0' ||
        (record.state != YZ_RUNTIME_STATE_REDIRECTED &&
         record.state != YZ_RUNTIME_STATE_INJECTED))
      continue;
    generation = std::max(generation, record.generation);
  }
  return generation;
}
void handle_client(int client) {
#if defined(YUKIZYGISK_RUNTIME_LOG)
  const ClientReader reader(client);
  const auto read_client = [&reader](void *buffer, size_t size) {
    return reader.read_exact(buffer, size);
  };
#else
  const auto read_client = [client](void *buffer, size_t size) {
    return read_exact(client, buffer, size);
  };
#endif
  uint8_t op = 0;
  if (!read_client(&op, sizeof(op)))
    return;

  switch (static_cast<zygiskd::Request>(op)) {
  case zygiskd::Request::GetModuleCount: {
    uint32_t n = static_cast<uint32_t>(g_modules.size());
    write_exact(client, &n, sizeof(n));
    break;
  }
  case zygiskd::Request::GetModuleFd: {
    uint32_t idx = 0;
    if (!read_client(&idx, sizeof(idx)) || idx >= g_modules.size()) {
      send_fd(client, -1);
      break;
    }
    // Never expose the source module inode to zygote. Besides preserving
    // anonymous loading, this avoids an SCM_RIGHTS SELinux check against a
    // module that was installed with adb_data_file context.
    int fd = copy_file_to_memfd(g_modules[idx].lib_path);
    send_fd(client, fd);
    if (fd >= 0)
      close(fd);
    break;
  }
  case zygiskd::Request::ConnectCompanion: {
    uint32_t idx = 0;
    if (!read_client(&idx, sizeof(idx)) || !ensure_companion(idx)) {
      send_fd(client, -1);
      break;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
      send_fd(client, -1);
      break;
    }
    // companion services sv[1] on a thread; caller talks over sv[0]
    if (!send_fd(g_companions[idx].ctrl, sv[1])) {
      close(sv[0]);
      close(sv[1]);
      send_fd(client, -1);
      break;
    }
    close(sv[1]);
    send_fd(client, sv[0]);
    close(sv[0]);
    break;
  }
  case zygiskd::Request::GetModuleDir: {
    uint32_t idx = 0;
    if (!read_client(&idx, sizeof(idx)) || idx >= g_modules.size()) {
      send_fd(client, -1);
      break;
    }
    std::string dir = yzhost::modules_dir() + "/" + g_modules[idx].name;
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    bool policy_armed = false;
    if (fd >= 0 &&
        getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0) {
      yz_module_load_policy_cmd cmd{};
      cmd.pid = static_cast<uint32_t>(cr.pid);
      cmd.dirfd = fd;
      int ret = yzhost::ctl(YZ_IOCTL_ALLOW_MODULE_LOAD_POLICY, &cmd);
      policy_armed = ret == 0;
      DLOGI("module dir policy: module=%s pid=%d ret=%d",
            g_modules[idx].name.c_str(), cr.pid, ret);
      if (!policy_armed) {
        close(fd);
        fd = -1;
      }
    } else if (fd >= 0) {
      close(fd);
      fd = -1;
    }
    bool sent = send_fd(client, fd);
    if (fd >= 0)
      close(fd);
    if (!sent && policy_armed) {
      yz_native_load_policy_cmd cmd{};
      cmd.pid = static_cast<uint32_t>(cr.pid);
      (void)yzhost::ctl(YZ_IOCTL_RESTORE_NATIVE_LOAD_POLICY, &cmd);
    }
    break;
  }
  case zygiskd::Request::GetProcessFlags: {
    uint32_t uid = 0;
    if (!read_client(&uid, sizeof(uid)))
      break;
    uint32_t flags = query_flags(uid);
    write_exact(client, &flags, sizeof(flags));
    break;
  }
  case zygiskd::Request::GetConfig: {
    write_exact(client, &g_yz_config, sizeof(g_yz_config));
    break;
  }
  case zygiskd::Request::RevertMount: {
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint8_t ok = 0;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0) {
      yz_umount_pid_cmd cmd{};
      cmd.pid = static_cast<uint32_t>(cr.pid);
      ok = yzhost::ctl(YZ_IOCTL_UMOUNT_PID, &cmd) == 0 ? 1 : 0;
    }
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  case zygiskd::Request::SelfDestruct: {
    uint8_t n = 0;
    if (!read_client(&n, sizeof(n)) || n == 0 || n > YZ_MAX_UNMAP_SEGS)
      break;
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint8_t ok = 0;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0) {
      yz_unmap_pid_cmd ucmd{};
      ucmd.pid = static_cast<uint32_t>(cr.pid);
      ucmd.n_segs = n;
      bool good = true;
      for (uint8_t i = 0; i < n; ++i)
        if (!read_client(&ucmd.addr[i], sizeof(ucmd.addr[i])) ||
            !read_client(&ucmd.size[i], sizeof(ucmd.size[i]))) {
          good = false;
          break;
        }
      if (good) {
        yz_umount_pid_cmd mcmd{};
        mcmd.pid = ucmd.pid;
        ok = yzhost::ctl(YZ_IOCTL_UMOUNT_PID, &mcmd) == 0 ? 1 : 0;
      }
    }
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  case zygiskd::Request::PatchText: {
    uint64_t addr = 0;
    uint32_t len = 0;
    if (!read_client(&addr, sizeof(addr)) || !read_client(&len, sizeof(len)) ||
        len == 0 || len > YZ_PATCH_TEXT_MAX)
      break;
    uint8_t bytes[YZ_PATCH_TEXT_MAX];
    if (!read_client(bytes, len))
      break;
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint8_t ok = 0;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0) {
      yz_patch_text_cmd pcmd{};
      pcmd.pid = static_cast<uint32_t>(cr.pid);
      pcmd.len = len;
      pcmd.addr = addr;
      memcpy(pcmd.bytes, bytes, len);
      ok = yzhost::ctl(YZ_IOCTL_PATCH_TEXT, &pcmd) == 0 ? 1 : 0;
    }
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  case zygiskd::Request::Log: {
    uint16_t len = 0;
    if (!read_client(&len, sizeof(len)) || len == 0 || len > 256)
      break;
    char buf[257];
    if (!read_client(buf, len))
      break;
    buf[len] = '\0';
#if defined(YUKIZYGISK_RUNTIME_LOG)
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0)
      zygiskd::logging::write(zygiskd::LogLevel::Info,
                              zygiskd::LogSource::Zygisk, cr.pid, cr.uid, buf);
#else
    dlog("core: %s", buf);
#endif
    break;
  }
#if defined(YUKIZYGISK_RUNTIME_LOG)
  case zygiskd::Request::WriteLog: {
    zygiskd::LogHeader header{};
    if (!read_client(&header, sizeof(header)) || header.length == 0 ||
        header.length > zygiskd::kLogMessageMax ||
        static_cast<uint8_t>(header.level) >
            static_cast<uint8_t>(zygiskd::LogLevel::Error) ||
        static_cast<uint8_t>(header.source) <
            static_cast<uint8_t>(zygiskd::LogSource::Zygisk) ||
        static_cast<uint8_t>(header.source) >
            static_cast<uint8_t>(zygiskd::LogSource::Linker))
      break;
    char buf[zygiskd::kLogMessageMax + 1];
    if (!read_client(buf, header.length))
      break;
    buf[header.length] = '\0';
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0)
      zygiskd::logging::write(header.level, header.source, cr.pid, cr.uid, buf);
    break;
  }
#endif
  case zygiskd::Request::ReportZygote: {
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint32_t generation = 0;
    uint8_t ok = 0;
    if (read_client(&generation, sizeof(generation)) &&
        getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0)
      ok = report_runtime(cr.pid, YZ_RUNTIME_KIND_ZYGOTE, generation) ? 1 : 0;
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  case zygiskd::Request::GetRuntimeGeneration: {
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint8_t kind = 0;
    uint32_t generation = 0;
    if (read_client(&kind, sizeof(kind)) &&
        getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0)
      generation = runtime_generation(cr.pid, kind);
    write_exact(client, &generation, sizeof(generation));
    break;
  }
  case zygiskd::Request::GetNativeModuleCount: {
    uint32_t n = static_cast<uint32_t>(g_native_modules.size());
    write_exact(client, &n, sizeof(n));
    break;
  }
  case zygiskd::Request::GetNativeModuleInfo: {
    uint32_t idx = 0;
    zygiskd::NativeModuleInfo info{};
    if (read_client(&idx, sizeof(idx)) && idx < g_native_modules.size()) {
      const NativeModule &m = g_native_modules[idx];
      info.target_type = m.target_type;
      info.has_companion = m.has_companion ? 1 : 0;
      snprintf(info.module_id, sizeof(info.module_id), "%s",
               m.module_id.c_str());
      snprintf(info.target, sizeof(info.target), "%s", m.target.c_str());
      snprintf(info.lib_path, sizeof(info.lib_path), "%s", m.lib_path.c_str());
    }
    write_exact(client, &info, sizeof(info));
    break;
  }
  case zygiskd::Request::GetNativeModuleFd: {
    uint32_t idx = 0;
    if (!read_client(&idx, sizeof(idx)) || idx >= g_native_modules.size()) {
      send_fd(client, -1);
      break;
    }
    const std::string &path = g_native_modules[idx].lib_path;
    int fd = copy_file_to_memfd(path);
    send_fd(client, fd);
    if (fd >= 0)
      close(fd);
    break;
  }
  case zygiskd::Request::ConnectNativeCompanion: {
    uint32_t idx = 0;
    if (!read_client(&idx, sizeof(idx)) || !ensure_native_companion(idx)) {
      send_fd(client, -1);
      break;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
      send_fd(client, -1);
      break;
    }
    if (!send_fd(g_native_companions[idx].ctrl, sv[1])) {
      close(sv[0]);
      close(sv[1]);
      send_fd(client, -1);
      break;
    }
    close(sv[1]);
    send_fd(client, sv[0]);
    close(sv[0]);
    break;
  }
  case zygiskd::Request::RestoreNativeLoadPolicy: {
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint8_t ok = 0;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0) {
      yz_native_load_policy_cmd cmd{};
      cmd.pid = static_cast<uint32_t>(cr.pid);
      int ret = yzhost::ctl(YZ_IOCTL_RESTORE_NATIVE_LOAD_POLICY, &cmd);
      DLOGI("load policy restore: pid=%d ret=%d", cr.pid, ret);
      ok = ret == 0 ? 1 : 0;
    }
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  case zygiskd::Request::ReportNativeInjection: {
    struct ucred cr{};
    socklen_t crlen = sizeof(cr);
    uint32_t idx = 0;
    uint32_t generation = 0;
    uint8_t ok = 0;
    if (read_client(&idx, sizeof(idx)) &&
        read_client(&generation, sizeof(generation)) &&
        getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 &&
        cr.pid > 0 && idx < g_native_modules.size())
      ok = report_runtime(cr.pid, YZ_RUNTIME_KIND_NATIVE, generation,
                          g_native_modules[idx].module_id.c_str())
               ? 1
               : 0;
    write_exact(client, &ok, sizeof(ok));
    break;
  }
  default:
    break;
  }
}

int bind_listen() {
  int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (srv < 0) {
    DLOGE("socket failed: %s", strerror(errno));
    return -1;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  /* abstract namespace */
  const size_t name_len = strlen(zygiskd::kSocketName);
  memcpy(addr.sun_path + 1, zygiskd::kSocketName, name_len);
  socklen_t len =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name_len);

  if (bind(srv, reinterpret_cast<sockaddr *>(&addr), len) < 0) {
    DLOGE("bind @%s failed: %s", zygiskd::kSocketName, strerror(errno));
    close(srv);
    return -1;
  }
  if (listen(srv, 32) < 0) {
    DLOGE("listen failed: %s", strerror(errno));
    close(srv);
    return -1;
  }
  return srv;
}

int nl_listen() {
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, YZ_NETLINK_PROTO);
  if (fd < 0) {
    DLOGE("netlink socket: %s", strerror(errno));
    return -1;
  }
  sockaddr_nl addr{};
  addr.nl_family = AF_NETLINK;
  addr.nl_groups = 1u << (YZ_NL_GROUP_EVENTS - 1);
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    DLOGE("netlink bind: %s", strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

void nl_drain(int fd) {
  char buf[4096];
  ssize_t got = recv(fd, buf, sizeof(buf), 0);
  if (got <= 0)
    return;

  size_t remaining = static_cast<size_t>(got);
  for (nlmsghdr *nlh = reinterpret_cast<nlmsghdr *>(buf);
       remaining >= sizeof(*nlh) && nlh->nlmsg_len >= sizeof(*nlh) &&
       nlh->nlmsg_len <= remaining;) {
    if (nlh->nlmsg_type == YZ_NL_MSG_EVENT &&
        nlh->nlmsg_len >= NLMSG_LENGTH(sizeof(yz_event))) {
      auto *ev = static_cast<yz_event *>(NLMSG_DATA(nlh));
      if (ev->type == YZ_EV_RELOAD) {
#if defined(YUKIZYGISK_RUNTIME_LOG)
        read_yzconfig();
        DLOGI("reload event");
        rescan_modules();
#else
        DLOGI("reload event");
        rescan_modules();
        read_yzconfig();
#endif
        (void)yzpolicy::refresh(true);
      } else if (ev->type == YZ_EV_SAFEMODE) {
        DLOGI("safemode event pid=%u crashes=%u", ev->pid, ev->appid);
      } else if (ev->type == YZ_EV_POLICY_REFRESH) {
        yzpolicy::handle_refresh_request(ev->appid);
      }
    }

    const size_t step = NLMSG_ALIGN(nlh->nlmsg_len);
    if (step > remaining)
      break;
    remaining -= step;
    nlh = reinterpret_cast<nlmsghdr *>(reinterpret_cast<char *>(nlh) + step);
  }
}

uint64_t resolve_linker_sym(const char *path, const char *want) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(ElfW(Ehdr))) {
    close(fd);
    return 0;
  }
  void *map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    return 0;

  auto *base = static_cast<const uint8_t *>(map);
  auto *eh = reinterpret_cast<const ElfW(Ehdr) *>(base);
  uint64_t result = 0;
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 &&
      eh->e_ident[EI_CLASS] ==
          (sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32)) {
    auto *sh = reinterpret_cast<const ElfW(Shdr) *>(base + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum && !result; i++) {
      if (sh[i].sh_type != SHT_DYNSYM)
        continue;
      auto *syms = reinterpret_cast<const ElfW(Sym) *>(base + sh[i].sh_offset);
      const char *strs =
          reinterpret_cast<const char *>(base + sh[sh[i].sh_link].sh_offset);
      size_t n = sh[i].sh_size / sizeof(ElfW(Sym));
      for (size_t j = 0; j < n; j++) {
        if (strcmp(strs + syms[j].st_name, want) == 0) {
          result = syms[j].st_value;
          break;
        }
      }
    }
  }
  munmap(map, st.st_size);
  return result;
}

uint64_t resolve_first(const char *const *cands, size_t n, const char **hit) {
  for (size_t i = 0; i < n; ++i) {
    uint64_t off = resolve_linker_sym(kLinkerPath, cands[i]);
    if (off) {
      if (hit)
        *hit = cands[i];
      return off;
    }
  }
  return 0;
}

bool send_dlopen_offset() {
  static const char *const kDlopen[] = {
      "__loader_android_dlopen_ext",
      "android_dlopen_ext",
  };
  static const char *const kDlsym[] = {
      "__loader_dlsym",
      "dlsym",
  };

  const char *dlopen_name = nullptr;
  const char *dlsym_name = nullptr;
  yz_dlopen_cmd cmd{};
  cmd.dlopen_offset = resolve_first(kDlopen, 2, &dlopen_name);
  cmd.dlsym_offset = resolve_first(kDlsym, 2, &dlsym_name);

  if (!cmd.dlopen_offset || !cmd.dlsym_offset) {
    DLOGI("linker resolve incomplete: dlopen=%s dlsym=%s",
          dlopen_name ? dlopen_name : "(none)",
          dlsym_name ? dlsym_name : "(none)");
    return false;
  }

  int ret = yzhost::ctl(kSetDlopenRequest, &cmd);
  DLOGI("%s dlopen '%s'=0x%llx dlsym '%s'=0x%llx -> kernel ret=%d", kLinkerPath,
        dlopen_name, (unsigned long long)cmd.dlopen_offset, dlsym_name,
        (unsigned long long)cmd.dlsym_offset, ret);
  return ret == 0;
}

int run_daemon() {
  int ready_fd = consume_ready_fd();
#if defined(__LP64__)
  pid_t compat_pid = -1;
#endif // #if defined(__LP64__)
  yzhost::load_env();

  signal(SIGPIPE, SIG_IGN);

  int srv = bind_listen();
  if (srv < 0) {
    DLOGI("@%s already owned by another zygiskd; refusing duplicate start",
          zygiskd::kSocketName);
    notify_ready(ready_fd, false);
    return 1;
  }

  if (yzhost::claim_control_fd() < 0) {
    close(srv);
    notify_ready(ready_fd, false);
    return 1;
  }

  if (yzhost::ctl(YZ_IOCTL_PREPARE_RUNTIME_POLICY, nullptr) != 0) {
    DLOGE("failed to prepare runtime SELinux policy: %s", strerror(errno));
    close(srv);
    notify_ready(ready_fd, false);
    return 1;
  }

  int nlfd = nl_listen();
  if (nlfd < 0) {
    DLOGE("kernel event channel unavailable; exiting");
    close(srv);
    notify_ready(ready_fd, false);
    return 1;
  }
#if defined(YUKIZYGISK_RUNTIME_LOG)
  read_yzconfig();
#endif
  yz_root_status_cmd root_status{};
  if (!yzhost::get_root_status(&root_status) ||
      !yzpolicy::setup(yzhost::g_control_fd, root_status)) {
    DLOGE("root policy unavailable; exiting");
    close(nlfd);
    close(srv);
    notify_ready(ready_fd, false);
    return 1;
  }

#if defined(YUKIZYGISK_RUNTIME_LOG)
  rescan_modules();
#else
  rescan_modules();
  read_yzconfig();
#endif
  if (!send_dlopen_offset()) {
    DLOGE("linker offsets unavailable; exiting");
    close(nlfd);
    close(srv);
    notify_ready(ready_fd, false);
    return 1;
  }

#if defined(__LP64__)
  if (needs_compat_daemon()) {
    compat_pid = spawn_compat_daemon();
    if (compat_pid < 0) {
      DLOGE("zygiskd32 failed to start");
      close(nlfd);
      close(srv);
      notify_ready(ready_fd, false);
      return 1;
    }
  } else {
    DLOGI("no 32-bit zygote or native target; skipping zygiskd32");
  }
  // The daemon-ready handshake disarms the LKM bootstrap fail-closed guard.
  // Integrated (built-in) kernels arm no such guard and the control fd is not a
  // bootstrap fd, so the ioctl would be rejected; skip it in that mode.
  if (!yzhost::g_integrated &&
      yzhost::ctl(YZ_IOCTL_DAEMON_READY, nullptr) != 0) {
    DLOGE("kernel rejected daemon readiness: %s", strerror(errno));
    stop_compat_daemon(compat_pid);
    close(nlfd);
    close(srv);
    notify_ready(ready_fd, false);
    return 1;
  }
#endif // #if defined(__LP64__)

  DLOGI("zygiskd up: unix @%s, netlink proto=%d, modules=%s, config=%s, "
        "policy=%s",
        zygiskd::kSocketName, YZ_NETLINK_PROTO, yzhost::modules_dir().c_str(),
        yzhost::config_path().c_str(), yzpolicy::source_name());
  notify_ready(ready_fd, true);

  pollfd pfds[2] = {{srv, POLLIN, 0}, {nlfd, POLLIN, 0}};
  for (;;) {
    if (poll(pfds, 2, -1) < 0) {
      if (errno == EINTR)
        continue;
      DLOGE("poll failed: %s; exiting", strerror(errno));
#if defined(__LP64__)
      stop_compat_daemon(compat_pid);
#endif // #if defined(__LP64__)
      return 1;
    }
    if ((pfds[0].revents | pfds[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
      DLOGE("daemon channel failed; exiting");
#if defined(__LP64__)
      stop_compat_daemon(compat_pid);
#endif // #if defined(__LP64__)
      return 1;
    }
    if (pfds[0].revents & POLLIN) {
      int client = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC);
      if (client >= 0) {
        handle_client(client); // TODO: concurrency once companions exist
        close(client);
      }
    }
    if (pfds[1].revents & POLLIN)
      nl_drain(nlfd);
  }
}

} // namespace

extern "C" int zygiskd_main() { return run_daemon(); }

#ifndef YZ_ZYGISKD_NO_MAIN
int main(int argc, char **argv) {
  // Lightweight probe used by post-fs-data.sh to decide whether the running
  // kernel is integrated (built-in): exit 0 when the KSU control fd is served,
  // non-zero otherwise. Does not start the daemon.
  if (argc >= 2 && strcmp(argv[1], "--probe-integrated") == 0) {
    int fd = yzhost::try_claim_via_ksu();
    if (fd >= 0) {
      close(fd);
      return 0;
    }
    return 1;
  }
  return zygiskd_main();
}
#endif // #ifndef YZ_ZYGISKD_NO_MAIN
