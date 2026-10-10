# MaaMcp stdio 服务

这是一个使用官方 `mcp` Python SDK（当前锁定 `1.29.0`）的本地 stdio MCP 服务。它只观察**自己创建的** MaaCore 实例，不会连接或控制另一个正在运行的 MAA GUI，也不会伪造战斗状态。

## 启动

先准备包含 `MaaCore` 动态库和资源的目录，并安装依赖：

```bash
python -m pip install -r tools/MaaMcp/requirements.txt
python tools/MaaMcp/server.py --core-dir /path/to/maa-runtime \
  --adb /path/to/adb --address 127.0.0.1:5555
```

MCP 客户端通过 stdin/stdout 使用标准协议；日志写入 stderr。无 `--core-dir` 也可启动以检查协议，但 `observe_state` 会返回 `not_configured`，不会返回模拟状态。

## 工具边界

`observe_state`、`observe_tasks` 返回 MaaCore C API 的连接、运行、任务 ID 以及回调历史。`observe_roguelike_state` 仅返回实际收到的回调，无法证实的字段保持 `null`。`battle_action` 会执行安全闸门、状态哈希 CAS、前置条件、超时和幂等校验；由于 MaaCore C API 没有原子 deploy/retreat/skill/speed/pause 原语，当前五类动作明确返回 `unsupported`，绝不报告假成功。

如需运行普通/肉鸽任务，使用本地明确授权的 JSON 文件（不是 MCP tool）：

```bash
python tools/MaaMcp/server.py --core-dir /path/to/maa-runtime \
  --adb /path/to/adb --address 127.0.0.1:5555 \
  --task-file ./my-task.json
```

文件必须严格为 `{"type":"Fight"|"Roguelike","params":{...}}`；服务仅调用现有 `AsstAppendTask`/`AsstStart`，不重写识图或点击逻辑。
