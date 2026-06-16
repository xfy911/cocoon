# Cocoon — 基于 coco 协程库的轻量级 Web 服务器
# 多阶段构建，最终镜像约 50MB

# ==================== 构建阶段 ====================
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc \
    make \
    cmake \
    git \
    libssl-dev \
    libbrotli-dev \
    libnghttp2-dev \
    libcurl4-openssl-dev \
    liburing-dev \
    zlib1g-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# 复制源码（包含子模块）
COPY . .

# 初始化并更新子模块（如果通过 COPY 未包含）
RUN git submodule update --init --recursive || true

# 构建 coco 依赖
RUN make deps

# 构建 cocoon
RUN make COCO_INCLUDE=./coco/include COCO_LIB=./coco/build

# 验证二进制
RUN ./cocoon --help || true

# ==================== 运行阶段 ====================
FROM debian:bookworm-slim AS runner

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libbrotli1 \
    libnghttp2-14 \
    libcurl4 \
    liburing2 \
    zlib1g \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# 创建运行用户（非 root）
RUN groupadd -r cocoon && useradd -r -g cocoon -s /bin/false cocoon

# 创建工作目录
WORKDIR /app

# 复制二进制和默认配置
COPY --from=builder /build/cocoon /usr/local/bin/cocoon
COPY --from=builder /build/cocoon.json /app/cocoon.json
COPY --from=builder /build/examples /app/examples
COPY --from=builder /build/plugins /app/plugins
COPY --from=builder /build/cocoon.service /app/cocoon.service

# 设置权限
RUN chown -R cocoon:cocoon /app

# 健康检查
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8080/ || exit 1

USER cocoon

EXPOSE 8080

ENTRYPOINT ["cocoon"]
CMD ["-c", "/app/cocoon.json"]
