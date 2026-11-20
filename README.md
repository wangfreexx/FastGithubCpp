# FastGithubCpp

Windows x64 下用 C++17 实现的 GitHub 访问加速原型，对标 `creazyboyone/FastGithub` 的核心体验，但**不做中间人、不需要安装自签 CA**。

它通过 WinDivert 改写 DNS 与回环 TCP 端口，把 GitHub 相关流量引到本机，再从 TLS ClientHello 读取 SNI / 从 HTTP 首包读取 Host，自己查询干净 DNS、测速并直连最优上游 IP，然后透传字节。客户端校验的仍是 GitHub 的真实证书。

## 功能

- **零手动配置代理**：浏览器、git、命令行都不用设置 `http_proxy`。
- **两种运行形态**：
  - 桌面：`FastGithubCpp.UI.exe` 双击运行，托盘常驻，实时速度曲线。
  - 服务：`FastGithubCpp.exe start` / `stop` 安装、启动、卸载 Windows 服务。
- **干净 DNS**：自己构造 DNS 报文，强制 TCP:53 查询，避免被本机 WinDivert 拦截器改写。
- **上游验证**：不只测 TCP 连通，还会发送客户端首包并等待上游首字节，自动跳过“TCP 能连但 TLS 立刻断”的坏 IP。
- **多协议**：HTTPS(443)、SSH(22)、git(9418)、HTTP(80)。
- **实时统计**：引擎与 UI 通过命名共享内存 `Local\FastGithubCppStats` 通信，不落盘。
- **单实例**：重复双击只会把已有窗口显示到前台。

## 目录结构

```
FastGithubCpp/
├── main.cpp                       # 引擎 + WinDivert 拦截 + Windows 服务
├── ui.cpp                         # Win32 桌面 UI
├── build.bat                      # MinGW 构建脚本
├── config.ini                     # 配置模板
├── app.ico / app.rc / make_icon.py# FG 图标生成与资源
├── WinDivert-LICENSE.txt          # WinDivert 许可
└── release/                       # 可直接运行的成品
    ├── FastGithubCpp.exe
    ├── FastGithubCpp.UI.exe
    ├── FastGithubCpp.UI.exe.manifest
    ├── config.ini
    ├── WinDivert.dll
    └── WinDivert64.sys
```

## 快速开始

### 桌面模式

以管理员身份双击 `release\FastGithubCpp.UI.exe`（manifest 会请求 UAC）。启动后：

- 窗口显示 PID、管理员状态、下载/上传速度和最近 60 秒流量曲线；
- 关闭或最小化会隐藏到托盘，托盘图标双击显示窗口，右键可退出；
- 日志写着 `WinDivert DNS interceptor started` 和 `WinDivert TCP redirector started` 即表示透明模式生效。

### 服务模式

管理员权限终端：

```bat
FastGithubCpp.exe start
FastGithubCpp.exe stop
```

`start` 会安装并启动名为 `FastGithubCpp` 的服务（自动启动），`stop` 停止并删除服务。目录移动后再执行 `start` 会自动更新服务路径。

### 手动代理模式（备用）

透明拦截需要管理员权限。若无法提权，程序仍会监听：

```
http://127.0.0.1:38457
```

可手动给 git 设置代理：

```bat
git config --global http.proxy http://127.0.0.1:38457
git config --global https.proxy http://127.0.0.1:38457
```

## 工作原理

1. WinDivert 拦截 UDP:53，把配置域名的 A 查询应答改成 `127.0.0.1`；AAAA 返回空应答强制 IPv4。
2. WinDivert 拦截回环 TCP `443/22/9418/80`，重定向到本机 `38443/38422/38418/38480`。
3. 本地监听器从 ClientHello 读 SNI、从 HTTP 首包读 Host。
4. 引擎通过 TCP:53 查询多个 DNS 服务器，汇总候选 IP 并并行测速。
5. 对每个候选 IP 发送客户端首包并等待上游首字节；验证通过才使用，否则换下一个。
6. 上下游字节直接透传，不做 TLS 解密，不需要信任自签根证书。

## 构建

需要：

- MinGW-w64 g++（已测试 11.4.0）
- WinDivert 2.2.2-A SDK，放在 `D:\dev\WinDivert-2.2.2-A`

修改 `build.bat` 中的 `BIN` / `WD` 路径后运行：

```bat
build.bat
```

输出到 `release\`。脚本会自动调用 `windres` 把 FG 图标编进 UI，并复制 WinDivert 运行文件。

## 配置

`config.ini` 常用项：

| 项 | 说明 |
|---|---|
| `port` | 手动 CONNECT 代理端口，默认 38457 |
| `redirect_https_port` 等 | 透明模式本地接收端口 |
| `dns` | DNS 服务器，分号分隔；引擎强制走 TCP:53 |
| `domains` | 需要加速的域名，支持 `*.` 通配 |
| `verbose` | 是否写 `fastgithubcpp.log` |
| `connect_timeout_ms` / `upstream_timeout_ms` | 测速与连接超时 |

## 已知限制

- 仅 Windows x64；透明拦截依赖 WinDivert 驱动，必须管理员。
- 不做 MITM，因此不支持原项目的 SNI 改写、CDN 替换等功能。
- 非 GitHub 域名走系统 DNS 直连，不受影响。
- Linux arm64 尚未实现，核心转发逻辑可复用，透明拦截需改用 nftables/iptables TPROXY。

## 许可与第三方组件

本项目自身代码按 MIT 使用。

`release\WinDivert.dll` 与 `release\WinDivert64.sys` 来自 [WinDivert](https://reqrypt.org/windivert.html) 2.2.2-A，采用 LGPLv3 或 GPLv2 双许可，完整许可文本见 `WinDivert-LICENSE.txt`。使用这些二进制文件即表示接受其许可条款。
