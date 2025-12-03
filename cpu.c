#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <errno.h>

typedef int64_t  word_t;   // 64-bit 有符号
typedef uint8_t  byte_t;
typedef uint64_t addr_t;

// ---- 指令码 ICODE ----
enum {
    IHALT   = 0,
    INOP    = 1,
    IRRMOVQ = 2,  // 包括 cmovXX
    IIRMOVQ = 3,
    IRMMOVQ = 4,
    IMRMOVQ = 5,
    IOPQ    = 6,
    IJXX    = 7,
    ICALL   = 8,
    IRET    = 9,
    IPUSHQ  = 10,
    IPOPQ   = 11
};

// ---- 寄存器编号（高 4 位 / 低 4 位）----
enum {
    R_RAX = 0,
    R_RCX = 1,
    R_RDX = 2,
    R_RBX = 3,
    R_RSP = 4,
    R_RBP = 5,
    R_RSI = 6,
    R_RDI = 7,
    R_R8  = 8,
    R_R9  = 9,
    R_R10 = 10,
    R_R11 = 11,
    R_R12 = 12,
    R_R13 = 13,
    R_R14 = 14,
    R_NONE = 15          // RNONE
};

// ---- 状态码 Stat ----
typedef enum {
    SAOK,   // 正常
    SHLT,   // halt
    SADR,   // 取指/访存地址错误
    SINS    // 非法指令
} stat_t;

// ---- 内存模型：一大片字节 ----
#define MEM_SIZE 0x100000  // 1MB 示例，按需调整

typedef struct {
    byte_t data[MEM_SIZE];
} mem_t;

// 简单的内存读接口：出错返回 false
static bool mem_read_byte(const mem_t *mem, addr_t addr, byte_t *out) {
    if (addr >= MEM_SIZE) return false;
    *out = mem->data[addr];
    return true;
}

static bool mem_read_word(const mem_t *mem, addr_t addr, word_t *out) {
    if (addr >= MEM_SIZE || addr > MEM_SIZE - 8) return false;
    word_t val = 0;
    // 小端序：低地址是低字节
    for (int i = 0; i < 8; i++) {
        val |= ((word_t)mem->data[addr + i]) << (8 * i);
    }
    *out = val;
    return true;
}

// 内存写接口：出错返回 false
static bool mem_write_byte(mem_t *mem, addr_t addr, byte_t val) {
    if (addr >= MEM_SIZE) return false;
    mem->data[addr] = val;
    return true;
}

static bool mem_write_word(mem_t *mem, addr_t addr, word_t val) {
    if (addr >= MEM_SIZE || addr > MEM_SIZE - 8) return false;
    // 小端序：低地址写低字节
    for (int i = 0; i < 8; i++) {
        mem->data[addr + i] = (byte_t)((val >> (8 * i)) & 0xFF);
    }
    return true;
}


typedef struct {
    bool ZF;  // zero flag
    bool SF;  // sign flag
    bool OF;  // overflow flag
} cc_t;


typedef struct {
    addr_t pc;   // 当前指令地址
    mem_t  mem;  // 指令+数据内存

    // 通用寄存器文件：0..14 对应 R_RAX..R_R14
    word_t regs[15];

    cc_t   cc;    // 条件码 ZF/SF/OF
    stat_t stat;  // 当前状态码（目前暂时没用，后面 Memory/PC 阶段会用）
} cpu_state_t;


// Fetch 阶段产生的所有信号
typedef struct {
    bool   imem_error;   // 取指是否越界/失败
    bool   instr_valid;  // icode 是否为合法指令

    int    icode;        // 指令码
    int    ifun;         // 功能码

    int    rA;           // 寄存器 ID，高 4 位
    int    rB;           // 寄存器 ID，低 4 位

    word_t valC;         // 立即数（若有）
    addr_t valP;         // 顺序下一条指令地址

    bool   need_regids;  // 是否有 rA:rB 字节
    bool   need_valC;    // 是否有 8 字节立即数
} fetch_out_t;

