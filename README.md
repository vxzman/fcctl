# fcctl

`fcctl` 用 C++ 管理 Firecracker 虚拟机。虚拟机配置就是 Firecracker 自己的 JSON，和主机设置放在同一个目录 `/etc/fcctl`。`config.json` 是主机设置，其余 `*.json` 是虚拟机。程序不另做一套虚拟机格式。

`fcctl` 做三件事：启动前按主机 `network[]` 准备网卡，关机后删掉它和 API socket，以及在一组 JSON 之间检查磁盘和 MAC。生成的单元名是 `<name>.fc.service`，例如 `debian.fc.service`。安装后把二进制链接为 systemd generator，`daemon-reload` 会为每个虚拟机 JSON 写出单元。

不要让 `<name>.fc.service` 和旧模板 `firecracker@<name>.service` 同时启动。它们用同一块盘、同一个 tap、同一个 API socket。socket 已被占用时 `fcctl --run` 以 69 退出，单元里的 `RestartPreventExitStatus=69` 避免 `Restart=on-failure` 反复拉起。

当前 NAS（unas）上四台在跑：`debian.fc`、`rocky.fc`、`arch.fc`、`gentoo.fc`，Firecracker 为 v1.17.0。主机配置目录 `/etc/fcctl`，运行目录 `/run/firecracker`，网卡挂在桥 `brg27`。

## 配置

虚拟机文件保持 Firecracker 原样。网卡名和 MAC 来自 `network-interfaces[].host_dev_name` 和 `guest_mac`，磁盘来自 `drives[].path_on_host`。Firecracker 打开的是 TAP：进程对 `/dev/net/tun` 做 `TUNSETIFF`。vsock 不是 TAP，fcctl 不管。

桥、macvtap、macvlan 写在主机 `/etc/fcctl/config.json` 的 `network` 里，不进虚拟机 JSON。fcctl 只创建并拉起接口，不配 IP、不做 NAT、不写 nft/iptables。

```json
{
  "config_dir": "/etc/fcctl",
  "network": [
    { "type": "bridge", "master": "brg27" }
  ],
  "binary": "/opt/firecracker/release-v1.17.0-x86_64/firecracker-v1.17.0-x86_64"
}
```

未写 `name` 的条目套到所有虚拟机网卡。带 `name` 的条目只匹配该 `host_dev_name`。`type`：

| type | 主机上创建 | Firecracker 能否用这块当 `host_dev_name` |
| :--- | :--- | :--- |
| `tap` | 持久 TAP | 能 |
| `bridge` | 持久 TAP，再挂到 `master` | 能（NAS 上 `fc-debian` → `brg27`） |
| `macvtap` | kind=`macvtap`，父口=`master` | 不保证。FC 走 TUNSETIFF，不是 `/dev/tapN` |
| `macvlan` | kind=`macvlan`，父口=`master` | 不能当 TAP 打开 |

`macvlan` / `macvtap` 可写 `mode`：`bridge`（默认）、`vepa`、`private`、`passthru`。空数组 `network: []` 只建 TAP，不挂桥。旧键 `"bridge": "brg27"` 在没有 `network` 时仍可用，等价于上面那条。两个键一起写会报错。

内核放在 `/etc/fcctl/boot`。`binary` 为空时，按版本选择 `/opt/firecracker/release-*/firecracker-v*-x86_64`，跳过 `.debug`。

可用环境变量覆盖：`FCCTL_CONFIG`、`FCCTL_CONFIG_DIR`、`FCCTL_BRIDGE`、`FCCTL_RUNTIME`、`FCCTL_FIRECRACKER`、`FCCTL_BIN`。`FCCTL_BRIDGE` 覆盖默认（未命名）bridge 条目的 `master`。

## 命令

```bash
fcctl --config /etc/fcctl/config.json --check
fcctl --config /etc/fcctl/config.json --generate /etc/fcctl/debian.json
sudo fcctl --config /etc/fcctl/config.json --generate --all --write
sudo systemctl daemon-reload
sudo systemctl start debian.fc
```

启动一条虚拟机：

```bash
/usr/local/bin/fcctl --config /etc/fcctl/config.json --run "/etc/fcctl/debian.json"
```

| 命令 | 作用 |
| :--- | :--- |
| `--run <vm.json>` | 检查冲突，按 `network[]` 建网卡，前台运行 Firecracker。进程退出后删除网卡和 socket |
| `--stop <vm.json>` | 对 API socket 发送 `SendCtrlAltDel`，等待进程退出 |
| `--clean <vm.json>` | 虚拟机不在运行时删除网卡和 socket |
| `--prepare <vm.json>` | 只做启动前的网卡准备 |
| `--check [vm.json]` | 可写磁盘、`guest_mac`、tap 名冲突。缺文件只告警 |
| `--generate <vm\|--all> [--write]` | 打印或写入 `<name>.fc.service` |

`--config` / `-c` 指定主机 JSON，可写在动作前后。`run`、`stop`、`clean`、`prepare`、`check`、`generate` 仍是这些 flag 的别名。

`debian.fc.service` 的主体是：

```ini
[Service]
Type=simple
SyslogIdentifier=debian.fc
ExecStart="/usr/local/bin/fcctl" --config "/etc/fcctl/config.json" --run "/etc/fcctl/debian.json"
ExecStop="/usr/local/bin/fcctl" --config "/etc/fcctl/config.json" --stop "/etc/fcctl/debian.json"
ExecStopPost=-"/usr/local/bin/fcctl" --config "/etc/fcctl/config.json" --clean "/etc/fcctl/debian.json"
TimeoutStopSec=90
Restart=on-failure
RestartPreventExitStatus=69
RestartSec=2
```

`/dev/` 上的磁盘会写成 systemd device 依赖。串口仍是 Firecracker 的标准输入输出；单元把标准输入设为空，输出进 journal。客户机里 `reboot` 和 `poweroff` 在 Firecracker 1.16 及以上都是进程退出码 0，单元不会把这种退出当成失败再拉起。

安装后把 `fcctl` 链接为 `/etc/systemd/system-generators/fcctl-generator`。`systemctl daemon-reload` 会为 `/etc/fcctl/*.json` 生成单元，并跳过 `config.json`。有冲突的虚拟机会被跳过。

## 编译与安装

`make test` 用系统 g++ 编译并跑检查。`make static` 在 Alpine 容器里用 musl 静态链接，产物是 `build/fcctl`（linux amd64），Release 里的文件名是 `fcctl-linux-amd64`。

```bash
make test
make static
```

拷到主机后：

```bash
sudo install -m 755 fcctl-linux-amd64 /usr/local/bin/fcctl
sudo mkdir -p /etc/fcctl /run/firecracker
sudo ln -sf /usr/local/bin/fcctl /etc/systemd/system-generators/fcctl-generator
# 已有 /etc/fcctl/config.json 时不要覆盖
sudo systemctl daemon-reload
```

NAS unas 上提权用 `doas -n`，不用 sudo。正在跑的虚拟机要吃到新二进制和新 ExecStart，需要 `systemctl restart <name>.fc`。
