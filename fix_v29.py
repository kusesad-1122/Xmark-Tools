#!/usr/bin/env python3
import os

os.chdir('/tmp/xmt')

with open('zygisk/zygisk_cpuinfo.cpp', 'r') as f:
    lines = f.readlines()

# Find key points
found = {}
for i, line in enumerate(lines):
    if 'companion_handler(int client)' in line:
        found['handler_start'] = i
    if 'bool cpu_t  = decide_target(nice);' in line:
        found['decide_start'] = i
    if 'class CpuSpoofModule' in line:
        found['class_start'] = i

print(f"Found: handler_start={found.get('handler_start')}, decide_start={found.get('decide_start')}, class_start={found.get('class_start')}")

if 'decide_start' not in found:
    print("ERROR: cannot find key locations")
    exit(1)

# Build new code between decide_target and class CpuSpoofModule
new_code = '''    bool cpu_t  = decide_target(nice);
    bool is_main = (strchr(nice,':')==nullptr);
    bool hide_t = hide_decide(nice);
    if(!cpu_t && !hide_t){ unsigned char z=0; xwrite(client,&z,1); return; }
    if(!is_main){ unsigned char z=0; xwrite(client,&z,1); return; }
    // inotify
    int inotify_fd = -1;
    if(app_pid>0){
        inotify_fd=inotify_init();
        if(inotify_fd>=0){
            char pp[64]; snprintf(pp,sizeof(pp),"/proc/%d",app_pid);
            if(inotify_add_watch(inotify_fd,pp,IN_DELETE_SELF)<0){close(inotify_fd);inotify_fd=-1;}
            else flog("INOTIFY watching /proc/%d",app_pid);
        }
    }
    // v2.9: live 6s before mount, filter background restarts
    unsigned char st=1; xwrite(client,&st,1);
    bool app_died=false;
    bool cpu_inc=false, hide_inc=false;
    bool did_mount=false;
    int stage=0;
    while(stage<2 && !app_died){
        struct pollfd fds[2]; int nfds=0;
        fds[nfds].fd=client; fds[nfds].events=POLLIN; fds[nfds].revents=0; nfds++;
        if(inotify_fd>=0){fds[nfds].fd=inotify_fd;fds[nfds].events=POLLIN;fds[nfds].revents=0;nfds++;}
        int timeout = (stage==0) ? 6000 : -1;
        int ret=poll(fds,nfds,timeout);
        if(ret<0){if(errno==EINTR)continue;break;}
        if(fds[0].revents&(POLLIN|POLLHUP|POLLERR)){
            char buf[8]; ssize_t k=read(client,buf,sizeof(buf));
            if(k<=0){flog("SOCKET-EOF k=%zd err=%d nice=%s",k,k<0?errno:0,nice);app_died=true;}
        }
        if(!app_died&&inotify_fd>=0&&(fds[1].revents&POLLIN)){
            char ev_buf[4096]; ssize_t len=read(inotify_fd,ev_buf,sizeof(ev_buf));
            if(len>0){flog("APP-DIED pid=%d (inotify)",app_pid);app_died=true;}
        }
        if(ret==0 && stage==0 && !app_died){
            stage=1;
            pthread_mutex_lock(&g_lock);
            if(cpu_t){
                if(!g_mounted) g_mounted=do_global_mount();
                if(g_mounted){g_count++;cpu_inc=true;did_mount=true;}
            }
            if(hide_t){
                if(!g_hide_on) g_hide_on=do_hide_mount();
                if(g_hide_on){g_hide_count++;hide_inc=true;did_mount=true;}
            }
            pthread_mutex_unlock(&g_lock);
            flog("MOUNTED after 6s cpu=%d hide=%d (pid=%d)",cpu_inc,hide_inc,app_pid);
        }
        if(app_died && stage==0){
            flog("SKIP mount (died early, pid=%d lived <6s)",app_pid);
            stage=2;
        }
    }
    if(inotify_fd>=0) close(inotify_fd);
    if(did_mount && app_died){
        pthread_mutex_lock(&g_lock);
        if(cpu_inc  && --g_count==0      && g_mounted){ do_global_umount(); g_mounted=false; }
        if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount(); g_hide_on=false; }
        pthread_mutex_unlock(&g_lock);
        flog("UMOUNT on death cpu=%d hide=%d (nice=%s)",cpu_inc,hide_inc,nice);
    }
'''

# Find the old code between decide_start and class_start
old_code_lines = lines[found['decide_start']:found['class_start']]
old_code = ''.join(old_code_lines)

# Replace
lines[found['decide_start']:found['class_start']] = [new_code]

with open('zygisk/zygisk_cpuinfo.cpp', 'w') as f:
    f.writelines(lines)

# Verify
with open('zygisk/zygisk_cpuinfo.cpp', 'r') as f:
    v = f.read()

if 'stage=0' in v and 'SKIP mount' in v and 'MOUNTED after 6s' in v:
    print(f"SUCCESS! File: {len(v)} bytes")
else:
    print("FAILED!")
    print(f"stage=0: {'stage=0' in v}")
    print(f"SKIP mount: {'SKIP mount' in v}")
    print(f"MOUNTED after 6s: {'MOUNTED after 6s' in v}")
