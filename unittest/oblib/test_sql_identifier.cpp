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

#include <cstdlib>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "common/ob_sql_identifier.h"

using namespace oceanbase::common;

namespace
{
class IdentifierTestAllocator : public ObIAllocator
{
public:
  IdentifierTestAllocator() : fail_(false), allocations_(0) {}
  ~IdentifierTestAllocator()
  {
    for (void *ptr : blocks_) {
      std::free(ptr);
    }
  }
  void *alloc(const int64_t size) override
  {
    ++allocations_;
    void *ptr = fail_ ? nullptr : std::malloc(size);
    if (nullptr != ptr) {
      blocks_.push_back(ptr);
    }
    return ptr;
  }
  void *alloc(const int64_t size, const ObMemAttr &) override { return alloc(size); }
  void free(void *) override {}
  bool fail_;
  int64_t allocations_;
private:
  std::vector<void *> blocks_;
};

std::string contents(const ObString &value)
{
  return std::string(value.ptr(), value.length());
}
} // namespace

TEST(TestSqlIdentifier, plain_name_borrows_source)
{
  IdentifierTestAllocator allocator;
  allocator.fail_ = true;
  const ObString src("plain_column");
  ObString dst;
  ASSERT_EQ(OB_SUCCESS, escape_mysql_identifier(allocator, src, dst));
  EXPECT_EQ(src.ptr(), dst.ptr());
  EXPECT_EQ(src.length(), dst.length());
  EXPECT_EQ(0, allocator.allocations_);
}

TEST(TestSqlIdentifier, doubles_each_backtick)
{
  IdentifierTestAllocator allocator;
  struct Case { const char *input; const char *expected; };
  const Case cases[] = {
    {"a`b", "a``b"}, {"`leading", "``leading"}, {"trailing`", "trailing``"},
    {"`", "``"}, {"``", "````"}, {"`a``b`", "``a````b``"},
    {"a'\"b", "a'\"b"}, {"列`名", "列``名"},
  };
  for (const Case &test_case : cases) {
    ObString dst;
    ASSERT_EQ(OB_SUCCESS, escape_mysql_identifier(allocator, ObString(test_case.input), dst));
    EXPECT_EQ(std::string(test_case.expected), contents(dst));
    EXPECT_EQ('\0', dst.ptr()[dst.length()]);
  }
}

TEST(TestSqlIdentifier, respects_length_and_preserves_embedded_bytes)
{
  IdentifierTestAllocator allocator;
  const char source[] = {'a', '`', '\0', '`', 'z'};
  ObString dst;
  ASSERT_EQ(OB_SUCCESS, escape_mysql_identifier(allocator, ObString(4, source), dst));
  const char expected[] = {'a', '`', '`', '\0', '`', '`'};
  EXPECT_EQ(std::string(expected, sizeof(expected)), contents(dst));
  EXPECT_EQ('\0', dst.ptr()[dst.length()]);
}

TEST(TestSqlIdentifier, source_and_destination_may_alias)
{
  IdentifierTestAllocator allocator;
  ObString name("a`b");
  ASSERT_EQ(OB_SUCCESS, escape_mysql_identifier(allocator, name, name));
  EXPECT_EQ("a``b", contents(name));
}

TEST(TestSqlIdentifier, empty_input_clears_destination)
{
  IdentifierTestAllocator allocator;
  ObString dst("previous");
  ASSERT_EQ(OB_SUCCESS, escape_mysql_identifier(allocator, ObString(), dst));
  EXPECT_EQ(nullptr, dst.ptr());
  EXPECT_EQ(0, dst.length());
  EXPECT_EQ(0, allocator.allocations_);
}

TEST(TestSqlIdentifier, allocation_failure_clears_destination)
{
  IdentifierTestAllocator allocator;
  allocator.fail_ = true;
  ObString dst("previous");
  ASSERT_EQ(OB_ALLOCATE_MEMORY_FAILED, escape_mysql_identifier(allocator, ObString("a`b"), dst));
  EXPECT_EQ(nullptr, dst.ptr());
  EXPECT_EQ(0, dst.length());
  EXPECT_EQ(1, allocator.allocations_);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
