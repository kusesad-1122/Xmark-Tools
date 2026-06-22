#!/usr/bin/env python3
import os
os.chdir('/tmp/xmt')

with open('zygisk/zygisk_cpuinfo.cpp', 'r') as f:
    c = f.read()

# We need to completely rewrite the companion_handler.
# Current file has v2.9.6 (10s delay) which is wrong.
# New approach (v2.9.7): game-exit cooldown
# - Mount immediately (zero delay)
# - When game dies → write cooldown file (10s)
# - Within cooldown: new game process → check foreground → skip/kill if not, mount if yes
# - After cooldown: normal

# Find the companion_handler boundaries
idx_handler = c.find('static void companion_handler')
idx_class = c.find('class CpuSpoofModule')

if idx_handler < 0 or idx_class < 0:
    print("ERROR: boundaries not found")
    exit(1)

new_handler = '''// v2.9.7: Game-exit cooldown — mount immediately, 10s cooldown after exit to block background restarts
static void companion_handler(int client){
    // --- Cooldown check at entry ---
    // After a game process exits, we set a 10-second cooldown.
    // Any new game process starting during cooldown that is NOT foreground → skip + kill
    struct stat cd_st;
    if(stat(MODDIR "/pid/.cooldown", &cd_st)==0){
        int cdfd = open(MODDIR "/pid/.cooldown", O_RDONLY);
        if(cdfd>=0){
            char ts[32]={0}; ssize_t r=read(cdfd,ts,sizeof(ts)-1); close(cdfd);
            if(r>0){
                long long cooldown_until = atoll(ts);
                long long now = (long long)time(nullptr);
                if(now < cooldown_until){
                    // In cooldown period — need to read process info first
                    int nlen=0; if(!xread(client,&nlen,sizeof(nlen))||nlen<=0||nlen>240)return;
                    char nice[256]={0}; if(!xread(client,nice,(size_t)nlen))return; nice[nlen]=0;
                    int app_pid=0; xread(client,&app_pid,sizeof(app_pid));
                    
                    // Wait briefly for oom_score_adj to stabilize (avoid -1000 race)
                    // During cooldown, a short wait is acceptable
                    usleep(1500000); // 1.5s
                    
                    char oom_path[64]; snprintf(oom_path,sizeof(oom_path),"/proc/%d/oom_score_adj",app_pid);
                    int oom_fd = open(oom_path,O_RDONLY);
                    bool is_fg = false;
                    if(oom_fd>=0){
                        char buf[16]={0}; read(oom_fd,buf,sizeof(buf)-1); close(oom_fd);
                        int score = atoi(buf);
                        is_fg = (score > 0 && score <= 200); // Not -1000 (not set) and foreground range
                    }
                    
                    if(!is_fg){
                        flog("COOLDOWN skip (background restart) nice=%s pid=%d",nice,app_pid);
                        unsigned char z=0; xwrite(client,&z,1);
                        kill(app_pid, SIGKILL);
                        return;
                    } else {
                        // User actually opened the game — clear cooldown and proceed
                        flog("COOLDOWN cleared (user opened game) nice=%s pid=%d",nice,app_pid);
                        unlink(MODDIR "/pid/.cooldown");
                    }
                } else {
                    // Cooldown expired
                    unlink(MODDIR "/pid/.cooldown");
                }
            } else {
                unlink(MODDIR "/pid/.cooldown");
            }
        } else {
            unlink(MODDIR "/pid/.cooldown");
        }
    }
    
    // --- Normal companion logic (same as v2.9.5.2) ---
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

    unsigned char st=1; xwrite(client,&st,1);

    // Mount immediately (zero delay)
    pthread_mutex_lock(&g_lock);
    bool cpu_inc=false,hide_inc=false,hide_inc_pro=false;
    if(cpu_t){ if(!g_mounted) g_mounted=do_global_mount(); if(g_mounted){g_count++;cpu_inc=true;} }
    if(hide_basic){ if(!g_hide_on) g_hide_on=do_hide_mount_basic(); if(g_hide_on){g_hide_count++;hide_inc=true;} }
    if(hide_pro){ if(!g_hide_on_pro) g_hide_on_pro=do_hide_mount_pro(); if(g_hide_on_pro){g_hide_count_pro++;hide_inc_pro=true;} }
    pthread_mutex_unlock(&g_lock);
    flog("MOUNTED cpu=%d hide=%d hide_pro=%d nice=%s pid=%d",cpu_inc,hide_inc,hide_inc_pro,nice,app_pid);

    // Monitor for death (inotify only, socket EOF ignored)
    int inotify_fd=-1;
    if(app_pid>0){
        inotify_fd=inotify_init();
        if(inotify_fd>=0){
            char pp[64]; snprintf(pp,sizeof(pp),"/proc/%d",app_pid);
            if(inotify_add_watch(inotify_fd,pp,IN_DELETE_SELF)<0){close(inotify_fd);inotify_fd=-1;}
        }
    }
    
    bool socket_eof=false;
    bool app_died=false;
    while(!app_died){
        struct pollfd fds[2]; int nfds=0;
        if(!socket_eof){ fds[nfds].fd=client; fds[nfds].events=POLLIN; fds[nfds].revents=0; nfds++; }
        if(inotify_fd>=0){ fds[nfds].fd=inotify_fd; fds[nfds].events=POLLIN; fds[nfds].revents=0; nfds++; }
        if(nfds==0){ app_died=true; break; }
        int ret=poll(fds,nfds,-1);
        if(ret<0){if(errno==EINTR)continue;break;}
        if(!socket_eof && (fds[0].revents&(POLLIN|POLLHUP|POLLERR))){
            char buf[8]; ssize_t k=read(client,buf,sizeof(buf));
            if(k<=0){ socket_eof=true; flog("SOCKET-EOF (dlclose) nice=%s",nice); }
        }
        if(!app_died&&inotify_fd>=0&&(fds[socket_eof?0:1].revents&POLLIN)){
            char ev_buf[4096]; ssize_t len=read(inotify_fd,ev_buf,sizeof(ev_buf));
            if(len>0){
                flog("APP-DIED pid=%d (inotify) nice=%s",app_pid,nice);
                app_died=true;
            }
        }
    }

    if(inotify_fd>=0) close(inotify_fd);

    // Write cooldown file when game exits (10 seconds from now)
    if(cpu_inc || hide_inc || hide_inc_pro){
        int cdfd = open(MODDIR "/pid/.cooldown", O_CREAT|O_WRONLY|O_TRUNC, 0644);
        if(cdfd>=0){
            char ts[32]; snprintf(ts,sizeof(ts),"%lld",(long long)(time(nullptr)+10));
            write(cdfd,ts,strlen(ts));
            close(cdfd);
            flog("COOLDOWN set 10s for nice=%s",nice);
        }
    }

    // Umount
    pthread_mutex_lock(&g_lock);
    if(cpu_inc  && --g_count==0      && g_mounted){ do_global_umount(); g_mounted=false; }
    if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount_basic(); g_hide_on=false; }
    if(hide_inc_pro && --g_hide_count_pro==0 && g_hide_on_pro){ do_hide_umount_pro(); g_hide_on_pro=false; }
    pthread_mutex_unlock(&g_lock);
    flog("UMOUNT on death cpu=%d hide=%d hide_pro=%d (nice=%s)",cpu_inc,hide_inc,hide_inc_pro,nice);
}'''

c = c[:idx_handler] + new_handler + '\n' + c[idx_class:]

with open('zygisk/zygisk_cpuinfo.cpp', 'w') as f:
    f.write(c)

# Verify
checks = [
    ('COOLDOWN skip (background restart)', 'cooldown skip'),
    ('COOLDOWN cleared (user opened game)', 'cooldown clear on fg'),
    ('COOLDOWN set 10s for nice=', 'cooldown write on exit'),
    ('MOUNTED cpu=', 'immediate mount'),
    ('SOCKET-EOF (dlclose)', 'socket EOF ignored'),
    ('APP-DIED pid=', 'inotify death'),
    ('MODDIR "/pid/.cooldown"', 'cooldown path uses MODDIR'),
    ('kill(app_pid, SIGKILL)', 'kill background process'),
    ('usleep(1500000)', '1.5s wait for oom stable'),
]
all_ok = True
for pat, name in checks:
    ok = pat in c
    if not ok: all_ok = False
    print(f"  {'OK' if ok else 'MISSING'}: {name}")

print(f"File: {len(c)} bytes")
print(f"ALL CHECKS PASSED: {all_ok}")
