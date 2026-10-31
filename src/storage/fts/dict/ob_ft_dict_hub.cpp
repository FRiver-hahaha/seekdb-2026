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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(shared_dict_map_.create(K_MAX_DICT_BUCKET, "shared_dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  is_inited_ = false;
  // Release the dictionary cache handles held by the shared dictionaries before their
  // backing memory goes away with dict_alloc_.
  for (hash::ObHashMap<ObFTDictInfoKey, SharedDict *>::iterator it = shared_dict_map_.begin();
       it != shared_dict_map_.end();
       ++it) {
    SharedDict *shared = it->second;
    if (OB_NOT_NULL(shared)) {
      if (OB_NOT_NULL(shared->dict_)) {
        shared->dict_->~ObIFTDict();
      }
      if (OB_NOT_NULL(shared->container_)) {
        shared->container_->~ObFTCacheRangeContainer();
      }
    }
  }
  shared_dict_map_.destroy();
  dict_map_.destroy();
  rw_dict_lock_.destroy();
  dict_alloc_.reset();
  return ret;
}
int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}

int ObFTDictHub::lookup_shared_dict(const ObFTDictInfoKey &key, SharedDict *&shared)
{
  int ret = OB_SUCCESS;
  shared = nullptr;
  ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
  if (OB_FAIL(shared_dict_map_.get_refactored(key, shared))) {
    if (OB_HASH_NOT_EXIST == ret) {
      shared = nullptr;
      ret = OB_ENTRY_NOT_EXIST;
    } else {
      STORAGE_FTS_LOG(WARN, "failed to lookup shared dict", K(ret));
    }
  } else if (OB_ISNULL(shared)) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_FTS_LOG(WARN, "shared dict entry has no dict", K(ret));
  }
  return ret;
}

int ObFTDictHub::publish_shared_dict(const ObFTDictInfoKey &key, SharedDict *shared)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(shared)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_FTS_LOG(WARN, "invalid shared dict to publish", K(ret));
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());
    if (OB_FAIL(shared_dict_map_.set_refactored(key, shared, 1 /*overwrite*/))) {
      STORAGE_FTS_LOG(WARN, "failed to publish shared dict", K(ret));
    }
  }
  return ret;
}

int ObFTDictHub::build_shared_dict(const ObFTDictDesc &desc, SharedDict *&shared)
{
  int ret = OB_SUCCESS;
  ObFTCacheRangeContainer *container = nullptr;
  ObIFTDict *dict = nullptr;
  shared = nullptr;

  if (OB_ISNULL(container = OB_NEWx(ObFTCacheRangeContainer, &dict_alloc_, dict_alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    STORAGE_FTS_LOG(WARN, "failed to alloc range container", K(ret));
  } else if (OB_FAIL(load_cache(desc, *container))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
      if (OB_FAIL(build_cache(desc, *container))) {
        STORAGE_FTS_LOG(WARN, "failed to build dict cache", K(ret), K(desc.type_));
      }
    } else {
      STORAGE_FTS_LOG(WARN, "failed to load dict cache", K(ret), K(desc.type_));
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(
                 dict = OB_NEWx(ObFTRangeDict, &dict_alloc_, dict_alloc_, container, desc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    STORAGE_FTS_LOG(WARN, "failed to alloc range dict", K(ret));
  } else if (OB_FAIL(dict->init())) {
    STORAGE_FTS_LOG(WARN, "failed to init range dict", K(ret), K(desc.type_));
  } else if (OB_ISNULL(shared = OB_NEWx(SharedDict, &dict_alloc_, container, dict))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    STORAGE_FTS_LOG(WARN, "failed to alloc shared dict entry", K(ret));
  }

  if (OB_FAIL(ret)) {
    if (OB_NOT_NULL(dict)) {
      dict->~ObIFTDict();
      dict = nullptr;
    }
    if (OB_NOT_NULL(container)) {
      container->~ObFTCacheRangeContainer();
      container = nullptr;
    }
    shared = nullptr;
  }
  return ret;
}

int ObFTDictHub::get_shared_dict(const ObFTDictDesc &desc, ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  dict = nullptr;
  const ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  SharedDict *shared = nullptr;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_SUCC(lookup_shared_dict(key, shared))) {
    dict = shared->dict_;
  } else if (OB_ENTRY_NOT_EXIST != ret) {
    STORAGE_FTS_LOG(WARN, "failed to look up shared dict", K(ret), K(desc.type_));
  } else {
    ret = OB_SUCCESS;
    // Only one thread builds a given dictionary. build_lock_ is separate from
    // rw_dict_lock_ because load_cache()/build_cache() take the bucket lock themselves.
    lib::ObMutexGuard guard(build_lock_);
    shared = nullptr;
    if (OB_SUCC(lookup_shared_dict(key, shared))) {
      dict = shared->dict_;
    } else if (OB_ENTRY_NOT_EXIST != ret) {
      STORAGE_FTS_LOG(WARN, "failed to look up shared dict", K(ret), K(desc.type_));
    } else if (OB_FAIL(build_shared_dict(desc, shared))) {
      STORAGE_FTS_LOG(WARN, "failed to build shared dict", K(ret), K(desc.type_));
    } else if (OB_FAIL(publish_shared_dict(key, shared))) {
      STORAGE_FTS_LOG(WARN, "failed to publish shared dict", K(ret), K(desc.type_));
    } else {
      dict = shared->dict_;
    }
  }
  return ret;
}

} //  namespace storage
} //  namespace oceanbase
