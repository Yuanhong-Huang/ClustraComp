/*
 * 文件: pipeline.c
 * 说明: 从特征提取到压缩报告的端到端实验流程。
 * 职责:
 *   - 驱动 Odess/Palantir 特征提取与分组准备。
 *   - 执行分组压缩与顺序基线压缩实验。
 *   - 输出可对比指标与汇总诊断信息。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "../include/pipeline.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#endif

#include "../include/compression.h"
#include "../include/core_types.h"
#include "../include/feature_api.h"
#include "../include/grouping_api.h"

static GroupingResult g_grouping_result = {0};

/**
 * @brief 特征阶段内存监控状态。
 */
typedef struct {
  uint64_t rss_baseline_bytes;
  uint64_t excluded_baseline_bytes;
  uint64_t peak_rss_bytes;
  uint64_t peak_working_bytes;
} FeatureStageMemoryTracker;

/* 诊断用：整个测试流程的起始单调时间，供各阶段打印累计耗时 */
static double g_overall_start = 0.0;

/**
 * @brief 饱和加法，避免 uint64 溢出。
 * @param lhs 左操作数。
 * @param rhs 右操作数。
 * @return 相加结果；若溢出则返回 UINT64_MAX。
 */
static uint64_t safe_add_u64(uint64_t lhs, uint64_t rhs) {
  return (UINT64_MAX - lhs < rhs) ? UINT64_MAX : (lhs + rhs);
}

/**
 * @brief 从 /proc/self/status 读取指定字段并转换为字节数。
 * @param field 字段名（如 "VmRSS:"）。
 * @return 字节数；读取失败时返回 0。
 */
static uint64_t read_proc_status_bytes(const char *field) {
  FILE *fp = fopen("/proc/self/status", "r");
  if (fp == NULL) {
    return 0;
  }

  char line[256];
  uint64_t bytes = 0;
  size_t field_len = strlen(field);

  while (fgets(line, sizeof(line), fp) != NULL) {
    if (strncmp(line, field, field_len) == 0) {
      unsigned long long value_kb = 0;
      if (sscanf(line + field_len, "%llu", &value_kb) == 1) {
        bytes = (uint64_t)value_kb * 1024ULL;
      }
      break;
    }
  }

  fclose(fp);
  return bytes;
}

static uint64_t current_rss_bytes(void) {
  return read_proc_status_bytes("VmRSS:");
}

/**
 * @brief 读取当前进程历史高水位 RSS（VmHWM）。
 * @return 字节数；读取失败时返回 0。
 */
static uint64_t current_hwm_bytes(void) {
  return read_proc_status_bytes("VmHWM:");
}

/**
 * @brief 估算需要从“工作内存”口径中排除的容量占用。
 * @param io_buffer_bytes I/O 读块缓存字节数。
 * @param odess_features Odess 特征向量。
 * @param palantir_features Palantir 特征向量。
 * @param odess_meta Odess 元数据向量。
 * @param palantir_meta Palantir 元数据向量。
 * @param source_type 来源类型向量。
 * @param palantir_cache Palantir 来源缓存向量。
 * @return 估算的排除项字节数。
 */
static uint64_t estimate_feature_data_reserved_bytes(
    size_t io_buffer_bytes, const FeatureVector *odess_features,
    const FeatureVector *palantir_features, const U32Vector *odess_meta,
    const U32Vector *palantir_meta, const U8Vector *source_type,
    const U8Vector *palantir_cache) {
  uint64_t bytes = (uint64_t)io_buffer_bytes;

  if (odess_features != NULL) {
    bytes = safe_add_u64(bytes, (uint64_t)odess_features->capacity *
                                    (uint64_t)sizeof(FeatureLevels));
  }
  if (palantir_features != NULL) {
    bytes = safe_add_u64(bytes, (uint64_t)palantir_features->capacity *
                                    (uint64_t)sizeof(FeatureLevels));
  }
  if (odess_meta != NULL) {
    bytes = safe_add_u64(bytes, (uint64_t)odess_meta->capacity *
                                    (uint64_t)sizeof(uint32_t));
  }
  if (palantir_meta != NULL) {
    bytes = safe_add_u64(bytes, (uint64_t)palantir_meta->capacity *
                                    (uint64_t)sizeof(uint32_t));
  }
  if (source_type != NULL) {
    bytes = safe_add_u64(bytes, (uint64_t)source_type->capacity *
                                    (uint64_t)sizeof(uint8_t));
  }
  if (palantir_cache != NULL) {
    bytes = safe_add_u64(bytes, (uint64_t)palantir_cache->capacity *
                                    (uint64_t)sizeof(uint8_t));
  }

  return bytes;
}

