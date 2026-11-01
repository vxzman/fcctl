# fcctl

`fcctl` 用 C++ 管理 Firecracker 虚拟机。虚拟机配置就是 Firecracker 自己的 JSON，和主机设置放在同一个目录 `/etc/fcctl`。`config.json` 是主机设置，其余 `*.json` 是虚拟机。程序不另做一套虚拟机格式，也不使用 macvtap。

NAS 上现在的模板是 `firecracker@.service`：启动前用 `ip tuntap` 建 `fc-<name>`，挂到 `brg27`，再执行

```text
firecracker --api-sock /run/firecracker/<name>.sock --config-file /etc/fcctl/<name>.json
```

正在跑的是 v1.16.1，`debian` 和 `rocky` 各一个。`fcctl` 做三件事：启动前准备这块 tap，关机后删掉它和 API socket，以及在一组 JSON 之间检查磁盘和 MAC。生成的单元名是 `<name>.fc.service`，例如 `debian.fc.service`。

不要让 `debian.fc.service` 和正在运行的 `firecracker@debian.service` 同时启动。它们用同一块盘、同一个 tap、同一个 API socket。socket 已被占用时 `fcctl run` 以 69 退出，单元里的 `RestartPreventExitStatus=69` 避免 `Restart=on-failure` 反复拉起。

## 配置

虚拟机文件保持 Firecracker 原样。网卡名和 MAC 来自 `network-interfaces[].host_dev_name` 和 `guest_mac`，磁盘来自 `drives[].path_on_host`。

桥不在 Firecracker JSON 里。主机设置是同目录的 `/etc/fcctl/config.json`：桥 `brg27`，配置目录 `/etc/fcctl`，运行目录 `/run/firecracker`。内核放在 `/etc/fcctl/boot`。`binary` 为空时，按版本选择 `/opt/firecracker/release-*/firecracker-v*-x86_64`，跳过 `.debug`。

可用环境变量覆盖：`FCCTL_CONFIG`、`FCCTL_CONFIG_DIR`、`FCCTL_BRIDGE`、`FCCTL_RUNTIME`、`FCCTL_FIRECRACKER`、`FCCTL_BIN`。

## 命令

```bash
fcctl check
fcctl generate /etc/fcctl/debian.json
sudo fcctl generate --all --write
sudo systemctl daemon-reload
sudo systemctl start debian.fc
```

| 命令 | 作用 |
| :--- | :--- |
| `run <vm.json>` | 检查冲突，建 tap 并加入桥，前台运行 Firecracker。进程退出后删除 tap 和 socket |
| `stop <vm.json>` | 对 API socket 发送 `SendCtrlAltDel`，等待进程退出 |
| `clean <vm.json>` | 虚拟机不在运行时删除 tap 和 socket |
| `prepare <vm.json>` | 只做启动前的网卡准备 |
| `check [vm.json]` | 可写磁盘、`guest_mac`、tap 名冲突。缺文件只告警 |
| `generate <vm\|--all> [--write]` | 打印或写入 `<name>.fc.service` |

`debian.fc.service` 的主体是：

```ini
[Service]
Type=simple
SyslogIdentifier=debian.fc
ExecStart="/usr/local/bin/fcctl" run "/etc/fcctl/debian.json"
ExecStop="/usr/local/bin/fcctl" stop "/etc/fcctl/debian.json"
ExecStopPost=-"/usr/local/bin/fcctl" clean "/etc/fcctl/debian.json"
TimeoutStopSec=90
Restart=on-failure
RestartPreventExitStatus=69
RestartSec=2
```

`/dev/` 上的磁盘会写成 systemd device 依赖。串口仍是 Firecracker 的标准输入输出；单元把标准输入设为空，输出进 journal。客户机里 `reboot` 和 `poweroff` 在 Firecracker 1.16 上都是进程退出码 0，单元不会把这种退出当成失败再拉起。

安装后把 `fcctl` 链接为 `/etc/systemd/system-generators/fcctl-generator`。`systemctl daemon-reload` 会为 `/etc/fcctl/*.json` 生成单元，并跳过 `config.json`。有冲突的虚拟机会被跳过。

## 编译

`make test` 用系统 g++ 编译并跑检查。`make static` 在 Alpine 容器里用 musl 静态链接，产物是 `build/fcctl`，可以直接拷到 NAS。

```bash
make test
make static
```
