#!/usr/bin/env python3
import os
os.chdir('/tmp/xmt')

with open('zygisk/zygisk_cpuinfo.cpp', 'r') as f:
    content = f.read()

# Find the companion_handler and replace with simple inotify-only approach
# No monitoring thread, no compound literals, no C99 features
idx_handler = content.find('static void companion_handler')
idx_class = content.find('class CpuSpoofModule')

if idx_handler <0 or idx_class <0:
    print("ERROR: boundaries not found")
    exit(1)

# New companion_handler - simple, no threads, C++17 compatible
new_handler = '''static void companion_handler(int client){
    int nlen=0; if(!xread(client,&nlen,sizeof(nlen))||nlen<=0||nlen>240)return;
    char nice[256]={0}; if(!xread(client,nice,(size_t)nlen))return; nice[nlen]=0;
    int app_pid=0; xread(client,&app_pid,sizeof(app_pid));
    flog("COMPANION nice=%s pid=%d",nice,app_pid);

    bool cpu_t  = decide_target(nice);
    bool is_main = (strchr(nice,':')==nullptr);
    bool hide_t = hide_decide(nice);
    if(!cpu_t && !hide_t){ unsigned char z=0; xwrite(client,&z,1); return; }
    if(!is_main){ unsigned char z=0; xwrite(client,&z,1); return; }

    // Mount immediately (no delay)
    bool cpu_inc=false, hide_inc=false;
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

    // Set up inotify BEFORE sending status
    int inotify_fd = -1;
    if(app_pid>0){
        inotify_fd=inotify_init();
        if(inotify_fd>=0){
            char pp[64]; snprintf(pp,sizeof(pp),"/proc/%d",app_pid);
            if(inotify_add_watch(inotify_fd,pp,IN_DELETE_SELF)<0){close(inotify_fd);inotify_fd=-1;}
            else flog("INOTIFY watching /proc/%d",app_pid);
        }
    }

    // Send status=1 (client keeps connection, though we ignore socket EOF)
    unsigned char st=1;
    if(!xwrite(client,&st,1)){
        pthread_mutex_lock(&g_lock);
        if(cpu_inc  && --g_count==0      && g_mounted){ do_global_umount(); g_mounted=false; }
        if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount(); g_hide_on=false; }
        pthread_mutex_unlock(&g_lock);
        if(inotify_fd>=0) close(inotify_fd);
        return;
    }

    // Death detection: ONLY inotify IN_DELETE_SELF counts as death
    // Socket EOF from ZygiskNext DLCLOSE is NOT real death - IGNORE it
    bool app_died=false;
    while(!app_died){
        struct pollfd fds[2]; int nfds=0;
        fds[nfds].fd=client; fds[nfds].events=POLLIN; fds[nfds].revents=0; nfds++;
        if(inotify_fd>=0){fds[nfds].fd=inotify_fd;fds[nfds].events=POLLIN;fds[nfds].revents=0;nfds++;}
        int ret=poll(fds,nfds,-1);
        if(ret<0){if(errno==EINTR)continue;break;}
        // Socket IGNORED: DLCLOSE causes immediate EOF, NOT death
        if(fds[0].revents&(POLLIN|POLLHUP|POLLERR)){
            char buf[8]; ssize_t k=read(client,buf,sizeof(buf));
            if(k<=0){flog("SOCKET-EOF ignored (dlclose) nice=%s",nice);}
        }
        // Inotify IN_DELETE_SELF: ONLY this confirms real death
        if(!app_died&&inotify_fd>=0&&(fds[1].revents&POLLIN)){
            char ev_buf[4096]; ssize_t len=read(inotify_fd,ev_buf,sizeof(ev_buf));
            if(len>0){flog("APP-DIED pid=%d (inotify)",app_pid);app_died=true;}
        }
    }
    if(inotify_fd>=0) close(inotify_fd);

    // Decrement count, umount if zero
    pthread_mutex_lock(&g_lock);
    if(cpu_inc  && --g_count==0      && g_mounted){ do_global_umount(); g_mounted=false; }
    if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount(); g_hide_on=false; }
    pthread_mutex_unlock(&g_lock);
    flog("UMOUNT on death cpu=%d hide=%d (nice=%s)",cpu_inc,hide_inc,nice);
}'''

# Replace
content = content[:idx_handler] + new_handler + '\n' + content[idx_class:]

# Verify no C99 compound literals
if '(struct timespec' in content:
    print("WARNING: compound literal still present!")
else:
    print("OK: no compound literals")

if 'pthread_create' in content:
    print("WARNING: pthread_create still present!")
else:
    print("OK: no threads")

if 'SOCKET-EOF ignored' in content:
    print("OK: socket EOF ignored correctly")

if 'APP-DIED' in content:
    print("OK: inotify-only death detection")

with open('zygisk/zygisk_cpuinfo.cpp', 'w') as f:
    f.write(content)

print(f"\\nFile: {len(content)} bytes")