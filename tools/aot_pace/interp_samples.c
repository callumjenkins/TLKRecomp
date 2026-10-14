/* Sample interp816_runOpcode() for every opcode x M x X on random states with a
 * logging bus; one line per sample (state, cycles, bus accesses). Input for
 * verify.py. Build: see tools/README.md (aot_pace). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interp816.h"
int interp816_opcode_hook(uint32_t a){(void)a;return 0;}
static uint32_t acc[64]; static int nacc; static char rw[64];
static uint32_t seed;
static uint8_t memf(uint32_t a){ uint32_t h=(a^seed)*2654435761u; h^=h>>15; h*=2246822519u; h^=h>>13; return (uint8_t)h; }
static uint32_t pcbase; static uint8_t prog[4];
static uint8_t rd(void*m,uint32_t a){(void)m; if(nacc<64){acc[nacc]=a&0xffffff;rw[nacc++]='r';}
  if(a>=pcbase && a<pcbase+4) return prog[a-pcbase]; return memf(a&0xffffff);}
static void wr(void*m,uint32_t a,uint8_t v){(void)m;(void)v; if(nacc<64){acc[nacc]=a&0xffffff;rw[nacc++]='w';}}
static uint32_t rnd(void){ static uint32_t s=12345; s^=s<<13; s^=s>>17; s^=s<<5; return s; }
int main(int argc,char**argv){
  int N=argc>1?atoi(argv[1]):40;
  Interp816*c=interp816_init(NULL,rd,wr);
  for(int op=0;op<256;op++) for(int m=0;m<2;m++) for(int x=0;x<2;x++) for(int n=0;n<N;n++){
    if(op==0xDB||op==0xCB) continue; /* STP/WAI */
    c->e=false; c->mf=m; c->xf=x; c->stopped=false; c->waiting=false; c->irqWanted=c->nmiWanted=false;
    uint32_t r=rnd();
    c->a=rnd(); c->x=rnd(); c->y=rnd(); if(x){c->x&=0xff;c->y&=0xff;}
    if (n&1) { c->x &= 0x3; c->y &= 0x3; }
    static const uint8_t dbs[]={0x00,0x7e,0x7f,0xc0,0x80,0x40,0xc3,0x01};
    c->db=dbs[r&7]; c->k=(r>>3)&1?0xC1:0x00;
    c->dp = (r>>4)&1 ? (rnd()&0xffff) : ((r>>5)&1 ? 0x0000 : (rnd()&0x1f00));
    if (((r>>6)&3)==0) c->dp = 0x4300; 
    c->sp = (r>>8)&1 ? (0x1000 + (rnd()&0xff)) : (rnd()&0xffff);
    c->pc = 0x8000 + (rnd()&0x7ff0);
    uint8_t fl = rnd(); c->c=fl&1; c->z=fl&2; c->v=fl&4; c->n=fl&8; c->i=fl&16; c->d=0;
    seed=rnd();
    pcbase=((uint32_t)c->k<<16)|c->pc; prog[0]=op; prog[1]=rnd(); prog[2]=rnd(); prog[3]=rnd();
    if (n%5==2) prog[2] = (rnd()&1)? 0x21 : 0x43;  /* hit IO regions sometimes */
    uint16_t A=c->a,X=c->x,Y=c->y,S=c->sp,D=c->dp,PC=c->pc; uint8_t DB=c->db,K=c->k;
    nacc=0;
    int cyc=interp816_runOpcode(c);
    printf("%02X %d %d %04X %04X %04X %04X %04X %02X %02X %04X %02X %02X%02X%02X %08X %d %d",
      op,m,x,A,X,Y,S,D,DB,K,PC,fl,prog[1],prog[2],prog[3],seed,cyc,nacc);
    for(int i=0;i<nacc;i++) printf(" %c%06X",rw[i],acc[i]);
    printf("\n");
  }
  return 0;
}
