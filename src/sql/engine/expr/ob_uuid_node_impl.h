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

// Included inside oceanbase::sql by the expression translation unit and the
// focused native node probe. Keep initialization in this single production body.
#if defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
namespace
{
bool usable_linux_uuid_mac(const struct sockaddr &address)
{
  unsigned char nonzero = 0;
  for (int i = 0; i < 6; ++i) {
    nonzero |= static_cast<unsigned char>(address.sa_data[i]);
  }
  return address.sa_family == ARPHRD_ETHER && nonzero != 0 && (address.sa_data[0] & 0x01) == 0;
}

int fill_linux_random_uuid_node(unsigned char *node)
{
  int ret = OB_SUCCESS;
  size_t offset = 0;
  bool use_device = false;
  while (offset < 6 && OB_SUCC(ret) && !use_device) {
    const ssize_t count = getrandom(node + offset, 6 - offset, 0);
    const int random_errno = count < 0 ? errno : 0;
    if (count > 0) {
      offset += count;
    } else if (count < 0 && random_errno == EINTR) {
      // Retry without discarding any bytes already obtained.
    } else if (count < 0 && (random_errno == ENOSYS || random_errno == EPERM || random_errno == EACCES)) {
      use_device = true;
      LOG_WARN("getrandom unavailable, trying secure random device", K(random_errno));
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("UUID node entropy failed at getrandom", K(ret), K(random_errno), K(count));
    }
  }
  if (OB_SUCC(ret) && use_device) {
    int fd = -1;
    do {
      fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
      const int random_errno = errno;
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("UUID node entropy failed opening /dev/urandom", K(ret), K(random_errno));
    } else {
      // Start afresh if getrandom produced bytes before becoming unavailable.
      offset = 0;
      while (offset < 6 && OB_SUCC(ret)) {
        const ssize_t count = read(fd, node + offset, 6 - offset);
        const int random_errno = count < 0 ? errno : 0;
        if (count > 0) {
          offset += count;
        } else if (count < 0 && random_errno == EINTR) {
          // Retry the remaining bytes. EOF and all other errors are fatal.
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("UUID node entropy failed reading /dev/urandom", K(ret), K(random_errno), K(count));
        }
      }
      // Linux close must not be retried after EINTR: the descriptor is released.
      close(fd);
    }
  }
  if (OB_SUCC(ret)) {
    // RFC 9562 section 6.10: distinguish synthetic UUIDv1 nodes from MACs.
    node[0] |= 0x01;
  }
  return ret;
}
} // namespace
#endif

ObUUIDNode::ObUUIDNode()
{
  is_inited_ = false;
  (void)init();
}


/*
 * get and set mac address
 *
 */
int ObUUIDNode::init()
{
  int ret = OB_SUCCESS;
  is_inited_ = false;
#ifdef __EMSCRIPTEN__
  // Browsers expose no hardware MAC. Use a random UUID node and mark it as
  // non-MAC with the multicast bit, as on the Android fallback below.
  // Emscripten getentropy uses the host cryptographic random source.
  if (getentropy(mac_addr_, sizeof(mac_addr_)) != 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to generate UUID node", K(ret), K(errno));
  } else {
    mac_addr_[0] |= 0x01;
    is_inited_ = true;
  }
#elif defined(_WIN32)
  ULONG buf_len = 15000;
  PIP_ADAPTER_ADDRESSES addresses = NULL;
  bool mac_addr_found = false;
  addresses = (IP_ADAPTER_ADDRESSES *)malloc(buf_len);
  if (OB_ISNULL(addresses)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, addresses, &buf_len) == NO_ERROR) {
    for (PIP_ADAPTER_ADDRESSES addr = addresses; addr && !mac_addr_found; addr = addr->Next) {
      if (addr->PhysicalAddressLength == 6 && addr->IfType != IF_TYPE_SOFTWARE_LOOPBACK) {
        MEMCPY(mac_addr_, addr->PhysicalAddress, 6);
        mac_addr_found = true;
      }
    }
    if (!mac_addr_found) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      is_inited_ = true;
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  if (addresses) { free(addresses); }
#elif defined(__APPLE__)
  // macOS: Use getifaddrs to get network interface information
  struct ifaddrs *ifaddrs_list = nullptr;
  struct ifaddrs *ifa = nullptr;
  bool mac_addr_found = false;

  if (getifaddrs(&ifaddrs_list) != 0) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (ifa = ifaddrs_list; ifa != nullptr && OB_SUCC(ret) && !mac_addr_found; ifa = ifa->ifa_next) {
      if (ifa->ifa_addr == nullptr) {
        continue;
      }
      // Check if it's a link-level address (MAC address)
      if (ifa->ifa_addr->sa_family == AF_LINK) {
        // Skip loopback interface
        if ((ifa->ifa_flags & IFF_LOOPBACK) == 0) {
          struct sockaddr_dl *sdl = (struct sockaddr_dl *)ifa->ifa_addr;
          if (sdl->sdl_alen == 6) {  // MAC address is 6 bytes
            MEMCPY(mac_addr_, LLADDR(sdl), 6);
            mac_addr_found = true;
          }
        }
      }
    }
    freeifaddrs(ifaddrs_list);

    if (OB_FAIL(ret)) {
      // Error already logged
    } else if (!mac_addr_found) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      is_inited_ = true;
    }
  }
