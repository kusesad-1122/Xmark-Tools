/*
 * zygisk_cpuinfo.cpp — XinmaskPlus CPU 伪装 Zygisk 模块
 * 署名: 苦涩or苳季
 * v2.9.4: inotify-only死亡, socket EOF忽略 监控线程死亡检测, socket EOF不算死亡, 过滤后台短暂拉起进程
 */
#define _GNU_SOURCE
#include <jni.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <dirent.h>
#include "zygisk.hpp"
#include "cpuinfo_presets.h"
using zygisk::Api; using zygisk::AppSpecializeArgs; using zygisk::ServerSpecializeArgs;
extern "C" int  __cxa_guard_acquire(long long *g){return !*((volatile char*)g);}
extern "C" void __cxa_guard_release(long long *g){*((volatile char*)g)=1;}
extern "C" void __cxa_guard_abort(long long *g){(void)g;}

#define MODDIR     "/data/adb/modules/xinmaskplus"
#define GAMES_F    MODDIR "/pid/cpu_games.txt"
#define PROFILE_F  MODDIR "/pid/cpu_profile"
#define STATE_DIR  MODDIR "/running_state"
#define ACTIVE_SRC STATE_DIR "/.internal_cpu"
#define DATADIR    "/data/adb/xinmaskplus"
#define LOGDIR     DATADIR "/log"
#define LOGFILE    LOGDIR "/cpu_zygisk.log"
#define HIDE_GAMES_F MODDIR "/pid/hide_games.txt"
#define ANTIMARK_F   MODDIR "/pid/anti_mark_off"
#define PERSIST_DIR  "/mnt/vendor/persist"
#define STORAGE_DIR  "/storage/emulated/0"
#define DOWNLOAD_DIR "/storage/emulated/0/Download"
#define EMPTY_DIR    "/data/local/tmp/xmp_empty_dir"
#define EMPTY_FILE   "/data/local/tmp/xmp_empty_file"

static bool xwrite(int fd,const void*b,size_t n){const char*p=(const char*)b;while(n){ssize_t k=write(fd,p,n);if(k<=0){if(k<0&&errno==EINTR)continue;return false;}p+=k;n-=(size_t)k;}return true;}
static bool xread(int fd,void*b,size_t n){char*p=(char*)b;while(n){ssize_t k=read(fd,p,n);if(k<=0){if(k<0&&errno==EINTR)continue;return false;}p+=k;n-=(size_t)k;}return true;}
static void flog(const char*fmt,...){
    mkdir(DATADIR,0755); mkdir(LOGDIR,0755);
    int fd=open(LOGFILE,O_WRONLY|O_CREAT|O_APPEND,0644); if(fd<0) return;
    char ts[32]; time_t t=time(0); struct tm tm; localtime_r(&t,&tm); strftime(ts,sizeof(ts),"%m-%d %H:%M:%S",&tm);
    char line[400]; int n=snprintf(line,sizeof(line),"[%s] ",ts);
    va_list ap; va_start(ap,fmt); n+=vsnprintf(line+n,sizeof(line)-n,fmt,ap); va_end(ap);
    if(n>0&&n<(int)sizeof(line)-1){line[n++]='\n';(void)!write(fd,line,(size_t)n);} close(fd);
}
static void str_trim(char*s){char*e=s+strlen(s);while(e>s&&(e[-1]=='\n'||e[-1]=='\r'||e[-1]==' '||e[-1]=='\t'))*--e=0;}
static bool name_in_file(const char*path,const char*nice){
    FILE*f=fopen(path,"r"); if(!f)return false; char line[256]; bool hit=false;
    while(fgets(line,sizeof(line),f)){char*p=line;while(*p==' '||*p=='\t')p++;str_trim(p);if(!*p)continue;size_t L=strlen(p);
        if(strcmp(nice,p)==0){hit=true;break;} if(strncmp(nice,p,L)==0&&nice[L]==':'){hit=true;break;}}
    fclose(f); return hit;
}
static bool name_in_games(const char*nice){ return name_in_file(GAMES_F,nice); }
static bool decide_target(const char*nice){
    if(access(MODDIR "/pid/cpu_spoof",F_OK)==0)return name_in_games(nice);
    return false;
}
static int pick_preset(void){
    char prof[64]="9000s"; int fd=open(PROFILE_F,O_RDONLY);
    if(fd>=0){char b[64]={0};ssize_t r=read(fd,b,sizeof(b)-1);close(fd);
        if(r>0){b[r]=0;char*s=b;while(*s==' '||*s=='\n'||*s=='\t'||*s=='\r')s++;str_trim(s);if(*s){strncpy(prof,s,sizeof(prof)-1);prof[sizeof(prof)-1]=0;}}}
    for(int i=0;i<CPU_PRESET_COUNT;i++)if(strcmp(prof,CPU_PRESETS[i].name)==0)return i;
    for(int i=0;i<CPU_PRESET_COUNT;i++)if(strcmp("9000s",CPU_PRESETS[i].name)==0)return i;
    return 0;
}
static bool materialize_preset(int idx){
    mkdir(STATE_DIR,0755); int fd=open(ACTIVE_SRC,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd<0)return false;
    bool ok=xwrite(fd,CPU_PRESETS[idx].data,CPU_PRESETS[idx].len); close(fd); return ok;
}
static bool do_global_mount(void){
    int idx=pick_preset(); char src[256];
    if(materialize_preset(idx)){strncpy(src,ACTIVE_SRC,sizeof(src)-1);src[sizeof(src)-1]=0;}
    else{snprintf(src,sizeof(src),MODDIR "/cpuinfo_%s",CPU_PRESETS[idx].name);
         if(access(src,R_OK)!=0){flog("FAIL errno=%d",errno);return false;}}
    umount2("/proc/cpuinfo",MNT_DETACH);
    int r=mount(src,"/proc/cpuinfo",nullptr,MS_BIND,nullptr);
    flog("%s profile=%s",r==0?"MOUNT-OK":"MOUNT-FAIL",CPU_PRESETS[idx].name);
    return r==0;
}
static void do_global_umount(void){
    umount2("/proc/cpuinfo",MNT_DETACH);
    flog("UMOUNT /proc/cpuinfo");
}
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int  g_count   = 0;
static bool g_mounted = false;

