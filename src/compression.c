/*
 * 文件: compression.c
 * 说明: 分组压缩与顺序压缩执行模块。
 * 职责:
 *   - 执行分组压缩并汇总各来源统计信息。
 *   - 执行顺序基线压缩以便对照分析。
 *   - 提供分组效果与压缩收益分析工具函数。
 * 依赖: LZ4、ZSTD、OpenMP，以及项目核心类型定义。
 * 备注: 本文件为性能敏感区域，请保持控制流和内存分配策略稳定。
 */

#define _POSIX_C_SOURCE 200809L

#include "../include/compression.h"

#include <lz4frame.h>
#include <omp.h>
#include <zstd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

/**
 * @brief 分组后条目（用于按 base_id 排序）。
 */
typedef struct {
  uint32_t base_id;
  uint32_t chunk_idx;
} ChunkGroupEntry;

/**
 * @brief 连续分组片段定义。
 */
typedef struct {
  size_t start;
  size_t count;
} GroupSpan;

/**
 * @brief 一组压缩结果（LZ4/ZSTD）对。
 */
typedef struct {
  uint64_t lz4_size;
  uint64_t zstd_size;
} PairU64;

/**
 * @brief 获取单调时钟秒值。
 * @return 当前时间（秒）。
 */
static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/**
 * @brief ChunkGroupEntry 排序比较器（先 base_id，后 chunk_idx）。
 * @param a 左元素。
 * @param b 右元素。
 * @return <0/0/>0 同 qsort 约定。
 */
static int cmp_chunk_group_entry(const void *a, const void *b) {
  const ChunkGroupEntry *lhs = (const ChunkGroupEntry *)a;
  const ChunkGroupEntry *rhs = (const ChunkGroupEntry *)b;
  if (lhs->base_id < rhs->base_id) {
    return -1;
  }
  if (lhs->base_id > rhs->base_id) {
    return 1;
  }
  if (lhs->chunk_idx < rhs->chunk_idx) {
    return -1;
  }
  if (lhs->chunk_idx > rhs->chunk_idx) {
    return 1;
  }
  return 0;
}

/**
 * @brief uint32 排序比较器。
 * @param a 左元素。
 * @param b 右元素。
 * @return <0/0/>0 同 qsort 约定。
 */
static int cmp_u32(const void *a, const void *b) {
  uint32_t lhs = *(const uint32_t *)a;
  uint32_t rhs = *(const uint32_t *)b;
  if (lhs < rhs) {
    return -1;
  }
  if (lhs > rhs) {
    return 1;
  }
  return 0;
}

/**
 * @brief 计算块在文件中的字节偏移。
 * @param chunk_index 块索引。
 * @return 字节偏移。
 */
uint64_t safe_chunk_position(uint32_t chunk_index) {
  return (uint64_t)chunk_index * (uint64_t)CHUNK_SIZE;
}

/**
 * @brief 按 chunk->group 映射执行并行分组压缩。
 * @param filename 输入文件路径。
 * @param chunk_meta chunk 分组映射。
 * @param feature_type 方法标签（用于日志）。
 * @return 分组压缩结果。
 */
