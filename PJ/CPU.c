#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <errno.h>

#define MEM_SIZE (1<<20)   /* 1MB memory */
typedef enum { STAT_AOK=1, STAT_HLT=2, STAT_ADR=3, STAT_INS=4 } stat_t;

enum { RAX=0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
       R8, R9, R10, R11, R12, R13, R14, RNONE=15 };

enum {
    I_HALT=0, I_NOP=1, I_RRMOVQ=2, I_IRMOVQ=3, I_RMMOVQ=4,
    I_MRMOVQ=5, I_OPQ=6, I_JXX=7, I_CALL=8,
    I_RET=9, I_PUSHQ=0xA, I_POPQ=0xB
};

enum { A_ADD=0, A_SUB=1, A_AND=2, A_XOR=3 };

typedef struct {
    uint8_t *mem;
    uint64_t pc;
    uint64_t reg[15];
    bool ZF, SF, OF;
    stat_t status;
    bool fatal;
} cpu_t;

static const char *stat_name(stat_t s) {
    switch (s) {
        case STAT_AOK: return "AOK";
        case STAT_HLT: return "HLT";
        case STAT_ADR: return "ADR";
        case STAT_INS: return "INS";
        default:        return "UNK";
    }
}

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

static bool cond(cpu_t *c, uint8_t f) {
    switch(f){
        case 0: return true;
        case 1: return (c->SF ^ c->OF) || c->ZF;
        case 2: return (c->SF ^ c->OF);
        case 3: return c->ZF;
        case 4: return !c->ZF;
        case 5: return !(c->SF ^ c->OF);
        case 6: return !(c->SF ^ c->OF) && !c->ZF;
    }
    return false;
}

static uint64_t alu(cpu_t *c, uint8_t f, uint64_t a, uint64_t b) {
    long long A=a, B=b, R=0;
    bool of=false;
    switch(f){
        case A_ADD: R = B + A;
            of = ((A<0&&B<0&&R>=0)||(A>=0&&B>=0&&R<0)); break;
        case A_SUB: R = B - A;
            of = ((B<0&&A>=0&&R>=0)||(B>=0&&A<0&&R<0)); break;
        case A_AND: R = B & A; break;
        case A_XOR: R = B ^ A; break;
    }
    uint64_t res = (uint64_t)R;
    c->ZF = (res == 0);
    c->SF = ((long long)res < 0);
    c->OF = of;
    return res;
}

/*
 * ========= instruction fetch helpers =========
 */

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

/*
 * ========= EXECUTE ONE INSTRUCTION =========
 */

