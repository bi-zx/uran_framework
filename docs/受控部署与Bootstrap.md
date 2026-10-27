# 受控部署、授权与 Bootstrap

本文是 2026-10-27 确定的部署安全边界和 v1 实现说明，补充《多仓库拆分与完整迁移方案》第 6 节。常规部署不使用 SSH、SCP 或 FRPC。云端 agent 发送签名的结构化任务，经 Core 路由后，由独立监督进程下载和安装可信发行物。用户另行批准具体请求后，设备才能开放临时远程维护。

## 1. 职责与信任

```text
agent → 云端 MQTT/TLS → Core → management worker → 私有 Unix socket
                                                    ↓
                                             部署监督进程
                                              ├─ HTTPS 制品获取
                                              ├─ staging / 校验 / 状态备份
                                              ├─ Core 与依赖服务生命周期
                                              └─ 提交 / 失败补偿 / 重启恢复
```

Core 是唯一云端管理入口。大文件由监督进程内的传输服务处理，不经过 MQTT 或 Core 主事件循环。监督进程不开放 TCP 管理端口，也不接收云端任意命令、目标路径或待执行脚本文本。原生宿主的管理 socket 不向云端暴露。

三类密钥相互独立：

| 身份 | 私钥持有者 | 设备端用途 |
| --- | --- | --- |
| agent | 指定云端 agent 的凭据服务 | 验证任务身份、目标设备和有效期 |
| user | 经过独立用户认证的批准服务或用户本地工具 | 验证具体任务的授权及授权撤销 |
| publisher | 发行流水线的发布服务 | 验证制品来源和确定内容 |

设备只保存上述公钥。agent 不应获得 user/publisher 私钥；`controller`、MQTT 用户名或 `approved: true` 均不能代替用户签名。云平台还应配置每台设备的 MQTT Topic ACL；不能通过共享整个租户的读写凭据提供隔离。

bootstrap 采用 RSA/SHA-256 签名，支持 2048/3072/4096 位 RSA 公钥，通过系统 OpenSSL 验证，兼容 Python 3.6 与 OpenSSL 1.1.1。私钥格式为 OpenSSL 可读 PEM。签名输入为 UTF-8 JSON：对象键按 Unicode 码点排序、无空白分隔、保留 Unicode、禁止浮点数、重复键和非有限数字。签名文档时间使用完整 Unix **秒**整数；不能使用旧运动协议的低 32 位纳秒值。跨语言发行端必须按此规则生成相同字节。

## 2. 一次性设备准备

目标机需要匹配平台的基础 Linux、Python 3.6+、OpenSSL、系统证书和 native SDK 的系统动态库。框架默认依赖 yaml-cpp、glibc 和 libstdc++；bootstrap 安装会执行宿主和 Python 导入预检。需要额外动态库时，应在发行环境提供匹配的平台依赖，不在设备端临时编译。业务需要的 ROS、厂商系统服务、驱动、内核/固件和硬件权限必须预先满足，或由产品安装流程处理。

用户只需准备联网与时间同步、验证并安装一个 bootstrap 包、输入注册码或导入独立设备凭据。初始包包含原生 Core、worker 适配器、管理与 MQTT worker、HTTPS 获取器、事务引擎、独立监督进程和签名的空管理 Profile。业务插件与机械臂不属于这个初始包。

首次安装需要一个干净的管理数据目录。安装器拒绝覆盖现有部署；它不会自动停止旧机器人服务或迁移旧目录。已有设备迁移时，先制定停机窗口，迁出身份、凭据和标定，停止旧服务，再启用新管理服务，避免两个 Runtime 同时控制硬件。

生产安装使用 root 运行一次安装器，指定已存在的非 root 服务账号；此后监督进程和发布代码均以该账号运行。bootstrap 程序和本地授权配置位于 root 所有、组/其他用户不可写的目录。配置与信任公钥位于 bootstrap 上级的独立 `device-<device_id>` 目录，不能放到服务账号可替换的可写父目录中。普通用户安装模式用于开发验证，不提供这种操作系统级配置保护。