CompressionResult compress_grouped_parallel(const char *filename,
                                            const U32Vector *chunk_meta,
                                            const char *feature_type) {
  CompressionResult result;
  memset(&result, 0, sizeof(result));

  FILE *file = fopen(filename, "rb");
  if (file == NULL) {
    fprintf(stderr, "无法打开文件: %s\n", filename);
    return result;
  }

  if (fseeko(file, 0, SEEK_END) != 0) {
    fprintf(stderr, "无法定位文件末尾: %s\n", filename);
    fclose(file);
    return result;
  }

  uint64_t actual_file_size = (uint64_t)ftello(file);
  fclose(file);

  uint64_t actual_total_chunks =
      (actual_file_size + (uint64_t)CHUNK_SIZE - 1) / (uint64_t)CHUNK_SIZE;
  uint64_t chunks_to_process = chunk_meta->size < actual_total_chunks
                                   ? (uint64_t)chunk_meta->size
                                   : actual_total_chunks;

  if (chunks_to_process == 0) {
    printf("分组压缩覆盖统计: 目标块数=0, 入组块数=0, 组数=0\n");
    return result;
  }

  ChunkGroupEntry *entries = (ChunkGroupEntry *)malloc(
      (size_t)chunks_to_process * sizeof(ChunkGroupEntry));
  if (entries == NULL) {
    fprintf(stderr, "内存不足: 无法分配分组条目\n");
    return result;
  }

  for (uint64_t i = 0; i < chunks_to_process; ++i) {
    entries[i].base_id = chunk_meta->data[i];
    entries[i].chunk_idx = (uint32_t)i;
  }

  qsort(entries, (size_t)chunks_to_process, sizeof(ChunkGroupEntry),
        cmp_chunk_group_entry);

  GroupSpan *groups =
      (GroupSpan *)malloc((size_t)chunks_to_process * sizeof(GroupSpan));
  if (groups == NULL) {
    fprintf(stderr, "内存不足: 无法分配组边界\n");
    free(entries);
    return result;
  }

  size_t group_count = 0;
  size_t cursor = 0;
  while (cursor < (size_t)chunks_to_process) {
    size_t start = cursor;
    uint32_t base_id = entries[cursor].base_id;
    while (cursor < (size_t)chunks_to_process &&
           entries[cursor].base_id == base_id) {
      cursor++;
    }
    groups[group_count].start = start;
    groups[group_count].count = cursor - start;
    group_count++;
  }

  printf("分组压缩覆盖统计: 目标块数=%llu, 入组块数=%llu, 组数=%zu\n",
         (unsigned long long)chunks_to_process,
         (unsigned long long)chunks_to_process, group_count);

  int num_threads = omp_get_max_threads();
  if (num_threads <= 0) {
    num_threads = 1;
  }
  printf("使用 %d 个线程进行并行压缩 (%s)\n", num_threads, feature_type);

  uint64_t *lz4_sizes_by_thread =
      (uint64_t *)calloc((size_t)num_threads, sizeof(uint64_t));
  uint64_t *zstd_sizes_by_thread =
      (uint64_t *)calloc((size_t)num_threads, sizeof(uint64_t));
  uint64_t *compressed_chunks_by_thread =
      (uint64_t *)calloc((size_t)num_threads, sizeof(uint64_t));
  uint64_t *skipped_batches_by_thread =
      (uint64_t *)calloc((size_t)num_threads, sizeof(uint64_t));
  uint64_t *source_raw_by_thread =
      (uint64_t *)calloc((size_t)num_threads * 4, sizeof(uint64_t));
  double *lz4_source_comp_by_thread =
      (double *)calloc((size_t)num_threads * 4, sizeof(double));
  double *zstd_source_comp_by_thread =
      (double *)calloc((size_t)num_threads * 4, sizeof(double));

  if (lz4_sizes_by_thread == NULL || zstd_sizes_by_thread == NULL ||
      compressed_chunks_by_thread == NULL ||
      skipped_batches_by_thread == NULL || source_raw_by_thread == NULL ||
      lz4_source_comp_by_thread == NULL || zstd_source_comp_by_thread == NULL) {
    fprintf(stderr, "内存不足: 无法分配线程统计缓冲区\n");
    free(entries);
    free(groups);
    free(lz4_sizes_by_thread);
    free(zstd_sizes_by_thread);
    free(compressed_chunks_by_thread);
    free(skipped_batches_by_thread);
    free(source_raw_by_thread);
    free(lz4_source_comp_by_thread);
    free(zstd_source_comp_by_thread);
    return result;
  }

  LZ4F_preferences_t kPrefs;
  memset(&kPrefs, 0, sizeof(kPrefs));
  kPrefs.frameInfo.blockMode = LZ4F_blockLinked;
  kPrefs.frameInfo.blockSizeID = LZ4F_max256KB;

#pragma omp parallel num_threads(num_threads)
  {
    int tid = omp_get_thread_num();
    FILE *local_file = fopen(filename, "rb");
    const size_t max_chunk_count = 16;

#pragma omp for schedule(dynamic)
    for (size_t group_idx = 0; group_idx < group_count; ++group_idx) {
      if (local_file == NULL) {
        skipped_batches_by_thread[tid]++;
        continue;
      }

      const GroupSpan span = groups[group_idx];
      size_t cycle_count = (span.count + max_chunk_count - 1) / max_chunk_count;

      for (size_t cycle_idx = 0; cycle_idx < cycle_count; ++cycle_idx) {
        size_t start_idx = cycle_idx * max_chunk_count;
        size_t end_idx = start_idx + max_chunk_count;
        if (end_idx > span.count) {
          end_idx = span.count;
        }
        size_t actual_chunk_count = end_idx - start_idx;
        size_t src_size = actual_chunk_count * (size_t)CHUNK_SIZE;

        uint8_t *src_buffer = (uint8_t *)malloc(src_size);
        if (src_buffer == NULL) {
          skipped_batches_by_thread[tid]++;
          continue;
        }

        uint64_t group_source_raw[4] = {0, 0, 0, 0};
        uint64_t batch_raw_total = 0;
        int batch_ok = 1;

        for (size_t i = 0; i < actual_chunk_count; ++i) {
          uint32_t chunk_idx = entries[span.start + start_idx + i].chunk_idx;
          uint64_t chunk_pos = safe_chunk_position(chunk_idx);

          if (chunk_pos >= actual_file_size) {
            batch_ok = 0;
            break;
          }

          if (fseeko(local_file, (off_t)chunk_pos, SEEK_SET) != 0) {
            batch_ok = 0;
            break;
          }

          size_t got = fread(src_buffer + i * (size_t)CHUNK_SIZE, 1,
                             (size_t)CHUNK_SIZE, local_file);
          if (got != (size_t)CHUNK_SIZE) {
            if (got > 0) {
              memset(src_buffer + i * (size_t)CHUNK_SIZE + got, 0,
                     (size_t)CHUNK_SIZE - got);
            } else {
              batch_ok = 0;
              break;
            }
          }

          uint8_t source_type = 3;
          if ((size_t)chunk_idx < chunk_source_type.size) {
            source_type = chunk_source_type.data[chunk_idx];
            if (source_type > 3) {
              source_type = 3;
            }
          }

          uint64_t raw_bytes =
              (chunk_pos + (uint64_t)CHUNK_SIZE <= actual_file_size)
                  ? (uint64_t)CHUNK_SIZE
                  : (actual_file_size - chunk_pos);
          group_source_raw[source_type] += raw_bytes;
          batch_raw_total += raw_bytes;
        }

        if (!batch_ok) {
          skipped_batches_by_thread[tid]++;
          free(src_buffer);
          continue;
        }

        size_t lz4_bound = LZ4F_compressFrameBound(src_size, &kPrefs);
        size_t zstd_bound = ZSTD_compressBound(src_size);
        char *lz4_buffer = (char *)malloc(lz4_bound);
        char *zstd_buffer = (char *)malloc(zstd_bound);
        if (lz4_buffer == NULL || zstd_buffer == NULL) {
          skipped_batches_by_thread[tid]++;
          free(src_buffer);
          free(lz4_buffer);
          free(zstd_buffer);
          continue;
        }

        compressed_chunks_by_thread[tid] += (uint64_t)actual_chunk_count;

        size_t lz4_size = LZ4F_compressFrame(lz4_buffer, lz4_bound, src_buffer,
                                             src_size, &kPrefs);
        uint64_t final_lz4_size = 0;
        if (LZ4F_isError(lz4_size)) {
          final_lz4_size = (uint64_t)src_size;
        } else {
          final_lz4_size = ((uint64_t)lz4_size < (uint64_t)src_size)
                               ? (uint64_t)lz4_size
                               : (uint64_t)src_size;
        }
        lz4_sizes_by_thread[tid] += final_lz4_size;

        size_t zstd_size =
            ZSTD_compress(zstd_buffer, zstd_bound, src_buffer, src_size, 10);
        uint64_t final_zstd_size = 0;
        if (ZSTD_isError(zstd_size)) {
          final_zstd_size = (uint64_t)src_size;
        } else {
          final_zstd_size = ((uint64_t)zstd_size < (uint64_t)src_size)
                                ? (uint64_t)zstd_size
                                : (uint64_t)src_size;
        }
        zstd_sizes_by_thread[tid] += final_zstd_size;

        if (batch_raw_total > 0) {
          for (int s = 0; s < 4; ++s) {
            source_raw_by_thread[(size_t)tid * 4 + (size_t)s] +=
                group_source_raw[s];
            if (group_source_raw[s] > 0) {
              double weight =
                  (double)group_source_raw[s] / (double)batch_raw_total;
              lz4_source_comp_by_thread[(size_t)tid * 4 + (size_t)s] +=
                  weight * (double)final_lz4_size;
              zstd_source_comp_by_thread[(size_t)tid * 4 + (size_t)s] +=
                  weight * (double)final_zstd_size;
            }
          }
        }

        free(src_buffer);
        free(lz4_buffer);
        free(zstd_buffer);
      }
    }

    if (local_file != NULL) {
      fclose(local_file);
    }
  }

  uint64_t compressed_chunks = 0;
  uint64_t skipped_batches = 0;

  for (int t = 0; t < num_threads; ++t) {
    result.lz4_size += lz4_sizes_by_thread[t];
    result.zstd_size += zstd_sizes_by_thread[t];
    compressed_chunks += compressed_chunks_by_thread[t];
    skipped_batches += skipped_batches_by_thread[t];

    for (int s = 0; s < 4; ++s) {
      result.source_raw_bytes[s] +=
          source_raw_by_thread[(size_t)t * 4 + (size_t)s];
      result.lz4_source_compressed_bytes[s] +=
          lz4_source_comp_by_thread[(size_t)t * 4 + (size_t)s];
      result.zstd_source_compressed_bytes[s] +=
          zstd_source_comp_by_thread[(size_t)t * 4 + (size_t)s];
    }
  }

  printf("分组压缩完成: 实际压缩块数=%llu/%llu, 跳过批次数=%llu\n",
         (unsigned long long)compressed_chunks,
         (unsigned long long)chunks_to_process,
         (unsigned long long)skipped_batches);

  free(entries);
  free(groups);
  free(lz4_sizes_by_thread);
  free(zstd_sizes_by_thread);
  free(compressed_chunks_by_thread);
  free(skipped_batches_by_thread);
  free(source_raw_by_thread);
  free(lz4_source_comp_by_thread);
  free(zstd_source_comp_by_thread);

  return result;
}

