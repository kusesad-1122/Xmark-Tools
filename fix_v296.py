#!/usr/bin/env python3
import os
os.chdir('/tmp/xmt')

with open('zygisk/zygisk_cpuinfo.cpp', 'r') as f:
    c = f.read()

# Replace the companion_handler with the 10-second delay + foreground check approach
idx_handler = c.find('static void companion_handler')
idx_class = c.find('class CpuSpoofModule')

if idx_handler <0 or idx_class <0:
    print("ERROR: boundaries not found")
    exit(1)

new_handler = '''// v2.9.6: 10s foreground check before mount, prevent background restart triggers
// Game process must survive 10s AND be in foreground (oom_score_adj<=200)
static bool is_foreground(pid_t pid){
    char path[64];
    snprintf(path,sizeof(path),"/proc/%d/oom_score_adj",pid);
    int fd=open(path,O_RDONLY);
    if(fd<0) return false;
    char buf[16]={0};
    read(fd,buf,sizeof(buf)-1);
    close(fd);
    int score=atoi(buf);
    // foreground <=200, background >=800
    return score <= 200;
}
static void companion_handler(int client){
    int nlen=0; if(!xread(client,&nlen,sizeof(nlen))||nlen<=0||nlen>240)return;
    char nice[256]={0}; if(!xread(client,nice,(size_t)nlen))return; nice[nlen]=0;
    int app_pid=0; xread(client,&app_pid,sizeof(app_pid));
    flog("COMPANION nice=%s pid=%d",nice,app_pid);

    bool cpu_t  = decide_target(nice);
    bool is_main = (strchr(nice,':')==nullptr);
    bool hide_basic = (access(MODDIR "/pid/anti_mark",F_OK)==0) && name_in_file(HIDE_GAMES_F,nice);
    bool hide_pro = (access(MODDIR "/pid/hide_storage",F_OK)==0) && name_in_file(HIDE_GAMES_F,nice);
    if(!cpu_t && !hide_basic && !hide_pro){ unsigned char z=0; xwrite(client,&z,1); return; }
    if(!is_main){ unsigned char z=0; xwrite(client,&z,1); return; }

    // Set up inotify immediately for death detection
    int inotify_fd = -1;
    if(app_pid>0){
        inotify_fd=inotify_init();
        if(inotify_fd>=0){
            char pp[64]; snprintf(pp,sizeof(pp),"/proc/%d",app_pid);
            if(inotify_add_watch(inotify_fd,pp,IN_DELETE_SELF)<0){close(inotify_fd);inotify_fd=-1;}
            else flog("INOTIFY watching /proc/%d",app_pid);
        }
    }

    // Send status=1 immediately (client keeps connection, we do death detection)
    unsigned char st=1; xwrite(client,&st,1);

    // Phase 1: Wait 10s to let the process stabilize, check if foreground game
    bool app_died=false;
    bool did_mount=false;
    bool cpu_inc=false, hide_inc=false, hide_inc_pro=false;
    int elapsed=0;

    while(elapsed<10000 && !app_died && !did_mount){
        struct pollfd fds[2]; int nfds=0;
        fds[nfds].fd=client; fds[nfds].events=POLLIN; fds[nfds].revents=0; nfds++;
        if(inotify_fd>=0){fds[nfds].fd=inotify_fd;fds[nfds].events=POLLIN;fds[nfds].revents=0;nfds++;}
        int timeout = 10000 - elapsed;
        int ret=poll(fds,nfds,timeout>0?timeout:0);
        if(ret<0){if(errno==EINTR)continue;break;}
        if(ret==0){
            // 10s timeout: process survived, check if foreground
            if(is_foreground(app_pid)){
                // This is a real foreground game session, mount now
                pthread_mutex_lock(&g_lock);
                if(cpu_t){
                    if(!g_mounted) g_mounted=do_global_mount();
                    if(g_mounted){g_count++;cpu_inc=true;did_mount=true;}
                }
                if(hide_basic){
                    if(!g_hide_on) g_hide_on=do_hide_mount_basic();
                    if(g_hide_on){g_hide_count++;hide_inc=true;}
                }
                if(hide_pro){
                    if(!g_hide_on_pro) g_hide_on_pro=do_hide_mount_pro();
                    if(g_hide_on_pro){g_hide_count_pro++;hide_inc_pro=true;}
                }
                pthread_mutex_unlock(&g_lock);
                flog("MOUNTED (foreground confirmed after 10s) cpu=%d hide=%d hide_pro=%d pid=%d",cpu_inc,hide_inc,hide_inc_pro,app_pid);
            } else {
                // Process alive but not foreground - background restart, skip mount
                flog("SKIP mount (alive but not foreground) pid=%d score=%d",app_pid,is_foreground(app_pid)?0:999);
                app_died=true; // exit, don't monitor further
            }
        } else {
            // Socket or inotify event during wait period
            if(fds[0].revents&(POLLIN|POLLHUP|POLLERR)){
                char buf[8]; ssize_t k=read(client,buf,sizeof(buf));
                if(k<=0){
                    if(elapsed<10000){
                        // Process died within 10s - was a background restart
                        flog("SKIP mount (died early within 10s) nice=%s pid=%d",nice,app_pid);
                    }
                    app_died=true;
                }
            }
            if(!app_died&&inotify_fd>=0&&(fds[1].revents&POLLIN)){
                char ev_buf[4096]; ssize_t len=read(inotify_fd,ev_buf,sizeof(ev_buf));
                if(len>0){
                    flog("APP-DIED (early) pid=%d",app_pid);
                    app_died=true;
                }
            }
            // Update elapsed time
            elapsed += 100;
        }
        elapsed += (ret==0)?10000:100;
    }

    // Phase 2: If we mounted, continue monitoring for death
    bool socket_eof=false;
    while(did_mount && !app_died){
        struct pollfd fds[2]; int nfds=0;
        if(!socket_eof){
            fds[nfds].fd=client; fds[nfds].events=POLLIN; fds[nfds].revents=0; nfds++;
        }
        if(inotify_fd>=0){
            fds[nfds].fd=inotify_fd; fds[nfds].events=POLLIN; fds[nfds].revents=0; nfds++;
        }
        if(nfds==0){ app_died=true; break; }
        int ret=poll(fds,nfds,-1);
        if(ret<0){if(errno==EINTR)continue;break;}
        if(!socket_eof && (fds[0].revents&(POLLIN|POLLHUP|POLLERR))){
            char buf[8]; ssize_t k=read(client,buf,sizeof(buf));
            if(k<=0){
                socket_eof=true;
                flog("SOCKET-EOF (dlclose) nice=%s",nice);
                if(inotify_fd<0){ app_died=true; }
            }
        }
        if(!app_died&&inotify_fd>=0&&(fds[socket_eof?0:1].revents&POLLIN)){
            char ev_buf[4096]; ssize_t len=read(inotify_fd,ev_buf,sizeof(ev_buf));
            if(len>0){flog("APP-DIED pid=%d (inotify)",app_pid);app_died=true;}
        }
    }

    // Cleanup
    if(inotify_fd>=0) close(inotify_fd);
    if(did_mount){
        pthread_mutex_lock(&g_lock);
        if(cpu_inc  && --g_count==0      && g_mounted){ do_global_umount(); g_mounted=false; }
        if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount_basic(); g_hide_on=false; }
        if(hide_inc_pro && --g_hide_count_pro==0 && g_hide_on_pro){ do_hide_umount_pro(); g_hide_on_pro=false; }
        pthread_mutex_unlock(&g_lock);
        flog("UMOUNT on death cpu=%d hide=%d hide_pro=%d (nice=%s)",cpu_inc,hide_inc,hide_inc_pro,nice);
    }
}'''

c = c[:idx_handler] + new_handler + '\n' + c[idx_class:]

# Remove old is_foreground_process if present
c = c.replace('static bool is_foreground_process', '// static bool is_foreground_process (removed)')

with open('zygisk/zygisk_cpuinfo.cpp', 'w') as f:
    f.write(c)

# Verify
checks = [
    ('is_foreground(pid_t', 'fg check function'),
    ('10000', '10s timeout'),
    ('MOUNTED (foreground confirmed after', 'mount after fg check'),
    ('SKIP mount (alive but not foreground)', 'skip background'),
    ('SKIP mount (died early within', 'skip early death'),
]
for pat, name in checks:
    print(f"  {'OK' if pat in c else 'MISSING'}: {name}")

print(f"File: {len(c)} bytes")