/*
 * drmid_hook.c  —  DRM 层防标记：libcrypto SHA256 随机盐注入 (aarch64)
 * ---------------------------------------------------------------------------
 * 原理
 *   Widevine L1 的 device-unique-id 与设备签名都依赖 libcrypto(BoringSSL) 的
 *   SHA256 链式计算。本库在进程内定位 libcrypto.so 的 SHA256_Update /
 *   SHA256_Final，并在 SHA256_Final 真正执行前，先对该 ctx 追加一次
 *   SHA256_Update(ctx, salt, 64)。服务端拿到的派生 ID 因此每次开机都不同、
 *   且彼此不关联，无法把这些设备聚类成"同一批改机设备"，交叉验证失效。
 *
 * 实现要点（均为本项目的设计选择）
 *   1. 只 patch SHA256_Final，不 patch SHA256_Update。
 *      注入盐需要调用"未被修改"的 SHA256_Update；直接用 ELF 解析出的原始
 *      地址调用 Update，既避开 Update 上的 trampoline 递归风险，也让
 *      patch 点从 2 个降到 1 个，失败面更小。
 *   2. 盐 = getrandom() 的 32 字节真随机 → hex 成 64 字符。
 *      不用 mt19937 一类可恢复状态的 PRNG：CLOCK_MONOTONIC 每次开机的
 *      初值高度可预测，不构成安全随机源；而32 字符 = 128 bit 盐，
 *      在派生 ID 的输入空间由设备属性决定的前提下，雪崩效应不够充分。
 *      64 个 hex 字符 = 256 bit 熵，且纯ASCII 可打印，喂进任何
 *      "把 ctx 当字节串处理"的实现都不会引入 NUL 截断问题。
 *   3. 符号定位走 PT_DYNAMIC(DT_SYMTAB/DT_STRTAB)，完全不依赖 section header。
 *      Android release so 常被 strip 掉 .symtab，只剩 .dynsym。
 *   4. 全链路静默失败：只 __android_log_print，绝不 abort/_exit。
 *      hook 失败仅使本层防标记失效，Widevine 自身照常工作（不会黑屏）。
 *
 * 安全性说明（为什么必须配 sepolicy.rule）
 *   trampoline 需要一块 RWX 匿名内存，在 enforcing 模式下需要目标进程的
 *   self:process execmem 权限；patch libcrypto 的代码页还需要对该 so 的
 *   file:file 写权限。这些由 sepolicy.rule 授予。若策略缺失，mprotect/mmap
 *   会失败，hook 静默跳过 —— 失败方向是"少一层保护"，而不是"设备坏掉"。
 *
 * 构建
 *   clang --target=aarch64-linux-android26 -fPIC -shared -O2 -fvisibility=hidden \
 *         -o libdrmid_hook.so drmid_hook.c -llog
 */

#define _GNU_SOURCE

#include <android/log.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define TAG "XmarkDrmId"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* 盐：32 字节熵 → 64 个 hex 字符（另加结尾 NUL，不计入长度） */
#define SALT_RAW_LEN 32
#define SALT_LEN     (SALT_RAW_LEN * 2)

/* 最多搬 16 条序言指令。搬迁后单条最多膨胀到 5 条(远跳)，4KB 足够。 */
#define MAX_RELOC_INSNS 16
#define TRAMP_SIZE      4096

typedef int  (*sha256_update_fn)(void *ctx, const void *data, size_t len);
typedef int  (*sha256_final_fn)(unsigned char *md, void *ctx);

/* ===================================================================== */
/* 1. AArch64 指令编码                                                   */
/* ===================================================================== */

static inline uint32_t enc_nop(void) { return 0xD503201Fu; }
/* BTI c —— 落地页桩。trampoline 是被 blr 间接进入的，开启 BTI 时必须打。 */
static inline uint32_t enc_bti_c(void) { return 0xD503245Fu; }

/* BR Xn / BLR Xn */
static inline uint32_t enc_br (unsigned rn) { return 0xD61F0000u | (rn << 5); }
static inline uint32_t enc_blr(unsigned rn) { return 0xD63F0000u | (rn << 5); }

/* LDR Xt, #imm19  (64bit 标量 literal, 偏移单位 4 字节, PC 相对) */
static inline uint32_t enc_ldr_x_lit(unsigned rt, uint32_t imm19) {
    return 0x58000000u | ((imm19 & 0x7FFFFu) << 5) | (rt & 31u);
}

/* ADRP Xd, label —— imm 是"页号之差"，21 位有符号，单位 4KB */
static inline uint32_t enc_adrp(unsigned rd, int64_t page_delta) {
    uint32_t imm = (uint32_t)(page_delta & 0x1FFFFF);
    return 0x90000000u | ((imm & 3u) << 29) | (((imm >> 2) & 0x7FFFFu) << 5) | (rd & 31u);
}