static const char* PERSIST_KEEP[] = {
  "rfs","hlos_rfs","sensors","wlan_mac.bin","WCNSS_qcom_wlan_nv.bin","mac.txt",
  "wifi","wlan","bluetooth","bt_firmware","bdaddr","modem","mdm","dpm","audio",
  "factory","nvdata","nvram","nvcfg","md_udc", nullptr };
static const char* PRO_KEEP[] = {
  "Ringtones","Recordings","Podcasts","Pictures","Notifications","My Documents",
  "Music","Movies","Documents","Download","DCIM","ColorOS","Browser","backups",
  "Audiobooks","Android","Alarms","7399",".SLOGAN",".lutThumbnail", nullptr };
static const char* DOWNLOAD_KEEP[] = {
  ".7934039a",".csj","appshare","com.tencent.game","netease","Operit","QQ",
  "UCDownloads","update",".exmu-cfg1.data", nullptr };
static bool in_keep(const char**lst,const char*name){ for(int i=0;lst[i];i++) if(strcmp(lst[i],name)==0) return true; return false; }
// Separate feature control:
//   /pid/anti_mark  exists -> basic persist hide ON
//   /pid/hide_storage exists -> pro storage+download hide ON
//   Default (no files): both OFF
static bool hide_decide_basic(const char*nice){
    if(access(MODDIR "/pid/anti_mark",F_OK)!=0) return false;
    return name_in_file(HIDE_GAMES_F,nice);
}
static bool hide_decide_pro(const char*nice){
    if(access(MODDIR "/pid/hide_storage",F_OK)!=0) return false;
    return name_in_file(HIDE_GAMES_F,nice);
}
static void ensure_empty(void){ mkdir(EMPTY_DIR,0755); int fd=open(EMPTY_FILE,O_WRONLY|O_CREAT,0644); if(fd>=0)close(fd); }
static void force_umount(const char*path){
    for(int i=0;i<16;i++){ if(umount2(path,MNT_DETACH)!=0) break; }
}
static void mnt_unescape(char*str){
    char*o=str;
    for(char*q=str;*q;){
        if(q[0]=='\\'&&q[1]>='0'&&q[1]<='3'&&q[2]>='0'&&q[2]<='7'&&q[3]>='0'&&q[3]<='7'){
            *o++=(char)(((q[1]-'0')<<6)|((q[2]-'0')<<3)|(q[3]-'0')); q+=4;
        } else *o++=*q++;
    }
    *o=0;
}
static void umount_under(const char*base,const char**keep){
    size_t blen=strlen(base);
    for(int pass=0;pass<8;pass++){
        int fd=open("/proc/self/mountinfo",O_RDONLY); if(fd<0) return;
        static char buf[262144]; size_t tot=0; ssize_t k;
        while(tot<sizeof(buf)-1 && (k=read(fd,buf+tot,sizeof(buf)-1-tot))>0) tot+=(size_t)k;
        close(fd); buf[tot]=0;
        static char found[256][512]; int nf=0; char*line=buf;
        while(line&&*line&&nf<256){
            char*nl=strchr(line,'\n'); if(nl)*nl=0;
            char*pp=line; int field=0; char*mp=nullptr;
            for(;;){ if(field==4){mp=pp;break;} char*sp=strchr(pp,' '); if(!sp)break; pp=sp+1; field++; }
            if(mp){ char*sp=strchr(mp,' '); if(sp)*sp=0; mnt_unescape(mp); }
            if(mp&&strncmp(mp,base,blen)==0&&mp[blen]=='/'){
                const char*child=mp+blen+1; char cname[256]; size_t i=0;
                while(child[i]&&child[i]!='/'&&i<sizeof(cname)-1){cname[i]=child[i];i++;} cname[i]=0;
                if(!in_keep(keep,cname)){ strncpy(found[nf],mp,sizeof(found[nf])-1); found[nf][sizeof(found[nf])-1]=0; nf++; }
            }
            if(!nl)break; line=nl+1;
        }
        if(nf==0) break;
        for(int i=nf-1;i>=0;i--) force_umount(found[i]);
    }
}
static void hide_bind_dir(const char*base,const char**keep){
    DIR*d=opendir(base); if(!d)return; struct dirent*e;
    while((e=readdir(d))){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        if(in_keep(keep,e->d_name))continue;
        char path[600]; snprintf(path,sizeof(path),"%s/%s",base,e->d_name);
        force_umount(path);
        struct stat st; if(lstat(path,&st)!=0)continue;
        if(S_ISDIR(st.st_mode)) mount(EMPTY_DIR,path,nullptr,MS_BIND,nullptr);
        else                    mount(EMPTY_FILE,path,nullptr,MS_BIND,nullptr);
    }
    closedir(d);
}
static void hide_unbind_dir(const char*base,const char**keep){
    DIR*d=opendir(base); if(!d)return; struct dirent*e;
    while((e=readdir(d))){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        if(in_keep(keep,e->d_name))continue;
        char path[600]; snprintf(path,sizeof(path),"%s/%s",base,e->d_name);
        force_umount(path);
    }
    closedir(d);
}
// Separate hide mount: basic=persist, pro=storage+download
static bool do_hide_mount_basic(void){
    ensure_empty();
    if(access(PERSIST_DIR,F_OK)==0) hide_bind_dir(PERSIST_DIR,PERSIST_KEEP);
    flog("HIDE-MOUNT persist");
    return true;
}
static bool do_hide_mount_pro(void){
    ensure_empty();
    if(access(STORAGE_DIR,F_OK)==0) hide_bind_dir(STORAGE_DIR,PRO_KEEP);
    if(access(DOWNLOAD_DIR,F_OK)==0) hide_bind_dir(DOWNLOAD_DIR,DOWNLOAD_KEEP);
    flog("HIDE-MOUNT pro+download");
    return true;
}
static void do_hide_umount_basic(void){
    if(access(PERSIST_DIR,F_OK)==0){ hide_unbind_dir(PERSIST_DIR,PERSIST_KEEP);   umount_under(PERSIST_DIR,PERSIST_KEEP); }
    flog("HIDE-UMOUNT persist");
}
static void do_hide_umount_pro(void){
    if(access(STORAGE_DIR,F_OK)==0){ hide_unbind_dir(STORAGE_DIR,PRO_KEEP);       umount_under(STORAGE_DIR,PRO_KEEP); }
    if(access(DOWNLOAD_DIR,F_OK)==0){ hide_unbind_dir(DOWNLOAD_DIR,DOWNLOAD_KEEP); umount_under(DOWNLOAD_DIR,DOWNLOAD_KEEP); }
    flog("HIDE-UMOUNT pro+download");
}
static int  g_hide_count = 0;
static bool g_hide_on    = false;
static int  g_hide_count_pro = 0;
static bool g_hide_on_pro    = false;

