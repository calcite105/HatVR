//
// pointer/ParentAnim checks.  The expensive named-rig/BoneMap inspection runs
// once only for HatVR's selected pose component and components directly connected
// to it through ParentAnimComponent.
//
// to reveal the pipeline used by mixed Hat Kid / HCR / Bow Kid without turning
static void AV_V34TracePlayerFamilyPoseOwnership(uintptr_t comp)
{
    if(!g_fpV1Enabled || !FP_V111PlausiblePtr(comp) ||
       !FP_V111PlausiblePtr(g_avV27TargetComponent))
        return;

    const uintptr_t target = g_avV27TargetComponent;
    const uintptr_t parent = AV_V211ReadParentAnim(comp);
    const uintptr_t targetParent = AV_V211ReadParentAnim(target);

    // Cheap gate first.  No name/ref-skeleton work happens for unrelated meshes.
    const bool selected = (comp == target);
    const bool childOfSelected = (parent == target);
    const bool selectedChildOfThis = (targetParent == comp);
    const bool sharesSelectedParent =
        FP_V111PlausiblePtr(targetParent) && parent == targetParent;

    if(!(selected || childOfSelected || selectedChildOfThis || sharesSelectedParent))
        return;

    struct Seen {
        uintptr_t comp, parent, target, targetParent, mesh;
    };
    static Seen seen[48]{};
    static int seenCount = 0;

    uintptr_t mesh=0;
    if(g_avV1SkeletalMeshOffset>=0)
        FP_V18ReadPtr(comp+(uintptr_t)g_avV1SkeletalMeshOffset,mesh);

    for(int i=0;i<seenCount;++i){
        if(seen[i].comp==comp && seen[i].parent==parent &&
           seen[i].target==target && seen[i].targetParent==targetParent &&
           seen[i].mesh==mesh)
            return;
    }
    if(seenCount<48) seen[seenCount++]={comp,parent,target,targetParent,mesh};

    AV_V16MasterRig childRig{}, parentRig{}, targetRig{};
    const bool childRigOk = AV_V16ResolveMasterRig(comp,childRig);
    const bool parentRigOk = FP_V111PlausiblePtr(parent) &&
                             AV_V16ResolveMasterRig(parent,parentRig);
    const bool targetRigOk = AV_V16ResolveMasterRig(target,targetRig);

    char compName[128]={"?"}, meshName[128]={"?"};
    char parentName[128]={"?"}, parentMeshName[128]={"?"};
    char targetName[128]={"?"}, targetMeshName[128]={"?"};
    AV_V1ReadRawObjectName(comp,compName,sizeof(compName));
    if(FP_V111PlausiblePtr(mesh))
        AV_V1ReadRawObjectName(mesh,meshName,sizeof(meshName));

    uintptr_t parentMesh=0, targetMesh=0;
    if(FP_V111PlausiblePtr(parent)){
        AV_V1ReadRawObjectName(parent,parentName,sizeof(parentName));
        if(g_avV1SkeletalMeshOffset>=0 &&
           FP_V18ReadPtr(parent+(uintptr_t)g_avV1SkeletalMeshOffset,parentMesh) &&
           FP_V111PlausiblePtr(parentMesh))
            AV_V1ReadRawObjectName(parentMesh,parentMeshName,sizeof(parentMeshName));
    }
    AV_V1ReadRawObjectName(target,targetName,sizeof(targetName));
    if(g_avV1SkeletalMeshOffset>=0 &&
       FP_V18ReadPtr(target+(uintptr_t)g_avV1SkeletalMeshOffset,targetMesh) &&
       FP_V111PlausiblePtr(targetMesh))
        AV_V1ReadRawObjectName(targetMesh,targetMeshName,sizeof(targetMeshName));

    AV_V27TArray64 boneMap{};
    bool boneMapOk=false;
    if(FP_V111PlausiblePtr(parent) &&
       FP_ReadMemory((const void*)(comp+0x384),&boneMap,sizeof(boneMap)) &&
       FP_V111PlausiblePtr(boneMap.Data) &&
       boneMap.Count>0 && boneMap.Count<=512 &&
       boneMap.Max>=boneMap.Count && boneMap.Max<=1024)
        boneMapOk=true;

    auto mapBone=[&](int childBone)->int {
        if(!boneMapOk || childBone<0 || childBone>=boneMap.Count) return -999;
        int32_t p=-999;
        if(!FP_ReadMemory((const void*)(boneMap.Data+(uintptr_t)childBone*sizeof(int32_t)),
                          &p,sizeof(p))) return -999;
        return (int)p;
    };

    const int mLU=childRigOk?mapBone(childRig.lUpper):-999;
    const int mLF=childRigOk?mapBone(childRig.lFore):-999;
    const int mLH=childRigOk?mapBone(childRig.lHand):-999;
    const int mRU=childRigOk?mapBone(childRig.rUpper):-999;
    const int mRF=childRigOk?mapBone(childRig.rFore):-999;
    const int mRH=childRigOk?mapBone(childRig.rHand):-999;

    char line[1700]{};
    sprintf_s(line,sizeof(line),
        "AV_V34_PLAYER_FAMILY comp=%p name=%s mesh=%s selected=%d childOfSelected=%d "
        "selectedChildOfThis=%d sharesSelectedParent=%d parent=%p parentName=%s parentMesh=%s "
        "target=%p targetName=%s targetMesh=%s targetParent=%p "
        "childRig=%d bones=%d L=[%d %d %d] R=[%d %d %d] "
        "boneMap=%d mapCount=%d mappedL=[%d %d %d] mappedR=[%d %d %d] "
        "parentRig=%d parentBones=%d parentL=[%d %d %d] parentR=[%d %d %d] "
        "targetRig=%d targetBones=%d targetL=[%d %d %d] targetR=[%d %d %d]\n",
        (void*)comp,compName,meshName,selected?1:0,childOfSelected?1:0,
        selectedChildOfThis?1:0,sharesSelectedParent?1:0,
        (void*)parent,parentName,parentMeshName,
        (void*)target,targetName,targetMeshName,(void*)targetParent,
        childRigOk?1:0,childRigOk?childRig.count:0,
        childRigOk?childRig.lUpper:-1,childRigOk?childRig.lFore:-1,childRigOk?childRig.lHand:-1,
        childRigOk?childRig.rUpper:-1,childRigOk?childRig.rFore:-1,childRigOk?childRig.rHand:-1,
        boneMapOk?1:0,boneMapOk?boneMap.Count:0,mLU,mLF,mLH,mRU,mRF,mRH,
        parentRigOk?1:0,parentRigOk?parentRig.count:0,
        parentRigOk?parentRig.lUpper:-1,parentRigOk?parentRig.lFore:-1,parentRigOk?parentRig.lHand:-1,
        parentRigOk?parentRig.rUpper:-1,parentRigOk?parentRig.rFore:-1,parentRigOk?parentRig.rHand:-1,
        targetRigOk?1:0,targetRigOk?targetRig.count:0,
        targetRigOk?targetRig.lUpper:-1,targetRigOk?targetRig.lFore:-1,targetRigOk?targetRig.lHand:-1,
        targetRigOk?targetRig.rUpper:-1,targetRigOk?targetRig.rFore:-1,targetRigOk?targetRig.rHand:-1);
    FP_Log(line);

    if(childRigOk && parentRigOk && boneMapOk){
        const bool exact =
            mLU==parentRig.lUpper && mLF==parentRig.lFore && mLH==parentRig.lHand &&
            mRU==parentRig.rUpper && mRF==parentRig.rFore && mRH==parentRig.rHand;
        char verdict[640]{};
        sprintf_s(verdict,sizeof(verdict),
            "AV_V34_BONEMAP_MATCH child=%p parent=%p exactNamedArmMap=%d "
            "L=[%d->%d/%d %d->%d/%d %d->%d/%d] "
            "R=[%d->%d/%d %d->%d/%d %d->%d/%d]\n",
            (void*)comp,(void*)parent,exact?1:0,
            childRig.lUpper,mLU,parentRig.lUpper,
            childRig.lFore,mLF,parentRig.lFore,
            childRig.lHand,mLH,parentRig.lHand,
            childRig.rUpper,mRU,parentRig.rUpper,
            childRig.rFore,mRF,parentRig.rFore,
            childRig.rHand,mRH,parentRig.rHand);
        FP_Log(verdict);
    }
}

