/*
 * 文件: core_types.h
 * 说明: 共享类型、全局状态与特征提取接口声明。
 * 职责:
 *   - 定义可复用的向量与特征结构。
 *   - 声明 Chunk 抽象及特征提取接口。
 *   - 声明全局运行时常量与分组元数据缓冲区。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Odess 默认掩码常量。
 * @note 该常量用于特征提取相关流程中的默认配置。
 */
extern const int default_odess_mask;

/**
 * @brief 运行时块大小（字节）。
 * @note 由程序入口按命令行参数设置，影响读块、分组和压缩粒度。
 */
extern int CHUNK_SIZE;

/**
 * @brief uint32_t 动态向量。
 */
typedef struct {
    uint32_t *data;
    size_t size;
    size_t capacity;
} U32Vector;

/**
 * @brief uint8_t 动态向量。
 */
typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} U8Vector;

/**
 * @brief float 动态向量。
 */
typedef struct {
    float *data;
    size_t size;
    size_t capacity;
} FloatVector;

#define FEATURE_MAX_LEVELS 3
#define FEATURE_MAX_WIDTH 6

/**
 * @brief 多层特征表示。
 * @note values 为固定上限二维数组，实际使用宽度由 level_width 指示。
 */
typedef struct {
    int level_count;
    int level_width[FEATURE_MAX_LEVELS];
    uint32_t values[FEATURE_MAX_LEVELS][FEATURE_MAX_WIDTH];
} FeatureLevels;

/**
 * @brief FeatureLevels 动态向量。
 */
typedef struct {
    FeatureLevels *data;
    size_t size;
    size_t capacity;
} FeatureVector;

/**
 * @brief Odess 分组元数据（chunk -> base chunk）。
 */
extern U32Vector chunkMeta_odess;

/**
 * @brief Palantir 分组元数据（chunk -> base/group id）。
 */
extern U32Vector chunkMeta_palantir;

/**
 * @brief 块来源类型（Level0/Level1/Level2/未聚合）标记向量。
 */
extern U8Vector chunk_source_type;

/* 动态向量接口 */

/**
 * @brief 预留 U32Vector 容量。
 * @param vec 目标向量。
 * @param new_capacity 目标容量（元素数）。
 * @return 成功返回 0，失败返回 -1。
 */
int u32vec_reserve(U32Vector *vec, size_t new_capacity);

/**
 * @brief 调整 U32Vector 逻辑大小。
 * @param vec 目标向量。
 * @param new_size 新大小（元素数）。
 * @return 成功返回 0，失败返回 -1。
 */
int u32vec_resize(U32Vector *vec, size_t new_size);

/**
 * @brief 在 U32Vector 尾部追加一个元素。
 * @param vec 目标向量。
 * @param value 追加值。
 * @return 成功返回 0，失败返回 -1。
 */
int u32vec_push_back(U32Vector *vec, uint32_t value);

/**
 * @brief 清空 U32Vector（仅重置 size，不释放容量）。
 * @param vec 目标向量。
 */
void u32vec_clear(U32Vector *vec);

/**
 * @brief 释放 U32Vector 持有的堆内存并重置状态。
 * @param vec 目标向量。
 */
void u32vec_free(U32Vector *vec);

/**
 * @brief 预留 U8Vector 容量。
 * @param vec 目标向量。
 * @param new_capacity 目标容量（元素数）。
 * @return 成功返回 0，失败返回 -1。
 */
int u8vec_reserve(U8Vector *vec, size_t new_capacity);

/**
 * @brief 调整 U8Vector 逻辑大小。
 * @param vec 目标向量。
 * @param new_size 新大小（元素数）。
 * @return 成功返回 0，失败返回 -1。
 */
int u8vec_resize(U8Vector *vec, size_t new_size);

/**
 * @brief 在 U8Vector 尾部追加一个元素。
 * @param vec 目标向量。
 * @param value 追加值。
 * @return 成功返回 0，失败返回 -1。
 */
int u8vec_push_back(U8Vector *vec, uint8_t value);

