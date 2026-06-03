# Cocoon - 基于 coco 协程库的静态资源 Web 服务器

# coco 库路径（可覆盖）
COCO_INCLUDE ?= ../coco/include
COCO_LIB     ?= ../coco/build

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -I$(COCO_INCLUDE)
LDFLAGS = -L$(COCO_LIB) -lcoco -lpthread -lm -luring

# 调试模式
DEBUG ?= 0
ifeq ($(DEBUG),1)
    CFLAGS += -g -O0 -DCOCOON_DEBUG
endif

# 安装路径
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

# 源文件
SRCS = main.c server.c http.c static.c
OBJS = $(SRCS:.c=.o)
TARGET = cocoon

# 默认目标
all: $(TARGET)

# 链接
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

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

.PHONY: all clean install uninstall rebuild