/* ADD (immediate) 64bit: 不可缩放的立即数形式 */
static inline uint32_t enc_add_imm(unsigned rd, unsigned rn, uint32_t imm12) {
    return 0x91000000u | ((imm12 & 0xFFFu) << 10) | (rn << 5) | (rd & 31u);
}

/* MOVZ / MOVK 64bit, 用于物化任意 64 位立即数 */
static inline uint32_t enc_movz(unsigned rd, uint16_t imm16, unsigned shift) {
    return 0xD2800000u | ((shift / 16u) << 21) | ((uint32_t)imm16 << 5) | (rd & 31u);
}
static inline uint32_t enc_movk(unsigned rd, uint16_t imm16, unsigned shift) {
    return 0xF2800000u | ((shift / 16u) << 21) | ((uint32_t)imm16 << 5) | (rd & 31u);
}

/* ---- 分类 ---- */
static inline bool is_b  (uint32_t i) { return (i & 0xFC000000u) == 0x14000000u; }
static inline bool is_bl (uint32_t i) { return (i & 0xFC000000u) == 0x94000000u; }
static inline bool is_br (uint32_t i) { return (i & 0xFFFFFC1Fu) == 0xD61F0000u; }
static inline bool is_blr(uint32_t i) { return (i & 0xFFFFFC1Fu) == 0xD63F0000u; }
/* is_ret 的掩码 0xFFFFFC1F 刻意抹掉 bit9:5（Rn），使 RET 默认形式
 * 0xD65F03C0 与 RET Xn 全部命中；同时 bit23:22（opc）仍在掩码内，
 * 所以 BR(0xD61F)/BLR(0xD63F) 不会误命中。已穷举 bits31:16 全
 * 65536 组验证：唯一命中前缀为 0xD65F，即 RET 族。 */
static inline bool is_ret(uint32_t i) { return (i & 0xFFFFFC1Fu) == 0xD65F0000u; }
static inline bool is_adr (uint32_t i) { return (i & 0x9F000000u) == 0x10000000u; }
static inline bool is_adrp(uint32_t i) { return (i & 0x9F000000u) == 0x90000000u; }

/* B.cond / CBZ / CBNZ / TBZ / TBNZ / LDR-literal / PRFM-literal：
 * 这些我们不做重定位（见 relocate_one 的说明），命中即放弃整个 hook。
 *
 * is_ldr_lit 的掩码 0x3B000000 刻意覆盖整个 PC-relative literal 类：
 * 已穷举 bits31:24 全 256 组，命中前缀为
 *   0x18 / 0x1C / 0x58 / 0x5C / 0x98 / 0x9C / 0xD8 / 0xDC
 * 其中 0xD8/0xDC 就是 PRFM literal —— 所以不需要单独的 is_prfm_lit
 * （早先写过一个 (i & 0x3B000000) == 0xD8000000 的谓词，掩码结果最大
 * 只有 0x3B000000，永远小于 0xD8000000，属恒假的死代码，已删除）。
 * 而 ADR/ADRP/B/BL 都不命中该掩码（见 relocate_one 的 (d)(e) 分支）。 */
static inline bool is_b_cond (uint32_t i) { return (i & 0xFF000010u) == 0x54000000u; }
static inline bool is_cbz   (uint32_t i) { return (i & 0x7E000000u) == 0x34000000u; }
static inline bool is_tbz   (uint32_t i) { return (i & 0x7E000000u) == 0x36000000u; }
static inline bool is_ldr_lit(uint32_t i) { return (i & 0x3B000000u) == 0x18000000u; }

/* B/BL 的 imm26：单位 4 字节，26 位有符号 */
static inline int64_t b_imm26(uint32_t i) {
    int64_t v = (int64_t)(i & 0x03FFFFFFu);
    if (v & (1LL << 25)) v -= (1LL << 26);
    return v << 2;
}
/*
 * ADR/ADRP 共用的 21 位立即数字段解码（immhi:immlo，符号扩展）。
 * 注意单位不同：ADR 是"字节偏移"，ADRP 是"页偏移(×4096)"，
 * 调用方需自行决定是否左移 12 位。
 */
static inline int64_t pc_rel_imm21(uint32_t i) {
    uint32_t immlo = (i >> 29) & 3u;
    uint32_t immhi = (i >> 5) & 0x7FFFFu;
    int64_t v = (int64_t)((immhi << 2) | immlo);
    if (v & (1LL << 20)) v -= (1LL << 21);
    return v;
}
/* B.cond / CBZ 的 imm19、TBZ 的 imm14 解码器在分类阶段被判定为
 * "不做重定位"后就不再需要，因此这里不再提供对应的反解函数 ——
 * 留着只会产生 -Wunused-function 告警，且暗示它们参与 relocation，
 * 反而误导后续维护。 */

