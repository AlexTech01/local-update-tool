#include <switch.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

Result __wrap_nsRequestReceiveApplication(AsyncResult *,u32,u16,u64,const NcmContentMetaKey *,s32,NcmStorageId);
static const u64 app=UINT64_C(0x01003a400c3da000);
static NcmContentMetaKey keys[18];
static u8 native_input[16];
static unsigned dispatches,closes,events;
static s32 expected_count=1;
static Result open_result,request_result;
void labLog(const char *format,...) {(void)format;}
void labLogBytes(const char *name,const void *data,size_t size) {(void)name;(void)data;(void)size;}
Result nsGetApplicationManagerInterface(Service *out) {
    if(!open_result)*out=(Service){.active=true,.kind=2010};
    return open_result;
}
void serviceClose(Service *s) {if(s->active)closes++;s->active=false;}
void eventLoadRemote(Event *out,Handle handle,bool autoclear) {
    assert(handle==42 && !autoclear);*out=(Event){handle,autoclear};events++;
}
Result serviceDispatchImpl(Service *s,unsigned command,const void *in,size_t size,
    void *out,size_t out_size,SfDispatchParams params) {
    assert(s->active && s->kind==2010 && command==2010 && !out && !out_size);
    assert(size==sizeof(native_input) && !memcmp(in,native_input,sizeof(native_input)));
    assert(params.buffer_attrs[0]==(SfBufferAttr_HipcMapAlias|SfBufferAttr_In));
    assert(params.buffers[0].ptr==keys && params.buffers[0].size==(size_t)expected_count*sizeof(*keys));
    assert(params.out_num_objects==1 && params.out_objects);
    assert(params.out_handle_attrs[0]==SfOutHandleAttr_HipcCopy && params.out_handles);
    dispatches++;
    if(!request_result) {
        *params.out_objects=(Service){.active=true,.kind=99};*params.out_handles=42;
    }
    return request_result;
}
static Result request(AsyncResult *a,s32 count) {
    return __wrap_nsRequestReceiveApplication(a,0xa9fe3b02,55556,app,keys,count,6);
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *fixture=fopen(argv[1],"r");assert(fixture);
    for(unsigned i=0;i<sizeof(native_input);i++) {
        unsigned byte;assert(fscanf(fixture,"%2x",&byte)==1);native_input[i]=(u8)byte;
    }
    assert(fgetc(fixture)=='\n' && fgetc(fixture)==EOF);fclose(fixture);
    const u8 expected[16]={6,0,4,0xd9,2,0x3b,0xfe,0xa9,0,0xa0,0x3d,0x0c,0x40,0x3a,0,1};
    assert(!memcmp(native_input,expected,sizeof(expected)));
    for(unsigned i=0;i<18;i++)keys[i]=(NcmContentMetaKey){.id=app|0x800,.version=0x40000+i,.type=0x81};
    AsyncResult a={0};assert(!request(&a,1));
    assert(dispatches==1 && closes==1 && events==1 && a.s.active && a.event.handle==42);
    expected_count=18;assert(!request(&a,18));assert(dispatches==2 && events==2);
    a=(AsyncResult){0};expected_count=1;request_result=0x67c10;
    assert(request(&a,1)==request_result);
    assert(dispatches==3 && closes==3 && events==2 && !a.s.active && !a.event.handle);
    open_result=0x1234;assert(request(&a,1)==open_result);
    assert(dispatches==3 && closes==3 && events==2);
    assert(request(NULL,1));assert(request(&a,0));assert(request(&a,19));assert(request(&a,-1));
    assert(__wrap_nsRequestReceiveApplication(&a,0,55556,app,keys,1,6));
    assert(__wrap_nsRequestReceiveApplication(&a,1,55556,0,keys,1,6));
    assert(__wrap_nsRequestReceiveApplication(&a,1,55556,app,NULL,1,6));
    assert(dispatches==3);
    puts("PASS: native NS receive IPC fixture, 16-byte ABI, metadata buffer, event/object ownership, errors and bounds");
}
