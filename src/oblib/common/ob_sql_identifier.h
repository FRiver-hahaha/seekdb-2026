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

#include "lib/alloc/ob_iallocator.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace common
{
// Escape the contents of a MySQL quoted identifier, without adding delimiters.
// The result borrows src when no escaping is needed; otherwise allocator owns it.
inline int escape_mysql_identifier(ObIAllocator &allocator,
                                   const ObString &src,
                                   ObString &dst)
{
  int ret = OB_SUCCESS;
  const int64_t src_len = src.length();
  int64_t dst_len = src_len;
  const char escape_character = '`';
  char *ptr = nullptr;

  if (OB_ISNULL(src.ptr()) || OB_UNLIKELY(0 >= src_len)) {
    dst.assign(nullptr, 0);
  } else if (nullptr == src.find(escape_character)) {
    dst = src;
  } else {
    for (int64_t i = 0; i < src_len; ++i) {
      if (src[i] == escape_character) {
        ++dst_len;
      }
    }
    if (OB_UNLIKELY(dst_len > INT32_MAX)) {
      dst.assign(nullptr, 0);
      ret = OB_SIZE_OVERFLOW;
    } else if (nullptr == (ptr = static_cast<char *>(allocator.alloc(dst_len + 1)))) {
      dst.assign(nullptr, 0);
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t dst_pos = 0;
      for (int64_t i = 0; i < src_len; ++i) {
        if (src[i] == escape_character) {
          ptr[dst_pos++] = escape_character;
        }
        ptr[dst_pos++] = src[i];
      }
      ptr[dst_len] = '\0';
      dst.assign_ptr(ptr, static_cast<ObString::obstr_size_t>(dst_len));
    }
  }
  return ret;
}
} // namespace common
} // namespace oceanbase