/* ===================================================================== */
/* 2. Inline hook 引擎                                                   */
/* ===================================================================== */
/*
 * trampoline 布局（RWX 匿名页）
 *
 *   +0x000  BTI c
 *   +0x004  [relocated prologue ...]   来自 target[1..]（target[0] 被 patch 掉）
 *   +0x0NN  B (target + 4*k)           跳回原函数剩余部分
 *   +0x0NN+ [绝对地址表 / 远跳序列]
 *
 * 原函数处布局
 *
 *   +0x000  B  replacement        (若在 ±128MB 内，只占 1 条)
 *     或
 *   +0x000  LDR X16, #12 ; BR X16 ; NOP ; addr_lo ; addr_hi    (占 5 条)
 *
 * relocation 规则 —— 见 relocate_one() 的逐条注释。
 */

/*
 * 远跳序列：跳到任意 64 位绝对地址，不受 ±128MB / ±4GB 限制。
 *   +0  LDR X16, #12      ; 从 PC+12 装载 64 位地址
 *   +4  BR   X16
 *   +8  NOP               ; 保持 8 字节对齐
 *   +12 addr_lo
 *   +16 addr_hi
 * 写 5 条指令。X16 是 IP0 调用者保存寄存器，用作跳板是安全的。
 */
static size_t emit_far_jump(uint32_t *out, uint64_t target) {
    out[0] = enc_ldr_x_lit(/*rt=*/16, /*imm19=*/3); /* PC + 3*4 = +12 */
    out[1] = enc_br(/*rn=*/16);
    out[2] = enc_nop();
    out[3] = (uint32_t)(target & 0xFFFFFFFFu);
    out[4] = (uint32_t)(target >> 32);
    return 5;
}

/*
 * 搬迁单条指令到 trampoline。
 *   src_pc  : 指令在原函数中的真实地址（PC 相对计算的基准）
 *   dst_pc  : 指令在 trampoline 中的真实地址
 *   out     : 输出缓冲区
 *   out_max : 输出容量（指令数）
 * 返回写入的指令数；0 = 搬迁失败（调用方应放弃整个 hook）。
 */
static size_t relocate_one(uint32_t *out, const uint32_t *src,
                           uintptr_t src_pc, uintptr_t dst_pc, size_t out_max) {
    uint32_t insn = src[0];

    /* ---- (a) 与 PC 无关：原样复制 ----
     * 覆盖绝大多数 ALU / 立即数 / 寄存器偏移访存 / 系统寄存器访问等。
     * BLR Xn 也在这一类：它不依赖 PC，且只改写 X30(LR)，
     * 而序言阶段我们没有动过调用者的 LR，语义保持不变。 */
    if (is_br(insn) || is_blr(insn) || is_ret(insn)) {
        if (out_max < 1) return 0;
        out[0] = insn;
        return 1;
    }

    /* ---- (b) B label (±128MB) ---- */
    if (is_b(insn)) {
        uint64_t dst = (uint64_t)((int64_t)src_pc + b_imm26(insn));
        int64_t  rel = (int64_t)dst - (int64_t)dst_pc;
        if (rel >= -(1LL << 27) && rel < (1LL << 27)) {
            if (out_max < 1) return 0;
            out[0] = 0x14000000u | (uint32_t)((rel >> 2) & 0x03FFFFFFu);
            return 1;
        }
        /* 超出 B 的 ±128MB —— 改用绝对跳转 */
        if (out_max < 5) return 0;
        return emit_far_jump(out, dst);
    }

    /* ---- (c) BL label (±128MB) ----
     * 与 B 同理，但第二条指令必须是 BLR X16 才能保持"写回 LR"的语义。 */
    if (is_bl(insn)) {
        uint64_t dst = (uint64_t)((int64_t)src_pc + b_imm26(insn));
        int64_t  rel = (int64_t)dst - (int64_t)dst_pc;
        if (rel >= -(1LL << 27) && rel < (1LL << 27)) {
            if (out_max < 1) return 0;
            out[0] = 0x94000000u | (uint32_t)((rel >> 2) & 0x03FFFFFFu);
            return 1;
        }
        if (out_max < 5) return 0;
        out[0] = enc_ldr_x_lit(16, 3);
        out[1] = enc_blr(16);
        out[2] = enc_nop();
        out[3] = (uint32_t)(dst & 0xFFFFFFFFu);
        out[4] = (uint32_t)(dst >> 32);
        return 5;
    }

    /* ---- (d) ADR : ±1MB 字偏移 → 重写为 ADRP + ADD ----
     * ADR 的语义是 Xd = PC + imm。搬到新地址后 PC 变了，必须重算。
     * 拆成 ADRP(页) + ADD(页内偏移) 后，范围由 ±1MB 提升到 ±4GB。 */
    if (is_adr(insn)) {
        unsigned rd = insn & 31u;
        uint64_t target = (uint64_t)((int64_t)src_pc + pc_rel_imm21(insn));
        int64_t page_delta = (int64_t)((target >> 12) - (int64_t)(dst_pc >> 12));
        if (page_delta >= -(1LL << 20) && page_delta < (1LL << 20)) {
            if (out_max < 2) return 0;
            out[0] = enc_adrp(rd, page_delta);
            out[1] = enc_add_imm(rd, rd, (uint32_t)(target & 0xFFFu));
            return 2;
        }
        /* 极远（>4GB，实际几乎不可能）：用 MOVZ/MOVK 直接物化绝对值 */
        if (out_max < 4) return 0;
        uint64_t v = target;
        out[0] = enc_movz(rd, (uint16_t)(v & 0xFFFFu), 0);
        out[1] = enc_movk(rd, (uint16_t)((v >> 16) & 0xFFFFu), 16);
        out[2] = enc_movk(rd, (uint16_t)((v >> 32) & 0xFFFFu), 32);
        out[3] = enc_movk(rd, (uint16_t)((v >> 48) & 0xFFFFu), 48);
        return 4;
    }

    /* ---- (e) ADRP : ±4GB 页偏移 → 重算页差 ---- */
    if (is_adrp(insn)) {
        unsigned rd = insn & 31u;
        uint64_t target_page = (uint64_t)(((int64_t)src_pc & ~0xFFFLL)
                                + (pc_rel_imm21(insn) << 12));
        int64_t page_delta = (int64_t)(target_page >> 12) - (int64_t)(dst_pc >> 12);
        if (page_delta >= -(1LL << 20) && page_delta < (1LL << 20)) {
            if (out_max < 1) return 0;
            out[0] = enc_adrp(rd, page_delta);
            return 1;
        }
        if (out_max < 4) return 0;
        uint64_t v = target_page;
        out[0] = enc_movz(rd, (uint16_t)(v & 0xFFFFu), 0);
        out[1] = enc_movk(rd, (uint16_t)((v >> 16) & 0xFFFFu), 16);
        out[2] = enc_movk(rd, (uint16_t)((v >> 32) & 0xFFFFu), 32);
        out[3] = enc_movk(rd, (uint16_t)((v >> 48) & 0xFFFFu), 48);
        return 4;
    }

    /* ---- (f) 明确放弃的类别 ----
     * 这些是 PC 相对的，但重定位它们需要引入临时寄存器或条件反转，
     * 而函数序言里出现它们的概率极低（B.cond/CBZ 多在循环或分支体中）。
     * 与其冒错译导致进程崩溃，不如整个 hook 放弃 —— 失败方向是
     * "少一层防标记"，绝不能是"Widevine 崩了"。 */
    if (is_b_cond(insn) || is_cbz(insn) || is_tbz(insn) || is_ldr_lit(insn)) {
        return 0;
    }

    /* ---- (g) 其余一律原样复制 ----
     * A64 里 PC 相对的编码基本就上面这些；剩下的（Hint/NOP、系统
     * 寄存器读写等）都不依赖 PC。未识别的编码原样搬运，最坏结果是
     * 语义不符，不会破坏进程。 */
    if (out_max < 1) return 0;
    out[0] = insn;
    return 1;
}

