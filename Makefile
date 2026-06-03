# Cocoon - 基于 coco 协程库的静态资源 Web 服务器

# coco 库路径（默认使用 submodule，可覆盖）
COCO_DIR     ?= coco
COCO_INCLUDE ?= $(COCO_DIR)/include
COCO_LIB     ?= $(COCO_DIR)/build

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -I$(COCO_INCLUDE)
LDFLAGS = -L$(COCO_LIB) -lcoco -lpthread -lm -luring -lz

# 调试模式
DEBUG ?= 0
ifeq ($(DEBUG),1)
    CFLAGS += -g -O0 -DCOCOON_DEBUG
endif

# 安装路径
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

# 源文件
SRCS = main.c server.c http.c static.c log.c
OBJS = $(SRCS:.c=.o)
TARGET = cocoon

# 默认目标
all: $(TARGET)

# 链接
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# 构建 coco 依赖（如果 submodule 已初始化）
deps:
	@if [ ! -d "$(COCO_DIR)/build" ]; then \
		echo "[Cocoon] 构建 coco 依赖..."; \
		cd $(COCO_DIR) && mkdir -p build && cd build && cmake .. && $(MAKE); \
	else \
		echo "[Cocoon] coco 已构建，跳过"; \
	fi

# 完整构建（先构建依赖，再构建项目）
build-all: deps $(TARGET)

# 集成测试
test: $(TARGET)
	@echo "[Cocoon] 运行集成测试..."
	@./tests/integration_test.sh

# 性能基准
bench: $(TARGET)
	@echo "[Cocoon] 运行性能基准..."
	@./tests/benchmark.sh

# 编译规则
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 安装
install: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/

# 卸载
uninstall:
	rm -f $(BINDIR)/$(TARGET)

# 清理
clean:
	rm -f $(OBJS) $(TARGET)

# 重新构建
rebuild: clean all

.PHONY: all clean install uninstall rebuild deps build-all
