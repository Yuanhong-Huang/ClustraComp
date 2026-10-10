/*
 * 文件: indexing.h
 * 说明: 基于特征分组的索引结构与对外接口。
 * 职责:
 *   - 定义哈希映射别名与索引对象模型。
 *   - 声明 Odess/Palantir 索引生命周期与查询接口。
 *   - 将索引层契约与 pipeline 编排逻辑解耦。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_types.h"

#define NAME vt_u32_u32_map
#define KEY_TY uint32_t
#define VAL_TY uint32_t
#define HASH_FN vt_hash_integer
#define CMPR_FN vt_cmpr_integer
#include "verstable.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief uint32->uint32 哈希映射别名。
 * @note 由 verstable 模板实例化得到。
 */
typedef vt_u32_u32_map HashMapU32;

/* 通用 uint32->uint32 哈希映射封装接口 */

/**
 * @brief 初始化哈希映射。
 * @param map 目标映射。
 * @param initial_capacity 期望初始容量。
 * @return 成功返回 0，失败返回 -1。
 */
int hashmap_u32_init(HashMapU32 *map, size_t initial_capacity);

/**
 * @brief 释放哈希映射内部资源。
 * @param map 目标映射。
 */
void hashmap_u32_free(HashMapU32 *map);

/**
 * @brief 尝试插入键值；若键已存在返回既有值。
 * @param map 目标映射。
 * @param key 键。
 * @param value 候选插入值。
 * @param out_value 输出最终值（已存在或新插入）。
 * @param out_inserted 输出是否发生插入（1 表示新插入，0 表示已存在）。
 * @return 成功返回 0，失败返回 -1。
 */
int hashmap_u32_try_emplace(HashMapU32 *map, uint32_t key, uint32_t value,
                            uint32_t *out_value, int *out_inserted);

/**
 * @brief 查询指定键是否存在。
 * @param map 目标映射。
 * @param key 键。
 * @param out_value 命中时输出对应值。
 * @return 命中返回 1，未命中返回 0。
 */
int hashmap_u32_get(const HashMapU32 *map, uint32_t key, uint32_t *out_value);

/**
 * @brief 写入键值（插入或覆盖语义由底层容器实现决定）。
 * @param map 目标映射。
 * @param key 键。
 * @param value 值。
 * @return 成功返回 0，失败返回 -1。
 */
int hashmap_u32_set(HashMapU32 *map, uint32_t key, uint32_t value);

/**
 * @brief Super-feature 分层索引。
 */
typedef struct {
  HashMapU32 *index;
  int super_feature_count;
  int feature_type;
} SuperFeatureIndex;

/* Super feature 索引接口 */

/**
 * @brief 初始化 Super-feature 索引。
 * @param index 目标索引对象。
 * @param super_feature_count 子特征数量。
 * @param feature_type 特征类型标识。
 * @return 成功返回 0，失败返回 -1。
 */
int super_feature_index_init(SuperFeatureIndex *index, int super_feature_count,
                             int feature_type);

/**
 * @brief 销毁 Super-feature 索引。
 * @param index 目标索引对象。
 */
void super_feature_index_destroy(SuperFeatureIndex *index);

/**
 * @brief Palantir 模式下查询/插入并返回基准块信息（含首插标记）。
 * @param index 目标索引对象。
 * @param super_feature 当前块 super-feature 数组。
 * @param chunk_id 当前块 ID。
 * @param is_first_insert 输出是否为首插。
 * @param base_chunk_id 输出基准块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int super_feature_index_get_base_id_palantir_with_insert_flag(
    SuperFeatureIndex *index, const uint32_t *super_feature, int chunk_id,
    int *is_first_insert, uint32_t *base_chunk_id);

/**
 * @brief Odess 模式下查询/插入并解析基准块 ID。
 * @param index 目标索引对象。
 * @param super_feature 当前块 super-feature 数组。
 * @param chunk_id 当前块 ID。
 * @param found 输出是否命中已有分组。
 * @param base_chunk_id 输出基准块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int super_feature_index_get_base_id_odess(SuperFeatureIndex *index,
                                          const uint32_t *super_feature,
                                          int chunk_id, int *found,
                                          uint32_t *base_chunk_id);

/**
 * @brief 向 Odess 索引写入一个块的 super-feature。
 * @param index 目标索引对象。
 * @param super_feature super-feature 数组。
 * @param id 块 ID。
 */
