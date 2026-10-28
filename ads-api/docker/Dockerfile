# 多阶段构建 (build context = 仓库根目录):
#   sdk     : Ubuntu 24.04 + g++-14 + 从源码编译的 aws-sdk-cpp (只编 dynamodb 模块) + libpq
#   build   : 编译本项目 (AWS 版本：DynamoDB + PostgreSQL)
#   runtime : 只带运行所需的 .so 和可执行文件，非 root 用户运行
# 本地: docker compose -f ads-api/docker/compose.yaml up --build
# AWS : CDK 的 DockerImageAsset 用同一个 Dockerfile 构建镜像推到 ECR，跑在 Fargate (ARM64)

FROM ubuntu:24.04@sha256:24737cef17f4e22b1977df7d9a05e77c4e28b7da5b8911f8d8ccda136bf3573c AS sdk
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      g++-14 cmake ninja-build git ca-certificates curl \
      libcurl4-openssl-dev libssl-dev zlib1g-dev libpq-dev \
 && rm -rf /var/lib/apt/lists/*
ENV CC=gcc-14 CXX=g++-14
ARG AWS_SDK_CPP_VERSION=1.11.905
# 只编 dynamodb (core 会自动带上)。全量编译要一个多小时，这样大约 10 分钟
RUN git clone --depth 1 --branch ${AWS_SDK_CPP_VERSION} --recurse-submodules --shallow-submodules \
      https://github.com/aws/aws-sdk-cpp.git /tmp/aws-sdk-cpp \
 && cmake -S /tmp/aws-sdk-cpp -B /tmp/sdk-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_ONLY="dynamodb" -DBUILD_SHARED_LIBS=ON \
      -DENABLE_TESTING=OFF -DAUTORUN_UNIT_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/opt/aws \
 && cmake --build /tmp/sdk-build \
 && cmake --install /tmp/sdk-build \
 && rm -rf /tmp/aws-sdk-cpp /tmp/sdk-build

FROM sdk AS build
WORKDIR /src
COPY common ./common
COPY ads-api ./ads-api
RUN cmake -S ads-api -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DADS_WITH_AWS=ON -DCMAKE_PREFIX_PATH=/opt/aws \
 && cmake --build /build \
 && ctest --test-dir /build --output-on-failure

FROM ubuntu:24.04@sha256:24737cef17f4e22b1977df7d9a05e77c4e28b7da5b8911f8d8ccda136bf3573c AS runtime
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libcurl4t64 libssl3t64 zlib1g libpq5 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 ads
COPY --from=build /opt/aws/lib/ /usr/local/lib/
COPY --from=build /build/ads_api /build/budget_bench /usr/local/bin/
RUN ldconfig
USER ads
EXPOSE 8080
ENTRYPOINT ["ads_api"]
