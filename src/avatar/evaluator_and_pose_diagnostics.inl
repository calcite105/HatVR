struct AV_V27TArray64 { uintptr_t Data; int32_t Count; int32_t Max; };
struct AV_V27Atom48 { float scale[4]; float rotation[4]; float translation[4]; };
static_assert(sizeof(AV_V27Atom48)==0x30,"AHiT live BoneAtom entry must be 0x30");
static AV_V27Atom48 g_avV84Solved[128]{}; // v84b: declared after AV_V27Atom48 is complete
// standard/Mixed publication state. This preserves every existing path and gives
// Bow/HCR their own generic master publication generation.
static AV_V27Atom48 g_avV97Published[128]{};
static bool g_avV97PublishedValid=false;
static uintptr_t g_avV97PublishedOwner=0;
static int g_avV97PublishedCount=0;
// BuildRefToLocal receives that exact published pose back (for example while
// the game is paused and animation has not advanced), remove this old delta
// before applying the new one.  Otherwise roomscale accumulates every render.
static float g_avV104PublishedRoomscale[3]{0.0f,0.0f,0.0f};
static bool g_avV104PublishedRoomscaleValid=false;
static ULONGLONG g_avV97LastPublishLog=0;
/*
 v99 SINGLE-SOLVER CHECKPOINT
 ----------------------------
 Every AV_V16ResolveMasterRig success now enters the generic path regardless of
 standard70. The old Standard/Mixed solver and V240 function remain compiled only
 as rollback scaffolding; their runtime routes are disabled. This is deliberate:
 test one-path parity first, then physically delete the dead implementation.

 v98 UNIVERSALIZATION CHECKPOINT
 -------------------------------
 Proven architecture from v97b:
   native game pose -> rig/reference discovery -> selective animation preservation
   -> HMD roomscale -> generic controller arm solve -> live wrist descendants
   -> head suppression -> native BuildRefToLocal -> persistent authoritative
   master SpaceBases -> bForceMeshObjectUpdate -> native UE3 attachments.

 This build deliberately keeps the legacy Standard/Mixed solver as a control
 while removing failed attachment/slave repair mechanisms. Once Hat Kid,
 Nyakuza/Mixed, Bow and HCR all pass this checkpoint, the next cleanup can
 route Standard/Mixed through this same generic solver without changing the
 attachment/publication contract.
*/

static bool AV_V240ForceRenderRefresh(uintptr_t comp,uint32_t& before,uint32_t& after);

static bool g_avV27HookInstalled=false;
static uintptr_t g_avV27TargetComponent=0;
static ULONGLONG g_avV27LastLiveLog=0;
static uintptr_t g_avV27SeenCallers[32]{};
static int g_avV27SeenCallerCount=0;
static ULONGLONG g_avV39LastNativePoseTrace=0;
using AV_V27PoseEvalFn = void(__fastcall*)(void* component,float deltaTime,uint32_t forceUpdate);
static AV_V27PoseEvalFn g_avV27OriginalPoseEval=nullptr;
// LocalAtoms are copied into SkeletalMeshComponent+0x328.  Trace this narrower
// native composition/controller boundary separately.
using AV_V40ComposeFn = void(__fastcall*)(void* component);
static AV_V40ComposeFn g_avV40OriginalCompose=nullptr;
static bool g_avV40ComposeHookInstalled=false;
static ULONGLONG g_avV40LastComposeTrace=0;
static float g_avV119RootRoomscale[3]{0.0f,0.0f,0.0f};
static bool g_avV119RootRoomscaleValid=false;

// Publish ONLY the animated neck's forward/up displacement relative to the root.
// Lateral/right translation is deliberately excluded.
float g_hatVrAnimHeadCameraForwardUU=0.0f;
float g_hatVrAnimHeadCameraUpUU=0.0f;
bool  g_hatVrAnimHeadCameraOffsetValid=false;

// +0x2F0 before LocalAtoms are copied to component+0x328.  Capture that exact
// virtual target at runtime and trace it while 0x6A5180 is evaluating our master.
using AV_V41GetBoneAtomsFn = void(__fastcall*)(
    void* animTree, void* outAtoms, void* requiredBones, void* rootTransform,
    void* arg5, void* arg6);
static AV_V41GetBoneAtomsFn g_avV41OriginalGetBoneAtoms=nullptr;
static void* g_avV41GetBoneAtomsTarget=nullptr;
static bool g_avV41GetBoneAtomsHookInstalled=false;
static thread_local uintptr_t g_avV41EvaluatingComponent=0;
static ULONGLONG g_avV41LastGetBoneAtomsTrace=0;
static ULONGLONG g_avV42LastOutputTrace=0;

using AV_V43SkelControlBoundaryFn = void(__fastcall*)(
    void* component,int boneIndex,int arg3,int arg4,
    uintptr_t arg5,int arg6,void* arg7);
static AV_V43SkelControlBoundaryFn g_avV43OriginalSkelControlBoundary=nullptr;
static bool g_avV43SkelControlBoundaryInstalled=false;
static ULONGLONG g_avV43LastBoundaryTrace=0;
// 0x693810 calls control vtable +0x288/+0x290/+0x298 with:
//   RCX=control, EDX=boneIndex, R8=USkeletalMeshComponent*, R9=TArray*.
// we resolve the live control from the same tables used by 0x693810, hook each
// concrete virtual target dynamically, call its trampoline, then inspect R9.
struct AV_V46TArray64 { uintptr_t Data; int Count; int Max; };
using AV_V46ControlVirtualFn = void(__fastcall*)(void*,int,void*,void*);
struct AV_V46HookRec { void* target; AV_V46ControlVirtualFn original; int slot; };
static AV_V46HookRec g_avV46Hooks[48]{};
static int g_avV46HookCount=0;
static thread_local uintptr_t g_avV46BoundaryComp=0;
static thread_local int g_avV46BoundaryBone=-1;
static thread_local uintptr_t g_avV46BoundaryControl=0;
static ULONGLONG g_avV46LastTrace=0;

