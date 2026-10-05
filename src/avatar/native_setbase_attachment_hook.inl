//
// Reverse-engineered from HatinTimeGame.exe:
//   AActor::SetBase = RVA 0x37DF10
//   RCX = this actor
//   RDX = NewBase actor
//   R8  = pointer to FVector NewFloor (12-byte aggregate)
//   R9D = bNotifyActor
//   [entry RSP+0x28] = USkeletalMeshComponent* SkelComp
//   [entry RSP+0x30] = FName AttachName (8 bytes)
//
// the function itself stores:
//   Actor+0xE0  = Base
//   Actor+0x1D8 = BaseSkelComponent
//   Actor+0x1E0 = BaseBoneName
//
// for a slave component it resolves AttachName with RVA 0x69E5D0.  HatVR can
// therefore redirect the attachment at creation time to ParentAnimComponent,
// preserving the SAME FName when that bone exists on the master.  This is the
// architectural equivalent of attaching directly to the authoritative pose.
using AV_V90SetBaseFn = void (__fastcall*)(
    uintptr_t actor,
    uintptr_t newBase,
    const void* newFloor,
    int bNotifyActor,
    uintptr_t skelComp,
    uint64_t attachName);

static AV_V90SetBaseFn g_avV90OriginalSetBase=nullptr;
static bool g_avV90SetBaseInstalled=false;
static uint64_t g_avV90Calls=0;
static uint64_t g_avV90Redirects=0;

static void __fastcall AV_V90HookedSetBase(
    uintptr_t actor,
    uintptr_t newBase,
    const void* newFloor,
    int bNotifyActor,
    uintptr_t skelComp,
    uint64_t attachName)
{
    ++g_avV90Calls;

    uintptr_t finalComp=skelComp;
    bool redirected=false;
    int childBone=-1, masterBone=-1, mapped=-1;
    uintptr_t master=0;
    int mapCount=0;
    uintptr_t mapData=0;

    if(g_fpV1Enabled && g_avV29ProbeEnabled &&
       FP_V111PlausiblePtr(actor) && FP_V111PlausiblePtr(skelComp) &&
       g_avV88GetBoneIndex)
    {
        __try { childBone=g_avV88GetBoneIndex(skelComp,&attachName); }
        __except(EXCEPTION_EXECUTE_HANDLER) { childBone=-1; }

        FP_ReadMemory((const void*)(skelComp+0x37C),&master,sizeof(master));
        FP_ReadMemory((const void*)(skelComp+0x384),&mapData,sizeof(mapData));
        FP_ReadMemory((const void*)(skelComp+0x38C),&mapCount,sizeof(mapCount));

        if(childBone>=0 && childBone<512 &&
           FP_V111PlausiblePtr(master) &&
           FP_V111PlausiblePtr(mapData) &&
           mapCount>0 && mapCount<=512 && childBone<mapCount)
        {
            FP_ReadMemory((const void*)(mapData+(uintptr_t)childBone*sizeof(int32_t)),&mapped,sizeof(mapped));

            // keep this scoped to the current player/master pose rather than
            // rewriting arbitrary UE3 slave-component attachments elsewhere.
            // g_avV27TargetComponent is the authoritative current player
            // skeletal master in this source lineage.  The later publication
            // caches do not expose separate component globals here.
            const bool currentMaster =
                (master==g_avV27TargetComponent);

            if(currentMaster && mapped>=0 && mapped<512)
            {
                __try { masterBone=g_avV88GetBoneIndex(master,&attachName); }
                __except(EXCEPTION_EXECUTE_HANDLER) { masterBone=-1; }

                // ParentBoneMap tells us which master bone drives this slave bone.
                // Requiring the same FName to resolve to that mapped index makes
                // this generic while avoiding a blind index/name substitution.
                if(masterBone==mapped)
                {
                    finalComp=master;
                    redirected=true;
                    ++g_avV90Redirects;
                }
            }
        }

        static ULONGLONG s_last=0;
        static uintptr_t s_lastActor=0,s_lastComp=0;
        static uint64_t s_lastName=~0ull;
        const ULONGLONG now=GetTickCount64();
        if(actor!=s_lastActor || skelComp!=s_lastComp || attachName!=s_lastName || now-s_last>=500)
        {
            s_last=now; s_lastActor=actor; s_lastComp=skelComp; s_lastName=attachName;
            char an[128]="?",cn[128]="?",mn[128]="?",bn[128]="?",attachN[128]="?";
            AV_V1ReadRawObjectName(actor,an,sizeof(an));
            AV_V1ReadRawObjectName(skelComp,cn,sizeof(cn));
            if(FP_V111PlausiblePtr(master)) AV_V1ReadRawObjectName(master,mn,sizeof(mn));
            if(FP_V111PlausiblePtr(newBase)) AV_V1ReadRawObjectName(newBase,bn,sizeof(bn));
            const int32_t attachNameIndex=(int32_t)(attachName & 0xffffffffu);
            if(attachNameIndex>=0)
                FP_V125RawNameFromIndex(attachNameIndex,attachN,sizeof(attachN));
            char l[896]{};
            sprintf_s(l,sizeof(l),
                "AV_V95_SETBASE actor=%p name=\"%s\" newBase=%p baseName=\"%s\" skelComp=%p comp=\"%s\" attach=\"%s\" attachFName=0x%016llX childBone=%d parentAnim=%p parent=\"%s\" mapped=%d masterBoneSameName=%d redirect=%d finalComp=%p calls=%llu redirects=%llu\n",
                (void*)actor,an,(void*)newBase,bn,(void*)skelComp,cn,attachN,
                (unsigned long long)attachName,childBone,(void*)master,mn,mapped,masterBone,
                redirected?1:0,(void*)finalComp,
                (unsigned long long)g_avV90Calls,(unsigned long long)g_avV90Redirects);
            FP_Log(l);
        }
    }

    if(g_avV90OriginalSetBase)
        g_avV90OriginalSetBase(actor,newBase,newFloor,bNotifyActor,finalComp,attachName);
}

