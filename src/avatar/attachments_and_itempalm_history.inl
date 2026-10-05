//
// a reorganized slave mesh, mapped that bone and its parent through ParentBoneMap,
// recovered the authored parent-local transform from the slave RefSkeleton, then
// composed ONLY that attachment bone beneath the already solved master parent.
//
// were the remaining problem" path.

static void __fastcall AV_V215HookedBuildRefToLocal(
    AV_V27TArray64* output, void* component, int lodIndex, void* extra)
{
    LARGE_INTEGER avPerfT0{},avPerfT1{},avPerfT2{},avPerfT3{},avPerfT4{};
    QueryPerformanceCounter(&avPerfT0);

    const uintptr_t comp=(uintptr_t)component;
    static bool s_avV10Banner=false;
    if(!s_avV10Banner){ s_avV10Banner=true; FP_Log("AV_V119_NATIVE_ROOT_ROOMSCALE ACTIVE -- single universal solver; attachment archaeology removed; arm-name discovery cached process-wide\n"); }

    const bool active=g_avV29ProbeEnabled && AV_V215IsHatKidFamily(comp) &&
                      g_controllerGripPoseValid[1] && g_xrTrackingOriginSet;
    QueryPerformanceCounter(&avPerfT1);

    AV_V27TArray64 sb{};
    AV_V27Atom48 saved[15]{}; // 32,33,34 and 35..45
    AV_V27Atom48 savedAll[70]{};
    bool savedAllValid=false;
    AV_V27Atom48 avV16Saved[128]{};
    AV_V27Atom48 avV38IncomingLocal[128]{};
    int avV16SavedCount=0;
    bool avV16SavedValid=false;
    bool avV38IncomingLocalValid=false;
    bool changed=false;
    AV_V220V3 bodyOffset{};
    AV_V220V3 avV94PhysicalRoomscale{};
    AV_V220V3 shoulder{},oldElbow{},oldHand{},target{},newElbow{};
    float upperLen=0,lowerLen=0,solveUpperLenDbg=0,solveLowerLenDbg=0,stretchDbg=0;
    XrVector3f debugRel{};
    AV_V220V3 debugPhysicalOffsetComponent{};

    // always mutate the MASTER HatKidHead SpaceBases. Child body/legs components
    // can consume this master through ParentAnimComponent.
    uintptr_t poseComp=g_avV27TargetComponent;
    AV_V16MasterRig avV16Rig{};
    LARGE_INTEGER avV101RigT0{},avV101RigT1{};
    QueryPerformanceCounter(&avV101RigT0);
    const bool avV16HaveRig=active && AV_V16ResolveMasterRig(poseComp,avV16Rig);
    QueryPerformanceCounter(&avV101RigT1);
    // successfully resolved player master uses the same RefSkeleton/IK/publication path.
    const bool avV16Nonstandard=avV16HaveRig;
    // all player masters use the same solver.

    // Every resolved rig is handled by the universal RefSkeleton solver.

    //
    // do this only after the corrected 0..17 head chain has been
    // published for sockets/hat tracking, and only immediately around
    // BuildRefToLocalMatrices. SpaceBases is component-space here, so collapsing
    // the scale of the neck/head-owned bones does not move the already-solved
    // arm/body component-space transforms. The game's live animation atoms are
    // restored below immediately after matrix generation.
    //
    // Bone 3 = bip_neck. Bones 4..16 = ponytail/hat/hair. Bone 17 = zipper.
    // the stock head has no separate bip_head entry; neck is its skinning anchor.
    bool headRenderCollapsed=false;
    if(active && changed && savedAllValid && sb.Count>=18){
        bool collapseOk=true;
        AV_V27Atom48 neckAnchor{};
        collapseOk=AV_V220ReadAtom(sb,3,neckAnchor);
        for(int b=3;b<18 && collapseOk;++b){
            AV_V27Atom48 a{};
            collapseOk=AV_V220ReadAtom(sb,b,a);
            if(collapseOk){
                // Collapse every head/hair/ponytail/hat-weighted bone to one
                // microscopic point at the neck. Scaling alone left visible
                // paired hair islands because their component-space translations
                // remained separated.
                a.translation[0]=neckAnchor.translation[0];
                a.translation[1]=neckAnchor.translation[1];
                a.translation[2]=neckAnchor.translation[2];
                a.scale[0]=0.000001f;
                a.scale[1]=0.000001f;
                a.scale[2]=0.000001f;
                collapseOk=AV_V220WriteAtom(sb,b,a);
            }
        }
        headRenderCollapsed=collapseOk;
        static bool s_loggedHeadCollapse=false;
        if(!s_loggedHeadCollapse || !collapseOk){
            char l[320]{};
            sprintf_s(l,sizeof(l),
                "AV_V248_RENDER_HEAD_COLLAPSE component=%p bones=3..17 ok=%d -- render matrices only; live head chain preserved\n",
                (void*)poseComp,collapseOk?1:0);
            FP_Log(l);
            if(collapseOk) s_loggedHeadCollapse=true;
        }
    }

    //
    // could leave that offset at -1 forever and silently disable the body/leg
    // slave correction. Resolve it once from a real Hat Kid skeletal component.
    if(g_avV14ParentAnimComponentOffset < 0 && FP_V111PlausiblePtr(comp)){
        static bool s_avV106TriedParentAnimResolve=false;
        if(!s_avV106TriedParentAnimResolve){
            s_avV106TriedParentAnimResolve=true;
            const bool avV106Resolved =
                AV_V1ResolvePropertyOffset(comp,
                    "ParentAnimComponent",
                    g_avV14ParentAnimComponentOffset);
            char avV106Line[256]{};
            sprintf_s(avV106Line,sizeof(avV106Line),
                "AV_V106_PARENTANIM_INIT resolved=%d offset=0x%llX comp=%p\n",
                avV106Resolved?1:0,
                (unsigned long long)(g_avV14ParentAnimComponentOffset>=0 ?
                    (uintptr_t)g_avV14ParentAnimComponentOffset : 0),
                (void*)comp);
            FP_Log(avV106Line);
        }
    }

    // normally leave direct ParentAnimComponent slave LocalToWorld alone.
    // BuildRefToLocal maps body/leg bones from corrected master SpaceBases and a
    // normal second roomscale translation would double-correct them.
    //
    // Large maps can, however, expose a different failure: a direct slave can
    // retain an absolute-world translation from a different origin generation
    // and appear to fly far away from the authoritative HatKidHead master. Only
    // repair that clearly-impossible desync. During this BuildRefToLocal call we
    // borrow the master's world translation, preserving the slave's rotation and
    // scale, then restore the original matrix immediately afterward.
    float avV10SavedL2W[16]{};
    bool avV10PatchedSlaveL2W=false;
    AV_V220V3 avV10WorldDelta{};
    uintptr_t avV10Parent=0;
    if(active && FP_V111PlausiblePtr(comp) && FP_V111PlausiblePtr(poseComp) && comp!=poseComp)
    {
        avV10Parent=AV_V211ReadParentAnim(comp);
        if(avV10Parent==poseComp)
        {
            float childL2W[16]{}, masterL2W[16]{};
            if(AV_V230ReadLocalToWorld(comp,childL2W) &&
               AV_V230ReadLocalToWorld(poseComp,masterL2W))
            {
                const float dx=childL2W[12]-masterL2W[12];
                const float dy=childL2W[13]-masterL2W[13];
                const float dz=childL2W[14]-masterL2W[14];
                const float d2=dx*dx+dy*dy+dz*dz;
                // Components of the same pawn should never be five meters apart.
                // keep ordinary attachment offsets untouched; this is only an
                // origin-generation / catastrophic-desync guard.
                if(_finite(d2) && d2 > 500.0f*500.0f)
                {
                    memcpy(avV10SavedL2W,childL2W,sizeof(avV10SavedL2W));
                    childL2W[12]=masterL2W[12];
                    childL2W[13]=masterL2W[13];
                    childL2W[14]=masterL2W[14];
                    __try
                    {
                        memcpy((void*)(comp+AV_V231_LOCAL_TO_WORLD_OFFSET),childL2W,sizeof(childL2W));
                        avV10PatchedSlaveL2W=true;
                        avV10WorldDelta={dx,dy,dz};
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER){}
                }
            }
        }
    }

    // repeatedly for the authoritative player master during one rendered frame;
    // solve once, then replay the exact published pose for subsequent requests.
    const unsigned long long avV84Frame=(unsigned long long)g_presentFrameNumber;
    if(active && avV16Nonstandard &&
       g_avV84CacheValid && g_avV84CacheFrame==avV84Frame &&
       g_avV84CacheOwner==poseComp && g_avV84CacheCount==avV16Rig.count &&
       avV16Rig.count>0 && avV16Rig.count<=128 &&
       AV_V27GetSpaceBases(poseComp,sb) && sb.Count>=avV16Rig.count)
    {
        AV_V27Atom48 incoming[128]{};
        bool cacheOk=false;
        const size_t v111PoseBytes=sizeof(AV_V27Atom48)*(size_t)avV16Rig.count;
        __try{
            memcpy(incoming,(const void*)sb.Data,v111PoseBytes);
            memcpy((void*)sb.Data,g_avV84Solved,v111PoseBytes);
            cacheOk=true;
        }__except(EXCEPTION_EXECUTE_HANDLER){ cacheOk=false; }

        if(cacheOk){
            ++g_avV84Hits;
            if(g_avV215OriginalBuildRefToLocal)
                g_avV215OriginalBuildRefToLocal(output,component,lodIndex,extra);

            // master must remain in the solved publication generation after native
            // BuildRefToLocal returns. This is the nonstandard equivalent of
            if(g_avV97PublishedValid &&
               g_avV97PublishedOwner==poseComp &&
               g_avV97PublishedCount==avV16Rig.count){
                __try{ memcpy((void*)sb.Data,g_avV97Published,v111PoseBytes); }
                __except(EXCEPTION_EXECUTE_HANDLER){}
            }else{
                __try{ memcpy((void*)sb.Data,incoming,v111PoseBytes); }
                __except(EXCEPTION_EXECUTE_HANDLER){}
            }

            if(avV10PatchedSlaveL2W){
                __try{ memcpy((void*)(comp+AV_V231_LOCAL_TO_WORLD_OFFSET),
                               avV10SavedL2W,sizeof(avV10SavedL2W)); }
                __except(EXCEPTION_EXECUTE_HANDLER){}
            }

            const ULONGLONG now=GetTickCount64();
            if(!g_avV84LastReport) g_avV84LastReport=now;
            if(now-g_avV84LastReport>=1000){
                char line[256]{};
                sprintf_s(line,sizeof(line),
                    "AV_V84_CACHE solves=%llu hits=%llu frame=%llu owner=%p bones=%d\\n",
                    (unsigned long long)g_avV84Solves,
                    (unsigned long long)g_avV84Hits,
                    avV84Frame,(void*)poseComp,avV16Rig.count);
                FP_Log(line);
                g_avV84Solves=0;
                g_avV84Hits=0;
                g_avV84LastReport=now;
            }
            return;
        }

        // A failed replay must never leave the master partially cached.
        __try{ memcpy((void*)sb.Data,incoming,v111PoseBytes); }
        __except(EXCEPTION_EXECUTE_HANDLER){}
        g_avV84CacheValid=false;
    }

    // resolved rigs all use their OWN bind geometry and hierarchy here.
    if(active && avV16Nonstandard && AV_V27GetSpaceBases(poseComp,sb) &&
       sb.Count>=avV16Rig.count && avV16Rig.count<=128)
    {
        int parents[128]{};
        AV_V27Atom48 ref[128]{};
        avV16SavedCount=avV16Rig.count;
        avV16SavedValid=false;
        __try{
            memcpy(avV16Saved,(const void*)sb.Data,sizeof(AV_V27Atom48)*(size_t)avV16SavedCount);
            avV16SavedValid=true;
        }__except(EXCEPTION_EXECUTE_HANDLER){ avV16SavedValid=false; }

        // leg atoms retain HatVR's previous roomscale, which then gets added again.
        // keep only a cheap generation gate here; individual non-arm atoms are
        // compared against the published generation immediately before translation.
        const bool avV112HavePublishedGeneration =
            avV16SavedValid &&
            g_avV97PublishedValid &&
            g_avV97PublishedOwner==poseComp &&
            g_avV97PublishedCount==avV16SavedCount &&
            g_avV104PublishedRoomscaleValid;

        LARGE_INTEGER avV101RefT0{},avV101RefT1{};
        QueryPerformanceCounter(&avV101RefT0);
        bool ok=avV16SavedValid &&
            AV_V16ReadNonstandardRefPose(poseComp,avV16Rig,parents,ref);
        QueryPerformanceCounter(&avV101RefT1);
        {
            const double avV101RigMs=AV_PerfV2Ms(avV101RigT0,avV101RigT1);
            const double avV101RefMs=AV_PerfV2Ms(avV101RefT0,avV101RefT1);
            if(avV101RigMs>=10.0 || avV101RefMs>=10.0){
                char avV101Line[448]{};
                sprintf_s(avV101Line,sizeof(avV101Line),
                    "AV_V103_STALL_PROFILE comp=%p mesh=%p bones=%d standard70=%d resolveRig=%.3fms refPose=%.3fms refOK=%d frame=%llu\n",
                    (void*)poseComp,(void*)avV16Rig.mesh,avV16Rig.count,avV16Rig.standard70?1:0,
                    avV101RigMs,avV101RefMs,ok?1:0,
                    (unsigned long long)g_presentFrameNumber);
                FP_Log(avV101Line);
            }
        }
        if(ok)
            avV38IncomingLocalValid=AV_V38CaptureAnimatedLocals(
                avV16Saved,avV16Rig.count,parents,avV38IncomingLocal);
        if(ok){
            // nonstandard master with its authored component-space RefSkeleton.
            // this is the practical UE3 equivalent of "all bones neutral, then arms".
            for(int b=0;b<avV16Rig.count && ok;++b)
                ok &= AV_V220WriteAtom(sb,b,ref[b]);

            // only the legs. Mixed keeps the GAME pose on every non-arm bone,
            // while root/spine01/spine02/neck are the neutral VR torso frame.
            // this restores jump/crouch/body animation without letting gameplay
            // animation steer either tracked arm.
            for(int b=0;b<avV16Rig.count && ok;++b){
                const bool inLeftArm =
                    b==avV16Rig.lUpper ||
                    AV_V259IsDescendantOf(b,avV16Rig.lUpper,parents,avV16Rig.count);
                const bool inRightArm =
                    b==avV16Rig.rUpper ||
                    AV_V259IsDescendantOf(b,avV16Rig.rUpper,parents,avV16Rig.count);
                // Resetting 0..3 to ref here would erase root inheritance.
                if(!inLeftArm && !inRightArm)
                    ok &= AV_V220WriteAtom(sb,b,avV16Saved[b]);
            }
        }
        if(ok){
            AV_V220V3 rootPos=AV_V220AtomPos(ref[0]);
            AV_V220V3 neckPos=AV_V220AtomPos(ref[3]);
            AV_V220V3 lShoulderBind=AV_V220AtomPos(ref[avV16Rig.lUpper]);
            AV_V220V3 rShoulderBind=AV_V220AtomPos(ref[avV16Rig.rUpper]);
            AV_V220V3 modelRight=AV_V220Norm(AV_V220Sub(rShoulderBind,lShoulderBind));
            AV_V220V3 modelUp=AV_V220Norm(AV_V220Sub(neckPos,rootPos));
            modelUp=AV_V220Norm(AV_V220Sub(modelUp,AV_V220Mul(modelRight,AV_V220Dot(modelUp,modelRight))));
            AV_V220V3 modelForward=AV_V220Norm(AV_V228Cross(modelRight,modelUp));

            // relationship, not from world/pawn motion and not from HMD roomscale.
            // Stock AHiT has no separate bip_head skinning anchor; bone 3 (bip_neck)
            // is the stable head anchor already used by HatVR's head suppression.
            //
            // Comparing (liveNeck-liveRoot) against (refNeck-refRoot) cancels the
            // onto modelForward/modelUp. modelRight (lateral) is intentionally
            // ignored so animation can never sway the VR camera left/right.
            if(avV16Rig.count>3){
                const AV_V220V3 liveRoot=AV_V220AtomPos(avV16Saved[0]);
                const AV_V220V3 liveNeck=AV_V220AtomPos(avV16Saved[3]);
                const AV_V220V3 refRel=AV_V220Sub(neckPos,rootPos);
                const AV_V220V3 liveRel=AV_V220Sub(liveNeck,liveRoot);
                const AV_V220V3 animDelta=AV_V220Sub(liveRel,refRel);

                float animForward=AV_V220Dot(animDelta,modelForward);
                float animUp=AV_V220Dot(animDelta,modelUp);

                // so the viewpoint follows torso translation instead of letting the
                // chest drift through the camera while walking. Vertical keeps the
                // wider rejection band to suppress ordinary step/head bob.
                auto deadzone=[](float v,float dz)->float{
                    if(v>dz) return v-dz;
                    if(v<-dz) return v+dz;
                    return 0.0f;
                };
                animForward=deadzone(animForward,3.0f);
                animUp=deadzone(animUp,6.0f);

                // keep this first test bounded. Extreme/corrupt custom poses should
                // not be allowed to throw the VR camera across the level.
                constexpr float kAnimCameraMaxUU=80.0f;
                animForward=(std::max)(-kAnimCameraMaxUU,(std::min)(kAnimCameraMaxUU,animForward));
                animUp=(std::max)(-kAnimCameraMaxUU,(std::min)(kAnimCameraMaxUU,animUp));

                g_hatVrAnimHeadCameraForwardUU=animForward;
                g_hatVrAnimHeadCameraUpUU=animUp;
                g_hatVrAnimHeadCameraOffsetValid=true;
            }else{
                g_hatVrAnimHeadCameraForwardUU=0.0f;
                g_hatVrAnimHeadCameraUpUU=0.0f;
                g_hatVrAnimHeadCameraOffsetValid=false;
            }

            auto mapTracking=[&](XrVector3f xr)->AV_V220V3{
                AV_V220V3 ue=AV_V220XrVectorToGame(xr);
                return AV_V220Add(AV_V220Add(AV_V220Mul(modelForward,ue.x),AV_V220Mul(modelRight,ue.y)),AV_V220Mul(modelUp,ue.z));
            };
            XrVector3f head=g_xrHeadPosition;
            if(g_xrViewsValidThisFrame){
                head.x=(g_xrViews[0].pose.position.x+g_xrViews[1].pose.position.x)*0.5f;
                head.y=(g_xrViews[0].pose.position.y+g_xrViews[1].pose.position.y)*0.5f;
                head.z=(g_xrViews[0].pose.position.z+g_xrViews[1].pose.position.z)*0.5f;
            }
            XrVector3f hd{head.x-g_xrTrackingOriginPosition.x,head.y-g_xrTrackingOriginPosition.y,head.z-g_xrTrackingOriginPosition.z};
            bodyOffset=mapTracking(hd);
            const AV_V220V3 v94PhysicalRoomscale=bodyOffset;
            avV94PhysicalRoomscale=v94PhysicalRoomscale;
            g_avV119RootRoomscale[0]=v94PhysicalRoomscale.x;
            g_avV119RootRoomscale[1]=v94PhysicalRoomscale.y;
            g_avV119RootRoomscale[2]=v94PhysicalRoomscale.z;
            g_avV119RootRoomscaleValid=true;

            // owns body roomscale through LocalAtoms[0]. Do not subtract the old
            // published delta and do not add the current delta per bone.

            AV_V220V3 neckToEye=AV_V220Mul(modelUp,g_avV256NeckToEyeUU);
            float neckHeight=AV_V220Dot(AV_V220Sub(neckPos,rootPos),modelUp);
            AV_V220V3 anchor=AV_V220Add(AV_V220Add(rootPos,AV_V220Mul(modelUp,neckHeight)),neckToEye);
            AV_V220V3 desiredNeck=AV_V220Sub(AV_V220Add(anchor,bodyOffset),neckToEye);
            // roomscale, so the VR shoulder roots must receive that SAME delta.
            // cancelling roomscale back to ~zero and leaving the arms behind.
            // Controller targets stay tracking-origin-relative, matching the
            // proven Mixed Hat Kid path.
            AV_V220V3 bodyCorrection=v94PhysicalRoomscale;

            auto solve=[&](bool left)->bool{
                int ui=left?avV16Rig.lUpper:avV16Rig.rUpper;
                int fi=left?avV16Rig.lFore:avV16Rig.rFore;
                int hi=left?avV16Rig.lHand:avV16Rig.rHand;
                int ci=left?0:1;
                if(!g_controllerGripPoseValid[ci]) return true;
                // roomscale-shifted animated body. Start IK from that live shoulder
                // so dives/leans carry the arm root with the torso.
                //
                // keep ref only for immutable bind calibration (lengths/axes and
                // the solver's authored arm atom basis); it no longer determines
                // where the shoulder lives in component space.
                AV_V27Atom48 upper=ref[ui],fore=ref[fi],hand=ref[hi];
                AV_V220V3 sh=AV_V220AtomPos(avV16Saved[ui]);
                const AV_V220V3 refSh=AV_V220AtomPos(ref[ui]);
                const AV_V220V3 refElbow=AV_V220AtomPos(ref[fi]);
                const AV_V220V3 refHand=AV_V220AtomPos(ref[hi]);
                float l1=AV_V220Len(AV_V220Sub(refElbow,refSh));
                float l2=AV_V220Len(AV_V220Sub(refHand,refElbow));
                AV_V220V3 oe=AV_V220Add(sh,AV_V220Sub(refElbow,refSh));
                AV_V220V3 oh=AV_V220Add(oe,AV_V220Sub(refHand,refElbow));
                XrPosef grip=g_controllerGripLocations[ci].pose;
                XrVector3f rel{grip.position.x-g_xrTrackingOriginPosition.x,grip.position.y-g_xrTrackingOriginPosition.y,grip.position.z-g_xrTrackingOriginPosition.z};
                // displacement used by the camera. Do not use the raw neck delta:
                AV_V220V3 cameraAnimOffset=AV_V220Add(
                    AV_V220Mul(modelForward,g_hatVrAnimHeadCameraForwardUU),
                    AV_V220Mul(modelUp,g_hatVrAnimHeadCameraUpUU));
                AV_V220V3 tgt=AV_V220Add(
                    AV_V220Add(anchor,mapTracking(rel)),
                    cameraAnimOffset);
                // the forearm bend plane can respond when the palm turns inward.
                // Final wrist orientation below uses these exact same values.
                XrQuaternionf gripNow=AV_V220QNorm(grip.orientation);
                float gripGameM[9]{};
                AV_V222QuatToGame3x3(gripNow,gripGameM);
                XrQuaternionf gripGameQ=AV_V222Mat3ToQuat(gripGameM);
                const XrQuaternionf measuredCorrection = left
                    ? XrQuaternionf{0.40501651f,-0.09978920f,0.36195150f,0.83366351f}
                    : XrQuaternionf{-0.28435887f,-0.00978553f,-0.19563290f,0.93849456f};
                gripGameQ=AV_V220QNorm(AV_V220QMul(gripGameQ,measuredCorrection));
                const float hs=0.7071067811865475f;
                XrQuaternionf gripToHandGame{0.0f,hs,0.0f,hs};
                XrQuaternionf correctedGripGameQ=AV_V220QNorm(AV_V220QMul(
                    AV_V220QMul(gripToHandGame,gripGameQ),
                    AV_V220QConj(gripToHandGame)));
                XrQuaternionf masterRootQ=AV_V220QNorm({
                    ref[0].rotation[0],ref[0].rotation[1],
                    ref[0].rotation[2],ref[0].rotation[3]});
                XrQuaternionf gripComponentQ=AV_V220QNorm(AV_V220QMul(
                    AV_V220QMul(masterRootQ,correctedGripGameQ),
                    AV_V220QConj(masterRootQ)));

                AV_V220V3 pole=AV_V220Add(AV_V220Mul(modelRight,left?-1.f:1.f),AV_V220Add(AV_V220Mul(modelUp,-1.f),AV_V220Mul(modelForward,-.5f)));

                // handled as actual axial rotation of the forearm bone AFTER the
                // positional two-bone solve; it must not move the IK elbow target.
                AV_V290IKResult ik=AV_V290SolveTwoBoneIK(sh,tgt,pole,l1,l2);
                if(!ik.valid) return false;
                AV_V220V3 ad1=AV_V220Sub(oe,sh),ad2=AV_V220Sub(oh,oe);
                int ax1=AV_V131FindBindLongitudinalAxis(ref[ui],ad1),ax2=AV_V131FindBindLongitudinalAxis(ref[fi],ad2);
                AV_V220RotateAtomToward(upper,ad1,AV_V220Sub(ik.elbow,sh));
                AV_V220RotateAtomToward(fore,ad2,AV_V220Sub(ik.hand,ik.elbow));
                AV_V131StretchAtomAxis(upper,ax1,ik.upperLen/l1); AV_V131StretchAtomAxis(fore,ax2,ik.lowerLen/l2);

                // it deliberately leaves the roll about elbow->hand unconstrained.
                // Build the same absolute controller-driven hand target used below,
                // compare it with the already-aimed forearm, and retain ONLY the
                // relative rotation about the solved forearm axis. Apply that twist
                // directly to the Elbow/forearm atom. This changes bone rotation,
                // not elbow position.
                const XrQuaternionf v131BindHandQ=AV_V220QNorm({
                    ref[hi].rotation[0],ref[hi].rotation[1],
                    ref[hi].rotation[2],ref[hi].rotation[3]});
                const XrQuaternionf v131HandTargetQ=AV_V220QNorm(
                    AV_V220QMul(gripComponentQ,v131BindHandQ));
                XrQuaternionf v131ForeQ=AV_V220QNorm({
                    fore.rotation[0],fore.rotation[1],
                    fore.rotation[2],fore.rotation[3]});
                const AV_V220V3 v131ForeAxis=AV_V220Norm(
                    AV_V220Sub(ik.hand,ik.elbow));
                const XrQuaternionf v131Relative=AV_V220QNorm(
                    AV_V220QMul(v131HandTargetQ,AV_V220QConj(v131ForeQ)));
                const float v131Projected=
                    v131Relative.x*v131ForeAxis.x +
                    v131Relative.y*v131ForeAxis.y +
                    v131Relative.z*v131ForeAxis.z;
                const float v131TwistNorm=sqrtf(
                    v131Projected*v131Projected +
                    v131Relative.w*v131Relative.w);
                if(v131TwistNorm>1e-5f){
                    XrQuaternionf v131TwistQ={
                        v131ForeAxis.x*v131Projected/v131TwistNorm,
                        v131ForeAxis.y*v131Projected/v131TwistNorm,
                        v131ForeAxis.z*v131Projected/v131TwistNorm,
                        v131Relative.w/v131TwistNorm};
                    // Give the forearm half of the extracted axial twist while the
                    // hand continues to use the full controller-driven orientation.
                    // for a unit twist quaternion, halving the rotation is the
                    // normalized midpoint between identity and the twist quaternion.
                    if(v131TwistQ.w<0.0f){
                        v131TwistQ.x=-v131TwistQ.x; v131TwistQ.y=-v131TwistQ.y;
                        v131TwistQ.z=-v131TwistQ.z; v131TwistQ.w=-v131TwistQ.w;
                    }
                    XrQuaternionf v132HalfTwistQ=AV_V220QNorm({
                        v131TwistQ.x,v131TwistQ.y,v131TwistQ.z,v131TwistQ.w+1.0f});
                    v131ForeQ=AV_V220QNorm(AV_V220QMul(v132HalfTwistQ,v131ForeQ));
                    fore.rotation[0]=v131ForeQ.x; fore.rotation[1]=v131ForeQ.y;
                    fore.rotation[2]=v131ForeQ.z; fore.rotation[3]=v131ForeQ.w;
                }
                //
                // Hat Kid: the selected head/master owns the pose and UE3 maps it
                // into body/legs through ParentAnimComponent + BoneMap.  The old
                // nonstandard path solved the named master arm positions but then
                // deliberately restored the GAME'S live wrist rotation here.  That
                // exactly explains the observed "animated hand that never rotates
                // to the VR controller" behavior.
                //
                // Use the same absolute OpenXR->game->component wrist pipeline as
                // the proven mixed Hat Kid path, but apply it to the named wrist on
                // whichever MASTER skeleton is active.  Child outfit components are
                // never solved directly; UE3's BoneMap remains the propagation path.
                // forearm now receives only that target's axial twist component.
                XrQuaternionf handQ=v131HandTargetQ;
                hand.rotation[0]=handQ.x; hand.rotation[1]=handQ.y;
                hand.rotation[2]=handQ.z; hand.rotation[3]=handQ.w;

                // tooling can inspect Bow/HCR through the same fields as Hat Kid.
                g_avV250FinalHandQ[ci]=handQ;
                g_avV250FinalHandValid[ci]=true;

                AV_V220SetAtomPos(upper,sh);AV_V220SetAtomPos(fore,ik.elbow);AV_V220SetAtomPos(hand,ik.hand);
                if(!AV_V220WriteAtom(sb,ui,upper)||!AV_V220WriteAtom(sb,fi,fore)||!AV_V220WriteAtom(sb,hi,hand)) return false;

                //
                // saved component-space descendants beneath the solved wrist.
                // we have already tried deriving "live locals" back out of SpaceBases
                // LocalAtoms.Data and those atoms are genuinely parent-local.
                //
                // keep upper/forearm accessory behavior on the old reference carry,
                // but for every descendant of the wrist, compose the engine's live
                // LocalAtom directly beneath the newly solved parent, in topological
                // bone order.  This preserves animation without treating saved
                // component-space finger atoms as an authoritative hierarchy.
                AV_V27TArray64 v79LocalAtoms{};
                const bool v79HaveLocalAtoms =
                    FP_ReadMemory((const void*)(poseComp+0x328),
                                  &v79LocalAtoms,sizeof(v79LocalAtoms)) &&
                    FP_V111PlausiblePtr(v79LocalAtoms.Data) &&
                    v79LocalAtoms.Count>=avV16Rig.count &&
                    v79LocalAtoms.Count<=512;

                auto v79Rot=[](XrQuaternionf q,AV_V220V3 v)->AV_V220V3{
                    XrQuaternionf p{v.x,v.y,v.z,0.0f};
                    XrQuaternionf r=AV_V220QMul(
                        AV_V220QMul(q,p),AV_V220QConj(q));
                    return {r.x,r.y,r.z};
                };

                for(int b=0;b<avV16Rig.count;++b){
                    if(b==ui || b==fi || b==hi) continue;
                    if(!AV_V259IsDescendantOf(b,ui,parents,avV16Rig.count)) continue;

                    if(AV_V259IsDescendantOf(b,hi,parents,avV16Rig.count) &&
                       avV16SavedValid)
                    {
                        // in principle. Do NOT rebuild LocalAtoms here. Take the
                        // complete CURRENT animated component-space descendant pose
                        // and rigidly carry it from the old animated wrist into the
                        // solved VR wrist. This is what Mixed uses for fingers,
                        // ItemPalm and any other authored hand descendants.
                        // one pose generation. On partial SpaceBases refreshes the
                        // wrist can still be HatVR's published atom while a finger
                        // atom has already changed (or vice versa), producing a
                        // one-frame bad rigid-carry delta. If the saved wrist is
                        // exactly our previous published wrist, use the matching
                        // published descendant as well.
                        const bool v112PublishedWrist =
                            g_avV97PublishedValid &&
                            g_avV97PublishedOwner==poseComp &&
                            g_avV97PublishedCount==avV16Rig.count &&
                            memcmp(&avV16Saved[hi],&g_avV97Published[hi],
                                   sizeof(AV_V27Atom48))==0;
                        const AV_V27Atom48& driverOld = v112PublishedWrist
                            ? g_avV97Published[hi] : avV16Saved[hi];
                        AV_V27Atom48 child = v112PublishedWrist
                            ? g_avV97Published[b] : avV16Saved[b];

                        XrQuaternionf oldQ=AV_V220QNorm({
                            driverOld.rotation[0],driverOld.rotation[1],
                            driverOld.rotation[2],driverOld.rotation[3]});
                        XrQuaternionf newQ=AV_V220QNorm({
                            hand.rotation[0],hand.rotation[1],
                            hand.rotation[2],hand.rotation[3]});
                        XrQuaternionf deltaQ=AV_V220QNorm(
                            AV_V220QMul(newQ,AV_V220QConj(oldQ)));

                        AV_V220V3 rel=AV_V220Sub(
                            AV_V220AtomPos(child),AV_V220AtomPos(driverOld));
                        AV_V220SetAtomPos(child,AV_V220Add(
                            AV_V220AtomPos(hand),v79Rot(deltaQ,rel)));

                        XrQuaternionf cq=AV_V220QNorm({
                            child.rotation[0],child.rotation[1],
                            child.rotation[2],child.rotation[3]});
                        cq=AV_V220QNorm(AV_V220QMul(deltaQ,cq));
                        child.rotation[0]=cq.x; child.rotation[1]=cq.y;
                        child.rotation[2]=cq.z; child.rotation[3]=cq.w;

                        if(!AV_V220WriteAtom(sb,b,child)) return false;
                        continue;
                    }

                    // Upper/forearm descendants are not the finger bug isolated by
                    int driver=ui;
                    AV_V27Atom48 driverNew=upper;
                    if(AV_V259IsDescendantOf(b,hi,parents,avV16Rig.count)){
                        driver=hi; driverNew=hand;
                    }else if(AV_V259IsDescendantOf(b,fi,parents,avV16Rig.count)){
                        driver=fi; driverNew=fore;
                    }

                    const AV_V27Atom48& driverOld=ref[driver];
                    AV_V27Atom48 child=ref[b];
                    XrQuaternionf oldQ=AV_V220QNorm({
                        driverOld.rotation[0],driverOld.rotation[1],
                        driverOld.rotation[2],driverOld.rotation[3]});
                    XrQuaternionf newQ=AV_V220QNorm({
                        driverNew.rotation[0],driverNew.rotation[1],
                        driverNew.rotation[2],driverNew.rotation[3]});
                    XrQuaternionf deltaQ=AV_V220QNorm(
                        AV_V220QMul(newQ,AV_V220QConj(oldQ)));
                    AV_V220V3 rel=AV_V220Sub(
                        AV_V220AtomPos(child),AV_V220AtomPos(driverOld));

                    // outward with forearm extension. The hand is already the IK
                    // end effector, so exclude it. Scale only the component along
                    // the authored forearm axis; keep sideways sleeve offsets intact.
                    if(driver==fi && parents[b]==fi && b!=hi){
                        const float foreStretch=(l2>0.001f)?(ik.lowerLen/l2):1.0f;
                        if(foreStretch>1.0001f && isfinite(foreStretch)){
                            const AV_V220V3 foreAxis=AV_V220Norm(ad2);
                            const float along=AV_V220Dot(rel,foreAxis);
                            rel=AV_V220Add(rel,AV_V220Mul(
                                foreAxis,along*(foreStretch-1.0f)));
                        }
                    }
                    AV_V220SetAtomPos(child,AV_V220Add(
                        AV_V220AtomPos(driverNew),v79Rot(deltaQ,rel)));
                    XrQuaternionf cq=AV_V220QNorm({
                        child.rotation[0],child.rotation[1],
                        child.rotation[2],child.rotation[3]});
                    cq=AV_V220QNorm(AV_V220QMul(deltaQ,cq));
                    child.rotation[0]=cq.x; child.rotation[1]=cq.y;
                    child.rotation[2]=cq.z; child.rotation[3]=cq.w;
                    if(!AV_V220WriteAtom(sb,b,child)) return false;
                }

                // control showed that native ParentAnimComponent + ParentBoneMap
                // consumption already carries the correctly reconstructed master pose
                // into reorganized render components, including held-item attachment.
                return true;
            };
            ok=solve(false)&&solve(true);
            if(ok){
                changed=true;

                // AHiT's player meshes use bip_neck as the head skinning anchor;
                // collapse that authored neck subtree to scale zero at the neck.
                // do this after arm solving so it cannot alter shoulder/hand IK.
                const int v93Neck=AV_V14FindBoneIndex(avV16Rig.mesh,"bip_neck");
                if(v93Neck>=0 && v93Neck<avV16Rig.count){
                    AV_V27Atom48 v93NeckAnchor{};
                    if(AV_V220ReadAtom(sb,v93Neck,v93NeckAnchor)){
                        for(int b=0;b<avV16Rig.count;++b){
                            if(b!=v93Neck &&
                               !AV_V259IsDescendantOf(b,v93Neck,parents,avV16Rig.count))
                                continue;
                            // do not collapse either tracked arm if a custom rig
                            // happens to parent shoulders below neck.
                            if(b==avV16Rig.lUpper || b==avV16Rig.rUpper ||
                               AV_V259IsDescendantOf(b,avV16Rig.lUpper,parents,avV16Rig.count) ||
                               AV_V259IsDescendantOf(b,avV16Rig.rUpper,parents,avV16Rig.count))
                                continue;
                            AV_V27Atom48 a{};
                            if(!AV_V220ReadAtom(sb,b,a)) continue;
                            a.translation[0]=v93NeckAnchor.translation[0];
                            a.translation[1]=v93NeckAnchor.translation[1];
                            a.translation[2]=v93NeckAnchor.translation[2];
                            a.scale[0]=0.0f; a.scale[1]=0.0f; a.scale[2]=0.0f;
                            AV_V220WriteAtom(sb,b,a);
                        }
                    }
                }

                // BuildRefToLocal consumes it. Subsequent requests in this SAME
                // present frame can replay it without repeating IK/ref-pose/local
                // hierarchy reconstruction.
                bool v84CaptureOk=avV16Rig.count>0 && avV16Rig.count<=128;
                if(v84CaptureOk){
                    __try{ memcpy(g_avV84Solved,(const void*)sb.Data,sizeof(AV_V27Atom48)*(size_t)avV16Rig.count); }
                    __except(EXCEPTION_EXECUTE_HANDLER){ v84CaptureOk=false; }
                }
                if(v84CaptureOk){
                    g_avV84CacheFrame=(unsigned long long)g_presentFrameNumber;
                    g_avV84CacheOwner=poseComp;
                    g_avV84CacheCount=avV16Rig.count;
                    g_avV84CacheValid=true;
                    ++g_avV84Solves;

                    // DIFFERENT cache for persistent attachment-visible
                    // same-frame render-performance system.
                    memcpy(g_avV97Published,g_avV84Solved,sizeof(AV_V27Atom48)*(size_t)avV16Rig.count);
                    g_avV97PublishedOwner=poseComp;
                    g_avV97PublishedCount=avV16Rig.count;
                    g_avV97PublishedValid=true;
                    g_avV104PublishedRoomscale[0]=avV94PhysicalRoomscale.x;
                    g_avV104PublishedRoomscale[1]=avV94PhysicalRoomscale.y;
                    g_avV104PublishedRoomscale[2]=avV94PhysicalRoomscale.z;
                    g_avV104PublishedRoomscaleValid=true;
                }else{
                    g_avV84CacheValid=false;
                    g_avV97PublishedValid=false;
                    g_avV104PublishedRoomscaleValid=false;
                }

                // BuildRefToLocal consumes this render-only pose immediately, then the
                // original SpaceBases are restored below. This is the exact lifetime
            }
        }
    }

QueryPerformanceCounter(&avPerfT2);
    if(g_avV215OriginalBuildRefToLocal)
        g_avV215OriginalBuildRefToLocal(output,component,lodIndex,extra);
    QueryPerformanceCounter(&avPerfT3);

    if(avV10PatchedSlaveL2W){
        __try{ memcpy((void*)(comp+AV_V231_LOCAL_TO_WORLD_OFFSET),avV10SavedL2W,sizeof(avV10SavedL2W)); }
        __except(EXCEPTION_EXECUTE_HANDLER){}
    }

    // keep the solved authoritative master live for later engine consumers and
    // invalidate the mesh object exactly once through the universal path.
    if(active && avV16Nonstandard && g_avV97PublishedValid &&
       g_avV97PublishedOwner==poseComp && g_avV97PublishedCount>0 &&
       g_avV97PublishedCount<=128)
    {
        AV_V27TArray64 v97Sb{};
        bool v97Wrote=AV_V27GetSpaceBases(poseComp,v97Sb) &&
                      v97Sb.Count>=g_avV97PublishedCount;
        if(v97Wrote){
            __try{ memcpy((void*)v97Sb.Data,g_avV97Published,
                          sizeof(AV_V27Atom48)*(size_t)g_avV97PublishedCount); }
            __except(EXCEPTION_EXECUTE_HANDLER){ v97Wrote=false; }
        }

        uint32_t v97Before=0,v97After=0;
        const bool v97Forced=v97Wrote &&
            AV_V240ForceRenderRefresh(poseComp,v97Before,v97After);

        const ULONGLONG v97Now=GetTickCount64();
        if(v97Now-g_avV97LastPublishLog>=500){
            g_avV97LastPublishLog=v97Now;
            char v97Line[512]{};
            sprintf_s(v97Line,sizeof(v97Line),
                "AV_V103_UNIVERSAL_PUBLISH comp=%p wrote=%d bones=%d forceOffset=0x%X forceWord=%08X->%08X forced=%d frame=%llu\\n",
                (void*)poseComp,v97Wrote?1:0,g_avV97PublishedCount,
                g_avV240ForceMeshObjectUpdateOffset,v97Before,v97After,
                v97Forced?1:0,(unsigned long long)g_presentFrameNumber);
            FP_Log(v97Line);
        }
    }

// keep the game's animation state pristine. This restores BOTH the VR pose
    // matrices have been generated.
    if(changed && !avV16Nonstandard){
        if(savedAllValid){
            for(int b=0;b<70;++b) AV_V220WriteAtom(sb,b,savedAll[b]);
        }else{
            for(int i=0;i<15;++i){int bone=(i<3)?32+i:35+(i-3);AV_V220WriteAtom(sb,bone,saved[i]);}
        }
    }

    QueryPerformanceCounter(&avPerfT4);

    LARGE_INTEGER avPerfT5{};
    QueryPerformanceCounter(&avPerfT5);
    const double setupMs=AV_PerfV2Ms(avPerfT0,avPerfT1);
    const double preOriginalMs=AV_PerfV2Ms(avPerfT1,avPerfT2);
    const double originalMs=AV_PerfV2Ms(avPerfT2,avPerfT3);
    const double restoreMs=AV_PerfV2Ms(avPerfT3,avPerfT4);
    const double postMs=AV_PerfV2Ms(avPerfT4,avPerfT5);

    ++g_avPerfV2.calls;
    if(active) ++g_avPerfV2.activeCalls;
    g_avPerfV2.setupMs += setupMs;
    if(active) g_avPerfV2.hatvrActiveMs += preOriginalMs;
    else       g_avPerfV2.hatvrInactiveMs += preOriginalMs;
    g_avPerfV2.originalMs += originalMs;
    g_avPerfV2.restoreMs += restoreMs;
    g_avPerfV2.postMs += postMs;

    const ULONGLONG avPerfNow=GetTickCount64();
    if(!g_avPerfV2LastReport) g_avPerfV2LastReport=avPerfNow;
    if(avPerfNow-g_avPerfV2LastReport>=1000){
        const uint64_t inactiveCalls=g_avPerfV2.calls-g_avPerfV2.activeCalls;
        const double hatvrMs=g_avPerfV2.setupMs+g_avPerfV2.hatvrActiveMs+
                             g_avPerfV2.hatvrInactiveMs+g_avPerfV2.restoreMs+
                             g_avPerfV2.postMs;
        char avPerfLine[768]{};
        sprintf_s(avPerfLine,sizeof(avPerfLine),
            "AV_PERF_V2 calls=%llu active=%llu inactive=%llu "
            "hatvr=%.3fms setup=%.3fms activeWork=%.3fms inactiveWork=%.3fms "
            "restore=%.3fms post=%.3fms originalUE3=%.3fms "
            "avgHatVR=%.4fms/call avgActiveWork=%.4fms/activeCall changed=%d\n",
            (unsigned long long)g_avPerfV2.calls,
            (unsigned long long)g_avPerfV2.activeCalls,
            (unsigned long long)inactiveCalls,
            hatvrMs,g_avPerfV2.setupMs,g_avPerfV2.hatvrActiveMs,g_avPerfV2.hatvrInactiveMs,
            g_avPerfV2.restoreMs,g_avPerfV2.postMs,g_avPerfV2.originalMs,
            g_avPerfV2.calls?hatvrMs/(double)g_avPerfV2.calls:0.0,
            g_avPerfV2.activeCalls?g_avPerfV2.hatvrActiveMs/(double)g_avPerfV2.activeCalls:0.0,
            changed?1:0);
        FP_Log(avPerfLine);
        g_avPerfV2={};
        g_avPerfV2LastReport=avPerfNow;
    }
}

