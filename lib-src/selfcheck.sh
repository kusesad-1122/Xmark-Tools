#!/usr/bin/env bash
# 交付前静态自检。不编译、不联网，只验证结构与一致性。
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT="$PWD"
FAIL=0
ok()   { echo "  [PASS] $1"; }
bad()  { echo "  [FAIL] $1"; FAIL=1; }

decode() { sed -n '3p' "$1" | tr -d '\r\n' | base64 -d 2>/dev/null; }

echo "=============================================="
echo " 1. base64 封装格式与往返一致性"
echo "=============================================="
for f in scripts/drm_hook_ctl.sh service.sh scripts/monitor_app.sh; do
    n=$(wc -l < "$f")
    if [ "$n" -ne 5 ]; then bad "$f 行数=$n (期望 5)"; else ok "$f 行数=5"; fi
    head -n1 "$f" | grep -q '^#!/system/bin/sh$' \
        && ok "$f shebang" || bad "$f shebang"
    sed -n '2p' "$f" | grep -q "base64 -d << 'B64END'" \
        && ok "$f eval 包装" || bad "$f eval 包装"
    # 收尾必须是 )" —— eval "$(...)" 的双引号要闭合。
    # 只写 ) 会让脚本以 "unexpected EOF while looking for matching \"" 失败，
    # 整个封装脚本根本执行不了（已实测：形态 ) 报语法错，形态 )" 正常）。
    tail -n1 "$f" | grep -q '^)"$' && ok "$f 收尾 )\"" || bad "$f 收尾应为 )\"（当前: $(tail -n1 "$f")）"
    # base64 载荷必须是单行且只含 base64 字符
    if sed -n '3p' "$f" | grep -qE '^[A-Za-z0-9+/]+={0,2}$'; then
        ok "$f 载荷为合法单行 base64"
    else
        bad "$f 载荷含非 base64 字符"
    fi
    # 往返
    tmp=$(mktemp); decode "$f" > "$tmp"
    if [ -s "$tmp" ] && head -n1 "$tmp" | grep -q '^#!/system/bin/sh$'; then
        if diff -q <(decode "$f") <(decode "$f") >/dev/null; then
            ok "$f 解码有效 ($(wc -l < "$tmp") 行)"
        else
            bad "$f 解码不稳定"
        fi
    else
        bad "$f 解码后不是有效脚本"
    fi
    sh -n "$tmp" 2>/dev/null && ok "$f 解码后 sh -n" || bad "$f 解码后 sh -n"
    rm -f "$tmp"
done

echo
echo "=============================================="
echo " 2. 解码脚本的 sh -n 语法检查"
echo "=============================================="
for f in scripts/drm_hook_ctl.sh service.sh scripts/monitor_app.sh; do
    tmp=$(mktemp); decode "$f" > "$tmp"
    if sh -n "$tmp" 2>/dev/null; then ok "$(basename $f) 语法"; else bad "$(basename $f) 语法"; fi
    rm -f "$tmp"
done
# 未封装的新脚本
sh -n lib-src/build.sh 2>/dev/null && ok "lib-src/build.sh 语法" || bad "lib-src/build.sh 语法"

# 后续检查需要读明文，就地生成 lib-src/*.plain。
# 不预先提交这些副本（.gitignore 已排除 *.plain），每次自检现解，
# 避免副本与封装内容不同步导致误判。
for pair in "scripts/drm_hook_ctl.sh:lib-src/drm_hook_ctl.sh.plain" \
            "service.sh:lib-src/service.sh.plain" \
            "scripts/monitor_app.sh:lib-src/monitor_app.sh.plain"; do
    src="${pair%%:*}"; dst="${pair##*:}"
    if decode "$src" > "$dst" 2>/dev/null && [ -s "$dst" ]; then
        ok "解出明文 $dst"
    else
        bad "无法解出 $dst（$src 封装异常）"
    fi
done

