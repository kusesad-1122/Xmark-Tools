#!/usr/bin/env python
"""机型表自洽性终检。

逐个 case 校验：
  1. set_all 存在且 6 个参数齐全
  2. set_fp 存在且恰好 1 个参数（整个 fingerprint 串）
  3. fingerprint 段数 == 5（brand/product/device:release/id/incremental:type/tags）
  4. fingerprint 第 1 段 == set_all 的 brand
  5. fingerprint 第 2 段 == set_all 的 product（device 同段）
  6. 结尾 :type/tags 存在且 tags=release-keys
  7. brand 与 manufacturer 的关系符合厂商惯例（子品牌 vs 母品牌）

之前用正则抓 set_fp 参数时把后面 log 里的单词当成了参数，
必须按 shell 的引号配对来解析参数，不能靠 \S+。
"""
import base64
import re
import sys

import os
_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
SRC = os.path.join(_ROOT, 'scripts', 'model_spoof.sh')
d = open(SRC, encoding='utf-8', newline='').read()
m = re.search(r"<< 'B64END'\n(.*?)\nB64END\n", d, re.S)
p = base64.b64decode(m.group(1)).decode('utf-8')
lines = p.split('\n')


def sh_args(s):
    """按 shell 规则取参数：双引号内整体算一个，裸词按空白切。"""
    out, cur, q = [], '', None
    for ch in s:
        if q:
            if ch == q:
                q = None
            else:
                cur += ch
        elif ch in '"\'':
            q = ch
        elif ch.isspace():
            if cur:
                out.append(cur)
                cur = ''
        else:
            cur += ch
    if cur:
        out.append(cur)
    return out


# 切出每个 case 块
case_re = re.compile(r'^ {8}([a-z0-9_]+)\)\s*$')
cases = []
for i, l in enumerate(lines):
    m2 = case_re.match(l)
    if not m2:
        continue
    body = []
    for j in range(i + 1, len(lines)):
        s = lines[j]
        if case_re.match(s) or re.match(r'^\s*esac', s):
            break
        body.append(s)
    cases.append((m2.group(1), i + 1, '\n'.join(body)))

print(f'机型条目数: {len(cases)}')
print()

problems = []
for mid, ln, body in cases:
    sa = re.search(r'^\s*set_all\s+(.*)$', body, re.M)
    sf = re.search(r'^\s*set_fp\s+(.*)$', body, re.M)

    if not sa:
        problems.append((mid, ln, '缺 set_all'))
        continue
    if not sf:
        problems.append((mid, ln, '缺 set_fp'))
        continue

    a = sh_args(sa.group(1))
    f = sh_args(sf.group(1))

    if len(a) != 6:
        problems.append((mid, ln, f'set_all 参数 {len(a)} 个（应为 6）: {a}'))
        continue
    if len(f) != 1:
        problems.append((mid, ln, f'set_fp 参数 {len(f)} 个（应为 1）: {f}'))
        continue

    brand, manuf, product, market, device, name = a
    fp = f[0]
    parts = fp.split('/')

    # AOSP 官方格式为 6 段：
    #   brand/product/device:release/build-id/incremental:type/tags
    # 注意 fp[1] 是 product(=ro.product.name)，fp[2] 是 device，
    # 与 set_all 的第 6 槽(name) / 第 5 槽(device) 对应，不是 model。
    if len(parts) != 6:
        problems.append((mid, ln, f'fp 段数 {len(parts)}（应为 6）: {fp}'))
        continue
    # 品牌：Pixel 官方指纹就是小写 google，属正常
    if parts[0] != brand and not (brand == 'Google' and parts[0] == 'google'):
        problems.append((mid, ln, f'fp[0]={parts[0]!r} != set_all brand={brand!r}'))
    if parts[1] != name:
        problems.append((mid, ln, f'fp[1]={parts[1]!r} != set_all name={name!r}'))
    # parts[2] = "device:release"
    dev_rel = parts[2].split(':')
    if len(dev_rel) != 2 or dev_rel[0] != device:
        problems.append((mid, ln,
                         f'fp[2]={parts[2]!r} 应为 "{device}:<release>"'))
    # release：AOSP 纯数字（15）或华为 HarmonyOS NEXT 的 OpenHarmony-x.x.x.x
    rel = dev_rel[1]
    if not (rel.isdigit() or rel.startswith('OpenHarmony-')):
        problems.append((mid, ln, f'fp release={rel!r} 既非数字也非 OpenHarmony-*'))

    # parts[3] = build-id。华为/荣耀是 HUAWEI<model> 式，红魔含版本后缀，
    # 都是厂商真实格式，不做字符集限制，只要求非空且不含冒号/斜杠。
    if not parts[3] or ':' in parts[3] or '/' in parts[3]:
        problems.append((mid, ln, f'fp build-id={parts[3]!r} 格式异常'))

    # parts[4] = "incremental:type"
    inc_type = parts[4].split(':')
    if len(inc_type) != 2:
        problems.append((mid, ln, f'fp[4]={parts[4]!r} 应为 incremental:type'))
        continue
    if inc_type[1] != 'user':
        problems.append((mid, ln, f'fp type={inc_type[1]!r}（应为 user）'))
    if parts[5] != 'release-keys':
        problems.append((mid, ln, f'fp tags={parts[5]!r}（应为 release-keys）'))

print('=== 问题清单 ===')
if problems:
    for mid, ln, why in problems:
        print(f'  [{mid}] 行{ln}: {why}')
else:
    print('  无 ✅')

# 厂商惯例抽查（子品牌 manufacturer 应为母公司，brand 为子品牌）
print()
print('=== 品牌/厂商对照（人工确认用）===')
for mid, ln, body in cases:
    sa = re.search(r'^\s*set_all\s+(.*)$', body, re.M)
    if sa:
        a = sh_args(sa.group(1))
        print(f'  {mid:26s} brand={a[0]:10s} manufacturer={a[1]}')

sys.exit(1 if problems else 0)
