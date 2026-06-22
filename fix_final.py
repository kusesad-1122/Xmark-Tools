#!/usr/bin/env python3
import os
os.chdir('/tmp/xmt')

with open('zygisk/zygisk_cpuinfo.cpp', 'r') as f:
    content = f.read()

# Fix 1: hide_decide - check separate trigger files
# anti_mark file exists -> basic hide ON
# hide_storage file exists -> pro hide ON
# Default: both OFF (need to create file to enable)
old_hide_decide = '''static bool hide_decide(const char*nice){
    if(access(ANTIMARK_F,F_OK)==0) return false;
    return name_in_file(HIDE_GAMES_F,nice);
}'''

new_hide_decide = '''// Separate feature control:
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
}'''

content = content.replace(old_hide_decide, new_hide_decide, 1)

# Fix 2: do_hide_mount - separate basic and pro
old_do_hide_mount = '''static bool do_hide_mount(void){
    ensure_empty();
    if(access(PERSIST_DIR,F_OK)==0) hide_bind_dir(PERSIST_DIR,PERSIST_KEEP);
    if(access(STORAGE_DIR,F_OK)==0) hide_bind_dir(STORAGE_DIR,PRO_KEEP);
    if(access(DOWNLOAD_DIR,F_OK)==0) hide_bind_dir(DOWNLOAD_DIR,DOWNLOAD_KEEP);
    flog("HIDE-MOUNT persist+Pro+Download");
    return true;
}'''

new_do_hide_mount = '''// Separate hide mount: basic=persist, pro=storage+download
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
}'''

content = content.replace(old_do_hide_mount, new_do_hide_mount, 1)

# Fix 3: do_hide_umount - separate too
old_do_hide_umount = '''static void do_hide_umount(void){
    if(access(PERSIST_DIR,F_OK)==0){ hide_unbind_dir(PERSIST_DIR,PERSIST_KEEP);   umount_under(PERSIST_DIR,PERSIST_KEEP); }
    if(access(STORAGE_DIR,F_OK)==0){ hide_unbind_dir(STORAGE_DIR,PRO_KEEP);       umount_under(STORAGE_DIR,PRO_KEEP); }
    if(access(DOWNLOAD_DIR,F_OK)==0){ hide_unbind_dir(DOWNLOAD_DIR,DOWNLOAD_KEEP); umount_under(DOWNLOAD_DIR,DOWNLOAD_KEEP); }
    flog("HIDE-UMOUNT persist+Pro+Download");
}'''

new_do_hide_umount = '''static void do_hide_umount_basic(void){
    if(access(PERSIST_DIR,F_OK)==0){ hide_unbind_dir(PERSIST_DIR,PERSIST_KEEP);   umount_under(PERSIST_DIR,PERSIST_KEEP); }
    flog("HIDE-UMOUNT persist");
}
static void do_hide_umount_pro(void){
    if(access(STORAGE_DIR,F_OK)==0){ hide_unbind_dir(STORAGE_DIR,PRO_KEEP);       umount_under(STORAGE_DIR,PRO_KEEP); }
    if(access(DOWNLOAD_DIR,F_OK)==0){ hide_unbind_dir(DOWNLOAD_DIR,DOWNLOAD_KEEP); umount_under(DOWNLOAD_DIR,DOWNLOAD_KEEP); }
    flog("HIDE-UMOUNT pro+download");
}'''

content = content.replace(old_do_hide_umount, new_do_hide_umount, 1)

# Fix 4: companion_handler - use separated hide + inotify-only death
# Find and replace the hide_t/hide_inc/hide_count section in companion_handler
old_hide_section = '''    bool hide_t = hide_decide(nice);'''
new_hide_section = '''    bool hide_basic = hide_decide_basic(nice);
    bool hide_pro = hide_decide_pro(nice);
    bool hide_t = hide_basic || hide_pro;'''

content = content.replace(old_hide_section, new_hide_section, 1)