// 只要 icode 落在 0..IPOPQ 之间就算合法
static bool instr_valid(int icode) {
    return icode >= IHALT && icode <= IPOPQ;
}
// 这条指令是否包含 rA:rB 寄存器字节？
static bool need_regids(int icode) {
    switch (icode) {
        case IRRMOVQ:
        case IIRMOVQ:
        case IRMMOVQ:
        case IMRMOVQ:
        case IOPQ:
        case IPUSHQ:
        case IPOPQ:
            return true;
        default:
            return false;
    }
}

// 这条指令是否包含 8 字节立即数 valC？
static bool need_valC(int icode) {
    switch (icode) {
        case IIRMOVQ:
        case IRMMOVQ:
        case IMRMOVQ:
        case IJXX:
        case ICALL:
            return true;
        default:
            return false;
    }
}

// 拆 opcode 字节为 icode/ifun
static void split_icode_ifun(byte_t byte0, int *icode, int *ifun) {
    *icode = (byte0 >> 4) & 0xF;
    *ifun  = byte0 & 0xF;
}

// 检查从 pc 开始、长度 len 的取指是否越界
static void decode_regids(byte_t regbyte, int *rA, int *rB) {
    *rA = (regbyte >> 4) & 0xF;
    *rB = regbyte & 0xF;
}
// 根据 need_regids / need_valC 计算整条指令的长度（字节数）
static int instr_length(bool need_regids, bool need_valC) {
    int len = 1;          // icode:ifun 这一字节
    if (need_regids) len += 1;  // rA:rB
    if (need_valC)   len += 8;  // 8 字节 valC
    return len;
}

// 顺序下一条指令地址 valP = pc + instr_length(...)
static addr_t compute_valP(addr_t pc, bool need_regids, bool need_valC) {
    return pc + (addr_t)instr_length(need_regids, need_valC);
}

// 检查从 pc 开始、长度 len 的取指是否越界
static bool check_imem_error(addr_t pc, bool need_regids, bool need_valC) {
    int len = instr_length(need_regids, need_valC);
    return (pc + len > MEM_SIZE);
}
fetch_out_t fetch_stage(const cpu_state_t *S) {
    fetch_out_t F;

    // 先给所有字段填一个“保守默认值”
    F.imem_error  = false;
    F.instr_valid = false;

    F.icode = INOP;
    F.ifun  = 0;
    F.rA    = R_NONE;
    F.rB    = R_NONE;
    F.valC  = 0;
    F.valP  = S->pc;

    F.need_regids = false;
    F.need_valC   = false;

    addr_t pc = S->pc;

    // 1. 取 opcode 字节
    byte_t byte0;
    if (!mem_read_byte(&S->mem, pc, &byte0)) {
        // 连第一个字节都取不到，说明肯定越界了
        F.imem_error = true;
        return F;
    }

    // 2. 解析 icode / ifun
    split_icode_ifun(byte0, &F.icode, &F.ifun);

    // 3. 判断是否合法、是否需要寄存器字节 / 立即数
    F.instr_valid = instr_valid(F.icode);
    F.need_regids = need_regids(F.icode);
    F.need_valC   = need_valC(F.icode);

    // 4. 先算出顺序下一条地址 valP（PC 增加器逻辑）
    F.valP = compute_valP(pc, F.need_regids, F.need_valC);

    // 5. 可选：用长度预检查是否越界
    if (check_imem_error(pc, F.need_regids, F.need_valC)) {
        F.imem_error = true;
        return F;
    }

    // 6. 如果需要寄存器字节，从 pc+1 处继续读
    addr_t cursor = pc + 1;
    if (F.need_regids) {
        byte_t regbyte;
        if (!mem_read_byte(&S->mem, cursor, &regbyte)) {
            F.imem_error = true;
            return F;
        }
        decode_regids(regbyte, &F.rA, &F.rB);
        cursor += 1;
    }

    // 7. 如果需要 valC，从当前 cursor 处读 8 字节
    if (F.need_valC) {
        if (!mem_read_word(&S->mem, cursor, &F.valC)) {
            F.imem_error = true;
            return F;
        }
        // cursor += 8;   // 如果后面需要继续往后读，可以更新
    }

    // 至此：F 里已经包含了 Fetch 阶段的全部输出信号
    return F;
}