// AV_V16ResolveMasterRig, AV_V260ReadParentTable, AV_V259IsDescendantOf and
// AV_V16ReadNonstandardRefPose because avatar_system.inl is a single C++ TU.
static void AV_V39TraceNativePose(
    uintptr_t comp,void* component,uintptr_t callerRva,
    const AV_V27Atom48* v39PreSb,int v39PreSbCount,
    const AV_V27Atom48* v39PreLocal,int v39PreLocalCount)
{
    AV_V27TArray64 v39PostSb{},v39PostLocal{};
    const bool v39HavePostSb=AV_V27GetSpaceBases(comp,v39PostSb);
    const bool v39HavePostLocal=AV_V39GetLocalAtoms(comp,v39PostLocal);
    const ULONGLONG v39Now=GetTickCount64();
    AV_V16MasterRig v39Rig{};
    const bool v39RigOk=AV_V16ResolveMasterRig(comp,v39Rig);
    if(v39RigOk && v39Now-g_avV39LastNativePoseTrace>=250){
        g_avV39LastNativePoseTrace=v39Now;
        const int ids[6]={v39Rig.lUpper,v39Rig.lFore,v39Rig.lHand,
                          v39Rig.rUpper,v39Rig.rFore,v39Rig.rHand};
        char line[1800]{};
        int n=sprintf_s(line,sizeof(line),
            "AV_V39_NATIVE_UPDATE_POSE comp=%p mesh=%p bones=%d callerRva=0x%llX Local[%d->%d] Space[%d->%d] arms=",
            component,(void*)v39Rig.mesh,v39Rig.count,(unsigned long long)callerRva,
            v39PreLocalCount,v39HavePostLocal?v39PostLocal.Count:0,
            v39PreSbCount,v39HavePostSb?v39PostSb.Count:0);
        for(int i=0;i<6 && n>0 && n<(int)sizeof(line)-180;++i){
            const int b=ids[i]; AV_V27Atom48 pl{},ps{};
            const bool hl=b>=0&&b<v39PreLocalCount&&v39HavePostLocal&&AV_V39ReadAtomSafe(v39PostLocal,b,pl);
            const bool hs=b>=0&&b<v39PreSbCount&&v39HavePostSb&&AV_V39ReadAtomSafe(v39PostSb,b,ps);
            n+=sprintf_s(line+n,sizeof(line)-n,"%s%d:L(p%.3f/q%.5f) S(p%.3f/q%.5f)",
                i?",":"",b,
                hl?AV_V39PosDelta(v39PreLocal[b],pl):-1.0f,hl?AV_V39QDotAbs(v39PreLocal[b],pl):-1.0f,
                hs?AV_V39PosDelta(v39PreSb[b],ps):-1.0f,hs?AV_V39QDotAbs(v39PreSb[b],ps):-1.0f);
        }
        if(n>0&&n<(int)sizeof(line)-3){line[n++]='\n';line[n]=0;} FP_Log(line);
    
        int parents[128]{}; AV_V27Atom48 ref[128]{};
        bool hp=AV_V16ReadNonstandardRefPose(comp,v39Rig,parents,ref);
        if(!hp && v39Rig.standard70) hp=AV_V260ReadParentTable(comp,parents,70);
        if(hp){
            char dl[1800]{}; int dn=sprintf_s(dl,sizeof(dl),
                "AV_V39_NATIVE_DESC comp=%p mesh=%p hand=%d descendants=",
                component,(void*)v39Rig.mesh,v39Rig.rHand);
            int emitted=0;
            for(int b=0;b<v39Rig.count&&emitted<16&&dn>0&&dn<(int)sizeof(dl)-180;++b){
                if(!AV_V259IsDescendantOf(b,v39Rig.rHand,parents,v39Rig.count)) continue;
                AV_V27Atom48 pl{},ps{};
                const bool hl=b<v39PreLocalCount&&v39HavePostLocal&&AV_V39ReadAtomSafe(v39PostLocal,b,pl);
                const bool hs=b<v39PreSbCount&&v39HavePostSb&&AV_V39ReadAtomSafe(v39PostSb,b,ps);
                dn+=sprintf_s(dl+dn,sizeof(dl)-dn,"%s%d:L%.3f/%.5f S%.3f/%.5f",
                    emitted?",":"",b,
                    hl?AV_V39PosDelta(v39PreLocal[b],pl):-1.0f,hl?AV_V39QDotAbs(v39PreLocal[b],pl):-1.0f,
                    hs?AV_V39PosDelta(v39PreSb[b],ps):-1.0f,hs?AV_V39QDotAbs(v39PreSb[b],ps):-1.0f);
                ++emitted;
            }
            if(dn>0&&dn<(int)sizeof(dl)-3){dl[dn++]='\n';dl[dn]=0;} FP_Log(dl);
        }
    }
}

static void __fastcall AV_V43HookedSkelControlBoundary(
    void* component,int boneIndex,int arg3,int arg4,
    uintptr_t arg5,int arg6,void* arg7)
{
    // BEFORE 0x693810 returns control to 0x698B40.  At this instant UE3 has just
    // composed this required bone into SpaceBases, but has not advanced to later
    // required bones yet.  Descendants therefore see our VR parent naturally.

    const uintptr_t comp=(uintptr_t)component;
    const bool ours=g_fpV1Enabled && comp &&
                    comp==g_avV27TargetComponent &&
                    FP_V111PlausiblePtr(comp);

    AV_V27TArray64 sb{};
    AV_V27Atom48 before{};
    bool haveBefore=false;
    if(ours && AV_V27GetSpaceBases(comp,sb) &&
       boneIndex>=0 && boneIndex<sb.Count)
        haveBefore=AV_V39ReadAtomSafe(sb,boneIndex,before);

    // Bow/HCR now use the same render-hook VR-arm strategy as standard Hat Kid:
    // solve from RefSkeleton, then rigidly carry the LIVE animated wrist subtree.

    if(g_avV43OriginalSkelControlBoundary)
        g_avV43OriginalSkelControlBoundary(
            component,boneIndex,arg3,arg4,arg5,arg6,arg7);

    if(!ours) return;

    AV_V16MasterRig rig{};
    if(!AV_V16ResolveMasterRig(comp,rig)) return;

    const bool armBone=
        boneIndex==rig.lUpper || boneIndex==rig.lFore || boneIndex==rig.lHand ||
        boneIndex==rig.rUpper || boneIndex==rig.rFore || boneIndex==rig.rHand;

    int parents[128]{};
    AV_V27Atom48 ref[128]{};
    bool hp=AV_V16ReadNonstandardRefPose(comp,rig,parents,ref);
    if(!hp && rig.standard70) hp=AV_V260ReadParentTable(comp,parents,70);
    const bool rightDesc=hp && boneIndex>=0 && boneIndex<rig.count &&
                         AV_V259IsDescendantOf(boneIndex,rig.rHand,parents,rig.count);

    if(!armBone && !rightDesc) return;

    AV_V27TArray64 postSb{};
    AV_V27Atom48 after{};
    const bool haveAfter=AV_V27GetSpaceBases(comp,postSb) &&
                         boneIndex>=0 && boneIndex<postSb.Count &&
                         AV_V39ReadAtomSafe(postSb,boneIndex,after);

    const ULONGLONG now=GetTickCount64();
    if(!armBone && now-g_avV43LastBoundaryTrace<180) return;
    if(armBone && boneIndex==rig.rHand) g_avV43LastBoundaryTrace=now;

    char line[1050]{};
    sprintf_s(line,sizeof(line),
        "AV_V43_SKELCONTROL_BOUNDARY comp=%p mesh=%p bone=%d arms=[%d,%d,%d|%d,%d,%d] "
        "args=[%d,%d,0x%llX,%d,%p] callerRva=0x%llX "
        "Space pre=%d post=%d delta[p=%.4f q=%.6f] "
        "preP=[%+.3f,%+.3f,%+.3f] postP=[%+.3f,%+.3f,%+.3f]\n",
        component,(void*)rig.mesh,boneIndex,
        rig.lUpper,rig.lFore,rig.lHand,rig.rUpper,rig.rFore,rig.rHand,
        arg3,arg4,(unsigned long long)arg5,arg6,arg7,
        (unsigned long long)AV_V27ToExeRva((uintptr_t)_ReturnAddress()),
        haveBefore?1:0,haveAfter?1:0,
        (haveBefore&&haveAfter)?AV_V39PosDelta(before,after):-1.0f,
        (haveBefore&&haveAfter)?AV_V39QDotAbs(before,after):-1.0f,
        haveBefore?before.translation[0]:-9999.0f,
        haveBefore?before.translation[1]:-9999.0f,
        haveBefore?before.translation[2]:-9999.0f,
        haveAfter?after.translation[0]:-9999.0f,
        haveAfter?after.translation[1]:-9999.0f,
        haveAfter?after.translation[2]:-9999.0f);
    FP_Log(line);
}