static bool AV_V90InstallSetBaseHook()
{
    if(g_avV90SetBaseInstalled) return true;
    const uintptr_t exe=(uintptr_t)GetModuleHandleW(nullptr);
    if(!exe) return false;

    constexpr uintptr_t kSetBaseRva=0x37DF10;
    constexpr uintptr_t kGetBoneIndexRva=0x69E5D0;
    if(!g_avV88GetBoneIndex)
        g_avV88GetBoneIndex=(AV_V88GetBoneIndexFn)(exe+kGetBoneIndexRva);

    void* target=(void*)(exe+kSetBaseRva);
    MH_STATUS st=MH_CreateHook(target,&AV_V90HookedSetBase,
        reinterpret_cast<void**>(&g_avV90OriginalSetBase));
    if(st!=MH_OK && st!=MH_ERROR_ALREADY_CREATED){
        char l[256]{};
        sprintf_s(l,sizeof(l),"AV_V90_SETBASE_HOOK_CREATE_FAIL target=%p status=%d\n",target,(int)st);
        FP_Log(l); return false;
    }
    st=MH_EnableHook(target);
    if(st!=MH_OK && st!=MH_ERROR_ENABLED){
        char l[256]{};
        sprintf_s(l,sizeof(l),"AV_V90_SETBASE_HOOK_ENABLE_FAIL target=%p status=%d\n",target,(int)st);
        FP_Log(l); return false;
    }
    g_avV90SetBaseInstalled=true;
    char l[384]{};
    sprintf_s(l,sizeof(l),
        "AV_V90_SETBASE_HOOK_INSTALLED target=%p rva=0x%llX -- native AActor::SetBase; mapped player slave attachments redirect to authoritative ParentAnimComponent\n",
        target,(unsigned long long)kSetBaseRva);
    FP_Log(l);
    return true;
}