/**
 * @brief 清空 U8Vector（仅重置 size，不释放容量）。
 * @param vec 目标向量。
 */
void u8vec_clear(U8Vector *vec);

/**
 * @brief 释放 U8Vector 持有的堆内存并重置状态。
 * @param vec 目标向量。
 */
void u8vec_free(U8Vector *vec);

/**
 * @brief 拷贝 U8Vector 内容到目标向量。
 * @param dst 目标向量。
 * @param src 源向量。
 * @return 成功返回 0，失败返回 -1。
 */
int u8vec_copy(U8Vector *dst, const U8Vector *src);

/**
 * @brief 预留 FloatVector 容量。
 * @param vec 目标向量。
 * @param new_capacity 目标容量（元素数）。
 * @return 成功返回 0，失败返回 -1。
 */
int f32vec_reserve(FloatVector *vec, size_t new_capacity);

/**
 * @brief 在 FloatVector 尾部追加一个元素。
 * @param vec 目标向量。
 * @param value 追加值。
 * @return 成功返回 0，失败返回 -1。
 */
int f32vec_push_back(FloatVector *vec, float value);

/**
 * @brief 清空 FloatVector（仅重置 size，不释放容量）。
 * @param vec 目标向量。
 */
void f32vec_clear(FloatVector *vec);

/**
 * @brief 释放 FloatVector 持有的堆内存并重置状态。
 * @param vec 目标向量。
 */
void f32vec_free(FloatVector *vec);

/**
 * @brief 预留 FeatureVector 容量。
 * @param vec 目标向量。
 * @param new_capacity 目标容量（元素数）。
 * @return 成功返回 0，失败返回 -1。
 */
int featurevec_reserve(FeatureVector *vec, size_t new_capacity);

/**
 * @brief 在 FeatureVector 尾部追加一个特征对象。
 * @param vec 目标向量。
 * @param value 追加特征。
 * @return 成功返回 0，失败返回 -1。
 */
int featurevec_push_back(FeatureVector *vec, const FeatureLevels *value);

/**
 * @brief 清空 FeatureVector（仅重置 size，不释放容量）。
 * @param vec 目标向量。
 */
void featurevec_clear(FeatureVector *vec);

/**
 * @brief 释放 FeatureVector 持有的堆内存并重置状态。
 * @param vec 目标向量。
 */
void featurevec_free(FeatureVector *vec);

/**
 * @brief 数据块对象。
 */
typedef struct {
    uint8_t *data;
    int len;
    uint32_t id;
} Chunk;

/* Chunk 模型与特征提取接口 */

/**
 * @brief 以拷贝方式初始化 Chunk。
 * @param chunk 输出块对象。
 * @param buf 输入数据缓存。
 * @param len 输入数据长度（字节）。
 * @param id 块 ID。
 * @return 成功返回 0，失败返回 -1。
 */
int chunk_init_copy(Chunk *chunk, const uint8_t *buf, int len, uint32_t id);

/**
 * @brief 销毁 Chunk，释放其持有的数据内存。
 * @param chunk 目标块对象。
 */
void chunk_destroy(Chunk *chunk);

/**
 * @brief 提取 Odess 原始 12 维子特征。
 * @param chunk 输入数据块。
 * @param out_subfeatures 输出数组，长度必须为 12。
 */
void odess_extract_subfeatures(const Chunk *chunk, uint32_t out_subfeatures[12]);

/**
 * @brief 生成 Odess 使用的层级特征。
 * @param chunk 输入数据块。
 * @param out_features 输出特征对象。
 */
void odess_feature_extract(const Chunk *chunk, FeatureLevels *out_features);

/**
 * @brief 生成 Palantir 使用的层级特征。
 * @param chunk 输入数据块。
 * @param out_features 输出特征对象。
 */
void palantir_feature_extract(const Chunk *chunk, FeatureLevels *out_features);

/**
 * @brief 由多层特征构造相似度门控键。
 * @param features 输入特征。
 * @return 紧凑 uint32 键值。
 */
uint32_t build_similarity_gate_key_from_feature(const FeatureLevels *features);

#ifdef __cplusplus
}
#endif