static bool AV_V43InstallSkelControlBoundaryHook()
{
    if(g_avV43SkelControlBoundaryInstalled) return true;
    const uintptr_t exe=(uintptr_t)GetModuleHandleW(nullptr);
    if(!exe) return false;
    void* target=(void*)(exe+0x693810ull);

    MH_STATUS st=MH_CreateHook(
        target,&AV_V43HookedSkelControlBoundary,
        reinterpret_cast<void**>(&g_avV43OriginalSkelControlBoundary));
    if(st!=MH_OK && st!=MH_ERROR_ALREADY_CREATED){
        char line[320]{};
        sprintf_s(line,sizeof(line),
            "AV_V43_SKELCONTROL_HOOK_CREATE_FAIL target=%p rva=0x693810 status=%d\n",
            target,(int)st);
        FP_Log(line); return false;
    }
    st=MH_EnableHook(target);
    if(st!=MH_OK && st!=MH_ERROR_ENABLED){
        char line[300]{};
        sprintf_s(line,sizeof(line),
            "AV_V43_SKELCONTROL_HOOK_ENABLE_FAIL target=%p status=%d\n",
            target,(int)st);
        FP_Log(line); return false;
    }
    g_avV43SkelControlBoundaryInstalled=true;
    char line[520]{};
    sprintf_s(line,sizeof(line),
        "AV_V43_SKELCONTROL_HOOK_INSTALLED target=%p rva=0x693810 original=%p "
        "-- 0x698B40 composes LocalAtoms(+0x328)->SpaceBases(+0x318), then calls "
        "this function after each ComposeOrderedRequiredBone; vtable +0x288/+0x290 "
        "dispatch observed inside\n",
        target,(void*)g_avV43OriginalSkelControlBoundary);
    FP_Log(line);
    return true;
}

// 0x6A5180 copies this output into LocalAtoms only after the call returns.
static void AV_V42TraceGetBoneAtomsOutput(
    uintptr_t comp,void* animTree,void* outAtoms,void* requiredBones,
    void* rootTransform,void* arg5,void* arg6,uintptr_t callerRva,
    const AV_V27TArray64& preOut,bool preOutReadable)
{
    const ULONGLONG now=GetTickCount64();
    if(now-g_avV42LastOutputTrace<200) return;

    AV_V16MasterRig rig{};
    if(!AV_V16ResolveMasterRig(comp,rig)) return;

    AV_V27TArray64 out{};
    if(!outAtoms || !FP_ReadMemory(outAtoms,&out,sizeof(out))){
        char bad[512]{};
        sprintf_s(bad,sizeof(bad),
            "AV_V42_GETBONEATOMS_OUTPUT_UNREADABLE comp=%p animTree=%p outAtoms=%p "
            "required=%p root=%p arg5=%p arg6=%p callerRva=0x%llX\n",
            (void*)comp,animTree,outAtoms,requiredBones,rootTransform,arg5,arg6,
            (unsigned long long)callerRva);
        FP_Log(bad);
        g_avV42LastOutputTrace=now;
        return;
    }

    // Be deliberately strict before treating the returned buffer as BoneAtom[Count].
    const bool saneCount=out.Count>0 && out.Count<=256 && out.Max>=out.Count && out.Max<=1024;
    const bool saneData=FP_V111PlausiblePtr(out.Data);
    g_avV42LastOutputTrace=now;

    AV_V27TArray64 local{};
    const bool haveLocal=AV_V39GetLocalAtoms(comp,local);

    char line[2200]{};
    int n=sprintf_s(line,sizeof(line),
        "AV_V42_GETBONEATOMS_OUTPUT comp=%p animTree=%p mesh=%p rigBones=%d callerRva=0x%llX "
        "outArg=%p preOut[data=%p count=%d max=%d readable=%d] "
        "postOut[data=%p count=%d max=%d sane=%d] required=%p root=%p arg5=%p arg6=%p arms=",
        (void*)comp,animTree,(void*)rig.mesh,rig.count,
        (unsigned long long)callerRva,outAtoms,
        (void*)preOut.Data,preOut.Count,preOut.Max,preOutReadable?1:0,
        (void*)out.Data,out.Count,out.Max,(saneCount&&saneData)?1:0,
        requiredBones,rootTransform,arg5,arg6);

    const int ids[6]={rig.lUpper,rig.lFore,rig.lHand,rig.rUpper,rig.rFore,rig.rHand};
    for(int i=0;i<6 && n>0 && n<(int)sizeof(line)-240;++i){
        const int b=ids[i];
        AV_V27Atom48 oa{},la{};
        bool ho=false,hl=false;
        if(saneCount&&saneData&&b>=0&&b<out.Count)
            ho=FP_ReadMemory((const void*)(out.Data+(uintptr_t)b*sizeof(AV_V27Atom48)),
                             &oa,sizeof(oa));
        if(haveLocal&&b>=0&&b<local.Count)
            hl=AV_V39ReadAtomSafe(local,b,la);

        n+=sprintf_s(line+n,sizeof(line)-n,
            "%s%d:O[%+.2f,%+.2f,%+.2f q=%+.3f,%+.3f,%+.3f,%+.3f] vsLocal[dp=%.3f q=%.5f]",
            i?",":"",b,
            ho?oa.translation[0]:-9999.0f,ho?oa.translation[1]:-9999.0f,
            ho?oa.translation[2]:-9999.0f,
            ho?oa.rotation[0]:-9.0f,ho?oa.rotation[1]:-9.0f,
            ho?oa.rotation[2]:-9.0f,ho?oa.rotation[3]:-9.0f,
            (ho&&hl)?AV_V39PosDelta(oa,la):-1.0f,
            (ho&&hl)?AV_V39QDotAbs(oa,la):-1.0f);
    }
    if(n>0&&n<(int)sizeof(line)-3){line[n++]='\n';line[n]=0;}
    FP_Log(line);

    if(!(saneCount&&saneData)) return;

    int parents[128]{};
    AV_V27Atom48 ref[128]{};
    bool hp=AV_V16ReadNonstandardRefPose(comp,rig,parents,ref);
    if(!hp && rig.standard70) hp=AV_V260ReadParentTable(comp,parents,70);
    if(!hp) return;

    char dl[2400]{};
    int dn=sprintf_s(dl,sizeof(dl),
        "AV_V42_GETBONEATOMS_DESC comp=%p mesh=%p hand=%d outCount=%d descendants=",
        (void*)comp,(void*)rig.mesh,rig.rHand,out.Count);
    int emitted=0;
    const int limit=(rig.count<out.Count)?rig.count:out.Count;
    for(int b=0;b<limit && emitted<20 && dn>0 && dn<(int)sizeof(dl)-220;++b){
        if(!AV_V259IsDescendantOf(b,rig.rHand,parents,rig.count)) continue;
        AV_V27Atom48 oa{},la{};
        const bool ho=FP_ReadMemory(
            (const void*)(out.Data+(uintptr_t)b*sizeof(AV_V27Atom48)),&oa,sizeof(oa));
        const bool hl=haveLocal&&b<local.Count&&AV_V39ReadAtomSafe(local,b,la);
        dn+=sprintf_s(dl+dn,sizeof(dl)-dn,
            "%s%d:Opos[%+.2f,%+.2f,%+.2f] Oq[%+.3f,%+.3f,%+.3f,%+.3f] vsLocal[%.3f/%.5f]",
            emitted?",":"",b,
            ho?oa.translation[0]:-9999.0f,ho?oa.translation[1]:-9999.0f,
            ho?oa.translation[2]:-9999.0f,
            ho?oa.rotation[0]:-9.0f,ho?oa.rotation[1]:-9.0f,
            ho?oa.rotation[2]:-9.0f,ho?oa.rotation[3]:-9.0f,
            (ho&&hl)?AV_V39PosDelta(oa,la):-1.0f,
            (ho&&hl)?AV_V39QDotAbs(oa,la):-1.0f);
        ++emitted;
    }
    if(dn>0&&dn<(int)sizeof(dl)-3){dl[dn++]='\n';dl[dn]=0;}
    FP_Log(dl);
}

