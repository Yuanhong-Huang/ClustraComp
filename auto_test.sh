#!/bin/bash

# 文件: auto_test.sh
# 说明: ClustraComp 特征提取与压缩实验的批量测试脚本。
# 职责:
#   - 从配置文件读取测试参数（数据目录、输出目录、块大小、CPU 绑核等）。
#   - 校验可执行文件与数据集前置条件。
#   - 按可配置块大小逐文件执行测试（算法由程序内部自动选择）。
#   - 落盘每个文件日志并输出成功/失败/超时汇总。
# 用法: ./auto_test.sh [--config <配置文件>]

set -e

# 默认配置文件路径
CONFIG_FILE="./test_config.cfg"

# 默认值（当配置文件不存在时使用）
DATA_DIR="./data"
OUTPUT_DIR="./logs"
CHUNK_SIZE="8192"
EXECUTABLE="./ClustraComp"
FEATURE_CORE=-1
GROUP_CORE=-1

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# 从配置文件加载参数
load_config() {
    if [ ! -f "$CONFIG_FILE" ]; then
        print_warning "配置文件 $CONFIG_FILE 不存在，使用默认值"
        return
    fi

    print_info "加载配置文件: $CONFIG_FILE"

    while IFS='=' read -r key value; do
        # 去除前后空白
        key=$(echo "$key" | xargs)
        value=$(echo "$value" | xargs)

        # 跳过空行和注释
        [[ -z "$key" || "$key" == \#* ]] && continue

        case "$key" in
            DATA_DIR)     DATA_DIR="$value" ;;
            OUTPUT_DIR)   OUTPUT_DIR="$value" ;;
            CHUNK_SIZE)   CHUNK_SIZE="$value" ;;
            EXECUTABLE)   EXECUTABLE="$value" ;;
            FEATURE_CORE) FEATURE_CORE="$value" ;;
            GROUP_CORE)   GROUP_CORE="$value" ;;
            *)
                print_warning "配置文件中存在未知配置项: $key"
                ;;
        esac
    done < "$CONFIG_FILE"
}

check_executable() {
    if [ ! -f "$EXECUTABLE" ]; then
        print_error "可执行文件 $EXECUTABLE 不存在"
        print_info "请先编译项目: make"
        exit 1
    fi
    if [ ! -x "$EXECUTABLE" ]; then
        chmod +x "$EXECUTABLE"
    fi
}

check_data_dir() {
    if [ ! -d "$DATA_DIR" ]; then
        print_error "数据目录 $DATA_DIR 不存在"
        mkdir -p "$DATA_DIR"
        print_warning "请放入测试文件后重试"
        exit 1
    fi

    local file_count
    file_count=$(find "$DATA_DIR" -maxdepth 1 -type f | wc -l)
    if [ "$file_count" -eq 0 ]; then
        print_warning "数据目录中没有可测试文件"
        exit 1
    fi

    print_success "找到 $file_count 个文件"
}

create_output_dir() {
    mkdir -p "$OUTPUT_DIR"
}

# 构建附加命令行参数（CPU 绑核）
build_extra_args() {
    EXTRA_ARGS=""
    if [ "$FEATURE_CORE" -ge 0 ] 2>/dev/null; then
        EXTRA_ARGS="$EXTRA_ARGS --feature-core $FEATURE_CORE"
    fi
    if [ "$GROUP_CORE" -ge 0 ] 2>/dev/null; then
        EXTRA_ARGS="$EXTRA_ARGS --group-core $GROUP_CORE"
    fi
}

run_single_test() {
    local data_file="$1"
    local filename
    filename=$(basename "$data_file")

    local log_file="$OUTPUT_DIR/${filename}_chunk_${CHUNK_SIZE}.log"

    print_info "测试文件: $filename"
    print_info "块大小: $CHUNK_SIZE"
    print_info "算法: 自动选择（.ibd -> Odess，其它 -> Palantir）"
    if [ "$FEATURE_CORE" -ge 0 ] 2>/dev/null; then
        print_info "特征提取绑核: $FEATURE_CORE"
    fi
    if [ "$GROUP_CORE" -ge 0 ] 2>/dev/null; then
        print_info "聚类分组绑核: $GROUP_CORE"
    fi

    local start_time
    start_time=$(date +%s)

    {
        echo "测试文件: $data_file"
        echo "块大小: $CHUNK_SIZE"
        echo "算法: 自动选择（.ibd -> Odess，其它 -> Palantir）"
        echo "特征提取绑核: $FEATURE_CORE"
        echo "聚类分组绑核: $GROUP_CORE"
        echo "开始时间: $(date)"
        "$EXECUTABLE" "$data_file" -c "$CHUNK_SIZE" $EXTRA_ARGS
        echo "结束时间: $(date)"
        echo "退出码: $?"
    } > "$log_file" 2>&1 || true

    local end_time
    end_time=$(date +%s)
    local duration=$((end_time - start_time))

    if grep -q "程序执行出错" "$log_file"; then
        print_error "失败: $filename (${duration}s)"
        return 1
    fi

    if grep -q "退出码: 124" "$log_file"; then
        print_warning "超时: $filename (${duration}s)"
        return 2
    fi

    print_success "完成: $filename (${duration}s)"
    return 0
}