//////////////////////////// decode ////////////////////////////////////

// Decode 阶段产生的信号
typedef struct {
    int    srcA;
    int    srcB;
    int    dstE;
    int    dstM;
    word_t valA;
    word_t valB;
} decode_out_t;
// 从寄存器文件中读一个寄存器
static word_t reg_read(const cpu_state_t *S, int regid) {
    if (regid == R_NONE) {
        return 0;   // 约定：RNONE 读出来就是 0
    }
    if (regid < 0 || regid > R_R14) {
        return 0;   // 防御性处理
    }
    return S->regs[regid];
}

// 写寄存器（Decode 阶段暂时用不到，先放在这里，后面 Write-back 会用）
static void reg_write(cpu_state_t *S, int regid, word_t val) {
    if (regid == R_NONE) return;
    if (regid < 0 || regid > R_R14) return;
    S->regs[regid] = val;
}
static int select_srcA(int icode, int rA) {
    switch (icode) {
        case IRRMOVQ:  // 包括 cmovXX
        case IRMMOVQ:
        case IOPQ:
        case IPUSHQ:
            return rA;
        case IPOPQ:
        case IRET:
            return R_RSP;
        default:
            return R_NONE;
    }
}
static int select_srcB(int icode, int rB) {
    switch (icode) {
        case IRMMOVQ:
        case IMRMOVQ:
        case IOPQ:
            return rB;
        case IPUSHQ:
        case IPOPQ:
        case ICALL:
        case IRET:
            return R_RSP;
        default:
            return R_NONE;
    }
}
// 根据 icode 和 Cnd 选择 dstE
// 真实硬件中：IRRMOVQ/CMOVXX 只有当 Cnd 为真时才写 rB
static int select_dstE(int icode, int rB, bool Cnd) {
    switch (icode) {
        case IRRMOVQ:  // 条件传送 / 普通寄存器传送
            return Cnd ? rB : R_NONE;
        case IIRMOVQ:
        case IOPQ:
            return rB;
        case IPUSHQ:
        case IPOPQ:
        case ICALL:
        case IRET:
            return R_RSP;
        default:
            return R_NONE;
    }
}
static int select_dstM(int icode, int rA) {
    switch (icode) {
        case IMRMOVQ:
        case IPOPQ:
            return rA;
        default:
            return R_NONE;
    }
}
// 根据 icode 和 srcA 计算 valA（需要用到寄存器或 valP）
static word_t compute_valA(const cpu_state_t *S,
                           const fetch_out_t *F,
                           int srcA) {
    switch (F->icode) {
        case ICALL:
        case IJXX:
            // 对 call/jXX，valA 应该是顺序下一条的地址 valP（返回地址）
            return (word_t)F->valP;
        default:
            // 其他情况：如果 srcA 有效，就读寄存器
            return reg_read(S, srcA);
    }
}

// 根据 icode 和 srcB 计算 valB
static word_t compute_valB(const cpu_state_t *S,
                           const fetch_out_t *F,
                           int srcB) {
    switch (F->icode) {
        case IRMMOVQ:
        case IMRMOVQ:
        case IOPQ:
        case IPUSHQ:
        case IPOPQ:
        case ICALL:
        case IRET:
            return reg_read(S, srcB);
        default:
            return 0;
    }
}
decode_out_t decode_stage(const cpu_state_t *S, const fetch_out_t *F) {
    decode_out_t D;

    // 1. 先根据 icode/rA/rB 选出 srcA/srcB
    D.srcA = select_srcA(F->icode, F->rA);
    D.srcB = select_srcB(F->icode, F->rB);

    // 2. 选出“潜在”的 dstE/dstM
    //    这里暂时假设 Cnd = true，表示“条件传送如果成立时要写的寄存器”。
    D.dstE = select_dstE(F->icode, F->rB, true /* Cnd 假设为真 */);
    D.dstM = select_dstM(F->icode, F->rA);

    // 3. 计算 valA / valB
    D.valA = compute_valA(S, F, D.srcA);
    D.valB = compute_valB(S, F, D.srcB);

    return D;
}
// ---- ALU 功能码（ifun 在 IOPQ 时的含义）----
enum {
    ALU_ADD = 0,
    ALU_SUB = 1,
    ALU_AND = 2,
    ALU_XOR = 3
};
// Execute 阶段产生的信号
typedef struct {
    word_t valE;   // ALU 结果
    bool   Cnd;    // 条件是否满足（给 JXX / CMOVXX 用）
} execute_out_t;

