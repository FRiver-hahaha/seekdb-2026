/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <vector>
#include "sql/engine/expr/ob_expr_uuid.h"

using oceanbase::sql::ObUUIDNode;
using oceanbase::common::OB_SUCCESS;
using oceanbase::common::OB_ERR_UNEXPECTED;

namespace {
enum class Fault { NONE, KNOWN_MAC, SOCKET_DENIED, ENUMERATION_DENIED, FLAGS_DENIED, MAC_DENIED, LOOPBACK_ONLY };
thread_local Fault fault = Fault::NONE;
thread_local bool entropy_fails = false;
thread_local bool deterministic_entropy = false;
thread_local int random_calls = 0;
thread_local int tracked_socket = -1;
thread_local int socket_closes = 0;

class UuidNodeTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    fault = Fault::NONE;
    entropy_fails = false;
    deterministic_entropy = false;
    random_calls = 0;
    tracked_socket = -1;
    socket_closes = 0;
  }

  void TearDown() override { fault = Fault::NONE; entropy_fails = false; deterministic_entropy = false; }
};
}

// Fault-inject only the operating-system/entropy boundary. Tests call the real
// ObUUIDNode::init, not a substitute node implementation or random generator.
extern "C" int __real_socket(int domain, int type, int protocol);
extern "C" int __real_close(int descriptor);
extern "C" int __real_ioctl(int descriptor, unsigned long request, ...);
extern "C" int __real_RAND_bytes(unsigned char *output, int size);

extern "C" int __wrap_socket(int domain, int type, int protocol)
{
  if (fault == Fault::KNOWN_MAC && domain == AF_INET && type == SOCK_DGRAM) {
    return tracked_socket = 12345;
  }
  if (fault == Fault::SOCKET_DENIED && domain == AF_INET && type == SOCK_DGRAM) {
    errno = EPERM;
    return -1;
  }
  int descriptor = __real_socket(domain, type, protocol);
  if (domain == AF_INET && type == SOCK_DGRAM) { tracked_socket = descriptor; }
  return descriptor;
}

extern "C" int __wrap_close(int descriptor)
{
  const bool synthetic = fault == Fault::KNOWN_MAC && descriptor == tracked_socket;
  if (descriptor == tracked_socket && tracked_socket >= 0) { ++socket_closes; tracked_socket = -1; }
  return synthetic ? 0 : __real_close(descriptor);
}

extern "C" int __wrap_ioctl(int descriptor, unsigned long request, ...)
{
  va_list arguments;
  va_start(arguments, request);
  void *argument = va_arg(arguments, void *);
  va_end(arguments);
  if (fault == Fault::KNOWN_MAC) {
    if (request == SIOCGIFCONF) {
      ifconf *configuration = static_cast<ifconf *>(argument);
      ifreq *interface = configuration->ifc_req;
      std::memset(interface, 0, sizeof(*interface));
      std::strncpy(interface->ifr_name, "test0", IFNAMSIZ - 1);
      configuration->ifc_len = sizeof(*interface);
    } else if (request == SIOCGIFFLAGS) {
      static_cast<ifreq *>(argument)->ifr_flags = 0;
    } else if (request == SIOCGIFHWADDR) {
      const unsigned char expected[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
      std::memcpy(static_cast<ifreq *>(argument)->ifr_hwaddr.sa_data, expected, sizeof(expected));
    }
    return 0;
  }
  if ((request == SIOCGIFCONF && fault == Fault::ENUMERATION_DENIED)
      || (request == SIOCGIFFLAGS && fault == Fault::FLAGS_DENIED)
      || (request == SIOCGIFHWADDR && fault == Fault::MAC_DENIED)) {
    errno = EPERM;
    return -1;
  }
  int result = __real_ioctl(descriptor, request, argument);
  if (result == 0 && request == SIOCGIFFLAGS && fault == Fault::LOOPBACK_ONLY) {
    static_cast<ifreq *>(argument)->ifr_flags |= IFF_LOOPBACK;
  }
  return result;
}

extern "C" int __wrap_RAND_bytes(unsigned char *output, int size)
{
  ++random_calls;
  if (entropy_fails) { return 0; }
  if (deterministic_entropy) { std::memset(output, 0x20, size); return 1; }
  return __real_RAND_bytes(output, size);
}

TEST_F(UuidNodeTest, known_hardware_mac_does_not_use_random_fallback)
{
  ObUUIDNode node;
  fault = Fault::KNOWN_MAC;
  random_calls = 0;
  const unsigned char expected[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
  ASSERT_EQ(OB_SUCCESS, node.init());
  ASSERT_TRUE(node.is_inited_);
  EXPECT_EQ(0, random_calls);
  EXPECT_EQ(0, std::memcmp(expected, node.mac_addr_, sizeof(expected)));
}

TEST_F(UuidNodeTest, real_host_initialization_remains_available)
{
  ObUUIDNode node;
  ASSERT_EQ(OB_SUCCESS, node.init());
  EXPECT_TRUE(node.is_inited_);
}

TEST_F(UuidNodeTest, denied_enumeration_uses_marked_node_and_closes_socket)
{
  ObUUIDNode node;
  fault = Fault::ENUMERATION_DENIED;
  deterministic_entropy = true;
  random_calls = socket_closes = 0;
  ASSERT_EQ(OB_SUCCESS, node.init());
  ASSERT_TRUE(node.is_inited_);
  EXPECT_EQ(1, random_calls);
  EXPECT_EQ(1, socket_closes);
  EXPECT_EQ(0x21, node.mac_addr_[0]);
  for (int index = 1; index < 6; ++index) { EXPECT_EQ(0x20, node.mac_addr_[index]); }
}

TEST_F(UuidNodeTest, unavailable_interfaces_and_socket_use_real_entropy)
{
  ObUUIDNode node;
  for (Fault mode : {Fault::SOCKET_DENIED, Fault::FLAGS_DENIED, Fault::MAC_DENIED, Fault::LOOPBACK_ONLY}) {
    fault = mode;
    random_calls = 0;
    ASSERT_EQ(OB_SUCCESS, node.init());
    ASSERT_TRUE(node.is_inited_);
    EXPECT_EQ(1, random_calls);
    EXPECT_EQ(1, node.mac_addr_[0] & 0x01);
  }
}

TEST_F(UuidNodeTest, entropy_failure_fails_closed)
{
  ObUUIDNode node;
  fault = Fault::ENUMERATION_DENIED;
  entropy_fails = true;
  random_calls = 0;
  EXPECT_EQ(OB_ERR_UNEXPECTED, node.init());
  EXPECT_FALSE(node.is_inited_);
  EXPECT_EQ(1, random_calls);
}

TEST_F(UuidNodeTest, repeated_reinitialization_uses_fresh_random_node)
{
  ObUUIDNode node;
  fault = Fault::ENUMERATION_DENIED;
  ASSERT_EQ(OB_SUCCESS, node.init());
  const std::vector<unsigned char> first(node.mac_addr_, node.mac_addr_ + 6);
  ASSERT_EQ(OB_SUCCESS, node.init());
  const std::vector<unsigned char> second(node.mac_addr_, node.mac_addr_ + 6);
  EXPECT_NE(first, second);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