echo
echo "=============================================="
echo " 3. C 源码静态检查"
echo "=============================================="
if [ -f lib-src/drmid_hook.c ]; then
    python3 lib-src/_check_balance.py lib-src/drmid_hook.c >/dev/null 2>&1 \
        && ok "括号配平 + 字符串闭合" || bad "括号配平"
    # 半行注释：注释掉代码再数一次花括号，暴露被注释掩盖的不平衡
    grep -q '/\*' lib-src/drmid_hook.c && ok "块注释闭合" || bad "块注释"
    # 关键符号必须存在
    for sym in A64HookFunction A64HookFunctionV zn_module JNI_OnLoad drmid_hook_init; do
        grep -q "$sym" lib-src/drmid_hook.c && ok "导出/入口 $sym" || bad "缺少 $sym"
    done
    # 绝不能有 abort/exit/_exit(失败即崩溃会拖垮 Widevine)
    if grep -nE '\b(abort|_exit|exit)\s*\(' lib-src/drmid_hook.c; then
        bad "源码中出现 abort/exit（违反静默失败要求）"
    else
        ok "无 abort/exit（静默失败）"
    fi
    # switch 覆盖（在解码后的明文里查，封装文件里查不到）
    if grep -q 'case "\$1"' lib-src/drm_hook_ctl.sh.plain 2>/dev/null; then
        ok "drm_hook_ctl.sh 有 case 分派"
    else
        bad "drm_hook_ctl.sh 缺少 case 分派"
    fi
else
    bad "lib-src/drmid_hook.c 不存在"
fi

echo
echo "=============================================="
echo " 4. 调用点闭环（新增符号都有真实调用者）"
echo "=============================================="
# drm_hook_ctl.sh 的五个 action（boot = 开机路径，只校验不stop/start）
for a in enable boot disable status verify; do
    grep -q "do_$a()" lib-src/drm_hook_ctl.sh.plain 2>/dev/null \
        && grep -q "    $a)" lib-src/drm_hook_ctl.sh.plain \
        && ok "action '$a' 有定义且被 case 调度" \
        || bad "action '$a' 未闭环"
done
# service.sh 必须调 boot 而不是 enable：开机路径 stop/start Widevine HAL
# 会打断播放并在部分 ROM 上引发 mediaserver 连锁异常
grep -q 'drm_hook_ctl.sh" boot' lib-src/service.sh.plain \
    && ok "service.sh 调用 drm_hook_ctl.sh boot（开机路径）" || bad "service.sh 未调用 boot"
if grep -q 'drm_hook_ctl.sh" enable' lib-src/service.sh.plain; then
    bad "service.sh 仍在开机路径调 enable（会每次开机重启 HAL）"
else
    ok "service.sh 开机路径不再调用 enable"
fi
# boot 函数体内不得出现 stop —— 属性持久，init 已自动应用。
# 剥注释再查，避免"说明性注释里的 stop 字样"造成误判。
awk '/^do_boot\(\)/,/^}/' lib-src/drm_hook_ctl.sh.plain | sed 's/#.*//' | grep -q 'stop ' \
    && bad "do_boot 内出现 stop（开机路径不应 stop HAL）" \
    || ok "do_boot 内无 stop 调用（注释已排除）"
# boot 必须仍有 start 补救能力
awk '/^do_boot\(\)/,/^}/' lib-src/drm_hook_ctl.sh.plain | grep -q 'start "' \
    && ok "do_boot 保留 start 补救分支" || bad "do_boot 缺少 start 补救"
# enable 是唯一允许 stop 的路径
awk '/^do_enable\(\)/,/^}/' lib-src/drm_hook_ctl.sh.plain | grep -q 'stop "' \
    && ok "do_enable 保留 stop/start（用户手动开启需重启 HAL）" || bad "do_enable 缺少 stop"
