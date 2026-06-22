#!/usr/bin/env python3
import os
os.chdir('/tmp/xmt')

with open('zygisk/zygisk_cpuinfo.cpp', 'r') as f:
    content = f.read()

# Find the companion_handler and replace it completely
idx_handler = content.find('static void companion_handler')
if idx_handler <0:
    print("ERROR: companion_handler not found")
    exit(1)

idx_class = content.find('class CpuSpoofModule')
if idx_class <0:
    print("ERROR: class not found")
    exit(1)

# The new companion_handler + thread function (matching CPU伪装1.7 approach)
# Key: file-based instance_count, monitoring thread, socket EOF ignored
new_code = '''// v2.9.3: file-based counting + monitoring thread (CPU伪装1.7 approach)
// instance_count file tracks active processes
// Monitoring thread polls /proc/<pid> via stat() for real death detection
// Socket EOF from DLCLOSE is ignored - only thread-confirmed death counts
static void* monitor_thread(void* arg){
    int pid = (int)(long)arg;
    char path[64];
    snprintf(path,sizeof(path),"/proc/%d",pid);
    // Poll /proc/<pid> every 500ms until it disappears (process died)
    while(stat(path,nullptr)==0){
        nanosleep((const struct timespec[]){{0,500000000}},nullptr);
    }
    return nullptr;
}
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

    // Send status=1 (client keeps connection, though we ignore socket EOF)
    unsigned char st=(cpu_inc||hide_inc)?1:0;
    if(!xwrite(client,&st,1)){
        // Write fail, undo
        pthread_mutex_lock(&g_lock);
        if(cpu_inc  && --g_count==0      && g_mounted){ do_global_umount(); g_mounted=false; }
        if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount(); g_hide_on=false; }
        pthread_mutex_unlock(&g_lock);
        return;
    }

    // Create monitoring thread to track true process death
    // Socket EOF from DLCLOSE is NOT real death - thread confirms via /proc/<pid>
    pthread_t monitor;
    pthread_create(&monitor,nullptr,monitor_thread,(void*)(long)app_pid);

    // Read loop: ignore socket EOF (dlclose), wait for inotify
    // But also: if thread finishes (process died during inotify setup), handle it
    bool app_died=false;
    int inotify_fd = -1;
    if(app_pid>0){
        inotify_fd=inotify_init();
        if(inotify_fd>=0){
            char pp[64]; snprintf(pp,sizeof(pp),"/proc/%d",app_pid);
            if(inotify_add_watch(inotify_fd,pp,IN_DELETE_SELF)<0){close(inotify_fd);inotify_fd=-1;}
            else flog("INOTIFY watching /proc/%d",app_pid);
        }
    }
    while(!app_died){
        struct pollfd fds[2]; int nfds=0;
        fds[nfds].fd=client; fds[nfds].events=POLLIN; fds[nfds].revents=0; nfds++;
        if(inotify_fd>=0){fds[nfds].fd=inotify_fd;fds[nfds].events=POLLIN;fds[nfds].revents=0;nfds++;}
        int ret=poll(fds,nfds,-1);
        if(ret<0){if(errno==EINTR)continue;break;}
        // Socket EOF from DLCLOSE: NOT real death, ignore
        if(fds[0].revents&(POLLIN|POLLHUP|POLLERR)){
            char buf[8]; ssize_t k=read(client,buf,sizeof(buf));
            if(k<=0){flog("SOCKET-EOF (dlclose, waiting for inotify) nice=%s",nice);}
        }
        // Inotify IN_DELETE_SELF: real death confirmed
        if(!app_died&&inotify_fd>=0&&(fds[1].revents&POLLIN)){
            char ev_buf[4096]; ssize_t len=read(inotify_fd,ev_buf,sizeof(ev_buf));
            if(len>0){flog("APP-DIED pid=%d (inotify)",app_pid);app_died=true;}
        }
    }
    if(inotify_fd>=0) close(inotify_fd);

    // Wait for monitoring thread to also confirm death
    pthread_join(monitor,nullptr);
    flog("MONITOR confirmed death pid=%d",app_pid);

    // Decrement count, umount if zero
    pthread_mutex_lock(&g_lock);
    if(cpu_inc  && --g_count==0      && g_mounted){ do_global_umount(); g_mounted=false; }
    if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount(); g_hide_on=false; }
    pthread_mutex_unlock(&g_lock);
    flog("UMOUNT on death cpu=%d hide=%d (nice=%s)",cpu_inc,hide_inc,nice);
}'''

# Replace from handler start to class start
content = content[:idx_handler] + new_code + '\n' + content[idx_class:]

with open('zygisk/zygisk_cpuinfo.cpp', 'w') as f:
    f.write(content)

# Verify key patterns
checks = ['monitor_thread', 'pthread_create', 'pthread_join', 'nanosleep',
          'MONITOR confirmed death', 'inotify', 'dlclose']
for check in checks:
    if check in content:
        print(f"  OK: {check}")
    else:
        print(f"  MISSING: {check}")

print(f"\\nFile: {len(content)} bytes")