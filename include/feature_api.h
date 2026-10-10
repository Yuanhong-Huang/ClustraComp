/*
 * 文件: feature_api.h
 * 说明: 特征计算 API 声明。
 * 职责:
 *   - 提供块级特征计算接口。
 *   - 提供特征结果容器的初始化与资源释放能力。
 *   - 输出可用于后续聚类分组阶段的特征结果。
 */

#pragma once

#include <stdint.h>

#include "core_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 特征计算阶段输出。
 */
typedef struct {
  FeatureVector odess_features;
  FeatureVector palantir_features;
  int chunk_count;
  uint64_t file_size;
  double odess_total_time;
  double palantir_total_time;
} FeatureComputeResult;

/**
 * @brief 初始化特征计算结果容器并预留容量。
 * @param out_result 输出结果对象（调用后将被重置）。
 * @param file_size 输入文件大小（字节）。
 * @param run_odess 是否计算 Odess 特征。
 * @param run_palantir 是否计算 Palantir 特征。
 * @return 成功返回 0，失败返回 -1。
 */
int feature_compute_result_prepare(FeatureComputeResult *out_result,
                                   uint64_t file_size, int run_odess,
                                   int run_palantir);

/**
 * @brief 计算单个数据块的特征并追加到结果容器。
 * @param chunk 输入数据块。
 * @param run_odess 是否计算 Odess 特征。
 * @param run_palantir 是否计算 Palantir 特征。
 * @param out_result 输出结果对象。
 * @return 成功返回 0，失败返回 -1。
 */
int feature_compute_chunk(const Chunk *chunk, int run_odess, int run_palantir,
                          FeatureComputeResult *out_result);

/**
 * @brief 释放特征计算结果中的动态资源。
 * @param result 结果对象。
 */
void feature_compute_result_destroy(FeatureComputeResult *result);

#ifdef __cplusplus
}
#endif
