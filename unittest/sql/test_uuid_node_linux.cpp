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
#include <algorithm>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <vector>
#include "src/sql/engine/expr/uuid_node_linux.h"

namespace
{
constexpr int SOCKET_FD = 1000000;
constexpr int RANDOM_FD = 1000001;
const unsigned char MAC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
const unsigned char RANDOM_BYTES[6] = {0x42, 0x67, 0x89, 0xab, 0xcd, 0xef};
enum class Network { MAC_OK, SOCKET_DENIED, ENUM_DENIED, EMPTY, LOOPBACK, FLAGS_DENIED, MAC_DENIED, FIRST_MAC_DENIED };
Network network;
bool open_denied;
int open_interruptions;
std::vector<int> reads;
size_t read_index;
size_t random_offset;
int socket_closes;
int random_closes;
int random_opens;

class LinuxUuidNodeTest : public testing::Test
{
protected:
  void SetUp() override
  {
    network = Network::MAC_OK;
    open_denied = false;
    open_interruptions = 0;
    reads.clear();
    read_index = 0;
    random_offset = 0;
    socket_closes = random_closes = random_opens = 0;
  }
};
}

extern "C" int __real_socket(int, int, int);
extern "C" int __real_ioctl(int, unsigned long, ...);
extern "C" int __real_open(const char *, int, ...);
extern "C" ssize_t __real_read(int, void *, size_t);
extern "C" int __real_close(int);

extern "C" int __wrap_socket(int domain, int type, int protocol)
{
  if (domain != AF_INET || type != SOCK_DGRAM || protocol != IPPROTO_IP) {
    return __real_socket(domain, type, protocol);
  }
  if (network == Network::SOCKET_DENIED) {
    errno = EPERM;
    return -1;
  }
  return SOCKET_FD;
}

extern "C" int __wrap_ioctl(int fd, unsigned long op, ...)
{
  va_list args;
  va_start(args, op);
  void *arg = va_arg(args, void *);
  va_end(args);
  if (fd != SOCKET_FD) { return __real_ioctl(fd, op, arg); }
  if (op == SIOCGIFCONF) {
    if (network == Network::ENUM_DENIED) { errno = EPERM; return -1; }
    auto *config = static_cast<struct ifconf *>(arg);
    if (network == Network::EMPTY) { config->ifc_len = 0; return 0; }
    std::memset(config->ifc_req, 0, sizeof(struct ifreq));
    std::strcpy(config->ifc_req[0].ifr_name, "test0");
    config->ifc_len = sizeof(struct ifreq);
    if (network == Network::FIRST_MAC_DENIED) {
      std::memset(&config->ifc_req[1], 0, sizeof(struct ifreq));
      std::strcpy(config->ifc_req[1].ifr_name, "test1");
      config->ifc_len *= 2;
    }
  } else if (op == SIOCGIFFLAGS) {
    if (network == Network::FLAGS_DENIED) { errno = EPERM; return -1; }
    static_cast<struct ifreq *>(arg)->ifr_flags = network == Network::LOOPBACK ? IFF_LOOPBACK : 0;
  } else if (op == SIOCGIFHWADDR) {
    if (network == Network::MAC_DENIED
        || (network == Network::FIRST_MAC_DENIED
            && std::strcmp(static_cast<struct ifreq *>(arg)->ifr_name, "test0") == 0)) {
      errno = EPERM;
      return -1;
    }
    std::memcpy(static_cast<struct ifreq *>(arg)->ifr_hwaddr.sa_data, MAC, sizeof(MAC));
  } else {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

extern "C" int __wrap_open(const char *path, int flags, ...)
{
  if (std::strcmp(path, "/dev/urandom") != 0) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
      va_list args;
      va_start(args, flags);
      mode = va_arg(args, int);
      va_end(args);
    }
    return __real_open(path, flags, mode);
  }
  ++random_opens;
  if (open_interruptions > 0) { --open_interruptions; errno = EINTR; return -1; }
  if (open_denied) { errno = EACCES; return -1; }
  return RANDOM_FD;
}

extern "C" ssize_t __wrap_read(int fd, void *buf, size_t size)
{
  if (fd != RANDOM_FD) { return __real_read(fd, buf, size); }
  int result = read_index < reads.size() ? reads[read_index++] : static_cast<int>(size);
  if (result < 0) { errno = -result; return -1; }
  const size_t count = std::min(size, static_cast<size_t>(result));
  if (random_offset + count > sizeof(RANDOM_BYTES)) { errno = EIO; return -1; }
  std::memcpy(buf, RANDOM_BYTES + random_offset, count);
  random_offset += count;
  return count;
}

// Optimized glibc builds may fortify read() into __read_chk(). Keep the
// same fault injection while preserving the checked-call buffer bound.
extern "C" ssize_t __wrap___read_chk(int fd, void *buf, size_t size, size_t capacity)
{
  if (size > capacity) { std::abort(); }
  return __wrap_read(fd, buf, size);
}

