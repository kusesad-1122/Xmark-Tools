#!/usr/bin/env python3
import os, sys

# Read the current github source
os.chdir('/tmp/xmt')

# The complete companion_handler we want (v2.9)
new_handler = r'''
// v2.9: 不用 oom_score_adj（有race condition），改用进程存活时长过滤
// 进程存活超过6秒才挂载→过滤掉后台短暂拉起的进程
static void companion_handler(int client){
    int nlen=0; if(!xread(client,&nlen,sizeof(nlen))||nlen<=0||nlen>240)return;
    char nice[256]={0}; if(!xread(client,nice,(size_t)nlen))return; nice[nlen]=0;
    int app_pid=0; xread(client,&app_pid,sizeof(app_pid));
    flog("COMPANION nice=%s pid=%d",nice,app_pid);
    bool cpu_t  = decide_target(nice);
    bool is_main = (strchr(nice,':')==nullptr);
    bool hide_t = hide_decide(nice);
    if(!cpu_t && !hide_t){ unsigned char z=0; xwrite(client,&z,1); return; }
    if(!is_main){ unsigned char z=0; xwrite(client,&z,1); return; }
    int inotify_fd = -1;
    if(app_pid>0){
        inotify_fd=inotify_init();
        if(inotify_fd>=0){
            char pp[64]; snprintf(pp,sizeof(pp),"/proc/%d",app_pid);
            if(inotify_add_watch(inotify_fd,pp,IN_DELETE_SELF)<0){close(inotify_fd);inotify_fd=-1;}
            else flog("INOTIFY watching /proc/%d",app_pid);
        }
    }
    unsigned char st=1;
    if(!xwrite(client,&st,1)){ if(inotify_fd>=0)close(inotify_fd); return; }
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
}
'''

# Read the old file  
with open('zygisk/zygisk_cpuinfo.cpp', 'r') as f:
    old = f.read()

# Find where companion_handler starts
idx = old.find('static void companion_handler')
if idx < 0:
    print("ERROR: companion_handler not found!")
    sys.exit(1)

# Find where the companion_handler ends (the next function after it)
# The companion_handler is the last function before the class CpuSpoofModule
idx_end = old.find('class CpuSpoofModule')
if idx_end < 0:
    print("ERROR: CpuSpoofModule not found!")
    sys.exit(1)

# Keep everything before companion_handler and append the new one + the rest
new_content = old[:idx] + new_handler + '\n' + old[idx_end:]

# Write back
with open('zygisk/zygisk_cpuinfo.cpp', 'w') as f:
    f.write(new_content)

# Verify
with open('zygisk/zygisk_cpuinfo.cpp', 'r') as f:
    verify = f.read()
print(f"Wrote {len(verify)} bytes")
if 'stage=0' in verify and 'SKIP mount' in verify:
    print("SUCCESS: v2.9 code confirmed!")
else:
    print("ERROR: v2.9 patterns not found!")