static bool AV_V215InstallBuildRefToLocalHook()
{
    if(g_avV215HookInstalled) return true;
    const uintptr_t exe=(uintptr_t)GetModuleHandleW(nullptr);
    if(!exe) return false;
    constexpr uintptr_t kRva=0x6CE6E0;
    void* target=(void*)(exe+kRva);
    MH_STATUS st=MH_CreateHook(target,&AV_V215HookedBuildRefToLocal,
        reinterpret_cast<void**>(&g_avV215OriginalBuildRefToLocal));
    if(st!=MH_OK && st!=MH_ERROR_ALREADY_CREATED){
        char l[256]{}; sprintf_s(l,sizeof(l),"AV_V215_RENDER_HOOK_CREATE_FAIL target=%p status=%d\n",target,(int)st); FP_Log(l); return false;
    }
    st=MH_EnableHook(target);
    if(st!=MH_OK && st!=MH_ERROR_ENABLED){
        char l[256]{}; sprintf_s(l,sizeof(l),"AV_V215_RENDER_HOOK_ENABLE_FAIL target=%p status=%d\n",target,(int)st); FP_Log(l); return false;
    }
    g_avV215HookInstalled=true;
    char l[384]{}; sprintf_s(l,sizeof(l),
        "AV_V240_RENDER_HOOK_INSTALLED target=%p rva=0x%llX original=%p -- F5 applies component-space right-arm IK before ReferenceToLocal build\n",
        target,(unsigned long long)kRva,(void*)g_avV215OriginalBuildRefToLocal); FP_Log(l);
    return true;
}

static bool AV_V40InstallComposeHook()
{
    if(g_avV40ComposeHookInstalled)return true;

    const uintptr_t exe=(uintptr_t)GetModuleHandleW(nullptr);
    if(!exe)return false;

    constexpr uintptr_t kComposeRva=0x698B40;
    void* target=(void*)(exe+kComposeRva);

    MH_STATUS st=MH_CreateHook(
        target,
        &AV_V40HookedCompose,
        reinterpret_cast<void**>(&g_avV40OriginalCompose));
    if(st!=MH_OK && st!=MH_ERROR_ALREADY_CREATED){
        char line[320]{};
        sprintf_s(line,sizeof(line),
            "AV_V40_COMPOSE_HOOK_CREATE_FAIL target=%p rva=0x%llX status=%d\n",
            target,(unsigned long long)kComposeRva,(int)st);
        FP_Log(line);
        return false;
    }

    st=MH_EnableHook(target);
    if(st!=MH_OK && st!=MH_ERROR_ENABLED){
        char line[256]{};
        sprintf_s(line,sizeof(line),
            "AV_V40_COMPOSE_HOOK_ENABLE_FAIL target=%p status=%d\n",
            target,(int)st);
        FP_Log(line);
        return false;
    }

    g_avV40ComposeHookInstalled=true;
    char line[420]{};
    sprintf_s(line,sizeof(line),
        "AV_V40_COMPOSE_HOOK_INSTALLED target=%p rva=0x%llX original=%p "
        "-- v120 actual LocalAtoms->SpaceBases composition; root inheritance test\n",
        target,(unsigned long long)kComposeRva,(void*)g_avV40OriginalCompose);
    FP_Log(line);
    return true;
}

static bool AV_V27InstallPoseEvalHook()
{
    if(g_avV27HookInstalled)return true;

    const uintptr_t exe=(uintptr_t)GetModuleHandleW(nullptr);
    if(!exe)return false;

    constexpr uintptr_t kPoseEvalRva=0x6A5180;
    void* target=(void*)(exe+kPoseEvalRva);

    MH_STATUS st=MH_CreateHook(
        target,
        &AV_V27HookedPoseEval,
        reinterpret_cast<void**>(&g_avV27OriginalPoseEval));

    if(st!=MH_OK && st!=MH_ERROR_ALREADY_CREATED){
        char line[256]{};
        sprintf_s(line,sizeof(line),
            "AV_V27_HOOK_CREATE_FAIL target=%p rva=0x%llX status=%d\n",
            target,(unsigned long long)kPoseEvalRva,(int)st);
        FP_Log(line);
        return false;
    }

    st=MH_EnableHook(target);
    if(st!=MH_OK && st!=MH_ERROR_ENABLED){
        char line[256]{};
        sprintf_s(line,sizeof(line),
            "AV_V27_HOOK_ENABLE_FAIL target=%p status=%d\n",
            target,(int)st);
        FP_Log(line);
        return false;
    }

    g_avV27HookInstalled=true;
    char line[384]{};
    sprintf_s(line,sizeof(line),
        "AV_V39_NATIVE_UPDATE_POSE_HOOK target=%p rva=0x%llX original=%p -- diagnostic LocalAtoms/SpaceBases trace enabled\n",
        target,(unsigned long long)kPoseEvalRva,(void*)g_avV27OriginalPoseEval);
    FP_Log(line);
    return true;
}

