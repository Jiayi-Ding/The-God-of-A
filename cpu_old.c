#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <errno.h>

/*
 * Y86-64 Simulator（结构化实现）
 * 流程：取指 -> 译码 -> 执行 -> 访存 -> 写回 -> PC 更新
 */

/*
关于get_XXX 与 fetch_XXX
get：   查看并返回数值，不自动更新pc(cpu, some_addr, &ok)
fetch： 取指，自动更新pc(cpu, &opcode)
*/

/* ===== 常量与枚举 ===== */
#define MEM_SIZE (1<<20) /* 1MB 模拟内存 */

typedef enum { STAT_AOK=1, STAT_HLT=2, STAT_ADR=3, STAT_INS=4 } stat_t;

enum reg_id {
    RAX=0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, RNONE=15
};

enum icode {
    I_HALT=0, I_NOP=1, I_RRMOVQ=2, I_IRMOVQ=3, I_RMMOVQ=4,
    I_MRMOVQ=5, I_OPQ=6, I_JXX=7, I_CALL=8,
    I_RET=9, I_PUSHQ=0xA, I_POPQ=0xB, I_PRT=0xC
};

enum op_fun { A_ADD=0, A_SUB=1, A_AND=2, A_XOR=3 };

/* ===== CPU 状态 ===== */
typedef struct {
    uint8_t  *mem;
    uint64_t pc;
    uint64_t reg[15];
    bool ZF, SF, OF;
    stat_t status;
} cpu_t;

/* 当前指令字段容器，便于分阶段处理 */
typedef struct {
    uint8_t icode, ifun;
    uint8_t rA, rB;
    uint64_t valC;  /* 立即数/目标/偏移 */
    uint64_t valP;  /* 取指后 PC（下一字节位置） */
} instr_t;

/* ===== 工具函数：地址检查与内存读写 ===== */
static bool check_addr(uint64_t a, size_t len) {
    if (len == 0) return true;
    if (a >= MEM_SIZE) return false;
    return (a + len - 1) < MEM_SIZE;
}

static uint8_t get_byte(cpu_t *cpu, uint64_t a, bool *ok) {
    if (!check_addr(a,1)) { if(ok) *ok = false; return 0; }
    if(ok) *ok = true;
    return cpu->mem[a];
}

static uint64_t get_long(cpu_t *cpu, uint64_t a, bool *ok) {
    if (!check_addr(a,8)) { if(ok) *ok = false; return 0; }
    uint64_t v = 0;
    for (int i=0;i<8;i++) v |= ((uint64_t)cpu->mem[a+i]) << (8*i);
    if(ok) *ok = true;
    return v;
}

static bool set_byte(cpu_t *cpu, uint64_t a, uint8_t v) {
    if (!check_addr(a,1)) return false;
    cpu->mem[a] = v;
    return true;
}

static bool set_long(cpu_t *cpu, uint64_t a, uint64_t v) {
    if (!check_addr(a,8)) return false;
    for (int i=0;i<8;i++) cpu->mem[a+i] = (v >> (8*i)) & 0xFF;
    return true;
}

/* ===== 条件判断与 ALU ===== */
static bool cond(cpu_t *c, uint8_t f) {
    switch(f){
        case 0: return true;                          /* uncond */
        case 1: return (c->SF ^ c->OF) || c->ZF;      /* le */
        case 2: return (c->SF ^ c->OF);               /* l */
        case 3: return c->ZF;                         /* e */
        case 4: return !c->ZF;                        /* ne */
        case 5: return !(c->SF ^ c->OF);              /* ge */
        case 6: return !(c->SF ^ c->OF) && !c->ZF;    /* g */
    }
    return false;
}

static uint64_t alu(cpu_t *c, uint8_t f, uint64_t a, uint64_t b) {
    long long A=a, B=b, R=0;
    bool of=false;
    switch(f){
        case A_ADD: R = B + A; of = ((A<0&&B<0&&R>=0)||(A>=0&&B>=0&&R<0)); break;
        case A_SUB: R = B - A; of = ((B<0&&A>=0&&R>=0)||(B>=0&&A<0&&R<0)); break;
        case A_AND: R = B & A; break;
        case A_XOR: R = B ^ A; break;
    }

    uint64_t res = (uint64_t)R;

    c->ZF = (res == 0);
    c->SF = ((long long)res < 0);
    c->OF = of;

    return res;
}

