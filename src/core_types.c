/*
 * 文件: core_types.c
 * 说明: 核心容器、数据块模型与特征提取实现。
 * 职责:
 *   - 提供贯穿全流程使用的动态向量基础能力。
 *   - 提供块对象生命周期与派生指标（如零字节占比）。
 *   - 提供 Odess/Palantir 特征提取基础函数。
 * 备注: 多数函数位于热点路径，应关注分配次数与访存开销。
 */

#include "../include/core_types.h"

#include <immintrin.h>
#include <stdlib.h>
#include <string.h>

#include "../include/gear.h"

const int default_odess_mask = 0x40030341;
int CHUNK_SIZE = 8192;

U32Vector chunkMeta_odess = {0};
U32Vector chunkMeta_palantir = {0};
U8Vector chunk_source_type = {0};

/**
 * @brief 计算满足需求的下一容量。
 * @param current 当前容量。
 * @param needed 需要满足的最小容量。
 * @return 扩展后的容量。
 */
static size_t next_capacity(size_t current, size_t needed) {
  size_t cap = (current == 0) ? 16 : current;
  while (cap < needed) {
    cap *= 2;
  }
  return cap;
}

/* U32Vector 接口实现 */
/**
 * @brief 预留 U32Vector 容量。
 * @param vec 目标向量。
 * @param new_capacity 新容量。
 * @return 成功返回 0，失败返回 -1。
 */
int u32vec_reserve(U32Vector *vec, size_t new_capacity) {
  if (new_capacity <= vec->capacity) {
    return 0;
  }
  uint32_t *new_data =
      (uint32_t *)realloc(vec->data, new_capacity * sizeof(uint32_t));
  if (new_data == NULL) {
    return -1;
  }
  vec->data = new_data;
  vec->capacity = new_capacity;
  return 0;
}

/**
 * @brief 调整 U32Vector 大小。
 * @param vec 目标向量。
 * @param new_size 新大小。
 * @return 成功返回 0，失败返回 -1。
 */
int u32vec_resize(U32Vector *vec, size_t new_size) {
  if (new_size > vec->capacity) {
    if (u32vec_reserve(vec, next_capacity(vec->capacity, new_size)) != 0) {
      return -1;
    }
  }
  if (new_size > vec->size) {
    memset(vec->data + vec->size, 0, (new_size - vec->size) * sizeof(uint32_t));
  }
  vec->size = new_size;
  return 0;
}

/**
 * @brief 向 U32Vector 尾部追加一个元素。
 * @param vec 目标向量。
 * @param value 追加值。
 * @return 成功返回 0，失败返回 -1。
 */
int u32vec_push_back(U32Vector *vec, uint32_t value) {
  if (vec->size == vec->capacity) {
    if (u32vec_reserve(vec, next_capacity(vec->capacity, vec->size + 1)) != 0) {
      return -1;
    }
  }
  vec->data[vec->size++] = value;
  return 0;
}

/**
 * @brief 清空 U32Vector（保留容量）。
 * @param vec 目标向量。
 */
void u32vec_clear(U32Vector *vec) { vec->size = 0; }

/**
 * @brief 释放 U32Vector 资源并重置状态。
 * @param vec 目标向量。
 */
void u32vec_free(U32Vector *vec) {
  free(vec->data);
  vec->data = NULL;
  vec->size = 0;
  vec->capacity = 0;
}

/* U8Vector 接口实现 */
/**
 * @brief 预留 U8Vector 容量。
 * @param vec 目标向量。
 * @param new_capacity 新容量。
 * @return 成功返回 0，失败返回 -1。
 */
int u8vec_reserve(U8Vector *vec, size_t new_capacity) {
  if (new_capacity <= vec->capacity) {
    return 0;
  }
  uint8_t *new_data =
      (uint8_t *)realloc(vec->data, new_capacity * sizeof(uint8_t));
  if (new_data == NULL) {
    return -1;
  }
  vec->data = new_data;
  vec->capacity = new_capacity;
  return 0;
}

/**
 * @brief 调整 U8Vector 大小。
 * @param vec 目标向量。
 * @param new_size 新大小。
 * @return 成功返回 0，失败返回 -1。
 */
int u8vec_resize(U8Vector *vec, size_t new_size) {
  if (new_size > vec->capacity) {
    if (u8vec_reserve(vec, next_capacity(vec->capacity, new_size)) != 0) {
      return -1;
    }
  }
  if (new_size > vec->size) {
    memset(vec->data + vec->size, 0, (new_size - vec->size) * sizeof(uint8_t));
  }
  vec->size = new_size;
  return 0;
}