// SkeletalMeshComponent+0x290 is the object used by 0x6A5180; the EXE calls
// [vtable+0x2F0] on it at 0x6A5AD6.
static void AV_V41MaybeInstallGetBoneAtomsHook(uintptr_t comp)
{
    if(g_avV41GetBoneAtomsHookInstalled || !FP_V111PlausiblePtr(comp)) return;

    uintptr_t animTree=0,vtable=0,target=0;
    if(!FP_ReadMemory((const void*)(comp+0x290),&animTree,sizeof(animTree)) ||
       !FP_V111PlausiblePtr(animTree)) return;
    if(!FP_ReadMemory((const void*)animTree,&vtable,sizeof(vtable)) ||
       !FP_V111PlausiblePtr(vtable)) return;
    if(!FP_ReadMemory((const void*)(vtable+0x2F0),&target,sizeof(target)) ||
       !FP_V111PlausiblePtr(target)) return;

    MH_STATUS st=MH_CreateHook(
        (void*)target,&AV_V41HookedGetBoneAtoms,
        reinterpret_cast<void**>(&g_avV41OriginalGetBoneAtoms));
    if(st!=MH_OK && st!=MH_ERROR_ALREADY_CREATED){
        char line[320]{};
        sprintf_s(line,sizeof(line),
            "AV_V41_GETBONEATOMS_HOOK_CREATE_FAIL animTree=%p target=%p rva=0x%llX status=%d\n",
            (void*)animTree,(void*)target,
            (unsigned long long)AV_V27ToExeRva(target),(int)st);
        FP_Log(line); return;
    }
    st=MH_EnableHook((void*)target);
    if(st!=MH_OK && st!=MH_ERROR_ENABLED){
        char line[280]{};
        sprintf_s(line,sizeof(line),
            "AV_V41_GETBONEATOMS_HOOK_ENABLE_FAIL target=%p status=%d\n",
            (void*)target,(int)st);
        FP_Log(line); return;
    }

    g_avV41GetBoneAtomsTarget=(void*)target;
    g_avV41GetBoneAtomsHookInstalled=true;
    char line[420]{};
    sprintf_s(line,sizeof(line),
        "AV_V41_GETBONEATOMS_HOOK_INSTALLED animTree=%p vtable=%p slot=0x2F0 target=%p rva=0x%llX "
        "-- runtime-resolved native animation virtual\n",
        (void*)animTree,(void*)vtable,(void*)target,
        (unsigned long long)AV_V27ToExeRva(target));
    FP_Log(line);
}

static void AV_V41TraceGetBoneAtoms(
    uintptr_t comp,void* animTree,uintptr_t callerRva,
    const AV_V27Atom48* preSb,int preSbCount,
    const AV_V27Atom48* preLocal,int preLocalCount)
{
    const ULONGLONG now=GetTickCount64();
    if(now-g_avV41LastGetBoneAtomsTrace<200) return;

    AV_V16MasterRig rig{};
    if(!AV_V16ResolveMasterRig(comp,rig)) return;
    AV_V27TArray64 postSb{},postLocal{};
    const bool haveSb=AV_V27GetSpaceBases(comp,postSb);
    const bool haveLocal=AV_V39GetLocalAtoms(comp,postLocal);
    g_avV41LastGetBoneAtomsTrace=now;

    const int ids[6]={rig.lUpper,rig.lFore,rig.lHand,rig.rUpper,rig.rFore,rig.rHand};
    char line[1900]{};
    int n=sprintf_s(line,sizeof(line),
        "AV_V41_GETBONEATOMS_BOUNDARY comp=%p animTree=%p mesh=%p bones=%d callerRva=0x%llX "
        "Local[%d->%d] Space[%d->%d] arms=",
        (void*)comp,animTree,(void*)rig.mesh,rig.count,
        (unsigned long long)callerRva,
        preLocalCount,haveLocal?postLocal.Count:0,
        preSbCount,haveSb?postSb.Count:0);
    for(int i=0;i<6 && n>0 && n<(int)sizeof(line)-190;++i){
        const int b=ids[i]; AV_V27Atom48 l{},sp{};
        const bool hl=b>=0&&b<preLocalCount&&haveLocal&&AV_V39ReadAtomSafe(postLocal,b,l);
        const bool hs=b>=0&&b<preSbCount&&haveSb&&AV_V39ReadAtomSafe(postSb,b,sp);
        n+=sprintf_s(line+n,sizeof(line)-n,
            "%s%d:L%.3f/%.5f S%.3f/%.5f",i?",":"",b,
            hl?AV_V39PosDelta(preLocal[b],l):-1.0f,hl?AV_V39QDotAbs(preLocal[b],l):-1.0f,
            hs?AV_V39PosDelta(preSb[b],sp):-1.0f,hs?AV_V39QDotAbs(preSb[b],sp):-1.0f);
    }
    if(n>0&&n<(int)sizeof(line)-3){line[n++]='\n';line[n]=0;}
    FP_Log(line);
}

// helpers have already been declared.
static void AV_V40TraceComposeBoundary(
    uintptr_t comp,void* component,uintptr_t callerRva,
    const AV_V27Atom48* preSb,int preSbCount,
    const AV_V27Atom48* preLocal,int preLocalCount)
{
    const ULONGLONG now=GetTickCount64();
    if(now-g_avV40LastComposeTrace<200) return;

    AV_V16MasterRig rig{};
    if(!AV_V16ResolveMasterRig(comp,rig)) return;

    AV_V27TArray64 postSb{},postLocal{};
    const bool haveSb=AV_V27GetSpaceBases(comp,postSb);
    const bool haveLocal=AV_V39GetLocalAtoms(comp,postLocal);
    if(!haveSb && !haveLocal) return;

    g_avV40LastComposeTrace=now;

    const int ids[6]={rig.lUpper,rig.lFore,rig.lHand,
                      rig.rUpper,rig.rFore,rig.rHand};
    char line[1900]{};
    int n=sprintf_s(line,sizeof(line),
        "AV_V40_COMPOSE_BOUNDARY comp=%p mesh=%p bones=%d callerRva=0x%llX "
        "Local[%d->%d] Space[%d->%d] arms=",
        component,(void*)rig.mesh,rig.count,(unsigned long long)callerRva,
        preLocalCount,haveLocal?postLocal.Count:0,
        preSbCount,haveSb?postSb.Count:0);

    for(int i=0;i<6 && n>0 && n<(int)sizeof(line)-190;++i){
        const int b=ids[i];
        AV_V27Atom48 l{},sp{};
        const bool hl=b>=0 && b<preLocalCount && haveLocal &&
                      AV_V39ReadAtomSafe(postLocal,b,l);
        const bool hs=b>=0 && b<preSbCount && haveSb &&
                      AV_V39ReadAtomSafe(postSb,b,sp);
        n+=sprintf_s(line+n,sizeof(line)-n,
            "%s%d:L%.3f/%.5f S%.3f/%.5f",
            i?",":"",b,
            hl?AV_V39PosDelta(preLocal[b],l):-1.0f,
            hl?AV_V39QDotAbs(preLocal[b],l):-1.0f,
            hs?AV_V39PosDelta(preSb[b],sp):-1.0f,
            hs?AV_V39QDotAbs(preSb[b],sp):-1.0f);
    }
    if(n>0 && n<(int)sizeof(line)-3){line[n++]='\n';line[n]=0;}
    FP_Log(line);

    int parents[128]{};
    AV_V27Atom48 ref[128]{};
    bool hp=AV_V16ReadNonstandardRefPose(comp,rig,parents,ref);
    if(!hp && rig.standard70) hp=AV_V260ReadParentTable(comp,parents,70);
    if(!hp) return;

    char dl[2000]{};
    int dn=sprintf_s(dl,sizeof(dl),
        "AV_V40_COMPOSE_DESC comp=%p mesh=%p hand=%d descendants=",
        component,(void*)rig.mesh,rig.rHand);
    int emitted=0;
    for(int b=0;b<rig.count && emitted<20 && dn>0 && dn<(int)sizeof(dl)-190;++b){
        if(!AV_V259IsDescendantOf(b,rig.rHand,parents,rig.count)) continue;
        AV_V27Atom48 l{},sp{};
        const bool hl=b<preLocalCount && haveLocal &&
                      AV_V39ReadAtomSafe(postLocal,b,l);
        const bool hs=b<preSbCount && haveSb &&
                      AV_V39ReadAtomSafe(postSb,b,sp);
        dn+=sprintf_s(dl+dn,sizeof(dl)-dn,
            "%s%d:L%.3f/%.5f S%.3f/%.5f",
            emitted?",":"",b,
            hl?AV_V39PosDelta(preLocal[b],l):-1.0f,
            hl?AV_V39QDotAbs(preLocal[b],l):-1.0f,
            hs?AV_V39PosDelta(preSb[b],sp):-1.0f,
            hs?AV_V39QDotAbs(preSb[b],sp):-1.0f);
        ++emitted;
    }
    if(dn>0 && dn<(int)sizeof(dl)-3){dl[dn++]='\n';dl[dn]=0;}
    FP_Log(dl);
}