static void memory_tracker_init(FeatureStageMemoryTracker *tracker,
                                uint64_t excluded_now_bytes) {
  uint64_t rss_now = current_rss_bytes();
  tracker->rss_baseline_bytes = rss_now;
  tracker->excluded_baseline_bytes = excluded_now_bytes;
  tracker->peak_rss_bytes = rss_now;
  tracker->peak_working_bytes = 0;
}

/**
 * @brief 采样并更新阶段内 RSS 峰值与“额外工作内存”峰值。
 * @param tracker 内存跟踪器。
 * @param excluded_now_bytes 当前排除项估算值。
 */
static void memory_tracker_sample(FeatureStageMemoryTracker *tracker,
                                  uint64_t excluded_now_bytes) {
  uint64_t rss_now = current_rss_bytes();
  if (rss_now > tracker->peak_rss_bytes) {
    tracker->peak_rss_bytes = rss_now;
  }

  uint64_t delta_rss = (rss_now > tracker->rss_baseline_bytes)
                           ? (rss_now - tracker->rss_baseline_bytes)
                           : 0;
  uint64_t delta_excluded =
      (excluded_now_bytes > tracker->excluded_baseline_bytes)
          ? (excluded_now_bytes - tracker->excluded_baseline_bytes)
          : 0;
  uint64_t working_delta =
      (delta_rss > delta_excluded) ? (delta_rss - delta_excluded) : 0;

  if (working_delta > tracker->peak_working_bytes) {
    tracker->peak_working_bytes = working_delta;
  }
}

/**
 * @brief 字节转 MB。
 * @param bytes 字节数。
 * @return MB 值。
 */
static double bytes_to_mb(uint64_t bytes) {
  return (double)bytes / (1024.0 * 1024.0);
}

/**
 * @brief 获取单调时钟秒值。
 * @return 当前时间（秒）。
 */
static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int has_ext_ignore_case(const char *filename, const char *ext) {
  if (filename == NULL || ext == NULL) {
    return 0;
  }
  size_t file_len = strlen(filename);
  size_t ext_len = strlen(ext);
  if (file_len < ext_len) {
    return 0;
  }
  const char *tail = filename + (file_len - ext_len);
  for (size_t i = 0; i < ext_len; ++i) {
    unsigned char a = (unsigned char)tail[i];
    unsigned char b = (unsigned char)ext[i];
    if (tolower(a) != tolower(b)) {
      return 0;
    }
  }
  return 1;
}

static void resolve_method_by_filetype(const char *filename, int *run_odess,
                                       int *run_palantir) {
  if (run_odess == NULL || run_palantir == NULL) {
    return;
  }
  if (has_ext_ignore_case(filename, ".ibd")) {
    *run_odess = 1;
    *run_palantir = 0;
    return;
  }
  *run_odess = 0;
  *run_palantir = 1;
}

#ifdef __linux__
static int online_cpu_count(void) {
  long nproc = sysconf(_SC_NPROCESSORS_ONLN);
  return (nproc > 0) ? (int)nproc : 1;
}

static int bind_current_thread_to_core(int core_id) {
  if (core_id < 0) {
    return 0;
  }

  int nproc = online_cpu_count();
  if (core_id >= nproc) {
    fprintf(stderr,
            "警告：pipeline 特征阶段指定核心 %d 超出范围 [0, "
            "%d)，将使用系统默认调度\n",
            core_id, nproc);
    return -1;
  }

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);

  if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
    fprintf(stderr,
            "警告：pipeline 特征阶段绑定核心 %d 失败: %s，将使用系统默认调度\n",
            core_id, strerror(errno));
    return -1;
  }

  return 0;
}
#endif

/**
 * @brief 安全计算压缩比。
 * @param original_size 原始大小。
 * @param compressed_size 压缩后大小。
 * @return 压缩比；当 compressed_size 为 0 时返回 0。
 */
static double safe_compression_ratio(uint64_t original_size,
                                     uint64_t compressed_size) {
  if (compressed_size == 0) {
    return 0.0;
  }
  return (double)original_size / (double)compressed_size;
}