show_help() {
    echo "ClustraComp 自动特征压缩测试脚本"
    echo ""
    echo "用法: $0 [--config <配置文件>]"
    echo ""
    echo "选项:"
    echo "  --config <配置文件>     指定配置文件路径（默认: ./test_config.cfg）"
    echo "  -h, --help              显示帮助信息"
    echo ""
    echo "配置文件格式 (KEY=VALUE):"
    echo "  DATA_DIR=<数据目录>         默认 ./data"
    echo "  OUTPUT_DIR=<输出目录>       默认 ./logs"
    echo "  CHUNK_SIZE=<块大小>         默认 8192"
    echo "  EXECUTABLE=<可执行文件>     默认 ./ClustraComp"
    echo "  FEATURE_CORE=<核心ID>      默认 -1（不绑定）"
    echo "  GROUP_CORE=<核心ID>        默认 -1（不绑定）"
    echo ""
    echo "说明:"
    echo "  - 算法由程序内部自动选择（.ibd -> Odess，其它 -> Palantir）"
    echo "  - 日志文件保存在输出目录中"
    echo "  - FEATURE_CORE / GROUP_CORE 设为非负整数时绑定对应 CPU 核心，-1 表示不绑定"
}

# 解析命令行参数
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            show_help
            exit 0
            ;;
        --config)
            if [ -z "$2" ]; then
                print_error "--config 需要指定配置文件路径"
                exit 1
            fi
            CONFIG_FILE="$2"
            shift 2
            ;;
        --)
            shift
            break
            ;;
        -*)
            print_error "未知参数: $1"
            show_help
            exit 1
            ;;
        *)
            print_error "未知参数: $1（如需指定配置文件请使用 --config）"
            show_help
            exit 1
            ;;
    esac
done

# 加载配置文件
load_config

# 校验块大小
if ! [[ "$CHUNK_SIZE" =~ ^[0-9]+$ ]] || [ "$CHUNK_SIZE" -le 0 ]; then
    print_error "块大小必须是正整数（当前值: $CHUNK_SIZE）"
    exit 1
fi

# 校验核心编号
if ! [[ "$FEATURE_CORE" =~ ^-?[0-9]+$ ]]; then
    print_error "FEATURE_CORE 必须是整数（当前值: $FEATURE_CORE）"
    exit 1
fi
if ! [[ "$GROUP_CORE" =~ ^-?[0-9]+$ ]]; then
    print_error "GROUP_CORE 必须是整数（当前值: $GROUP_CORE）"
    exit 1
fi

# 构建附加参数
build_extra_args

print_info "开始 ClustraComp 自动测试"
print_info "数据目录: $DATA_DIR"
print_info "输出目录: $OUTPUT_DIR"
print_info "块大小: $CHUNK_SIZE"
print_info "算法策略: 自动选择（.ibd -> Odess，其它 -> Palantir）"
print_info "特征提取绑核: $( [ "$FEATURE_CORE" -ge 0 ] 2>/dev/null && echo "核心 $FEATURE_CORE" || echo "不绑定" )"
print_info "聚类分组绑核: $( [ "$GROUP_CORE" -ge 0 ] 2>/dev/null && echo "核心 $GROUP_CORE" || echo "不绑定" )"

check_executable
check_data_dir
create_output_dir

mapfile -t data_files < <(find "$DATA_DIR" -maxdepth 1 -type f -exec du -b {} \; | sort -n | cut -f2-)

total=0
ok=0
fail=0
timeout_count=0

for data_file in "${data_files[@]}"; do
    total=$((total + 1))
    run_single_test "$data_file"
    code=$?
    if [ "$code" -eq 0 ]; then
        ok=$((ok + 1))
    elif [ "$code" -eq 2 ]; then
        timeout_count=$((timeout_count + 1))
    else
        fail=$((fail + 1))
    fi
    echo ""
done

echo "=============================================="
print_success "全部测试完成"
echo "总数: $total"
echo "成功: $ok"
echo "失败: $fail"
echo "超时: $timeout_count"
if [ "$total" -gt 0 ]; then
    echo "成功率: $((ok * 100 / total))%"
fi