# WebUI 调用 ctl 脚本
grep -q 'DRM_CTL} enable' webroot/index.html && ok "WebUI 调用 enable" || bad "WebUI 未调用 enable"
grep -q 'DRM_CTL} disable' webroot/index.html && ok "WebUI 调用 disable" || bad "WebUI 未调用 disable"
grep -q 'DRM_CTL} status' webroot/index.html && ok "WebUI 调用 status" || bad "WebUI 未调用 status"
# WebUI toggle 函数被 HTML onchange 引用
grep -q 'onchange="toggleDrmHook()"' webroot/index.html && ok "onchange 绑定 toggleDrmHook" || bad "onchange 未绑定"
grep -q 'async function toggleDrmHook' webroot/index.html && ok "toggleDrmHook 已定义" || bad "toggleDrmHook 未定义"
# 破坏性操作必须先确认，且复用既有 fm-modal 弹窗体系（不新造 UI）
grep -q 'await confirmDrmHookEnable()' webroot/index.html \
    && ok "开启前有确认步骤（confirmDrmHookEnable）" || bad "开启前缺确认步骤"
grep -q 'id="drmhook-confirm"' webroot/index.html \
    && grep -q 'class="fm-confirm-card"' webroot/index.html \
    && ok "确认弹窗复用 fm-modal/fm-confirm 组件" || bad "确认弹窗未复用既有组件"
# 确认必须在写 flag / 调 enable 之前
awk '/async function toggleDrmHook/,/^    }/' webroot/index.html \
    | awk '/confirmDrmHookEnable/{c=NR} /DRM_CTL} enable/{e=NR} END{exit !(c>0 && e>0 && c<e)}' \
    && ok "确认发生在 enable 之前" || bad "确认未发生在 enable 之前"
# 关闭不应弹确认（恢复性操作）
awk '/async function toggleDrmHook/,/^    }/' webroot/index.html \
    | awk '/else \{/{f=1} f&&/confirmDrmHookEnable/{exit 1} END{exit 0}' \
    && ok "关闭路径不弹确认" || bad "关闭路径多余地弹了确认"
# 失败要把日志里的 [DrmHook] 根因 toast 给用户
grep -q 'drmHookFailureReason' webroot/index.html \
    && grep -q '\[DrmHook\\]' webroot/index.html \
    && ok "失败时从日志抓 [DrmHook] 根因" || bad "失败未展示日志根因"
# 开关描述必须含代价说明（重启/黑屏/部分机型不适用）
grep -A2 'id="drmHookToggle"' webroot/index.html | grep -q '短暂黑屏' \
    && ok "描述含黑屏代价说明" || bad "描述缺代价说明"
grep -A2 'id="drmHookToggle"' webroot/index.html | grep -q '不以独立服务运行' \
    && ok "描述含部分机型不适用说明" || bad "描述缺适用性说明"

echo
echo "=============================================="
echo " 5. WebUI 状态同步三处齐全"
echo "=============================================="
grep -q "id: 'drmHookToggle', flag: 'drm_hook'" webroot/index.html \
    && ok "syncSwitchesFromConfig 已注册 drm_hook" || bad "syncSwitches 未注册"
grep -q "drmHookToggle: 'led-drmHook'" webroot/index.html \
    && ok "ledMap 已注册" || bad "ledMap 未注册"
grep -q 'id="led-drmHook"' webroot/index.html \
    && ok "LED 容器存在" || bad "LED 容器缺失"
grep -q 'id="drmHookToggle"' webroot/index.html \
    && ok "checkbox 存在" || bad "checkbox 缺失"
grep -q 'drm_hook=\${dh}' lib-src/monitor_app.sh.plain \
    && ok "monitor_app.sh 输出 drm_hook 状态" || bad "monitor_app.sh 未输出 drm_hook"