/* 把 [page_start, page_end) 设为 RWX。失败返回 -1。 */
static int make_wx(void *page_start, size_t len) {
    return mprotect(page_start, len, PROT_READ | PROT_WRITE | PROT_EXEC);
}
static int make_rx(void *page_start, size_t len) {
    return mprotect(page_start, len, PROT_READ | PROT_EXEC);
}

static size_t page_size(void) {
    static size_t ps = 0;
    if (!ps) {
        long v = sysconf(_SC_PAGESIZE);
        ps = (v > 0) ? (size_t)v : 4096u;
    }
    return ps;
}

/*
 * 核心：给 target 打 inline hook。
 *   replacement : 替换函数
 *   out_tramp   : 返回 trampoline 入口（replacement 用它回调原函数）
 * 成功返回 0，失败返回 -1（调用方必须静默继续）。
 */
static int a64_hook(void *target, void *replacement, void **out_tramp) {
    if (!target || !replacement || !out_tramp) return -1;

    const size_t ps = page_size();
    const uintptr_t tgt = (uintptr_t)target;

    /* --- 1. 分配 trampoline --- */
    void *mem = mmap(NULL, TRAMP_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        LOGW("mmap trampoline failed: %s", strerror(errno));
        return -1;
    }
    uint32_t *tramp = (uint32_t *)mem;

    /* --- 2. 判断能否用单条 B 跳到 replacement --- */
    size_t patch_insns;
    int64_t rel_repl = (int64_t)replacement - (int64_t)tgt;
    if (rel_repl >= -(1LL << 27) && rel_repl < (1LL << 27)) {
        patch_insns = 1;
    } else {
        patch_insns = 5;   /* 远跳序列 */
    }

    /* --- 3. 搬迁序言指令 ---
     * target[0] 会被 patch 掉，所以从 target[1] 开始搬。
     * 为了让原函数尾部仍能顺序执行，我们要搬够 patch_insns 条，
     * 并且落点必须能连续接回 target + 4*k。 */
    uintptr_t body = (uintptr_t)(tramp + 1);        /* 跳过 BTI 桩 */
    uint32_t *out = (uint32_t *)body;
    const size_t out_cap = (TRAMP_SIZE / 4) - 8;

    size_t want = patch_insns + 2;   /* 多搬 2 条，减少"半条序言"式的破碎 */
    if (want > MAX_RELOC_INSNS) want = MAX_RELOC_INSNS;
    if (want > out_cap) { munmap(mem, TRAMP_SIZE); return -1; }

    /* 原函数代码页此刻仍是 r-x，先读出来 */
    uint32_t *src = (uint32_t *)(tgt + 4);   /* 从 target[1] 开始 */
    size_t produced = 0, consumed = 0;
    for (; consumed < want; consumed++) {
        size_t n = relocate_one(out + produced, src + consumed,
                                tgt + 4 + consumed * 4,
                                (uintptr_t)(out + produced), out_cap - produced);
        if (n == 0) {                       /* 遇到不可重定位的指令 */
            munmap(mem, TRAMP_SIZE);
            LOGW("prologue relocation refused at +%zu, abort hook", consumed);
            return -1;
        }
        produced += n;
    }
    if (produced < patch_insns) { munmap(mem, TRAMP_SIZE); return -1; }

    /* --- 4. trampoline 尾部：跳回 target + 4*(1+consumed) --- */
    uintptr_t resume = tgt + 4 + consumed * 4;
    {
        uint32_t *tail = out + produced;
        size_t cap_left = out_cap - produced;
        int64_t rel = (int64_t)resume - (int64_t)(uintptr_t)tail;
        if (rel >= -(1LL << 27) && rel < (1LL << 27) && cap_left >= 1) {
            tail[0] = 0x14000000u | (uint32_t)((rel >> 2) & 0x03FFFFFFu);
        } else if (cap_left >= 5) {
            emit_far_jump(tail, resume);
        } else {
            munmap(mem, TRAMP_SIZE);
            return -1;
        }
    }

    /* --- 5. patch 原函数代码页 ---
     * 需要覆盖 [tgt, tgt + 4*patch_insns)，可能跨页，按页对齐处理。 */
    uintptr_t p_lo = tgt & ~(uintptr_t)(ps - 1);
    uintptr_t p_hi = (tgt + 4 * patch_insns + ps - 1) & ~(uintptr_t)(ps - 1);
    if (make_wx((void *)p_lo, (size_t)(p_hi - p_lo)) != 0) {
        LOGW("mprotect RWX failed @ %p: %s", (void *)tgt, strerror(errno));
        munmap(mem, TRAMP_SIZE);
        return -1;
    }

    if (patch_insns == 1) {
        ((volatile uint32_t *)tgt)[0] = 0x14000000u | (uint32_t)(rel_repl >> 2);
    } else {
        emit_far_jump((uint32_t *)tgt, (uint64_t)replacement);
    }
    __builtin___clear_cache((char *)tgt, (char *)(tgt + 4 * patch_insns));

    /* 立即恢复 r-x：绝大多数 so 的 .text 是 r-xp，留在 RWX 状态徒增暴露面 */
    (void)make_rx((void *)p_lo, (size_t)(p_hi - p_lo));

    /* BTI 桩放在 tramp[0]，且 out_tramp 指向 tramp[0] 本身 ——
     * 开启 BTI 的设备上，replacement 是用 blr 间接进入 trampoline 的，
     * 落地页必须以 BTI 指令开头，否则会触发分支目标识别故障。
     * 被搬走的序言指令紧随其后（tramp[1] 起），顺序执行自然落到序言。 */
    tramp[0] = enc_bti_c();
    __builtin___clear_cache((char *)mem, (char *)mem + TRAMP_SIZE);

    *out_tramp = (void *)tramp;   /* 入口 = BTI 桩，桩后紧接序言 */
    LOGI("A64HookFunction SHA256_Final @ %p in %s (patch=%zu insns, moved=%zu)",
         target, "libcrypto.so", patch_insns, consumed);
    return 0;
}