/**
 * @brief 按固定窗口执行顺序基线并行压缩。
 * @param filename 输入文件路径。
 * @param group_size 顺序分组窗口大小。
 * @param out_lz4_size 输出 LZ4 总压缩大小。
 * @param out_zstd_size 输出 ZSTD 总压缩大小。
 */
void compress_sequential_parallel(const char *filename, size_t group_size,
                                  uint64_t *out_lz4_size,
                                  uint64_t *out_zstd_size) {
  if (out_lz4_size != NULL) {
    *out_lz4_size = 0;
  }
  if (out_zstd_size != NULL) {
    *out_zstd_size = 0;
  }

  FILE *file = fopen(filename, "rb");
  if (file == NULL) {
    fprintf(stderr, "无法打开文件: %s\n", filename);
    return;
  }

  if (fseeko(file, 0, SEEK_END) != 0) {
    fprintf(stderr, "无法定位文件末尾: %s\n", filename);
    fclose(file);
    return;
  }

  uint64_t actual_file_size = (uint64_t)ftello(file);
  fclose(file);

  uint64_t actual_total_chunks =
      (actual_file_size + (uint64_t)CHUNK_SIZE - 1) / (uint64_t)CHUNK_SIZE;
  printf("并行顺序压缩 - 实际文件大小: %llu 字节, 实际块数: %llu\n",
         (unsigned long long)actual_file_size,
         (unsigned long long)actual_total_chunks);

  if (group_size == 0) {
    group_size = 1;
  }

  uint64_t total_groups =
      (actual_total_chunks + (uint64_t)group_size - 1) / (uint64_t)group_size;
  printf("预计处理组数: %llu\n", (unsigned long long)total_groups);

  PairU64 *group_results =
      (PairU64 *)calloc((size_t)total_groups, sizeof(PairU64));
  if (group_results == NULL) {
    fprintf(stderr, "内存不足: 无法分配顺序压缩结果数组\n");
    return;
  }

  int num_threads = omp_get_max_threads();
  if (num_threads <= 0) {
    num_threads = 1;
  }
  printf("使用 %d 个线程进行并行顺序压缩\n", num_threads);

  LZ4F_preferences_t kPrefs;
  memset(&kPrefs, 0, sizeof(kPrefs));
  kPrefs.frameInfo.blockMode = LZ4F_blockLinked;
  kPrefs.frameInfo.blockSizeID = LZ4F_max256KB;

#pragma omp parallel num_threads(num_threads)
  {
    FILE *local_file = fopen(filename, "rb");

#pragma omp for schedule(dynamic)
    for (uint64_t group_idx = 0; group_idx < total_groups; ++group_idx) {
      if (local_file == NULL) {
        continue;
      }

      uint64_t start_chunk = group_idx * (uint64_t)group_size;
      uint64_t end_chunk = start_chunk + (uint64_t)group_size;
      if (end_chunk > actual_total_chunks) {
        end_chunk = actual_total_chunks;
      }

      size_t chunk_count = (size_t)(end_chunk - start_chunk);
      size_t max_src_size = chunk_count * (size_t)CHUNK_SIZE;
      uint8_t *src_buffer = (uint8_t *)malloc(max_src_size);
      if (src_buffer == NULL) {
        continue;
      }

      size_t src_size = 0;
      int read_ok = 1;

      for (uint64_t chunk_idx = start_chunk; chunk_idx < end_chunk;
           ++chunk_idx) {
        uint64_t chunk_pos = safe_chunk_position((uint32_t)chunk_idx);
        if (chunk_pos >= actual_file_size) {
          continue;
        }

        size_t bytes_to_read = (size_t)CHUNK_SIZE;
        if (chunk_pos + (uint64_t)CHUNK_SIZE > actual_file_size) {
          bytes_to_read = (size_t)(actual_file_size - chunk_pos);
        }

        if (fseeko(local_file, (off_t)chunk_pos, SEEK_SET) != 0) {
          read_ok = 0;
          break;
        }
        size_t got = fread(src_buffer + src_size, 1, bytes_to_read, local_file);
        if (got != bytes_to_read) {
          read_ok = 0;
          break;
        }
        src_size += bytes_to_read;
      }

      if (!read_ok || src_size == 0) {
        free(src_buffer);
        continue;
      }

      size_t lz4_bound = LZ4F_compressFrameBound(src_size, &kPrefs);
      size_t zstd_bound = ZSTD_compressBound(src_size);
      char *lz4_buffer = (char *)malloc(lz4_bound);
      char *zstd_buffer = (char *)malloc(zstd_bound);
      if (lz4_buffer == NULL || zstd_buffer == NULL) {
        free(src_buffer);
        free(lz4_buffer);
        free(zstd_buffer);
        continue;
      }

      size_t lz4_size = LZ4F_compressFrame(lz4_buffer, lz4_bound, src_buffer,
                                           src_size, &kPrefs);
      size_t zstd_size =
          ZSTD_compress(zstd_buffer, zstd_bound, src_buffer, src_size, 10);

      if (!LZ4F_isError(lz4_size) && !ZSTD_isError(zstd_size)) {
        group_results[group_idx].lz4_size = (uint64_t)lz4_size;
        group_results[group_idx].zstd_size = (uint64_t)zstd_size;
      }

      free(src_buffer);
      free(lz4_buffer);
      free(zstd_buffer);
    }

    if (local_file != NULL) {
      fclose(local_file);
    }
  }

  uint64_t lz4_total_size = 0;
  uint64_t zstd_total_size = 0;
  for (uint64_t i = 0; i < total_groups; ++i) {
    lz4_total_size += group_results[i].lz4_size;
    zstd_total_size += group_results[i].zstd_size;
  }

  free(group_results);

  if (out_lz4_size != NULL) {
    *out_lz4_size = lz4_total_size;
  }
  if (out_zstd_size != NULL) {
    *out_zstd_size = zstd_total_size;
  }
}

