#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 iamcamiel
# X-2 CV Camiel Vanderhoeven 16-Nov-2026: Check the expanded worker limit.
# X-1 CV Camiel Vanderhoeven 15-Nov-2026: native control-flow simulation.
"""Compile native supervisor branch against service stubs, not the VMS ABI."""
import os
from pathlib import Path
import subprocess
from BUILD_HOST import PORT

HEADER = r"""
#ifndef PTS_VMS_STUB_H
#define PTS_VMS_STUB_H
#include <stddef.h>
struct dsc$descriptor_s {
    unsigned short dsc$w_length;
    unsigned char dsc$b_dtype, dsc$b_class;
    char *dsc$a_pointer;
};
struct accdef {
    unsigned short acc$w_msgtyp, size;
    unsigned int acc$l_finalsts, acc$l_pid;
    unsigned char rest[72];
};
#define ACC$K_TERMLEN 84
#define MSG$_DELPROC 42
#define DSC$K_DTYPE_T 14
#define DSC$K_CLASS_S 1
#define DVI$_UNIT 12
#define SS$_NORMAL 1
#define SS$_ENDOFFILE 16
#define SS$_BADPARAM 20
#define SS$_NONEXPR 24
#define SS$_ABORT 28
#define IO$_READVBLK 1
#define IO$M_NOW 128
unsigned int sys$gettim(unsigned long long *);
unsigned int sys$crembx(int, unsigned short *, unsigned int, unsigned int,
                        int, int, int, int);
unsigned int sys$getdviw(int, unsigned short, int, void *, void *, int, int, int);
unsigned int lib$get_ef(unsigned int *);
unsigned int lib$free_ef(unsigned int *);
unsigned int sys$clref(unsigned int);
unsigned int sys$setimr(unsigned int, const long long *, int, unsigned int, int);
unsigned int sys$cantim(unsigned int, int);
unsigned int sys$creprc(unsigned int *, void *, void *, void *, void *,
                       int, int, int, int, int, unsigned short, int);
unsigned int sys$qiow(int, unsigned short, unsigned int, void *, int, int,
                      void *, unsigned int, int, int, int, int);
unsigned int sys$readef(unsigned int, unsigned int *);
unsigned int sys$delprc(unsigned int *, int);
unsigned int sys$schdwk(int, int, const long long *, int);
unsigned int sys$hiber(void);
unsigned int sys$dassgn(unsigned short);
#endif
"""

