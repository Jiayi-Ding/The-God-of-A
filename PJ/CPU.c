#include <stdlib.h>
#include <stdio.h>
#include <string.h>


int BUFF_SIZE = 1000000; //this is 1 MB 
char buff[1000000];
int size = 524288; //2^19
char mem[524288];
int status; 

//寄存器
typedef struct {
	int rax; int eax;
	int rbx; int ebx;
    int rcx; int ecx;
	int rdx; int edx; 

	int rsi; int esi;
	int rdi; int edi;
	int rsp; int esp; 
	int rbp; int ebp;

    int r8; int r8d;
    int r9; int r9d;
    int r10; int r10d;
    int r11; int r11d;
    int r12; int r12d;
    int r13; int r13d;
    int r14; int r14d;
    int r15; int r15d;
} registers;

typedef struct {
	int SF; //negative < 0, signed 
	int ZF; //zero == 0
	int OF; //overflow > 0, signed
} flags;


void set_reg2(registers reg, int pc, int value){

	char rb = mem[pc];
	pc++; 

	switch (rb) {
		
			case '0':
			reg.eax = value; 
			break;

			case '1':
			reg.ecx = value; 
			break;

			case '2':
			reg.edx = value; 
			break;

			case '3':
			reg.ebx = value; 
			break;

			case '4':
			reg.esp = value; 
			break;

			case '5':
			reg.ebp = value; 
			break;

			case '6':
			reg.esi = value; 
			break;

			case '7':
			reg.edi = value; 
			break;

			default: 
			status = 3; 
			break;
	}	

}


void get_value(char*value,int pc){

  int i;
  pc+=6;
  for(i=0;i<8;i++){
    
    value[i]=mem[pc];
    i++;
    value[i] = mem[pc+1];
    pc-=2;
  }
}

void get_mem_value(char*value, int addr, int pc) {
	int i; 
	pc+=6;
	for(i = 0; i < 8; i++) {
		value[i] = mem[pc];
		i++;
		value[i] = mem[pc + 1];
		pc-=2; 
	}
}

int get_reg_value(int pc, registers reg) {

	int tmp; 

	switch(mem[pc]) {
		case '0':
		tmp = reg.eax;
		break;

		case '1':
		tmp = reg.ecx;
		break;

		case '2':
		tmp = reg.edx;
		break;

		case '3':
		tmp = reg.ebx;
		break;

		case '4':
		tmp = reg.esp;
		break;

		case '5':
		tmp = reg.ebp;
		break;

		case '6':
		tmp = reg.esi;
		break;

		case '7':
		tmp = reg.edi;
		break;

		default:
		status = 3;
		break;
	}

	return tmp; 
}

int alo(int pc, registers reg, char ifun, flags flags){
	int reg1Val, reg2Val; 
	reg1Val=get_reg_value(pc,reg);
	pc++;
	reg2Val=get_reg_value(pc, reg);
	pc++; 
	switch(ifun){
		case 0:
		reg2Val+=reg1Val;
		break;

		case 1:
		reg2Val-=reg1Val;
		break;
		
		case 2:
		reg2Val=reg1Val & reg2Val;
		break;
		
		case 3:
		reg2Val=reg1Val ^ reg2Val;
		break;

		default:
		status=3;
		break;		
	}
	return pc; 
}

//move operations 

int rrmovl(registers reg, int pc) {

	int r1val = get_reg_value(pc, reg);
	pc++;
	
	set_reg2(reg, pc, r1val);

	return pc; 

}

int cmov(registers reg, int pc, flags flags){
		char r2 = mem[pc];

		switch(r2){

			case '0':
			rrmovl(reg, pc);
			break;

			case '1':
			if(flags.ZF || flags.SF){
				rrmovl(reg, pc);
			}
			break;

			case '2':
			if(flags.SF){
				rrmovl(reg, pc);
			}
			break;

			case '3':
			if(flags.ZF) {
				rrmovl(reg, pc);
			}
			break;

			case '4':
			if(!flags.ZF){
				rrmovl(reg, pc);
			}
			break;

			case'5':
			if(!flags.ZF && !flags.SF){
				rrmovl(reg, pc);
			}
			break;	

			default:
			status = 3; 
			break;

		}
		pc++; 
		return pc; 
	}



int irmovl(registers reg, int pc) {

  pc++;
  
  char value[8]; //its 8 bits

  get_value(value,pc);
  set_reg2(reg, pc, (int)strtol(value,NULL,16));
  pc++;

  return pc+8;

}

int rmmovl(registers reg, int pc){
	
	int val1, val2; 

	val1 = get_reg_value(pc, reg);
	pc++;
	val2 = get_reg_value(pc, reg);
	pc++; 

	char value[8];
	get_value(value, pc);

	int d = (int)strtol(value,NULL,16);
	int addr; 

	addr = val2 + d;

	mem[addr] = val1;  

	return pc; 

}

int mrmovl(registers reg, int pc){
	//memory to registers

	char value[8];
	get_value(value, pc);

	int val1, val2; 

	val1 = get_reg_value(pc, reg);  //ra 
	pc++;
	val2 = get_reg_value(pc, reg); //rb 


	int addr;

	int d = (int)strtol(value,NULL,16); 

	addr = val1 + d; 
	int a = mem[addr];
	set_reg2(reg, pc, a);

	return pc; 

}

int push(registers reg, int pc){
	//decrease esp by 4
	//store val1 to memory at esp

	reg.esp-=4;
	int val = get_reg_value(pc, reg);
	mem[reg.esp] = val; 
	pc+=4; 
	return pc; 

}


