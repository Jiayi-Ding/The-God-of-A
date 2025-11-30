#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <errno.h>

#define MEM_SIZE (1<<20)   /* 1MB 内存 */
typedef enum { STAT_AOK=1, STAT_HLT=2, STAT_ADR=3, STAT_INS=4 } stat_t; // 处理器状态：正常、停机、地址错误、指令错误

enum { RAX=0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
       R8, R9, R10, R11, R12, R13, R14, RNONE=15 }; // 15个64位寄存器

enum {
    I_HALT=0, I_NOP=1, I_RRMOVQ=2, I_IRMOVQ=3, I_RMMOVQ=4,
    I_MRMOVQ=5, I_OPQ=6, I_JXX=7, I_CALL=8,
    I_RET=9, I_PUSHQ=0xA, I_POPQ=0xB, I_PRT=0xC
}; // 11种指令类型

enum { A_ADD=0, A_SUB=1, A_AND=2, A_XOR=3 }; // ALU的4种运算类型

// 细分的条件移动和跳转指令类型
typedef enum {
    C_ALWAYS=0,   // 无条件 (cmovq / jmp)
    C_LE=1,       // 小于等于 (cmovle / jle)
    C_L=2,        // 小于 (cmovl / jl)  
    C_E=3,        // 等于 (cmove / je)
    C_NE=4,       // 不等于 (cmovne / jne)
    C_GE=5,       // 大于等于 (cmovge / jge)
    C_G=6         // 大于 (cmovg / jg)
} condition_t;

/* ==================== CPU 状态结构体 ==================== */
typedef struct {
    uint8_t *mem;          // 1MB内存空间
    uint64_t pc;           // 程序计数器，指向下一条要执行的指令
    uint64_t reg[15];      // 15个64位通用寄存器
    bool ZF, SF, OF;       // 条件码：零标志、符号标志、溢出标志
    stat_t status;         // 处理器当前状态
    bool fatal;            // 致命错误标志
    
    // 流水线寄存器 - 用于在流水线阶段间传递数据
    struct {
        uint8_t icode;     // 指令代码
        uint8_t ifun;      // 指令功能码
        uint8_t rA;        // 寄存器A编号
        uint8_t rB;        // 寄存器B编号
        uint64_t valC;     // 立即数值
        uint64_t valP;     // 下一条指令的PC值
        uint64_t valA;     // 从寄存器A读出的值
        uint64_t valB;     // 从寄存器B读出的值
        uint64_t valE;     // ALU执行结果
        uint64_t valM;     // 从内存读取的值
        uint64_t dstE;     // 目标寄存器E（用于写入ALU结果）
        uint64_t dstM;     // 目标寄存器M（用于写入内存数据）
        const char* instr_name; // 当前执行的指令名称（用于调试）
    } pipe;
} cpu_t;

/* ==================== 内存访问函数 ==================== */

// 检查内存地址是否有效
static bool check_addr(uint64_t a, size_t len) {
    if (len == 0) return true;                    // 长度为0的访问总是有效
    if (a >= MEM_SIZE) return false;              // 地址超出内存范围
    return (a + len - 1) < MEM_SIZE;              // 检查结束地址是否越界
}

// 从内存读取1个字节
static uint8_t get_byte(cpu_t *cpu, uint64_t a, bool *ok) {
    if (!check_addr(a,1)) { 
        if(ok) *ok = false; 
        return 0; 
    }
    if(ok) *ok = true;
    return cpu->mem[a];  // 直接访问内存数组
}

// 从内存读取8个字节（小端序）
static uint64_t get_long(cpu_t *cpu, uint64_t a, bool *ok) {
    if (!check_addr(a,8)) { 
        if(ok) *ok = false; 
        return 0; 
    }
    uint64_t v = 0;
    // 小端序：低地址存放低位字节
    for (int i=0;i<8;i++) v |= ((uint64_t)cpu->mem[a+i]) << (8*i);
    if(ok) *ok = true;
    return v;
}