/* 对外导出：AArch64 inline hook 的标准入口签名。
 * 必须显式标 visibility("default")——build.sh 用了 -fvisibility=hidden，
 * 漏标会让这两个函数从 .dynsym 消失，CI 的符号校验与外部加载器都会找不到。 */
__attribute__((visibility("default")))
void *A64HookFunction(void *target, void *replacement, void **orig) {
    void *tramp = NULL;
    if (a64_hook(target, replacement, &tramp) != 0) return NULL;
    if (orig) *orig = tramp;
    return tramp;
}
__attribute__((visibility("default")))
void *A64HookFunctionV(void *target, void *replacement, void **orig) {
    return A64HookFunction(target, replacement, orig);
}

/* ===================================================================== */
/* 3. 随机盐                                                             */
/* ===================================================================== */

/*
 * getrandom(2) 自 Bionic API 28 起才声明。本模块最低支持 Android 12(API 31)，
 * 运行时必然有；但如果按 API 26 头文件编译，__ANDROID_API__< 28 时
 * <sys/random.h> 不会声明它，直接调用会编译失败。
 *
 * 这里用弱符号做软依赖：API 26 头文件下自动退化为 NULL，运行时由下面的
 * NULL 检查切到 /dev/urandom。Android 12+ 上 p 该符号必然非 NULL，
 * 走的是 getrandom 快路径。绝不用 __ANDROID_API__ 宏做 #if 分支 ——
 * 那样会让编译头文件与运行设备耦合，换个 NDK 就静默降级。
 */