// rigidly rotating component-space descendants, preserve each skeleton's own
// parent-relative animated transform and rebuild it below the solved VR wrist.
static bool AV_V38CaptureAnimatedLocals(
    const AV_V27Atom48* pose,int count,const int* parents,AV_V27Atom48* local)
{
    if(!pose||!parents||!local||count<=0||count>128) return false;
    for(int b=0;b<count;++b){
        const int p=parents[b];
        if(p<0||p>=count||p==b){ local[b]=pose[b]; continue; }
        const AV_V27Atom48& pa=pose[p];
        const AV_V27Atom48& ch=pose[b];
        XrQuaternionf pq=AV_V220QNorm({pa.rotation[0],pa.rotation[1],pa.rotation[2],pa.rotation[3]});
        XrQuaternionf cq=AV_V220QNorm({ch.rotation[0],ch.rotation[1],ch.rotation[2],ch.rotation[3]});
        XrQuaternionf iq=AV_V220QConj(pq);
        XrQuaternionf lq=AV_V220QNorm(AV_V220QMul(iq,cq));
        AV_V220V3 d=AV_V220Sub(AV_V220AtomPos(ch),AV_V220AtomPos(pa));
        XrQuaternionf dq{d.x,d.y,d.z,0};
        XrQuaternionf lr=AV_V220QMul(AV_V220QMul(iq,dq),pq);
        local[b]=ch;
        local[b].rotation[0]=lq.x; local[b].rotation[1]=lq.y;
        local[b].rotation[2]=lq.z; local[b].rotation[3]=lq.w;
        AV_V220SetAtomPos(local[b],{lr.x,lr.y,lr.z});
    }
    return true;
}
static bool AV_V38RebuildHandDescendants(
    AV_V27TArray64& sb,int count,const int* parents,int hand,const AV_V27Atom48* local)
{
    if(!parents||!local||count<=0||count>128||hand<0||hand>=count) return false;
    for(int b=0;b<count;++b){
        if(b==hand||!AV_V259IsDescendantOf(b,hand,parents,count)) continue;
        const int p=parents[b];
        if(p<0||p>=count||p==b) return false;
        AV_V27Atom48 pa{},out=local[b];
        if(!AV_V220ReadAtom(sb,p,pa)) return false;
        XrQuaternionf pq=AV_V220QNorm({pa.rotation[0],pa.rotation[1],pa.rotation[2],pa.rotation[3]});
        XrQuaternionf lq=AV_V220QNorm({local[b].rotation[0],local[b].rotation[1],local[b].rotation[2],local[b].rotation[3]});
        XrQuaternionf oq=AV_V220QNorm(AV_V220QMul(pq,lq));
        AV_V220V3 lp=AV_V220AtomPos(local[b]);
        XrQuaternionf lv{lp.x,lp.y,lp.z,0};
        XrQuaternionf rv=AV_V220QMul(AV_V220QMul(pq,lv),AV_V220QConj(pq));
        AV_V220SetAtomPos(out,AV_V220Add(AV_V220AtomPos(pa),{rv.x,rv.y,rv.z}));
        out.rotation[0]=oq.x;out.rotation[1]=oq.y;out.rotation[2]=oq.z;out.rotation[3]=oq.w;
        if(!AV_V220WriteAtom(sb,b,out)) return false;
    }
    return true;
}

// compare the mixed Hat Kid path that already works against Bow/HCR/other
// named-master paths.  No pose behavior is changed by these helpers.
static float AV_V36QuatDotAbs(const AV_V27Atom48& a,const AV_V27Atom48& b)
{
    float d=a.rotation[0]*b.rotation[0]+a.rotation[1]*b.rotation[1]+
            a.rotation[2]*b.rotation[2]+a.rotation[3]*b.rotation[3];
    return d<0.0f?-d:d;
}
static float AV_V36PosDelta(const AV_V27Atom48& a,const AV_V27Atom48& b)
{
    const float x=a.translation[0]-b.translation[0];
    const float y=a.translation[1]-b.translation[1];
    const float z=a.translation[2]-b.translation[2];
    return sqrtf(x*x+y*y+z*z);
}
static void AV_V36TraceDescendants(
    const char* phase,const char* branch,uintptr_t comp,uintptr_t mesh,
    AV_V27TArray64& sb,int count,int hand,
    const int* parents,const AV_V27Atom48* before)
{
    if(!phase||!branch||!FP_V111PlausiblePtr(comp)||!FP_V111PlausiblePtr(mesh)||
       !FP_V111PlausiblePtr(sb.Data)||count<=0||count>128||hand<0||hand>=count||
       !parents||!before) return;

    static ULONGLONG lastTick=0;
    static uintptr_t lastMesh=0;
    static int burst=0;
    const ULONGLONG now=GetTickCount64();
    if(mesh!=lastMesh){ lastMesh=mesh; burst=0; lastTick=0; }
    if(burst>=10 || (lastTick && now-lastTick<350)) return;
    lastTick=now; ++burst;

    AV_V27Atom48 handNow{};
    if(!AV_V220ReadAtom(sb,hand,handNow)) return;

    char line[1400]{};
    int off=sprintf_s(line,sizeof(line),
        "AV_V36_DESC phase=%s branch=%s comp=%p mesh=%p count=%d hand=%d "
        "handMove=%.3f handQdot=%.5f descendants=[",
        phase,branch,(void*)comp,(void*)mesh,count,hand,
        AV_V36PosDelta(handNow,before[hand]),AV_V36QuatDotAbs(handNow,before[hand]));

    int shown=0;
    for(int b=0;b<count && shown<12 && off>0 && off<(int)sizeof(line)-120;++b){
        if(b==hand || !AV_V259IsDescendantOf(b,hand,parents,count)) continue;
        AV_V27Atom48 cur{};
        if(!AV_V220ReadAtom(sb,b,cur)) continue;
        off += sprintf_s(line+off,sizeof(line)-off,
            "%s%d:p%.2f/q%.4f",
            shown?",":"",b,AV_V36PosDelta(cur,before[b]),AV_V36QuatDotAbs(cur,before[b]));
        ++shown;
    }
    if(off>0 && off<(int)sizeof(line)-8) sprintf_s(line+off,sizeof(line)-off,"]\n");
    FP_Log(line);
}

//
// Static RE correction:
//   RSI starts as selected LODModel, then `add rsi,0x20`.
//   0x6CE820 iterates a TArray<WORD> at that address.
//   When that list ends, 0x6CEA9C loads RSI from [rsp+r12*8+0x28].
//   the two stack entries are:
//       +0x20 = selectedLODModel + 0x20
//       +0x28 = original R9 / hook `extra` argument
//   So BuildRefToLocal consumes TWO independent WORD bone-index lists.
struct AV_V62WordArray { uintptr_t Data; int Count; int Max; };

static bool AV_V62ReadWordArray(uintptr_t p,AV_V62WordArray& a)
{
    memset(&a,0,sizeof(a));
    if(!FP_V111PlausiblePtr(p)||!FP_ReadMemory((const void*)p,&a,sizeof(a))) return false;
    if(a.Count<0||a.Count>512||a.Max<a.Count) return false;
    if(a.Count>0&&!FP_V111PlausiblePtr(a.Data)) return false;
    return true;
}

