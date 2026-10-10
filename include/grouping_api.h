/*
 * 文件: grouping_api.h
 * 说明: 聚类分组 API 声明。
 * 职责:
 *   - 基于特征计算结果生成 Odess/Palantir 分组元数据。
 *   - 输出供压缩阶段使用的分组统计和来源缓存。
 */

#pragma once

#include <stdint.h>

#include "core_types.h"
#include "feature_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 聚类分组阶段输出。
 */
typedef struct {
  int odess_ready;
  int palantir_ready;
  double odess_grouping_total_time;
  double palantir_grouping_total_time;

  uint64_t odess_grouped_count;
  uint64_t odess_ungrouped_count;

  uint64_t palantir_level0_count;
  uint64_t palantir_level1_count;
  uint64_t palantir_level2_count;
  uint64_t palantir_ungrouped_count;

  U8Vector palantir_source_type_cache;
} GroupingResult;

/**
 * @brief 基于特征结果执行聚类分组。
 * @param feature_result 特征计算结果。
 * @param run_odess 是否执行 Odess 分组。
 * @param run_palantir 是否执行 Palantir 分组。
 * @param grouping_core_id 分组阶段绑定核心 ID，<0 表示不绑定。
 * @param out_result 输出结果对象。
 * @return 成功返回 0，失败返回 -1。
 */
int grouping_build_from_features(const FeatureComputeResult *feature_result,
                                 int run_odess, int run_palantir,
                                 int grouping_core_id,
                                 GroupingResult *out_result);

/**
 * @brief 将缓存的 Palantir 来源类型恢复到全局来源向量。
 * @param result 分组结果对象。
 * @return 成功返回 0，失败返回 -1。
 */
int grouping_restore_palantir_source_type(const GroupingResult *result);

/**
 * @brief 释放分组结果中的动态资源。
 * @param result 结果对象。
 */
void grouping_result_destroy(GroupingResult *result);

#ifdef __cplusplus
}
#endif