static bool AV_V29InstallPostPoseHook()
{
    if(g_avV29HookInstalled)return true;
    const uintptr_t exe=(uintptr_t)GetModuleHandleW(nullptr); if(!exe)return false;
    void* target=(void*)(exe+0x6A4980);
    MH_STATUS st=MH_CreateHook(target,&AV_V29HookedPostPose,reinterpret_cast<void**>(&g_avV29OriginalPostPose));
    if(st!=MH_OK && st!=MH_ERROR_ALREADY_CREATED){ FP_Log("AV_V29_4B90_CREATE_FAIL\n"); return false; }
    st=MH_EnableHook(target);
    if(st!=MH_OK && st!=MH_ERROR_ENABLED){ FP_Log("AV_V29_4B90_ENABLE_FAIL\n"); return false; }
    g_avV29HookInstalled=true; FP_Log("AV_V240_4B90_HOOK_INSTALLED rva=0x6A4980 -- render-time SpaceBases arm IK is performed only inside 0x6CE6E0 hook\n"); return true;
}

static bool AV_V29InstallBranchHooks()
{
    if(g_avV29BranchHooksInstalled)return true;
    const uintptr_t exe=(uintptr_t)GetModuleHandleW(nullptr); if(!exe)return false;
    struct H { uintptr_t rva; void* detour; void** original; const char* name; };
    H hs[]={
        {0x35A3B0,(void*)&AV_V29Hooked3590D0,(void**)&g_avV29Original3590D0,"3590D0"},
        {0x35A3F0,(void*)&AV_V29Hooked359110,(void**)&g_avV29Original359110,"359110"},
        {0x359A60,(void*)&AV_V29Hooked358780,(void**)&g_avV29Original358780,"358780"}
    };
    for(auto& h:hs){
        void* target=(void*)(exe+h.rva);
        MH_STATUS st=MH_CreateHook(target,h.detour,h.original);
        if(st!=MH_OK && st!=MH_ERROR_ALREADY_CREATED){ char l[192]{}; sprintf_s(l,sizeof(l),"AV_V29_BRANCH_CREATE_FAIL %s status=%d\n",h.name,(int)st); FP_Log(l); return false; }
        st=MH_EnableHook(target);
        if(st!=MH_OK && st!=MH_ERROR_ENABLED){ char l[192]{}; sprintf_s(l,sizeof(l),"AV_V29_BRANCH_ENABLE_FAIL %s status=%d\n",h.name,(int)st); FP_Log(l); return false; }
    }
    g_avV29BranchHooksInstalled=true; FP_Log("AV_V29_BRANCH_HOOKS_INSTALLED rvas=0x35A3B0,0x35A3F0,0x359A60\n"); return true;
}

