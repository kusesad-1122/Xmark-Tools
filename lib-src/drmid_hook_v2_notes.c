/* drmid_hook.c v2 增量补丁 —— 基于 libdrmid.so（苳季 DongJi_5.2.0）静态逆向结论
 * 只列【需要合并进现有 drmid_hook.c】的增量；hook 引擎与主流程沿用现有实现。
 *
 * 逆向结论（libdrmid.so, aarch64, 240KB, dynsym 仅导出 A64HookFunction/V）：
 *
 * 1. extra_data：32 字节随机数 → 64 字符【大写】HEX 字符串（查表 "0123456789ABCDEF"），
 *    进程启动时生成一次存全局槽；SHA256_Final 替身在真正 Final 前先调
 *    原始 SHA256_Update(ctx, extra, len)（替身 @0x111fc，反汇编确认：
 *    csinc/csel 处理 libc++ std::string SSO，blr x13 调原始 Update，br x2 尾调 Final）。
 *    —— 与我们现有 salt 注入同构，唯一行为差异是【大写】hex（我们目前小写）。
 *
 * 2. 多实例 hook：遍历 /proc/self/maps 收集所有 libcrypto.so 实例逐个 hook
 *    （日志 "found %zu libcrypto.so instance(s)" / "done hooked %zu"，
 *    hook 成功计数存 .bss 0x54818，每项 {orig_addr, trampoline}）。
 *    我们当前是"找到第一个就停"，多 libcrypto ROM 上会漏挂。
 *
 * 3. 日志 "A64HookFunction SHA256_Update @ %p" 是误导：安装函数 @0x116e4
 *    只对 Final 定位+patch，Update 符号查找后仅保存原始指针 —— Update 本体未 patch，
 *    与我们"只 patch Final"的设计一致，无需改动。
 *
 * 4. A64HookDrmModule 不是导出符号（dynsym 无），只是日志 tag 串。
 *
 * 5. 它的 hook 槽池 = .bss 0x48000 起 55KB RWX（constructor mprotect），
 *    每槽 200B；patch 长度 0x32=50B（备份 50B 原指令）——实现细节，不必对齐。
 */

/* ── 增量1：hex 大小写 ──
 * 现有 drmid_hook.c：
 *   static const char HEX[] = "0123456789abcdef";
 * 改为（大写/小写进 SHA256 的输入不同；服务端聚类按派生 ID 字节比对，
 * 用大写可与 DongJi 系改机设备的指纹特征混在同一空间，提升混淆度）：
 */
static const char HEX[] = "0123456789ABCDEF";

/* ── 增量2：多实例全量 hook ──
 * 把现有"扫 /proc/self/maps 找第一个 libcrypto → 定位符号 → patch"
 * 的单次逻辑抽成 patch_final_in_instance(base)，主流程改为循环全量：
 */
typedef struct { unsigned long lo, hi; } crypto_inst_t;

static int collect_crypto_instances(crypto_inst_t *out, int max) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < max) {
        unsigned long lo, hi;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if (perms[2] != 'x') continue;                 /* 只要可执行段 */
        if (!strstr(line, "libcrypto.so")) continue;
        if (n && out[n - 1].hi == lo) {                /* 同实例连续段合并 */
            out[n - 1].hi = hi;
            continue;
        }
        out[n].lo = lo;
        out[n].hi = hi;
        n++;
    }
    fclose(f);
    return n;
}

/* 主流程替换：
 *   crypto_inst_t inst[8];
 *   int n = collect_crypto_instances(inst, 8);
 *   LOGI("found %d libcrypto.so instance(s)", n);
 *   int hooked = 0;
 *   for (int i = 0; i < n; i++) {
 *       // 现有 PT_DYNAMIC(DT_SYMTAB/DT_STRTAB) 符号定位 + patch_final 逻辑
 *       // 以 inst[i].lo 为解析基址（现有代码已按基址解析，抽出为函数即可）
 *       if (patch_final_in_instance(inst[i].lo)) hooked++;
 *   }
 *   LOGI("done hooked %d libcrypto.so instance(s)", hooked);
 *   // hooked == 0 时保留现有"no libcrypto.so found"日志
 */

/* ── 不采纳项（逆向结论支撑）──
 * - 双 hook SHA256_Update：它的替身实际用原始指针，Update 未被 patch，与我们同构。
 * - extra_data 持久化：DongJi 每次启动重新随机 —— Widevine 密钥库每次重启失效重拉，
 *   我们同样每次随机，行为一致；若要"授权缓存存活"反而是负优化（更容易被聚类）。
 * - sepolicy 全开 allow hal_drm_widevine * * *：我们的最小集更安全，保持。
 */