// 向内存写入1个字节
static bool set_byte(cpu_t *cpu, uint64_t a, uint8_t v) {
    if (!check_addr(a,1)) return false;
    cpu->mem[a] = v;
    return true;
}

// 向内存写入8个字节（小端序）
static bool set_long(cpu_t *cpu, uint64_t a, uint64_t v) {
    if (!check_addr(a,8)) return false;
    // 小端序：低位字节写入低地址
    for (int i=0;i<8;i++) cpu->mem[a+i] = (v >> (8*i)) & 0xFF;
    return true;
}

/* ==================== ALU 和条件码处理 ==================== */

// 根据条件码和功能码判断条件是否满足，同时检查功能码有效性
static bool cond(cpu_t *c, uint8_t f, const char** cond_name) {
    // 定义条件名称映射
    static const char* condition_names[] = {
        "always", "le", "l", "e", "ne", "ge", "g"
    };
    
    // 检查功能码有效性
    if (f > 6) {
        if (cond_name) *cond_name = "invalid";
        c->status = STAT_INS;  // 无效功能码，指令错误
        return false;
    }
    
    // 设置条件名称
    if (cond_name) *cond_name = condition_names[f];
    
    // 根据功能码判断条件
    switch(f){
        case C_ALWAYS: return true;                      // 无条件
        case C_LE: return (c->SF ^ c->OF) || c->ZF;      // 小于等于：SF≠OF 或 ZF=1
        case C_L: return (c->SF ^ c->OF);                // 小于：SF≠OF
        case C_E: return c->ZF;                          // 等于：ZF=1
        case C_NE: return !c->ZF;                        // 不等于：ZF=0
        case C_GE: return !(c->SF ^ c->OF);              // 大于等于：SF=OF
        case C_G: return !(c->SF ^ c->OF) && !c->ZF;     // 大于：SF=OF 且 ZF=0
    }
    
    return false;  // 不应该执行到这里
}

// 获取条件移动指令的名称
static const char* get_cmov_name(uint8_t ifun) {
    static const char* cmov_names[] = {
        "cmovq", "cmovle", "cmovl", "cmove", "cmovne", "cmovge", "cmovg"
    };
    return (ifun <= 6) ? cmov_names[ifun] : "cmov_invalid";
}

// 获取跳转指令的名称  
static const char* get_jump_name(uint8_t ifun) {
    static const char* jump_names[] = {
        "jmp", "jle", "jl", "je", "jne", "jge", "jg"
    };
    return (ifun <= 6) ? jump_names[ifun] : "j_invalid";
}

// 算术逻辑单元：执行运算并设置条件码
static uint64_t alu(cpu_t *c, uint8_t f, uint64_t a, uint64_t b) {
    long long A=a, B=b, R=0;  // 使用有符号数检测溢出
    bool of=false;            // 溢出标志
    
    switch(f){
        case A_ADD: 
            R = B + A;
            // 溢出检测：同号相加结果变号
            of = ((A<0&&B<0&&R>=0)||(A>=0&&B>=0&&R<0)); 
            break;
        case A_SUB: 
            R = B - A;
            // 溢出检测：符号不同的数相减
            of = ((B<0&&A>=0&&R>=0)||(B>=0&&A<0&&R<0)); 
            break;
        case A_AND: R = B & A; break;  // 按位与
        case A_XOR: R = B ^ A; break;  // 按位异或
    }
    
    uint64_t res = (uint64_t)R;
    // 设置条件码
    c->ZF = (res == 0);              // 零标志：结果为0
    c->SF = ((long long)res < 0);    // 符号标志：结果为负
    c->OF = of;                      // 溢出标志
    return res;
}

/* ==================== 取指阶段 ==================== */