/* ===== 取指/译码辅助 ===== */
static bool fetch_byte(cpu_t *c, uint8_t *r) {
    bool ok;
    *r = get_byte(c, c->pc, &ok);
    if (!ok) { c->status = STAT_ADR; return false; }
    c->pc += 1;
    return true;
}

static bool fetch_reg(cpu_t *c, uint8_t *rA, uint8_t *rB) {
    uint8_t b;
    if(!fetch_byte(c,&b)) return false;
    *rA = (b >> 4) & 0xF;
    *rB = b & 0xF;
    return true;
}

static bool fetch_long(cpu_t *c, uint64_t *v) {
    bool ok;
    *v = get_long(c, c->pc, &ok);
    if (!ok) { c->status = STAT_ADR; return false; }
    c->pc += 8;
    return true;
}

/* 带地址错误处理的 8 字节读内存封装 */    //这是添加了的
static bool load_long_or_fail(cpu_t *c, uint64_t addr,
                              uint64_t *out, uint64_t start_pc) {
    bool ok;
    *out = get_long(c, addr, &ok);
    if (!ok) {
        /* 统一的地址错误处理逻辑 */
        c->status = STAT_ADR;
        c->pc = start_pc;
        return false;
    }
    return true;
}

/* ===== 单步执行 ===== */
static void step(cpu_t *c){
    if (c->status != STAT_AOK) return;

    instr_t ins = {0};
    //bool ok;
    uint64_t start_pc = c->pc; /* 记录本条指令的起始地址，HALT 等用 */
#define ADR_FAIL do { c->status = STAT_ADR; c->pc = start_pc; return; } while(0)
    
    //复用fetch_byte函数
    uint8_t b0={0};
    if(!fetch_byte(c,&b0))  return;

    // uint8_t b0 = get_byte(c, c->pc, &ok);
    // if(!ok){ c->status = STAT_ADR; return; }
    // c->pc += 1;

    ins.icode = (b0 >> 4) & 0xF;
    ins.ifun  = (b0 & 0xF);
    ins.valP = c->pc; /* 当前取指后位置，作为默认下一条 PC */

    /* 译码：根据指令格式读取 rA/rB/valC */
    switch (ins.icode) {
        case I_RRMOVQ: case I_OPQ: case I_PUSHQ: case I_POPQ:
            if(!fetch_reg(c,&ins.rA,&ins.rB)) return;
            break;
        case I_PRT:
            //if reg
            if(ins.ifun == 0){
                if(!fetch_reg(c,&ins.rA,&ins.rB)) return;
            }
            //if addr
            if(ins.ifun == 1){
                if(!fetch_long(c,&ins.valC)) return;
            }
            break;
        case I_IRMOVQ: case I_RMMOVQ: case I_MRMOVQ:
            if(!fetch_reg(c,&ins.rA,&ins.rB)) return;
            if(!fetch_long(c,&ins.valC)) return;
            break;
        case I_JXX: case I_CALL:
            if(!fetch_long(c,&ins.valC)) return;
            break;
        case I_RET: case I_NOP: case I_HALT:
            break;
        default:
            c->status = STAT_INS; return;
    }
    ins.valP = c->pc; /* 译码后 pc 已指向下一条 */

    /* 执行/访存/写回/PC 更新 */
    switch (ins.icode) {
        case I_NOP:
            break;
        case I_HALT:
            c->status = STAT_HLT;
            c->pc = start_pc; /* HALT 时 PC 应指向 halt 字节所在地址 */
            break;
        case I_RRMOVQ:
            if (cond(c, ins.ifun) && ins.rA < 15 && ins.rB < 15)
                c->reg[ins.rB] = c->reg[ins.rA];
            break;
        case I_IRMOVQ:
            if (ins.rB < 15) c->reg[ins.rB] = ins.valC;
            break;
        case I_RMMOVQ: {
            uint64_t addr = c->reg[ins.rB] + ins.valC;
            if(!set_long(c, addr, c->reg[ins.rA])) { ADR_FAIL; }
            break;
        }
        // case I_MRMOVQ: {
        //     uint64_t addr = c->reg[ins.rB] + ins.valC;
        //     uint64_t v = get_long(c, addr, &ok);
        //     if(!ok){ ADR_FAIL; }
        //     if(ins.rA < 15) c->reg[ins.rA] = v;
        //     break;
        // }
        case I_MRMOVQ: {
            uint64_t addr = c->reg[ins.rB] + ins.valC;
            uint64_t v;
            if (!load_long_or_fail(c, addr, &v, start_pc)) return;
            if (ins.rA < 15) c->reg[ins.rA] = v;
            break;
        }
        case I_OPQ:
            if(ins.rA < 15 && ins.rB < 15)
                c->reg[ins.rB] = alu(c, ins.ifun, c->reg[ins.rA], c->reg[ins.rB]);
            break;
        case I_JXX:
            if(cond(c, ins.ifun)) c->pc = ins.valC;
            break;
        case I_CALL:
            c->reg[RSP] -= 8;
            if(!set_long(c, c->reg[RSP], ins.valP)) { ADR_FAIL; }
            c->pc = ins.valC;
            break;
        // case I_RET: {
        //     uint64_t dest = get_long(c, c->reg[RSP], &ok);
        //     if(!ok){ ADR_FAIL; }
        //     c->reg[RSP] += 8;
        //     c->pc = dest;
        //     break;
        // }
        case I_RET: {
            uint64_t dest;
            if (!load_long_or_fail(c, c->reg[RSP], &dest, start_pc)) return;
            c->reg[RSP] += 8;
            c->pc = dest;
            break;
        }
        case I_PUSHQ: {
            if(ins.rA >= 15) break;
            uint64_t valA = c->reg[ins.rA]; /* 保存原始寄存器值，推 rsp 时需旧值 */
            c->reg[RSP] -= 8;
            if(!set_long(c, c->reg[RSP], valA)) { ADR_FAIL; }
            break;
        }
        // case I_POPQ: {
        //     if(ins.rA >= 15) { c->reg[RSP] += 8; break; }
        //     uint64_t v = get_long(c, c->reg[RSP], &ok);
        //     if(!ok){ ADR_FAIL; }
        //     c->reg[RSP] += 8;
        //     c->reg[ins.rA] = v;
        //     break;
        // }
        case I_POPQ: {
            if (ins.rA >= 15) { c->reg[RSP] += 8; break; }
            uint64_t v;
            if (!load_long_or_fail(c, c->reg[RSP], &v, start_pc)) return;
            c->reg[RSP] += 8;
            c->reg[ins.rA] = v;
            break;
        }
        // case I_PRT:{
        //     if(ins.ifun == 0 ){
        //         printf("%ld\n", (int64_t)c->reg[ins.rA]);
        //     } 
        //     else if(ins.ifun == 1){
        //         uint64_t val = get_long(c, ins.valC, &ok);
        //         if(!ok){ ADR_FAIL; }
        //         printf("MEM[%" PRIu64 "] : %ld\n", ins.valC, (int64_t)val);
        //     }
        //     break;
        // }
        case I_PRT:{
            if(ins.ifun == 0 ){
                printf("\n%ld\n", (int64_t)c->reg[ins.rA]);
            } 
            else if(ins.ifun == 1){
                uint64_t val = {0};
                if (!load_long_or_fail(c, ins.valC, &val, start_pc)) return;
                printf("\n%ld\n", (int64_t)val);
            }
            break;
        }
        default:
            c->status = STAT_INS;
    }
#undef ADR_FAIL
}