static void AV_V62TraceExactBuildRefLists(
    AV_V27TArray64* output,uintptr_t poseComp,const AV_V16MasterRig& rig,
    const int* parents,int requestedLod,uintptr_t extra)
{
    if(!output||!parents||!FP_V111PlausiblePtr(poseComp)||
       !FP_V111PlausiblePtr(rig.mesh)||rig.count<=0||rig.count>128) return;

    AV_V27TArray64 out{};
    if(!FP_ReadMemory(output,&out,sizeof(out))||
       !FP_V111PlausiblePtr(out.Data)||out.Count<=0||out.Count>512) return;

    int lodCount=0; uintptr_t lodArray=0;
    if(!FP_ReadMemory((const void*)(rig.mesh+0x138),&lodCount,sizeof(lodCount))||
       !FP_ReadMemory((const void*)(rig.mesh+0x130),&lodArray,sizeof(lodArray))||
       lodCount<=0||lodCount>32||!FP_V111PlausiblePtr(lodArray)) return;
    int lod=requestedLod;
    if(lod<0)lod=0; if(lod>=lodCount)lod=lodCount-1;
    uintptr_t lodModel=0;
    if(!FP_ReadMemory((const void*)(lodArray+(uintptr_t)lod*8),&lodModel,sizeof(lodModel))||
       !FP_V111PlausiblePtr(lodModel)) return;

    AV_V62WordArray list0{},list1{};
    const bool ok0=AV_V62ReadWordArray(lodModel+0x20,list0);
    const bool ok1=AV_V62ReadWordArray(extra,list1);

    bool in0[128]{},in1[128]{};
    auto fill=[&](const AV_V62WordArray& a,bool* mask){
        for(int i=0;i<a.Count;++i){
            unsigned short b=0xFFFF;
            if(FP_ReadMemory((const void*)(a.Data+(uintptr_t)i*2),&b,sizeof(b))&&b<128)
                mask[b]=true;
        }
    };
    if(ok0)fill(list0,in0);
    if(ok1)fill(list1,in1);

    static uintptr_t lastMesh=0; static int budget=0;
    if(lastMesh!=rig.mesh){lastMesh=rig.mesh;budget=0;}
    if(budget>=3)return; ++budget;

    char h[700]{};
    sprintf_s(h,sizeof(h),
        "AV_V62_RTL_LISTS_BEGIN poseComp=%p mesh=%p reqLod=%d lod=%d lodCount=%d "
        "rigCount=%d outCount=%d list0Ptr=%p list0Ok=%d list0Count=%d "
        "extra=%p list1Ok=%d list1Count=%d sample=%d/3\n",
        (void*)poseComp,(void*)rig.mesh,requestedLod,lod,lodCount,rig.count,out.Count,
        (void*)(lodModel+0x20),ok0?1:0,ok0?list0.Count:-1,
        (void*)extra,ok1?1:0,ok1?list1.Count:-1,budget);
    FP_Log(h);

    // actually represents and what the R9 list contributes.
    for(int which=0;which<2;++which){
        const AV_V62WordArray& a=which?list1:list0;
        const bool ok=which?ok1:ok0;
        if(!ok)continue;
        char line[1200]{};
        int off=sprintf_s(line,sizeof(line),"AV_V62_LIST%d_VALUES count=%d values=",which,a.Count);
        const int cap=(a.Count<96)?a.Count:96;
        for(int i=0;i<cap && off>0 && off<(int)sizeof(line)-24;++i){
            unsigned short b=0xFFFF;
            if(!FP_ReadMemory((const void*)(a.Data+(uintptr_t)i*2),&b,sizeof(b)))break;
            off+=sprintf_s(line+off,sizeof(line)-off,"%s%u",i?",":"",b);
        }
        sprintf_s(line+off,sizeof(line)-off,"%s\n",a.Count>cap?",...":"");
        FP_Log(line);
    }

    for(int side=0;side<2;++side){
        const int hand=side?rig.rHand:rig.lHand;
        for(int b=0;b<rig.count;++b){
            if(b!=hand&&!AV_V259IsDescendantOf(b,hand,parents,rig.count))continue;
            const bool authored=in0[b]||in1[b];
            char l[900]{};
            if(authored&&b<out.Count){
                float m[16]{};
                if(FP_ReadMemory((const void*)(out.Data+(uintptr_t)b*0x40),m,sizeof(m))){
                    const float r0=sqrtf(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);
                    const float r1=sqrtf(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]);
                    const float r2=sqrtf(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
                    sprintf_s(l,sizeof(l),
                        "AV_V62_RTL side=%s b=%d p=%d in0=%d in1=%d authored=1 "
                        "T=[%+.4f,%+.4f,%+.4f,%+.4f] rowLen=[%.5f,%.5f,%.5f] "
                        "diag=[%+.5f,%+.5f,%+.5f,%+.5f]\n",
                        side?"R":"L",b,parents[b],in0[b]?1:0,in1[b]?1:0,
                        m[12],m[13],m[14],m[15],r0,r1,r2,m[0],m[5],m[10],m[15]);
                }else{
                    sprintf_s(l,sizeof(l),
                        "AV_V62_RTL side=%s b=%d p=%d in0=%d in1=%d authored=1 matrixRead=0\n",
                        side?"R":"L",b,parents[b],in0[b]?1:0,in1[b]?1:0);
                }
            }else{
                sprintf_s(l,sizeof(l),
                    "AV_V62_RTL side=%s b=%d p=%d in0=%d in1=%d authored=0\n",
                    side?"R":"L",b,parents[b],in0[b]?1:0,in1[b]?1:0);
            }
            FP_Log(l);
        }
    }
    FP_Log("AV_V62_RTL_LISTS_END\n");
}

// UE3 FStaticLODModel layout identifies selectedLOD+0x20 as ActiveBoneIndices.
// R9 is ExtraRequiredBoneIndices (nullable / commonly empty).  The native loop
// writes ReferenceToLocal only for the union of those sets; that is intentional,
// because only active skinning bones are consumed by the render sections.
static void AV_V63TraceRefToLocalCall(
    AV_V27TArray64* output, uintptr_t component, int requestedLod, uintptr_t extra)
{
    if(!output || !FP_V111PlausiblePtr(component)) return;

    uintptr_t mesh=0, parentAnim=0, spaceData=0, parentMapData=0;
    int spaceCount=0, parentMapCount=0;
    if(!FP_ReadMemory((const void*)(component+0x278),&mesh,sizeof(mesh)) ||
       !FP_V111PlausiblePtr(mesh)) return;
    FP_ReadMemory((const void*)(component+0x37c),&parentAnim,sizeof(parentAnim));
    FP_ReadMemory((const void*)(component+0x318),&spaceData,sizeof(spaceData));
    FP_ReadMemory((const void*)(component+0x320),&spaceCount,sizeof(spaceCount));
    FP_ReadMemory((const void*)(component+0x384),&parentMapData,sizeof(parentMapData));
    FP_ReadMemory((const void*)(component+0x38c),&parentMapCount,sizeof(parentMapCount));

    uintptr_t lodArray=0, refInvData=0;
    int lodCount=0, refInvCount=0;
    if(!FP_ReadMemory((const void*)(mesh+0x130),&lodArray,sizeof(lodArray)) ||
       !FP_ReadMemory((const void*)(mesh+0x138),&lodCount,sizeof(lodCount)) ||
       lodCount<=0 || lodCount>32 || !FP_V111PlausiblePtr(lodArray)) return;
    FP_ReadMemory((const void*)(mesh+0x148),&refInvData,sizeof(refInvData));
    FP_ReadMemory((const void*)(mesh+0x150),&refInvCount,sizeof(refInvCount));

    int lod=requestedLod;
    if(lod<0) lod=0;
    if(lod>=lodCount) lod=lodCount-1;
    uintptr_t lodModel=0;
    if(!FP_ReadMemory((const void*)(lodArray+(uintptr_t)lod*8),&lodModel,sizeof(lodModel)) ||
       !FP_V111PlausiblePtr(lodModel)) return;

    AV_V62WordArray activeBones{}, extraBones{};
    const bool activeOk=AV_V62ReadWordArray(lodModel+0x20,activeBones);
    const bool extraOk=extra ? AV_V62ReadWordArray(extra,extraBones) : true;

    // Rate-limit per component+mesh pair but allow enough calls to expose body,
    // head, clothing/accessory, and master-pose component differences.
    struct Seen { uintptr_t comp,mesh; int hits; };
    static Seen seen[32]{};
    int slot=-1;
    for(int i=0;i<32;++i){
        if(seen[i].comp==component && seen[i].mesh==mesh){ slot=i; break; }
        if(slot<0 && seen[i].comp==0) slot=i;
    }
    if(slot<0) return;
    if(seen[slot].comp==0){ seen[slot].comp=component; seen[slot].mesh=mesh; }
    if(seen[slot].hits>=4) return;
    const int sample=++seen[slot].hits;

    char line[1800]{};
    int off=sprintf_s(line,sizeof(line),
        "AV_V63_RTL_CALL comp=%p mesh=%p reqLod=%d lod=%d/%d refBones=%d "
        "spaceBases=%p/%d parentAnim=%p parentMap=%p/%d activePtr=%p activeOk=%d activeCount=%d "
        "extra=%p extraOk=%d extraCount=%d sample=%d/4 active=[",
        (void*)component,(void*)mesh,requestedLod,lod,lodCount,refInvCount,
        (void*)spaceData,spaceCount,(void*)parentAnim,(void*)parentMapData,parentMapCount,
        (void*)(lodModel+0x20),activeOk?1:0,activeOk?activeBones.Count:-1,
        (void*)extra,extraOk?1:0,(extra&&extraOk)?extraBones.Count:0,sample);

    if(activeOk){
        const int cap=activeBones.Count<96?activeBones.Count:96;
        for(int i=0;i<cap && off>0 && off<(int)sizeof(line)-40;++i){
            unsigned short b=0xFFFF;
            if(!FP_ReadMemory((const void*)(activeBones.Data+(uintptr_t)i*2),&b,sizeof(b))) break;
            off+=sprintf_s(line+off,sizeof(line)-off,"%s%u",i?",":"",b);
        }
        if(activeBones.Count>cap) off+=sprintf_s(line+off,sizeof(line)-off,",...");
    }
    off+=sprintf_s(line+off,sizeof(line)-off,"] extra=[");
    if(extra && extraOk){
        const int cap=extraBones.Count<96?extraBones.Count:96;
        for(int i=0;i<cap && off>0 && off<(int)sizeof(line)-40;++i){
            unsigned short b=0xFFFF;
            if(!FP_ReadMemory((const void*)(extraBones.Data+(uintptr_t)i*2),&b,sizeof(b))) break;
            off+=sprintf_s(line+off,sizeof(line)-off,"%s%u",i?",":"",b);
        }
        if(extraBones.Count>cap) off+=sprintf_s(line+off,sizeof(line)-off,",...");
    }
    sprintf_s(line+off,sizeof(line)-off,"]\n");
    FP_Log(line);
}