// 取指阶段：从内存读取指令并解析
static bool fetch_stage(cpu_t *c) {
    bool ok;
    // 读取指令的第一个字节
    uint8_t b0 = get_byte(c, c->pc, &ok);
    if (!ok) { 
        c->status = STAT_ADR;  // 地址错误
        return false; 
    }
    
    // 解析指令字节：高4位是指令码，低4位是功能码
    c->pipe.icode = (b0 >> 4) & 0xF;
    c->pipe.ifun = b0 & 0xF;
    c->pipe.valP = c->pc + 1;  // 默认下一条指令地址
    c->pipe.instr_name = "unknown"; // 默认指令名称
    
    // HALT指令特殊处理：直接停机
    if (c->pipe.icode == I_HALT) {
        c->pipe.instr_name = "halt";
        c->status = STAT_HLT;
        return false;
    }
    
    // 读取寄存器字节（对于需要寄存器的指令）
    if (c->pipe.icode == I_RRMOVQ || c->pipe.icode == I_IRMOVQ || 
        c->pipe.icode == I_RMMOVQ || c->pipe.icode == I_MRMOVQ ||
        c->pipe.icode == I_OPQ || c->pipe.icode == I_PUSHQ || 
        c->pipe.icode == I_POPQ) {
        
        uint8_t reg_byte = get_byte(c, c->pipe.valP, &ok);
        if (!ok) { 
            c->status = STAT_ADR; 
            return false; 
        }
        
        // 解析寄存器编号：高4位是rA，低4位是rB
        c->pipe.rA = (reg_byte >> 4) & 0xF;
        c->pipe.rB = reg_byte & 0xF;
        c->pipe.valP++;  // PC指向下一个字节
    } else {
        c->pipe.rA = RNONE;
        c->pipe.rB = RNONE;
    }
    
    // 读取立即数（对于需要立即数的指令）
    if (c->pipe.icode == I_IRMOVQ || c->pipe.icode == I_RMMOVQ || 
        c->pipe.icode == I_MRMOVQ || c->pipe.icode == I_JXX || 
        c->pipe.icode == I_CALL) {
        
        c->pipe.valC = get_long(c, c->pipe.valP, &ok);
        if (!ok) { 
            c->status = STAT_ADR; 
            return false; 
        }
        c->pipe.valP += 8;  // PC跳过8字节立即数
    } else {
        c->pipe.valC = 0;
    }
    
    // 设置指令名称（用于调试和错误报告）
    switch (c->pipe.icode) {
        case I_NOP: c->pipe.instr_name = "nop"; break;
        case I_RRMOVQ: c->pipe.instr_name = get_cmov_name(c->pipe.ifun); break;
        case I_IRMOVQ: c->pipe.instr_name = "irmovq"; break;
        case I_RMMOVQ: c->pipe.instr_name = "rmmovq"; break;
        case I_MRMOVQ: c->pipe.instr_name = "mrmovq"; break;
        case I_OPQ: c->pipe.instr_name = "opq"; break;
        case I_JXX: c->pipe.instr_name = get_jump_name(c->pipe.ifun); break;
        case I_CALL: c->pipe.instr_name = "call"; break;
        case I_RET: c->pipe.instr_name = "ret"; break;
        case I_PUSHQ: c->pipe.instr_name = "pushq"; break;
        case I_POPQ: c->pipe.instr_name = "popq"; break;
    }
    
    return true;
}

/* ==================== 译码阶段 ==================== */

