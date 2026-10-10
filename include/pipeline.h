/*
 * 文件: pipeline.h
 * 说明: 实验流程对外入口声明。
 * 职责:
 *   - 暴露阶段 1（特征提取）测试入口。
 *   - 暴露阶段 2（聚类压缩）测试入口。
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 执行阶段 1：特征提取与分组元数据构建。
 * @param filename 输入文件路径。
 * @param feature_core_id 特征计算阶段绑定核心 ID，<0 表示不绑定。
 * @param grouping_core_id 分组阶段绑定核心 ID，<0 表示不绑定。
 * @param manual_method 手动指定算法：0=自动选择，1=Odess，2=Palantir。
 */
void test_file_feature_extraction(const char *filename, int feature_core_id,
                                  int grouping_core_id, int manual_method);

/**
 * @brief 执行阶段 2：压缩实验与方法对比报告输出。
 * @param filename 输入文件路径。
 * @note 算法由 pipeline 根据文件类型自动选择（.ibd -> Odess，其它 ->
 * Palantir）。
 */
void test_clustering_compression(const char *filename);

#ifdef __cplusplus
}
#endif
