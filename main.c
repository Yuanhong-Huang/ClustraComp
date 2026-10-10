/*
 * 文件: main.c
 * 说明: 特征提取与压缩实验的程序入口。
 * 职责:
 *   - 解析命令行参数。
 *   - 配置块大小与算法执行路径。
 *   - 触发端到端 pipeline 执行。
 *
 * 命令行参数说明:
 *   [文件名]              : 要处理的输入文件路径（可选，默认为 test_data.bin）
 *   -c <块大小>           : 设置数据块大小（字节），必须为正整数（默认: 8192）
 *   -m <方法>             :
 * 手动指定算法（odess/palantir），不指定则根据文件类型自动选择 :   (.ibd ->
 * Odess，其它 -> Palantir)
 *   --feature-core <核心ID> : 绑定特征提取阶段到指定CPU核心（非负整数，默认:
 * 不绑定）
 *   --group-core <核心ID>   : 绑定分组阶段到指定CPU核心（非负整数，默认:
 * 不绑定）
 *
 * 使用示例:
 *   ./program data.bin                    # 使用默认参数处理 data.bin
 *   ./program -c 4096 data.bin            # 设置块大小为4092字节
 *   ./program --feature-core 0 --group-core 1 data.bin  # CPU核心绑定
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "include/compression.h"
#include "include/core_types.h"
#include "include/pipeline.h"

/**
 * @brief 程序主入口，负责参数解析与端到端实验编排。
 * @param argc 命令行参数数量。
 * @param argv 命令行参数数组。
 * @return 成功返回 0，失败返回非 0。
 */
int main(int argc, char *argv[]) {
  const char *filename = NULL;
  int chunk_size = 8192;
  int feature_core_id = -1;
  int grouping_core_id = -1;
  int manual_method = 0; // 0: 自动选择, 1: Odess, 2: Palantir

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      chunk_size = atoi(argv[i + 1]);
      if (chunk_size <= 0) {
        fprintf(stderr, "错误：块大小必须是正整数\n");
        return 1;
      }
      i++;
    } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
      i++;
      if (strcasecmp(argv[i], "odess") == 0) {
        manual_method = 1;
      } else if (strcasecmp(argv[i], "palantir") == 0) {
        manual_method = 2;
      } else {
        fprintf(stderr, "错误：-m 参数必须是 odess 或 palantir\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--feature-core") == 0 && i + 1 < argc) {
      feature_core_id = atoi(argv[i + 1]);
      if (feature_core_id < 0) {
        fprintf(stderr, "错误：--feature-core 必须是非负整数\n");
        return 1;
      }
      i++;
    } else if (strcmp(argv[i], "--group-core") == 0 && i + 1 < argc) {
      grouping_core_id = atoi(argv[i + 1]);
      if (grouping_core_id < 0) {
        fprintf(stderr, "错误：--group-core 必须是非负整数\n");
        return 1;
      }
      i++;
    } else if (filename == NULL) {
      filename = argv[i];
    }
  }

  CHUNK_SIZE = chunk_size;

  if (filename == NULL) {
    filename = "test_data.bin";
    printf("未指定文件，生成测试文件...\n");
    generate_test_file(filename, 10);
  }

  if (feature_core_id >= 0 || grouping_core_id >= 0) {
    printf("CPU 绑定配置: 特征阶段=%s, 分组阶段=%s\n",
           feature_core_id >= 0 ? "已指定" : "不绑定",
           grouping_core_id >= 0 ? "已指定" : "不绑定");
  }

  if (manual_method > 0) {
    printf("算法选择: %s（手动指定）\n",
           manual_method == 1 ? "Odess" : "Palantir");
  } else {
    printf("算法选择: 自动（根据文件类型）\n");
  }

  printf("=== 文件特征提取性能测试 ===\n");
  test_file_feature_extraction(filename, feature_core_id, grouping_core_id,
                               manual_method);

  printf("=== 文件聚类压缩性能测试 ===\n");
  test_clustering_compression(filename);

  return 0;
}