```text
/opt/uran/bootstrap/
├── 1.0.0/                 # 固定的受信管理程序，root 所有
└── device-robot_001/       # 安装器按 prefix.parent 推导的设备配置

/var/lib/uran/
├── active.json            # 权威提交记录
├── current -> releases/<release_id>
├── releases/              # 不可变整套发行目录
├── receipts/              # 签名发行描述与摘要
├── cache/                 # 制品、可续传的临时文件
├── staging/               # 校验前后的暂存
├── tasks/                 # 幂等任务及恢复日志
├── backups/               # 文件状态快照
├── data/state/            # 可备份、可迁移的运行状态
├── data/shared/           # 身份引用、现场标定等持久化数据
├── run/                   # 仅本机可访问的 socket、进程身份记录
├── recovery/              # 从受信 bootstrap 提取的空管理 Core
└── logs/                  # 审计、Core、服务和检查日志
```

实际设备配置路径由安装器打印；以上 prefix 示例对应 `/opt/uran/bootstrap/device-robot_001/device.json`。`data/shared` 不随切换或回滚覆盖。云端 MQTT 凭据保存在独立受保护的设备配置中，不写入制品、发布锁或审计记录。

## 3. 制作和安装 bootstrap

在与目标架构、系统发行版、libc 和 Python 主次版本均匹配的发行环境准备安装后的 framework SDK 和私有 Python 依赖目录，并使用对应的 Python 解释器执行构建与安装。bootstrap 记录这些平台条件，安装器会检查自身和所选服务解释器，避免把含 CPython 扩展的现代 Python 依赖安装到 Python 3.6 设备。Python 目录由已发布 wheel 安装组成，应包含 `uran_worker_sdk`、`uran_interfaces`、`uran_mqtt_plugin`、`paho-mqtt` 及其依赖。不得使用相邻源码目录或目标机全局 `PYTHONPATH`。Python 3.6 需要 dataclasses backport、jsonschema 3.2 和 paho-mqtt 1.6；现代 Python 按各包环境标记解析依赖。发行时记录实际 wheel 版本与摘要。

下面的命令均从已安装 SDK 调用。密钥生成在相应身份的安全环境分别执行，示例文件名不代表应把三类私钥放到同一 agent 中。

```bash
uran-deploy keygen --private-key publisher.key.pem --public-key publisher.pub.pem
uran-deploy build-bootstrap \
  --sdk-prefix /opt/uran-sdk \
  --python-site /opt/uran-release-python \
  --source-ref '<framework 确定版本或 commit>' \
  --private-key publisher.key.pem --key-id publisher-2026 \
  --artifact-url https://artifacts.example.com/seed-1.0.0.tar.gz \
  --output uran-bootstrap-1.0.0.tar.gz
```

输出包和 detached `.sig`；包内部还包含逐文件清单与签名。`source-ref` 和包清单用于追溯，发行端应保留构建环境、依赖锁和测试结果。v1 的 bootstrap 制作在匹配的目标架构/系统环境完成，不从宿主平台推测跨编译 SDK 的目标环境。

用户必须通过独立可信渠道获得 publisher 公钥，**先在包外验签，再解包并运行安装程序**：

```bash
openssl dgst -sha256 -verify publisher.pub.pem \
  -signature uran-bootstrap-1.0.0.tar.gz.sig uran-bootstrap-1.0.0.tar.gz
mkdir bootstrap-package
tar -xzf uran-bootstrap-1.0.0.tar.gz -C bootstrap-package
```

从 SDK 的 `config/deployment/device.example.json` 创建本机配置，导入三类公钥。可以预置每台设备独立的 MQTT 凭据，或先调用注册码接口：

```bash
python3 bootstrap-package/bin/uran-deploy enroll \
  --endpoint https://control.example.com/v1/device-enroll --config device.json
```

注册码通过终端隐藏输入，不放在命令行参数。接口使用 HTTPS 且不允许跳转。请求为 `schema_version/device_id/registration_code`；响应为 `device_id/cloud`，其中 cloud 是完整的 MQTT/TLS 连接配置。服务端必须核验并一次性消费注册码；这个仓库实现客户端及接口契约，不包含生产云平台的注册服务或用户审批 UI。