void super_feature_index_add_feature_odess(SuperFeatureIndex *index,
                                           const uint32_t *super_feature,
                                           int id);

/**
 * @brief Palantir 模式下查询/插入并解析基准块 ID。
 * @param index 目标索引对象。
 * @param super_feature 当前块 super-feature 数组。
 * @param chunk_id 当前块 ID。
 * @param found 输出是否命中已有分组。
 * @param base_chunk_id 输出基准块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int super_feature_index_get_base_id_palantir(SuperFeatureIndex *index,
                                             const uint32_t *super_feature,
                                             int chunk_id, int *found,
                                             uint32_t *base_chunk_id);

/**
 * @brief 向 Palantir 索引写入一个块的 super-feature。
 * @param index 目标索引对象。
 * @param super_feature super-feature 数组。
 * @param id 块 ID。
 */
void super_feature_index_add_feature_palantir(SuperFeatureIndex *index,
                                              const uint32_t *super_feature,
                                              int id);

/**
 * @brief Odess 多层索引对象。
 */
typedef struct {
  SuperFeatureIndex *levels;
  int level_count;
} OdessIndex;

/* Odess 索引接口 */

/**
 * @brief 初始化 Odess 索引。
 * @param index 目标索引对象。
 * @return 成功返回 0，失败返回 -1。
 */
int odess_index_init(OdessIndex *index);

/**
 * @brief 销毁 Odess 索引。
 * @param index 目标索引对象。
 */
void odess_index_destroy(OdessIndex *index);

/**
 * @brief 查询 Odess 基准块 ID。
 * @param index Odess 索引对象。
 * @param feature 输入特征。
 * @param chunk_id 当前块 ID。
 * @param found 输出是否命中已有分组。
 * @param base_chunk_id 输出基准块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int odess_index_get_base_id(OdessIndex *index, const FeatureLevels *feature,
                            int chunk_id, int *found, uint32_t *base_chunk_id);

/**
 * @brief Palantir 多层索引对象。
 */
typedef struct {
  SuperFeatureIndex *levels;
  int level_count;
} PalantirIndex;

/* Palantir 索引接口 */

/**
 * @brief 初始化 Palantir 索引。
 * @param index 目标索引对象。
 * @return 成功返回 0，失败返回 -1。
 */
int palantir_index_init(PalantirIndex *index);

/**
 * @brief 销毁 Palantir 索引。
 * @param index 目标索引对象。
 */
void palantir_index_destroy(PalantirIndex *index);

/**
 * @brief 按层查询 Palantir 基准块 ID，并返回命中的层级。
 * @param index Palantir 索引对象。
 * @param feature 输入特征。
 * @param chunk_id 当前块 ID。
 * @param matched_level 输出命中的层级编号。
 * @param found 输出是否命中已有分组。
 * @param base_chunk_id 输出基准块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int palantir_index_get_base_id_by_level(PalantirIndex *index,
                                        const FeatureLevels *feature,
                                        int chunk_id, int *matched_level,
                                        int *found, uint32_t *base_chunk_id);

/**
 * @brief 查询 Palantir 基准块 ID（不关心命中层级）。
 * @param index Palantir 索引对象。
 * @param feature 输入特征。
 * @param chunk_id 当前块 ID。
 * @param found 输出是否命中已有分组。
 * @param base_chunk_id 输出基准块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int palantir_index_get_base_id(PalantirIndex *index,
                               const FeatureLevels *feature, int chunk_id,
                               int *found, uint32_t *base_chunk_id);

#ifdef __cplusplus
}
#endif
