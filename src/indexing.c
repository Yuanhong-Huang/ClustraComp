/*
 * 文件: indexing.c
 * 说明: Odess/Palantir 特征分组索引实现。
 * 职责:
 *   - 封装分组索引使用的哈希映射操作。
 *   - 构建并查询多层 super-feature 索引。
 *   - 返回稳定的基准块 ID 以完成分组归并。
 */

#include "../include/indexing.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief 初始化 uint32->uint32 哈希映射。
 * @param map 目标映射。
 * @param initial_capacity 初始容量建议值。
 * @return 成功返回 0，失败返回 -1。
 */
int hashmap_u32_init(HashMapU32 *map, size_t initial_capacity) {
  vt_u32_u32_map_init(map);
  if (initial_capacity > 0 && !vt_u32_u32_map_reserve(map, initial_capacity)) {
    vt_u32_u32_map_cleanup(map);
    return -1;
  }
  return 0;
}

/**
 * @brief 释放哈希映射内部资源。
 * @param map 目标映射。
 */
void hashmap_u32_free(HashMapU32 *map) { vt_u32_u32_map_cleanup(map); }

/**
 * @brief 尝试插入键值，若已存在则返回既有值。
 * @param map 目标映射。
 * @param key 键。
 * @param value 候选插入值。
 * @param out_value 输出最终值。
 * @param out_inserted 输出是否发生插入。
 * @return 成功返回 0，失败返回 -1。
 */
int hashmap_u32_try_emplace(HashMapU32 *map, uint32_t key, uint32_t value,
                            uint32_t *out_value, int *out_inserted) {
  vt_u32_u32_map_itr itr = vt_u32_u32_map_get(map, key);
  if (!vt_u32_u32_map_is_end(itr)) {
    if (out_value != NULL) {
      *out_value = itr.data->val;
    }
    if (out_inserted != NULL) {
      *out_inserted = 0;
    }
    return 0;
  }

  itr = vt_u32_u32_map_insert(map, key, value);
  if (vt_u32_u32_map_is_end(itr)) {
    return -1;
  }

  if (out_value != NULL) {
    *out_value = itr.data->val;
  }
  if (out_inserted != NULL) {
    *out_inserted = 1;
  }
  return 0;
}

/**
 * @brief 查询键值映射。
 * @param map 目标映射。
 * @param key 键。
 * @param out_value 命中时输出值。
 * @return 命中返回 1，未命中返回 0。
 */
int hashmap_u32_get(const HashMapU32 *map, uint32_t key, uint32_t *out_value) {
  vt_u32_u32_map_itr itr = vt_u32_u32_map_get(map, key);
  if (vt_u32_u32_map_is_end(itr)) {
    return 0;
  }

  if (out_value != NULL) {
    *out_value = itr.data->val;
  }
  return 1;
}

/**
 * @brief 写入键值。
 * @param map 目标映射。
 * @param key 键。
 * @param value 值。
 * @return 成功返回 0，失败返回 -1。
 */
int hashmap_u32_set(HashMapU32 *map, uint32_t key, uint32_t value) {
  vt_u32_u32_map_itr itr = vt_u32_u32_map_insert(map, key, value);
  if (vt_u32_u32_map_is_end(itr)) {
    return -1;
  }
  return 0;
}

/**
 * @brief 初始化 Super-feature 索引。
 * @param index 索引对象。
 * @param super_feature_count 子特征数。
 * @param feature_type 特征类型标识。
 * @return 成功返回 0，失败返回 -1。
 */
int super_feature_index_init(SuperFeatureIndex *index, int super_feature_count,
                             int feature_type) {
  index->super_feature_count = super_feature_count;
  index->feature_type = feature_type;
  index->index =
      (HashMapU32 *)calloc((size_t)super_feature_count, sizeof(HashMapU32));
  if (index->index == NULL) {
    return -1;
  }

  for (int i = 0; i < super_feature_count; ++i) {
    if (hashmap_u32_init(&index->index[i], 1024) != 0) {
      for (int j = 0; j < i; ++j) {
        hashmap_u32_free(&index->index[j]);
      }
      free(index->index);
      index->index = NULL;
      return -1;
    }
  }

  return 0;
}

/**
 * @brief 销毁 Super-feature 索引。
 * @param index 索引对象。
 */
void super_feature_index_destroy(SuperFeatureIndex *index) {
  if (index->index == NULL) {
    return;
  }
  for (int i = 0; i < index->super_feature_count; ++i) {
    hashmap_u32_free(&index->index[i]);
  }
  free(index->index);
  index->index = NULL;
  index->super_feature_count = 0;
  index->feature_type = 0;
}