// AHiT's 0x6CE6E0 path is validated independently against the executable at runtime.
static void AV_V64TraceParentBoneMap(uintptr_t component)
{
    if(!FP_V111PlausiblePtr(component)) return;

    uintptr_t mesh=0,parentAnim=0,parentMapData=0,parentSpaceData=0,childSpaceData=0;
    int parentMapCount=0,parentSpaceCount=0,childSpaceCount=0;
    if(!FP_ReadMemory((const void*)(component+0x278),&mesh,sizeof(mesh)) || !FP_V111PlausiblePtr(mesh)) return;
    FP_ReadMemory((const void*)(component+0x37c),&parentAnim,sizeof(parentAnim));
    FP_ReadMemory((const void*)(component+0x384),&parentMapData,sizeof(parentMapData));
    FP_ReadMemory((const void*)(component+0x38c),&parentMapCount,sizeof(parentMapCount));
    FP_ReadMemory((const void*)(component+0x318),&childSpaceData,sizeof(childSpaceData));
    FP_ReadMemory((const void*)(component+0x320),&childSpaceCount,sizeof(childSpaceCount));
    if(!FP_V111PlausiblePtr(parentAnim) || !FP_V111PlausiblePtr(parentMapData) || parentMapCount<=0 || parentMapCount>256) return;
    FP_ReadMemory((const void*)(parentAnim+0x318),&parentSpaceData,sizeof(parentSpaceData));
    FP_ReadMemory((const void*)(parentAnim+0x320),&parentSpaceCount,sizeof(parentSpaceCount));

    struct Seen { uintptr_t comp,mesh,parent; int hits; };
    static Seen seen[32]{};
    int slot=-1;
    for(int i=0;i<32;++i){
        if(seen[i].comp==component && seen[i].mesh==mesh && seen[i].parent==parentAnim){slot=i;break;}
        if(slot<0 && seen[i].comp==0) slot=i;
    }
    if(slot<0) return;
    if(seen[slot].comp==0){seen[slot].comp=component;seen[slot].mesh=mesh;seen[slot].parent=parentAnim;}
    if(seen[slot].hits>=2) return;
    const int sample=++seen[slot].hits;

    char line[2200]{};
    int off=sprintf_s(line,sizeof(line),
        "AV_V64_PARENTMAP comp=%p mesh=%p parent=%p map=%p/%d childSB=%p/%d parentSB=%p/%d sample=%d/2 map=[",
        (void*)component,(void*)mesh,(void*)parentAnim,(void*)parentMapData,parentMapCount,
        (void*)childSpaceData,childSpaceCount,(void*)parentSpaceData,parentSpaceCount,sample);
    const int cap=parentMapCount<96?parentMapCount:96;
    for(int i=0;i<cap && off>0 && off<(int)sizeof(line)-48;++i){
        int32_t m=-1;
        if(!FP_ReadMemory((const void*)(parentMapData+(uintptr_t)i*4),&m,sizeof(m))) break;
        off+=sprintf_s(line+off,sizeof(line)-off,"%s%d>%d",i?",":"",i,m);
    }
    if(parentMapCount>cap) off+=sprintf_s(line+off,sizeof(line)-off,",...");
    sprintf_s(line+off,sizeof(line)-off,"]\n");
    FP_Log(line);

    // Focused matrix-source trace for the two finger ranges we've been investigating.
    // Log child->parent mapping plus translations from child and parent SpaceBases where available.
    const int ranges[][2]={{31,43},{45,58}};
    for(int r=0;r<2;++r){
        for(int b=ranges[r][0];b<=ranges[r][1] && b<parentMapCount;++b){
            int32_t m=-1;
            if(!FP_ReadMemory((const void*)(parentMapData+(uintptr_t)b*4),&m,sizeof(m))) continue;
            AV_V27Atom48 ca{},pa{};
            bool cok=false,pok=false;
            AV_V27TArray64 childSB{childSpaceData,childSpaceCount,childSpaceCount};
            AV_V27TArray64 parentSB{parentSpaceData,parentSpaceCount,parentSpaceCount};
            if(FP_V111PlausiblePtr(childSpaceData) && b<childSpaceCount) cok=AV_V220ReadAtom(childSB,b,ca);
            if(FP_V111PlausiblePtr(parentSpaceData) && m<parentSpaceCount) pok=AV_V220ReadAtom(parentSB,(int)m,pa);
            char q[512]{};
            sprintf_s(q,sizeof(q),
                "AV_V64_MAP_BONE comp=%p child=%d parent=%d childSBok=%d childT=[%.4f %.4f %.4f] parentSBok=%d parentT=[%.4f %.4f %.4f]\n",
                (void*)component,b,(int)m,cok?1:0,cok?ca.translation[0]:0.0f,cok?ca.translation[1]:0.0f,cok?ca.translation[2]:0.0f,
                pok?1:0,pok?pa.translation[0]:0.0f,pok?pa.translation[1]:0.0f,pok?pa.translation[2]:0.0f);
            FP_Log(q);
        }
    }
}

//
// Static confirmation from HatinTimeGame.exe RVA 0x6CE6E0:
//   +0x37C ParentAnimComponent
//   +0x384 ParentBoneMap.Data
//   +0x38C ParentBoneMap.Count
//   +0x318 SpaceBases.Data
//   +0x320 SpaceBases.Count
//
// Parent path at 0x1406CE0F6:
//   mapData = [component+0x384]
//   parentBone = *(int32*)(mapData + activeBone*4)   <-- int32, not WORD
//   sourceAtom = ParentAnimComponent->SpaceBases[parentBone]
// Own-pose path uses component->SpaceBases[activeBone].
// RefBasesInvMatrix is indexed by the CHILD/active bone, not parentBone.
//
// this helper records the exact inputs before the original call and the resulting
struct AV_V65ChainSample {
    bool valid{};
    uintptr_t comp{}, mesh{}, parent{}, mapData{}, sourceData{};
    int activeBone{-1}, parentBone{-1}, sourceBone{-1};
    AV_V27Atom48 sourceAtom{};
};

static bool AV_V65CaptureChain(uintptr_t comp,int requestedLod,AV_V65ChainSample* out,int cap,int& outCount)
{
    outCount=0;
    if(!out || cap<=0 || !FP_V111PlausiblePtr(comp)) return false;

    uintptr_t mesh=0,parent=0,mapData=0,sourceData=0,lodData=0,lodModel=0,activeData=0;
    int mapCount=0,sourceCount=0,lodCount=0,activeCount=0;
    if(!FP_ReadMemory((const void*)(comp+0x278),&mesh,sizeof(mesh)) || !FP_V111PlausiblePtr(mesh)) return false;
    FP_ReadMemory((const void*)(comp+0x37c),&parent,sizeof(parent));
    FP_ReadMemory((const void*)(comp+0x384),&mapData,sizeof(mapData));
    FP_ReadMemory((const void*)(comp+0x38c),&mapCount,sizeof(mapCount));

    // Reproduce the executable's parent-path gate: parent != null and
    // ParentBoneMap.Count == SkeletalMesh.RefSkeleton.Num (mesh+0xDC).
    int refSkelCount=-1;
    FP_ReadMemory((const void*)(mesh+0xDC),&refSkelCount,sizeof(refSkelCount));
    const bool useParent=FP_V111PlausiblePtr(parent) && FP_V111PlausiblePtr(mapData) &&
                         mapCount>0 && mapCount==refSkelCount;

    const uintptr_t sourceComp=useParent?parent:comp;
    FP_ReadMemory((const void*)(sourceComp+0x318),&sourceData,sizeof(sourceData));
    FP_ReadMemory((const void*)(sourceComp+0x320),&sourceCount,sizeof(sourceCount));
    if(!FP_V111PlausiblePtr(sourceData) || sourceCount<=0 || sourceCount>512) return false;

    FP_ReadMemory((const void*)(mesh+0x130),&lodData,sizeof(lodData));
    FP_ReadMemory((const void*)(mesh+0x138),&lodCount,sizeof(lodCount));
    if(!FP_V111PlausiblePtr(lodData) || lodCount<=0 || lodCount>32) return false;
    int lod=requestedLod;
    if(lod<0) lod=0;
    if(lod>=lodCount) lod=lodCount-1;
    if(!FP_ReadMemory((const void*)(lodData+(uintptr_t)lod*8),&lodModel,sizeof(lodModel)) ||
       !FP_V111PlausiblePtr(lodModel)) return false;

    FP_ReadMemory((const void*)(lodModel+0x20),&activeData,sizeof(activeData));
    FP_ReadMemory((const void*)(lodModel+0x28),&activeCount,sizeof(activeCount));
    if(!FP_V111PlausiblePtr(activeData) || activeCount<=0 || activeCount>256) return false;

    AV_V27TArray64 sourceSB{sourceData,sourceCount,sourceCount};
    const int n=activeCount<cap?activeCount:cap;
    for(int i=0;i<n;++i){
        uint16_t child=0xFFFF;
        if(!FP_ReadMemory((const void*)(activeData+(uintptr_t)i*2),&child,sizeof(child))) continue;

        int parentBone=(int)child;
        if(useParent){
            if((int)child<0 || (int)child>=mapCount) continue;
            if(!FP_ReadMemory((const void*)(mapData+(uintptr_t)child*4),&parentBone,sizeof(parentBone))) continue;
        }
        if(parentBone<0 || parentBone>=sourceCount) continue;

        AV_V65ChainSample s{};
        s.valid=AV_V220ReadAtom(sourceSB,parentBone,s.sourceAtom);
        s.comp=comp; s.mesh=mesh; s.parent=parent; s.mapData=mapData; s.sourceData=sourceData;
        s.activeBone=(int)child; s.parentBone=useParent?parentBone:-1; s.sourceBone=parentBone;
        if(s.valid) out[outCount++]=s;
    }
    return outCount>0;
}

