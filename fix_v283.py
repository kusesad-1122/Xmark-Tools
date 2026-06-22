with open('/tmp/xmt/zygisk/zygisk_cpuinfo.cpp', 'r') as f:
    content = f.read()

old_func = '''static bool is_foreground_process(pid_t pid){
    char path[64];
    snprintf(path,sizeof(path),"/proc/%d/oom_score_adj",pid);
    int fd=open(path,O_RDONLY);
    if(fd<0) return true;  // can't read, assume foreground (conservative)
    char buf[16]={0};
    read(fd,buf,sizeof(buf)-1);
    close(fd);
    int score=atoi(buf);
    // foreground apps oom_score_adj<=200, background services>=800
    return score <= 500;
}'''

new_func = '''static bool is_foreground_process(pid_t pid){
    char path[64];
    snprintf(path,sizeof(path),"/proc/%d/oom_score_adj",pid);
    // Zygote inherits -1000; AMS sets actual value (0 for FG, ~945 for BG)
    // within ~50-200ms. Poll until value stabilizes != -1000.
    for(int i=0; i<40; i++){  // up to 2 seconds
        int fd=open(path,O_RDONLY);
        if(fd<0) return true;  // process died, assume foreground
        char buf[16]={0};
        if(read(fd,buf,sizeof(buf)-1)<=0){close(fd);break;}
        close(fd);
        int score=atoi(buf);
        // If AMS hasn't set it yet, value is still -1000 (zygote default)
        if(score != -1000) return score <= 500;
        usleep(50000);  // 50ms
    }
    return true;  // timeout, assume foreground
}'''

if old_func in content:
    content = content.replace(old_func, new_func, 1)
    with open('/tmp/xmt/zygisk/zygisk_cpuinfo.cpp', 'w') as f:
        f.write(content)
    print("REPLACED OK")
else:
    print("ERROR: old_func not found!")
    # Debug: show what's around line 192
    lines = content.split('\n')
    for i, line in enumerate(lines, 1):
        if 'is_foreground_process' in line:
            print(f"Found at line {i}: {line[:100]}")
