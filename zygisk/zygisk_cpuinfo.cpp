/**
 * zygisk_cpuinfo.cpp — XinmaskPlus CPU 伪装 Zygisk 模块 (正式版)
 * 署名: 苦涩or苳季
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