/**
 * @brief Palantir 路径下查询/插入并返回基准块。
 * @param index 索引对象。
 * @param super_feature super-feature 数组。
 * @param chunk_id 当前块 ID。
 * @param is_first_insert 输出是否首次插入。
 * @param base_chunk_id 输出基准块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int super_feature_index_get_base_id_palantir_with_insert_flag(
    SuperFeatureIndex *index, const uint32_t *super_feature, int chunk_id,
    int *is_first_insert, uint32_t *base_chunk_id) {
  for (int i = 0; i < index->super_feature_count; ++i) {
    uint32_t existing = 0;
    int inserted = 0;
    if (hashmap_u32_try_emplace(&index->index[i], super_feature[i],
                                (uint32_t)chunk_id, &existing,
                                &inserted) != 0) {
      return -1;
    }
    if (!inserted) {
      if (is_first_insert != NULL) {
        *is_first_insert = 0;
      }
      if (base_chunk_id != NULL) {
        *base_chunk_id = existing;
      }
      return 0;
    }
  }

  if (is_first_insert != NULL) {
    *is_first_insert = 1;
  }
  if (base_chunk_id != NULL) {
    *base_chunk_id = (uint32_t)chunk_id;
  }
  return 0;
}

/**
 * @brief Odess 路径下查询/插入并返回基准块。
 * @param index 索引对象。
 * @param super_feature super-feature 数组。
 * @param chunk_id 当前块 ID。
 * @param found 输出是否命中。
 * @param base_chunk_id 输出基准块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int super_feature_index_get_base_id_odess(SuperFeatureIndex *index,
                                          const uint32_t *super_feature,
                                          int chunk_id, int *found,
                                          uint32_t *base_chunk_id) {
  if (found != NULL) {
    *found = 0;
  }

  for (int i = 0; i < index->super_feature_count; ++i) {
    uint32_t existing = 0;
    int inserted = 0;
    if (hashmap_u32_try_emplace(&index->index[i], super_feature[i],
                                (uint32_t)chunk_id, &existing,
                                &inserted) != 0) {
      return -1;
    }

    if (!inserted) {
      uint32_t resolved = existing;
      if ((size_t)existing < chunkMeta_odess.size) {
        resolved = chunkMeta_odess.data[existing];
      }
      if (found != NULL) {
        *found = 1;
      }
      if (base_chunk_id != NULL) {
        *base_chunk_id = resolved;
      }
      return 0;
    }
  }

  return 0;
}

/**
 * @brief 向 Odess 索引写入 super-feature。
 * @param index 索引对象。
 * @param super_feature super-feature 数组。
 * @param id 块 ID。
 */
void super_feature_index_add_feature_odess(SuperFeatureIndex *index,
                                           const uint32_t *super_feature,
                                           int id) {
  for (int i = 0; i < index->super_feature_count; ++i) {
    (void)hashmap_u32_set(&index->index[i], super_feature[i], (uint32_t)id);
  }
}

/**
 * @brief Palantir 路径下查询/插入并返回基准块。
 * @param index 索引对象。
 * @param super_feature super-feature 数组。
 * @param chunk_id 当前块 ID。
 * @param found 输出是否命中。
 * @param base_chunk_id 输出基准块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int super_feature_index_get_base_id_palantir(SuperFeatureIndex *index,
                                             const uint32_t *super_feature,
                                             int chunk_id, int *found,
                                             uint32_t *base_chunk_id) {
  if (found != NULL) {
    *found = 0;
  }

  for (int i = 0; i < index->super_feature_count; ++i) {
    uint32_t existing = 0;
    int inserted = 0;
    if (hashmap_u32_try_emplace(&index->index[i], super_feature[i],
                                (uint32_t)chunk_id, &existing,
                                &inserted) != 0) {
      return -1;
    }

    if (!inserted) {
      uint32_t resolved = existing;
      if ((size_t)existing < chunkMeta_palantir.size) {
        resolved = chunkMeta_palantir.data[existing];
      }
      if (found != NULL) {
        *found = 1;
      }
      if (base_chunk_id != NULL) {
        *base_chunk_id = resolved;
      }
      return 0;
    }
  }

  return 0;
}

/**
 * @brief 向 Palantir 索引写入 super-feature。
 * @param index 索引对象。
 * @param super_feature super-feature 数组。
 * @param id 块 ID。
 */
void super_feature_index_add_feature_palantir(SuperFeatureIndex *index,
                                              const uint32_t *super_feature,
                                              int id) {
  for (int i = 0; i < index->super_feature_count; ++i) {
    (void)hashmap_u32_set(&index->index[i], super_feature[i], (uint32_t)id);
  }
}

/**
 * @brief 初始化 Odess 索引对象。
 * @param index 目标索引。
 * @return 成功返回 0，失败返回 -1。
 */