配置完成后执行：

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin uran
sudo python3 bootstrap-package/bin/uran-deploy install bootstrap-package \
  --config device.json --trusted-key publisher.pub.pem \
  --root /var/lib/uran --prefix /opt/uran/bootstrap/1.0.0 \
  --python /usr/bin/python3 --service-user uran
```

已有 `uran` 账号时跳过 useradd。安装器安装并启用 `uran-supervisor.service`，使用 `KillMode=control-group`、非 root、只读系统路径和独立可写数据目录。硬件产品所需 dialout/video 等组权限由用户按产品说明预置。首次联网和 TLS 注册需可达；网络或依赖未满足时服务会重试，安装成功不等于硬件业务验收成功。

开发验证可省略 `--service-user`，按安装器输出的配置路径手动执行 `uran-deploy supervise --config ...`；监督进程拒绝以 root 运行。

## 4. 发行物与事务

发行物是完整的已安装目录，不是源码仓库集合。生成工具从目录计算所有文件的 SHA-256、长度和执行位，写入 `deployment.lock.json`，打包后签名发行描述。锁 schema 位于 `schemas/deployment/release-lock.schema.json`。包身份、精确版本、来源及摘要不得为空。依赖服务的启动顺序由有向无环图确定。

```bash
uran-deploy build-release /opt/product-assembly \
  --spec release-spec.json --url https://artifacts.example.com/product-1.0.0.tar.gz \
  --private-key publisher.key.pem --key-id publisher-2026 \
  --output product-1.0.0.tar.gz