extern ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)
    __attribute__((weak));

static void gen_salt(char *out /* >= SALT_LEN+1 */) {
    unsigned char raw[SALT_RAW_LEN];
    size_t got = 0;

    /* 首选 getrandom(2)：内核 CSPRNG。Android 12 (API 31) 必然可用。 */
    if (getrandom) {
        while (got < sizeof(raw)) {
            ssize_t n = getrandom(raw + got, sizeof(raw) - got, 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            got += (size_t)n;
        }
    }

    /* 退化路径：/dev/urandom（getrandom 不可用或失败时） */
    if (got < sizeof(raw)) {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            while (got < sizeof(raw)) {
                ssize_t n = read(fd, raw + got, sizeof(raw) - got);
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) continue;
                    break;
                }
                got += (size_t)n;
            }
            close(fd);
        }
    }

    if (got < sizeof(raw)) {
        /* 最后兜底：绝不能返回全 0 —— 全 0 盐等价于"没有防标记" */
        LOGW("entropy source degraded (%zu/%zu bytes), using clock fallback",
             got, sizeof(raw));
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t s = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        s ^= (uint64_t)getpid() << 32;
        for (size_t i = got; i < sizeof(raw); i++) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            raw[i] = (unsigned char)(s >> 33);
        }
    }

    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < SALT_RAW_LEN; i++) {
        out[i * 2]     = hex[(raw[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[raw[i] & 0xF];
    }
    out[SALT_LEN] = '\0';
}

/* ===================================================================== */
/* 4. /proc/self/maps 扫描：找出所有 libcrypto.so 实例                        */
/* ===================================================================== */

struct libcrypto_inst {
    uintptr_t lo, hi;      /* 该实例所有映射的并集范围（用于校验） */
    char      path[256];   /* 映射里的原始路径，可能带 " (deleted)" */
};

/* 单个进程里 libcrypto.so 的实例数上限。
 * 正常情况 1 个；Android 上常见的额外实例来自 zygote 预加载、
 * vendor 侧自带一份、或 so 被替换后遗留的 (deleted) 映射。
 * 给 8 是留足余量又不允许无界增长的折中：超出部分会记日志并跳过，
 * 不会溢出栈上数组。struct 约 264 字节，8 份≈2KB，安全。 */
#define MAX_LIBCRYPTO 8

/*
 * 匹配规则：路径含 "libcrypto.so" 即命中。
 *  - 容忍 "(deleted)"：so 在进程运行期被替换/卸载时，maps 里会出现
 *    "/path/libcrypto.so (deleted)"。这是多实例的常见来源，必须一并收集。
 *  - 同一 so 通常有多个 PT_LOAD 映射（.text/.rodata/.data），maps 已按
 *    地址升序排列，所以每个 path 第一次出现的那段就是最低地址 —— 即
 *    ELF 头所在处（load bias 基准），无需二次扫描。
 */
static size_t scan_libcrypto(struct libcrypto_inst *out, size_t max_out) {
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) { LOGW("open /proc/self/maps failed: %s", strerror(errno)); return 0; }

    size_t count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo, hi;
        char perms[8], rest[512];
        rest[0] = '\0';
        /* 格式： lo-hi perms offset dev inode path */
        if (sscanf(line, "%lx-%lx %7s %*x %*x:%*x %*u %511[^\n]",
                   &lo, &hi, perms, rest) < 4) {
            continue;
        }
        if (!strstr(rest, "libcrypto.so")) continue;

        /* 去掉行尾空白，避免路径匹配时因换行/空格导致同实例被拆成两条 */
        size_t rl = strlen(rest);
        while (rl > 0 && (rest[rl-1] == ' ' || rest[rl-1] == '\t' ||
                          rest[rl-1] == '\n' || rest[rl-1] == '\r')) {
            rest[--rl] = '\0';
        }

        size_t i;
        for (i = 0; i < count; i++) {
            if (strcmp(out[i].path, rest) == 0) {
                if ((uintptr_t)lo < out[i].lo) out[i].lo = (uintptr_t)lo;
                if ((uintptr_t)hi > out[i].hi) out[i].hi = (uintptr_t)hi;
                break;
            }
        }
        if (i == count) {
            if (count >= max_out) break;
            snprintf(out[count].path, sizeof(out[count].path), "%s", rest);
            out[count].lo = (uintptr_t)lo;
            out[count].hi = (uintptr_t)hi;
            count++;
        }
    }
    fclose(f);
    return count;
}

