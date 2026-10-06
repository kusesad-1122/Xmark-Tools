## Xmark-Tools v2.9.1

### 本次更新

**修复：开启机型伪装后开机卡顿（卡开机画面）**

v2.9.0 把机型伪装挪到开机早期（修复"必须二次重启"），但完整应用一个机型
需要约 65 次 resetprop 调用，全部同步挤在开机最繁忙的阶段执行，
导致开机画面卡住数秒到十几秒。

现拆为两级：

- **同步（开机早期）**：只写系统启动时必读的顶层属性，调用次数
  65 → 12，开机阶段 1~2 秒内完成，伪装在进入系统前就已生效
- **后台（自动补齐）**：分区级属性由后台任务补齐，不影响开机速度，
  也不影响伪装效果

**修复：「重置设备标识」在部分环境下完全无效**

重置设备标识脚本直接调用 `resetprop`，但通过 WebUI 执行时命令通道
的 PATH 不包含 KernelSU / APatch 的二进制目录（`/data/adb/ksu/bin`、
`/data/adb/ap/bin`），导致序列号 / IMEI / MAC 的修改全部静默失败。

- 现在会自动在三个位置查找 resetprop（Magisk / KSU / APatch）
- 找不到时不再静默跳过：日志明确记录，未受影响的功能
  （Android ID / GAID / GSF）继续执行
- WebUI 开关现在会给出结果反馈（成功 / 部分成功 / 失败原因），
  不再是无声失败

### 说明

- 开机卡顿修复仅影响 v2.9.0 引入的开机路径，WebUI 手动切换机型的行为不变
- 「重置设备标识」中 GAID / GSF ID 两项本就设计为重启后生效

---

## Xmark-Tools v2.9.0

### 本次更新

**修复：伪装机型冷启动不生效（长期存在的必现问题）**

此前开启「伪装机型」后必须额外软重启一次才生效，根因有两个：

- `scripts/model_spoof.sh` 的 `boot` 动作全仓无调用者，是一段死代码
- 唯一会跑逻辑的 `service.sh` 开头阻塞等 `sys.boot_completed=1`，
  等到那时 zygote / system_server 早已缓存 `ro.product.*`，写了也不生效

现在改为在 `post-fs-data.sh`（zygote 启动之前）应用伪装，
开机后首次进入系统即为伪装后的机型，无需任何额外重启。

**机型库扩充至 48 个机型**

- 补齐全部机型的 `ro.build.fingerprint`，覆盖率从 8/48 提升到 48/48
- 新增从 fingerprint 反解 `build.id` / `version.incremental` / `build.type` /
  `build.tags`，并同步到 `ro.{system,vendor,odm,product,system_ext}.build.*`，
  避免指纹与各分区 build 信息互相矛盾
- 强制 `ro.build.tags=release-keys`、`ro.build.type=user`
- 修正 resetprop 调用方式：去掉 `-n`（init 已写值会导致全部跳过）与
  `-p`（会在 `/data/property` 留残留）
- 刻意不改 `ro.build.version.release` / `sdk`：指纹里的版本号与本机真实
  SDK 不一定相同，硬改反而制造「sdk=31 但 release=16」这类一眼假的矛盾
- 全部 48 个 fingerprint 统一为标准 6 段格式，修正 iPhone 两处参数错位

**新增：DRM 层防标记**

- 新增 `lib/drmid_hook.so`：在进程内定位 libcrypto(BoringSSL) 的
  `SHA256_Final`，在它执行前追加一次 32 字节随机盐（`getrandom()` 真随机，
  hex 成 64 字符）
- Widevine L1 的 device-unique-id 与设备签名都依赖 SHA256 链式计算，
  派生 ID 因此每次开机都不同且彼此不关联，无法被服务端聚类成同一批设备，
  交叉验证失效
- 实现上只patch `SHA256_Final` 一个点，patch 面最小化；符号定位走
  PT_DYNAMIC，不依赖可能被 strip 的 section header
- trampoline 按指令类做重定位（B/BL 超界改绝对跳转序列，ADR 拆成
  ADRP+ADD 扩范围到 ±4GB）；遇B.cond/CBZ/TBZ/LDR-literal/PRFM 等
  无法安全搬移的指令，直接放弃整个 hook——宁少一层保护不可黑屏