# Fix 5: separate hide mount operations in companion_handler
old_mount_hide = '''    if(hide_t){
        if(!g_hide_on) g_hide_on=do_hide_mount();
        if(g_hide_on){g_hide_count++;hide_inc=true;}
    }'''
new_mount_hide = '''    if(hide_basic){
        if(!g_hide_on) g_hide_on=do_hide_mount_basic();
        if(g_hide_on){g_hide_count++;hide_inc=true;}
    }
    if(hide_pro){
        if(!g_hide_on_pro) g_hide_on_pro=do_hide_mount_pro();
        if(g_hide_on_pro){g_hide_count_pro++;hide_inc_pro=true;}
    }'''

content = content.replace(old_mount_hide, new_mount_hide, 1)

# Fix 6: g_hide_on / g_hide_count need pro variants
old_g_hide = '''static int g_hide_count = 0;
static bool g_hide_on = false;'''
new_g_hide = '''static int g_hide_count = 0;
static bool g_hide_on = false;
static int g_hide_count_pro = 0;
static bool g_hide_on_pro = false;'''

content = content.replace(old_g_hide, new_g_hide, 1)

# Fix 7: hide_inc in companion_handler - add pro variant
old_hide_inc = '''    bool cpu_inc=false, hide_inc=false;'''
new_hide_inc = '''    bool cpu_inc=false, hide_inc=false, hide_inc_pro=false;'''

content = content.replace(old_hide_inc, new_hide_inc, 1)

# Fix 8: cleanup section - add pro cleanup
old_cleanup_hide_death = '''    if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount(); g_hide_on=false; }'''
new_cleanup_hide_death = '''    if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount_basic(); g_hide_on=false; }
    if(hide_inc_pro && --g_hide_count_pro==0 && g_hide_on_pro){ do_hide_umount_pro(); g_hide_on_pro=false; }'''

content = content.replace(old_cleanup_hide_death, new_cleanup_hide_death, 1)

# Fix 9: same for write-fail undo section
old_undo_hide = '''        if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount(); g_hide_on=false; }'''
new_undo_hide = '''        if(hide_inc && --g_hide_count==0 && g_hide_on){ do_hide_umount_basic(); g_hide_on=false; }
        if(hide_inc_pro && --g_hide_count_pro==0 && g_hide_on_pro){ do_hide_umount_pro(); g_hide_on_pro=false; }'''

content = content.replace(old_undo_hide, new_undo_hide, 1)

# Fix 10: ANTIMARK_F no longer needed but keep for compatibility
# Remove #define ANTIMARK_F
# Actually keep it, just add new defines
old_defines = '''#define ANTIMARK_F   MODDIR "/pid/anti_mark_off"
#define CPUSPOOF_F   MODDIR "/pid/cpu_spoof"'''
new_defines = '''#define ANTIMARK_F   MODDIR "/pid/anti_mark_off"
#define CPUSPOOF_F   MODDIR "/pid/cpu_spoof"
#define HIDE_BASIC_F MODDIR "/pid/anti_mark"
#define HIDE_PRO_F   MODDIR "/pid/hide_storage"'''

content = content.replace(old_defines, new_defines, 1)

with open('zygisk/zygisk_cpuinfo.cpp', 'w') as f:
    f.write(content)

# Verify
checks = [
    ('hide_decide_basic', 'hide_decide_basic created'),
    ('hide_decide_pro', 'hide_decide_pro created'),
    ('do_hide_mount_basic', 'basic mount'),
    ('do_hide_mount_pro', 'pro mount'),
    ('do_hide_umount_basic', 'basic umount'),
    ('do_hide_umount_pro', 'pro umount'),
    ('HIDE_BASIC_F', 'basic define'),
    ('HIDE_PRO_F', 'pro define'),
]
for pattern, name in checks:
    if pattern in content:
        print(f"  OK: {name}")
    else:
        print(f"  MISSING: {name} - {pattern}")

print(f"\\nFile: {len(content)} bytes")