# 文件: Makefile
# 说明: ClustraComp - 高性能数据聚类压缩工具
# 职责:
#   - 定义 OpenMP/LZ4/ZSTD 所需编译与链接参数。
#   - 构建主可执行目标并提供常用辅助目标。
#   - 提供 clean 与本地运行便捷命令。

CC := gcc
TARGET := ClustraComp
AR := ar
SRC_DIR := src
INC_DIR := include
LIB_DIR := lib
BUILD_DIR := build

# 源文件路径
MAIN_SRC := main.c
COMMON_SRCS := $(SRC_DIR)/core_types.c \
               $(SRC_DIR)/indexing.c \
               $(SRC_DIR)/compression.c \
               $(SRC_DIR)/pipeline.c
FEATURE_API_SRC := $(SRC_DIR)/feature_api.c
GROUPING_API_SRC := $(SRC_DIR)/grouping_api.c

# 对象文件路径（放在 build 目录）
MAIN_OBJ := $(BUILD_DIR)/main.o
CORE_TYPES_OBJ := $(BUILD_DIR)/core_types.o
INDEXING_OBJ := $(BUILD_DIR)/indexing.o
COMPRESSION_OBJ := $(BUILD_DIR)/compression.o
PIPELINE_OBJ := $(BUILD_DIR)/pipeline.o
FEATURE_API_OBJ := $(BUILD_DIR)/feature_api.o
GROUPING_API_OBJ := $(BUILD_DIR)/grouping_api.o

COMMON_OBJS := $(CORE_TYPES_OBJ) $(INDEXING_OBJ) $(COMPRESSION_OBJ) $(PIPELINE_OBJ)

FEATURE_API_LIB := $(LIB_DIR)/libfeature_api.a
GROUPING_API_LIB := $(LIB_DIR)/libgrouping_api.a

# 编译参数
CFLAGS := -std=c11 -O2 -mavx2 -fopenmp -Wall -Wextra -I$(SRC_DIR) -I$(INC_DIR)
LDFLAGS := -fopenmp -llz4 -lzstd -static

.PHONY: all clean run test directories

all: directories $(FEATURE_API_LIB) $(GROUPING_API_LIB) $(TARGET)
	@echo "✅ 构建完成！"
	@echo "📦 可执行文件: $(TARGET)"
	@echo "📚 静态库: $(FEATURE_API_LIB), $(GROUPING_API_LIB)"

directories:
	@mkdir -p $(BUILD_DIR) $(LIB_DIR)

$(FEATURE_API_LIB): $(FEATURE_API_OBJ) | directories
	$(AR) rcs $@ $^

$(GROUPING_API_LIB): $(GROUPING_API_OBJ) | directories
	$(AR) rcs $@ $^

# 通用编译规则：将所有 .o 文件放到 build 目录
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | directories
	$(CC) $(CFLAGS) -c $< -o $@

# main.c 在主目录的编译规则
$(BUILD_DIR)/main.o: main.c | directories
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(MAIN_OBJ) $(COMMON_OBJS) $(FEATURE_API_LIB) $(GROUPING_API_LIB)
	$(CC) $(CFLAGS) -o $@ $(MAIN_OBJ) $(COMMON_OBJS) $(FEATURE_API_LIB) $(GROUPING_API_LIB) $(LDFLAGS)
	@echo "🔗 链接完成: $@"

run: $(TARGET)
	@echo "🚀 运行程序..."
	./$(TARGET)

test: $(TARGET)
	@echo "🧪 运行自动化测试..."
	@bash scripts/auto_test.sh

clean:
	@echo "🧹 清理构建产物..."
	rm -rf $(BUILD_DIR) $(TARGET)
	rm -f $(FEATURE_API_LIB) $(GROUPING_API_LIB)
	@echo "✨ 清理完成"