// v2.8: 子进程完全不计入CPU引用计数, 只有主进程(不带:)参与挂载+计数
// v2.9.3: file-based counting + monitoring thread (CPU伪装1.7 approach)
// instance_count file tracks active processes
// Monitoring thread polls /proc/<pid> via stat() for real death detection
// Socket EOF from DLCLOSE is ignored - only thread-confirmed death counts
static void* monitor_thread(void* arg){
    int pid = (int)(long)arg;
    char path[64];
    snprintf(path,sizeof(path),"/proc/%d",pid);
    // Poll /proc/<pid> via access() every 500ms until the process dies
    // stat() with NULL buffer causes EFAULT - use access(F_OK) instead
    while(access(path,F_OK)==0){
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
    bool hide_basic = hide_decide_basic(nice);
    bool hide_pro = hide_decide_pro(nice);
    bool hide_t = hide_basic || hide_pro;
    if(!cpu_t && !hide_t){ unsigned char z=0; xwrite(client,&z,1); return; }
    if(!is_main){ unsigned char z=0; xwrite(client,&z,1); return; }

    // Mount immediately (no delay)
    bool cpu_inc=false, hide_inc=false, hide_inc_pro=false;
    pthread_mutex_lock(&g_lock);
    if(cpu_t){
        if(!g_mounted) g_mounted=do_global_mount();
        if(g_mounted){g_count++;cpu_inc=true;}
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
        if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount_basic(); g_hide_on=false; }
    if(hide_inc_pro && --g_hide_count_pro==0 && g_hide_on_pro){ do_hide_umount_pro(); g_hide_on_pro=false; }
        pthread_mutex_unlock(&g_lock);
        if(inotify_fd>=0) close(inotify_fd);
        return;
    }

    // Death detection: ONLY inotify IN_DELETE_SELF counts as death
    // Socket EOF from ZygiskNext DLCLOSE is NOT real death - IGNORE it
    bool socket_eof=false;
    bool app_died=false;
    while(!app_died){
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
    if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount_basic(); g_hide_on=false; }
    pthread_mutex_unlock(&g_lock);
    flog("UMOUNT on death cpu=%d hide=%d (nice=%s)",cpu_inc,hide_inc,nice);
}
class CpuSpoofModule:public zygisk::ModuleBase{
public:
    void onLoad(Api*a,JNIEnv*e)override{api=a;env=e;}
    void preAppSpecialize(AppSpecializeArgs*args)override{
        if(args->is_child_zygote&&*args->is_child_zygote)return;
        api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        if(!args->nice_name)return;
        const char*nice=env->GetStringUTFChars(args->nice_name,nullptr); if(!nice)return;
        int fd=api->connectCompanion();
        if(fd>=0){
            int nlen=(int)strlen(nice);
            int my_pid=getpid();
            unsigned char st=0;
            if(xwrite(fd,&nlen,sizeof(nlen))&&xwrite(fd,nice,(size_t)nlen)&&xwrite(fd,&my_pid,sizeof(my_pid))&&xread(fd,&st,1)){
                if(st==1){
                    env->ReleaseStringUTFChars(args->nice_name,nice);
                    return;
                }
            }
            close(fd);
        }
        env->ReleaseStringUTFChars(args->nice_name,nice);
    }
private: Api*api=nullptr; JNIEnv*env=nullptr;
};
REGISTER_ZYGISK_MODULE(CpuSpoofModule)
REGISTER_ZYGISK_COMPANION(companion_handler)