/**
 * @brief 向 U8Vector 尾部追加一个元素。
 * @param vec 目标向量。
 * @param value 追加值。
 * @return 成功返回 0，失败返回 -1。
 */
int u8vec_push_back(U8Vector *vec, uint8_t value) {
  if (vec->size == vec->capacity) {
    if (u8vec_reserve(vec, next_capacity(vec->capacity, vec->size + 1)) != 0) {
      return -1;
    }
  }
  vec->data[vec->size++] = value;
  return 0;
}

/**
 * @brief 清空 U8Vector（保留容量）。
 * @param vec 目标向量。
 */
void u8vec_clear(U8Vector *vec) { vec->size = 0; }

/**
 * @brief 释放 U8Vector 资源并重置状态。
 * @param vec 目标向量。
 */
void u8vec_free(U8Vector *vec) {
  free(vec->data);
  vec->data = NULL;
  vec->size = 0;
  vec->capacity = 0;
}

/**
 * @brief 拷贝 U8Vector 内容。
 * @param dst 目标向量。
 * @param src 源向量。
 * @return 成功返回 0，失败返回 -1。
 */
int u8vec_copy(U8Vector *dst, const U8Vector *src) {
  if (u8vec_resize(dst, src->size) != 0) {
    return -1;
  }
  if (src->size > 0) {
    memcpy(dst->data, src->data, src->size * sizeof(uint8_t));
  }
  return 0;
}

/* FloatVector 接口实现 */
/**
 * @brief 预留 FloatVector 容量。
 * @param vec 目标向量。
 * @param new_capacity 新容量。
 * @return 成功返回 0，失败返回 -1。
 */
int f32vec_reserve(FloatVector *vec, size_t new_capacity) {
  if (new_capacity <= vec->capacity) {
    return 0;
  }
  float *new_data = (float *)realloc(vec->data, new_capacity * sizeof(float));
  if (new_data == NULL) {
    return -1;
  }
  vec->data = new_data;
  vec->capacity = new_capacity;
  return 0;
}

/**
 * @brief 向 FloatVector 尾部追加一个元素。
 * @param vec 目标向量。
 * @param value 追加值。
 * @return 成功返回 0，失败返回 -1。
 */
int f32vec_push_back(FloatVector *vec, float value) {
  if (vec->size == vec->capacity) {
    size_t target = next_capacity(vec->capacity, vec->size + 1);
    if (f32vec_reserve(vec, target) != 0) {
      return -1;
    }
  }
  vec->data[vec->size++] = value;
  return 0;
}

/**
 * @brief 清空 FloatVector（保留容量）。
 * @param vec 目标向量。
 */
void f32vec_clear(FloatVector *vec) { vec->size = 0; }

/**
 * @brief 释放 FloatVector 资源并重置状态。
 * @param vec 目标向量。
 */
void f32vec_free(FloatVector *vec) {
  free(vec->data);
  vec->data = NULL;
  vec->size = 0;
  vec->capacity = 0;
}

/* FeatureVector 接口实现 */
/**
 * @brief 预留 FeatureVector 容量。
 * @param vec 目标向量。
 * @param new_capacity 新容量。
 * @return 成功返回 0，失败返回 -1。
 */
int featurevec_reserve(FeatureVector *vec, size_t new_capacity) {
  if (new_capacity <= vec->capacity) {
    return 0;
  }
  FeatureLevels *new_data =
      (FeatureLevels *)realloc(vec->data, new_capacity * sizeof(FeatureLevels));
  if (new_data == NULL) {
    return -1;
  }
  vec->data = new_data;
  vec->capacity = new_capacity;
  return 0;
}

/**
 * @brief 向 FeatureVector 尾部追加一个元素。
 * @param vec 目标向量。
 * @param value 追加特征。
 * @return 成功返回 0，失败返回 -1。
 */
int featurevec_push_back(FeatureVector *vec, const FeatureLevels *value) {
  if (vec->size == vec->capacity) {
    if (featurevec_reserve(vec, next_capacity(vec->capacity, vec->size + 1)) !=
        0) {
      return -1;
    }
  }
  vec->data[vec->size++] = *value;
  return 0;
}

/**
 * @brief 清空 FeatureVector（保留容量）。
 * @param vec 目标向量。
 */
void featurevec_clear(FeatureVector *vec) { vec->size = 0; }