/**
 * @brief 打印 Palantir 各来源块压缩统计。
 * @param palantir_result Palantir 压缩结果。
 */
static void print_palantir_source_compression_stats(
    const CompressionResult *palantir_result) {
  static const char *source_names[4] = {"Level 0", "Level 1", "Level 2",
                                        "未聚合"};

  printf("\n=== Palantir 各来源块压缩率统计 ===\n");
  for (int s = 0; s < 4; ++s) {
    uint64_t raw_size = palantir_result->source_raw_bytes[s];
    if (raw_size == 0) {
      printf("%s: 无数据\n", source_names[s]);
      continue;
    }
    double raw_size_mb = (double)raw_size / (1024.0 * 1024.0);

    double lz4_ratio =
        (palantir_result->lz4_source_compressed_bytes[s] <= 0.0)
            ? 0.0
            : (double)raw_size /
                  palantir_result->lz4_source_compressed_bytes[s];
    double zstd_ratio =
        (palantir_result->zstd_source_compressed_bytes[s] <= 0.0)
            ? 0.0
            : (double)raw_size /
                  palantir_result->zstd_source_compressed_bytes[s];

    printf("%s: 原始=%.3f MB, LZ4=%.3fx, ZSTD=%.3fx\n", source_names[s],
           raw_size_mb, lz4_ratio, zstd_ratio);
  }
}

/**
 * @brief 阶段 1：提取特征并构建分组元数据。
 * @param filename 输入文件路径。
 * @param feature_core_id 特征计算阶段绑定核心 ID，<0 表示不绑定。
 * @param grouping_core_id 分组阶段绑定核心 ID，<0 表示不绑定。
 * @param manual_method 手动指定算法：0=自动选择，1=Odess，2=Palantir。
 */