TEST = r"""
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "VMS_STUB.H"
static char *host_cwd(char *b, size_t n) { return getcwd(b, n); }
#define getcwd(b, n, ...) host_cwd(b, n)
#define main pts_native_runner_main
#include "PTS_RUNNER.C"
#undef main
#undef getcwd
static int mode, killed, assigned, freed, released, cancelled, timer_calls;
unsigned int sys$gettim(unsigned long long *v) { *v=10000000; return 1; }
unsigned int sys$crembx(int a, unsigned short *c, unsigned int b, unsigned int d,
                       int e,int f,int g,int h)
{
    (void)a;(void)b;(void)d;(void)e;(void)f;(void)g;(void)h;
    *c=7; assigned++; return 1;
}
unsigned int sys$getdviw(int a,unsigned short c,int d,void *items,void *sb,
                        int e,int f,int g)
{
    struct item { unsigned short length,code; void *buffer; unsigned short *r; };
    (void)a;(void)c;(void)d;(void)e;(void)f;(void)g;
    *(unsigned int *)((struct item *)items)->buffer=7;
    ((struct io_status *)sb)->status=mode==7?SS$_ABORT:SS$_NORMAL;
    return 1;
}
unsigned int lib$get_ef(unsigned int *e) { *e=35; return 1; }
unsigned int lib$free_ef(unsigned int *e) { assert(*e==35);freed++;return 1; }
unsigned int sys$clref(unsigned int e) { assert(e==35);return 1; }
unsigned int sys$setimr(unsigned int e,const long long *d,int a,
                        unsigned int r,int b)
{
    (void)a;(void)b; assert(e==35 && *d<0 && r==1);timer_calls++;
    return mode==6?SS$_ABORT:SS$_NORMAL;
}
unsigned int sys$cantim(unsigned int r,int a)
{ (void)a;assert(r==1);cancelled++;return 1; }
unsigned int sys$creprc(unsigned int *p,void *i,void *a,void *b,void *c,
                       int d,int e,int f,int g,int h,unsigned short m,int flags)
{
    (void)i;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;
    assert(m==7 && flags==0);
    if(mode==5)return SS$_ABORT;
    *p=777;return 1;
}
unsigned int sys$qiow(int e,unsigned short c,unsigned int fn,void *sb,int a,int b,
                      void *msg,unsigned int size,int d,int f,int g,int h)
{
    struct io_status *s=sb;struct accdef *m=msg;
    (void)e;(void)a;(void)b;(void)d;(void)f;(void)g;(void)h;
    assert(c==7 && fn==(IO$_READVBLK|IO$M_NOW) && size>=ACC$K_TERMLEN);
    if(mode==1 && !killed){s->status=SS$_ENDOFFILE;return 1;}
    if(mode==4){s->status=SS$_ABORT;return 1;}
    memset(msg,0,size);
    s->status=SS$_NORMAL;s->count=ACC$K_TERMLEN;
    m->acc$w_msgtyp=mode==3?99:MSG$_DELPROC;
    m->acc$l_pid=mode==2?888:777;
    m->acc$l_finalsts=killed?SS$_ABORT:SS$_NORMAL;
    return 1;
}
unsigned int sys$readef(unsigned int e,unsigned int *s)
{ assert(e==35);*s=1U<<(35%32);return 1; }
unsigned int sys$delprc(unsigned int *p,int a)
{ (void)a;assert(*p==777);killed++;return 1; }
unsigned int sys$schdwk(int a,int b,const long long *d,int c)
{ (void)a;(void)b;(void)c;assert(*d==-200000);return 1; }
unsigned int sys$hiber(void){return 1;}
unsigned int sys$dassgn(unsigned short c){assert(c==7);released++;return 1;}

static void protocol_checks(void)
{
    const char *records[]={
        "PTS1|CASE|TOKEN|0\n",
        "PTS1|CASE|OTHER|0\n",
        "PTS1|OTHER|TOKEN|0\n",
        "PTS1|CASE|TOKEN|0\nPTS1|CASE|TOKEN|0\n",
        "PTS1|CASE|TOKEN|0",
        "PTS1|CASE|TOKEN|3\n",
        "PTS1|CASE|TOKEN|-1\n"
    };
    int i, code;
    unsigned long number;
    FILE *out;
    char path[128], joined[128], root[128], first[256], second[256];
    struct pts_control c, read;
    snprintf(path,sizeof(path),"/tmp/pts_protocol_%lu.dat",(unsigned long)getpid());
    for(i=0;i<7;i++){
        out=fopen(path,"w");assert(out);
        fputs(records[i],out);assert(!fclose(out));
        assert((pts_read_result(path,"CASE","TOKEN",&code)==0)==(i==0));
    }
    memset(&c,0,sizeof(c));
    strcpy(c.id,"CASE");strcpy(c.token,"TOKEN");
    strcpy(c.result,"RESULT.PTS");strcpy(c.directory,"DISK:[DIR]");
    c.seed=1;c.threads=8;c.iterations=200;
    out=tmpfile();assert(out);
    assert(!pts_write_control(out,&c));rewind(out);
    assert(!pts_read_control(out,&read));
    assert(!strcmp(read.id,c.id) && read.iterations==200);
    fclose(out);
    c.threads=600;
    out=tmpfile();assert(out);
    assert(!pts_write_control(out,&c));rewind(out);
    assert(!pts_read_control(out,&read) && read.threads==600);
    fclose(out);
    c.threads=PTS_MAX_THREADS;
    out=tmpfile();assert(out);
    assert(!pts_write_control(out,&c));rewind(out);
    assert(!pts_read_control(out,&read) && read.threads==PTS_MAX_THREADS);
    fclose(out);
    c.threads=PTS_MAX_THREADS+1;
    out=tmpfile();assert(out);
    assert(!pts_write_control(out,&c));rewind(out);
    assert(pts_read_control(out,&read));
    fclose(out);
    assert(pts_number("-1",1,256,&number));
    assert(pts_number("257",1,256,&number));
    assert(pts_number("1x",1,256,&number));
    assert(!pts_number("256",1,256,&number) && number==256);
    assert(!pts_safe_id("../escape"));
    assert(!pts_subdirectory(joined,sizeof(joined),"DISK:[DIR]","CHILD"));
    assert(!strcmp(joined,"DISK:[DIR.CHILD]"));
    assert(!pts_join(joined,sizeof(joined),"DISK:[DIR]","FILE.DAT"));
    assert(!strcmp(joined,"DISK:[DIR]FILE.DAT"));
    /* Exercise native-format names with repeated identity/time. These are
       literal directory names in the host simulation, not VMS ABI tests. */
    snprintf(root,sizeof(root),"SIM_%lu:[RESULTS]",(unsigned long)getpid());
    assert(!pts_create_run_directory(first,sizeof(first),root,42,1700000000));
    assert(!pts_create_run_directory(second,sizeof(second),root,42,1700000000));
    assert(strcmp(first,second));
    assert(!rmdir(first) && !rmdir(second));
    remove(path);
}
int main(void)
{
    struct outcome r;
    protocol_checks();
    for(mode=0;mode<8;mode++){
        killed=assigned=freed=released=cancelled=timer_calls=0;
        r=execute("IMAGE.EXE","IN.DAT","OUT.LOG","ERR.LOG",5);
        assert(assigned==1 && released==1);
        assert(freed==(mode==7?0:1));
        if(mode==0)assert(r.clean && !r.error && !r.timeout && !killed);
        else if(mode==1)assert(r.timeout && !r.error && killed==1 && timer_calls==2);
        else assert(r.error);
        if(mode==2 || mode==3 || mode==4)assert(killed==1);
        if(mode==5 || mode==6 || mode==7)assert(!killed);
    }
    puts("PASS: native supervisor control-flow simulation (8 service scenarios)");
    puts("PASS: strict result/control protocol and native path checks");
    puts("This is not OpenVMS ABI or target execution validation.");
    return 0;
}
"""


def main():
    build = PORT / "BUILD"
    stubs = build / "VMS_STUBS"
    stubs.mkdir(parents=True, exist_ok=True)
    (stubs / "VMS_STUB.H").write_text(HEADER)
    for name in ["starlet.h", "lib$routines.h", "descrip.h", "ssdef.h",
                 "iodef.h", "dvidef.h", "accdef.h", "msgdef.h"]:
        (stubs / name).write_text('#include "VMS_STUB.H"\n')
    (stubs / "NATIVE_TEST.C").write_text(TEST)
    binary = stubs / "NATIVE_TEST"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c99",
                    "-D_POSIX_C_SOURCE=200809L", "-D__VMS",
                    "-D__INITIAL_POINTER_SIZE=32", "-D__int64=long long",
                    "-Wall", "-Wextra", "-Werror", "-Wno-unknown-pragmas",
                    "-I"+str(stubs), "-I"+str(PORT),
                    "-x", "c", str(stubs/"NATIVE_TEST.C"), str(PORT/"PTS_PORT.C"),
                    "-o", str(binary)], check=True, timeout=60)
    subprocess.run([str(binary)], cwd=stubs, check=True, timeout=10)


if __name__ == "__main__":
    main()