// Reverse engineered from HatinTimeGame.exe:
//   RVA 0x4B85C0 builds the current base transform for an AActor.
//   Actor+0xE0  = Base
//   Actor+0x1D8 = BaseSkelComponent
//   Actor+0x1E0 = BaseBoneName (FName)
// When a skeletal base/bone is present it resolves the bone through RVA 0x69E5D0
// and obtains its matrix through RVA 0x69B340. Hat_Weapon attaches the umbrella
// once with SetBase(P,, Parent, Bone), so this is the later consumer that actually
// moves the based weapon actor each frame.
struct AV_V88Matrix { float m[16]; };
using AV_V88BaseTransformFn = AV_V88Matrix* (__fastcall*)(AV_V88Matrix*, uintptr_t);
using AV_V88GetBoneIndexFn  = int (__fastcall*)(uintptr_t, const void*);
using AV_V88GetBoneMatrixFn = AV_V88Matrix* (__fastcall*)(uintptr_t, AV_V88Matrix*, int);
static AV_V88BaseTransformFn g_avV88OriginalBaseTransform=nullptr;
static AV_V88GetBoneIndexFn  g_avV88GetBoneIndex=nullptr;
static AV_V88GetBoneMatrixFn g_avV88GetBoneMatrix=nullptr;
static bool g_avV88BaseHookInstalled=false;
static ULONGLONG g_avV88LastLog=0;
static uint64_t g_avV88Queries=0,g_avV88Overrides=0;