void test_file_feature_extraction(const char *filename, int feature_core_id,
                                  int grouping_core_id, int manual_method) {
  int run_odess = 0;
  int run_palantir = 0;

  if (manual_method == 1) {
    // 用户手动指定 Odess
    run_odess = 1;
    run_palantir = 0;
  } else if (manual_method == 2) {
    // 用户手动指定 Palantir
    run_odess = 0;
    run_palantir = 1;
  } else {
    // 自动根据文件类型选择
    resolve_method_by_filetype(filename, &run_odess, &run_palantir);
  }

  grouping_result_destroy(&g_grouping_result);

  FeatureStageMemoryTracker memory_tracker = {0, 0, 0, 0};
  uint64_t excluded_now_bytes = estimate_feature_data_reserved_bytes(
      (size_t)CHUNK_SIZE, NULL, NULL, &chunkMeta_odess, &chunkMeta_palantir,
      &chunk_source_type, &g_grouping_result.palantir_source_type_cache);
  memory_tracker_init(&memory_tracker, excluded_now_bytes);
  memory_tracker_sample(&memory_tracker, excluded_now_bytes);

  FeatureComputeResult feature_result = {0};

  double total_start = now_seconds();
  g_overall_start = total_start;

  FILE *file = fopen(filename, "rb");
  if (file == NULL) {
    fprintf(stderr, "无法打开文件: %s\n", filename);
    return;
  }
  if (fseeko(file, 0, SEEK_END) != 0) {
    fprintf(stderr, "无法定位文件末尾\n");
    fclose(file);
    return;
  }
  uint64_t file_size = (uint64_t)ftello(file);
  if (fseeko(file, 0, SEEK_SET) != 0) {
    fprintf(stderr, "无法重置文件流到开头\n");
    fclose(file);
    return;
  }

  fprintf(stderr,
          "\n[DIAG] 阶段1(特征提取)开始 文件=%llu字节 RSS=%.1f MB\n\n",
          (unsigned long long)file_size,
          (double)current_rss_bytes() / (1024.0 * 1024.0));
  fflush(stderr);

  if (feature_compute_result_prepare(&feature_result, file_size, run_odess,
                                     run_palantir) != 0) {
    fprintf(stderr, "内存不足: 无法初始化特征计算结果\n");
    fclose(file);
    return;
  }

  uint8_t *buffer = (uint8_t *)malloc((size_t)CHUNK_SIZE);
  if (buffer == NULL) {
    fprintf(stderr, "内存不足: 无法分配读取缓冲区\n");
    feature_compute_result_destroy(&feature_result);
    fclose(file);
    return;
  }

#ifdef __linux__
  cpu_set_t original_affinity;
  int has_saved_affinity = (sched_getaffinity(0, sizeof(original_affinity),
                                              &original_affinity) == 0);
  (void)bind_current_thread_to_core(feature_core_id);
#endif

  while (1) {
    size_t bytes_read = fread(buffer, 1, (size_t)CHUNK_SIZE, file);
    if (bytes_read == 0) {
      break;
    }

    Chunk chunk;
    if (chunk_init_copy(&chunk, buffer, (int)bytes_read,
                        (uint32_t)feature_result.chunk_count) != 0) {
      fprintf(stderr, "内存不足: 无法构造数据块\n");
      break;
    }

    if (feature_compute_chunk(&chunk, run_odess, run_palantir,
                              &feature_result) != 0) {
      fprintf(stderr, "内存不足: 无法保存块级特征\n");
      chunk_destroy(&chunk);
      break;
    }
    chunk_destroy(&chunk);

    /* 诊断：每 500 万块打印进度/瞬时吞吐/当前RSS，定位为何超8h */
    if (feature_result.chunk_count % 5000000 == 0) {
      double el = now_seconds() - total_start;
      double mbps = (double)feature_result.chunk_count * (double)CHUNK_SIZE /
                    (1024.0 * 1024.0) / (el > 0 ? el : 1);
      uint64_t total_chunks =
          (file_size + (uint64_t)CHUNK_SIZE - 1) / (uint64_t)CHUNK_SIZE;
      fprintf(stderr,
              "\n[DIAG 特征] 块=%d/约%llu (%.1f%%) 耗时=%.1fs 速度=%.1f MB/s "
              "RSS=%.1f MB\n\n",
              feature_result.chunk_count,
              (unsigned long long)total_chunks,
              100.0 * (double)feature_result.chunk_count /
                  (double)total_chunks,
              el, mbps,
              (double)current_rss_bytes() / (1024.0 * 1024.0));
      fflush(stderr);
    }

    if (bytes_read < (size_t)CHUNK_SIZE) {
      break;
    }
  }

  if (ferror(file)) {
    fprintf(stderr, "警告：文件读取过程中发生错误\n");
  }

#ifdef __linux__
  if (has_saved_affinity) {
    if (sched_setaffinity(0, sizeof(original_affinity), &original_affinity) !=
        0) {
      fprintf(stderr, "警告：恢复 CPU 亲和性失败: %s\n", strerror(errno));
    }
  }
#endif

  free(buffer);
  fclose(file);

  if (feature_result.chunk_count <= 0) {
    feature_compute_result_destroy(&feature_result);
    return;
  }

  if (grouping_build_from_features(&feature_result, run_odess, run_palantir,
                                   grouping_core_id, &g_grouping_result) != 0) {
    feature_compute_result_destroy(&feature_result);
    return;
  }

  file_size = feature_result.file_size;
  int chunk_count = feature_result.chunk_count;
  uint64_t estimated_chunks =
      (file_size + (uint64_t)CHUNK_SIZE - 1) / (uint64_t)CHUNK_SIZE;

  printf("=== 第一部分：特征计算与分组测试 ===\n");
  printf("开始处理文件: %s\n", filename);
  printf("文件大小: %llu 字节 (%.2f MB)\n", (unsigned long long)file_size,
         (double)file_size / (1024.0 * 1024.0));
  printf("块大小: %d 字节\n", CHUNK_SIZE);
  printf("理论块数: %llu\n", (unsigned long long)estimated_chunks);
  printf("自动选择方法: %s\n", run_odess ? "Odess" : "Palantir");
  printf("----------------------------------------\n");

  printf("----------------------------------------\n");
  printf("特征提取完成，实际读取块数：%d\n", chunk_count);

  if (run_odess) {
    printf("\n=== Odess 分组统计 ===\n");
    printf("匹配分组块数：%llu\n",
           (unsigned long long)g_grouping_result.odess_grouped_count);
    printf("未匹配块数：%llu\n",
           (unsigned long long)g_grouping_result.odess_ungrouped_count);
    analyze_grouping_effect(&chunkMeta_odess, chunk_count);
  }

  if (run_palantir) {
    printf("\n=== Palantir 分组统计 ===\n");
    printf("Level 0 分组块数：%llu\n",
           (unsigned long long)g_grouping_result.palantir_level0_count);
    printf("Level 1 分组块数：%llu\n",
           (unsigned long long)g_grouping_result.palantir_level1_count);
    printf("Level 2 分组块数：%llu\n",
           (unsigned long long)g_grouping_result.palantir_level2_count);
    printf("未聚合块数：%llu\n",
           (unsigned long long)g_grouping_result.palantir_ungrouped_count);
    printf("来源统计总块数：%llu\n",
           (unsigned long long)(g_grouping_result.palantir_level0_count +
                                g_grouping_result.palantir_level1_count +
                                g_grouping_result.palantir_level2_count +
                                g_grouping_result.palantir_ungrouped_count));
    analyze_grouping_effect(&chunkMeta_palantir, chunk_count);
  }

  excluded_now_bytes = estimate_feature_data_reserved_bytes(
      (size_t)CHUNK_SIZE, &feature_result.odess_features,
      &feature_result.palantir_features, &chunkMeta_odess, &chunkMeta_palantir,
      &chunk_source_type, &g_grouping_result.palantir_source_type_cache);
  memory_tracker_sample(&memory_tracker, excluded_now_bytes);

  double total_time = now_seconds() - total_start;
  double total_mbytes =
      (double)chunk_count * (double)CHUNK_SIZE / (1024.0 * 1024.0);

  printf("\n=== 时间统计 ===\n");
  printf("总耗时: %.6f 秒\n", total_time);

  if (run_odess) {
    double odess_feature_total_time = feature_result.odess_total_time;
    double odess_total_feature_clustering_time =
        odess_feature_total_time + g_grouping_result.odess_grouping_total_time;
    printf("Odess Feature(特征计算) 总耗时: %.6f 秒\n",
           odess_feature_total_time);
    printf("Odess Grouping(聚类) 总耗时: %.6f 秒\n",
           g_grouping_result.odess_grouping_total_time);
    printf("Odess 特征计算+聚类总耗时: %.6f 秒\n",
           odess_total_feature_clustering_time);
  }

  if (run_palantir) {
    double palantir_feature_total_time = feature_result.palantir_total_time;
    double palantir_total_feature_clustering_time =
        palantir_feature_total_time +
        g_grouping_result.palantir_grouping_total_time;
    printf("Palantir Feature(特征计算) 总耗时: %.6f 秒\n",
           palantir_feature_total_time);
    printf("Palantir Grouping(聚类) 总耗时: %.6f 秒\n",
           g_grouping_result.palantir_grouping_total_time);
    printf("Palantir 特征计算+聚类总耗时: %.6f 秒\n",
           palantir_total_feature_clustering_time);
  }

  printf("\n=== 吞吐量 ===\n");

  if (total_time > 0.0) {
    printf("处理总量: %.2f MB\n", total_mbytes);
    printf("总处理速度: %.2f MB/s\n", total_mbytes / total_time);
  }

  if (run_odess) {
    double odess_feature_total_time = feature_result.odess_total_time;
    double odess_total_feature_clustering_time =
        odess_feature_total_time + g_grouping_result.odess_grouping_total_time;

    if (odess_feature_total_time > 0.0) {
      printf("Odess Feature(特征计算) 处理速度: %.2f MB/s\n",
             total_mbytes / odess_feature_total_time);
    }
    if (g_grouping_result.odess_grouping_total_time > 0.0) {
      printf("Odess 聚类速度: %.2f MB/s\n",
             total_mbytes / g_grouping_result.odess_grouping_total_time);
    }
    if (odess_total_feature_clustering_time > 0.0) {
      printf("Odess 特征计算+聚类总速度: %.2f MB/s\n",
             total_mbytes / odess_total_feature_clustering_time);
    }
  }

  if (run_palantir) {
    double palantir_feature_total_time = feature_result.palantir_total_time;
    double palantir_total_feature_clustering_time =
        palantir_feature_total_time +
        g_grouping_result.palantir_grouping_total_time;

    if (palantir_feature_total_time > 0.0) {
      printf("Palantir Feature(特征计算) 处理速度: %.2f MB/s\n",
             total_mbytes / palantir_feature_total_time);
    }
    if (g_grouping_result.palantir_grouping_total_time > 0.0) {
      printf("Palantir 聚类速度: %.2f MB/s\n",
             total_mbytes / g_grouping_result.palantir_grouping_total_time);
    }
    if (palantir_total_feature_clustering_time > 0.0) {
      printf("Palantir 特征计算+聚类总速度: %.2f MB/s\n",
             total_mbytes / palantir_total_feature_clustering_time);
    }
  }

  if (run_odess && run_palantir) {
    double odess_total = feature_result.odess_total_time +
                         g_grouping_result.odess_grouping_total_time;
    double palantir_total = feature_result.palantir_total_time +
                            g_grouping_result.palantir_grouping_total_time;
    if (odess_total > 0.0 && palantir_total > 0.0) {
      printf("\n=== 特征提取+分组性能对比 ===\n");
      printf("Odess 总时间: %.6f 秒\n", odess_total);
      printf("Palantir 总时间: %.6f 秒\n", palantir_total);
      printf("Palantir / Odess 时间比: %.3fx\n", palantir_total / odess_total);
    }
  }

  memory_tracker_sample(&memory_tracker,
                        estimate_feature_data_reserved_bytes(
                            (size_t)CHUNK_SIZE, &feature_result.odess_features,
                            &feature_result.palantir_features, &chunkMeta_odess,
                            &chunkMeta_palantir, &chunk_source_type,
                            &g_grouping_result.palantir_source_type_cache));

  printf("\n=== 内存统计（排除特征/元数据）===\n");
  printf("阶段内 RSS 峰值: %.2f MB\n",
         bytes_to_mb(memory_tracker.peak_rss_bytes));
  printf("运算与聚合过程峰值额外内存: %.2f MB\n",
         bytes_to_mb(memory_tracker.peak_working_bytes));
  if (file_size > 0) {
    double ratio =
        (double)memory_tracker.peak_working_bytes / (double)file_size * 100.0;
    printf("运算与聚合过程峰值额外内存 / 文件大小: %.2f%%\n", ratio);
  }
  uint64_t hwm_bytes = current_hwm_bytes();
  if (hwm_bytes > 0) {
    printf("进程历史 VmHWM 峰值: %.2f MB\n", bytes_to_mb(hwm_bytes));
  }

  feature_compute_result_destroy(&feature_result);
}

