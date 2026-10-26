/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <set>
#include <string>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "sql/engine/expr/ob_expr_uuid.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

namespace
{

// Install restrictions only in a death-test child. Each test exercises the
// production implementation with a real kernel denial, without interposing
// process-wide libc functions or needing privileged network configuration.
bool deny_operation(const int syscall_nr, const unsigned int ioctl_request = 0)
{
  struct sock_filter syscall_filter[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<unsigned int>(syscall_nr), 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_filter ioctl_filter[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<unsigned int>(SYS_ioctl), 0, 3),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ioctl_request, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog program = {};
  program.len = ioctl_request == 0 ? sizeof(syscall_filter) / sizeof(syscall_filter[0])
                                  : sizeof(ioctl_filter) / sizeof(ioctl_filter[0]);
  program.filter = ioctl_request == 0 ? syscall_filter : ioctl_filter;
  return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0
      && prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

int check_fallback(const int syscall_nr, const unsigned int ioctl_request = 0)
{
  if (!deny_operation(syscall_nr, ioctl_request)) {
    return 10;
  }
  ObUUIDNode node;
  if (!node.is_inited_ || (node.mac_addr_[0] & 0x01) == 0) {
    return 11;
  }
  if (OB_SUCCESS != ObExprUuid::init()) {
    return 12;
  }
  std::set<std::string> uuids;
  std::string first_node;
  for (int i = 0; i < 1024; ++i) {
    char uuid[UuidCommon::LENGTH_UUID + 1] = {};
    if (OB_SUCCESS != ObExprUuid::gen_server_uuid(uuid, UuidCommon::LENGTH_UUID)) {
      return 13;
    }
    const std::string value(uuid);
    // Retain UUIDv1 and the RFC variant, with the same random node for every
    // UUID produced by this server. The node's first byte ends at offset 25.
    if (value.size() != UuidCommon::LENGTH_UUID || value[14] != '1'
        || nullptr == strchr("89ab", value[19])
        || nullptr == strchr("13579bdf", value[25])) {
      return 14;
    }
    if (i == 0) {
      first_node = value.substr(24);
    } else if (first_node != value.substr(24)) {
      return 15;
    }
    if (!uuids.insert(value).second) {
      return 16;
    }
  }
  return 0;
}

TEST(UuidNodeDeathTest, socket_denied)
{
  ASSERT_EXIT(_exit(check_fallback(SYS_socket)), ::testing::ExitedWithCode(0), "");
}

TEST(UuidNodeDeathTest, interface_enumeration_denied)
{
  ASSERT_EXIT(_exit(check_fallback(SYS_ioctl, SIOCGIFCONF)), ::testing::ExitedWithCode(0), "");
}

TEST(UuidNodeDeathTest, interface_flags_denied)
{
  ASSERT_EXIT(_exit(check_fallback(SYS_ioctl, SIOCGIFFLAGS)), ::testing::ExitedWithCode(0), "");
}

TEST(UuidNodeDeathTest, mac_retrieval_denied)
{
  ASSERT_EXIT(_exit(check_fallback(SYS_ioctl, SIOCGIFHWADDR)), ::testing::ExitedWithCode(0), "");
}

TEST(UuidNodeDeathTest, entropy_denied_does_not_initialize_node)
{
  ASSERT_EXIT({
    if (!deny_operation(SYS_socket) || !deny_operation(SYS_getrandom)) {
      _exit(10);
    }
    ObUUIDNode node;
    if (node.is_inited_ || OB_ERR_UNEXPECTED != node.init() || node.is_inited_) {
      _exit(11);
    }
    _exit(OB_ERR_UNEXPECTED == ObExprUuid::init() ? 0 : 12);
  }, ::testing::ExitedWithCode(0), "");
}

TEST(UuidNodeDeathTest, mac_available_does_not_require_entropy)
{
  ObUUIDNode expected;
  if (!expected.is_inited_ || (expected.mac_addr_[0] & 0x01) != 0) {
    GTEST_SKIP() << "This host has no accessible unicast MAC address";
  }
  ASSERT_EXIT({
    if (!deny_operation(SYS_getrandom)) {
      _exit(10);
    }
    ObUUIDNode node;
    _exit(node.is_inited_ && 0 == memcmp(expected.mac_addr_, node.mac_addr_, sizeof(node.mac_addr_)) ? 0 : 11);
  }, ::testing::ExitedWithCode(0), "");
}

} // namespace
} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
