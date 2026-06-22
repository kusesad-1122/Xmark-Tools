with open('/tmp/xmt/zygisk/zygisk_cpuinfo.cpp','r') as f:
    content = f.read()

# the exact text from the file
old1 = "    bool is_main = (strchr(nice,':')==nullptr);"
new1 = "    bool is_main = (strchr(nice,':')==nullptr);\n    bool is_fg = is_foreground_process(app_pid);"
if old1 not in content:
    print("ERROR: old1 not found!")
    print(f"Looking for: {repr(old1)}")
else:
    content = content.replace(old1, new1, 1)
    print("old1 replaced OK")

old2 = "    if(cpu_t && is_main){"
new2 = "    if(cpu_t && is_main && is_fg){"
if old2 not in content:
    print("ERROR: old2 not found!")
else:
    content = content.replace(old2, new2, 1)
    print("old2 replaced OK")

old3 = "    if(hide_t && is_main){ if(!g_hide_on) g_hide_on=do_hide_mount();"
new3 = "    if(hide_t && is_main && is_fg){ if(!g_hide_on) g_hide_on=do_hide_mount();"
if old3 not in content:
    print("ERROR: old3 not found!")
else:
    content = content.replace(old3, new3, 1)
    print("old3 replaced OK")

with open('/tmp/xmt/zygisk/zygisk_cpuinfo.cpp','w') as f:
    f.write(content)
print("\nAll done!")