// 译码阶段：从寄存器文件读取操作数
static void decode_stage(cpu_t *c) {
    // 初始化流水线寄存器
    c->pipe.valA = 0;
    c->pipe.valB = 0;
    c->pipe.dstE = RNONE;  // 默认无目标寄存器
    c->pipe.dstM = RNONE;
    
    // 根据指令类型读取寄存器操作数
    switch (c->pipe.icode) {
        case I_RRMOVQ:   // 条件寄存器移动
        case I_RMMOVQ:   // 寄存器到内存
        case I_OPQ:      // 算术运算
        case I_PUSHQ:    // 压栈
            // 读取两个寄存器操作数
            if (c->pipe.rA < 15) c->pipe.valA = c->reg[c->pipe.rA];
            if (c->pipe.rB < 15) c->pipe.valB = c->reg[c->pipe.rB];
            break;
            
        case I_IRMOVQ:   // 立即数到寄存器
        case I_MRMOVQ:   // 内存到寄存器
            // 只读取基址寄存器
            if (c->pipe.rB < 15) c->pipe.valB = c->reg[c->pipe.rB];
            break;
            
        case I_CALL:     // 函数调用
        case I_RET:      // 函数返回
        case I_POPQ:     // 出栈
            // 使用栈指针
            c->pipe.valA = c->reg[RSP];
            c->pipe.valB = c->reg[RSP];
            break;
            
        case I_JXX:      // 条件跳转
            // 跳转指令不需要寄存器操作数
            break;
        case I_PRT:
    }
    
    // 设置目标寄存器（写回阶段使用）
    switch (c->pipe.icode) {
        case I_RRMOVQ:   // 条件寄存器移动
            // 注意：这里不检查条件，条件检查在执行阶段后决定是否写回
            if (c->pipe.rB < 15) c->pipe.dstE = c->pipe.rB;
            break;
        case I_IRMOVQ:   // 立即数加载
            if (c->pipe.rB < 15) c->pipe.dstE = c->pipe.rB;
            break;
        case I_OPQ:      // 算术运算
            if (c->pipe.rB < 15) c->pipe.dstE = c->pipe.rB;
            break;
        case I_PUSHQ:    // 压栈
        case I_POPQ:     // 出栈
        case I_CALL:     // 调用
        case I_RET:      // 返回
            // 这些指令都会更新栈指针
            c->pipe.dstE = RSP;
            break;
    }
    
    // 设置内存目标寄存器（用于加载指令）
    if (c->pipe.icode == I_POPQ || c->pipe.icode == I_MRMOVQ) {
        if (c->pipe.rA < 15) c->pipe.dstM = c->pipe.rA;
    }
}

/* ==================== 执行阶段 ==================== */

// 执行阶段：ALU运算和地址计算
static void execute_stage(cpu_t *c) {
    c->pipe.valE = 0;  // 初始化ALU结果
    
    switch (c->pipe.icode) {
        case I_RRMOVQ:   // 寄存器移动
            c->pipe.valE = c->pipe.valA;  // 直接传递源寄存器值
            break;
            
        case I_IRMOVQ:   // 立即数加载
            c->pipe.valE = c->pipe.valC;  // 直接使用立即数
            break;
            
        case I_RMMOVQ:   // 存储指令
            // 计算目标内存地址：valB(基址) + valC(偏移)
            c->pipe.valE = c->pipe.valB + c->pipe.valC;
            break;
            
        case I_MRMOVQ:   // 加载指令  
            // 计算源内存地址：valB(基址) + valC(偏移)
            c->pipe.valE = c->pipe.valB + c->pipe.valC;
            break;
            
        case I_OPQ:      // 算术运算
            c->pipe.valE = alu(c, c->pipe.ifun, c->pipe.valA, c->pipe.valB);
            break;
            
        case I_JXX:      // 跳转指令
            c->pipe.valE = c->pipe.valC;  // 跳转目标地址
            break;
            
        case I_CALL:     // 函数调用
            c->pipe.valE = c->pipe.valB - 8;  // 新栈指针：原SP-8
            break;
            
        case I_RET:      // 函数返回
            c->pipe.valE = c->pipe.valB + 8;  // 新栈指针：原SP+8
            break;
            
        case I_PUSHQ:    // 压栈
            c->pipe.valE = c->pipe.valB - 8;  // 新栈指针：原SP-8
            break;
            
        case I_POPQ:     // 出栈
            c->pipe.valE = c->pipe.valB + 8;  // 新栈指针：原SP+8
            break;
            
        case I_PRT: {
            if(c->pipe.ifun == 0)
                printf("%" PRId64 "\n", (int64_t)c->pipe.rA);
            if(c->pipe.ifun == 1)
                printf("%" PRId64 "\n", (int64_t)c->pipe.valM);
            return;
        }

        default:
            // 对于不支持的指令，valE保持为0
            break;
    }
}