/**
 * @brief 阶段 2：使用分组元数据执行压缩实验并输出对比报告。
 * @param filename 输入文件路径。
 * @note 算法由文件类型自动决定（.ibd -> Odess，其它 -> Palantir）。
 */
void test_clustering_compression(const char *filename) {
  int run_odess = 0;
  int run_palantir = 0;
  resolve_method_by_filetype(filename, &run_odess, &run_palantir);

  FILE *file = fopen(filename, "rb");
  if (file == NULL) {
    fprintf(stderr, "无法打开文件: %s\n", filename);
    return;
  }
  if (fseeko(file, 0, SEEK_END) != 0) {
    fclose(file);
    fprintf(stderr, "无法定位文件末尾: %s\n", filename);
    return;
  }
  uint64_t file_size = (uint64_t)ftello(file);
  fclose(file);

  if (run_odess && !g_grouping_result.odess_ready) {
    fprintf(stderr, "错误：请先运行特征提取阶段以生成 Odess 分组结果\n");
    return;
  }
  if (run_palantir && !g_grouping_result.palantir_ready) {
    fprintf(stderr, "错误：请先运行特征提取阶段以生成 Palantir 分组结果\n");
    return;
  }

  int chunk_count = 0;
  if (run_odess) {
    chunk_count = (int)chunkMeta_odess.size;
  }
  if (run_palantir) {
    chunk_count = (int)chunkMeta_palantir.size;
  }

  printf("\n\n=== 第二部分：聚类压缩测试 ===\n");
  fprintf(stderr,
          "\n[DIAG] 进入压缩阶段 累计耗时=%.1fs RSS=%.1f MB\n\n",
          now_seconds() - g_overall_start,
          (double)current_rss_bytes() / (1024.0 * 1024.0));
  fflush(stderr);
  printf("文件大小: %llu 字节 (%.2f MB)\n", (unsigned long long)file_size,
         (double)file_size / (1024.0 * 1024.0));
  printf("特征提取块数: %d\n", chunk_count);
  printf("----------------------------------------\n");

  /* 基线顺序压缩（已注释）
  printf("\n1. 无聚类顺序压缩测试:\n");
  SequentialCompressionResult sequential_result;
  if (test_sequential_group_sizes(filename, file_size, &sequential_result) !=
      0) {
    fprintf(stderr, "错误：顺序压缩测试失败\n");
    return;
  }

  const double sequential_lz4_ratio = sequential_result.lz4_ratio;
  const double sequential_zstd_ratio = sequential_result.zstd_ratio;
  */

  typedef struct {
    const char *method_name;
    CompressionResult result;
    double lz4_ratio;
    double zstd_ratio;
    double duration_s;
  } MethodCompressionReport;

  MethodCompressionReport reports[2];
  int report_count = 0;

  if (run_odess) {
    printf("\n1. Odess 聚类压缩测试:\n");
    double start = now_seconds();
    CompressionResult odess_result =
        compress_grouped_parallel(filename, &chunkMeta_odess, "Odess");
    double end = now_seconds();

    double odess_lz4_ratio =
        safe_compression_ratio(file_size, odess_result.lz4_size);
    double odess_zstd_ratio =
        safe_compression_ratio(file_size, odess_result.zstd_size);

    printf("Odess 聚类压缩耗时: %.6f 秒\n", end - start);
    printf("Odess LZ4 压缩比: %.3fx\n", odess_lz4_ratio);
    printf("Odess ZSTD 压缩比: %.3fx\n", odess_zstd_ratio);

    /* analyze_compression_improvement 已随基线顺序压缩一起注释
    analyze_compression_improvement(&sequential_result, odess_lz4_ratio,
                                    odess_zstd_ratio, "Odess");
    */

    reports[report_count].method_name = "Odess";
    reports[report_count].result = odess_result;
    reports[report_count].lz4_ratio = odess_lz4_ratio;
    reports[report_count].zstd_ratio = odess_zstd_ratio;
    reports[report_count].duration_s = end - start;
    report_count++;
  }

  if (run_palantir) {
    printf("\n2. Palantir 聚类压缩测试:\n");

    (void)grouping_restore_palantir_source_type(&g_grouping_result);

    double start = now_seconds();
    CompressionResult palantir_result =
        compress_grouped_parallel(filename, &chunkMeta_palantir, "Palantir");
    double end = now_seconds();

    double palantir_lz4_ratio =
        safe_compression_ratio(file_size, palantir_result.lz4_size);
    double palantir_zstd_ratio =
        safe_compression_ratio(file_size, palantir_result.zstd_size);

    printf("Palantir 聚类压缩耗时: %.6f 秒\n", end - start);
    printf("Palantir LZ4 压缩比: %.3fx\n", palantir_lz4_ratio);
    printf("Palantir ZSTD 压缩比: %.3fx\n", palantir_zstd_ratio);

    print_palantir_source_compression_stats(&palantir_result);
    /* analyze_compression_improvement 已随基线顺序压缩一起注释
    analyze_compression_improvement(&sequential_result, palantir_lz4_ratio,
                                    palantir_zstd_ratio, "Palantir");
    */

    reports[report_count].method_name = "Palantir";
    reports[report_count].result = palantir_result;
    reports[report_count].lz4_ratio = palantir_lz4_ratio;
    reports[report_count].zstd_ratio = palantir_zstd_ratio;
    reports[report_count].duration_s = end - start;
    report_count++;
  }

  printf("\n=== 方法对比汇总（可直接横向比较） ===\n");
  /* 基线顺序压缩已注释
  printf("基线(无聚类): LZ4=%.3fx, ZSTD=%.3fx\n", sequential_lz4_ratio,
         sequential_zstd_ratio);
  */

  for (int i = 0; i < report_count; ++i) {
    printf("%s: LZ4=%.3fx, ZSTD=%.3fx, 耗时=%.6f 秒\n", reports[i].method_name,
           reports[i].lz4_ratio, reports[i].zstd_ratio, reports[i].duration_s);
  }

  if (run_odess && run_palantir && report_count == 2) {
    const MethodCompressionReport *odess_report = &reports[0];
    const MethodCompressionReport *palantir_report = &reports[1];

    printf("\n=== Odess vs Palantir 差异 ===\n");
    printf("LZ4 压缩比差值 (Palantir - Odess): %.3f\n",
           palantir_report->lz4_ratio - odess_report->lz4_ratio);
    printf("ZSTD 压缩比差值 (Palantir - Odess): %.3f\n",
           palantir_report->zstd_ratio - odess_report->zstd_ratio);
    printf("耗时差值 (Palantir - Odess): %.6f 秒\n",
           palantir_report->duration_s - odess_report->duration_s);
  }
}