//////////////////////////////////// execute ////////////////////////////////////////

static word_t select_aluA(int icode, word_t valA, word_t valC) {
    switch (icode) {
        case IRRMOVQ:
        case IOPQ:
            return valA;   // 普通寄存器运算

        case IIRMOVQ:
        case IRMMOVQ:
        case IMRMOVQ:
            return valC;   // 带立即数的地址计算 / 移动

        case ICALL:
        case IPUSHQ:
            return -8;     // 栈指针减 8

        case IRET:
        case IPOPQ:
            return 8;      // 栈指针加 8

        default:
            return 0;
    }
}

static word_t select_aluB(int icode, word_t valB) {
    switch (icode) {
        case IRMMOVQ:
        case IMRMOVQ:
        case IOPQ:
        case ICALL:
        case IPUSHQ:
        case IRET:
        case IPOPQ:
            return valB;   // 需要基址寄存器 / 栈指针
        default:
            return 0;      // 其它不需要第二个操作数
    }
}

static int select_alufun(int icode, int ifun) {
    if (icode == IOPQ) {
        // addq/subq/andq/xorq 对应 ifun
        return ifun;
    }
    // 其他所有指令，ALU 都是做加法（地址计算、栈操作等）
    return ALU_ADD;
}

static bool need_set_cc(int icode /*, stat_t stat*/) {
    // 如果后面你在 Execute 阶段能拿到 stat，可以再加 stat == SAOK 的判断
    return icode == IOPQ;
}

static word_t alu_compute(word_t aluA, word_t aluB,
                          int alufun,
                          bool set_cc,
                          cc_t *cc) {
    word_t res = 0;

    switch (alufun) {
        case ALU_ADD:
            res = aluB + aluA;
            break;
        case ALU_SUB:
            // Y86 里 OPq rA, rB 是：rB <- rB op rA
            // 所以减法要做 aluB - aluA
            res = aluB - aluA;
            break;
        case ALU_AND:
            res = aluB & aluA;
            break;
        case ALU_XOR:
            res = aluB ^ aluA;
            break;
        default:
            res = 0;
            break;
    }

    if (set_cc && cc) {
        // ZF：是否为 0
        cc->ZF = (res == 0);
        // SF：符号位（最高位是否为 1）
        cc->SF = (res < 0);

        // OF：有符号加减溢出
        bool signA = (aluA < 0);
        bool signB = (aluB < 0);
        bool signR = (res < 0);

        switch (alufun) {
            case ALU_ADD:
                // 加法：同号相加，结果变号 → 溢出
                cc->OF = (signA == signB) && (signR != signA);
                break;
            case ALU_SUB:
                // 减法：B - A，看成 B + (-A)
                // 溢出条件：B 和 (-A) 同号，结果与 B 异号
                // 等价写法：signB != signA && signR != signB
                cc->OF = (signB != signA) && (signR != signB);
                break;
            default:
                cc->OF = false; // AND/XOR 不产生溢出
                break;
        }
    }

    return res;
}
static bool compute_Cnd(int icode, int ifun, const cc_t *cc) {
    // 只有条件跳转和条件传送用到 Cnd，其余我们一律返回 true（表示“无条件”）
    if (icode != IJXX && icode != IRRMOVQ) {
        return true;
    }

    // 防御：万一 cc 为空
    if (!cc) return true;

    bool ZF = cc->ZF;
    bool SF = cc->SF;
    bool OF = cc->OF;

    switch (ifun) {
        case 0: // C_YES: 无条件
            return true;
        case 1: // C_LE:  (SF ^ OF) | ZF
            return (SF ^ OF) || ZF;
        case 2: // C_L:   SF ^ OF
            return (SF ^ OF);
        case 3: // C_E:   ZF
            return ZF;
        case 4: // C_NE:  !ZF
            return !ZF;
        case 5: // C_GE:  !(SF ^ OF)
            return !(SF ^ OF);
        case 6: // C_G:   !(SF ^ OF) & !ZF
            return !(SF ^ OF) && !ZF;
        default:
            // 非法 ifun，当作条件不成立
            return false;
    }
}
execute_out_t execute_stage(cpu_state_t *S,
                            const fetch_out_t  *F,
                            const decode_out_t *D) {
    execute_out_t E;

    // 1. 选 aluA / aluB / alufun
    word_t aluA = select_aluA(F->icode, D->valA, F->valC);
    word_t aluB = select_aluB(F->icode, D->valB);
    int    aluf = select_alufun(F->icode, F->ifun);

    // 2. 是否更新条件码
    bool set_cc = need_set_cc(F->icode /*, S->stat*/);

    // 3. ALU 计算（如有需要，同时更新 S->cc）
    E.valE = alu_compute(aluA, aluB, aluf, set_cc, &S->cc);

    // 4. 根据当前 cc 和 ifun 计算 Cnd
    E.Cnd = compute_Cnd(F->icode, F->ifun, &S->cc);

    return E;
}

