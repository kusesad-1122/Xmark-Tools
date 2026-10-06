#!/usr/bin/env python
"""冲突面分析：新增的 DRM 层与既有模块是否互相踩脚。

关注三件事：
  1. 属性写入交集 —— 两个脚本写同一个 prop，后写的会覆盖先写的
  2. service 操作对象 —— DRM 层 stop/start 的是谁，会不会误伤别的功能
  3. do_boot 路径是否含 stop —— 开机时打断 DRM 会导致 Netflix 黑屏
"""
import base64
import os
import re

_HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.dirname(_HERE) + os.sep


def dec(p):
    d = open(p, encoding='utf-8', newline='').read()
    m = re.search(r"<< 'B64END'\n(.*?)\nB64END\n", d, re.S)
    return base64.b64decode(m.group(1)).decode('utf-8') if m else d


post = dec(R + 'post-fs-data.sh')
svc = dec(R + 'service.sh')
ctl = dec(R + 'scripts/drm_hook_ctl.sh')
msp = dec(R + 'scripts/model_spoof.sh')

PAT = re.compile(r'(?:setprop|resetprop|\$RP)\s+([a-zA-Z0-9._]+)')

print('=== 1. 各脚本写入的属性 ===')
allp = {}
for name, txt in [('post-fs-data.sh', post), ('service.sh', svc),
                  ('drm_hook_ctl.sh', ctl), ('model_spoof.sh', msp)]:
    hits = set(PAT.findall(txt))
    allp[name] = hits
    print(f'  {name:22s} {len(hits):3d} 个')

drm = allp['drm_hook_ctl.sh']
print()
print('  drm_hook_ctl.sh 写入的全部属性:')
for p in sorted(drm):
    print(f'    {p}')

other = allp['post-fs-data.sh'] | allp['service.sh'] | allp['model_spoof.sh']
inter = drm & other
print()
print('=== 2. DRM 与其他脚本的属性交集 ===')
print('  交集:', sorted(inter) if inter else '空 —— 无属性写入冲突')

print()
print('=== 3. drm_hook_ctl.sh 的 service 操作对象 ===')
for m in re.finditer(r'^\s*(SVC|FALLBACK_SVCS|CANDIDATE_SVCS)=([^\n]*)', ctl, re.M):
    print('  ' + m.group(0).strip()[:220])

# 找出所有 stop/start 调用行，看它们作用的变量
print()
print('  stop/start 调用点:')
for i, line in enumerate(ctl.split('\n'), 1):
    if re.search(r'(?<![\w-])(stop|start)\s+"?\$', line):
        print(f'    {i:4d}| {line.strip()[:120]}')

print()
print('=== 4. do_boot 是否含 stop（关键）===')
# 按行号区间切函数边界。用正则 .*?\n\} 会贪婪吃到下一个函数结尾，
# 把 do_disable 的 stop 误算进 do_boot —— 必须先定位各函数起止行。
lines = ctl.split('\n')
starts = [(i, l) for i, l in enumerate(lines, 1) if re.match(r'^(do_\w+)\(\)', l)]
bounds = {}
for idx, (ln, l) in enumerate(starts):
    end = starts[idx + 1][0] - 1 if idx + 1 < len(starts) else len(lines)
    bounds[l.split('(')[0]] = (ln, end)


def count_calls(seg_lines, verb):
    """只数真实调用，排除注释行与 log 字符串里的同名词。

    do_boot 的设计目标就是"绝不 stop"，而它的注释和日志里反复出现
    "stop" 这个词（"仍不 stop"、"不执行 stop"）。不剔除就会误报。
    """
    n = 0
    for l in seg_lines:
        s = l.strip()
        if s.startswith('#'):
            continue
        if not re.search(rf'(?<![\w-]){verb}\s+"?\$', s):
            continue
        n += 1
    return n


for fn, (a, b) in bounds.items():
    seg = lines[a - 1:b]
    ns = count_calls(seg, 'stop')
    nt = count_calls(seg, 'start')
    tag = 'OK  ' if not (fn == 'do_boot' and ns) else 'WARN'
    print(f'  [{tag}] {fn:12s} 行 {a}-{b}  stop={ns} start={nt}')

boot_stop = count_calls(lines[bounds['do_boot'][0] - 1:bounds['do_boot'][1]], 'stop')
print()
print('  结论:', 'do_boot 零 stop —— 开机不打断 DRM 播放' if boot_stop == 0
      else f'do_boot 含 {boot_stop} 处 stop —— 需修正')

print()
print('=== 5. post-fs-data.sh 中机型伪装的调用时机 ===')
for i, line in enumerate(post.split('\n'), 1):
    if 'model_spoof' in line:
        print(f'    {i:4d}| {line.strip()[:120]}')