- 全链路静默失败：hook 失败仅使本层失效，Widevine 自身照常工作
- 通过 `wrap.<service>` + LD_PRELOAD 注入 Widevine HAL，与项目内既有
  native 库保持同一模式
- 附带 `sepolicy.rule`：只放行必需的最小权限集合（不写
  `allow hal_drm_widevine * * *` 这类全开规则，全开会给 vendor 域挂上
  几乎无限制的访问能力，检测面反而更大）
- WebUI 新增开关，开启前有确认弹窗并明示代价

**其他**

- 修复 `scripts/monitor_app.sh` 的封装损坏（收尾引号未闭合，
  `sh`/`dash`/`bash` 全部报 unexpected EOF，脚本无法执行）
- 多个动作失败时退出码撒谎（`apply` 失败仍 `exit 0`）已修正为 `exit 1`，
  避免调用方误判成功
- service 名解析改为三级兜底（svc 记录 → init rc 扫描 → 候选名），
  适配不同 ROM 的 Widevine 服务名差异
- DRM 层开机路径改为零 service 重启：`service.sh` 调用 `boot` 而非
  `enable`，不再每次开机都 stop/start 打断正在恢复的 DRM 播放
- 发布流水线：新增 NDK 安装与 `libdrmid_hook.so` 编译步骤、
  4 项静态自检、机型表自洽性校验、冲突面校验

### 说明

- DRM 层防标记依赖 `getrandom()`；API 26 头文件下该符号为弱引用软依赖，
  运行期若不可用会退化到 `/dev/urandom`
- 部分机型 Widevine 不以独立 init service 运行，此时 DRM 层不生效，
  UI 已就此说明
- 开启 DRM 层会重启一次 Widevine 服务，此刻正在播放的 Netflix/YouTube
  会短暂黑屏
- `lib/drmid_hook.so` 不随源码入库，由发布流水线用 NDK r27c 现场编译
- 终端二进制（goterm/tmux）仍需自行放入 bin/ 目录

---

## Xmark-Tools v2.8.0

### 本次更新

- 优化本次清理：补齐各游戏保留项与存储/user_de 处理；
  修正 CF（仅留 shared_prefs 并改走存储）、重写无畏契约五段保留；
  新增 PUBG 四服（国际/日韩/越南/台服）；游戏列表与监控名单同步新增 PUBG 四服
- 检测环境页新增检测对抗卡：露娜/牛头驱动残留优化、春秋对抗包、Hunter 策略优化
- 功能页新增：重置防火墙、恢复实时网络、SELinux 严格/宽容切换、
  一键关闭 USB 调试、电量与充电伪装
- 改标识优化：按包 SSAID 支持自定义输入（8-16 位字母数字，失败自动从备份恢复）

### 说明

- 本次新增功能均为一次性动作，无常驻开关
- 露娜/春秋名单剔除了系统目录与正常应用数据，只删作弊/检测工具专属残留
- 终端二进制（goterm/tmux）仍需自行放入 bin/ 目录

---

## Xmark-Tools v2.7.3

### 本次更新

- 修复硬编码版本号：系统信息页的模块版本改为从 module.prop 实时读取（此前固定显示 2.3）
- 优化伪装进程：监控进程改为从 stdin 执行脚本，命令行不再暴露脚本路径；
  同时写入 /proc/self/comm 伪装内核 comm（ps/top/status Name 不再显示 sh/模块路径）
- 移除遗留的 21120 端口后台执行服务：该服务以 root 监听所有网卡且无鉴权，
  当前 WebUI 已改由 exec_daemon 常驻守护接管，无调用方，直接删除以消除风险
- 修复常驻通知每 2 秒重复发送的问题：通知只在状态变化时发送
- 修复发布包携带陈旧 pid 状态文件的问题：pid/ 目录不再打包，安装时自动清空，
  避免 yuki.pid/touch_daemon.pid 等旧文件误杀无关进程
- 修复隐藏存储数据位置：隐藏目录从模块目录迁到 /data/adb/xinmaskplus/storage_hidden，
  模块更新/卸载不再丢数据；旧数据自动迁移，卸载时自动还原
