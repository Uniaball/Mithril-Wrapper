# launcher-patch

来自对启动器源码的手工修改，以 **git diff patch** 形式作为唯一事实源保存。
我们不把启动器源码以子模块或直接克隆的方式放进本工作区，只在 CI 里"克隆 → 打补丁"。

## 源仓库

- GitHub: `https://github.com/LiuLPigeon617/Air_with-mithril`
- 基准提交: `f8035a1`（`fix(ios27): Fix JIT SIGBUS crash on iOS 27 + StikDebug (no TXM devices)`）
- 它是 `Uniaball/Amethyst-iOS-MyRemastered` 的 fork，i.e. 一个 iOS Minecraft Java 启动器（"Air"），
  内部把 mithril 作为**渲染器**捆绑（`libmithril.dylib`，偏好键 `video.renderer`）。

## 补丁文件

| 文件 | 说明 |
| --- | --- |
| `air-with-mithril-cli-f8035a1.patch` | 为启动器添加 CLI / 环境变量自动化入口 |
| `air-with-mithril-skip-mobileglues-f8035a1.patch` | 跳过 MobileGlues 构建（不参与 mithril 测试，其预编译库为 iOS 设备切片，导致 simulator 构建失败） |
| `air-with-mithril-simulator-target-f8035a1.patch` | native 编译加 `-target arm64-apple-ios14.0-simulator`（C/CXX），统一为 simulator 平台，避免"built for iOS vs iOS-simulator"链接冲突 |

## 如何复现

```sh
git clone --depth 1 https://github.com/LiuLPigeon617/Air_with-mithril.git
cd Air_with-mithril
git apply ../air-with-mithril-cli-f8035a1.patch
```

补丁只新增、不删除，`git apply --check` 应无冲突。`skip-mobileglues` 补丁把启动器 Makefile 的
`dep_mg` 目标替换为空实现（跳过 MobileGlues 编译），因为 CI 只测 mithril 渲染器，MobileGlues
不参与，且其预编译 `libglslang.a` 是 iOS 设备切片、会让 simulator 构建链接失败。

## 补丁做了什么

在 `Natives/LauncherNavigationController.m` 新增 `cliBootstrap`（由 `viewDidAppear` 触发一次），
读取 `AIR_*` 环境变量（CI 通过 `SIMCTL_CHILD_AIR_*` 注入到进程）并：

- **调节参数**：分辨率 `AIR_RESOLUTION` → `video.resolution`；JVM 参数 `AIR_JAVA_ARGS` → `java.java_args`；
  内存 `AIR_MEMORY_MB` → `java.allocated_memory`。
- **Java**：`AIR_JAVA_VERSION` → `java.java_version`（auto/8/17/21/25）。
- **渲染器**：`AIR_RENDERER` → `video.renderer`（例如 `libmithril.dylib`）。
- **命令行启动 + 游戏下载**：`AIR_AUTOLAUNCH=1` 时注入离线账号（`AIR_USERNAME`，默认 `Player`）
  并把 `AIR_VERSION` 填进版本框、调用 `launchMinecraft:`，从而自动下载并启动 MC。

无 `AIR_AUTOLAUNCH` 时行为完全不变，不影响正常 GUI 使用。

## 环境变量速查

| 变量 | 作用 | 示例 |
| --- | --- | --- |
| `AIR_RENDERER` | 渲染器 | `libmithril.dylib` |
| `AIR_JAVA_ARGS` | 额外 JVM 参数 | `-Xms512M` |
| `AIR_MEMORY_MB` | 分配内存(MB) | `2048` |
| `AIR_RESOLUTION` | 分辨率(%) | `100` |
| `AIR_JAVA_VERSION` | Java 版本 | `auto` |
| `AIR_AUTOLAUNCH` | 自动启动 | `1` |
| `AIR_VERSION` | 要启动的 MC 版本 | `1.21.4` |
| `AIR_USERNAME` | 离线账号名 | `Player` |

## 如何升级补丁

源仓库更新后，如需升级：在克隆里重新修改 → `git diff > 新补丁` → 替换本目录文件并更新 `README.md`。

> 注意：macOS/模拟器构建与运行需在 macOS 上（`xcodebuild`/`simctl`），本 Linux 沙箱无法本地构建验证，
> 补丁的正确性由 CI job `test-ios-launcher-e2e`（`test.yml`）在 `macos-26` runner 上验证。