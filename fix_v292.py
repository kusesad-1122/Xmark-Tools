#!/usr/bin/env python3
import os
os.chdir('/tmp/xmt')

with open('zygisk/zygisk_cpuinfo.cpp', 'r') as f:
    content = f.read()

# The current v2.9 companion_handler has a6-second stage0 delay
# Replace it with immediate mount + inotify-only death detection

old_start = '    // v2.9: live 6s before mount, filter background restarts'
new_start = '    // v2.9.2: mount immediately, inotify-only death, socket EOF ignored'

# Find the companion_handler body
idx = content.find(old_start)
if idx <0:
    # Try alternative Chinese version
    old_start_cn = '    // v2.9: 进程存活6s后才挂载, 过滤后台短暂拉起进程'
    idx = content.find(old_start_cn)
    if idx <0:
        print("ERROR: couldn't find the v2.9 block!")
        # Search for key patterns
        for kw in ['stage=0', '6000', 'live 6s']:
            i = content.find(kw)
            if i>=0:
                print(f"  Found '{kw}' at {i}")
        exit(1)

# Find the end of the companion_handler (right before cleanup)
# Look for the '    if(inotify_fd>=0) close(inotify_fd);' that's INSIDE companion_handler
# Actually, let's find the full range to replace

# The new code (immediate mount, inotify-only death)
new_code = '''    // v2.9.2: mount immediately, inotify-only death, socket EOF ignored
    pthread_mutex_lock(&g_lock);
    if(cpu_t){
        if(!g_mounted) g_mounted=do_global_mount();
        if(g_mounted){g_count++;cpu_inc=true;}
    }
    if(hide_t){
        if(!g_hide_on) g_hide_on=do_hide_mount();
        if(g_hide_on){g_hide_count++;hide_inc=true;}
    }
    pthread_mutex_unlock(&g_lock);
    flog("MOUNTED cpu=%d hide=%d (pid=%d)",cpu_inc,hide_inc,app_pid);
    unsigned char st=1; xwrite(client,&st,1);
    bool app_died=false;
    while(!app_died){
        struct pollfd fds[2]; int nfds=0;
        fds[nfds].fd=client; fds[nfds].events=POLLIN; fds[nfds].revents=0; nfds++;
        if(inotify_fd>=0){fds[nfds].fd=inotify_fd;fds[nfds].events=POLLIN;fds[nfds].revents=0;nfds++;}
        int ret=poll(fds,nfds,-1);
        if(ret<0){if(errno==EINTR)continue;break;}
        if(fds[0].revents&(POLLIN|POLLHUP|POLLERR)){
            char buf[8]; ssize_t k=read(client,buf,sizeof(buf));
            if(k<=0){
                // DLCLOSE causes immediate socket EOF, NOT real death
                // Keep waiting for inotify IN_DELETE_SELF
                flog("SOCKET-EOF (dlclose, waiting for inotify) nice=%s",nice);
            }
        }
        if(!app_died&&inotify_fd>=0&&(fds[1].revents&POLLIN)){
            char ev_buf[4096]; ssize_t len=read(inotify_fd,ev_buf,sizeof(ev_buf));
            if(len>0){flog("APP-DIED pid=%d (inotify)",app_pid);app_died=true;}
        }
    }
    if(inotify_fd>=0) close(inotify_fd);
    pthread_mutex_lock(&g_lock);
    if(cpu_inc  && --g_count==0      && g_mounted){ do_global_umount(); g_mounted=false; }
    if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount(); g_hide_on=false; }
    pthread_mutex_unlock(&g_lock);
    flog("UMOUNT on death cpu=%d hide=%d (nice=%s)",cpu_inc,hide_inc,nice);'''

# Find the old blocks from 'bool cpu_inc=false, hide_inc=false;' to the end of companion_handler
# The old block starts at '    // v2.9:' and goes to '    flog("UMOUNT on death ...'
# Let's find the exact boundaries

# Find the old UMOUNT log line inside companion_handler
old_umount = '    flog("UMOUNT on death cpu=%d hide=%d (nice=%s)",cpu_inc,hide_inc,nice);'
# After this line there should be a closing '}' and then the class

idx_umount = content.find(old_umount)
if idx_umount <0:
    print("ERROR: cannot find old UMOUNT line")
    # Debug
    for i, line in enumerate(content.split('\n')):
        if 'UMOUNT on death' in line:
            print(f"  Line {i+1}: {line}")
    exit(1)

# The block to replace goes from idx (start of v2.9 block) to idx_umount + len(old_umount)
end_idx = idx_umount + len(old_umount)

old_block = content[idx:end_idx]
print(f"Replacing {len(old_block)} bytes")

content = content[:idx] + new_code + content[end_idx:]

with open('zygisk/zygisk_cpuinfo.cpp', 'w') as f:
    f.write(content)

# Verify
if 'stage=0' in content:
    print("WARNING: stage=0 still present!")
else:
    print("OK: no stage delay")

if 'waiting for inotify' in content:
    print("OK: socket EOF correctly ignored")

print(f"File: {len(content)} bytes")