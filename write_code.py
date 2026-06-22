code = '''/*
 * zygisk_cpuinfo.cpp — XinmaskPlus CPU 伪装 Zygisk 模块
 * 署名: 苦涩or苳季
 * v2.9: 重构存活检测: 进程存活6s后才挂载, 消除误触发
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
#define CPUSPOOF_F   MODDIR "/pid/cpu_spoof"
#define PERSIST_DIR  "/mnt/vendor/persist"
#define STORAGE_DIR  "/storage/emulated/0"
#define DOWNLOAD_DIR "/storage/emulated/0/Download"
#define EMPTY_DIR    "/data/local/tmp/xmp_empty_dir"
#define EMPTY_FILE   "/data/local/tmp/xmp_empty_file"
'''
with open('/tmp/xmt/zygisk/zygisk_cpuinfo.cpp', 'w') as f:
    f.write(code)
print('P1 written', len(code))