static void AV_V65LogChainAfter(const AV_V65ChainSample* samples,int count,const AV_V27TArray64* output)
{
    if(!samples || count<=0 || !output || !FP_V111PlausiblePtr(output->Data)) return;

    // Bounded: two snapshots per unique component.
    struct Seen { uintptr_t comp,mesh; int hits; };
    static Seen seen[32]{};
    const uintptr_t comp=samples[0].comp, mesh=samples[0].mesh;
    int slot=-1;
    for(int i=0;i<32;++i){
        if(seen[i].comp==comp && seen[i].mesh==mesh){slot=i;break;}
        if(slot<0 && seen[i].comp==0) slot=i;
    }
    if(slot<0) return;
    if(seen[slot].comp==0){seen[slot].comp=comp;seen[slot].mesh=mesh;}
    if(seen[slot].hits>=2) return;
    const int sample=++seen[slot].hits;

    for(int i=0;i<count;++i){
        const auto& s=samples[i];
        if(!s.valid || s.activeBone<0 || s.activeBone>=output->Count) continue;

        // keep the trace focused on the arm/hand region plus any child bone that
        // maps into master 31..58. This avoids flooding the render thread.
        const bool interesting=(s.activeBone>=31 && s.activeBone<=68) ||
                               (s.sourceBone>=31 && s.sourceBone<=58);
        if(!interesting) continue;

        float m[16]{};
        const bool outOk=FP_ReadMemory(
            (const void*)(output->Data+(uintptr_t)s.activeBone*0x40),m,sizeof(m));

        char line[768]{};
        sprintf_s(line,sizeof(line),
            "AV_V65_CHAIN comp=%p mesh=%p parent=%p active=%d map=%d source=%d "
            "srcT=[%.4f %.4f %.4f] outOk=%d rtlT=[%.4f %.4f %.4f %.4f] sample=%d/2\n",
            (void*)s.comp,(void*)s.mesh,(void*)s.parent,s.activeBone,s.parentBone,s.sourceBone,
            s.sourceAtom.translation[0],s.sourceAtom.translation[1],s.sourceAtom.translation[2],
            outOk?1:0,outOk?m[12]:0.0f,outOk?m[13]:0.0f,outOk?m[14]:0.0f,outOk?m[15]:0.0f,
            sample);
        FP_Log(line);
    }
}

//
// Static chain:
//   0x1406BA936  mov rax,[rdi+0x90]      ; live DynamicData
//   0x1406BA960  mov r13,[rax+0x08]      ; ReferenceToLocal.Data
//   0x1406BAB23  shl r11,6
//   0x1406BAB27  add r11,r8              ; chunk = Chunks.Data + index*0x40
//   0x1406BAB64  mov rsi,[r11+0x24]      ; chunk->BoneMap.Data
//   0x1406BB1BE  movzx ecx,word [rsi+rax*2]
//   0x1406BB1C2  shl rcx,6
//   0x1406BB1C6  add rcx,[rbp+0xF8]      ; ReferenceToLocal[BoneMap[localBone]]
//
static void AV_V67TraceExactChunkBoneMaps(uintptr_t component,int requestedLod)
{
    if(!FP_V111PlausiblePtr(component)) return;
    uintptr_t mesh=0,lodData=0,lodModel=0,chunksData=0,activeData=0;
    int lodCount=0,chunkCount=0,chunkMax=0,activeCount=0;
    if(!FP_ReadMemory((const void*)(component+0x278),&mesh,sizeof(mesh)) || !FP_V111PlausiblePtr(mesh)) return;
    FP_ReadMemory((const void*)(mesh+0x130),&lodData,sizeof(lodData));
    FP_ReadMemory((const void*)(mesh+0x138),&lodCount,sizeof(lodCount));
    if(!FP_V111PlausiblePtr(lodData)||lodCount<=0||lodCount>32) return;
    int lod=requestedLod; if(lod<0) lod=0; if(lod>=lodCount) lod=lodCount-1;
    if(!FP_ReadMemory((const void*)(lodData+(uintptr_t)lod*8),&lodModel,sizeof(lodModel))||!FP_V111PlausiblePtr(lodModel)) return;

    FP_ReadMemory((const void*)(lodModel+0x10),&chunksData,sizeof(chunksData));
    FP_ReadMemory((const void*)(lodModel+0x18),&chunkCount,sizeof(chunkCount));
    FP_ReadMemory((const void*)(lodModel+0x1c),&chunkMax,sizeof(chunkMax));
    FP_ReadMemory((const void*)(lodModel+0x20),&activeData,sizeof(activeData));
    FP_ReadMemory((const void*)(lodModel+0x28),&activeCount,sizeof(activeCount));
    if(!FP_V111PlausiblePtr(chunksData)||chunkCount<=0||chunkCount>128) return;

    struct Seen{uintptr_t comp,mesh,lodModel;int hits;}; static Seen seen[32]{};
    int slot=-1;
    for(int i=0;i<32;++i){if(seen[i].comp==component&&seen[i].mesh==mesh&&seen[i].lodModel==lodModel){slot=i;break;} if(slot<0&&seen[i].comp==0)slot=i;}
    if(slot<0)return;
    if(!seen[slot].comp){seen[slot].comp=component;seen[slot].mesh=mesh;seen[slot].lodModel=lodModel;}
    if(seen[slot].hits>=2)return;
    int sample=++seen[slot].hits;

    char h[640]{};
    sprintf_s(h,sizeof(h),"AV_V67_EXACT_CHUNKS comp=%p mesh=%p lod=%d/%d lodModel=%p chunks=%p/%d/%d stride=0x40 active=%p/%d sample=%d/2\n",
        (void*)component,(void*)mesh,lod,lodCount,(void*)lodModel,(void*)chunksData,chunkCount,chunkMax,(void*)activeData,activeCount,sample);
    FP_Log(h);

    for(int ci=0;ci<chunkCount;++ci){
        uintptr_t chunk=chunksData+(uintptr_t)ci*0x40,boneMapData=0;
        int boneMapCount=0,boneMapMax=0;
        FP_ReadMemory((const void*)(chunk+0x24),&boneMapData,sizeof(boneMapData));
        FP_ReadMemory((const void*)(chunk+0x2c),&boneMapCount,sizeof(boneMapCount));
        FP_ReadMemory((const void*)(chunk+0x30),&boneMapMax,sizeof(boneMapMax));
        char line[4096]{};
        int off=sprintf_s(line,sizeof(line),"AV_V67_BONEMAP comp=%p mesh=%p chunk=%d base=%p ptr=%p count=%d max=%d values=[",
            (void*)component,(void*)mesh,ci,(void*)chunk,(void*)boneMapData,boneMapCount,boneMapMax);
        if(FP_V111PlausiblePtr(boneMapData)&&boneMapCount>0&&boneMapCount<=256&&boneMapMax>=boneMapCount&&boneMapMax<=512){
            int cap=boneMapCount<128?boneMapCount:128;
            for(int i=0;i<cap&&off<(int)sizeof(line)-64;++i){uint16_t b=0xFFFF;if(!FP_ReadMemory((const void*)(boneMapData+(uintptr_t)i*2),&b,sizeof(b)))break;off+=sprintf_s(line+off,sizeof(line)-off,"%s%u",i?",":"",(unsigned)b);}
            if(boneMapCount>cap)off+=sprintf_s(line+off,sizeof(line)-off,",...");
        }
        sprintf_s(line+off,sizeof(line)-off,"]\n"); FP_Log(line);
    }
}

// publication experiments. UE3 attachments now consume the persistent authoritative

