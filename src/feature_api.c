/*
 * 文件: feature_api.c
 * 说明: 特征计算 API 实现。
 */

#include "../include/feature_api.h"

#include <string.h>
#include <time.h>

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int feature_compute_result_prepare(FeatureComputeResult *out_result,
                                   uint64_t file_size, int run_odess,
                                   int run_palantir) {
  if (out_result == NULL) {
    return -1;
  }
  if (!run_odess && !run_palantir) {
    return -1;
  }

  memset(out_result, 0, sizeof(*out_result));
  out_result->file_size = file_size;
  uint64_t estimated_chunks =
      (file_size + (uint64_t)CHUNK_SIZE - 1) / (uint64_t)CHUNK_SIZE;
  if (run_odess) {
    if (featurevec_reserve(&out_result->odess_features,
                           (size_t)estimated_chunks) != 0) {
      feature_compute_result_destroy(out_result);
      return -1;
    }
  }
  if (run_palantir) {
    if (featurevec_reserve(&out_result->palantir_features,
                           (size_t)estimated_chunks) != 0) {
      feature_compute_result_destroy(out_result);
      return -1;
    }
  }
  return 0;
}

int feature_compute_chunk(const Chunk *chunk, int run_odess, int run_palantir,
                          FeatureComputeResult *out_result) {
  if (chunk == NULL || out_result == NULL) {
    return -1;
  }
  if (!run_odess && !run_palantir) {
    return -1;
  }

  if (run_odess) {
    double odess_start = now_seconds();
    FeatureLevels feature;
    odess_feature_extract(chunk, &feature);
    out_result->odess_total_time += now_seconds() - odess_start;
    if (featurevec_push_back(&out_result->odess_features, &feature) != 0) {
      return -1;
    }
  }

  if (run_palantir) {
    double palantir_start = now_seconds();
    FeatureLevels feature;
    palantir_feature_extract(chunk, &feature);
    out_result->palantir_total_time += now_seconds() - palantir_start;
    if (featurevec_push_back(&out_result->palantir_features, &feature) != 0) {
      return -1;
    }
  }

  out_result->chunk_count++;
  return 0;
}

void feature_compute_result_destroy(FeatureComputeResult *result) {
  if (result == NULL) {
    return;
  }
  featurevec_free(&result->odess_features);
  featurevec_free(&result->palantir_features);
  memset(result, 0, sizeof(*result));
}