/**
 * @brief 执行顺序基线测试并计算压缩比。
 * @param filename 输入文件路径。
 * @param original_size 原始文件大小。
 * @param out_result 输出测试结果。
 * @return 成功返回 0，失败返回 -1。
 */
int test_sequential_group_sizes(const char *filename, uint64_t original_size,
                                SequentialCompressionResult *out_result) {
  if (out_result == NULL) {
    return -1;
  }

  printf("\n=== 无聚类不同分组大小压缩测试 ===\n");

  const size_t group_size = 1;
  uint64_t lz4_size = 0;
  uint64_t zstd_size = 0;

  double start = now_seconds();
  compress_sequential_parallel(filename, group_size, &lz4_size, &zstd_size);
  double end = now_seconds();

  out_result->group_size = group_size;
  out_result->elapsed_sec = end - start;
  out_result->lz4_ratio =
      (lz4_size == 0) ? 0.0 : (double)original_size / (double)lz4_size;
  out_result->zstd_ratio =
      (zstd_size == 0) ? 0.0 : (double)original_size / (double)zstd_size;

  printf("分组大小 %zu: LZ4=%.3fx, ZSTD=%.3fx, 耗时=%.4f 秒\n",
         out_result->group_size, out_result->lz4_ratio, out_result->zstd_ratio,
         out_result->elapsed_sec);

  return 0;
}