////////////////////////////////////// memory //////////////////////////////////////

// Memory 阶段产生的信号
typedef struct {
    word_t valM;       // 从内存读出的值（若有）
    bool   dmem_error; // 数据内存访问是否出错
} memory_out_t;

static word_t select_mem_addr(int icode, word_t valE, word_t valA) {
    switch (icode) {
        case IRMMOVQ:
        case IPUSHQ:
        case ICALL:
        case IMRMOVQ:
            // rmmovq/pushq/call/mrmovq 使用 valE 作为内存地址
            return valE;
        case IPOPQ:
        case IRET:
            // popq/ret 使用 valA（栈顶）作为内存地址
            return valA;
        default:
            // 其他指令不访问内存
            return 0;
    }
}

static word_t select_mem_data(int icode, word_t valA, addr_t valP) {
    switch (icode) {
        case IRMMOVQ:
        case IPUSHQ:
            // rmmovq/pushq 把 valA 写入内存
            return valA;
        case ICALL:
            // call 把返回地址 valP 压栈
            return (word_t)valP;
        default:
            // 其他指令不写内存
            return 0;
    }
}

static bool is_mem_read(int icode) {
    switch (icode) {
        case IMRMOVQ:
        case IPOPQ:
        case IRET:
            return true;
        default:
            return false;
    }
}

static bool is_mem_write(int icode) {
    switch (icode) {
        case IRMMOVQ:
        case IPUSHQ:
        case ICALL:
            return true;
        default:
            return false;
    }
}

memory_out_t memory_stage(cpu_state_t *S,
                          const fetch_out_t   *F,
                          const decode_out_t  *D,
                          const execute_out_t *E) {
    memory_out_t M;
    M.valM       = 0;
    M.dmem_error = false;

    int    icode = F->icode;
    word_t valE  = E->valE;
    word_t valA  = D->valA;
    addr_t valP  = F->valP;

    // 1. 生成控制信号和地址/数据
    bool   mem_read  = is_mem_read(icode);
    bool   mem_write = is_mem_write(icode);
    word_t mem_addr_signed  = select_mem_addr(icode, valE, valA);
    word_t mem_data  = select_mem_data(icode, valA, valP);

    // 2. 访问内存（Y86 中没有一条指令同时读又写，所以读写互斥）
    if (mem_read) {
        word_t value;
        if (mem_addr_signed < 0 || !mem_read_word(&S->mem, (addr_t)mem_addr_signed, &value)) {
            M.dmem_error = true;
        } else {
            M.valM = value;
        }
    }

    if (mem_write) {
        if (mem_addr_signed < 0 || !mem_write_word(&S->mem, (addr_t)mem_addr_signed, mem_data)) {
            M.dmem_error = true;
        }
    }

    return M;
}

//////////////////////////////////// writeback //////////////////////////////////////