/**
 * @brief 释放 FeatureVector 资源并重置状态。
 * @param vec 目标向量。
 */
void featurevec_free(FeatureVector *vec) {
  free(vec->data);
  vec->data = NULL;
  vec->size = 0;
  vec->capacity = 0;
}

/* Chunk 对象生命周期与缓存指标。 */
/**
 * @brief 复制初始化 Chunk。
 * @param chunk 输出块对象。
 * @param buf 输入字节流。
 * @param len 输入长度。
 * @param id 块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int chunk_init_copy(Chunk *chunk, const uint8_t *buf, int len, uint32_t id) {
  if (len < 0) {
    return -1;
  }
  chunk->data = (uint8_t *)malloc((size_t)len);
  if (chunk->data == NULL && len > 0) {
    return -1;
  }
  if (len > 0) {
    memcpy(chunk->data, buf, (size_t)len);
  }
  chunk->len = len;
  chunk->id = id;
  return 0;
}

/**
 * @brief 销毁 Chunk 并释放其数据缓存。
 * @param chunk 目标块对象。
 */
void chunk_destroy(Chunk *chunk) {
  free(chunk->data);
  chunk->data = NULL;
  chunk->len = 0;
  chunk->id = 0;
}

/**
 * @brief SIMD 路径下更新 Odess 子特征最小值。
 * @param h 当前哈希值。
 * @param min_avx AVX 最小值寄存器。
 * @param min_sse SSE 最小值寄存器。
 * @param m0_avx2 AVX 乘法系数。
 * @param a0_avx2 AVX 加法系数。
 * @param m_sse SSE 乘法系数。
 * @param a_sse SSE 加法系数。
 */
static inline void odess_update_min(uint32_t h, __m256i *min_avx,
                                    __m128i *min_sse, const __m256i *m0_avx2,
                                    const __m256i *a0_avx2,
                                    const __m128i *m_sse,
                                    const __m128i *a_sse) {
  __m256i h_avx = _mm256_set1_epi32((int)h);
  __m256i t_avx =
      _mm256_add_epi32(_mm256_mullo_epi32(*m0_avx2, h_avx), *a0_avx2);
  *min_avx = _mm256_min_epu32(t_avx, *min_avx);

  __m128i h_sse = _mm_set1_epi32((int)h);
  __m128i t_sse = _mm_add_epi32(_mm_mullo_epi32(*m_sse, h_sse), *a_sse);
  *min_sse = _mm_min_epu32(t_sse, *min_sse);
}

/**
 * @brief 提取 Odess 原始 12 维子特征。
 * @param chunk 输入块。
 * @param out_subfeatures 输出数组（长度 12）。
 */
