# ADR 0004：Windows 静态运行库与分阶段 CMake 发布切换

## 状态

Accepted for MineBackup 1.16.

## 决策

Windows 正式编译器保持 MSVC。Release 全目标使用 `/MT`，Debug 使用
`/MTd`，1.16 继续由现有 MSBuild 工程产出正式单 EXE；CMake 使用 Visual
Studio Generator 驱动同一 MSVC 工具链并作为强制影子门禁。两套产物必须
通过同一版本、PE 子系统、图标、字体、7-Zip、VERSIONINFO、运行库与启动
检查，但不要求二进制哈希相同。

只有在 CMake/MSVC 与 MSBuild 完全等价并经过 1.16 发布验证后，才在独立
分支删除 `.sln/.vcxproj` 并切换 Windows 正式发布入口。不支持 MinGW。

## 结果

Windows 用户无需单独安装 VC Runtime，签名和资源仍由 MSVC/`link.exe`/
`rc.exe` 链完成。过渡期存在两套入口，但源清单、版本源和发布门禁唯一，
避免一次性切换掩盖资源或分发差异。