static void step(cpu_t *c){
    if (c->status != STAT_AOK) return;
    uint64_t current_pc = c->pc;
    bool ok;
    uint8_t b0 = get_byte(c, c->pc, &ok);
    if(!ok){ c->status = STAT_ADR; return; }

    uint8_t icode = (b0 >> 4) & 0xF;
    uint8_t ifun  = (b0 & 0xF);
    if (icode == I_HALT) {
    c->status = STAT_HLT;
    return;
    }
    if (c->status != STAT_AOK) return;
    c->pc += 1;

    switch(icode){
        case I_NOP:
            return;

        case I_RRMOVQ: {
            uint8_t rA,rB;
            if(!fetch_reg(c,&rA,&rB)) return;
            if(rA<15 && rB<15 && cond(c,ifun)) c->reg[rB] = c->reg[rA];
            return;
        }

        case I_IRMOVQ: {
            uint8_t rA,rB; uint64_t V;
            if(!fetch_reg(c,&rA,&rB)) return;
            if(!fetch_long(c,&V)) return;
            if(rB<15) c->reg[rB] = V;
            return;
        }

        case I_RMMOVQ: {
            uint8_t rA,rB; uint64_t D;
            if(!fetch_reg(c,&rA,&rB)) return;
            if(!fetch_long(c,&D)) return;
            uint64_t addr = c->reg[rB] + D;
            if(!set_long(c,addr,c->reg[rA])) { c->status = STAT_ADR; return; }
            return;
        }

        case I_MRMOVQ: {
            uint8_t rA,rB; uint64_t D;
            if(!fetch_reg(c,&rA,&rB)) return;
            if(!fetch_long(c,&D)) return;
            uint64_t addr = c->reg[rB] + D;
            uint64_t val = get_long(c,addr,&ok);
            if(!ok){ c->status = STAT_ADR; return; }
            if(rA<15) c->reg[rA] = val;
            return;
        }

        case I_OPQ: {
            uint8_t rA,rB;
            if(!fetch_reg(c,&rA,&rB)) return;
            if(rA<15 && rB<15){
                uint64_t res = alu(c, ifun, c->reg[rA], c->reg[rB]);
                c->reg[rB] = res;
            }
            return;
        }

        case I_JXX: {
            uint64_t dest;
            if(!fetch_long(c,&dest)) return;
            if(cond(c, ifun)) c->pc = dest;
            return;
        }

        case I_CALL: {
            uint64_t dest;
            if(!fetch_long(c,&dest)) return;
            c->reg[RSP] -= 8;
            if(!set_long(c, c->reg[RSP], c->pc)){ c->status = STAT_ADR; return; }
            c->pc = dest;
            return;
        }

        case I_RET: {
            bool ok2;
            uint64_t addr = get_long(c, c->reg[RSP], &ok2);
            if(!ok2){ c->status = STAT_ADR; return; }
            c->reg[RSP] += 8;
            c->pc = addr;
            return;
        }

        case I_PUSHQ: {
            uint8_t rA,rB;
            if(!fetch_reg(c,&rA,&rB)) return;
            uint64_t current_sp = c->reg[RSP];
            uint64_t new_sp = current_sp - 8;
            uint64_t value_to_push;
            if (rA == 4) {
                value_to_push = current_sp; 
            } else {
                value_to_push = c->reg[rA];
            }
            c->reg[RSP] = new_sp;
            if(!set_long(c, new_sp, value_to_push)) { c->status = STAT_ADR; c->pc=current_pc; return;}
            return;
        }

        case I_POPQ: {
            uint8_t rA,rB; bool ok2;
            if(!fetch_reg(c,&rA,&rB)) return;
            uint64_t v = get_long(c, c->reg[RSP], &ok2);
            if(!ok2){ c->status = STAT_ADR; return; }
            if(rA<15) c->reg[rA] = v;
            if(rA != 4) c->reg[RSP] += 8;
            return;
        }

        default:
            c->status = STAT_INS;
            return;
    }
}

/*
 * ========= JSON Output =========
 */

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

/*
 * ========= Load .yo File =========
 */

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
        if (!*s) return -1; /* odd nibble */
        int lo = hexval(*s++);
        if (lo < 0) break;
        if (len >= maxlen) return -1; /* overflow */
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
        /* read address (hex) */
        char *colon = strchr(s, ':');
        if (!colon) continue;
        /* parse address */
        uint64_t addr = 0;
        /* parse hex after 0x up to ':' */
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
        /* the bytes area may contain spaces or other separators; parse all hex nibbles after ':' */
        const char *bytes_area = colon + 1;
        uint8_t tmp[1024];
        int nb = parse_hex_bytes(bytes_area, tmp, sizeof(tmp));
        if (nb < 0) {
            /* odd nibble or overflow; skip this line */
            continue;
        }
        /* write bytes into memory at addr */
        for (int i = 0; i < nb; ++i) {
            if (!set_byte(c, addr + (uint64_t)i, tmp[i])) {
                /* address out of range -> fatal */
                c->fatal = true;
                c->status = STAT_ADR;
                return false;
            }
            any = true;
        }
    }
    return any;
}

/*
 * ========= CPU Setup =========
 */

static cpu_t *cpu_new() {
    cpu_t *c = calloc(1, sizeof(cpu_t));
    if (!c) return NULL;
    c->mem = calloc(1, MEM_SIZE);
    if (!c->mem) { free(c); return NULL; }
    c->pc = 0;
    c->status = STAT_AOK;
    c->SF = c->OF = 0;
    c->ZF = 1;
    for(int i=0;i<15;i++) {
        c->reg[i] = 0;
    }
    c->reg[RSP] = 0; 
    return c;
}
/*
static void cpu_free(cpu_t *c){
    if (!c) return;
    if (c->mem) free(c->mem);
    free(c);
}

/*
 * ========= main =========
 */

int main() {
    cpu_t *cpu = cpu_new();
    if(!load_yo(cpu)){
        printf("[]\n");
        return 1;
    }

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
    //cpu_free(cpu);
    return 0;
}
