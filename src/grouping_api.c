/*
 * 文件: grouping_api.c
 * 说明: 聚类分组 API 实现。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "../include/grouping_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <errno.h>
#include <sched.h>
#include <unistd.h>
#endif

#include "../include/indexing.h"

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
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
    fprintf(
        stderr,
        "警告：聚类分组 API 指定核心 %d 超出范围 [0, %d)，将使用系统默认调度\n",
        core_id, nproc);
    return -1;
  }

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);

  if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
    fprintf(stderr,
            "警告：聚类分组 API 绑定核心 %d 失败: %s，将使用系统默认调度\n",
            core_id, strerror(errno));
    return -1;
  }

  return 0;
}
#endif

int grouping_build_from_features(const FeatureComputeResult *feature_result,
                                 int run_odess, int run_palantir,
                                 int grouping_core_id,
                                 GroupingResult *out_result) {
  if (feature_result == NULL || out_result == NULL) {
    return -1;
  }
  if (!run_odess && !run_palantir) {
    return -1;
  }

  memset(out_result, 0, sizeof(*out_result));

  const int chunk_count = feature_result->chunk_count;
  if (chunk_count < 0) {
    return -1;
  }

  u32vec_clear(&chunkMeta_odess);
  u32vec_clear(&chunkMeta_palantir);
  u8vec_clear(&chunk_source_type);

#ifdef __linux__
  cpu_set_t original_affinity;
  int has_saved_affinity = (sched_getaffinity(0, sizeof(original_affinity),
                                              &original_affinity) == 0);
  (void)bind_current_thread_to_core(grouping_core_id);
#endif

  if (run_odess) {
    double odess_group_start = now_seconds();
    OdessIndex odess_index;

    if (odess_index_init(&odess_index) != 0) {
      fprintf(stderr, "错误：初始化 Odess 索引失败\n");
#ifdef __linux__
      if (has_saved_affinity) {
        (void)sched_setaffinity(0, sizeof(original_affinity),
                                &original_affinity);
      }
#endif
      return -1;
    }

    if (u32vec_resize(&chunkMeta_odess, (size_t)chunk_count) != 0) {
      fprintf(stderr, "错误：分配 chunkMeta_odess 失败\n");
      odess_index_destroy(&odess_index);
#ifdef __linux__
      if (has_saved_affinity) {
        (void)sched_setaffinity(0, sizeof(original_affinity),
                                &original_affinity);
      }
#endif
      return -1;
    }

    for (int i = 0; i < chunk_count; ++i) {
      int found = 0;
      uint32_t base_chunk_id = (uint32_t)i;
      if (odess_index_get_base_id(&odess_index,
                                  &feature_result->odess_features.data[i], i,
                                  &found, &base_chunk_id) != 0) {
        found = 0;
        base_chunk_id = (uint32_t)i;
      }

      if (found) {
        chunkMeta_odess.data[i] = base_chunk_id;
        out_result->odess_grouped_count++;
      } else {
        chunkMeta_odess.data[i] = (uint32_t)i;
        out_result->odess_ungrouped_count++;
      }
    }

    out_result->odess_grouping_total_time = now_seconds() - odess_group_start;
    out_result->odess_ready = 1;
    odess_index_destroy(&odess_index);
  }

  if (run_palantir) {
    double palantir_group_start = now_seconds();
    PalantirIndex palantir_index;

    if (palantir_index_init(&palantir_index) != 0) {
      fprintf(stderr, "错误：初始化 Palantir 索引失败\n");
#ifdef __linux__
      if (has_saved_affinity) {
        (void)sched_setaffinity(0, sizeof(original_affinity),
                                &original_affinity);
      }
#endif
      return -1;
    }

    if (u32vec_resize(&chunkMeta_palantir, (size_t)chunk_count) != 0 ||
        u8vec_resize(&chunk_source_type, (size_t)chunk_count) != 0) {
      fprintf(stderr, "错误：分配 Palantir 分组缓冲区失败\n");
      palantir_index_destroy(&palantir_index);
#ifdef __linux__
      if (has_saved_affinity) {
        (void)sched_setaffinity(0, sizeof(original_affinity),
                                &original_affinity);
      }
#endif
      return -1;
    }
    memset(chunk_source_type.data, 255, (size_t)chunk_count);

    uint32_t level_next_base_id[3] = {0, 0, 0};
    const uint32_t LEVEL_OFFSET = 100000000U;
    HashMapU32 level_mappings[3];

    for (int m = 0; m < 3; ++m) {
      if (hashmap_u32_init(&level_mappings[m], 1024) != 0) {
        for (int n = 0; n < m; ++n) {
          hashmap_u32_free(&level_mappings[n]);
        }
        palantir_index_destroy(&palantir_index);
        fprintf(stderr, "错误：初始化层映射失败\n");
#ifdef __linux__
        if (has_saved_affinity) {
          (void)sched_setaffinity(0, sizeof(original_affinity),
                                  &original_affinity);
        }
#endif
        return -1;
      }
    }

    for (int i = 0; i < chunk_count; ++i) {
      int matched_level = -1;
      int found = 0;
      uint32_t base_chunk_id = (uint32_t)i;

      if (palantir_index_get_base_id_by_level(
              &palantir_index, &feature_result->palantir_features.data[i], i,
              &matched_level, &found, &base_chunk_id) != 0) {
        found = 0;
        matched_level = -1;
      }

      if (found) {
        uint32_t original_base_id = base_chunk_id;
        if (matched_level == 0) {
          chunkMeta_palantir.data[i] = original_base_id;
          chunk_source_type.data[i] = 0;
          out_result->palantir_level0_count++;
        } else if (matched_level == 1 || matched_level == 2) {
          uint32_t remapped_group_id = 0;
          int has_mapping =
              hashmap_u32_get(&level_mappings[matched_level], original_base_id,
                              &remapped_group_id);
          if (has_mapping) {
            chunkMeta_palantir.data[i] = remapped_group_id;
          } else {
            uint32_t new_group_id = LEVEL_OFFSET * (uint32_t)matched_level +
                                    level_next_base_id[matched_level];
            level_next_base_id[matched_level]++;
            (void)hashmap_u32_set(&level_mappings[matched_level],
                                  original_base_id, new_group_id);
            chunkMeta_palantir.data[i] = new_group_id;
          }

          if (matched_level == 1) {
            chunk_source_type.data[i] = 1;
            out_result->palantir_level1_count++;
          } else {
            chunk_source_type.data[i] = 2;
            out_result->palantir_level2_count++;
          }
        }
      } else {
        chunkMeta_palantir.data[i] = (uint32_t)i;
        chunk_source_type.data[i] = 3;
        out_result->palantir_ungrouped_count++;
      }
    }

    (void)u8vec_copy(&out_result->palantir_source_type_cache,
                     &chunk_source_type);

    out_result->palantir_grouping_total_time =
        now_seconds() - palantir_group_start;
    out_result->palantir_ready = 1;

    for (int m = 0; m < 3; ++m) {
      hashmap_u32_free(&level_mappings[m]);
    }
    palantir_index_destroy(&palantir_index);
  }

#ifdef __linux__
  if (has_saved_affinity) {
    if (sched_setaffinity(0, sizeof(original_affinity), &original_affinity) !=
        0) {
      fprintf(stderr, "警告：恢复 CPU 亲和性失败: %s\n", strerror(errno));
    }
  }
#endif

  return 0;
}

int grouping_restore_palantir_source_type(const GroupingResult *result) {
  if (result == NULL) {
    return -1;
  }
  if (result->palantir_source_type_cache.size == 0) {
    return 0;
  }
  return u8vec_copy(&chunk_source_type, &result->palantir_source_type_cache);
}

void grouping_result_destroy(GroupingResult *result) {
  if (result == NULL) {
    return;
  }
  u8vec_free(&result->palantir_source_type_cache);
  memset(result, 0, sizeof(*result));
}
