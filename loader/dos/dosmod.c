/*
 * DOS Virtual Machine
 *
 * Copyright 1998 Ove K�ven
 */

#if defined(linux) && defined(__i386__)

/* apparently ELF images are usually loaded high anyway */
#ifndef __ELF__
/* if not, force dosmod at high addresses */
asm(".org 0x110000");
#endif __ELF__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/vm86.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include "dosmod.h"

 /* FIXME: hack because libc vm86 may be the old syscall version */

#define SYS_vm86   166

static __inline__ int vm86plus( int func, struct vm86plus_struct *ptr )
{
    int res;
#ifdef __PIC__
    __asm__ __volatile__( "pushl %%ebx\n\t"
                          "movl %2,%%ebx\n\t"
                          "int $0x80\n\t"
                          "popl %%ebx"
                          : "=a" (res)
                          : "0" (SYS_vm86),
                            "g" (func),
                            "c" (ptr) );
#else
    __asm__ __volatile__("int $0x80"
                         : "=a" (res)
                         : "0" (SYS_vm86),
                           "b" (func),
                           "c" (ptr) );
#endif  /* __PIC__ */
    if (res >= 0) return res;
    errno = -res;
    return -1;
}

void set_timer(struct timeval*tim)
{
 struct itimerval cur;

 cur.it_interval=*tim;
 cur.it_value=*tim;
 setitimer(ITIMER_REAL,&cur,NULL);
}

volatile int sig_pend,sig_fatal=0;
void*img;
struct vm86plus_struct VM86;

void sig_handler(int sig)
{
 if (sig_pend) fprintf(stderr,"DOSMOD previous signal %d lost\n",sig_pend);
 sig_pend=sig;
 signal(sig,sig_handler);
}

void bad_handler(int sig)
{
 fprintf(stderr,"DOSMOD caught fatal signal %d\n",sig);
 fprintf(stderr,"(Last known VM86 CS:IP was %04x:%04lx)\n",VM86.regs.cs,VM86.regs.eip);
 sig_pend=sig; sig_fatal++;
 signal(sig,bad_handler);
}

int main(int argc,char**argv)
{
 int mfd=open(argv[0],O_RDWR);
 struct timeval tim;
 int func,ret;
 off_t fofs=0;
 pid_t ppid=getppid();
 
/* fprintf(stderr,"main is at %08lx, file is %s, fd=%d\n",(unsigned long)&main,argv[0],mfd); */
 if (mfd<0) return 1;
/* Map in our DOS image at the start of the process address space */
 if (argv[1]) {
  /* Ulrich Weigand suggested mapping in the DOS image directly from the Wine
     address space */
  fofs=atol(argv[1]);
  /* linux currently only allows mapping a process memory if it's being ptraced */
  /* Linus doesn't like it, so this probably won't work in the future */
  /* it doesn't even work for me right now */
  kill(ppid,SIGSTOP);
  ptrace(PTRACE_ATTACH,ppid,0,0);
  waitpid(ppid,NULL,0);
 }
 img=mmap(NULL,0x110000,PROT_EXEC|PROT_READ|PROT_WRITE,MAP_FIXED|MAP_SHARED,mfd,fofs);
 if (argv[1]) {
  ptrace(PTRACE_DETACH,ppid,0,0);
  kill(ppid,SIGCONT);
 }
 if (img==(void*)-1) {
  fprintf(stderr,"DOS memory map failed, error=%s\n",strerror(errno));
  fprintf(stderr,"in attempt to map %s, offset %08lX, length 110000, to offset 0\n",argv[0],fofs);
  return 1;
 }
/* initialize signals and system timer */
 signal(SIGHUP,sig_handler);
 signal(SIGINT,sig_handler);
 signal(SIGUSR1,sig_handler);
 signal(SIGUSR2,sig_handler);
 signal(SIGALRM,sig_handler);

 signal(SIGQUIT,bad_handler);
 signal(SIGILL,bad_handler);
 signal(SIGBUS,bad_handler);
 signal(SIGFPE,bad_handler);
 signal(SIGSEGV,bad_handler);
 signal(SIGTERM,bad_handler);
#if 0
 tim.tv_sec=0; tim.tv_usec=54925;
 set_timer(&tim);
#endif
/* report back to the main program that we're ready */
 ret=1; /* dosmod protocol revision 1 */
 write(1,&ret,sizeof(ret));
/* context exchange loop */
 do {
  if (read(0,&func,sizeof(func))!=sizeof(func)) return 1;
  if (func<0) break;
  switch (func) {
   case DOSMOD_SET_TIMER:
    if (read(0,&tim,sizeof(tim))!=sizeof(tim)) return 1;
    set_timer(&tim);
    /* no response */
    break;
   case DOSMOD_ENTER:
   default:
    if (read(0,&VM86,sizeof(VM86))!=sizeof(VM86)) return 1;
    if (sig_pend) ret=DOSMOD_SIGNAL; else
     ret=vm86plus(func,&VM86);
    if (write(1,&ret,sizeof(ret))!=sizeof(ret)) return 1;
    if (write(1,&VM86,sizeof(VM86))!=sizeof(VM86)) return 1;
    switch (ret&0xff) {
     case DOSMOD_SIGNAL:
      ret=sig_pend; sig_pend=0;
      if (write(1,&ret,sizeof(ret))!=sizeof(ret)) return 1;
      if (sig_fatal) return 1;
      break;
    }
  }
 } while (1);
 return 0;
}

#else /* !linux-i386 */
int main(void) {return 1;}
#endif