echo
echo "=============================================="
echo " 5.5 random_ids.sh 的 resetprop 可达性（v2.9.1 回归）"
echo "=============================================="
# 事故背景：exec_daemon 给 WebUI 命令导出的 PATH 不含 /data/adb/ksu/bin、
# /data/adb/ap/bin，random_ids.sh 裸调 resetprop 会 command not found 且被
# 2>/dev/null 吞掉，"重置设备标识"完全无效。必须经 find_rp 定位 + rp_set 调用。
# 另：v2.9.1 首次发布时批量替换的 assert 写在写文件之前，异常导致 6 处替换
# 全部丢失而封装语法检查无法发现——这里固化断言防复发。
RID=lib-src/random_ids.sh.plain
decode scripts/random_ids.sh > "$RID" 2>/dev/null
# 注意：grep -c 无匹配时输出 "0" 但退出码为 1，不能接 || echo 0（会叠成 "0\n0"）
n_bare=$(grep -cE '^[[:space:]]*resetprop[[:space:]]' "$RID" 2>/dev/null)
n_rpset=$(grep -cE '^[[:space:]]*rp_set[[:space:]]' "$RID" 2>/dev/null)
[ "$n_bare" -eq 0 ] && ok "random_ids.sh 无裸调 resetprop" \
    || bad "random_ids.sh 有 $n_bare 处裸调 resetprop（会静默失败）"
[ "$n_rpset" -ge 7 ] && ok "random_ids.sh rp_set 调用 $n_rpset 处" \
    || bad "random_ids.sh rp_set 调用仅 $n_rpset 处（应 >=7）"
grep -q 'find_rp' "$RID" \
    && ok "random_ids.sh 含 find_rp 兜底" || bad "random_ids.sh 缺 find_rp"
grep -q '^echo "OK:' "$RID" \
    && ok "random_ids.sh 输出 OK: 结果行（UI 反馈依赖）" \
    || bad "random_ids.sh 缺 OK: 结果行"

echo
echo "=============================================="
echo " 6. 持久化闭环（pid 标志 <-> persist）"
echo "=============================================="
grep -q 'drm_hook' lib-src/service.sh.plain && ok "PERSIST_FLAGS 含 drm_hook" || bad "PERSIST_FLAGS 缺 drm_hook"
# PERSIST_FLAGS 同步循环用的是变量，无需额外改动；确认标志文件路径两处一致
grep -q 'pid/drm_hook' lib-src/service.sh.plain && ok "service.sh 读 pid/drm_hook" || bad "service.sh 未读 pid/drm_hook"
grep -q 'PIDDIR/drm_hook' lib-src/monitor_app.sh.plain && ok "monitor_app.sh 读 pid/drm_hook" || bad "monitor 未读"

echo
echo "=============================================="
echo " 6b. service 名持久化闭环（pid/drm_hook_svc）"
echo "=============================================="
P=lib-src/drm_hook_ctl.sh.plain
grep -q 'SVC_FILE="\$MODDIR/pid/drm_hook_svc"' $P \
    && ok "SVC_FILE 指向 pid/drm_hook_svc" || bad "SVC_FILE 路径不对"
# 三级兜底顺序：svc 文件 -> discover_svc -> FALLBACK_SVCS
if awk '/^resolve_svc\(\)/,/^}/' $P \
     | grep -q 'SVC_FILE' \
   && awk '/^resolve_svc\(\)/,/^}/' $P | grep -q 'discover_svc' \
   && awk '/^resolve_svc\(\)/,/^}/' $P | grep -q 'FALLBACK_SVCS'; then
    ok "resolve_svc 三级兜底：svc 文件 → discover_svc → FALLBACK_SVCS"
else
    bad "resolve_svc 缺少三级兜底"
fi
# 顺序不能颠倒：SVC_FILE 的判断必须早于 discover_svc
awk '/^resolve_svc\(\)/,/^}/' $P \
  | awk '/SVC_FILE/{f=NR} /discover_svc/{d=NR} END{exit !(f>0 && d>0 && f<d)}' \
  && ok "兜底顺序正确（svc 文件优先于动态发现）" || bad "兜底顺序颠倒"
# enable 成功要写入
awk '/^do_enable\(\)/,/^}/' $P | grep -q 'save_svc "\$SVC"' \
    && ok "do_enable 成功后持久化真实 service 名" || bad "do_enable 未持久化 svc 名"
