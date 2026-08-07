# midi2nbs

把 MIDI（`.mid` / `.midi`）文件转换为 Minecraft 音符盒（OpenNBS `.nbs`）文件的纯 C 命令行工具，**零外部依赖**，只依赖 C 标准库。

转换逻辑与 [mcbot-nodejs](https://github.com/RSSeeker/mcbot-nodejs) 的 `scripts/midi2nbs.js` 保持一致，输出为 NBS v6 格式（与 `@nbsjs/core` 逐字节一致）。

## 用法

### 拖拽使用（最简单）

直接把 `.mid` 文件拖到 `midi2nbs.exe` 上，就会在 MIDI 文件的**同目录**生成一个同名 `.nbs` 文件：

```
song.mid  ──拖到 exe 上──▶  song.nbs
```

一次可以拖多个文件，全部转换。

### 命令行

```
midi2nbs.exe <歌曲.mid> [曲速] [模式]
```

参数：

| 参数 | 说明 | 默认值 |
| --- | --- | --- |
| 曲速 | 1–100 tick/秒，控制 NBS 播放速度 | 10 |
| 模式 | `pitch`（按音高六八度分配乐器）、`auto`（按轨道分配）、`0-15`（固定乐器） | `pitch` |

示例：

```
midi2nbs.exe song.mid            # 默认 10 tick/s，pitch 模式
midi2nbs.exe song.mid 15 auto    # 15 tick/s，按轨道分配乐器
midi2nbs.exe song.mid 10 5       # 10 tick/s，固定乐器 5
```

> 默认转换完窗口会**自动关闭**；如果想看完结果再关闭，加 `--pause` 参数（例如从 cmd 里运行 `midi2nbs.exe song.mid --pause`）。自动化脚本无需额外参数。

## 转换规则

- 内置 SMF（Standard MIDI File）解析器，支持格式 0/1/2，PPQ 时间基准，支持 tempo（速度）变化事件
- 开头静音自动裁剪
- 同 tick 同乐器同音高自动去重
- 同一 tick 的多个音符自动分配到不同层（最多 40 层），避免互相覆盖
- 鼓通道（channel 9）按鼓音高映射到对应打击乐器
- `pitch` 模式（默认）：按音高六八度分配 Harp / Double Bass / Flute / Bell，实体 key 折叠到可发声范围（F#3–F#5）

## 本地构建

需要任意支持 C11 的编译器（Windows 上推荐 MinGW-w64）：

```
gcc -O2 -std=c11 -o midi2nbs.exe src/midi2nbs.c -lm
```

Linux/macOS 也可以直接编译（`gcc -O2 -std=c11 -o midi2nbs src/midi2nbs.c -lm`）。

## 自动化构建与发布

仓库内置 GitHub Actions（`.github/workflows/build.yml`），每次推送到 `main` 或 `master` 时自动：

1. 用 MinGW-w64 交叉编译 Windows x64 / x86 两个版本的 exe（静态链接，无需安装任何运行库）
2. 打包成 zip
3. 发布/更新名为 `continuous` 的 **pre-release**，附件可直接下载

推送形如 `v1.0.0` 的 tag 时，会以该 tag 名创建正式 release；tag 名包含 `-`（如 `v1.0.0-pre.1`）时发布为 pre-release。

也可以在仓库 Actions 页面手动触发 `workflow_dispatch`。

## 测试

开发时以 mcbot 的 Node.js 实现（`midi_common.js` + `@nbsjs/core`）为参考，对多个测试 MIDI 逐字节对比输出，三种模式（pitch / auto / 固定乐器）均完全一致。
