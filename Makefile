# Cocoon - 基于 coco 协程库的静态资源 Web 服务器

# coco 库路径（默认使用 submodule，可覆盖）
COCO_DIR     ?= coco
COCO_INCLUDE ?= $(COCO_DIR)/include
COCO_LIB     ?= $(COCO_DIR)/build

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -I$(COCO_INCLUDE)
LDFLAGS = -L$(COCO_LIB) -lcoco -lpthread -lm -luring -lz -lbrotlienc -lssl -lcrypto -lnghttp2 -ldl

# 调试模式
DEBUG ?= 0
ifeq ($(DEBUG),1)
    CFLAGS += -g -O0 -DCOCOON_DEBUG
endif

# 安装路径
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

# 源文件
SRCS = main.c server.c http.c static.c log.c config.c multipart.c tls.c http2.c access_log.c websocket.c platform.c middleware.c plugin.c
OBJS = $(SRCS:.c=.o)
TARGET = cocoon

# Windows (MinGW) 下链接 Winsock 库
ifeq ($(OS),Windows_NT)
    LDFLAGS += -lws2_32
endif

# 单元测试
UNITY_SRC = tests/unity/unity.c
UNIT_TEST_DIR = tests/unit
UNIT_TEST_SRCS = $(wildcard $(UNIT_TEST_DIR)/test_*.c)
UNIT_TEST_OBJS = $(UNIT_TEST_SRCS:.c=.o)
UNIT_TEST_BINS = $(UNIT_TEST_SRCS:.c=)

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

# 单元测试
unit-test: $(UNIT_TEST_BINS)
	@echo "[Cocoon] 运行单元测试..."
	@failed=0; \
	for bin in $(UNIT_TEST_BINS); do \
		echo "--- $$bin ---"; \
		if ! ./$$bin; then \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	if [ $$failed -gt 0 ]; then \
		echo "[Cocoon] $$failed 个单元测试失败"; \
		exit 1; \
	else \
		echo "[Cocoon] 全部单元测试通过 ✓"; \
	fi

# 单元测试编译规则
$(UNIT_TEST_DIR)/test_server: $(UNIT_TEST_DIR)/test_server.c server.c http.c static.c log.c config.c multipart.c tls.c http2.c access_log.c websocket.c platform.c middleware.c plugin.c $(UNITY_SRC)
	$(CC) $(CFLAGS) -I. -I$(UNIT_TEST_DIR)/../unity -o $@ $(UNIT_TEST_DIR)/test_server.c server.c http.c static.c log.c config.c multipart.c tls.c http2.c access_log.c websocket.c platform.c middleware.c plugin.c $(UNITY_SRC) $(LDFLAGS)

$(UNIT_TEST_DIR)/test_multipart: $(UNIT_TEST_DIR)/test_multipart.c multipart.c $(UNITY_SRC)
	$(CC) $(CFLAGS) -I. -I$(UNIT_TEST_DIR)/../unity -o $@ $(UNIT_TEST_DIR)/test_multipart.c multipart.c $(UNITY_SRC) -lm

$(UNIT_TEST_DIR)/test_http: $(UNIT_TEST_DIR)/test_http.c http.c log.c $(UNITY_SRC)
	$(CC) $(CFLAGS) -I. -I$(UNIT_TEST_DIR)/../unity -o $@ $(UNIT_TEST_DIR)/test_http.c http.c log.c $(UNITY_SRC) -lm

$(UNIT_TEST_DIR)/test_static: $(UNIT_TEST_DIR)/test_static.c http.c log.c tls.c access_log.c platform.c $(UNITY_SRC)
	$(CC) $(CFLAGS) -I. -I$(UNIT_TEST_DIR)/../unity -o $@ $(UNIT_TEST_DIR)/test_static.c http.c log.c tls.c access_log.c platform.c $(UNITY_SRC) $(LDFLAGS)

$(UNIT_TEST_DIR)/test_websocket: $(UNIT_TEST_DIR)/test_websocket.c websocket.c log.c $(UNITY_SRC)
	$(CC) $(CFLAGS) -I. -I$(UNIT_TEST_DIR)/../unity -o $@ $(UNIT_TEST_DIR)/test_websocket.c websocket.c log.c $(UNITY_SRC) $(LDFLAGS)

$(UNIT_TEST_DIR)/test_log: $(UNIT_TEST_DIR)/test_log.c log.c $(UNITY_SRC)
	$(CC) $(CFLAGS) -I. -I$(UNIT_TEST_DIR)/../unity -o $@ $(UNIT_TEST_DIR)/test_log.c log.c $(UNITY_SRC) -lm

$(UNIT_TEST_DIR)/test_config: $(UNIT_TEST_DIR)/test_config.c config.c log.c access_log.c http.c $(UNITY_SRC)
	$(CC) $(CFLAGS) -I. -I$(UNIT_TEST_DIR)/../unity -o $@ $(UNIT_TEST_DIR)/test_config.c config.c log.c access_log.c http.c $(UNITY_SRC) -lm

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
	rm -f $(UNIT_TEST_OBJS) $(UNIT_TEST_BINS)

# 重新构建
rebuild: clean all

.PHONY: all clean install uninstall rebuild deps build-all test bench unit-test test-all