void odess_extract_subfeatures(const Chunk *chunk,
                               uint32_t out_subfeatures[12]) {
  const uint8_t *data = chunk->data;
  const size_t length = (size_t)chunk->len;
  const uint32_t boundary = (uint32_t)(4294967295.0 * (1.0 - 1.0 / 256.0));

  const __m256i M0_AVX2 =
      _mm256_set_epi32(0xd767df81, 0xa868aeb3, 0xba2c8e95, 0xeba8d4d9,
                       0x8f9c99fc, 0x95f658b1, 0xe4f94e27, 0x5b49898a);
  const __m256i M1_AVX2 =
      _mm256_set_epi32(0xeb91e53d, 0x32d9d1be, 0xc1e70129, 0x843606a4,
                       0xeb91e53d, 0x32d9d1be, 0xc1e70129, 0x843606a4);
  const __m256i A0_AVX2 =
      _mm256_set_epi32(0x4f5400da, 0xf9979921, 0xd547b8ba, 0x7faa9b8a,
                       0x5b47dc4d, 0x012843ff, 0x6f485986, 0x0ff4be8c);
  const __m256i A1_AVX2 =
      _mm256_set_epi32(0x3f5adf5d, 0x0032716d, 0x3c9321ac, 0x725f79a9,
                       0x3f5adf5d, 0x0032716d, 0x3c9321ac, 0x725f79a9);

  __m128i hash_vec = _mm_setzero_si128();
  const __m128i boundary_vec = _mm_set1_epi32((int)boundary);
  const __m128i sign_flip = _mm_set1_epi32(0x80000000u);
  const __m128i shuffle_mask =
      _mm_setr_epi8(3, 2, 1, 0, 4, 3, 2, 1, 5, 4, 3, 2, 6, 5, 4, 3);

  const __m128i m_sse = _mm256_castsi256_si128(M1_AVX2);
  const __m128i a_sse = _mm256_castsi256_si128(A1_AVX2);

  __m256i min_avx = _mm256_set1_epi32(-1);
  __m128i min_sse = _mm_set1_epi32(-1);

  size_t n = 0;
  const size_t end_n = (length >= 20) ? (length - 20) : 0;

  while (n <= end_n) {
    _mm_prefetch((const char *)(data + n + 512), _MM_HINT_T0);

    const uint8_t *base = data + n;

    __m128i v0 = _mm_loadu_si128((const __m128i *)(base));
    __m128i v1 = _mm_loadu_si128((const __m128i *)(base + 4));
    __m128i v2 = _mm_loadu_si128((const __m128i *)(base + 8));
    __m128i v3 = _mm_loadu_si128((const __m128i *)(base + 12));

    __m128i d0 = _mm_shuffle_epi8(v0, shuffle_mask);
    __m128i d1 = _mm_shuffle_epi8(v1, shuffle_mask);
    __m128i d2 = _mm_shuffle_epi8(v2, shuffle_mask);
    __m128i d3 = _mm_shuffle_epi8(v3, shuffle_mask);

    hash_vec = _mm_add_epi32(_mm_slli_epi32(hash_vec, 4), d0);
    {
      __m128i cmp = _mm_cmpgt_epi32(_mm_xor_si128(hash_vec, sign_flip),
                                    _mm_xor_si128(boundary_vec, sign_flip));
      int mask = _mm_movemask_ps(_mm_castsi128_ps(cmp));
      if (__builtin_expect(mask != 0, 0)) {
        if (mask & 1) {
          odess_update_min((uint32_t)_mm_cvtsi128_si32(hash_vec), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
        if (mask & 2) {
          odess_update_min((uint32_t)_mm_extract_epi32(hash_vec, 1), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
        if (mask & 4) {
          odess_update_min((uint32_t)_mm_extract_epi32(hash_vec, 2), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
        if (mask & 8) {
          odess_update_min((uint32_t)_mm_extract_epi32(hash_vec, 3), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
      }
    }

    hash_vec = _mm_add_epi32(_mm_slli_epi32(hash_vec, 4), d1);
    {
      __m128i cmp = _mm_cmpgt_epi32(_mm_xor_si128(hash_vec, sign_flip),
                                    _mm_xor_si128(boundary_vec, sign_flip));
      int mask = _mm_movemask_ps(_mm_castsi128_ps(cmp));
      if (__builtin_expect(mask != 0, 0)) {
        if (mask & 1) {
          odess_update_min((uint32_t)_mm_cvtsi128_si32(hash_vec), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
        if (mask & 2) {
          odess_update_min((uint32_t)_mm_extract_epi32(hash_vec, 1), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
        if (mask & 4) {
          odess_update_min((uint32_t)_mm_extract_epi32(hash_vec, 2), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
        if (mask & 8) {
          odess_update_min((uint32_t)_mm_extract_epi32(hash_vec, 3), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
      }
    }

    hash_vec = _mm_add_epi32(_mm_slli_epi32(hash_vec, 4), d2);
    {
      __m128i cmp = _mm_cmpgt_epi32(_mm_xor_si128(hash_vec, sign_flip),
                                    _mm_xor_si128(boundary_vec, sign_flip));
      int mask = _mm_movemask_ps(_mm_castsi128_ps(cmp));
      if (__builtin_expect(mask != 0, 0)) {
        if (mask & 1) {
          odess_update_min((uint32_t)_mm_cvtsi128_si32(hash_vec), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
        if (mask & 2) {
          odess_update_min((uint32_t)_mm_extract_epi32(hash_vec, 1), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
        if (mask & 4) {
          odess_update_min((uint32_t)_mm_extract_epi32(hash_vec, 2), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
        if (mask & 8) {
          odess_update_min((uint32_t)_mm_extract_epi32(hash_vec, 3), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
      }
    }

    hash_vec = _mm_add_epi32(_mm_slli_epi32(hash_vec, 4), d3);
    {
      __m128i cmp = _mm_cmpgt_epi32(_mm_xor_si128(hash_vec, sign_flip),
                                    _mm_xor_si128(boundary_vec, sign_flip));
      int mask = _mm_movemask_ps(_mm_castsi128_ps(cmp));
      if (__builtin_expect(mask != 0, 0)) {
        if (mask & 1) {
          odess_update_min((uint32_t)_mm_cvtsi128_si32(hash_vec), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
        if (mask & 2) {
          odess_update_min((uint32_t)_mm_extract_epi32(hash_vec, 1), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
        if (mask & 4) {
          odess_update_min((uint32_t)_mm_extract_epi32(hash_vec, 2), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
        if (mask & 8) {
          odess_update_min((uint32_t)_mm_extract_epi32(hash_vec, 3), &min_avx,
                           &min_sse, &M0_AVX2, &A0_AVX2, &m_sse, &a_sse);
        }
      }
    }

    n += 16;
  }

  _mm256_storeu_si256((__m256i *)out_subfeatures, min_avx);
  _mm_storeu_si128((__m128i *)(out_subfeatures + 8), min_sse);
}

/**
 * @brief 构建 Odess 组级特征向量。
 * @param chunk 输入块。
 * @param out_features 输出特征。
 */
void odess_feature_extract(const Chunk *chunk, FeatureLevels *out_features) {
  if (chunk == NULL || out_features == NULL) {
    return;
  }

  uint32_t sub_features[12];
  odess_extract_subfeatures(chunk, sub_features);

  /* 先清零全部字段，再填充当前有效部分。 */
  memset(out_features, 0, sizeof(*out_features));

  out_features->level_count = 1;
  out_features->level_width[0] = 3;

  for (int i = 0; i < 3; ++i) {
    uint32_t hash_value = 0;
    const uint8_t *this_hash_buf = (const uint8_t *)(sub_features + i * 4);
    const int total = 4 * (int)sizeof(uint32_t);

    for (int j = 0; j < total; ++j) {
      hash_value = (hash_value << 1) + GEAR_TABLE[this_hash_buf[j]];
    }
    out_features->values[0][i] = hash_value;
  }
}

/**
 * @brief 按给定分组规则填充层级特征值。
 * @param hash_buf 原始哈希字节缓存。
 * @param sf_cnt super-feature 数量。
 * @param sf_subf 每个 super-feature 子特征数。
 * @param skip_prefix_bytes 计算时跳过的前缀字节数。
 * @param out_values 输出数组。
 */
static void fill_group_features(const uint8_t *hash_buf, int sf_cnt,
                                int sf_subf, int skip_prefix_bytes,
                                uint32_t *out_values) {
  for (int i = 0; i < sf_cnt; ++i) {
    uint64_t hash_value = 0;
    const uint8_t *this_hash_buf =
        hash_buf + i * sf_subf * (int)sizeof(uint32_t);
    const int end = sf_subf * (int)sizeof(uint32_t);
    for (int j = skip_prefix_bytes; j < end; ++j) {
      hash_value = (hash_value << 1) + GEAR_TABLE[this_hash_buf[j]];
    }
    out_values[i] = (uint32_t)hash_value;
  }
}

/**
 * @brief 构建 Palantir 多层级特征向量。
 * @param chunk 输入块。
 * @param out_features 输出特征。
 */
void palantir_feature_extract(const Chunk *chunk, FeatureLevels *out_features) {
  uint32_t sub_features[12];
  odess_extract_subfeatures(chunk, sub_features);
  const uint8_t *hash_buf = (const uint8_t *)sub_features;

  out_features->level_count = 3;
  out_features->level_width[0] = 3;
  out_features->level_width[1] = 4;
  out_features->level_width[2] = 6;

  fill_group_features(hash_buf, 3, 4, 4, out_features->values[0]);
  fill_group_features(hash_buf, 4, 3, 4, out_features->values[1]);
  fill_group_features(hash_buf, 6, 2, 4, out_features->values[2]);

  for (int i = 3; i < FEATURE_MAX_WIDTH; ++i) {
    out_features->values[0][i] = 0;
  }
  for (int i = 4; i < FEATURE_MAX_WIDTH; ++i) {
    out_features->values[1][i] = 0;
  }
}

/**
 * @brief 从特征构造相似度门控键。
 * @param features 输入特征。
 * @return 紧凑键值。
 */
uint32_t build_similarity_gate_key_from_feature(const FeatureLevels *features) {
  uint32_t l0 = 0;
  uint32_t l1 = 0;

  if (features->level_count > 0 && features->level_width[0] > 0) {
    l0 = features->values[0][0] >> 16;
  }
  if (features->level_count > 1 && features->level_width[1] > 0) {
    l1 = features->values[1][0] >> 20;
  }

  return (l0 << 12) ^ l1;
}