extern "C" int __wrap_close(int fd)
{
  if (fd == SOCKET_FD) { ++socket_closes; return 0; }
  if (fd == RANDOM_FD) { ++random_closes; errno = EBADF; return 0; }
  return __real_close(fd);
}

TEST_F(LinuxUuidNodeTest, preserves_hardware_mac_without_reading_entropy)
{
  unsigned char node[6] = {};
  bool random = true;
  ASSERT_EQ(0, oceanbase::sql::uuid_internal::init_linux_node(node, random));
  EXPECT_EQ(0, std::memcmp(node, MAC, sizeof(node)));
  EXPECT_FALSE(random);
  EXPECT_EQ(0, random_opens);
  EXPECT_EQ(1, socket_closes);
}

TEST_F(LinuxUuidNodeTest, falls_back_for_every_mac_discovery_failure)
{
  const Network failures[] = {Network::SOCKET_DENIED, Network::ENUM_DENIED,
      Network::EMPTY, Network::LOOPBACK, Network::FLAGS_DENIED, Network::MAC_DENIED};
  for (const Network failure : failures) {
    SetUp();
    network = failure;
    unsigned char node[6] = {};
    bool random = false;
    ASSERT_EQ(0, oceanbase::sql::uuid_internal::init_linux_node(node, random))
        << static_cast<int>(failure);
    EXPECT_TRUE(random);
    const unsigned char expected[6] = {0x43, 0x67, 0x89, 0xab, 0xcd, 0xef};
    EXPECT_EQ(0, std::memcmp(node, expected, sizeof(node)));
    EXPECT_EQ(failure == Network::SOCKET_DENIED ? 0 : 1, socket_closes);
    EXPECT_EQ(1, random_closes);
  }
}

TEST_F(LinuxUuidNodeTest, retries_interrupted_and_short_entropy_reads)
{
  network = Network::EMPTY;
  reads = {-EINTR, 2, -EINTR, 1, 3};
  unsigned char node[6] = {};
  bool random = false;
  ASSERT_EQ(0, oceanbase::sql::uuid_internal::init_linux_node(node, random));
  const unsigned char expected[6] = {0x43, 0x67, 0x89, 0xab, 0xcd, 0xef};
  EXPECT_EQ(0, std::memcmp(node, expected, sizeof(node)));
  EXPECT_TRUE(random);
  EXPECT_EQ(1, random_closes);
}

TEST_F(LinuxUuidNodeTest, fails_when_entropy_device_cannot_be_opened)
{
  network = Network::ENUM_DENIED;
  open_denied = true;
  unsigned char node[6] = {};
  bool random = false;
  EXPECT_EQ(-1, oceanbase::sql::uuid_internal::init_linux_node(node, random));
  EXPECT_EQ(EACCES, errno);
  EXPECT_FALSE(random);
  EXPECT_EQ(1, socket_closes);
  EXPECT_EQ(0, random_closes);
}

TEST_F(LinuxUuidNodeTest, does_not_publish_partial_random_node_on_eof_or_error)
{
  for (const int failure : {0, -EIO, -EACCES}) {
    SetUp();
    network = Network::EMPTY;
    reads = {2, failure};
    unsigned char node[6] = {9, 9, 9, 9, 9, 9};
    bool random = true;
    EXPECT_EQ(-1, oceanbase::sql::uuid_internal::init_linux_node(node, random));
    EXPECT_EQ(failure == 0 ? EIO : -failure, errno);
    const unsigned char untouched[6] = {9, 9, 9, 9, 9, 9};
    EXPECT_EQ(0, std::memcmp(node, untouched, sizeof(node)));
    EXPECT_FALSE(random);
    EXPECT_EQ(1, socket_closes);
    EXPECT_EQ(1, random_closes);
  }
}

TEST_F(LinuxUuidNodeTest, retries_interrupted_entropy_open)
{
  network = Network::EMPTY;
  open_interruptions = 1;
  unsigned char node[6] = {};
  bool random = false;
  ASSERT_EQ(0, oceanbase::sql::uuid_internal::init_linux_node(node, random));
  EXPECT_TRUE(random);
  EXPECT_EQ(2, random_opens);
  EXPECT_EQ(1, random_closes);
}

TEST_F(LinuxUuidNodeTest, uses_later_interface_after_mac_query_failure)
{
  network = Network::FIRST_MAC_DENIED;
  unsigned char node[6] = {};
  bool random = true;
  ASSERT_EQ(0, oceanbase::sql::uuid_internal::init_linux_node(node, random));
  EXPECT_EQ(0, std::memcmp(node, MAC, sizeof(node)));
  EXPECT_FALSE(random);
  EXPECT_EQ(0, random_opens);
  EXPECT_EQ(1, socket_closes);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
