#!/system/bin/sh
MOD="/data/adb/modules/xinmaskplus"
STATUS="/data/adb/xinmaskplus/run/update.status"
LOCK="/data/adb/xinmaskplus/run/update.lock"
[ -f "$LOCK" ] && { echo "RUNNING"; exit 0; }
mkdir -p /data/adb/xinmaskplus/run 2>/dev/null
echo "START" > "$STATUS"

# v2.9.9: worker 经 tmpfs staging 拉起 —— 更新流程可持续数分钟，
# 明文模块路径的 cmdline 会长期挂在 ps 里；暂存内容同样 base64 内嵌
BG_SALT="/data/adb/xinmask_device_id"
[ -f "$BG_SALT" ] || { dd if=/dev/urandom bs=16 count=1 2>/dev/null | sha256sum | cut -c1-32 > "$BG_SALT" 2>/dev/null; }
BGD="/dev/.b$(sha256sum "$BG_SALT" 2>/dev/null | cut -c1-8)"
[ "${#BGD}" -ge 8 ] || BGD="/dev/.bgd"
mkdir -p "$BGD" 2>/dev/null; chmod 700 "$BGD" 2>/dev/null
_B64SRC="$MOD/webroot/xz_worker.sh"
_b64=$(base64 "$_B64SRC" 2>/dev/null)
_b64=$(echo "$_b64" | tr -d '\n')
if [ -n "$_b64" ]; then
    {
        echo '#!/system/bin/sh'
        echo "eval \"\$(printf '%s' '$_b64' | base64 -d)\""
    } > "$BGD/updater.sh" 2>/dev/null
    chmod 700 "$BGD/updater.sh" 2>/dev/null
    "$BGD/updater.sh" </dev/null >/dev/null 2>&1 &
else
    setsid sh "$MOD/webroot/xz_worker.sh" >/dev/null 2>&1 &
fi
echo "STARTED"
exit 0