// Write-back 阶段：把 valE / valM 写回寄存器文件
void writeback_stage(cpu_state_t       *S,
                     const fetch_out_t *F,
                     const decode_out_t *D,
                     const execute_out_t *E,
                     const memory_out_t  *M) {
    // 1. 处理 dstE：需要考虑条件传送 IRRMOVQ/CMOVXX 的 Cnd
    int dstE = D->dstE;

    if (F->icode == IRRMOVQ) {
        // 对于 IRRMOVQ（包括 cmovXX），真正是否写回要看 Cnd
        // 重新用 Cnd 计算一次 dstE：
        dstE = select_dstE(F->icode, F->rB, E->Cnd);
        // 这样 Cnd == 0 时，dstE = R_NONE，reg_write 会什么都不做
    }

    // 2. 把 ALU 结果写回 dstE（如果不是 R_NONE）
    reg_write(S, dstE, E->valE);

    // 3. 把内存读出的结果写回 dstM（如果不是 R_NONE）
    reg_write(S, D->dstM, M->valM);
}

//////////////////////////////////// PC update //////////////////////////////////////

// 根据 imem_error / dmem_error / instr_valid / icode 计算 Stat
static stat_t compute_stat(bool imem_error,
                           bool dmem_error,
                           bool instr_valid,
                           int icode)
{
    // 地址错误优先（取指 or 访存）
    if (imem_error || dmem_error) {
        return SADR;
    }

    // 非法指令
    if (!instr_valid) {
        return SINS;
    }

    // 正常执行的 halt
    if (icode == IHALT) {
        return SHLT;
    }

    // 其它情况都算正常
    return SAOK;
}

// 按 HCL 规则计算下一条 PC
// word new_pc = [ 
//   icode == ICALL         : valC;
//   icode == IJXX && Cnd   : valC;
//   icode == IRET          : valM;
//   1                      : valP;
// ];
static addr_t compute_new_pc(int    icode,
                             bool   Cnd,
                             word_t valC,
                             word_t valM,
                             addr_t valP)
{
    if (icode == IHALT) {
        return valP; // HALT 不跳转
    }
    if (icode == ICALL) {
        return (addr_t)valC;
    }
    if (icode == IJXX && Cnd) {
        return (addr_t)valC;
    }
    if (icode == IRET) {
        return (addr_t)valM;
    }
// 默认顺序执行
    return valP;
}

// PC Update 阶段：更新 cpu_state_t 里的 stat 和 pc
static void pc_update_stage(cpu_state_t       *S,
                            const fetch_out_t *F,
                            const execute_out_t *E,
                            const memory_out_t  *M)
{
    // 1. 先根据错误 / 指令类型计算 Stat
    S->stat = compute_stat(F->imem_error,
                           M->dmem_error,
                           F->instr_valid,
                           F->icode);
    // 如果不是 SAOK，则保持 PC 在当前指令起始地址
    if (S->stat != SAOK) {
        addr_t start_pc = F->valP - (addr_t)instr_length(F->need_regids, F->need_valC);
        S->pc = start_pc;
        return;
    }
    // 2. 再按 HCL 规则计算 new_pc
    addr_t new_pc = compute_new_pc(F->icode,
                                   E->Cnd,
                                   F->valC,
                                   M->valM,
                                   F->valP);

    S->pc = new_pc;
}

void step_seq(cpu_state_t *S) {
    fetch_out_t   F = fetch_stage(S);
    decode_out_t  D = decode_stage(S, &F);
    execute_out_t E = execute_stage(S, &F, &D);
    memory_out_t  M = memory_stage(S, &F, &D, &E);

    writeback_stage(S, &F, &D, &E, &M);
    pc_update_stage(S, &F, &E, &M);
}

////////////////////////////// IO 支持（加载 yo / JSON 输出） //////////////////////////////

// 单个 hex 字符转值，非法返回 -1
static int hexval(char c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}

