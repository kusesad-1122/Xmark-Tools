#!/usr/bin/env python3
import os
os.chdir('/tmp/xmt')

with open('zygisk/zygisk_cpuinfo.cpp', 'r') as f:
    content = f.read()

# Fix: replace monitor_thread to use access() instead of stat(NULL)
old_thread = '''static void* monitor_thread(void* arg){
    int pid = (int)(long)arg;
    char path[64];
    snprintf(path,sizeof(path),"/proc/%d",pid);
    // Poll /proc/<pid> every 500ms until it disappears (process died)
    while(stat(path,nullptr)==0){
        nanosleep((const struct timespec[]){{0,500000000}},nullptr);
    }
    return nullptr;
}'''

new_thread = '''static void* monitor_thread(void* arg){
    int pid = (int)(long)arg;
    char path[64];
    snprintf(path,sizeof(path),"/proc/%d",pid);
    // Poll /proc/<pid> via access() every 500ms until the process dies
    // stat() with NULL buffer causes EFAULT - use access(F_OK) instead
    while(access(path,F_OK)==0){
        nanosleep((const struct timespec[]){{0,500000000}},nullptr);
    }
    return nullptr;
}'''

if old_thread in content:
    content = content.replace(old_thread, new_thread, 1)
    print("Fixed monitor_thread: stat->access")
else:
    print("WARNING: old_thread not found!")
    # Search for monitor_thread
    idx = content.find('monitor_thread')
    if idx >= 0:
        print(f"Found at {idx}")
        # Show surrounding text
        print(content[idx:idx+300])

with open('zygisk/zygisk_cpuinfo.cpp', 'w') as f:
    f.write(content)
print(f"Done. File: {len(content)} bytes")