/* ==================== 访存阶段 ==================== */

// 访存阶段：内存读写操作
static bool memory_stage(cpu_t *c) {
    c->pipe.valM = 0;  // 初始化内存读取值
    bool ok;
    
    switch (c->pipe.icode) {
        case I_RMMOVQ:   // 寄存器到内存
            // 将寄存器值存储到计算出的内存地址
            if (!set_long(c, c->pipe.valE, c->pipe.valA)) {
                c->status = STAT_ADR;
                return false;
            }
            break;
            
        case I_MRMOVQ:   // 内存到寄存器
            // 从计算出的内存地址加载值
            c->pipe.valM = get_long(c, c->pipe.valE, &ok);
            if (!ok) {
                c->status = STAT_ADR;
                return false;
            }
            break;
            
        case I_PUSHQ:    // 压栈
            // 特殊处理：PUSHQ %rsp 要压入原始SP值
            uint64_t value_to_push = c->pipe.valA;
            if (c->pipe.rA == RSP) {
                value_to_push = c->pipe.valB; // 使用旧的栈指针值
            }
            if (!set_long(c, c->pipe.valE, value_to_push)) {
                c->status = STAT_ADR;
                return false;
            }
            break;
            
        case I_POPQ:     // 出栈
        case I_RET:      // 返回
            // 从栈顶读取数据
            c->pipe.valM = get_long(c, c->pipe.valA, &ok);
            if (!ok) {
                c->status = STAT_ADR;
                return false;
            }
            break;
            
        case I_CALL:     // 函数调用
            // 将返回地址压栈
            if (!set_long(c, c->pipe.valE, c->pipe.valP)) {
                c->status = STAT_ADR;
                return false;
            }
            break;
    }
    
    return true;
}

/* ==================== 写回阶段 ==================== */

// 写回阶段：将结果写入寄存器文件
static void writeback_stage(cpu_t *c) {
    // 对于条件移动指令，需要检查条件后再决定是否写回
    if (c->pipe.icode == I_RRMOVQ) {
        const char* cond_name;
        bool condition_met = cond(c, c->pipe.ifun, &cond_name);
        
        // 如果条件不满足，取消目标寄存器写入
        if (!condition_met) {
            c->pipe.dstE = RNONE;
        }
        
        // 如果条件检查过程中出现错误（如无效功能码），状态已被设置，直接返回
        if (c->status != STAT_AOK) return;
    }
    
    // 写入目标寄存器E（ALU结果或地址计算结果）
    if (c->pipe.dstE != RNONE && c->pipe.dstE < 15) {
        c->reg[c->pipe.dstE] = c->pipe.valE;
    }
    
    // 写入目标寄存器M（内存读取的数据）
    if (c->pipe.dstM != RNONE && c->pipe.dstM < 15) {
        c->reg[c->pipe.dstM] = c->pipe.valM;
    }
}

/* ==================== 更新PC阶段 ==================== */

// 更新PC阶段：确定下一条指令地址
static void pc_update_stage(cpu_t *c) {
    switch (c->pipe.icode) {
        case I_JXX:      // 条件跳转
            // 根据条件码决定是否跳转
            const char* cond_name;
            bool should_jump = cond(c, c->pipe.ifun, &cond_name);
            
            // 如果条件检查过程中出现错误，状态已被设置，使用默认PC
            if (c->status != STAT_AOK) {
                c->pc = c->pipe.valP;
            } else if (should_jump) {
                c->pc = c->pipe.valE; // 跳转到目标地址
            } else {
                c->pc = c->pipe.valP; // 顺序执行下一条指令
            }
            break;
            
        case I_CALL:     // 函数调用
            // 跳转到函数入口地址
            c->pc = c->pipe.valE;
            break;
            
        case I_RET:      // 函数返回
            // 从栈中弹出返回地址
            c->pc = c->pipe.valM;
            break;
            
        default:
            // 其他指令：顺序执行
            c->pc = c->pipe.valP;
            break;
    }
}

