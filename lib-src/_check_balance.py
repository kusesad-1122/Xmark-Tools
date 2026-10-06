import sys

path = sys.argv[1]
src = open(path, encoding='utf-8').read()
i, n = 0, len(src)
out = []          # (char, line) for code only
line = 1
st = None         # None | '//' | '/*' | '"' | "'"
BS = chr(92)
Q = chr(34)
A = chr(39)

while i < n:
    c = src[i]
    if c == '\n':
        line += 1
    if st is None:
        if c == '/' and i + 1 < n and src[i+1] == '/':
            st = '//'; i += 2; continue
        if c == '/' and i + 1 < n and src[i+1] == '*':
            st = '/*'; i += 2; continue
        if c == Q:
            st = Q; i += 1; continue
        if c == A:
            st = A; i += 1; continue
        out.append((c, line)); i += 1
    elif st == '//':
        if c == '\n':
            st = None
        i += 1
    elif st == '/*':
        if c == '*' and i + 1 < n and src[i+1] == '/':
            st = None; i += 2; continue
        i += 1
    elif st == Q:
        if c == BS:
            i += 2; continue
        if c == Q:
            st = None
        i += 1
    elif st == A:
        if c == BS:
            i += 2; continue
        if c == A:
            st = None
        i += 1

pairs = {')': '(', ']': '[', '}': '{'}
stack = []
ok = True
for c, ln in out:
    if c in '([{':
        stack.append((c, ln))
    elif c in ')]}':
        if not stack or stack[-1][0] != pairs[c]:
            print('UNBALANCED close %r at line %d' % (c, ln)); ok = False; break
        stack.pop()

if stack:
    print('UNCLOSED opens:', stack[:5]); ok = False

print('lines            : %d' % len(src.splitlines()))
print('bracket balance  : %s' % ('PASS' if ok else 'FAIL'))
print('string/comment EOF: %s' % ('clean' if st is None else 'UNTERMINATED ' + repr(st)))
sys.exit(0 if ok and st is None else 1)