static void AV_V29TryInstallVFunc290(uintptr_t comp)
{
    if(g_avV29VFuncHookInstalled || !FP_V111PlausiblePtr(comp))return;
    uintptr_t vt=0,target=0;
    if(!FP_V18ReadPtr(comp,vt) || !FP_V111PlausiblePtr(vt) || !FP_V18ReadPtr(vt+0x290,target) || !FP_V111PlausiblePtr(target))return;
    MH_STATUS st=MH_CreateHook((void*)target,&AV_V29HookedVFunc290,reinterpret_cast<void**>(&g_avV29OriginalVFunc290));
    if(st!=MH_OK && st!=MH_ERROR_ALREADY_CREATED){ char l[256]{}; sprintf_s(l,sizeof(l),"AV_V29_VFUNC290_CREATE_FAIL target=%p status=%d\n",(void*)target,(int)st); FP_Log(l); return; }
    st=MH_EnableHook((void*)target);
    if(st!=MH_OK && st!=MH_ERROR_ENABLED){ char l[256]{}; sprintf_s(l,sizeof(l),"AV_V29_VFUNC290_ENABLE_FAIL target=%p status=%d\n",(void*)target,(int)st); FP_Log(l); return; }
    g_avV29VFunc290Target=target; g_avV29VFuncHookInstalled=true;
    char l[320]{}; sprintf_s(l,sizeof(l),"AV_V29_VFUNC290_HOOK_INSTALLED target=%p rva=0x%llX\n",(void*)target,(unsigned long long)AV_V27ToExeRva(target)); FP_Log(l);
}

static bool AV_V240ForceRenderRefresh(uintptr_t comp,uint32_t& before,uint32_t& after)
{
    before=after=0;
    if(!AV_V1ResolvePropertyOffset(
            comp,"bForceMeshObjectUpdate",g_avV240ForceMeshObjectUpdateOffset))
        return false;

    const uintptr_t p=comp+(uintptr_t)g_avV240ForceMeshObjectUpdateOffset;
    if(!FP_ReadMemory((const void*)p,&before,sizeof(before))) return false;
    after=before|1u;
    __try{
        *(volatile uint32_t*)p=after;
    }__except(EXCEPTION_EXECUTE_HANDLER){
        return false;
    }
    return true;
}

static void AV_V29UpdateTarget(uintptr_t pawn)
{
    // PoseEval/Compose are old reverse-engineering hooks; do not install them.
    AV_V215InstallBuildRefToLocalHook();
    AV_V40InstallComposeHook();
    uintptr_t comp=0;
    const uintptr_t previousTarget=g_avV27TargetComponent;
    if(FP_V111PlausiblePtr(pawn) && FP_V18ReadPtr(pawn+0x530,comp) && FP_V111PlausiblePtr(comp)) g_avV27TargetComponent=comp;
    else g_avV27TargetComponent=0;

    // do not depend on the old Hat Kid -> Bow Kid -> Hat Kid workaround to clear
    // stale hand/socket state.
    if(previousTarget!=g_avV27TargetComponent){
        g_hatVrAnimHeadCameraForwardUU=0.0f;
        g_hatVrAnimHeadCameraUpUU=0.0f;
        g_hatVrAnimHeadCameraOffsetValid=false;
        g_avV84CacheValid=false;
        g_avV84CacheOwner=0;
        g_avV104PublishedRoomscaleValid=false;
        g_avV84CacheCount=0;
        g_avV97PublishedValid=false;
        g_avV97PublishedOwner=0;
        g_avV97PublishedCount=0;
        g_avV37PublishedOwner=0;
        g_avV37PublishedMesh=0;
        g_avV37PublishedCount=0;
        char rl[256]{};
        sprintf_s(rl,sizeof(rl),
            "AV_V37_MASTER_RESET old=%p new=%p -- publication generation invalidated\\n",
            (void*)previousTarget,(void*)g_avV27TargetComponent);
        FP_Log(rl);
    }
}

static void AV_OnPawnDiscovered(uintptr_t pawn)
{
    if (!FP_V111PlausiblePtr(pawn))
        return;

    g_avV1LastPawn = pawn;

    AV_V29UpdateTarget(pawn);
    AV_V248UpdateHatVisibility(pawn);

    //   0 FULL              -> publish + force refresh
    //   1 PUBLISH_NO_FORCE  -> publish, no force refresh
    //   2 NO_LIVE_PUBLISH   -> render-hook solve only
    //   3 NATIVE_BUILD_ONLY -> no HatVR skeletal writes
    // do not run a second standard-only publication pass.
    (void)pawn;
}
