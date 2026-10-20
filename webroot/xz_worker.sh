#!/system/bin/sh
MOD="/data/adb/modules/xinmaskplus"
mkdir -p /data/adb/xinmaskplus/run 2>/dev/null
STATUS="/data/adb/xinmaskplus/run/update.status"
LOCK="/data/adb/xinmaskplus/run/update.lock"
ZIP="/data/adb/xinmaskplus/run/update.zip"
EXDIR="/data/adb/xinmaskplus/run/update_ex"
GH_USER="kusesad-1122"; GH_REPO="Xmark-Tools"; ZIP_NAME="Xmark-Tools.zip"
TARGET="https://github.com/$GH_USER/$GH_REPO/releases/latest/download/$ZIP_NAME"
PROXIES="https://ghfast.top/ https://gh-proxy.com/ EMPTY"
UA="Mozilla/5.0 (Linux; Android 13; K) Mobile Safari/537.36"

: > "$LOCK"
finish(){ echo "$1" > "$STATUS"; rm -f "$LOCK"; exit 0; }

ping -c 1 -W 2 223.5.5.5 >/dev/null 2>&1 || finish "FAILED:网络无法连接"

echo "DOWNLOADING" > "$STATUS"
rm -f "$ZIP"; OK=""
for P in $PROXIES; do
  [ "$P" = "EMPTY" ] && URL="$TARGET" || URL="$P$TARGET"
  curl -4 -L -k -s -A "$UA" --connect-timeout 15 --max-time 300 --retry 2 -o "$ZIP" "$URL?t=$(date +%s)"
  if [ -s "$ZIP" ] && [ "$(head -c 2 "$ZIP")" = "PK" ]; then OK=1; break; fi
  rm -f "$ZIP"
done
[ -z "$OK" ] && finish "FAILED:下载失败(代理可能失效)"

echo "INSTALLING" > "$STATUS"
rm -rf "$EXDIR"; mkdir -p "$EXDIR"
unzip -o -q "$ZIP" -d "$EXDIR" || { rm -rf "$EXDIR" "$ZIP"; finish "FAILED:解压失败"; }

if [ ! -f "$EXDIR/module.prop" ] || ! grep -q "^id=xinmaskplus" "$EXDIR/module.prop"; then
  rm -rf "$EXDIR" "$ZIP"; finish "FAILED:压缩包不是本模块"
fi

# 保留用户配置/选择：config、pid、自定义背景
rm -rf "$EXDIR/config" "$EXDIR/pid"
rm -f "$EXDIR/webroot/background.jpg"

# .so 先删后拷，避免 text busy
# v2.9.9: 终端功能已移除 —— 清理旧版本的 goterm/tmux/xterm 组件
rm -f "$MOD/bin/goterm" "$MOD/bin/tmux" "$MOD"/bin/*.so* 2>/dev/null
rm -f "$MOD/webroot/xterm.css" "$MOD/webroot/xterm.js" "$MOD/webroot/xterm-addon-fit.js" 2>/dev/null
rm -f "$MOD/scripts/ptyrun.sh" "$MOD/scripts/tmrun.sh" "$MOD/scripts/term_kill.sh" 2>/dev/null
rm -f /data/adb/xinmaskplus/log/goterm.log 2>/dev/null
rm -rf /data/adb/xinmaskplus/run/tmux_chk 2>/dev/null
rm -f "$MOD/zygisk/arm64-v8a.so" 2>/dev/null
cp -rf "$EXDIR/." "$MOD/" 2>/dev/null

chmod -R 0755 "$MOD/bin" "$MOD/scripts" "$MOD/webroot" 2>/dev/null
find "$MOD" -name "*.sh" -exec chmod 0755 {} \; 2>/dev/null
[ -f "$MOD/bin/yuki" ] && chmod 0755 "$MOD/bin/yuki"
chmod 0644 "$MOD/module.prop" 2>/dev/null

rm -rf "$EXDIR" "$ZIP"
NEWVER=$(grep '^version=' "$MOD/module.prop" 2>/dev/null | cut -d= -f2)
finish "DONE:$NEWVER"