/**
 * @brief 生成混合模式测试数据文件。
 * @param filename 输出文件路径。
 * @param size_mb 文件大小（MB）。
 */
void generate_test_file(const char *filename, size_t size_mb) {
  FILE *file = fopen(filename, "wb");
  if (file == NULL) {
    fprintf(stderr, "无法创建测试文件: %s\n", filename);
    return;
  }

  size_t total_size = size_mb * 1024 * 1024;
  uint8_t *buffer = (uint8_t *)malloc(1024 * 1024);
  if (buffer == NULL) {
    fclose(file);
    fprintf(stderr, "内存不足: 无法创建测试数据缓冲区\n");
    return;
  }

  size_t written = 0;
  while (written < total_size) {
    size_t chunk = 1024 * 1024;
    if (written + chunk > total_size) {
      chunk = total_size - written;
    }

    for (size_t i = 0; i < chunk; ++i) {
      size_t pos = written + i;
      if ((pos % 4096) < 3072) {
        buffer[i] = (uint8_t)(pos & 0xFF);
      } else {
        buffer[i] = (uint8_t)(rand() % 256);
      }
    }

    fwrite(buffer, 1, chunk, file);
    written += chunk;
  }

  free(buffer);
  fclose(file);

  printf("生成测试文件: %s (%zu MB)\n", filename, size_mb);
}

