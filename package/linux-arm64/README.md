# ZLMediaKit Linux ARM64 构建指南

## 概览

本目录维护 fork 的单一 Linux ARM64 交付流程。本地构建与 GitHub Actions
共用 Debian 11 ARM64 镜像和脚本；工作流只负责缓存、调度、归档、构建来源
证明和上传。

交付物固定为 `zlmediakit-debian11-arm64.tar.gz`，不区分 Rockchip SoC，也不
维护多版本矩阵。构建启用 OpenSSL、WebRTC、SCTP 和 SRT，关闭测试程序。

## 快速开始

Docker 服务端必须是 Linux ARM64。在仓库根目录执行：

```bash
./build-linux-arm64.sh
./build-linux-arm64.sh archive
```

构建目录位于 `artifact/zlmediakit-debian11-arm64/`。归档脚本生成可直接下载的
`artifact/zlmediakit-debian11-arm64.tar.gz`。

常用命令：

| 命令 | 作用 |
| --- | --- |
| `./build-linux-arm64.sh image` | 构建或复用 Debian 11 ARM64 构建镜像 |
| `./build-linux-arm64.sh check` | 检查 Shell、Dockerfile 和 Actions 工作流 |
| `./build-linux-arm64.sh cache-info` | 输出 Actions 使用的构建器和缓存元数据 |
| `./build-linux-arm64.sh build` | 准备锁定依赖并构建运行时目录 |
| `./build-linux-arm64.sh archive` | 在 Debian 容器中生成确定性 `.tar.gz` |
| `./build-linux-arm64.sh clean` | 清理此流程生成的构建、缓存和产物 |

## 产物内容

| 路径 | 内容 |
| --- | --- |
| `MediaServer` | ZLMediaKit 服务端程序 |
| `libmk_api.so` | C API 动态库 |
| `config.ini` | 默认配置 |
| `default.pem` | 默认证书 |
| `www/` | HTTP 静态资源 |
| `version.txt` | 基于源码提交日期和提交号的版本 |
| `BUILDINFO.txt` | 源码、依赖、构建器和功能开关信息 |
| `MANIFEST.mtree` | 文件类型、权限、链接目标和 SHA-256 清单 |

调试符号、测试程序、头文件和中间构建目录不会进入运行时包。目标系统需要
ARM64 Linux，并提供 glibc 2.31 或更新版本。

## GitHub Actions

`.github/workflows/linux_arm64.yml` 只在推送 `master` 或手动触发时构建一个
Linux ARM64 产物。归档通过 `actions/upload-artifact` 的直接文件模式上传，
因此下载结果就是 `.tar.gz`，没有额外 ZIP 包装层；同一个文件还会生成 GitHub
构建来源证明。

下载后验证内部清单：

```bash
tar -xzf zlmediakit-debian11-arm64.tar.gz
package/linux-arm64/tools/verify-artifact.sh \
  zlmediakit-debian11-arm64
```

Debian/Ubuntu 需要安装 `mtree-netbsd`，macOS 可使用系统 `mtree`。