// 简单加载 .yo：从 stdin 解析“0xADDR: bytes...”行，写入内存
static bool load_yo(cpu_state_t *S) {
    char line[1024];
    bool any = false;
    while (fgets(line, sizeof(line), stdin)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;
        if (strncmp(p, "0x", 2) != 0) continue;
        char *colon = strchr(p, ':');
        if (!colon) continue;
        // 解析地址
        uint64_t addr = 0;
        char addrbuf[64] = {0};
        size_t ai = 0;
        char *q = p + 2;
        while (q < colon && ai + 1 < sizeof(addrbuf)) {
            if (isxdigit((unsigned char)*q)) addrbuf[ai++] = *q;
            q++;
        }
        addrbuf[ai] = '\0';
        if (ai == 0) continue;
        errno = 0;
        char *endptr = NULL;
        addr = (uint64_t) strtoull(addrbuf, &endptr, 16);
        if (errno != 0 || endptr == addrbuf) continue;
        // 解析冒号后的字节
        const char *bytes_area = colon + 1;
        uint8_t tmp[1024];
        size_t len = 0;
        while (*bytes_area) {
            while (*bytes_area && isspace((unsigned char)*bytes_area)) bytes_area++;
            if (!*bytes_area) break;
            int hi = hexval(*bytes_area++);
            if (hi < 0) break;
            while (*bytes_area && isspace((unsigned char)*bytes_area)) bytes_area++;
            if (!*bytes_area) { len = 0; break; } // 奇数个半字节
            int lo = hexval(*bytes_area++);
            if (lo < 0) { len = 0; break; }
            if (len >= sizeof(tmp)) { len = 0; break; }
            tmp[len++] = (uint8_t)((hi << 4) | lo);
        }
        if (len == 0) continue;
        for (size_t i = 0; i < len; ++i) {
            if (!mem_write_byte(&S->mem, (addr_t)(addr + i), tmp[i])) {
                S->stat = SADR;
                return false;
            }
        }
        any = true;
    }
    return any;
}

// JSON 输出：执行完一条指令后调用
static void print_json(const cpu_state_t *S, bool need_comma) {
    printf("\n    {\n");
    printf("        \"CC\": {\n            \"OF\": %d,\n            \"SF\": %d,\n            \"ZF\": %d\n        },\n",
           S->cc.OF?1:0, S->cc.SF?1:0, S->cc.ZF?1:0);
    printf("        \"MEM\": {\n");

    bool first = true;
    for(addr_t addr=0; addr < MEM_SIZE; addr += 8) {
        word_t val;
        if(!mem_read_word(&S->mem, addr, &val)) continue;
        if (val != 0) {
            if (!first) printf(",\n");
            printf("            \"%" PRIu64 "\": %" PRId64, (uint64_t)addr, (int64_t)val);
            first = false;
        }
    }
    printf("\n        },\n");
    printf("        \"PC\": %" PRIu64 ",\n", (uint64_t)S->pc);
    printf("        \"REG\": {\n");
    const char *names[] = { "r10","r11","r12","r13","r14","r8","r9","rax","rbp","rbx","rcx","rdi","rdx","rsi","rsp" };
    int idxs[] = { R_R10, R_R11, R_R12, R_R13, R_R14, R_R8, R_R9, R_RAX, R_RBP, R_RBX, R_RCX, R_RDI, R_RDX, R_RSI, R_RSP };
    for(int i=0;i<15;i++){
        printf("            \"%s\": %" PRId64, names[i], (int64_t)S->regs[idxs[i]]);
        if (i < 14) printf(",");
        printf("\n");
    }
    printf("        },\n");
    printf("        \"STAT\": %d\n", (int)S->stat + 1);
    printf("    }");
    if(need_comma) printf(",");
}

/* ===== 主程序：加载 yo -> 反复 step_seq -> 输出 JSON ===== */
int main(int argc, char *argv[]) {
    cpu_state_t S = {0};
    S.cc.ZF = 1;
    S.stat = SAOK;

    if(!load_yo(&S)){
        printf("[]\n");
        return 1;
    }

    printf("[");
    uint64_t steps = 0;
    while (S.stat == SAOK) {
        step_seq(&S);
        steps++;
        bool more = (S.stat == SAOK);
        print_json(&S, more);
        if (steps > 10000000ULL) { fprintf(stderr, "Too many steps, abort.\n"); break; }
    }
    printf("\n]\n");
    return 0;
}
