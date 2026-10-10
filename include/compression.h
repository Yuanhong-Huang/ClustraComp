/*
 * 文件: compression.h
 * 说明: 压缩执行与结果分析的对外接口声明。
 * 职责:
 *   - 定义压缩结果模型与实验输出结构体。
 *   - 暴露分组压缩与顺序压缩入口。
 *   - 暴露测试数据生成与结果分析辅助接口。
 * 备注: 本头文件被 pipeline/main 等模块依赖，应保持接口稳定。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 分组压缩结果汇总。
 */
typedef struct {
  uint64_t lz4_size;
  uint64_t zstd_size;
  uint64_t source_raw_bytes[4];
  double lz4_source_compressed_bytes[4];
  double zstd_source_compressed_bytes[4];
} CompressionResult;

/**
 * @brief 顺序基线压缩结果。
 */
typedef struct {
  size_t group_size;
  double lz4_ratio;
  double zstd_ratio;
  double elapsed_sec;
} SequentialCompressionResult;

/* 核心压缩接口 */

/**
 * @brief 计算块在文件中的字节偏移。
 * @param chunk_index 块索引。
 * @return 块起始字节位置。
 */
uint64_t safe_chunk_position(uint32_t chunk_index);

/**
 * @brief 对预分组数据执行并行压缩。
 * @param filename 目标文件路径。
 * @param chunk_meta chunk 到 group/base 的映射。
 * @param feature_type 方法名标签（用于日志）。
 * @return 压缩结果统计。
 */
CompressionResult compress_grouped_parallel(const char *filename,
                                            const U32Vector *chunk_meta,
                                            const char *feature_type);

/**
 * @brief 执行顺序窗口并行压缩（无聚类基线）。
 * @param filename 目标文件路径。
 * @param group_size 顺序分组窗口大小。
 * @param out_lz4_size 输出 LZ4 压缩后总大小。
 * @param out_zstd_size 输出 ZSTD 压缩后总大小。
 */
void compress_sequential_parallel(const char *filename, size_t group_size,
                                  uint64_t *out_lz4_size,
                                  uint64_t *out_zstd_size);

/**
 * @brief 运行顺序基线测试并返回压缩比。
 * @param filename 目标文件路径。
 * @param original_size 原始文件大小。
 * @param out_result 输出测试结果。
 * @return 成功返回 0，失败返回 -1。
 */
int test_sequential_group_sizes(const char *filename, uint64_t original_size,
                                SequentialCompressionResult *out_result);

/**
 * @brief 生成用于实验的测试文件。
 * @param filename 输出文件路径。
 * @param size_mb 目标大小（MB）。
 */
void generate_test_file(const char *filename, size_t size_mb);

/**
 * @brief 分析分组效果（组数、组大小、重复率）。
 * @param chunk_meta chunk 分组映射。
 * @param chunk_count 样本块数。
 */
void analyze_grouping_effect(const U32Vector *chunk_meta, int chunk_count);

/**
 * @brief 对比聚类方案与顺序基线的压缩收益。
 * @param sequential_result 顺序基线结果。
 * @param grouped_lz4_ratio 聚类方案 LZ4 压缩比。
 * @param grouped_zstd_ratio 聚类方案 ZSTD 压缩比。
 * @param feature_name 方法名称。
 */
void analyze_compression_improvement(
    const SequentialCompressionResult *sequential_result,
    double grouped_lz4_ratio, double grouped_zstd_ratio,
    const char *feature_name);

#ifdef __cplusplus
}
#endif
