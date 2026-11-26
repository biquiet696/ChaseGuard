#!/usr/bin/env python3
# Runs the call-on-destroyed-interface stubs of src/chaseguard.c natively as a 32-bit Linux
# program (no libc, raw syscalls): return-0 path, float path and the MWCrashFix forward path,
# each checked for an exactly balanced stack. Needs clang with the i686 target.
# Usage: python3 tests/test_stubs.py
import os, re, subprocess, sys, tempfile
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(root, 'src', 'chaseguard.c')).read()
i = src.index('__asm__ (\n    ".text\\n"')
j = src.index('".endr\\n");', i) + len('".endr\\n");')
HARNESS = r'''
typedef unsigned int DWORD;
static long sys(long n,long a,long b,long c,long d,long e,long f){long r;
 __asm__ volatile("push %%ebp; mov %7,%%ebp; int $0x80; pop %%ebp":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c),"S"(d),"D"(e),"g"(f):"memory");return r;}
static void out(const char*s){int n=0;while(s[n])n++;sys(4,1,(long)s,n,0,0,0);}
static void hex(DWORD v){char b[11];int i;b[0]=' ';for(i=0;i<8;i++){int d=(v>>(28-4*i))&15;b[1+i]=d<10?'0'+d:'a'+d-10;}b[9]='\n';b[10]=0;out(b);}
volatile int g_ret; volatile DWORD g_slot, g_this, g_retaddr;
int on_dead_call(const DWORD *frame, DWORD this_ptr) __asm__("_on_dead_call");
int on_dead_call(const DWORD *frame, DWORD this_ptr){ g_slot=frame[0]; g_retaddr=frame[1]; g_this=this_ptr; return g_ret; }
extern const unsigned char dead_stubs[] __asm__("_dead_stubs");
volatile DWORD esp_before, esp_after, eax_after, ecx_seen; volatile float fres;
DWORD vt[128];
/* caller: push 3 args, call [eax+0x24] (slot 9), record esp/eax */
__attribute__((noinline)) void caller3(DWORD tbl, DWORD thisp){
 __asm__ volatile(
  "mov %%esp, %0\n\t"
  "push $1\n\tpush $2\n\tpush $3\n\t"
  "mov %3, %%eax\n\tmov %4, %%ecx\n\t"
  "call *0x24(%%eax)\n\t"
  "mov %%esp, %1\n\tmov %%eax, %2\n\t"
  "mov %0, %%esp\n\t"
  :"=m"(esp_before),"=m"(esp_after),"=m"(eax_after):"r"(tbl),"r"(thisp):"eax","ecx","edx","memory");
}
__attribute__((noinline)) void callerf(DWORD tbl, DWORD thisp){
 __asm__ volatile(
  "fld1\n\t"
  "mov %%esp, %0\n\t"
  "mov %3, %%eax\n\tmov %4, %%ecx\n\t"
  "call *0x2c(%%eax)\n\t"
  "mov %%esp, %1\n\t"
  "fstps %2\n\t"           /* pops the result: must be 0.0, leaving 1.0 below */
  "fstp %%st(0)\n\t"
  :"=m"(esp_before),"=m"(esp_after),"=m"(fres):"r"(tbl),"r"(thisp):"eax","ecx","edx","memory");
}
/* forward target in the fake MWCrashFix table: thiscall, 3 args, ret 12, returns ecx */
__attribute__((naked)) void fwd_target(void){ __asm__ volatile("mov %ecx,%eax\n\tret $12\n\t"); }
int fails;
static void check(int c,const char*n){out(c?"ok   ":"FAIL ");out(n);out("\n");fails+=!c;}
void _start(void){
 int k;
 for(k=0;k<128;k++) vt[k]=(DWORD)dead_stubs+10*k;
 /* return 0, 12 arg bytes */
 g_ret=12; caller3((DWORD)vt,0x1234);
 check(esp_after==esp_before,"return path: stack balanced (12 arg bytes)");
 check(eax_after==0,"return path: EAX=0");
 check(g_slot==9 && g_this==0x1234,"slot 9 and this passed to the handler");
 /* float */
 g_ret=0x10000; callerf((DWORD)vt,0x99);
 check(esp_after==esp_before,"float path: stack balanced");
 check(fres==0.0f && g_slot==11,"float path: 0.0 returned on ST(0), slot 11");
 /* forward: map the MWCrashFix table address */
 { long m=sys(192,0x8aa000,0x1000,3,0x32,-1,0); /* mmap2 FIXED|ANON|PRIVATE */
   check(m==0x8aa000,"mapped fake 0x8AA828 table");
   ((DWORD*)0x8aa828)[9]=(DWORD)fwd_target;
   g_ret=-1; caller3((DWORD)vt,0x5678);
   check(esp_after==esp_before,"forward path: stack balanced by the forwarded method");
   check(eax_after==0x5678,"forward path: method ran with the original ECX");
 }
 out(fails?"STUB TESTS FAILED\n":"all stub tests passed\n");
 sys(1,fails,0,0,0,0,0);
}
'''
d = tempfile.mkdtemp()
c, exe = os.path.join(d, 't.c'), os.path.join(d, 't')
open(c, 'w').write(HARNESS + '\n' + src[i:j] + '\n')
subprocess.check_call(['clang', '--target=i686-linux-gnu', '-O1', '-ffreestanding', '-fno-pic', '-nostdlib',
                       '-static', '-fno-stack-protector', '-mno-sse', '-o', exe, c, '-Wl,-e,_start'])
sys.exit(subprocess.call([exe]))