int odess_index_init(OdessIndex *index) {
  index->level_count = 1;
  index->levels = (SuperFeatureIndex *)calloc((size_t)index->level_count,
                                              sizeof(SuperFeatureIndex));
  if (index->levels == NULL) {
    return -1;
  }

  if (super_feature_index_init(&index->levels[0], 3, 0) != 0) {
    free(index->levels);
    index->levels = NULL;
    return -1;
  }
  return 0;
}

/**
 * @brief 销毁 Odess 索引对象。
 * @param index 目标索引。
 */
void odess_index_destroy(OdessIndex *index) {
  if (index->levels == NULL) {
    return;
  }
  for (int i = 0; i < index->level_count; ++i) {
    super_feature_index_destroy(&index->levels[i]);
  }
  free(index->levels);
  index->levels = NULL;
  index->level_count = 0;
}

/**
 * @brief 查询 Odess 基准块 ID。
 * @param index Odess 索引。
 * @param feature 输入特征。
 * @param chunk_id 当前块 ID。
 * @param found 输出是否命中。
 * @param base_chunk_id 输出基准块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int odess_index_get_base_id(OdessIndex *index, const FeatureLevels *feature,
                            int chunk_id, int *found, uint32_t *base_chunk_id) {
  if (found != NULL) {
    *found = 0;
  }

  const int level_count = (feature->level_count < index->level_count)
                              ? feature->level_count
                              : index->level_count;
  for (int i = 0; i < level_count; ++i) {
    int hit = 0;
    if (super_feature_index_get_base_id_odess(&index->levels[i],
                                              feature->values[i], chunk_id,
                                              &hit, base_chunk_id) != 0) {
      return -1;
    }
    if (hit) {
      if (found != NULL) {
        *found = 1;
      }
      return 0;
    }
  }

  return 0;
}

/**
 * @brief 初始化 Palantir 索引对象。
 * @param index 目标索引。
 * @return 成功返回 0，失败返回 -1。
 */
int palantir_index_init(PalantirIndex *index) {
  index->level_count = 3;
  index->levels = (SuperFeatureIndex *)calloc((size_t)index->level_count,
                                              sizeof(SuperFeatureIndex));
  if (index->levels == NULL) {
    return -1;
  }

  const int sf_counts[3] = {3, 4, 6};
  for (int i = 0; i < index->level_count; ++i) {
    if (super_feature_index_init(&index->levels[i], sf_counts[i], 1) != 0) {
      for (int j = 0; j < i; ++j) {
        super_feature_index_destroy(&index->levels[j]);
      }
      free(index->levels);
      index->levels = NULL;
      return -1;
    }
  }
  return 0;
}

/**
 * @brief 销毁 Palantir 索引对象。
 * @param index 目标索引。
 */
void palantir_index_destroy(PalantirIndex *index) {
  if (index->levels == NULL) {
    return;
  }
  for (int i = 0; i < index->level_count; ++i) {
    super_feature_index_destroy(&index->levels[i]);
  }
  free(index->levels);
  index->levels = NULL;
  index->level_count = 0;
}

/**
 * @brief 分层查询 Palantir 基准块 ID。
 * @param index Palantir 索引。
 * @param feature 输入特征。
 * @param chunk_id 当前块 ID。
 * @param matched_level 输出命中层。
 * @param found 输出是否命中。
 * @param base_chunk_id 输出基准块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int palantir_index_get_base_id_by_level(PalantirIndex *index,
                                        const FeatureLevels *feature,
                                        int chunk_id, int *matched_level,
                                        int *found, uint32_t *base_chunk_id) {
  if (matched_level != NULL) {
    *matched_level = -1;
  }
  if (found != NULL) {
    *found = 0;
  }

  const int level_count = (feature->level_count < index->level_count)
                              ? feature->level_count
                              : index->level_count;
  for (int i = 0; i < level_count; ++i) {
    int hit = 0;
    if (super_feature_index_get_base_id_palantir(&index->levels[i],
                                                 feature->values[i], chunk_id,
                                                 &hit, base_chunk_id) != 0) {
      return -1;
    }
    if (hit) {
      if (matched_level != NULL) {
        *matched_level = i;
      }
      if (found != NULL) {
        *found = 1;
      }
      return 0;
    }
  }

  return 0;
}

/**
 * @brief 查询 Palantir 基准块 ID（忽略命中层级）。
 * @param index Palantir 索引。
 * @param feature 输入特征。
 * @param chunk_id 当前块 ID。
 * @param found 输出是否命中。
 * @param base_chunk_id 输出基准块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int palantir_index_get_base_id(PalantirIndex *index,
                               const FeatureLevels *feature, int chunk_id,
                               int *found, uint32_t *base_chunk_id) {
  int matched_level = -1;
  return palantir_index_get_base_id_by_level(
      index, feature, chunk_id, &matched_level, found, base_chunk_id);
}