- 修复信号增强空壳 APK 问题：校验文件有效后才挂载，无效占位文件不会损坏系统应用；
  WebUI 开关会提示需要自备真实 APK
- 修复终端组件缺失体验：bin/goterm、term_kill.sh 等未随包发布时明确提示缺失而不是超时
- 发布流水线：修正不存在的 v2.6.0 回退标签为 v2.6，新增发布包不得携带 pid/ 的校验

### 说明

- goterm/tmux 等终端二进制从未进入过公开仓库，需要自行放入模块 bin/ 目录后终端功能才可用
- 伪装进程已做静态核验（mksh exec 内建支持 -a 参数替换 argv[0]，Android /system/bin/sh 即 mksh），
  实际对抗效果需真机验证

---

## Xmark-Tools v2.7.2

### 紧急恢复：核对 v2.5 完整版

用户反馈 v2.7 缺少「伪装 CPU 型号」选项、部分开关仍会自动关、字体缺失。定位后找到根因：public git 仓库长期是**发布包的子集**，以前每次发布都是手工打包上传的，git 里并没有维护完整的 shipped 内容。之前几次 workflow 打包只是把 git 树塞进 ZIP，导致 v2.7 的 ZIP 比 v2.5 的实际 release 缺了以下东西：

**脚本（9 个，大部分被 commit 98d57b9 当死代码清掉了）：**
- scripts/cpu_spoof.sh / scripts/model_spoof.sh —— CPU 信息伪装真正的执行体
- scripts/exec_daemon.sh —— service.sh 需要拉起的常驻执行守护
- scripts/ptyrun.sh / scripts/detect.sh / scripts/diag.sh / scripts/env_audit.sh / scripts/hide.sh / scripts/extract_icons_v2.sh

**二进制：**
- bin/ksu_reload.sh、bin/rcq_xt.sh
- bin/yuki 大小写修正（git 里有大写 Yuki 但脚本引用的是小写 yuki）

**WebUI 资源**
- webroot/fonts/zhiyuan.ttf（19 MB，界面字体）—— @font-face 声明一直在 index.html 里，只是文件没打包进去
- webroot/donate_wx.jpg / webroot/donate_zfb.jpg / webroot/xny.jpg
- webroot/gg.sh —— 云端公告拉取（带 ghfast / gh-proxy 兜底代理）
- webroot/xz.sh / webroot/xz_worker.sh —— 云更新触发与后台执行

**WebUI 本体**
- webroot/index.html 恢复到 v2.5 的 3596 行完整版（v2.7 里只剩 1900 多行，自定义冻结 / 云更新入口 / 诊断按钮 / 捐赠面板等全被砍了）

### 保留 v2.6 / v2.7 / v2.7.1 的修复

- customize.sh v2.6 修复：不再在 sourced 上下文里 exit 0 提前终止安装器、不再覆盖 MODPATH，config/flag 全部写到 $MODPATH/config/，移除末尾 am start mqqapi://。修好安装到最后一步 Error code: 1。
- monitor_app.sh v2.7 修复：补齐 force_hide= / coloros_lock= 两个 key，rkp 检查改成 pid/rkp_done。修好 WebUI 部分开关点亮后视觉上自动关。
- service.sh v2.7 修复：PERSIST_FLAGS 扩展到含 coloros_lock / coloros_signal / force_hide / notify_always / auto_clean / soter。修好重启后大部分开关变关。
- v2.7.1 发布流水线：.github/workflows/release-module.yml 从 .github/RELEASE_NOTES.md 读 release body，末尾自动追加 SHA256 + 构建时间。

### 建议手动测试

- [ ] 进「功能」页应看到「伪装白名单CPU型号」下方有「自定义游戏伪装」按钮，点开可勾选要伪装的游戏
- [ ] 界面文字终于用致远体渲染（而不是系统默认字）
- [ ] 首次打开 WebUI 应自动弹出公告卡片（gg.sh 走代理拉取）
- [ ] 首页应有「云更新到最新版」入口，点击后走 xz.sh 自动下载最新 release
- [ ] 强制开启隐藏 / 解除 ColorOS 云控锁帧 / 一键修复 RKP 3 个开关点亮后 3 秒不应变灭
- [ ] 重启一次，SOTER / 常驻通知 / 自动清理等开关应保持上次设置