/* ===================================================================== */
/* 5. ELF 符号定位（走 PT_DYNAMIC，不依赖 section header）                    */
/* ===================================================================== */

/*
 * 关键点：动态加载器已经把 PT_DYNAMIC 里的 d_ptr 全部重定位成了运行时
 * 绝对地址，所以 DT_SYMTAB / DT_STRTAB 的 d_ptr 可以直接当指针用，
 * 不要再叠加 load bias。只有 PT_DYNAMIC 自身的地址需要 bias（它的 p_vaddr
 * 是文件内虚拟地址）。
 */
static void *elf_find_symbol(uintptr_t base, const char *want) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return NULL;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) return NULL;
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) return NULL;

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + eh->e_phoff);

    /* load bias = 实际基址 - 第一个 PT_LOAD 的 p_vaddr(页对齐) */
    uintptr_t bias = base;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            bias = base - (ph[i].p_vaddr & ~0xFFFull);
            break;
        }
    }

    const Elf64_Dyn *dyn = NULL;
    size_t dyn_count = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (const Elf64_Dyn *)(base + ph[i].p_vaddr);
            dyn_count = ph[i].p_memsz / sizeof(Elf64_Dyn);
            break;
        }
    }
    if (!dyn) return NULL;

    const Elf64_Sym *symtab = NULL;
    const char *strtab = NULL;
    for (size_t i = 0; i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
        if (dyn[i].d_tag == DT_SYMTAB) symtab = (const Elf64_Sym *)dyn[i].d_un.d_ptr;
        else if (dyn[i].d_tag == DT_STRTAB) strtab = (const char *)dyn[i].d_un.d_ptr;
    }
    if (!symtab || !strtab) return NULL;

    /*
     * 符号总数没有直接的 dynamic tag，只能推断。这里优先用 DT_HASH 的
     * nchain（等于符号表长度，最可靠）；没有 DT_HASH 时退回 DT_GNU_HASH，
     * 沿各 bucket 的 chain 链走到底取最大下标。两者都没有就放弃 ——
     * 宁可少一层防标记，也不能越界读导致 Widevine 崩溃。
     */
    size_t max_sym = 0;
    for (size_t i = 0; i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
        if (dyn[i].d_tag == DT_HASH) {
            const uint32_t *h = (const uint32_t *)dyn[i].d_un.d_ptr;
            if (h) max_sym = h[1];              /* nchain == 符号数 */
            break;
        }
    }
    if (!max_sym) {
        for (size_t i = 0; i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
            if (dyn[i].d_tag != DT_GNU_HASH) continue;
            const uint32_t *g = (const uint32_t *)dyn[i].d_un.d_ptr;
            if (!g) break;
            uint32_t nbuckets = g[0];
            uint32_t symoffset = g[1];
            uint32_t bloom_size = g[2];
            if (nbuckets == 0 || bloom_size == 0) break;
            /* 布局: [0]=nbuckets [1]=symoffset [2]=bloom_size [3]=bloom_shift
             *       然后 bloom[bloom_size] (uint64)，然后 buckets[nbuckets]，
             *       然后 chain[] —— chain 的下标与符号表下标一一对应。 */
            const uint64_t *bloom = (const uint64_t *)&g[4];
            const uint32_t *buckets = (const uint32_t *)&bloom[bloom_size];
            const uint32_t *chain  = &buckets[nbuckets];
            for (uint32_t b = 0; b < nbuckets; b++) {
                if (buckets[b] < symoffset) continue;
                /* 从 bucket 起点沿 chain 前进，直到遇到 hash 值的最低位为 1
                 * （表示该桶的最后一个符号）。GNU hash 的 chain 数组与符号表
                 * 同下标，因此这里得到的就是符号表长度上界。 */
                for (uint32_t idx = buckets[b]; ; idx++) {
                    if (chain[idx - symoffset] & 1u) {
                        if ((size_t)idx + 1 > max_sym) max_sym = (size_t)idx + 1;
                        break;
                    }
                }
            }
            break;
        }
    }
    if (!max_sym) {
        LOGW("no DT_HASH/DT_GNU_HASH, cannot bound symtab, skip");
        return NULL;
    }

    for (size_t ref_idx = 0; ref_idx < max_sym; ref_idx++) {
        const Elf64_Sym *s = &symtab[ref_idx];
        if (s->st_name == 0 || s->st_name >= 0x10000) continue;
        if (s->st_shndx == SHN_UNDEF) continue;          /* 未定义，不是实现 */
        if (s->st_value == 0) continue;
        if (strncmp(strtab + s->st_name, want, 256) != 0) continue;

        uintptr_t off_in_so = (uintptr_t)(bias + s->st_value);
        LOGI("ELFIO: %s @ %p (base=0x%llx + off=0x%llx)",
             want, (void *)off_in_so,
             (unsigned long long)bias, (unsigned long long)s->st_value);
        return (void *)off_in_so;
    }
    return NULL;
}

