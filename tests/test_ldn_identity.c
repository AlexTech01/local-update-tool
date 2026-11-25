#include "ldn_identity.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const u64 own_pid=123, own_instance=456;
static u8 original[16], current[16];
static unsigned creates,destroys,calls,fail_call,next_updater,restore_failures;
static bool mapped, fail_after, unexpected_mapping, bad_readback;
static Result first_create, second_create;
typedef struct { bool active, present, remove, previous; u64 pid; u8 property[16]; } Updater;
static Updater updaters[32];
void labLog(const char *fmt, ...) { (void)fmt; }
void labLogResult(const char *stage,Result rc) { (void)stage;(void)rc; }
Result svcGetProcessId(u64 *pid,unsigned handle) { assert(handle==CUR_PROCESS_HANDLE);*pid=own_pid;return 0; }
Result labGetService(Service *sm,const char *name,Service *out) {
    assert(sm->active); out->active=true;
    if(!strcmp(name,"arp:r"))out->kind=1;
    else {assert(!strcmp(name,"arp:w"));out->kind=2;}
    return 0;
}
void serviceClose(Service *s) {
    if(s->active && s->kind>=10) {
        Updater *u=&updaters[s->kind-10];
        if(u->active) mapped=u->previous;
        u->active=false;
    }
    s->active=false;
}
Result ldnCreateNetwork(const LdnSecurityConfig *s,const LdnUserConfig *u,const LdnNetworkConfig *c) {
    assert(s && u && c); creates++;
    if(creates==1) return first_create;
    assert(!mapped); return second_create;
}
Result ldnDestroyNetwork(void) {destroys++;return 0;}
static Result dispatch(Service *s,unsigned cmd,const void *in,size_t in_size,
                       void *out,size_t out_size,SfDispatchParams params) {
    assert(s->active);
    if(s->kind==1) {
        assert(in_size==8);
        if(cmd==3) {
            assert(*(const u64 *)in==own_pid && out_size==8);
            if(!mapped && !unexpected_mapping)return 0xcc9d;
            *(u64 *)out=bad_readback && creates==2 ? own_instance+1:own_instance;return 0;
        }
        assert(cmd==2 && *(const u64 *)in==own_instance && out_size==16);
        assert(mapped);memcpy(out,current,16);return 0;
    }
    if(s->kind==2) {
        assert(cmd==2 && in_size==8 && *(const u64 *)in==own_instance);
        assert(params.out_num_objects==1 && params.out_objects);
        if(!mapped && restore_failures) {restore_failures--;return 0xaaaa;}
        assert(next_updater<32); unsigned index=next_updater++;
        Updater *u=&updaters[index]; *u=(Updater){.active=true,.present=mapped,.previous=mapped,.pid=own_pid};
        memcpy(u->property,current,16);
        *params.out_objects=(Service){.active=true,.kind=index+10};return 0;
    }
    assert(s->kind>=10); Updater *u=&updaters[s->kind-10]; assert(u->active);
    if(cmd==1) {
        assert(in_size==24 && *(const u64 *)in==own_pid);
        u->present=true;u->pid=*(const u64 *)in;memcpy(u->property,(const u8 *)in+8,16);
        assert(!memcmp(u->property,original,16));return 0;
    }
    assert(!in_size);
    if(cmd==2) {u->remove=true;return 0;}
    assert(cmd==0 && u->present);mapped=!u->remove;
    if(mapped)memcpy(current,u->property,16);
    u->active=false;return 0;
}
Result serviceDispatchImpl(Service *s,unsigned cmd,const void *in,size_t in_size,
                           void *out,size_t out_size,SfDispatchParams params) {
    calls++; bool fault=calls==fail_call;
    if(fault && !fail_after)return 0xbeef;
    Result rc=dispatch(s,cmd,in,in_size,out,out_size,params);
    return fault ? 0xbeef:rc;
}
static Result run(void) {
    Service sm={.active=true};LdnSecurityConfig s={0};LdnUserConfig u={0};LdnNetworkConfig c={0};
    return labCreateGameNetwork(&sm,&s,&u,&c);
}
static void reset(void) {
    creates=destroys=calls=fail_call=next_updater=restore_failures=0;
    mapped=true;fail_after=unexpected_mapping=bad_readback=false;
    first_create=0xc2cb;second_create=0;
    memset(updaters,0,sizeof(updaters));
    for(unsigned i=0;i<16;i++)original[i]=(u8)(i+1);
    memcpy(current,original,16);
}
int main(void) {
    reset();first_create=0;assert(!run() && creates==1 && !calls && mapped);
    reset();first_create=0x1234;assert(run()==0x1234 && creates==1 && !calls && mapped);
    reset();assert(!run() && creates==2 && calls==12 && mapped && !destroys);
    assert(!memcmp(current,original,16));
    for(unsigned step=1;step<=12;step++)for(unsigned after=0;after<2;after++) {
        reset();fail_call=step;fail_after=after;Result rc=run();
        (void)rc; assert(mapped && !memcmp(current,original,16));
        for(unsigned i=0;i<next_updater;i++)assert(!updaters[i].active);
    }
    reset();second_create=0x2345;assert(run()==second_create && mapped && !destroys);
    reset();unexpected_mapping=true;assert(run() && creates==1 && mapped);
    reset();bad_readback=true;assert(run() && destroys==1 && mapped);
    reset();restore_failures=2;assert(!run() && mapped && !destroys);
    reset();restore_failures=3;assert(run() && !mapped && destroys==1);
    puts("PASS: scoped own-process detach, restoration, readback and IPC failure paths");
}