static bool AV_V46ReadArray(void* p,AV_V46TArray64& a)
{
    a={}; if(!p || !FP_V111PlausiblePtr((uintptr_t)p)) return false;
    __try { memcpy(&a,p,sizeof(a)); } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    if(a.Count<0 || a.Max<0 || a.Count>a.Max || a.Count>512) return false;
    if(a.Count && !FP_V111PlausiblePtr(a.Data)) return false;
    return true;
}
static AV_V46ControlVirtualFn AV_V46FindOriginal(void* control,int slot)
{
    if(!control || !FP_V111PlausiblePtr((uintptr_t)control)) return nullptr;
    void* target=nullptr;
    __try {
        uintptr_t vt=*(uintptr_t*)control;
        target=*(void**)(vt+(uintptr_t)slot);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    for(int i=0;i<g_avV46HookCount;++i)
        if(g_avV46Hooks[i].target==target && g_avV46Hooks[i].slot==slot)
            return g_avV46Hooks[i].original;
    return nullptr;
}
static void AV_V46TraceReturnedArray(
    int slot,void* control,int boneIndex,void* component,void* outArray)
{
    if((uintptr_t)component!=g_avV46BoundaryComp ||
       boneIndex!=g_avV46BoundaryBone ||
       (uintptr_t)control!=g_avV46BoundaryControl) return;

    AV_V46TArray64 a{};
    const bool ok=AV_V46ReadArray(outArray,a);
    const ULONGLONG now=GetTickCount64();
    if(now-g_avV46LastTrace<35 && slot!=0x290) return;
    if(slot==0x290) g_avV46LastTrace=now;

    char line[1900]{}; int n=0;
    n+=sprintf_s(line+n,sizeof(line)-n,
        "AV_V49_CONTROL_VIRTUAL slot=0x%X control=%p comp=%p listBone=%d "
        "out=%p sane=%d count=%d max=%d data=%p values=",
        slot,control,component,boneIndex,outArray,ok?1:0,a.Count,a.Max,(void*)a.Data);

    if(ok && a.Count>0){
        const int cap=a.Count<12?a.Count:12;
        if(slot==0x288){
            for(int i=0;i<cap && n<(int)sizeof(line)-80;++i){
                int v=-9999; bool hv=false;
                __try { v=*(int*)(a.Data+(uintptr_t)i*4); hv=true; }
                __except(EXCEPTION_EXECUTE_HANDLER) { hv=false; }
                n+=sprintf_s(line+n,sizeof(line)-n,"%s%s%d",i?",":"",hv?"":"!",v);
            }
        }else if(slot==0x290){
            for(int i=0;i<cap && n<(int)sizeof(line)-190;++i){
                AV_V27Atom48 at{}; bool hv=false;
                __try { memcpy(&at,(void*)(a.Data+(uintptr_t)i*0x30),sizeof(at)); hv=true; }
                __except(EXCEPTION_EXECUTE_HANDLER) { hv=false; }
                if(!hv){ n+=sprintf_s(line+n,sizeof(line)-n,"%s<fault>",i?",":""); break; }
                n+=sprintf_s(line+n,sizeof(line)-n,
                    "%sP[%+.2f,%+.2f,%+.2f]Q[%+.3f,%+.3f,%+.3f,%+.3f]",
                    i?",":"",at.translation[0],at.translation[1],at.translation[2],
                    at.rotation[0],at.rotation[1],at.rotation[2],at.rotation[3]);
            }
        }else{
            // Unknown +0x298 element type: log first qwords only, without interpreting.
            const int qcap=(a.Count<8?a.Count:8);
            for(int i=0;i<qcap && n<(int)sizeof(line)-80;++i){
                unsigned long long q=0; bool hv=false;
                __try { q=*(unsigned long long*)(a.Data+(uintptr_t)i*8); hv=true; }
                __except(EXCEPTION_EXECUTE_HANDLER) { hv=false; }
                n+=sprintf_s(line+n,sizeof(line)-n,"%s%s0x%llX",i?",":"",hv?"":"!",
                    q);
            }
        }
        if(a.Count>cap) n+=sprintf_s(line+n,sizeof(line)-n,",...(+%d)",a.Count-cap);
    }
    n+=sprintf_s(line+n,sizeof(line)-n,"\n");
    FP_Log(line);
}
static void __fastcall AV_V46Hook288(void* c,int b,void* comp,void* out)
{
    auto fn=AV_V46FindOriginal(c,0x288); if(fn) fn(c,b,comp,out);
    AV_V46TraceReturnedArray(0x288,c,b,comp,out);
}
static void __fastcall AV_V46Hook290(void* c,int b,void* comp,void* out)
{
    auto fn=AV_V46FindOriginal(c,0x290); if(fn) fn(c,b,comp,out);
    AV_V46TraceReturnedArray(0x290,c,b,comp,out);
}
static void __fastcall AV_V46Hook298(void* c,int b,void* comp,void* out)
{
    auto fn=AV_V46FindOriginal(c,0x298); if(fn) fn(c,b,comp,out);
    AV_V46TraceReturnedArray(0x298,c,b,comp,out);
}
static bool AV_V46EnsureHook(void* control,int slot,void* detour)
{
    if(!control || !FP_V111PlausiblePtr((uintptr_t)control)) return false;
    void* target=nullptr;
    __try {
        uintptr_t vt=*(uintptr_t*)control;
        target=*(void**)(vt+(uintptr_t)slot);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    if(!target || !FP_V111PlausiblePtr((uintptr_t)target)) return false;
    for(int i=0;i<g_avV46HookCount;++i)
        if(g_avV46Hooks[i].target==target && g_avV46Hooks[i].slot==slot) return true;
    if(g_avV46HookCount>=48) return false;

    AV_V46ControlVirtualFn original=nullptr;
    MH_STATUS st=MH_CreateHook(target,detour,reinterpret_cast<void**>(&original));
    if(st!=MH_OK && st!=MH_ERROR_ALREADY_CREATED) return false;
    // if another slot/class already created this exact target, recover its trampoline.
    if(st==MH_ERROR_ALREADY_CREATED){
        for(int i=0;i<g_avV46HookCount;++i)
            if(g_avV46Hooks[i].target==target){ original=g_avV46Hooks[i].original; break; }
        if(!original) return false;
    }
    st=MH_EnableHook(target);
    if(st!=MH_OK && st!=MH_ERROR_ENABLED) return false;
    g_avV46Hooks[g_avV46HookCount++]={target,original,slot};

    char l[360]{};
    sprintf_s(l,sizeof(l),
        "AV_V49_CONTROL_HOOK_INSTALLED slot=0x%X control=%p target=%p original=%p total=%d\n",
        slot,control,target,(void*)original,g_avV46HookCount);
    FP_Log(l);
    return true;
}
static uintptr_t AV_V46ResolveControl(
    uintptr_t comp,int boneIndex,int arg3,int arg4,uintptr_t arg5)
{
    // Mirrors 0x69383F..0x69388F. arg5 is the fifth native argument and is
    // dereferenced by 0x693881 as the object containing SkelControlLists at +0x1AC.
    if(!comp || !arg5 || boneIndex<0) return 0;
    __try {
        const uintptr_t indexArrayBase =
            ((arg3==0 && arg4==0) ? comp+0x44C : comp+0x43C);
        const uintptr_t indexData=*(uintptr_t*)indexArrayBase;
        const int indexCount=*(int*)(indexArrayBase+8);
        if(!indexData || boneIndex>=indexCount) return 0;
        const unsigned char listIndex=*(unsigned char*)(indexData+(uintptr_t)boneIndex);
        if(listIndex==0xFF) return 0;
        // Exact 0x693881..0x69388F reconstruction:
        //   RAX = stack arg5 itself
        //   RCX = [RAX + 0x1AC]
        //   R14 = [RCX + listIndex*20 + 8]
        // recover the same R14 control pointer as the native function.
        const uintptr_t listsOwner=arg5;
        const uintptr_t lists=*(uintptr_t*)(listsOwner+0x1AC);
        if(!lists) return 0;
        // 0x69387D: listIndex*5 dwords; 0x69388F loads qword at +8.
        return *(uintptr_t*)(lists+(uintptr_t)listIndex*20+8);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// F5 now toggles a REAL post-build bone-34 matrix translation override; the old
// SpaceBases +80 write is disabled so this test isolates render-side skinning data.
static bool g_avV29HookInstalled=false;
static bool g_avV29BranchHooksInstalled=false;
static bool g_avV29VFuncHookInstalled=false;

static bool g_avV29PrevF5Down=false;
static ULONGLONG g_avV29Last4B90Log=0;
static ULONGLONG g_avV29LastStageLog=0;
static uint64_t g_avV29HitCount=0;

// the log can correlate one 0x6A4980 invocation with the branch/vfunc calls that
// immediately follow it without flooding the log forever.
static volatile LONG g_avV210CaptureRemaining=0;
static volatile LONG64 g_avV210GlobalPassId=0;
static thread_local uint64_t g_avV210TlsPassId=0;
static thread_local uintptr_t g_avV210TlsCallerRva=0;
static thread_local DWORD g_avV210TlsThreadId=0;
static thread_local uintptr_t g_avV211TlsComponent=0;

using AV_V29PostPoseFn = void(__fastcall*)(void* component,float deltaTime);
using AV_V29BranchWithArgFn = void(__fastcall*)(void* component,void* arg);
using AV_V29BranchFn = void(__fastcall*)(void* component);
using AV_V29VFunc290Fn = void(__fastcall*)(void* component);
static AV_V29PostPoseFn g_avV29OriginalPostPose=nullptr;
static AV_V29BranchWithArgFn g_avV29Original3590D0=nullptr;
static AV_V29BranchFn g_avV29Original359110=nullptr;
static AV_V29BranchFn g_avV29Original358780=nullptr;
static AV_V29VFunc290Fn g_avV29OriginalVFunc290=nullptr;
static uintptr_t g_avV29VFunc290Target=0;

// Signature recovered from the exact HatinTimeGame.exe: RCX=output TArray,
// RDX=SkeletalMeshComponent, R8D=LOD index, R9=extra build data.
using AV_V215BuildRefToLocalFn = void(__fastcall*)(AV_V27TArray64* output, void* component, int lodIndex, void* extra);
static AV_V215BuildRefToLocalFn g_avV215OriginalBuildRefToLocal=nullptr;
static bool g_avV215HookInstalled=false;
static ULONGLONG g_avV215LastLog=0;

// the normal camera-update path, then force UE3's mesh-object refresh just like
static bool g_avV240PublishedPoseValid=false;
static AV_V27Atom48 g_avV240PublishedRight[15]{}; // preserve known-good right subtree
static bool g_avV241PublishedLeftValid=false;
static AV_V27Atom48 g_avV241PublishedLeft[14]{}; // 18..31 left arm/hand/sleeve
// BuildRefToLocal. Cache the corrected root/head chain too so Hat Kid's hat follows
// the same HMD-rebased skeleton that is rendered.
static bool g_avV242PublishedHeadChainValid=false;
static AV_V27Atom48 g_avV242PublishedHeadChain[18]{}; // bones 0..17

// the RefSkeleton parent table decides which bones inherit a wrist transform;
// no contiguous-index assumptions are used for hand descendants.
static bool g_avV259BaselineAllValid=false;
static AV_V27Atom48 g_avV259BaselineAll[70]{};
static bool g_avV259PublishedArmValid=false;
static AV_V27Atom48 g_avV259PublishedArm[128]{};
static bool g_avV259PublishedArmMask[128]{};
static uintptr_t g_avV37PublishedOwner=0;
static uintptr_t g_avV37PublishedMesh=0;
static int g_avV37PublishedCount=0;

static int g_avV240ForceMeshObjectUpdateOffset=-1;
static ULONGLONG g_avV240LastPublishLog=0;

static bool AV_V27GetSpaceBases(uintptr_t comp, AV_V27TArray64& sb)
{
    sb={};
    return FP_V111PlausiblePtr(comp) &&
        FP_ReadMemory((const void*)(comp+0x318),&sb,sizeof(sb)) &&
        sb.Count>34 && sb.Count<=512 && sb.Max>=sb.Count && sb.Max<=1024 &&
        FP_V111PlausiblePtr(sb.Data);
}

// 0x6A5180 evaluator accesses both LocalAtoms and SpaceBases, so trace that
// native boundary rather than guessing another mutation point.
static bool AV_V39GetLocalAtoms(uintptr_t comp,AV_V27TArray64& a)
{
    a={};
    return FP_V111PlausiblePtr(comp) &&
        FP_ReadMemory((const void*)(comp+0x328),&a,sizeof(a)) &&
        a.Count>0 && a.Count<=128 && a.Max>=a.Count && a.Max<=256 &&
        FP_V111PlausiblePtr(a.Data);
}
static bool AV_V39ReadAtomSafe(const AV_V27TArray64& a,int b,AV_V27Atom48& out)
{
    return b>=0 && b<a.Count && FP_V111PlausiblePtr(a.Data) &&
        FP_ReadMemory((const void*)(a.Data+(uintptr_t)b*sizeof(AV_V27Atom48)),&out,sizeof(out));
}
static float AV_V39QDotAbs(const AV_V27Atom48& a,const AV_V27Atom48& b)
{
    return fabsf(a.rotation[0]*b.rotation[0]+a.rotation[1]*b.rotation[1]+
                 a.rotation[2]*b.rotation[2]+a.rotation[3]*b.rotation[3]);
}
static float AV_V39PosDelta(const AV_V27Atom48& a,const AV_V27Atom48& b)
{
    const float x=a.translation[0]-b.translation[0],y=a.translation[1]-b.translation[1],
                z=a.translation[2]-b.translation[2];
    return sqrtf(x*x+y*y+z*z);
}

static bool AV_V27ReadHandX(const AV_V27TArray64& sb,float& x)
{
    AV_V27Atom48 a{};
    if(!FP_ReadMemory((const void*)(sb.Data+(uintptr_t)34*0x30),&a,sizeof(a)))return false;
    x=a.translation[0];
    return isfinite(x) && fabsf(x)<10000.0f;
}

static bool AV_V28WriteHandX(const AV_V27TArray64& sb,float x)
{
    if(!isfinite(x) || fabsf(x)>=10000.0f || !FP_V111PlausiblePtr(sb.Data) || sb.Count<=34)
        return false;

    const uintptr_t address =
        sb.Data + (uintptr_t)34 * 0x30 + offsetof(AV_V27Atom48, translation);

    __try
    {
        *reinterpret_cast<float*>(address)=x;
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static uintptr_t AV_V27ToExeRva(uintptr_t address)
{
    const uintptr_t exe=(uintptr_t)GetModuleHandleW(nullptr);
    if(!exe || address<exe)return 0;
    const uintptr_t rva=address-exe;
    return rva<0x02000000 ? rva : 0;
}

static bool AV_V27MarkCaller(uintptr_t callerRva)
{
    if(!callerRva)return false;
    for(int i=0;i<g_avV27SeenCallerCount;i++)
        if(g_avV27SeenCallers[i]==callerRva)return false;
    if(g_avV27SeenCallerCount<(int)(sizeof(g_avV27SeenCallers)/sizeof(g_avV27SeenCallers[0])))
        g_avV27SeenCallers[g_avV27SeenCallerCount++]=callerRva;
    return true;
}

static void AV_V27LogStack(void* component,uintptr_t callerRva,void** stack,USHORT frames)
{
    char line[1024]{};
    int n=sprintf_s(line,sizeof(line),
        "AV_V27_NEW_CALLER component=%p callerRva=0x%llX stack=",
        component,(unsigned long long)callerRva);

    for(USHORT i=0;i<frames && n>0 && n<(int)sizeof(line)-64;i++){
        const uintptr_t a=(uintptr_t)stack[i];
        const uintptr_t r=AV_V27ToExeRva(a);
        if(r) n+=sprintf_s(line+n,sizeof(line)-n,"%s0x%llX",i?" <- ":"",(unsigned long long)r);
        else  n+=sprintf_s(line+n,sizeof(line)-n,"%s%p",i?" <- ":"",stack[i]);
    }

    if(n>0 && n<(int)sizeof(line)-2){
        line[n++]='\n';
        line[n]=0;
    }
    FP_Log(line);
}

static void AV_V39TraceNativePose(
    uintptr_t comp,void* component,uintptr_t callerRva,
    const AV_V27Atom48* preSb,int preSbCount,
    const AV_V27Atom48* preLocal,int preLocalCount);

static void AV_V41MaybeInstallGetBoneAtomsHook(uintptr_t comp);
static void AV_V41TraceGetBoneAtoms(
    uintptr_t comp,void* animTree,uintptr_t callerRva,
    const AV_V27Atom48* preSb,int preSbCount,
    const AV_V27Atom48* preLocal,int preLocalCount);

static void AV_V42TraceGetBoneAtomsOutput(
    uintptr_t comp,void* animTree,void* outAtoms,void* requiredBones,
    void* rootTransform,void* arg5,void* arg6,uintptr_t callerRva,
    const AV_V27TArray64& preOut,bool preOutReadable);

static void AV_V40TraceComposeBoundary(
    uintptr_t comp,void* component,uintptr_t callerRva,
    const AV_V27Atom48* preSb,int preSbCount,
    const AV_V27Atom48* preLocal,int preLocalCount);

static bool AV_V43InstallSkelControlBoundaryHook();

//
// Ground truth from HatinTimeGame.exe:
//   0x693A00 mov rax,[r14]
//   0x693A03 lea r9,[scratch288]
//   0x693A0A mov r8,rdi
//   0x693A0D mov edx,ebx
//   0x693A0F mov rcx,r14
//   0x693A12 call qword ptr [rax+0x288]
//
// Same shape at 0x693A6F (+0x290) and 0x693ACD (+0x298).
//
// we replace ONLY each six-byte indirect CALL with CALL rel32 -> a tiny thunk
// allocated near the EXE. The thunk receives the exact native register state,
// calls the exact virtual target from the live vtable, then reports the output
// TArray immediately after the virtual returns. No arg5/list reconstruction.

static bool g_avV50Installed=false;

static void __fastcall AV_V50AfterVirtual(
    uintptr_t slot, void* control, int boneIndex, void* component, void* outArray)
{
    static ULONGLONG last288=0,last290=0,last298=0;
    ULONGLONG now=GetTickCount64();
    ULONGLONG* last=(slot==0x288?&last288:(slot==0x290?&last290:&last298));
    if(now-*last<100) return;
    *last=now;

    uintptr_t vt=0,target=0,data=0;
    int count=0,max=0;
    __try {
        vt=control?*(uintptr_t*)control:0;
        target=vt?*(uintptr_t*)(vt+slot):0;
        if(outArray){
            data=*(uintptr_t*)outArray;
            count=*(int*)((uintptr_t)outArray+8);
            max=*(int*)((uintptr_t)outArray+12);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    char l[1800]{};
    int n=sprintf_s(l,sizeof(l),
        "AV_V50_NATIVE_R14_RETURN slot=0x%llX control=%p vt=%p target=%p "
        "bone=%d comp=%p out=%p data=%p count=%d max=%d",
        (unsigned long long)slot,control,(void*)vt,(void*)target,
        boneIndex,component,outArray,(void*)data,count,max);

    if(data && count>0 && count<256){
        if(slot==0x288){
            n+=sprintf_s(l+n,sizeof(l)-n," ints=[");
            __try {
                int lim=count<24?count:24;
                for(int i=0;i<lim;i++)
                    n+=sprintf_s(l+n,sizeof(l)-n,"%s%d",i?",":"",((int*)data)[i]);
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
            n+=sprintf_s(l+n,sizeof(l)-n,"]");
        } else if(slot==0x290) {
            n+=sprintf_s(l+n,sizeof(l)-n," atoms48=[");
            __try {
                int lim=count<6?count:6;
                for(int i=0;i<lim;i++){
                    float* f=(float*)(data+(uintptr_t)i*48);
                    n+=sprintf_s(l+n,sizeof(l)-n,
                        "%s{%g,%g,%g,%g | %g,%g,%g | %g,%g,%g}",
                        i?";":"",
                        f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[8],f[9],f[10]);
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
            n+=sprintf_s(l+n,sizeof(l)-n,"]");
        }
    }
    sprintf_s(l+n,sizeof(l)-n,"\n");
    FP_Log(l);
}

static void* AV_V50AllocNear(uintptr_t target,size_t size)
{
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    const uintptr_t gran=(uintptr_t)si.dwAllocationGranularity;
    const uintptr_t lo=(target>0x70000000ULL)?target-0x70000000ULL:gran;
    const uintptr_t hi=target+0x70000000ULL;
    uintptr_t base=target&~(gran-1);
    for(uintptr_t d=gran; d<0x70000000ULL; d+=gran){
        uintptr_t cand[2]={base+d,base>d?base-d:0};
        for(int k=0;k<2;k++){
            if(!cand[k] || cand[k]<lo || cand[k]>hi) continue;
            void* p=VirtualAlloc((void*)cand[k],size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
            if(p) return p;
        }
    }
    return nullptr;
}

static bool AV_V50PatchCallsite(uintptr_t exe,uintptr_t rva,uint32_t slot)
{
    unsigned char* site=(unsigned char*)(exe+rva);
    __try {
        if(site[0]!=0xFF || site[1]!=0x90 || *(uint32_t*)(site+2)!=slot){
            char l[320]{};
            sprintf_s(l,sizeof(l),
                "AV_V50_CALLSITE_VERIFY_FAIL rva=0x%llX bytes=%02X %02X slot=%08X expected=%08X\n",
                (unsigned long long)rva,site[0],site[1],*(uint32_t*)(site+2),slot);
            FP_Log(l); return false;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }

    unsigned char* t=(unsigned char*)AV_V50AllocNear((uintptr_t)site,0x1000);
    if(!t){ FP_Log("AV_V50_ALLOC_NEAR_FAIL\n"); return false; }

    // Thunk:
    // sub rsp,58h
    // save native rcx/rdx/r8/r9 and vtable-derived target
    // call target
    // prepare AV_V50AfterVirtual(slot,control,bone,component,outArray)
    // call helper absolute
    // add rsp,58h ; ret
    unsigned char* p=t;
    auto b=[&](unsigned char x){*p++=x;};
    auto q=[&](uint64_t x){memcpy(p,&x,8);p+=8;};
    auto d=[&](uint32_t x){memcpy(p,&x,4);p+=4;};

    // sub rsp,58
    b(0x48);b(0x83);b(0xEC);b(0x58);
    // [rsp+28]=control, +30=bone, +38=component, +40=out
    b(0x48);b(0x89);b(0x4C);b(0x24);b(0x28);
    b(0x48);b(0x89);b(0x54);b(0x24);b(0x30);
    b(0x4C);b(0x89);b(0x44);b(0x24);b(0x38);
    b(0x4C);b(0x89);b(0x4C);b(0x24);b(0x40);
    // r11=[rax+slot], call r11
    b(0x4C);b(0x8B);b(0x98);d(slot);
    b(0x41);b(0xFF);b(0xD3);
    // helper args: rcx=slot, rdx=control, r8d=bone, r9=component, [rsp+20]=out
    b(0x48);b(0xC7);b(0xC1);d(slot);
    b(0x48);b(0x8B);b(0x54);b(0x24);b(0x28);
    b(0x44);b(0x8B);b(0x44);b(0x24);b(0x30);
    b(0x4C);b(0x8B);b(0x4C);b(0x24);b(0x38);
    b(0x48);b(0x8B);b(0x44);b(0x24);b(0x40);
    b(0x48);b(0x89);b(0x44);b(0x24);b(0x20);
    // mov rax,helper ; call rax
    b(0x48);b(0xB8);q((uint64_t)(uintptr_t)&AV_V50AfterVirtual);
    b(0xFF);b(0xD0);
    // add rsp,58 ; ret
    b(0x48);b(0x83);b(0xC4);b(0x58);b(0xC3);
    FlushInstructionCache(GetCurrentProcess(),t,(SIZE_T)(p-t));

    intptr_t rel=(intptr_t)t-((intptr_t)site+5);
    if(rel<INT32_MIN || rel>INT32_MAX){
        FP_Log("AV_V50_REL32_RANGE_FAIL\n"); return false;
    }
    DWORD old=0;
    if(!VirtualProtect(site,6,PAGE_EXECUTE_READWRITE,&old)) return false;
    site[0]=0xE8; *(int32_t*)(site+1)=(int32_t)rel; site[5]=0x90;
    FlushInstructionCache(GetCurrentProcess(),site,6);
    DWORD tmp=0; VirtualProtect(site,6,old,&tmp);

    char l[420]{};
    sprintf_s(l,sizeof(l),
        "AV_V50_CALLSITE_PATCHED rva=0x%llX slot=0x%X site=%p thunk=%p helper=%p\n",
        (unsigned long long)rva,slot,site,t,(void*)&AV_V50AfterVirtual);
    FP_Log(l);
    return true;
}

static void AV_V50InstallNativeR14Probe()
{
    if(g_avV50Installed) return;
    uintptr_t exe=(uintptr_t)GetModuleHandleW(nullptr);
    if(!exe) return;
    bool a=AV_V50PatchCallsite(exe,0x693A12,0x288);
    bool b=AV_V50PatchCallsite(exe,0x693A6F,0x290);
    bool c=AV_V50PatchCallsite(exe,0x693ACD,0x298);
    g_avV50Installed=a&&b&&c;
    char l[320]{};
    sprintf_s(l,sizeof(l),
        "AV_V50_NATIVE_R14_CALLSITE_PROBE install=[%d,%d,%d] complete=%d\n",
        a?1:0,b?1:0,c?1:0,g_avV50Installed?1:0);
    FP_Log(l);
}

static void __fastcall AV_V43HookedSkelControlBoundary(
    void* component,int boneIndex,int arg3,int arg4,
    uintptr_t arg5,int arg6,void* arg7);

static void __fastcall AV_V41HookedGetBoneAtoms(
    void* animTree,void* outAtoms,void* requiredBones,void* rootTransform,
    void* arg5,void* arg6)
{
    const uintptr_t comp=g_avV41EvaluatingComponent;
    const bool ours=g_fpV1Enabled && comp &&
                    comp==g_avV27TargetComponent &&
                    FP_V111PlausiblePtr(comp);

    AV_V27TArray64 preSb{},preLocal{};
    AV_V27Atom48 preSbAtoms[128]{},preLocalAtoms[128]{};
    int preSbCount=0,preLocalCount=0;
    if(ours && AV_V27GetSpaceBases(comp,preSb)){
        preSbCount=(preSb.Count<128)?preSb.Count:128;
        FP_ReadMemory((const void*)preSb.Data,preSbAtoms,
                      (size_t)preSbCount*sizeof(AV_V27Atom48));
    }
    if(ours && AV_V39GetLocalAtoms(comp,preLocal)){
        preLocalCount=(preLocal.Count<128)?preLocal.Count:128;
        FP_ReadMemory((const void*)preLocal.Data,preLocalAtoms,
                      (size_t)preLocalCount*sizeof(AV_V27Atom48));
    }

    AV_V27TArray64 preOut{};
    const bool preOutReadable=ours && outAtoms &&
        FP_ReadMemory(outAtoms,&preOut,sizeof(preOut));

    if(g_avV41OriginalGetBoneAtoms)
        g_avV41OriginalGetBoneAtoms(animTree,outAtoms,requiredBones,rootTransform,arg5,arg6);

    if(ours){
        const uintptr_t callerRva=AV_V27ToExeRva((uintptr_t)_ReturnAddress());
        AV_V42TraceGetBoneAtomsOutput(
            comp,animTree,outAtoms,requiredBones,rootTransform,arg5,arg6,
            callerRva,preOut,preOutReadable);
        AV_V41TraceGetBoneAtoms(
            comp,animTree,callerRva,
            preSbAtoms,preSbCount,preLocalAtoms,preLocalCount);
    }
}

static void __fastcall AV_V40HookedCompose(void* component)
{
    // Its root case (0x698EA9) copies LocalAtoms[0] -> SpaceBases[0], and every
    // non-root bone then composes against its parent's freshly-built SpaceBases.
    const uintptr_t comp=(uintptr_t)component;
    const bool ours=g_fpV1Enabled &&
                    comp==g_avV27TargetComponent &&
                    FP_V111PlausiblePtr(comp);

    AV_V27TArray64 local{};
    AV_V27Atom48 savedRoot{};
    bool patchedRoot=false;

    if(ours && g_avV119RootRoomscaleValid &&
       AV_V39GetLocalAtoms(comp,local) && local.Count>0 &&
       FP_V111PlausiblePtr(local.Data))
    {
        __try{
            memcpy(&savedRoot,(const void*)local.Data,sizeof(savedRoot));
            AV_V27Atom48 root=savedRoot;
            root.translation[0]+=g_avV119RootRoomscale[0];
            root.translation[1]+=g_avV119RootRoomscale[1];
            root.translation[2]+=g_avV119RootRoomscale[2];
            memcpy((void*)local.Data,&root,sizeof(root));
            patchedRoot=true;
        }__except(EXCEPTION_EXECUTE_HANDLER){ patchedRoot=false; }
    }

    if(g_avV40OriginalCompose)
        g_avV40OriginalCompose(component);

    // Restore only LocalAtoms. SpaceBases now contains the generation UE3 built
    // from the translated root, including native parent->child inheritance.
    if(patchedRoot){
        __try{ memcpy((void*)local.Data,&savedRoot,sizeof(savedRoot)); }
        __except(EXCEPTION_EXECUTE_HANDLER){}
    }
}

static void __fastcall AV_V27HookedPoseEval(void* component,float deltaTime,uint32_t forceUpdate)
{
    void* returnAddress=_ReturnAddress();
    const uintptr_t callerRva=AV_V27ToExeRva((uintptr_t)returnAddress);
    const uintptr_t comp=(uintptr_t)component;
    const bool ours=g_fpV1Enabled &&
                    comp==g_avV27TargetComponent &&
                    FP_V111PlausiblePtr(comp);

    AV_V27TArray64 preSb{},preLocal{};
    float preX=0.0f,postX=0.0f;
    const bool havePre=ours &&
                       AV_V27GetSpaceBases(comp,preSb) &&
                       AV_V27ReadHandX(preSb,preX);
    const bool havePreLocal=ours && AV_V39GetLocalAtoms(comp,preLocal);
    AV_V27Atom48 v39PreSb[128]{},v39PreLocal[128]{};
    int v39PreSbCount=0,v39PreLocalCount=0;
    if(havePre){
        v39PreSbCount=(preSb.Count<128)?preSb.Count:128;
        FP_ReadMemory((const void*)preSb.Data,v39PreSb,
                      (size_t)v39PreSbCount*sizeof(AV_V27Atom48));
    }
    if(havePreLocal){
        v39PreLocalCount=(preLocal.Count<128)?preLocal.Count:128;
        FP_ReadMemory((const void*)preLocal.Data,v39PreLocal,
                      (size_t)v39PreLocalCount*sizeof(AV_V27Atom48));
    }

    if(ours && AV_V27MarkCaller(callerRva)){
        void* stack[10]{};
        const USHORT frames=CaptureStackBackTrace(0,10,stack,nullptr);
        AV_V27LogStack(component,callerRva,stack,frames);
    }

    if(ours) AV_V41MaybeInstallGetBoneAtomsHook(comp);
    const uintptr_t v41PrevComp=g_avV41EvaluatingComponent;
    if(ours) g_avV41EvaluatingComponent=comp;
    if(g_avV27OriginalPoseEval)
        g_avV27OriginalPoseEval(component,deltaTime,forceUpdate);
    g_avV41EvaluatingComponent=v41PrevComp;

    if(!ours)return;

    // and parent-hierarchy helpers it uses. Keeping this early hook independent
    // fixes C++ declaration-order errors without changing runtime behavior.
    AV_V39TraceNativePose(
        comp,component,callerRva,
        v39PreSb,v39PreSbCount,v39PreLocal,v39PreLocalCount);

    AV_V27TArray64 postSb{};
    const bool havePost=AV_V27GetSpaceBases(comp,postSb) &&
                        AV_V27ReadHandX(postSb,postX);

    const ULONGLONG now=GetTickCount64();
    if(now-g_avV27LastLiveLog>=500){
        g_avV27LastLiveLog=now;
        char line[640]{};
        sprintf_s(line,sizeof(line),
            "AV_V27_LIVE_CALL component=%p caller=%p callerRva=0x%llX dt=%.5f force=%u "
            "preSB=%p preCount=%d preX=%s%.5f postSB=%p postCount=%d postX=%s%.5f changed=%.5f\n",
            component,returnAddress,(unsigned long long)callerRva,deltaTime,forceUpdate,
            (void*)preSb.Data,preSb.Count,havePre?"":"NA/",havePre?preX:0.0f,
            (void*)postSb.Data,postSb.Count,havePost?"":"NA/",havePost?postX:0.0f,
            (havePre&&havePost)?(postX-preX):0.0f);
        FP_Log(line);
    }
}

//
// F5 still toggles the +80 handR SpaceBases write, but now it also starts a
// bounded detailed capture (180 post-pose calls).  Every 0x6A4980 invocation is
// assigned a pass id and records thread id + return-address RVA.  The immediate
// 0x35A3B0 / 0x35A3F0 / 0x359A60 / vfunc+0x290 hooks inherit that TLS pass id,
// letting the log show exactly which downstream path belongs to which pose pass.
static bool AV_V29ReadCurrentHandX(void* component, AV_V27TArray64& sb, float& x)
{
    return AV_V27GetSpaceBases((uintptr_t)component,sb) && AV_V27ReadHandX(sb,x);
}

static bool AV_V210CaptureActive()
{
    return InterlockedCompareExchange(&g_avV210CaptureRemaining,0,0)>0;
}

static uintptr_t AV_V211ReadParentAnim(uintptr_t comp)
{
    uintptr_t parent=0;
    if(!FP_V111PlausiblePtr(comp) || g_avV14ParentAnimComponentOffset<0) return 0;
    FP_V18ReadPtr(comp+(uintptr_t)g_avV14ParentAnimComponentOffset,parent);
    return parent;
}

static void AV_V210LogStage(const char* stage, void* component, bool after)
{
    const uintptr_t comp=(uintptr_t)component;
    if(!AV_V210CaptureActive() || !FP_V111PlausiblePtr(comp)) return;
    // these functions are generic. Only associate a stage with the most recent
    // 0x6A4980 pass on this thread when it is operating on the same component.
    if(!g_avV210TlsPassId || comp!=g_avV211TlsComponent) return;

    AV_V27TArray64 sb{};
    float x=0.0f;
    const bool have=AV_V29ReadCurrentHandX(component,sb,x);
    const DWORD tid=GetCurrentThreadId();
    const uintptr_t parent=AV_V211ReadParentAnim(comp);
    const bool selected=(comp==g_avV27TargetComponent);

    char line[768]{};
    sprintf_s(line,sizeof(line),
        "AV_V212_STAGE pass=%llu tid=%lu callerRva=0x%llX stage=%s side=%s "
        "component=%p selected=%d parent=%p SB=%p count=%d handX=%s%.5f probe=%d\n",
        (unsigned long long)g_avV210TlsPassId,(unsigned long)tid,
        (unsigned long long)g_avV210TlsCallerRva,stage,after?"POST":"PRE",
        component,selected?1:0,(void*)parent,(void*)sb.Data,sb.Count,
        have?"":"NA/",have?x:0.0f,g_avV29ProbeEnabled?1:0);
    FP_Log(line);
}

static void __fastcall AV_V29HookedPostPose(void* component,float deltaTime)
{
    const uintptr_t comp=(uintptr_t)component;
    const bool plausible=FP_V111PlausiblePtr(comp);
    const bool selected=g_fpV1Enabled && plausible && comp==g_avV27TargetComponent;
    const bool detailed=AV_V210CaptureActive();

    ++g_avV29HitCount;
    const uintptr_t callerRva=AV_V27ToExeRva((uintptr_t)_ReturnAddress());
    const DWORD tid=GetCurrentThreadId();
    const uint64_t passId=(uint64_t)InterlockedIncrement64(&g_avV210GlobalPassId);
    g_avV210TlsPassId=passId;
    g_avV210TlsCallerRva=callerRva;
    g_avV210TlsThreadId=tid;
    g_avV211TlsComponent=comp;

    AV_V27TArray64 preSb{},postSb{};
    float preX=0.0f,injectX=0.0f,postX=0.0f;
    const bool havePre=plausible && AV_V29ReadCurrentHandX(component,preSb,preX);
    const uintptr_t parent=plausible ? AV_V211ReadParentAnim(comp) : 0;

    bool wrote=false,haveInject=false;
    // 70-bone component whose ParentAnimComponent points directly at it.
    // this deliberately excludes unrelated characters/components.
    const bool familyMember =
        g_fpV1Enabled && plausible && havePre && preSb.Count==70 &&
        (selected || (FP_V111PlausiblePtr(g_avV27TargetComponent) &&
                      parent==g_avV27TargetComponent));
    // that write survives downstream but does not deform the rendered mesh.
    (void)familyMember;

    if(g_avV29OriginalPostPose) g_avV29OriginalPostPose(component,deltaTime);
    const bool havePost=plausible && AV_V29ReadCurrentHandX(component,postSb,postX);

    const ULONGLONG now=GetTickCount64();
    if(detailed || (selected && (g_avV29HitCount<=5 || now-g_avV29Last4B90Log>=500))){
        g_avV29Last4B90Log=now;
        char line[1024]{};
        sprintf_s(line,sizeof(line),
            "AV_V212_PASS pass=%llu hit=%llu tid=%lu callerRva=0x%llX component=%p "
            "selected=%d family=%d parent=%p dt=%.5f probe=%d preSB=%p preCount=%d PRE=%s%.5f "
            "wrote=%d INJECT=%s%.5f postSB=%p postCount=%d POST=%s%.5f postMinusInject=%.5f\n",
            (unsigned long long)passId,(unsigned long long)g_avV29HitCount,
            (unsigned long)tid,(unsigned long long)callerRva,component,selected?1:0,
            familyMember?1:0,(void*)parent,deltaTime,g_avV29ProbeEnabled?1:0,(void*)preSb.Data,preSb.Count,
            havePre?"":"NA/",havePre?preX:0.0f,wrote?1:0,
            haveInject?"":"NA/",haveInject?injectX:0.0f,(void*)postSb.Data,postSb.Count,
            havePost?"":"NA/",havePost?postX:0.0f,
            (havePost&&haveInject)?postX-injectX:0.0f);
        FP_Log(line);
    }

    if(detailed){
        const LONG left=InterlockedDecrement(&g_avV210CaptureRemaining);
        if(left==0) FP_Log("AV_V212_CAPTURE_END -- 180 unfiltered post-pose passes captured\n");
    }
}

static void __fastcall AV_V29Hooked3590D0(void* component,void* arg)
{
    AV_V210LogStage("3590D0",component,false);
    if(g_avV29Original3590D0) g_avV29Original3590D0(component,arg);
    AV_V210LogStage("3590D0",component,true);
}
static void __fastcall AV_V29Hooked359110(void* component)
{
    AV_V210LogStage("359110",component,false);
    if(g_avV29Original359110) g_avV29Original359110(component);
    AV_V210LogStage("359110",component,true);
}
// do not write it yet. Dump raw floats using several plausible per-entry strides so
// we can determine whether this is BoneAtom-like data, matrices, indices, or some
// other render-side per-bone structure.
static void AV_V214DumpCandidate720(const char* stage, void* component)
{
    if(!AV_V210CaptureActive() || !FP_V111PlausiblePtr((uintptr_t)component)) return;
    const uintptr_t comp=(uintptr_t)component;
    if(comp!=g_avV27TargetComponent) return;

    AV_V27TArray64 a{};
    if(!FP_ReadMemory((const void*)(comp+0x720),&a,sizeof(a))) return;
    if(a.Count!=70 || a.Max<70 || a.Max>512 || !FP_V111PlausiblePtr(a.Data)) return;

    static thread_local uint64_t lastDumpPass=0;
    if(g_avV210TlsPassId-lastDumpPass < 30) return;
    lastDumpPass=g_avV210TlsPassId;

    char hdr[512]{};
    sprintf_s(hdr,sizeof(hdr),
        "AV_V214_720_BEGIN pass=%llu stage=%s component=%p data=%p count=%d max=%d\n",
        (unsigned long long)g_avV210TlsPassId,stage,component,(void*)a.Data,a.Count,a.Max);
    FP_Log(hdr);

    const int bones[]={0,1,18,19,20,32,33,34,60,66};
    const int strides[]={0x10,0x20,0x30,0x40};
    for(int si=0;si<4;++si){
        const int stride=strides[si];
        for(int bi=0;bi<10;++bi){
            const int bone=bones[bi];
            float f[16]{};
            const uintptr_t addr=a.Data+(uintptr_t)bone*(uintptr_t)stride;
            if(!FP_ReadMemory((const void*)addr,f,sizeof(f))) continue;
            char line[1200]{};
            sprintf_s(line,sizeof(line),
                "AV_V214_720_RAW pass=%llu stride=0x%X bone=%d addr=%p "
                "f=[%.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f]\n",
                (unsigned long long)g_avV210TlsPassId,stride,bone,(void*)addr,
                f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],
                f[8],f[9],f[10],f[11],f[12],f[13],f[14],f[15]);
            FP_Log(line);
        }
    }
    FP_Log("AV_V214_720_END\n");
}

static void __fastcall AV_V29Hooked358780(void* component)
{
    AV_V210LogStage("358780",component,false);
    AV_V214DumpCandidate720("358780_PRE",component);
    if(g_avV29Original358780) g_avV29Original358780(component);
    AV_V214DumpCandidate720("358780_POST",component);
    AV_V210LogStage("358780",component,true);
}
static void __fastcall AV_V29HookedVFunc290(void* component)
{
    AV_V210LogStage("VFUNC_290",component,false);
    AV_V214DumpCandidate720("VFUNC290_PRE",component);
    if(g_avV29OriginalVFunc290) g_avV29OriginalVFunc290(component);
    AV_V214DumpCandidate720("VFUNC290_POST",component);
    AV_V210LogStage("VFUNC_290",component,true);
}