/* ===== JSON 状态输出 ===== */
static void print_json(cpu_t *c) {
    printf("\n    {\n");
    printf("        \"CC\": {\n            \"OF\": %d,\n            \"SF\": %d,\n            \"ZF\": %d\n        },\n",
           c->OF?1:0, c->SF?1:0, c->ZF?1:0);
    printf("        \"MEM\": {\n");

    bool first = true;
    for(uint64_t addr=0; addr < MEM_SIZE; addr += 8) {
        bool ok;
        uint64_t val = get_long(c, addr, &ok);
        if(!ok) continue;
        if (val != 0) {
            if (!first) printf(",\n");
            int64_t signed_val = (int64_t)val;
            printf("        \"%" PRIu64 "\": %" PRId64, addr, signed_val);
            first = false;
        }
    }
    printf("\n        },\n");
    printf("        \"PC\": %" PRIu64 ",\n", c->pc);
    printf("        \"REG\": {\n");
    const char *names[] = { "r10","r11","r12","r13","r14","r8","r9","rax","rbp","rbx","rcx","rdi","rdx","rsi","rsp" };
    int idxs[] = { R10, R11, R12, R13, R14, R8, R9, RAX, RBP, RBX, RCX, RDI, RDX, RSI, RSP };
    for(int i=0;i<15;i++){
        printf("            \"%s\": %" PRId64, names[i], (int64_t)c->reg[idxs[i]]);
        if (i < 14) printf(",");
        printf("\n");
    }
    printf("        },\n");
    printf("        \"STAT\": %d\n", (int)c->status);
    printf("    }");
    if(c->status == STAT_AOK) printf(",");
}

