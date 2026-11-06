# fcctl

`fcctl` 用 C++ 管理 Firecracker 虚拟机。命令分成两层：

- `--config` / `-c`：总配置（主机侧）。选哪一个 Firecracker 二进制、网卡怎么建（tap / bridge / macvtap / macvlan）、挂哪座桥。
- `--run`（以及 `--stop` / `--clean` / `--prepare` / `--check` / `--generate`）：这一台虚拟机的 Firecracker JSON。

虚拟机 JSON 保持 Firecracker 原样，不另做一套格式。同一份虚拟机文件可以配不同的总配置：

```bash
/usr/local/bin/fcctl --config /etc/fcctl/config1.json --run /etc/fcctl/debian.json
/usr/local/bin/fcctl --config /etc/fcctl/config2.json --run /etc/fcctl/debian.json
/usr/local/bin/fcctl --config /etc/fcctl/config3.json --run /etc/fcctl/debian.json
```

| 总配置 | 含义 |
| :--- | :--- |
| `config1.json` | 旧 Firecracker 二进制，TAP 挂 `brg27` |
| `config2.json` | 新 Firecracker 二进制，同样挂 `brg27` |
| `config3.json` | 不是 bridge，或挂到另一座桥 |

`--config` 可写在动作前面或后面。`run`、`stop`、`clean`、`prepare`、`check`、`generate` 是动作 flag 的别名。

`fcctl` 做三件事：按总配置准备网卡，关机后删掉网卡和 API socket，以及在一组虚拟机 JSON 之间检查磁盘和 MAC。生成的单元名是 `<name>.fc.service`。安装后把二进制链接为 systemd generator；`daemon-reload` 时把当时用的 `--config` 路径写进单元。

不要让 `<name>.fc.service` 和旧模板 `firecracker@<name>.service` 同时启动。同一份虚拟机 JSON 也不能用两份总配置同时跑：盘、tap 名、API socket 都来自虚拟机文件。socket 已被占用时 `fcctl --run` 以 69 退出，单元里的 `RestartPreventExitStatus=69` 避免反复拉起。

当前 NAS（unas）默认总配置是 `/etc/fcctl/config.json`。在跑的是 `debian.fc`、`rocky.fc`、`arch.fc`、`gentoo.fc`。

## 总配置

总配置在 `--config` 指向的 JSON 里。目录里可以放多份（`config1.json`、`config2.json` …）。扫描虚拟机时会跳过这些主机文件。默认路径是 `/etc/fcctl/config.json`。

```json
{
  "config_dir": "/etc/fcctl",
  "network": [
    { "type": "bridge", "master": "brg27" }
  ],
  "binary": "/opt/firecracker/release-v1.16.1-x86_64/firecracker-v1.16.1-x86_64"
}
```

换二进制只改总配置里的 `binary`。换网卡类型或桥，改 `network`：

```json
{ "type": "tap" }
```

```json
{ "type": "bridge", "master": "brg28" }
```

`network[]` 未写 `name` 的条目套到该总配置下所有网卡。带 `name` 的条目只匹配该 `host_dev_name`。`type`：

| type | 主机上创建 | Firecracker 能否用这块当 `host_dev_name` |
| :--- | :--- | :--- |
| `tap` | 持久 TAP | 能 |
| `bridge` | 持久 TAP，再挂到 `master` | 能 |
| `macvtap` | kind=`macvtap`，父口=`master`，MAC 写成 `guest_mac` | 能。打开已有的 `/dev/tap<ifindex>`，不创建、不删除这块设备 |
| `macvlan` | kind=`macvlan`，父口=`master` | 不能当 TAP 打开 |

`macvlan` / `macvtap` 可写 `mode`：`bridge`（默认）、`vepa`、`private`、`passthru`。空数组 `network: []` 只建 TAP，不挂桥。旧键 `"bridge": "brg27"` 在没有 `network` 时仍可用。两个键一起写会报错。

macvtap 的总配置示例：

```json
{ "type": "macvtap", "master": "eth0", "mode": "bridge" }
```

这块虚拟机网卡必须带 `guest_mac`。fcctl 把该地址写到 macvtap 上，再确认本进程能打开 `/dev/tap<ifindex>`（权限不够时把节点改成当前用户的 `0600`）。Firecracker 按 `host_dev_name` 打开这个已有设备。父口要已经存在。同名接口若已在且不是 macvtap，准备失败，不会改它的类型。`--clean` 和进程退出只删这块 macvtap，不删父口。macvtap 没有 hairpin，主机通常不能经父口访问客户机。fcctl 直接 exec Firecracker，不走 jailer。二进制需要会按接口名打开 macvtap；只走 `/dev/net/tun` + `TUNSETIFF` 的上游版本打不开。

fcctl 只创建并拉起接口，不配 IP、不做 NAT、不写 nft/iptables。

内核放在 `/etc/fcctl/boot`。`binary` 为空时，按版本选择 `/opt/firecracker/release-*/firecracker-v*-x86_64`，跳过 `.debug`。

可用环境变量覆盖：`FCCTL_CONFIG`、`FCCTL_CONFIG_DIR`、`FCCTL_BRIDGE`、`FCCTL_RUNTIME`、`FCCTL_FIRECRACKER`、`FCCTL_BIN`。`FCCTL_BRIDGE` 覆盖默认（未命名）bridge 条目的 `master`。

虚拟机 JSON 里网卡名和 MAC 来自 `network-interfaces[].host_dev_name` 和 `guest_mac`，磁盘来自 `drives[].path_on_host`。tap / bridge 由 Firecracker 打开 TAP。macvtap 时 `guest_mac` 同时是主机设备地址。vsock 不是 TAP，fcctl 不管。

## 命令

```bash
fcctl --config /etc/fcctl/config.json --check
fcctl --config /etc/fcctl/config.json --generate /etc/fcctl/debian.json
sudo fcctl --config /etc/fcctl/config2.json --generate /etc/fcctl/debian.json --write
sudo systemctl daemon-reload
sudo systemctl start debian.fc
```

| 命令 | 作用 |
| :--- | :--- |
| `--run <vm.json>` | 按总配置建网卡，前台运行 Firecracker。进程退出后删除网卡和 socket |
| `--stop <vm.json>` | 对 API socket 发送 `SendCtrlAltDel`，等待进程退出 |
| `--clean <vm.json>` | 虚拟机不在运行时删除网卡和 socket |
| `--prepare <vm.json>` | 只做启动前的网卡准备 |
| `--check [vm.json]` | 可写磁盘、`guest_mac`、tap 名冲突。缺文件只告警 |
| `--generate <vm\|--all> [--write]` | 打印或写入 `<name>.fc.service`，Exec 行带上这次的 `--config` |

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

安装后把 `fcctl` 链接为 `/etc/systemd/system-generators/fcctl-generator`。`systemctl daemon-reload` 会为 `/etc/fcctl` 里的虚拟机 JSON 生成单元，并跳过总配置文件。有冲突的虚拟机会被跳过。

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
sudo systemctl daemon-reload
```

NAS unas 上提权用 `doas -n`。正在跑的虚拟机要吃到新二进制和新 ExecStart，需要 `systemctl restart <name>.fc`。
