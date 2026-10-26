/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 */

// This includes the production constructor/init, not a test implementation.
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <cstddef>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "lib/ob_errno.h"
#include "lib/utility/alloc_assist.h"
#include "sql/engine/expr/ob_uuid_node.h"

// Suppress only the logging sink; project constants/macros and OS calls remain.
#define LOG_ERROR(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
namespace oceanbase
{
namespace sql
{
using namespace oceanbase::common;
#include "sql/engine/expr/ob_uuid_node_impl.h"
}
}
using oceanbase::sql::ObUUIDNode;
using namespace oceanbase::common;

namespace
{
enum class Network { ETHERNET, SOCKET_FAIL, ENUM_FAIL, EMPTY, LOOPBACK, FLAGS_FAIL,
                     HW_FAIL, FIRST_FLAGS_FAIL, FIRST_HW_FAIL, TUN_THEN_ETHER,
                     ZERO_MAC, MULTICAST_MAC };
enum class Entropy { NORMAL, INTERRUPTED, SHORT, PARTIAL_ERROR, ERROR, ZERO,
                     UNAVAILABLE, DENIED, OPEN_INTR, OPEN_ERROR, READ_INTR_SHORT,
                     READ_ERROR, READ_ZERO, READ_PARTIAL_ERROR };
Network network = Network::ETHERNET;
Entropy entropy = Entropy::NORMAL;
bool intercept = true;
constexpr int SOCKET_FD = 12345, RANDOM_FD = 12346;
const unsigned char MAC[6] = {0x02, 0x12, 0x34, 0x56, 0x78, 0x9a};
const unsigned char RANDOM[6] = {0x24, 0x35, 0x46, 0x57, 0x68, 0x79};
const unsigned char SYNTHETIC[6] = {0x25, 0x35, 0x46, 0x57, 0x68, 0x79};
int sockets = 0, socket_closes = 0, random_opens = 0, random_closes = 0;
int random_calls = 0, open_calls = 0, read_calls = 0, random_offset = 0;
int checks = 0, failures = 0;

void require(bool condition, const char *name)
{
  ++checks;
  std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
  if (!condition) { ++failures; }
}

void reset(Network next_network, Entropy next_entropy = Entropy::NORMAL)
{
  network = next_network;
  entropy = next_entropy;
  sockets = socket_closes = random_opens = random_closes = 0;
  random_calls = open_calls = read_calls = random_offset = 0;
}

void run_case(ObUUIDNode &node, Network next_network, Entropy next_entropy,
              bool success, bool synthetic, const char *name)
{
  unsigned char previous[6];
  std::memcpy(previous, node.mac_addr_, sizeof(previous));
  reset(next_network, next_entropy);
  const int ret = node.init();
  std::printf("CASE %s ret=%d initialized=%d rng_calls=%d device_reads=%d\n",
              name, ret, node.is_inited_, random_calls, read_calls);
  require(ret == (success ? OB_SUCCESS : OB_ERR_UNEXPECTED), "expected return code");
  require(node.is_inited_ == success, "publish initialized flag only on success");
  require(sockets == socket_closes && random_opens == random_closes, "all descriptors closed");
  if (success) {
    require(0 == std::memcmp(node.mac_addr_, synthetic ? SYNTHETIC : MAC, 6),
            "complete expected node bytes");
    require(synthetic ? random_calls > 0 : (random_calls == 0 && open_calls == 0),
            "entropy used only when no usable MAC exists");
  } else {
    require(0 == std::memcmp(node.mac_addr_, previous, 6), "failure never publishes partial bytes");
  }
}

ssize_t copy_random(void *buffer, size_t count, size_t maximum)
{
  const size_t copied = count < maximum ? count : maximum;
  if (random_offset + copied > sizeof(RANDOM)) { errno = EIO; return -1; }
  std::memcpy(buffer, RANDOM + random_offset, copied);
  random_offset += copied;
  return copied;
}

bool restrict_discovery(bool deny_getrandom)
{
  // Affects only this test process; no host namespace/policy changes.
  struct sock_filter filter[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getrandom, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, deny_getrandom ? (SECCOMP_RET_ERRNO | EPERM) : SECCOMP_RET_ALLOW),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog program = {static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
  return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0
      && prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}
} // namespace

extern "C" int __real_socket(int, int, int);
extern "C" int __real_close(int);
extern "C" int __real_ioctl(int, unsigned long, ...);
extern "C" ssize_t __real_getrandom(void *, size_t, unsigned int);
extern "C" int __real_open(const char *, int, ...);
extern "C" ssize_t __real_read(int, void *, size_t);
extern "C" ssize_t __real___read_chk(int, void *, size_t, size_t);

extern "C" int __wrap_socket(int domain, int type, int protocol)
{
  if (!intercept) { return __real_socket(domain, type, protocol); }
  if (network == Network::SOCKET_FAIL) { errno = EPERM; return -1; }
  ++sockets;
  return SOCKET_FD;
}

extern "C" int __wrap_close(int descriptor)
{
  if (!intercept) { return __real_close(descriptor); }
  if (descriptor == SOCKET_FD) { ++socket_closes; }
  else if (descriptor == RANDOM_FD) { ++random_closes; }
  else { return __real_close(descriptor); }
  // Make stale errno visibly different from the failure captured at its source.
  errno = ETIMEDOUT;
  return 0;
}

extern "C" int __wrap_ioctl(int descriptor, unsigned long request, ...)
{
  va_list arguments;
  va_start(arguments, request);
  void *argument = va_arg(arguments, void *);
  va_end(arguments);
  if (!intercept || descriptor != SOCKET_FD) { return __real_ioctl(descriptor, request, argument); }
  if (request == SIOCGIFCONF) {
    if (network == Network::ENUM_FAIL) { errno = EPERM; return -1; }
    auto *configuration = static_cast<ifconf *>(argument);
    if (network == Network::EMPTY) { configuration->ifc_len = 0; return 0; }
    const bool two = network == Network::FIRST_FLAGS_FAIL || network == Network::FIRST_HW_FAIL
                  || network == Network::TUN_THEN_ETHER;
    const int count = two ? 2 : 1;
    if (configuration->ifc_len < static_cast<int>(count * sizeof(ifreq))) { errno = EINVAL; return -1; }
    std::memset(configuration->ifc_req, 0, count * sizeof(ifreq));
    std::strcpy(configuration->ifc_req[0].ifr_name, "probe0");
    if (two) { std::strcpy(configuration->ifc_req[1].ifr_name, "probe1"); }
    configuration->ifc_len = count * sizeof(ifreq);
    return 0;
  }
  auto *interface = static_cast<ifreq *>(argument);
  const bool first = 0 == std::strcmp(interface->ifr_name, "probe0");
  if (request == SIOCGIFFLAGS) {
    if (network == Network::FLAGS_FAIL || (first && network == Network::FIRST_FLAGS_FAIL)) {
      errno = EPERM; return -1;
    }
    interface->ifr_flags = network == Network::LOOPBACK ? IFF_LOOPBACK : IFF_UP;
    return 0;
  }
  if (request == SIOCGIFHWADDR) {
    if (network == Network::HW_FAIL || (first && network == Network::FIRST_HW_FAIL)) {
      errno = EPERM; return -1;
    }
    std::memset(&interface->ifr_hwaddr, 0, sizeof(interface->ifr_hwaddr));
    if (first && network == Network::TUN_THEN_ETHER) {
      interface->ifr_hwaddr.sa_family = ARPHRD_NONE;
    } else {
      interface->ifr_hwaddr.sa_family = ARPHRD_ETHER;
      std::memcpy(interface->ifr_hwaddr.sa_data, MAC, 6);
      if (network == Network::ZERO_MAC) { std::memset(interface->ifr_hwaddr.sa_data, 0, 6); }
      if (network == Network::MULTICAST_MAC) { interface->ifr_hwaddr.sa_data[0] |= 1; }
    }
    return 0;
  }
  errno = EINVAL;
  return -1;
}

extern "C" ssize_t __wrap_getrandom(void *buffer, size_t count, unsigned int flags)
{
  if (!intercept) { return __real_getrandom(buffer, count, flags); }
  ++random_calls;
  if (entropy == Entropy::UNAVAILABLE || entropy >= Entropy::OPEN_INTR) { errno = ENOSYS; return -1; }
  if (entropy == Entropy::DENIED) { errno = EPERM; return -1; }
  if (entropy == Entropy::ERROR || (entropy == Entropy::PARTIAL_ERROR && random_calls > 1)) {
    errno = EIO; return -1;
  }
  if (entropy == Entropy::ZERO) { return 0; }
  if (entropy == Entropy::INTERRUPTED && (random_calls == 1 || random_calls == 3)) {
    errno = EINTR; return -1;
  }
  const size_t maximum = entropy == Entropy::NORMAL ? 6 : 2;
  return copy_random(buffer, count, maximum);
}

extern "C" int __wrap_open(const char *path, int flags, ...)
{
  if (!intercept) { return __real_open(path, flags); }
  ++open_calls;
  require(0 == std::strcmp(path, "/dev/urandom") && (flags & O_CLOEXEC), "secure device with close-on-exec");
  if (entropy == Entropy::OPEN_ERROR) { errno = EACCES; return -1; }
  if (entropy == Entropy::OPEN_INTR && open_calls == 1) { errno = EINTR; return -1; }
  ++random_opens;
  random_offset = 0;
  return RANDOM_FD;
}

extern "C" ssize_t __wrap_read(int descriptor, void *buffer, size_t count)
{
  if (!intercept || descriptor != RANDOM_FD) { return __real_read(descriptor, buffer, count); }
  ++read_calls;
  if (entropy == Entropy::READ_ERROR || (entropy == Entropy::READ_PARTIAL_ERROR && read_calls > 1)) {
    errno = EIO; return -1;
  }
  if (entropy == Entropy::READ_ZERO) { return 0; }
  if (entropy == Entropy::READ_INTR_SHORT && (read_calls == 1 || read_calls == 3)) {
    errno = EINTR; return -1;
  }
  return copy_random(buffer, count, entropy >= Entropy::READ_INTR_SHORT ? 2 : 6);
}

// Fortified builds may lower read() to this glibc entry point.
extern "C" ssize_t __wrap___read_chk(int descriptor, void *buffer, size_t count, size_t size)
{
  if (!intercept) { return __real___read_chk(descriptor, buffer, count, size); }
  if (count > size) { require(false, "read stays within its object"); errno = EIO; return -1; }
  return __wrap_read(descriptor, buffer, count);
}

int main(int argc, char **argv)
{
  const bool permission_mode = argc > 1 && 0 == std::strcmp(argv[1], "--real-permission");
  const bool device_mode = argc > 1 && 0 == std::strcmp(argv[1], "--real-device");
  if (argc > 1 && (0 == std::strcmp(argv[1], "--real") || permission_mode || device_mode)) {
    intercept = false;
    if (permission_mode || device_mode) {
      if (!restrict_discovery(device_mode)) { std::perror("install child seccomp filter"); return 2; }
      require(socket(AF_INET, SOCK_DGRAM, IPPROTO_IP) == -1 && errno == EPERM,
              "kernel denies discovery socket with EPERM");
      if (device_mode) {
        unsigned char byte;
        require(getrandom(&byte, 1, 0) == -1 && errno == EPERM, "kernel denies getrandom; use secure device");
      }
    }
    ObUUIDNode node;
    const int ret = node.init();
    std::printf("REAL_OS ret=%d initialized=%d", ret, node.is_inited_);
    if (node.is_inited_) {
      std::printf(" node=%02x:%02x:%02x:%02x:%02x:%02x", node.mac_addr_[0], node.mac_addr_[1],
          node.mac_addr_[2], node.mac_addr_[3], node.mac_addr_[4], node.mac_addr_[5]);
    }
    std::puts("");
    const unsigned char zero[6] = {};
    require(ret == OB_SUCCESS && node.is_inited_ && 0 != std::memcmp(node.mac_addr_, zero, 6),
            "real OS initialization produces a complete nonzero node");
    if (permission_mode || device_mode || (argc > 2 && 0 == std::strcmp(argv[2], "--expect-random"))) {
      require(node.is_inited_ && (node.mac_addr_[0] & 1), "real MAC-less environment uses synthetic node");
    }
    return failures ? 1 : 0;
  }
  ObUUIDNode node;
  require(node.is_inited_ && 0 == std::memcmp(node.mac_addr_, MAC, 6), "constructor uses real production path");
  for (Network net : {Network::ETHERNET, Network::FIRST_FLAGS_FAIL, Network::FIRST_HW_FAIL,
                     Network::TUN_THEN_ETHER}) {
    run_case(node, net, Entropy::NORMAL, true, false, "preserve usable MAC; continue past failed/ineligible interfaces");
  }
  for (Network net : {Network::SOCKET_FAIL, Network::ENUM_FAIL, Network::EMPTY, Network::LOOPBACK,
                     Network::FLAGS_FAIL, Network::HW_FAIL, Network::ZERO_MAC, Network::MULTICAST_MAC}) {
    run_case(node, net, Entropy::NORMAL, true, true, "unavailable MAC falls back despite discovery errors");
  }
  for (Entropy rng : {Entropy::INTERRUPTED, Entropy::SHORT, Entropy::UNAVAILABLE, Entropy::DENIED,
                     Entropy::OPEN_INTR, Entropy::READ_INTR_SHORT}) {
    run_case(node, Network::LOOPBACK, rng, true, true, "complete secure bytes across interruptions/partial reads");
  }
  for (Entropy rng : {Entropy::ERROR, Entropy::ZERO, Entropy::PARTIAL_ERROR, Entropy::OPEN_ERROR,
                     Entropy::READ_ERROR, Entropy::READ_ZERO, Entropy::READ_PARTIAL_ERROR}) {
    run_case(node, Network::LOOPBACK, rng, false, false, "entropy failure stays fatal and does not publish partial bytes");
    run_case(node, Network::LOOPBACK, rng, false, false, "repeated entropy failure remains uninitialized");
    run_case(node, Network::LOOPBACK, Entropy::NORMAL, true, true, "recover after entropy failure");
  }
  run_case(node, Network::ETHERNET, Entropy::ERROR, true, false, "valid MAC succeeds even with broken entropy");
  reset(Network::LOOPBACK, Entropy::ERROR);
  ObUUIDNode failed;
  require(!failed.is_inited_, "constructor failure is visible to consumers");
  run_case(failed, Network::ETHERNET, Entropy::NORMAL, true, false, "failed constructor can recover");
  std::printf("RESULT checks=%d failures=%d\n", checks, failures);
  return failures ? 1 : 0;
}