int pop(registers reg, int pc) {
	//read word from memory at esp
	//save in val 1
	//increase esp by 4
	
	int val1; 
	val1 = mem[reg.esp];
	set_reg2(reg, pc, val1);
	reg.esp += 4; 
	pc+=4; 
	return pc; 

}


//jump instructions 

int jmp(registers reg, int pc, flags flags) {

	int temp;
  char r2 = mem[pc];
  pc++;
  char value[8]; 

  switch (r2) {

      case '0': //jmp
        get_value(value,pc);
        temp = (int)strtol(value,NULL,16); 
        pc = temp;
      break;

      case '1': //jle
        get_value(value,pc);
        temp = (int)strtol(value,NULL,16); 
        if ((flags.SF ^ flags.OF) || flags.ZF) {
          pc = temp;
        } else {
          pc += 5;
        }
      break;

      case '2': //jl
        get_value(value,pc);
        temp = (int)strtol(value,NULL,16); 
        if (flags.SF ^ flags.OF) {
          pc = temp;
        } else {
          pc += 5;
        }
      break;

      case '3': //je
        get_value(value,pc);
        temp = (int)strtol(value,NULL,16); 
        if (flags.ZF) {
          pc += 5;
        } else {
          pc += 5;
        }
      break;

      case '4': //jne
        get_value(value,pc);
        temp = (int)strtol(value,NULL,16); 
        if (!flags.ZF) {
          pc = temp;
        } else {
          pc += 5;
        }
      break;

      case '5': //jge
       	get_value(value,pc);
       	temp = (int)strtol(value,NULL,16); 
        if (!(flags.SF ^ flags.OF)) {
          pc = temp;
        } else {
          pc += 5;
        }
      break;

      case '6': //jg
        get_value(value,pc);
        temp = (int)strtol(value,NULL,16); 
        if (!(flags.SF ^ flags.OF) & !flags.ZF) {
          pc = temp;
        } else {
          pc += 5;
        }
      break;

      default:
      status = 3;
      break;

    }

    return pc;

}

//subroutine call and return 

int ret (registers reg, int pc) {

	pc = mem[reg.esp];
	pc += 4; 
	return pc; 
}

int call (registers reg, int pc) {

	char value[8];
	get_value(value, pc);
	int addr = (int)strtol(value,NULL,16); 
	reg.esp -= 4; 
	mem[reg.esp] = pc;
	pc = addr;  
	return pc; 

}

int nop(int pc) {
	//pc+=2; 
	return pc; 
}

//miscellaneous instructions

int main (int argc, char *argv[]) {

	int i, j, k; 
	registers reg; 
	flags flags;
	char icode, ifun;
	int val1, val2; 
	int steps, pc;

	//read in the file 
	FILE *file;
    file = fopen(argv[1],"r");
    fgets(buff,BUFF_SIZE,file); 

    i = 0; 
    while(buff[i] != '\0'){
    	mem[i] = buff[i];
    	i++;
    }

	i = 0; 
	pc = 0; 
	reg.eax = reg.ecx = reg.edx = reg.ebx = reg.esi = reg.edi = 0; 
	flags.SF = flags.ZF = flags.OF = 0; 
	steps = 0; 
	status = 1; 

	reg.esp = reg.ebp = size - 4; //these are addresses

	while((status == 1) && (buff[i] != '\0')) {

		steps++;
		icode = buff[pc];
		pc++; 
		ifun = buff[pc];
		pc++; 
			 
		switch (icode) {
	
			case '0':
			//halt
			status = 2; 
			break;

			case '1':
			//no op- does nothing 
 			pc = nop(pc);
			break;

			case '2':
			//rrmove operation
			pc = cmov(reg, pc, flags);
			break;

			case '3':
			//irmove operation
			pc = irmovl(reg, pc);
			break; 

			case '4':
			//rmmove operation
			pc = rmmovl(reg, pc);
			break;

			case '5':
			//mrmove operation
			pc = mrmovl(reg, pc);
			break;

			case '6':
			//arithmetic or logic
			pc = alo(pc, reg, ifun, flags);
			break;

			case '7':
			//jump operations 
			pc = jmp(reg, pc, flags);
			break;

			case '8':
			//call
			pc = call(reg, pc);
			break;

			case '9':
			//ret 
			pc = ret(reg, pc); 
			break;

			case 'a':
			//push
			pc = push(reg, pc);
			break;

			case 'b':
			//pop
			pc = pop(reg, pc);
			break;

			default: 
			status = 4; 
			break;
		}

	}

	//now start getting ready to print things 
	char stat[5]; 

	switch(status) {

		case 1:
		strcpy(stat, "AOK");
		break;

		case 2:
		strcpy(stat, "HLT");
		break;

		case 3:
		strcpy(stat, "ADR");
		break;

		case 4:
		strcpy(stat, "INS");
		break;

		default:
		break;

	}

	printf("Steps: %d\n", steps);
	printf("PC: %d\n", pc);
	printf("Status: %s\n", stat);
	printf("CZ: %d\n", flags.ZF);
	printf("CS: %d\n", flags.SF);
	printf("CO: %d\n", flags.OF);
	printf("%ceax: %d\n", 37, reg.eax);
	printf("%cecx: %d\n", 37, reg.ecx);
	printf("%cedx: %d\n", 37, reg.edx);
	printf("%cebx: %d\n", 37, reg.ebx);
	printf("%cesp: %d\n", 37, reg.esp);
	printf("%cebp: %d\n", 37, reg.ebp);
	printf("%cesi: %d\n", 37, reg.esi);
	printf("%cedi: %d\n", 37, reg.edi);

}