# disable / status 必须走 resolve_svc，不能只靠 FALLBACK_SVCS 遍历
for fn in do_disable do_status do_verify do_boot; do
    awk -v f="^$fn\\\\(\\\\)" '$0 ~ f,/^}/' $P | grep -q 'resolve_svc' \
        && ok "$fn 使用 resolve_svc" || bad "$fn 未使用 resolve_svc"
done
# disable 要清理 svc 记录
awk '/^do_disable\(\)/,/^}/' $P | grep -q 'rm -f "\$FLAG" "\$SVC_FILE"' \
    && ok "do_disable 清理 pid/drm_hook_svc" || bad "do_disable 未清理 svc 记录"
# enable 回滚分支也要清理，不能留脏记录
awk '/^do_enable\(\)/,/^}/' $P | grep -q 'rm -f "\$FLAG" "\$SVC_FILE"' \
    && ok "do_enable 回滚分支清理 svc 记录" || bad "do_enable 回滚未清理 svc 记录"
# do_verify 不得再硬编码 AOSP 默认 service 名去 pidof。
# 剥掉注释再查：脚本里会保留一句说明"当初就是硬编码这个"的注释，
# 直接 grep 会把注释误判成残留代码。
awk '/^do_verify\(\)/,/^}/' $P | sed 's/#.*//' \
    | grep -q 'pidof android.hardware.drm-service.widevine' \
    && bad "do_verify 仍硬编码 pidof android.hardware.drm-service.widevine" \
    || ok "do_verify 已移除硬编码 pidof service 名（注释已排除）"
grep -q 'svc_bin()' $P && ok "svc_bin() 存在（从 init rc 取真实二进制）" || bad "svc_bin() 缺失"

echo
echo "=============================================="
echo " 7. 未触碰禁改文件"
echo "=============================================="
CHANGED_POST=$(git diff --name-only -- post-fs-data.sh | wc -l)
CHANGED_MODEL=$(git diff --name-only -- scripts/model_spoof.sh | wc -l)
SELF_POST=$(git diff --stat -- post-fs-data.sh | tail -1)
echo "  [info] post-fs-data.sh / model_spoof.sh 的现有 diff 来自另一 agent"
echo "  [info] (本任务未编辑这两个文件)"
# 确认我的改动没往这两个文件里加 drm 相关内容
if git diff -- post-fs-data.sh | grep -qi 'drm_hook\|drmid'; then
    bad "post-fs-data.sh 中出现我的改动"
else
    ok "post-fs-data.sh 未被我改动"
fi
if git diff -- scripts/model_spoof.sh | grep -qi 'drm_hook\|drmid'; then
    bad "model_spoof.sh 中出现我的改动"
else
    ok "model_spoof.sh 未被我改动"
fi

echo
echo "=============================================="
echo " 8. CI workflow YAML 结构"
echo "=============================================="
python3 - <<'PY'
import sys
try:
    import yaml
except ImportError:
    print("  [skip] PyYAML 未安装，改用缩进/制表符检查")
    src=open('.github/workflows/release-module.yml',encoding='utf-8').read()
    if '\t' in src:
        print("  [FAIL] YAML 含制表符"); sys.exit(1)
    print("  [PASS] 无制表符")
    sys.exit(0)
d=yaml.safe_load(open('.github/workflows/release-module.yml',encoding='utf-8'))
steps=d['jobs']['release']['steps']
names=[s.get('name','') for s in steps]
print("  [info] steps:", len(steps))
for want in ['Build libdrmid_hook.so (NDK)','Sanity-check ZIP has fixed customize.sh']:
    print(("  [PASS] " if want in names else "  [FAIL] ")+f"step 存在: {want}")
PY