#elif defined(__ANDROID__)
  // Android: Use getifaddrs with AF_PACKET + sockaddr_ll
  {
    struct ifaddrs *ifaddr = NULL;
    bool mac_addr_found = false;
    if (getifaddrs(&ifaddr) == -1) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      for (struct ifaddrs *ifa = ifaddr; ifa != NULL && !mac_addr_found; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family != AF_PACKET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        struct sockaddr_ll *sll = (struct sockaddr_ll *)ifa->ifa_addr;
        if (sll->sll_halen == 6) {
          MEMCPY(mac_addr_, sll->sll_addr, 6);
          mac_addr_found = true;
        }
      }
      freeifaddrs(ifaddr);
    }
    if (OB_SUCC(ret) && !mac_addr_found) {
      // RFC 4122 section 4.5: when MAC address is unavailable (Android sandbox),
      // use random bytes with the multicast bit set as the node ID.
      int fd = open("/dev/urandom", O_RDONLY);
      if (fd >= 0 && read(fd, mac_addr_, 6) == 6) {
        mac_addr_[0] |= 0x01;  // set multicast bit
        mac_addr_found = true;
        LOG_INFO("using random UUID node ID (no MAC available on Android)");
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      if (fd >= 0) {
        close(fd);
      }
    }
    if (OB_SUCC(ret) && mac_addr_found) {
      is_inited_ = true;
    }
  }
#else
  // Linux: MAC discovery is best effort; entropy failure remains fatal.
  unsigned char candidate[6] = {};
  bool mac_addr_found = false;
  const char *discovery_step = "no usable MAC";
  int discovery_errno = 0;
  struct ifreq ifr = {};
  struct ifconf ifc = {};
  struct ifreq interfaces[1024 / sizeof(struct ifreq)] = {};
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock == -1) {
    discovery_errno = errno;
    discovery_step = "socket";
  } else {
    ifc.ifc_len = sizeof(interfaces);
    ifc.ifc_req = interfaces;
    if (ioctl(sock, SIOCGIFCONF, &ifc) < 0) {
      discovery_errno = errno;
      discovery_step = "SIOCGIFCONF";
    } else {
      ifreq *it = ifc.ifc_req;
      ifreq *end = it + (ifc.ifc_len / sizeof(ifreq));
      for (; it != end; ++it) {
        if (OB_ISNULL(it) || OB_ISNULL(it->ifr_name) || OB_ISNULL(ifr.ifr_name)) {
          continue;
        } else {
          strncpy(ifr.ifr_name, it->ifr_name, IFNAMSIZ - 1);
          ifr.ifr_name[IFNAMSIZ - 1] = '\0';
          if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
            if (!(ifr.ifr_flags & IFF_LOOPBACK)) {
              if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                if (usable_linux_uuid_mac(ifr.ifr_hwaddr)) {
                  MEMCPY(candidate, ifr.ifr_hwaddr.sa_data, 6);
                  mac_addr_found = true;
                  break;
                }
              } else {
                discovery_errno = errno;
                discovery_step = "SIOCGIFHWADDR";
              }
            }
          } else {
            discovery_errno = errno;
            discovery_step = "SIOCGIFFLAGS";
          }
        }
      }//end for
    }
    close(sock);
  }
  if (!mac_addr_found) {
    if (OB_FAIL(fill_linux_random_uuid_node(candidate))) {
      LOG_ERROR("UUID node initialization failed after MAC discovery", K(ret), K(discovery_step), K(discovery_errno));
    } else {
      LOG_WARN("UUID MAC unavailable, using secure random node", K(discovery_step), K(discovery_errno));
    }
  }
  if (OB_SUCC(ret)) {
    // Do not expose a partially read node, even when init is retried.
    MEMCPY(mac_addr_, candidate, sizeof(candidate));
    is_inited_ = true;
  }
#endif
  return ret;
}