static AV_V88Matrix* __fastcall AV_V88HookedBaseTransform(AV_V88Matrix* out, uintptr_t actor)
{
    AV_V88Matrix* ret=g_avV88OriginalBaseTransform?g_avV88OriginalBaseTransform(out,actor):out;
    ++g_avV88Queries;
    if(!g_fpV1Enabled || !g_avV29ProbeEnabled || !FP_V111PlausiblePtr(actor) ||
       !g_avV88GetBoneIndex || !g_avV88GetBoneMatrix) return ret;

    uintptr_t baseActor=0,baseComp=0;
    FP_ReadMemory((const void*)(actor+0xE0),&baseActor,sizeof(baseActor));
    if(!FP_ReadMemory((const void*)(actor+0x1D8),&baseComp,sizeof(baseComp)) || !FP_V111PlausiblePtr(baseComp)) return ret;

    // not prove RVA 0x4B85C0 was unused. This tells us exactly which component SetBase stored.
    {
        static ULONGLONG s_v89RawLast=0; static uintptr_t s_v89LastActor=0,s_v89LastComp=0;
        const ULONGLONG now=GetTickCount64();
        if(actor!=s_v89LastActor || baseComp!=s_v89LastComp || now-s_v89RawLast>=500){
            s_v89RawLast=now; s_v89LastActor=actor; s_v89LastComp=baseComp;
            char an[128]="?",bn[128]="?",cn[128]="?";
            AV_V1ReadRawObjectName(actor,an,sizeof(an));
            if(FP_V111PlausiblePtr(baseActor)) AV_V1ReadRawObjectName(baseActor,bn,sizeof(bn));
            AV_V1ReadRawObjectName(baseComp,cn,sizeof(cn));
            uintptr_t pm=0; FP_ReadMemory((const void*)(baseComp+0x37C),&pm,sizeof(pm));
            int bi=-1; __try{ bi=g_avV88GetBoneIndex(baseComp,(const void*)(actor+0x1E0)); } __except(EXCEPTION_EXECUTE_HANDLER){ bi=-1; }
            char l[640]{}; sprintf_s(l,sizeof(l),
                "AV_V89_BASE_RAW actor=%p name=\"%s\" baseActor=%p baseName=\"%s\" baseComp=%p comp=\"%s\" parentAnim=%p boneIndex=%d queries=%llu\n",
                (void*)actor,an,(void*)baseActor,bn,(void*)baseComp,cn,(void*)pm,bi,(unsigned long long)g_avV88Queries); FP_Log(l);
        }
    }

    uintptr_t master=0;
    if(!FP_ReadMemory((const void*)(baseComp+0x37C),&master,sizeof(master)) || !FP_V111PlausiblePtr(master)) return ret;

    // Resolve the exact BaseBoneName selected by SetBase. 0x69E5D0 is the same
    // native bone-name resolver used by the original 0x4B85C0 function.
    int childBone=-1;
    __try{ childBone=g_avV88GetBoneIndex(baseComp,(const void*)(actor+0x1E0)); }
    __except(EXCEPTION_EXECUTE_HANDLER){ childBone=-1; }
    if(childBone<0 || childBone>511) return ret;

    uintptr_t mapData=0; int mapCount=0;
    FP_ReadMemory((const void*)(baseComp+0x384),&mapData,sizeof(mapData));
    FP_ReadMemory((const void*)(baseComp+0x38C),&mapCount,sizeof(mapCount));
    if(!FP_V111PlausiblePtr(mapData) || childBone>=mapCount || mapCount<=0 || mapCount>512) return ret;

    int32_t mapped=-1;
    if(!FP_ReadMemory((const void*)(mapData+(uintptr_t)childBone*sizeof(int32_t)),&mapped,sizeof(mapped)) || mapped<0 || mapped>511) return ret;

    AV_V88Matrix masterBone{};
    AV_V88Matrix* mr=nullptr;
    __try{ mr=g_avV88GetBoneMatrix(master,&masterBone,mapped); }
    __except(EXCEPTION_EXECUTE_HANDLER){ mr=nullptr; }
    if(!mr) return ret;

    // Replace UE3's slave-bone base transform with the corresponding master-bone
    // transform at the actual based-actor consumer. The actor's existing relative
    // transform remains untouched, preserving the authored Umbrella socket offset.
    __try{ memcpy(out,mr,sizeof(*out)); ret=out; ++g_avV88Overrides; }
    __except(EXCEPTION_EXECUTE_HANDLER){ return ret; }

    const ULONGLONG now=GetTickCount64();
    if(now-g_avV88LastLog>=500){
        g_avV88LastLog=now;
        char compName[128]="?",masterName[128]="?",actorName[128]="?";
        AV_V1ReadRawObjectName(baseComp,compName,sizeof(compName));
        AV_V1ReadRawObjectName(master,masterName,sizeof(masterName));
        AV_V1ReadRawObjectName(actor,actorName,sizeof(actorName));
        char l[640]{};
        sprintf_s(l,sizeof(l),
            "AV_V88_BASE_CONSUMER actor=%p name=\"%s\" baseComp=%p comp=\"%s\" master=%p masterName=\"%s\" childBone=%d mappedMaster=%d queries=%llu overrides=%llu pos=[%.2f %.2f %.2f]\n",
            (void*)actor,actorName,(void*)baseComp,compName,(void*)master,masterName,childBone,mapped,
            (unsigned long long)g_avV88Queries,(unsigned long long)g_avV88Overrides,
            out->m[12],out->m[13],out->m[14]);
        FP_Log(l);
    }
    return ret;
}