/**
 * @brief 输出分组分布统计信息。
 * @param chunk_meta chunk 分组映射。
 * @param chunk_count 样本块数。
 */
void analyze_grouping_effect(const U32Vector *chunk_meta, int chunk_count) {
  if (chunk_count <= 0 || chunk_meta->size == 0) {
    printf("总组数: 0\n");
    printf("最大组大小: 0\n");
    printf("最小组大小: 0\n");
    printf("平均组大小: 0\n");
    printf("数据重复率估计: 0%%\n");
    return;
  }

  size_t n = (size_t)chunk_count;
  if (n > chunk_meta->size) {
    n = chunk_meta->size;
  }

  uint32_t *sorted_ids = (uint32_t *)malloc(n * sizeof(uint32_t));
  if (sorted_ids == NULL) {
    fprintf(stderr, "内存不足: 无法统计分组效果\n");
    return;
  }

  memcpy(sorted_ids, chunk_meta->data, n * sizeof(uint32_t));
  qsort(sorted_ids, n, sizeof(uint32_t), cmp_u32);

  size_t unique_groups = 0;
  size_t max_group_size = 0;
  size_t min_group_size = n;

  size_t i = 0;
  while (i < n) {
    size_t j = i + 1;
    while (j < n && sorted_ids[j] == sorted_ids[i]) {
      j++;
    }
    size_t group_size = j - i;
    unique_groups++;
    if (group_size > max_group_size) {
      max_group_size = group_size;
    }
    if (group_size < min_group_size) {
      min_group_size = group_size;
    }
    i = j;
  }

  double avg_group_size =
      (unique_groups == 0) ? 0.0 : (double)n / (double)unique_groups;
  double duplicate_ratio =
      (n == 0) ? 0.0 : (1.0 - (double)unique_groups / (double)n) * 100.0;

  printf("总组数: %zu\n", unique_groups);
  printf("最大组大小: %zu\n", max_group_size);
  printf("最小组大小: %zu\n", min_group_size);
  printf("平均组大小: %.2f\n", avg_group_size);
  printf("数据重复率估计: %.2f%%\n", duplicate_ratio);

  free(sorted_ids);
}

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
    const char *feature_name) {
  printf("\n=== %s 压缩比提升分析 ===\n", feature_name);

  printf("\n--- LZ4压缩比提升 ---\n");
  if (sequential_result->lz4_ratio > 0.0) {
    double lz4_improvement = grouped_lz4_ratio / sequential_result->lz4_ratio;
    printf("分组大小 %zu: 基线=%.3fx, 聚类=%.3fx, 提升倍率=%.3fx\n",
           sequential_result->group_size, sequential_result->lz4_ratio,
           grouped_lz4_ratio, lz4_improvement);
  } else {
    printf("分组大小 %zu: 基线无效，无法计算提升\n",
           sequential_result->group_size);
  }

  printf("\n--- ZSTD压缩比提升 ---\n");
  if (sequential_result->zstd_ratio > 0.0) {
    double zstd_improvement =
        grouped_zstd_ratio / sequential_result->zstd_ratio;
    printf("分组大小 %zu: 基线=%.3fx, 聚类=%.3fx, 提升倍率=%.3fx\n",
           sequential_result->group_size, sequential_result->zstd_ratio,
           grouped_zstd_ratio, zstd_improvement);
  } else {
    printf("分组大小 %zu: 基线无效，无法计算提升\n",
           sequential_result->group_size);
  }
}
