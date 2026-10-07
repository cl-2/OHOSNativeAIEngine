# Security notes

- 不要提交 HarmonyOS 签名证书、profile、keystore、密码或本机绝对路径。
- 不要在源码中内置 API Key、私人 Ollama 地址或测试账号。远端配置应只写入应用沙箱的加密存储。
- `build-profile.json5` 只保存可共享的 SDK/产品配置；个人签名由 DevEco Studio 在本机管理。
- 诊断文件可能包含设备和会话元数据。公开前应审阅并脱敏。
- 如果凭据曾被推送到远端，即使后续删除文件，也应立即撤销/轮换凭据并清理 Git 历史。

发现问题时请提供最小复现、系统版本、ABI、构建类型和脱敏后的日志。