static bool AV_V88InstallBaseConsumerHook()
{
    if(g_avV88BaseHookInstalled) return true;
    const uintptr_t exe=(uintptr_t)GetModuleHandleW(nullptr);
    if(!exe) return false;
    constexpr uintptr_t kBaseTransformRva=0x4B85C0;
    constexpr uintptr_t kGetBoneIndexRva=0x69E5D0;
    constexpr uintptr_t kGetBoneMatrixRva=0x69B340;
    g_avV88GetBoneIndex=(AV_V88GetBoneIndexFn)(exe+kGetBoneIndexRva);
    g_avV88GetBoneMatrix=(AV_V88GetBoneMatrixFn)(exe+kGetBoneMatrixRva);
    void* target=(void*)(exe+kBaseTransformRva);
    MH_STATUS st=MH_CreateHook(target,&AV_V88HookedBaseTransform,reinterpret_cast<void**>(&g_avV88OriginalBaseTransform));
    if(st!=MH_OK && st!=MH_ERROR_ALREADY_CREATED){
        char l[256]{}; sprintf_s(l,sizeof(l),"AV_V88_BASE_HOOK_CREATE_FAIL target=%p status=%d\n",target,(int)st); FP_Log(l); return false;
    }
    st=MH_EnableHook(target);
    if(st!=MH_OK && st!=MH_ERROR_ENABLED){
        char l[256]{}; sprintf_s(l,sizeof(l),"AV_V88_BASE_HOOK_ENABLE_FAIL target=%p status=%d\n",target,(int)st); FP_Log(l); return false;
    }
    g_avV88BaseHookInstalled=true;
    char l[384]{}; sprintf_s(l,sizeof(l),
        "AV_V88_BASE_HOOK_INSTALLED target=%p rva=0x%llX getBoneIndex=%p getBoneMatrix=%p -- based skeletal attachments consume mapped master bone transforms\n",
        target,(unsigned long long)kBaseTransformRva,(void*)g_avV88GetBoneIndex,(void*)g_avV88GetBoneMatrix); FP_Log(l);
    return true;
}

