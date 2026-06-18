#!/usr/bin/env python3
"""Generate cpuinfo_presets.h from cpuinfo_* files."""
import os, glob

presets = []
for f in sorted(glob.glob("cpuinfo_*")):
    if not os.path.isfile(f) or f.endswith('.py') or f.endswith('.h') or f.endswith('.cpp'):
        continue
    name = os.path.basename(f).replace("cpuinfo_", "")
    with open(f, "rb") as fh:
        data = fh.read()
    arr = ",".join(str(b) for b in data)
    presets.append((name, len(data), arr))

with open("cpuinfo_presets.h", "w") as out:
    out.write(f"// Auto-generated: {len(presets)} CPU presets\n")
    out.write("#pragma once\n#include <stddef.h>\n\n")
    for name, size, arr in presets:
        out.write(f"static const unsigned char PRESET_{name}[] = {{{arr}}};\n")
    out.write("\nstatic const struct { const char *name; const unsigned char *data; size_t len; } CPU_PRESETS[] = {\n")
    for name, size, _ in presets:
        out.write(f'    {{"{name}", PRESET_{name}, {size}}},\n')
    out.write("};\n")
    out.write(f"\n#define CPU_PRESET_COUNT {len(presets)}\n")

print(f"Generated cpuinfo_presets.h with {len(presets)} presets")