/* ==================== 主步进函数 ==================== */

// 执行一条指令的完整流水线
static void step(cpu_t *c) {
    if (c->status != STAT_AOK) return;  // 只有正常状态才执行
    
    // 阶段1: 取指 - 从内存读取指令
    if (!fetch_stage(c)) return;
    
    // 阶段2: 译码 - 从寄存器文件读取操作数
    decode_stage(c);
    
    // 阶段3: 执行 - ALU运算和地址计算
    execute_stage(c);
    
    // 阶段4: 访存 - 内存读写操作
    if (!memory_stage(c)) return;
    
    // 阶段5: 写回 - 将结果写入寄存器
    writeback_stage(c);
    
    // 阶段6: 更新PC - 确定下一条指令地址
    pc_update_stage(c);
    
    // 调试输出：可以取消注释来查看每条执行的指令
    // printf("Executed: %s\n", c->pipe.instr_name);
}

/* ==================== JSON 输出函数 ==================== */

// 以JSON格式输出处理器当前状态
static void print_json(cpu_t *c) {
    printf("\n    {\n");
    // 输出条件码
    printf("        \"CC\": {\n            \"OF\": %d,\n            \"SF\": %d,\n            \"ZF\": %d\n        },\n",
           c->OF?1:0, c->SF?1:0, c->ZF?1:0);
    
    // 输出非零内存内容
    printf("        \"MEM\": {\n");
    bool first = true;
    for(uint64_t addr=0; addr < MEM_SIZE; addr += 8) {
        bool ok;
        uint64_t val = get_long(c, addr, &ok);
        if(!ok) continue;
        if (val != 0) {  // 只输出非零值
            if (!first) printf(",\n");
            int64_t signed_val = (int64_t)val;  // 转换为有符号数输出
            printf("        \"%" PRIu64 "\": %" PRId64, addr, signed_val);
            first = false;
        }
    }
    printf("\n        },\n");
    
    // 输出程序计数器
    printf("        \"PC\": %" PRIu64 ",\n", c->pc);
    
    // 输出所有寄存器值
    printf("        \"REG\": {\n");
    const char *names[] = { "r10","r11","r12","r13","r14","r8","r9","rax","rbp","rbx","rcx","rdi","rdx","rsi","rsp" };
    int idxs[] = { R10, R11, R12, R13, R14, R8, R9, RAX, RBP, RBX, RCX, RDI, RDX, RSI, RSP };
    for(int i=0;i<15;i++){
        printf("            \"%s\": %" PRId64, names[i], (int64_t)c->reg[idxs[i]]);
        if (i < 14) printf(",");
        printf("\n");
    }
    printf("        },\n");
    
    // 输出处理器状态
    printf("        \"STAT\": %d\n", (int)c->status);
    printf("    }");
    if(c->status == STAT_AOK) printf(",");  // 正常状态后面加逗号
}

/* ==================== 文件加载函数 ==================== */

// 十六进制字符转换
static int hexval(char c){
    if(c>='0'&&c<='9')return c-'0';           // 数字字符
    if(c>='a'&&c<='f')return c-'a'+10;        // 小写字母
    if(c>='A'&&c<='F')return c-'A'+10;        // 大写字母
    return -1;  // 无效字符
}

// 解析十六进制字节序列
static int parse_hex_bytes(const char *s, uint8_t *out, size_t maxlen) {
    size_t len = 0;
    while (*s) {
        // 跳过空白字符
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;

        // 解析高4位
        int hi = hexval(*s++);
        if (hi < 0) break;

        // 跳过中间空白
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) return -1; /* 半个字节 */

        // 解析低4位
        int lo = hexval(*s++);
        if (lo < 0) break;

        // 检查缓冲区溢出
        if (len >= maxlen) return -1;
        // 组合字节
        out[len++] = (uint8_t)((hi << 4) | lo);
    }
    return (int)len;
}

