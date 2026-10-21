#!/system/bin/sh
GH_USER="kusesad-1122"
GH_REPO="Xmark-Tools"
RAW="https://raw.githubusercontent.com/$GH_USER/$GH_REPO/main/notice.txt"
PROXIES="https://ghfast.top/ https://gh-proxy.com/ https://ghproxy.net/ EMPTY"
UA="Mozilla/5.0 (Linux; Android 13; K) Mobile Safari/537.36"

for P in $PROXIES; do
  [ "$P" = "EMPTY" ] && URL="$RAW" || URL="$P$RAW"
  OUT=$(curl -4 -L -k -s -A "$UA" --connect-timeout 8 --max-time 15 "$URL?t=$(date +%s)")
  if [ -n "$OUT" ] && ! echo "$OUT" | grep -qiE "404|Not Found"; then
    echo "$OUT" | base64 | tr -d '\n\r'
    exit 0
  fi
done
echo "NOTICE_FAIL"
exit 1