/* ===== 加载 .yo 文件 ===== */
static int hexval(char c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}

static int parse_hex_bytes(const char *s, uint8_t *out, size_t maxlen) {
    size_t len = 0;
    while (*s) {
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;
        int hi = hexval(*s++);
        if (hi < 0) break;
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) return -1;
        int lo = hexval(*s++);
        if (lo < 0) break;
        if (len >= maxlen) return -1;
        out[len++] = (uint8_t)((hi << 4) | lo);
    }
    return (int)len;
}

static bool load_yo(cpu_t *c) {
    char line[1024];
    bool any = false;
    while (fgets(line, sizeof(line), stdin)) {
        char *s = line;
        while (*s && isspace((unsigned char)*s)) s++;
        if (*s == '\0' || *s == '#') continue;
        if (strncmp(s, "0x", 2) != 0) continue;
        char *colon = strchr(s, ':');
        if (!colon) continue;
        uint64_t addr = 0;
        char addrbuf[64] = {0};
        size_t ai = 0;
        char *p = s + 2;
        while (p < colon && ai + 1 < sizeof(addrbuf)) {
            if (isxdigit((unsigned char)*p)) addrbuf[ai++] = *p;
            p++;
        }
        addrbuf[ai] = '\0';
        if (ai == 0) continue;
        errno = 0;
        char *endptr = NULL;
        addr = (uint64_t) strtoull(addrbuf, &endptr, 16);
        if (errno != 0 || endptr == addrbuf) continue;
        const char *bytes_area = colon + 1;
        uint8_t tmp[1024];
        int nb = parse_hex_bytes(bytes_area, tmp, sizeof(tmp));
        if (nb < 0) continue;
        for (int i = 0; i < nb; ++i) {
            if (!set_byte(c, addr + (uint64_t)i, tmp[i])) {
                c->status = STAT_ADR;
                return false;
            }
            any = true;
        }
    }
    return any;
}

/* ===== CPU 构造/销毁 ===== */
static cpu_t *cpu_new() {
    cpu_t *c = calloc(1, sizeof(cpu_t));
    if (!c) return NULL;
    c->mem = calloc(1, MEM_SIZE);
    if (!c->mem) { free(c); return NULL; }
    c->pc = 0;
    c->status = STAT_AOK;
    c->ZF = 1;
    return c;
}

static void cpu_free(cpu_t *c){
    if (!c) return;
    free(c->mem);
    free(c);
}

/* ===== 主程序 ===== */
int main(int argc, char *argv[]) {
    cpu_t *cpu = cpu_new();
    bool debug_mode = false;

    if(!cpu || !load_yo(cpu)){
        printf("[]\n");
        return 1;
    }
    if (argc > 1 && strcmp(argv[1], "--debug") == 0) {
        debug_mode = true;
    }

    if(debug_mode){
        while (cpu->status == STAT_AOK) {
            char input[10];
            printf("Press Enter to step, or type 'q' to quit debug: ");
            if (!fgets(input, sizeof(input), stdin)) break;
            if (input[0] == 'q') break;
            step(cpu);
            print_json(cpu);
        }
    } 
    else {
        uint64_t steps = 0;
        printf("[");
        do{
            step(cpu);
            steps++;
            print_json(cpu);
            if (steps > 10000000ULL) {
                fprintf(stderr, "Too many steps, abort.\n");
                break;
            }
        }while (cpu->status == STAT_AOK);
        printf("\n]\n");
    }
    cpu_free(cpu);
    return 0;
}