// 加载.yo格式的机器码文件
static bool load_yo(cpu_t *c) {
    char line[1024];
    bool any = false;  // 记录是否成功加载了任何代码
    
    // 逐行读取标准输入
    while (fgets(line, sizeof(line), stdin)) {
        char *s = line;
        // 跳过前导空白
        while (*s && isspace(*s)) s++;
        
        // 跳过空行和注释
        if (*s == '\0' || *s == '#') continue;
        
        // 检查地址标识"0x"
        if (strncmp(s, "0x", 2) != 0) continue;
        
        // 查找冒号分隔符
        char *colon = strchr(s, ':');
        if (!colon) continue;
        
        // 提取地址部分（"0x"和":"之间的十六进制数）
        char addrbuf[64] = {0};
        size_t ai = 0;
        char *p = s + 2;  // 跳过"0x"
        while (p < colon && ai + 1 < sizeof(addrbuf)) {
            if (isxdigit(*p)) addrbuf[ai++] = *p;
            p++;
        }
        addrbuf[ai] = '\0';
        
        if (ai == 0) continue;  // 无有效地址
        
        // 安全转换地址字符串为数值
        errno = 0;
        char *endptr = NULL;
        uint64_t addr = (uint64_t) strtoull(addrbuf, &endptr, 16);
        if (errno != 0 || endptr == addrbuf) continue;
        
        // 解析机器码字节（冒号后面的部分）
        const char *bytes_area = colon + 1;
        uint8_t tmp[1024];
        int nb = parse_hex_bytes(bytes_area, tmp, sizeof(tmp));
        if (nb < 0) continue;
        
        // 将机器码写入内存
        for (int i = 0; i < nb; ++i) {
            if (!set_byte(c, addr + i, tmp[i])) {
                c->fatal = true;
                c->status = STAT_ADR;
                return false;
            }
            any = true;  // 标记成功加载
        }
    }
    return any;
}

/* ==================== CPU 初始化函数 ==================== */

// 创建新的CPU实例
static cpu_t *cpu_new() {
    cpu_t *c = calloc(1, sizeof(cpu_t));
    if (!c) return NULL;
    
    // 分配1MB内存
    c->mem = calloc(1, MEM_SIZE);
    if (!c->mem) { 
        free(c); 
        return NULL; 
    }
    
    // 初始化CPU状态
    c->pc = 0;
    c->status = STAT_AOK;
    c->SF = c->OF = 0;
    c->ZF = 1;  // 初始时零标志为1
    for(int i=0;i<15;i++) {
        c->reg[i] = 0;
    }
    c->reg[RSP] = 0;  // 栈指针初始化为0
    
    return c;
}

// 释放CPU资源
static void cpu_free(cpu_t *c){
    if (!c) return;
    if (c->mem) free(c->mem);
    free(c);
}

/* ==================== 主函数 ==================== */

int main() {
    // 阶段1: 初始化CPU
    cpu_t *cpu = cpu_new();
    if (!cpu) {
        fprintf(stderr, "Failed to initialize CPU\n");
        return 1;
    }

    // 阶段2: 加载机器码
    if(!load_yo(cpu)){
        printf("[]\n");  // 无有效代码时输出空数组
        return 1;
    }

    // 阶段3: 执行循环
    uint64_t steps = 0;
    printf("[");  // 开始JSON数组
    
    do{
        step(cpu);               // 执行一条指令
        steps++;
        print_json(cpu);         // 输出当前状态

        // 安全防护：防止无限循环
        if (steps > 10000000ULL) {
            fprintf(stderr, "Too many steps, abort.\n");
            break;
        }
    }while (cpu->status == STAT_AOK);  // 正常状态继续执行
    
    // 阶段4: 清理退出
    printf("\n]\n");
    cpu_free(cpu);
    return 0;
}