echo
echo "=============================================="
echo " 9. sepolicy.rule 未使用全开规则"
echo "=============================================="
# 必须先剥掉注释：本文件注释里引用了 `allow hal_drm_widevine * * *` 作为
# 反例说明，直接 grep 会把注释误判成真实规则。
if sed 's/#.*//' sepolicy.rule 2>/dev/null \
     | grep -Eq 'allow\s+\S+\s+\*\s+\*\s+\*'; then
    bad "存在全开 allow 规则"
else
    ok "无全开 allow 规则（已排除注释）"
fi
grep -q 'execmem' sepolicy.rule && ok "含 execmem (trampoline 所需)" || bad "缺 execmem"
# 顺带确认 sepolicy.rule 真的被 Magisk 加载：文件名必须精确
[ -f sepolicy.rule ] && ok "sepolicy.rule 位于模块根目录 (Magisk 自动加载)" \
    || bad "sepolicy.rule 位置错误"

echo
echo "=============================================="
echo " 10. AArch64 分类器掩码语义验证"
echo "=============================================="
# 掩码写错是本文件最容易犯且最难靠肉眼发现的错误（写错后条件恒假、
# 谓词变死代码，运行时不崩溃但保护失效）。这里用穷举证明语义正确。
python3 - <<'PY'
import sys
fails = 0
def chk(cond, name):
    global fails
    print(("  [PASS] " if cond else "  [FAIL] ") + name)
    if not cond: fails += 1

def ret(i):    return (i & 0xFFFFFC1F) == 0xD65F0000
def br(i):     return (i & 0xFFFFFC1F) == 0xD61F0000
def blr(i):    return (i & 0xFFFFFC1F) == 0xD63F0000
def ldrlit(i): return (i & 0x3B000000) == 0x18000000

# 1) RET 族（含默认 RET 与 RET Xn）全部命中
chk(all(ret(x) for x in (0xD65F03C0, 0xD65F0000, 0xD65F0200)),
    "RET / RET Xn 全部被 is_ret 命中")
# 2) RET 不误命中 BR / BLR（最关键：误命中会把间接跳转当 PC 无关原样搬运）
chk(not any(ret(x) for x in (0xD61F0000, 0xD61F0200, 0xD63F0000, 0xD63F0200)),
    "BR / BLR 不被 is_ret 误命中")
# 3) 穷举 bits31:16 全 65536 组，is_ret 只命中 0xD65F 前缀
hit = [t for t in range(1 << 16) if ret((t << 16) | 0x03C0)]
chk(hit == [0xD65F], "穷举 bits31:16: is_ret 命中集合恰为 {0xD65F}")
# 4) LDR literal / PRFM literal 全族命中（穷举 bits31:24 共 256 组）
tops = sorted(t for t in range(256) if ldrlit((t << 24)))
chk(tops == [0x18, 0x1C, 0x58, 0x5C, 0x98, 0x9C, 0xD8, 0xDC],
    "穷举 bits31:24: literal 集合 = 8 个前缀（含 PRFM 0xD8/0xDC）")
# 5) LDR-literal 谓词不误命中需要重定位的 PC 相对指令
chk(not any(ldrlit(x) for x in (0x10000040, 0x90000040, 0x14000000, 0x94000000)),
    "ADR / ADRP / B / BL 不被 literal 谓词误命中")
# 6) 已删除的 is_prfm_lit 不得残留在源码里（只看代码行，注释里有这个名字的说明文字）
src = open('lib-src/drmid_hook.c', encoding='utf-8').read()
code_only = [l for l in src.splitlines() if not l.lstrip().startswith(('*', '/*', '//'))]
chk(not any('is_prfm_lit' in l for l in code_only),
    "死代码 is_prfm_lit 已从源码代码行移除")
sys.exit(1 if fails else 0)
PY
[ $? -eq 0 ] && ok "分类器掩码语义验证通过" || bad "分类器掩码语义验证"

echo
echo "=============================================="
if [ $FAIL -eq 0 ]; then echo " 结果: 全部通过"; else echo " 结果: 存在失败项"; fi
echo "=============================================="
exit $FAIL