/* ===================================================================== */
/* 6. 替换函数与安装                                                     */
/* ===================================================================== */

static char           g_salt[SALT_LEN + 1];
static sha256_update_fn g_real_update;      /* 未被 patch 的原始 Update */
static void          *g_tramp;             /* SHA256_Final 的 trampoline */
static size_t         g_hooked = 0;

static int hooked_sha256_final(unsigned char *md, void *ctx) {
    /*
     * 关键：在原函数真正跑之前，把盐并进 ctx 的哈希链。
     * 用的是"未被 patch 的"SHA256_Update（g_real_update 指向 ELF 里的原始
     * 地址），所以不会递归。
     * SHA256_Update 只往 ctx 的缓冲区追加数据并累加长度计数，不改变 ctx 的
     * 语义状态，因此对任意 SHA256 ctx 都是安全的。
     */
    if (g_real_update && g_salt[0]) {
        g_real_update(ctx, g_salt, SALT_LEN);
    }
    return ((sha256_final_fn)g_tramp)(md, ctx);
}

static void install_for_one(uintptr_t base, const char *path) {
    void *p_final  = elf_find_symbol(base, "SHA256_Final");
    void *p_update = elf_find_symbol(base, "SHA256_Update");

    if (!p_final || !p_update) {
        LOGW("neither SHA256_Update nor SHA256_Final found in %s", path);
        return;
    }
    /* 符号地址必须落在该实例的映射范围内，否则说明解析有误，宁可不 hook */
    if ((uintptr_t)p_final < base || (uintptr_t)p_final > base + (1UL << 32)) {
        LOGW("resolved SHA256_Final out of range in %s", path);
        return;
    }
    /* 只 patch Final；Update 保持原样，注入盐时直接调原始地址，避免递归 */
    g_real_update = (sha256_update_fn)p_update;

    void *tramp = NULL;
    if (a64_hook(p_final, (void *)hooked_sha256_final, &tramp) != 0) {
        LOGW("hook failed for %s (Widevine untouched)", path);
        return;
    }
    g_tramp = tramp;
    g_hooked++;
}

static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void drmid_init_once(void) {
    LOGI("drmid_hook initialising (pid=%d)", (int)getpid());

    /* 先生成盐：构造函数顺序不保证，必须在安装 hook 前完成，
     * 否则第一次 SHA256_Final 可能注入空盐。 */
    gen_salt(g_salt);
    LOGI("salt generated (%d hex chars, per-boot random)", SALT_LEN);

    struct libcrypto_inst inst[MAX_LIBCRYPTO];
    size_t n = scan_libcrypto(inst, MAX_LIBCRYPTO);
    if (n == 0) {
        LOGW("no libcrypto.so instance found in maps");
        return;
    }
    LOGI("found %zu libcrypto.so instance(s)", n);

    for (size_t i = 0; i < n; i++) {
        install_for_one(inst[i].lo, inst[i].path);
    }
    LOGI("done hooked %zu libcrypto instance(s)", g_hooked);
}

/* 显式初始化入口（幂等） */
void drmid_hook_init(void) { pthread_once(&g_once, drmid_init_once); }

/*
 * 构造函数。
 * LD_PRELOAD 场景下，动态链接器会先映射完所有 DT_NEEDED（libcrypto 已在
 * maps 里），再依次运行各库的构造函数，因此此处 libcrypto 一定可见。
 * 只 patch SHA256_Final 一个点，不需要 dlopen 时机上的重试逻辑。
 */
__attribute__((constructor))
static void drmid_ctor(void) { drmid_hook_init(); }

/*
 * JNI_OnLoad：兼容被当作 JNI 库加载的场合。
 * 不依赖 jni.h —— 用签名等价的裸声明即可，避免为一行入口引入 NDK jni 头。
 */
typedef int jint_;
__attribute__((visibility("default")))
jint_ JNI_OnLoad(void *vm, void *reserved) {
    (void)vm; (void)reserved;
    drmid_hook_init();
    return 0x00010006;   /* JNI_VERSION_1_6 */
}

/* ZygiskNext 风格的模块入口约定符号：加载器按此符号判定模块可用。
 * 本项目用的是普通 Zygisk + LD_PRELOAD，这个符号主要服务于
 * 「把本库当HAL/模块被 dlopen 加载」的场景，保留以兼容该加载路径。 */
__attribute__((visibility("default")))
void *zn_module(void *handle) {
    drmid_hook_init();
    return handle;
}

/* 供 status / 调试读取当前盐值（正常不会被调用） */
__attribute__((visibility("default")))
const char *drmid_salt(void) { return g_salt; }

/* 供 status 查询已 hook 的实例数 */
__attribute__((visibility("default")))
size_t drmid_hooked_count(void) { return g_hooked; }
