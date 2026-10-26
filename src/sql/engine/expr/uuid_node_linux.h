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

#pragma once

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace oceanbase
{
namespace sql
{
namespace uuid_internal
{

// Returns 0 on success, or -1 with errno set. The caller owns initialization state.
inline int init_linux_node(unsigned char (&node)[6], bool &is_random)
{
  is_random = false;
  bool found = false;
  const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock >= 0) {
    struct ifreq interfaces[32] = {};
    struct ifconf config = {};
    config.ifc_len = sizeof(interfaces);
    config.ifc_req = interfaces;
    if (ioctl(sock, SIOCGIFCONF, &config) == 0) {
      const int count = config.ifc_len / sizeof(struct ifreq);
      for (int i = 0; i < count && !found; ++i) {
        struct ifreq request = {};
        std::memcpy(request.ifr_name, interfaces[i].ifr_name, IFNAMSIZ);
        if (ioctl(sock, SIOCGIFFLAGS, &request) == 0
            && !(request.ifr_flags & IFF_LOOPBACK)
            && ioctl(sock, SIOCGIFHWADDR, &request) == 0) {
          std::memcpy(node, request.ifr_hwaddr.sa_data, sizeof(node));
          found = true;
        }
      }
    }
    close(sock);
  }
  if (found) {
    return 0;
  }
  // RFC 4122 section 4.5: a random node must have the multicast bit set
  // so it cannot collide with an IEEE 802 unicast address. Publish it only
  // after all six bytes have been read from the system random source.
  int random_fd;
  do {
    random_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  } while (random_fd < 0 && errno == EINTR);
  if (random_fd < 0) {
    return -1;
  }
  unsigned char random_node[sizeof(node)];
  size_t offset = 0;
  int error = 0;
  while (offset < sizeof(random_node) && error == 0) {
    const ssize_t bytes = read(random_fd, random_node + offset, sizeof(random_node) - offset);
    if (bytes > 0) {
      offset += bytes;
    } else if (bytes == 0) {
      error = EIO;
    } else if (errno != EINTR) {
      error = errno;
    }
  }
  close(random_fd);
  if (error != 0) {
    errno = error;
    return -1;
  }
  random_node[0] |= 0x01;
  std::memcpy(node, random_node, sizeof(node));
  is_random = true;
  return 0;
}

} // namespace uuid_internal
} // namespace sql
} // namespace oceanbase