```

规格示例为 `config/deployment/release-spec.example.json`，其中目标架构、发行版、libc、Python 版本和包摘要均为必须替换的占位值，不能直接作为某一型号机器人的默认配置。`uran_profiles/scripts/build_release.py` 提供发行端包装，并拒绝明显的源码控制目录和设备凭据文件。发布包中不能包含现场密钥或标定文件。

runtime 装配采用 JSON（也是合法 YAML），支持 `${release}`、`${device_id}`、`${data}`、`${shared}`、`${root}` 引用。业务实例使用本地 `${device_id}`，不能覆盖 `uran_management/uran_cloud/uran_runtime`。管理插件由监督进程从固定 bootstrap 注入，产品包不能替换其配置或把旧云端入口设为管理主协议。原生宿主须支持框架的标准 `--config/--state-file/--control-socket` 参数。API 2.1 与 native ABI 3 不兼容的包被拒绝。

事务顺序如下：

1. 验证 agent、设备、请求有效期、publisher 和本地策略/用户授权；持久化受理记录。
2. 检查平台和可用空间；从允许的 HTTPS origin 获取制品。允许恢复部分下载；跳转后的 origin 仍须允许。最终长度和摘要必须与签名描述一致。
3. 限量解包，拒绝路径穿越、符号/硬链接、特殊文件、重复条目和超限内容；验证整套锁及所有文件，检查组件/服务引用、环、组件必需能力和管理实例保留名称。
4. 在旧 Runtime 仍运行时执行有界 `preflight`，由产品判断停机条件、硬件状态和外部依赖。前置检查失败时不停止旧版本。
5. 再次校验授权，停止旧 Core 和依赖服务，确认其进程退出。保存 `data/state` 的持久化快照；不同时启动新旧控制器。
6. 按声明迁移文件状态、切换候选目录、依序启动依赖服务及新 Core。管理实例立即 active，业务实例先保持 configured；通过服务健康和产品 `checks` 后，再次确认授权并激活原本声明自动启动的业务实例。执行 `post_activate` 检查，重新确认授权和取消状态。声明不自动激活的实例继续保持 configured。
7. 原子提交 `active.json` 后报告成功。`current` 是运行选择的便捷链接，不能用它单独判定事务提交。业务插件必须遵循 configure 不驱动硬件、deactivate 安全释放控制权的生命周期契约。
8. 失败时停止候选版本，恢复本次升级前状态和旧发行物；恢复失败明确报告 `recovery_failed`。监督进程重启先终止有 PID/启动时间匹配记录的残留进程，再恢复最后提交版本。

如果最后提交版本已经损坏或无法启动，监督进程从 root 所有的签名 bootstrap 中提取空管理 Profile，启动仅含管理和云端连接的 Core，使用独立 `data/recovery` 状态目录。它保留 `active.json` 的原提交记录和业务状态，明确上报 `uran.management.recovery.v1`，不启动任何产品组件，也不静默跨 schema 回退数据。云端可继续通过同一授权接口提交修复发行物或明确授权的回滚。修复提交后退出管理恢复模式。bootstrap、底层动态库或网络本身损坏时仍需本地恢复。

同一 request ID 和相同请求返回既有结果；相同 ID 携带不同请求被拒绝。设备一次执行一个部署事务。相同 release ID 不允许替换内容。下载和暂存失败不会停止健康的旧版本。已安装版本的启动恢复只校验本地制品，不要求重新下载或继续允许其原下载域名。

文件状态 schema 改变必须声明 `migration`，且仅支持 `strategy: snapshot` 的可恢复文件迁移。跨 schema 回滚必须由已签名请求明确设置 `restore_state: true`，使用原升级前快照，会放弃快照之后的新状态。外部数据库、云端或硬件的不可逆副作用不属于文件快照事务；这类升级必须使用产品专门恢复流程，不能宣称通用自动回滚。

v1 管理完整的应用发行物、用户态依赖服务和文件状态，不对 apt、驱动、固件、任意 systemd 单元或 udev 规则执行远程 root 安装。依赖不满足时拒绝/恢复。固定 bootstrap 监督程序及其权限配置本身的升级保留为本地管理员操作；发行事务支持其所管理的 Core 自身升级。

## 5. 任务授权与云端契约

权威 wire schema 位于 `uran_interfaces/schemas/management/`。MQTT 下行保持原设备 Topic，增加消息：

```json
{"msg_type":"management_cmd","device_id":"robot_001","envelope":{"algorithm":"rsa-sha256","key_id":"agent-2026","payload":{},"signature":"..."}}
```

完整 command payload 包含 `schema_version/kind/request_id/device_id/actor_id/issued_at/expires_at/action/args`。可用操作：

| action | args | 权限 |
| --- | --- | --- |
| deployment.plan | release：publisher 签名发行描述 | 已认证 agent/user，预览静态计划 |
| deployment.apply | release | 本地预授权或绑定请求的用户 grant |
| deployment.status | request_id | 发起 agent 或 user |
| deployment.cancel | request_id | 发起 agent 或 user，按事务边界安全补偿 |
| deployment.rollback | release_id、restore_state | 本地明确允许回滚或用户 grant；限上一个提交版本 |
| maintenance.open | service_id、ssh_public_key | 必须有用户 grant，且本地已启用该服务 |
| maintenance.close | 空对象 | 会话所属 agent 或 user |
| authorization.revoke | grant_id | user 签名 |

user 直接签名部署任务表示用户明确发起操作。agent 的自动部署须匹配本地 `auto_deploy` 中的 **Profile、agent subject、publisher key ID 和 action**；默认列表为空。规则不能自动授权维护会话。

临时 grant 包含 `grant_id/device_id/actor_id/request_sha256/issued_at/expires_at` 等字段，由 user 签名，并放在 agent envelope 的 `grant` 成员。`request_sha256` 绑定整个具体 command payload，包括版本、摘要、操作和参数。不同请求不能复用 grant；撤销记录与已用记录持久化。任务 TTL 是受理窗口；grant 在开始停机和提交前重新检查，下载耗时不能使过期授权继续激活。

发行端/云端可以使用 CLI 生成请求。`command` 只生成 payload，`sign` 由 agent 身份签名；用户在批准界面核对完整请求，再由独立用户身份执行 `approve`：

```bash
uran-deploy command --device-id robot_001 --actor-id deployment_agent \
  --action deployment.apply --args apply-args.json --output command.json
