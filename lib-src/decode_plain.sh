#!/usr/bin/env bash
# 从 base64 封装脚本还原明文副本到 lib-src/*.plain
# 用途：让源码可读、可 review、可被 selfcheck.sh 检查。
# 封装格式（与项目现有 scripts/*.sh 完全一致）：
#   1: #!/system/bin/sh
#   2: eval "$(base64 -d << 'B64END'
#   3: <单行 base64>
#   4: B64END
#   5: )
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

decode() { sed -n '3p' "$1" | tr -d '\r\n' | base64 -d; }

gen() {
    local src="$1" dst="$2"
    decode "$src" > "$dst"
    printf '  %-32s -> %s (%s 行)\n' "$src" "$dst" "$(wc -l < "$dst")"
}

echo "还原明文副本:"
gen scripts/drm_hook_ctl.sh lib-src/drm_hook_ctl.sh.plain
gen service.sh              lib-src/service.sh.plain
gen scripts/monitor_app.sh  lib-src/monitor_app.sh.plain

# 一致性断言：明文必须能通过 sh -n
for f in lib-src/*.plain; do
    sh -n "$f"
    echo "  [OK] $f 语法有效"
done
