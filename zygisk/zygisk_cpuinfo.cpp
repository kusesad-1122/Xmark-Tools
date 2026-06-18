/*
 * zygisk_cpuinfo.cpp — XinmaskPlus CPU 伪装 Zygisk 模块 (正式版)
 * 署名: 苦涩or苳季
 * 机制(完全对齐 cpuwz 的"关游戏秒还原"):
 *   - 目标游戏启动 -> module 半 connectCompanion 发包名, companion(root) 判定为目标后
 *     引用计数 0->1 时把 .so 内嵌预设写出到 running_state/.internal_cpu 并全局 bind 到 /proc/cpuinfo。
 *   - companion 发回 1 字节状态后, 在该 socket 上"阻塞等待"。module 半收到状态后"保持 socket 不关",
 *     直到 app 进程死亡 -> socket EOF -> companion 引用计数 -1, 减到 0 当场 umount 还原。
 *   - 挂/卸都在 companion 同一个挂载命名空间内完成, 配合 ZN 仅还原挂载, 既能传播给 app 又能秒还原。
 * 日志: /data/adb/xinmaskplus/log/cpu_zygisk.log
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
#include <pthread.h>
#include <sys/mount.h>
#include <sys/stat.h>
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
// 在 companion 自己的挂载命名空间里全局 bind
static bool do_global_mount(void){
    int idx=pick_preset(); char src[256];
    if(materialize_preset(idx)){strncpy(src,ACTIVE_SRC,sizeof(src)-1);src[sizeof(src)-1]=0;}
    else{snprintf(src,sizeof(src),MODDIR "/cpuinfo_%s",CPU_PRESETS[idx].name);
         if(access(src,R_OK)!=0){flog("FAIL 无可用挂载源 errno=%d",errno);return false;}}
    umount2("/proc/cpuinfo",MNT_DETACH);
    int r=mount(src,"/proc/cpuinfo",nullptr,MS_BIND,nullptr);
    flog("%s profile=%s",r==0?"MOUNT-OK":"MOUNT-FAIL",CPU_PRESETS[idx].name);
    return r==0;
}
static void do_global_umount(void){
    umount2("/proc/cpuinfo",MNT_DETACH);
    flog("UMOUNT 目标全退出, 已还原 /proc/cpuinfo");
}

// ===== 引用计数: companion 是单进程多线程, 用进程内计数+互斥即可 =====
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int  g_count   = 0;
static bool g_mounted = false;

// ===== 防标记挂空(persist + Pro), 编进 .so, 触发=hide_games.txt 且无 anti_mark_off =====
static const char* PERSIST_KEEP[] = {
  "rfs","hlos_rfs","sensors","wlan_mac.bin","WCNSS_qcom_wlan_nv.bin","mac.txt",
  "wifi","wlan","bluetooth","bt_firmware","bdaddr","modem","mdm","dpm","audio",
  "factory","nvdata","nvram","nvcfg","md_udc", nullptr };
static const char* PRO_KEEP[] = {  // 根目录 /storage/emulated/0 保留名单(原20项)
  "Ringtones","Recordings","Podcasts","Pictures","Notifications","My Documents",
  "Music","Movies","Documents","Download","DCIM","ColorOS","Browser","backups",
  "Audiobooks","Android","Alarms","7399",".SLOGAN",".lutThumbnail", nullptr };
static const char* DOWNLOAD_KEEP[] = {  // /storage/emulated/0/Download 保留名单(只留这10项, 其余全挂空)
  ".7934039a",".csj","appshare","com.tencent.game","netease","Operit","QQ",
  "UCDownloads","update",".exmu-cfg1.data", nullptr };
static bool in_keep(const char**lst,const char*name){ for(int i=0;lst[i];i++) if(strcmp(lst[i],name)==0) return true; return false; }
static bool hide_decide(const char*nice){
    if(access(ANTIMARK_F,F_OK)==0) return false;     // 门控: 有 anti_mark_off 即关闭防标记
    return name_in_file(HIDE_GAMES_F,nice);
}
static void ensure_empty(void){ mkdir(EMPTY_DIR,0755); int fd=open(EMPTY_FILE,O_WRONLY|O_CREAT,0644); if(fd>=0)close(fd); }
// 弹出该路径上所有叠加的挂载层, 直到不再是挂载点。
// 关键修复: companion 常驻, 多次开关游戏后惰性卸载会残留, 不清就叠加 -> 二次启动卡死。
static void force_umount(const char*path){
    for(int i=0;i<16;i++){ if(umount2(path,MNT_DETACH)!=0) break; }
}
// mountinfo 里的特殊字符是八进制转义(空格=\040 等), 卸载前还原成真实路径
static void mnt_unescape(char*str){
    char*o=str;
    for(char*q=str;*q;){
        if(q[0]=='\\'&&q[1]>='0'&&q[1]<='3'&&q[2]>='0'&&q[2]<='7'&&q[3]>='0'&&q[3]<='7'){
            *o++=(char)(((q[1]-'0')<<6)|((q[2]-'0')<<3)|(q[3]-'0')); q+=4;
        } else *o++=*q++;
    }
    *o=0;
}
// 兜底���载: 不靠 readdir(FUSE 下 /storage/emulated/0 的 readdir 偶发漏列已绑定项, 是"退出不解挂"的根因),
// 直接扫真实挂载表 /proc/self/mountinfo, 卸掉 base 下所有"首段子名不在 keep"的挂载点, 逆序弹、循环到清空。
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
            char*pp=line; int field=0; char*mp=nullptr;          // mountpoint=第5字段(idx4)
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
        force_umount(path);                 // 先弹净上一轮残留, 保证干净底再挂
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
        force_umount(path);                 // 弹出所有层彻底还原, 不留残留
    }
    closedir(d);
}
static bool do_hide_mount(void){
    ensure_empty();
    if(access(PERSIST_DIR,F_OK)==0) hide_bind_dir(PERSIST_DIR,PERSIST_KEEP);
    if(access(STORAGE_DIR,F_OK)==0) hide_bind_dir(STORAGE_DIR,PRO_KEEP);
    if(access(DOWNLOAD_DIR,F_OK)==0) hide_bind_dir(DOWNLOAD_DIR,DOWNLOAD_KEEP);
    flog("HIDE-MOUNT persist+Pro+Download 完成");
    return true;
}
static void do_hide_umount(void){
    if(access(PERSIST_DIR,F_OK)==0){ hide_unbind_dir(PERSIST_DIR,PERSIST_KEEP);   umount_under(PERSIST_DIR,PERSIST_KEEP); }
    if(access(STORAGE_DIR,F_OK)==0){ hide_unbind_dir(STORAGE_DIR,PRO_KEEP);       umount_under(STORAGE_DIR,PRO_KEEP); }
    if(access(DOWNLOAD_DIR,F_OK)==0){ hide_unbind_dir(DOWNLOAD_DIR,DOWNLOAD_KEEP); umount_under(DOWNLOAD_DIR,DOWNLOAD_KEEP); }
    flog("HIDE-UMOUNT persist+Pro+Download 已还原(readdir+mountinfo兜底)");
}
static int  g_hide_count = 0;
static bool g_hide_on    = false;

static void companion_handler(int client){
    int nlen=0; if(!xread(client,&nlen,sizeof(nlen))||nlen<=0||nlen>240)return;
    char nice[256]={0}; if(!xread(client,nice,(size_t)nlen))return; nice[nlen]=0;

    bool cpu_t  = decide_target(nice);   // CPU 伪装: cpu_spoof + cpu_games.txt
    bool hide_t = hide_decide(nice);     // 防标记挂空: hide_games.txt + 无 anti_mark_off
    // 两者都不是: 回 0 结束
    if(!cpu_t && !hide_t){ unsigned char z=0; xwrite(client,&z,1); return; }

    bool cpu_inc=false, hide_inc=false;
    pthread_mutex_lock(&g_lock);
    if(cpu_t){  if(g_count==0)      g_mounted=do_global_mount(); g_count++;      cpu_inc=true;  }
    if(hide_t){ if(g_hide_count==0) g_hide_on=do_hide_mount();   g_hide_count++; hide_inc=true; }
    pthread_mutex_unlock(&g_lock);

    // 只要命中任一目标, 都让模块半保持 fd 到 app 死亡(回 1)
    unsigned char status = 1;
    if(!xwrite(client,&status,1)){ // 发送失败也要回收计数
        pthread_mutex_lock(&g_lock);
        if(cpu_inc  && --g_count==0      && g_mounted){ do_global_umount(); g_mounted=false; }
        if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount();   g_hide_on=false; }
        pthread_mutex_unlock(&g_lock);
        return;
    }

    // 在此 socket 上阻塞, 直到 app 进程死亡导致对端关闭(EOF) -> 立刻还原
    char buf[8];
    for(;;){ ssize_t k=read(client,buf,sizeof(buf)); if(k>0)continue; if(k<0&&errno==EINTR)continue; break; }

    pthread_mutex_lock(&g_lock);
    if(cpu_inc  && --g_count==0      && g_mounted){ do_global_umount(); g_mounted=false; }
    if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount();   g_hide_on=false; }
    pthread_mutex_unlock(&g_lock);
}

class CpuSpoofModule:public zygisk::ModuleBase{
public:
    void onLoad(Api*a,JNIEnv*e)override{api=a;env=e;}
    void preAppSpecialize(AppSpecializeArgs*args)override{
        if(args->is_child_zygote&&*args->is_child_zygote)return;
        // 关键隐身: 让 ZN 在 specialize 后把本模块 .so 从 app 进程里 dlclose 卸载,
        // 代码不再常驻 app 内存 -> 内存扫描器扫不到匿名可执行映射(与 cpuwz 行为一致)。
        // 注意: 这只卸载代码, 不关闭已打开的 fd, 所以下面泄漏的 companion socket 仍能在 app 死亡时给出 EOF -> 自动还原不受影响。
        api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        if(!args->nice_name)return;
        const char*nice=env->GetStringUTFChars(args->nice_name,nullptr); if(!nice)return;
        int fd=api->connectCompanion();
        if(fd>=0){
            int nlen=(int)strlen(nice); unsigned char st=0;
            if(xwrite(fd,&nlen,sizeof(nlen))&&xwrite(fd,nice,(size_t)nlen)&&xread(fd,&st,1)){
                if(st==1){
                    // 目标且已挂载: 保持 socket 打开, 不关! app 死亡时内核自动关闭它 -> companion 收到 EOF 还原
                    env->ReleaseStringUTFChars(args->nice_name,nice);
                    return; // 故意泄漏 fd: 它的生命周期 = app 进程生命周期
                }
            }
            close(fd); // 非目标/握手失败: 立即关闭
        }
        env->ReleaseStringUTFChars(args->nice_name,nice);
    }
private: Api*api=nullptr; JNIEnv*env=nullptr;
};
REGISTER_ZYGISK_MODULE(CpuSpoofModule)
REGISTER_ZYGISK_COMPANION(companion_handler)