uran-deploy sign command.json --private-key agent.key.pem --key-id agent-2026 --output request.json
uran-deploy approve request.json --private-key user.key.pem --key-id user-2026 --ttl 600 --output approved.json
```

本地 `request --socket ...` 仅供设备管理员测试同一签名契约，不能在云端开放该 socket。部署 worker 的普通 operation 成功表示任务已被监督进程受理；真正完成以 `uran.management.state.v1` 的 `status=succeeded, phase=committed` 为准。状态通过既有 `uplink_data` 包装上报；Core 升级后管理 worker 从持久化任务恢复上报。取消、失败、恢复失败分别有明确状态，不能只看 systemd active。

## 6. 临时 FRPC / SSH

旧 `frpc_ctrl` 消息被明确拒绝。`uran_plugins/network/frpc` 提供可选 provider，不属于默认 bootstrap 依赖。只有本地管理员预置 provider、FRPC 二进制、FRPS/TLS 配置和 SSH CA 后，才能启用 `maintenance.enabled`。

授权绑定具体维护请求和 agent 的 SSH 公钥。provider 只建立一条到固定 loopback 维护 SSHD 的 TCP 通道，默认 `127.0.0.1:2222`；agent 不能指定其他本地地址、端口或 FRPS 凭据。使用短期 OpenSSH 用户证书，限制 principal、loopback 来源和有效期，禁用 agent/X11/端口转发及用户 rc，仅开启终端权限。FRPC 转发后 SSHD 看到的是 loopback，不能用云端 IP 作为证书 source-address；云端 IP 白名单应在 FRPS 暴露端口的防火墙实施。

本地管理员须预置独立维护 SSHD：只监听 loopback 的非特权端口，AllowUsers 限定专用非 root 维护账号，关闭密码/交互认证、root 登录与转发，仅信任维护 CA/指定 principal，默认不授予 sudo。不要把设备通用的密码登录 SSHD 直接映射出去。provider 的本地服务配置包含 `frpc_binary/server_addr/server_port/frps_token/remote_port/tls_server_name/tls_ca_file/ssh_ca_key/ssh_principal/ssh_user`；可选 `local_ssh_port` 默认为 2222。

会话到期、用户撤销或监督进程退出时关闭通道与对应进程组，阻止继续通过通道传输；证书也会到期。返回 `state=starting` 只表示 provider 已启动，不声称 FRPS 可达或已登录 SSH。真实网络和 SSHD 连接仍需产品环境验收。

需要理解权限边界：获得交互 Shell 的账号本身可以运行程序和读写其有权访问的文件。平台可以禁止 agent 调用 SCP，但无法在开放任意 Shell 的同时从技术上禁止所有文件字节传输。严格要求所有文件流经 Core 的场景，应保持交互 SSH 禁用，使用更受限的诊断服务；用户批准 SSH 是明确的临时维护权限扩展。

## 7. 验证与接入范围

`scripts/test_deployment.sh` 使用安装后的 SDK 和私有依赖目录执行测试；通过 `URAN_DEPLOY_TEST_SDK`、`URAN_DEPLOY_TEST_PYTHON_SITE` 和 `URAN_PYTHON` 指定相匹配的环境。集成测试运行真实 Core、管理 worker、TLS MQTT broker 和 HTTPS 制品服务器，验证首次安装、Core 自升级、坏健康检查、监督进程强制终止恢复、已提交版本损坏后的管理恢复与在线修复、旧 FRPC 消息拒绝。其他测试覆盖签名角色、授权范围与撤销、幂等、并发拒绝、取消、摘要/归档攻击、状态迁移及恢复。生产安装使用 systemd 控制组兜底；每个受管进程另有 subreaper 监护，负责清理其后台后代，发布的长期服务仍须以前台方式运行。

当前实现没有连接生产云平台或修改 CyberDog2 的现有部署。生产云端仍需按契约接入注册、用户审批签名、制品发布和任务展示；现有 CyberDog2 Profile 需要转换为已安装制品发行物，并另做停机/回退和硬件验收。软件机制验证不代替实机验证。

本轮具体环境、测试结果、开发验证包和未覆盖边界见[部署验证记录（2026-10-27）](部署验证记录-20261027.md)。

随后对产品默认值的复查与修正见[通用层硬编码检查](通用层硬编码检查-20261027.md)。
