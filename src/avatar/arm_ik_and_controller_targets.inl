//
// edit the 0x30-byte SpaceBases atoms before 0x6CE6E0 builds ReferenceToLocal,
// then restore them afterward. fingers keep their normal animation and move with the wrist.
static bool AV_V215IsHatKidFamily(uintptr_t comp)
{
    if(!g_fpV1Enabled || !FP_V111PlausiblePtr(comp) ||
       !FP_V111PlausiblePtr(g_avV27TargetComponent)) return false;
    if(comp==g_avV27TargetComponent) return true;
    return AV_V211ReadParentAnim(comp)==g_avV27TargetComponent;
}

struct AV_V220V3 { float x,y,z; };
static AV_V220V3 AV_V220Add(AV_V220V3 a,AV_V220V3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
static AV_V220V3 AV_V220Sub(AV_V220V3 a,AV_V220V3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
static AV_V220V3 AV_V220Mul(AV_V220V3 a,float s){return {a.x*s,a.y*s,a.z*s};}
static float AV_V220Dot(AV_V220V3 a,AV_V220V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static AV_V220V3 AV_V220Cross(AV_V220V3 a,AV_V220V3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static float AV_V220Len(AV_V220V3 a){return sqrtf(AV_V220Dot(a,a));}
static AV_V220V3 AV_V220Norm(AV_V220V3 a){float l=AV_V220Len(a);return l>1e-5f?AV_V220Mul(a,1.0f/l):AV_V220V3{1,0,0};}
static AV_V220V3 AV_V220AtomPos(const AV_V27Atom48& a){return {a.translation[0],a.translation[1],a.translation[2]};}

static void AV_V220SetAtomPos(AV_V27Atom48& a,AV_V220V3 p){a.translation[0]=p.x;a.translation[1]=p.y;a.translation[2]=p.z;}

static XrQuaternionf AV_V220QConj(const XrQuaternionf& q){return {-q.x,-q.y,-q.z,q.w};}
static XrQuaternionf AV_V220QMul(const XrQuaternionf& a,const XrQuaternionf& b){
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
            a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
            a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
static XrQuaternionf AV_V220QNorm(XrQuaternionf q){
    float l=sqrtf(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    if(l<1e-5f)return {0,0,0,1}; q.x/=l;q.y/=l;q.z/=l;q.w/=l;return q;
}

static XrQuaternionf g_avV250FinalHandQ[2]{{0,0,0,1},{0,0,0,1}};
static bool g_avV250FinalHandValid[2]{false,false};

static AV_V220V3 AV_V250RotateVec(const XrQuaternionf& q,AV_V220V3 v)
{
    XrQuaternionf p{v.x,v.y,v.z,0.0f};
    XrQuaternionf r=AV_V220QMul(AV_V220QMul(q,p),AV_V220QConj(q));
    return {r.x,r.y,r.z};
}

static XrQuaternionf AV_V220FromTo(AV_V220V3 from,AV_V220V3 to){
    from=AV_V220Norm(from);to=AV_V220Norm(to);
    float d=AV_V220Dot(from,to);
    if(d>0.9999f)return {0,0,0,1};
    if(d<-0.9999f){
        AV_V220V3 axis=AV_V220Cross(from,{0,0,1});
        if(AV_V220Len(axis)<1e-4f)axis=AV_V220Cross(from,{0,1,0});
        axis=AV_V220Norm(axis);return {axis.x,axis.y,axis.z,0};
    }
    AV_V220V3 c=AV_V220Cross(from,to);
    return AV_V220QNorm({c.x,c.y,c.z,1.0f+d});
}
static void AV_V220RotateAtomToward(AV_V27Atom48& atom,AV_V220V3 oldDir,AV_V220V3 newDir){
    XrQuaternionf qOld{atom.rotation[0],atom.rotation[1],atom.rotation[2],atom.rotation[3]};
    XrQuaternionf dq=AV_V220FromTo(oldDir,newDir);
    XrQuaternionf q=AV_V220QNorm(AV_V220QMul(dq,qOld));
    atom.rotation[0]=q.x;atom.rotation[1]=q.y;atom.rotation[2]=q.z;atom.rotation[3]=q.w;
}

// bone toward the IK result. That made the selected axis pose-dependent: around a
// Y-pose two candidate axes could trade which had the largest dot product, causing
// the visible X/Y/Z scale axis to suddenly flip.
//
// Pick the longitudinal axis ONCE from the immutable RefSkeleton atom and its
// authored child direction, then apply the stretch to that same axis after IK.
static AV_V220V3 AV_V130RotateByQ(XrQuaternionf q,AV_V220V3 v){
    q=AV_V220QNorm(q);
    XrQuaternionf p{v.x,v.y,v.z,0.0f};
    XrQuaternionf o=AV_V220QMul(AV_V220QMul(q,p),AV_V220QConj(q));
    return {o.x,o.y,o.z};
}
static int AV_V131FindBindLongitudinalAxis(
    const AV_V27Atom48& bindAtom,AV_V220V3 bindChildDir)
{
    XrQuaternionf q=AV_V220QNorm({
        bindAtom.rotation[0],bindAtom.rotation[1],
        bindAtom.rotation[2],bindAtom.rotation[3]});
    const AV_V220V3 childDir=AV_V220Norm(bindChildDir);
    const AV_V220V3 axes[3]={
        AV_V130RotateByQ(q,{1,0,0}),
        AV_V130RotateByQ(q,{0,1,0}),
        AV_V130RotateByQ(q,{0,0,1})
    };
    int best=0;
    float bestDot=fabsf(AV_V220Dot(childDir,axes[0]));
    for(int i=1;i<3;++i){
        const float d=fabsf(AV_V220Dot(childDir,axes[i]));
        if(d>bestDot){bestDot=d;best=i;}
    }
    return best;
}
static void AV_V131StretchAtomAxis(
    AV_V27Atom48& atom,int longitudinalAxis,float stretchRatio)
{
    if(longitudinalAxis<0 || longitudinalAxis>2) return;
    if(!(stretchRatio>1.0001f) || !isfinite(stretchRatio)) return;
    atom.scale[longitudinalAxis] *= stretchRatio;
}

// HatVR paths had drifted apart and could leave one side behaving differently.
// normal reach uses the authored RefSkeleton lengths exactly. At natural maximum
// reach the chain extends without a hard maximum. Extra reach is distributed
// 75% upper / 25% forearm, and the elbow is recomputed from the NEW lengths every
// frame so it cannot freeze while the wrist continues outward.
struct AV_V290IKResult {
    AV_V220V3 elbow{};
    AV_V220V3 hand{};
    float upperLen=0.0f;
    float lowerLen=0.0f;
    float stretch=0.0f;
    bool valid=false;
};
static AV_V290IKResult AV_V290SolveTwoBoneIK(
    AV_V220V3 shoulder, AV_V220V3 target, AV_V220V3 poleHint,
    float naturalUpper, float naturalLower)
{
    AV_V290IKResult r{};
    if(naturalUpper<0.001f || naturalLower<0.001f) return r;

    AV_V220V3 toTarget=AV_V220Sub(target,shoulder);
    float dist=AV_V220Len(toTarget);
    if(dist<0.001f) dist=0.001f;
    AV_V220V3 dir=AV_V220Norm(toTarget);

    // keep the authored arm lengths through the normal reachable range. only extend past max reach,
    // distributing extra reach 75% to upper arm and 25% to forearm.
    float l1=naturalUpper;
    float l2=naturalLower;
    const float naturalReach=l1+l2;
    const float epsilon=0.001f;
    if(dist>naturalReach-epsilon){
        const float extra=dist-naturalReach+epsilon;
        l1 += extra*0.75f;
        l2 += extra*0.25f;
    }

    // recalculate the elbow from the stretched lengths so it keeps moving with the hand.
    const float minD=fabsf(l1-l2)+epsilon;
    const float maxD=l1+l2-epsilon;
    const float d=fmaxf(minD,fminf(dist,maxD));
    const float cosAlpha=fmaxf(-1.0f,fminf(1.0f,
        (l1*l1+d*d-l2*l2)/(2.0f*l1*d)));
    const float alpha=acosf(cosAlpha);

    // ik plane. poleHint is a direction in the same component
    // frame as shoulder/target. Left/right callers provide mirrored hints.
    AV_V220V3 planeNormal=AV_V220Cross(dir,poleHint);
    if(AV_V220Len(planeNormal)<0.001f){
        AV_V220V3 fallback={0.0f,0.0f,-1.0f};
        planeNormal=AV_V220Cross(dir,fallback);
        if(AV_V220Len(planeNormal)<0.001f)
            planeNormal=AV_V220Cross(dir,{0.0f,1.0f,0.0f});
    }
    planeNormal=AV_V220Norm(planeNormal);
    AV_V220V3 ortho=AV_V220Norm(AV_V220Cross(planeNormal,dir));

    const float ca=cosf(alpha), sa=sinf(alpha);
    AV_V220V3 upperDir=AV_V220Norm(
        AV_V220Add(AV_V220Mul(dir,ca),AV_V220Mul(ortho,sa)));

    r.elbow=AV_V220Add(shoulder,AV_V220Mul(upperDir,l1));
    r.hand=AV_V220Add(shoulder,AV_V220Mul(dir,d));
    r.upperLen=l1;
    r.lowerLen=l2;
    r.stretch=(l1+l2)-naturalReach;
    r.valid=true;
    return r;
}

// OpenXR: +X right, +Y up, -Z forward. UE3: +X forward,+Y right,+Z up.
static AV_V220V3 AV_V220XrVectorToGame(const XrVector3f& d){
    return {-d.z*g_xrWorldUnitsPerMeter,d.x*g_xrWorldUnitsPerMeter,d.y*g_xrWorldUnitsPerMeter};
}

static AV_V220V3 AV_V228Cross(AV_V220V3 a,AV_V220V3 b){
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}

static AV_V220V3 AV_V270RotateAroundAxis(AV_V220V3 v,AV_V220V3 axis,float radians){
    axis=AV_V220Norm(axis);
    const float c=cosf(radians), ss=sinf(radians);
    return AV_V220Add(AV_V220Add(AV_V220Mul(v,c),AV_V220Mul(AV_V228Cross(axis,v),ss)),
                      AV_V220Mul(axis,AV_V220Dot(axis,v)*(1.0f-c)));
}
static XrQuaternionf AV_V270AxisAngleQ(AV_V220V3 axis,float radians){
    axis=AV_V220Norm(axis); const float h=radians*0.5f, ss=sinf(h);
    return AV_V220QNorm({axis.x*ss,axis.y*ss,axis.z*ss,cosf(h)});
}
static float AV_V270RelativeHeadYaw(){
    // Relative to the tracking/recenter orientation, so enabling FP never snaps
    // Hat Kid to an arbitrary OpenXR world heading.
    XrQuaternionf now=AV_V220QNorm(g_xrHeadOrientation);
    XrQuaternionf origin=AV_V220QNorm(g_xrTrackingOriginOrientation);
    XrQuaternionf rel=AV_V220QNorm(AV_V220QMul(AV_V220QConj(origin),now));
    XrQuaternionf fwdP{0.0f,0.0f,-1.0f,0.0f};
    XrQuaternionf fwdQ=AV_V220QMul(AV_V220QMul(rel,fwdP),AV_V220QConj(rel));
    // OpenXR +X=right, -Z=forward. Positive result means look-right.
    return -atan2f(-fwdQ.x,-fwdQ.z); // v5.1: component-space positive yaw was opposite the HMD visual turn
}

static bool AV_V220ReadAtom(const AV_V27TArray64& sb,int bone,AV_V27Atom48& a){
    return bone>=0 && bone<sb.Count && FP_ReadMemory((const void*)(sb.Data+(uintptr_t)bone*0x30),&a,sizeof(a));
}
static bool AV_V220WriteAtom(const AV_V27TArray64& sb,int bone,const AV_V27Atom48& a){
    if(bone<0||bone>=sb.Count)return false;
    __try{memcpy((void*)(sb.Data+(uintptr_t)bone*0x30),&a,sizeof(a));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

static bool g_avV221BaselineValid=false;
static AV_V27Atom48 g_avV221Baseline[15]{};
static AV_V27Atom48 g_avV221BaselineNeck{};
static AV_V27Atom48 g_avV223BaselineRoot{};
// keep this as the model's anatomical neck-to-eye distance, not a hand offset.
static constexpr float g_avV256NeckToEyeUU=19.0f;

static AV_V27Atom48 g_avV231BaselineLeftShoulder{};
static bool g_avV241BaselineLeftValid=false;
static AV_V27Atom48 g_avV241BaselineLeft[14]{}; // bones 18..31

// camera/world -> inverse(player model) -> skeleton model space.  Before we
// move Hat Kid's whole body with the HMD, find AHiT's real UPrimitiveComponent
// LocalToWorld instead of guessing another skeleton basis.  UE3 stores a
// native 4x4 LocalToWorld on PrimitiveComponent; scan this exact HatKidHead
// instance for affine, near-orthogonal matrix candidates and log them once.
static bool g_avV229DumpedLocalToWorldCandidates=false;
static bool AV_V229Finite(float v){ return _finite(v)!=0 && fabsf(v)<10000000.0f; }
static void AV_V229DumpLocalToWorldCandidates(uintptr_t comp){
    if(g_avV229DumpedLocalToWorldCandidates || !FP_V111PlausiblePtr(comp)) return;
    g_avV229DumpedLocalToWorldCandidates=true;
    FP_Log("AV_V229_L2W_SCAN_BEGIN -- exact HatKidHead component; looking for UE3 affine 4x4 LocalToWorld candidates\n");
    int hits=0;
    for(uintptr_t off=0x30;off<=0x800;off+=4){
        float m[16]{};
        if(!FP_ReadMemory((const void*)(comp+off),m,sizeof(m))) continue;
        bool finite=true; for(float v:m) finite&=AV_V229Finite(v); if(!finite) continue;
        // UE3 FMatrix is row-vector style: last column is normally 0,0,0,1;
        // translation lives in row 3.  Allow a little numerical noise.
        if(fabsf(m[3])>0.02f||fabsf(m[7])>0.02f||fabsf(m[11])>0.02f||fabsf(m[15]-1.0f)>0.02f) continue;
        float l0=sqrtf(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);
        float l1=sqrtf(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]);
        float l2=sqrtf(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
        if(l0<0.05f||l1<0.05f||l2<0.05f||l0>20||l1>20||l2>20) continue;
        float d01=(m[0]*m[4]+m[1]*m[5]+m[2]*m[6])/(l0*l1);
        float d02=(m[0]*m[8]+m[1]*m[9]+m[2]*m[10])/(l0*l2);
        float d12=(m[4]*m[8]+m[5]*m[9]+m[6]*m[10])/(l1*l2);
        if(fabsf(d01)>0.08f||fabsf(d02)>0.08f||fabsf(d12)>0.08f) continue;
        char line[768]{};
        sprintf_s(line,sizeof(line),
            "AV_V229_L2W_CAND off=0x%llX scale=[%.3f %.3f %.3f] pos=[%.2f %.2f %.2f] r0=[%.3f %.3f %.3f] r1=[%.3f %.3f %.3f] r2=[%.3f %.3f %.3f]\n",
            (unsigned long long)off,l0,l1,l2,m[12],m[13],m[14],m[0],m[1],m[2],m[4],m[5],m[6],m[8],m[9],m[10]);
        FP_Log(line); if(++hits>=24) break;
    }
    char done[160]{};sprintf_s(done,sizeof(done),"AV_V229_L2W_SCAN_DONE hits=%d\n",hits);FP_Log(done);
}

// Physical controller/head offsets are in meters and g_xrWorldUnitsPerMeter is
// world scale (~100 UU/m), but Hat Kid's rendered arm is only ~14 UU long.
// Scale physical motion into avatar proportions so the IK target stays inside
// the arm's useful reach instead of spending most of its time hard-clamped.

// actual HatKidHead component LocalToWorld: its translation matched the pawn
// origin and its rotation matched the first-person camera yaw exactly.
static constexpr uintptr_t AV_V231_LOCAL_TO_WORLD_OFFSET = 0x1D0;
static bool AV_V230ReadLocalToWorld(uintptr_t comp,float m[16]){
    if(!FP_V111PlausiblePtr(comp) || !FP_ReadMemory((const void*)(comp+AV_V231_LOCAL_TO_WORLD_OFFSET),m,sizeof(float)*16)) return false;
    for(int i=0;i<16;++i) if(!AV_V229Finite(m[i])) return false;
    return fabsf(m[3])<0.02f && fabsf(m[7])<0.02f && fabsf(m[11])<0.02f && fabsf(m[15]-1.0f)<0.02f;
}
// UE3 FMatrix uses row vectors. Transform a local direction to world, then
// inverse-rotate a world direction back into component space. HatKidHead is
// unit scale in the discovered matrix, but normalize rows so this remains safe.
static AV_V220V3 AV_V230LocalDirToWorld(const float m[16],AV_V220V3 v){
    return {v.x*m[0]+v.y*m[4]+v.z*m[8],v.x*m[1]+v.y*m[5]+v.z*m[9],v.x*m[2]+v.y*m[6]+v.z*m[10]};
}
static AV_V220V3 AV_V230WorldDirToLocal(const float m[16],AV_V220V3 v){
    AV_V220V3 r0=AV_V220Norm({m[0],m[1],m[2]});
    AV_V220V3 r1=AV_V220Norm({m[4],m[5],m[6]});
    AV_V220V3 r2=AV_V220Norm({m[8],m[9],m[10]});
    return {AV_V220Dot(v,r0),AV_V220Dot(v,r1),AV_V220Dot(v,r2)};
}

static constexpr float AV_V222_PHYSICAL_ARM_METERS = 0.65f;

static void AV_V222QuatToGame3x3(const XrQuaternionf& q,float r[9]){
    auto rotate=[](const XrQuaternionf& q,const XrVector3f& v)->XrVector3f{
        XrQuaternionf p{v.x,v.y,v.z,0};
        XrQuaternionf qc{-q.x,-q.y,-q.z,q.w};
        XrQuaternionf a=AV_V220QMul(q,p), o=AV_V220QMul(a,qc);
        return {o.x,o.y,o.z};
    };
    auto gameToXr=[](float X,float Y,float Z)->XrVector3f{return {Y,Z,-X};};
    auto xrToGame=[](const XrVector3f& v,float& X,float& Y,float& Z){X=-v.z;Y=v.x;Z=v.y;};
    XrVector3f gx=rotate(q,gameToXr(1,0,0)), gy=rotate(q,gameToXr(0,1,0)), gz=rotate(q,gameToXr(0,0,1));
    float x0,y0,z0,x1,y1,z1,x2,y2,z2;
    xrToGame(gx,x0,y0,z0);xrToGame(gy,x1,y1,z1);xrToGame(gz,x2,y2,z2);
    r[0]=x0;r[1]=x1;r[2]=x2;r[3]=y0;r[4]=y1;r[5]=y2;r[6]=z0;r[7]=z1;r[8]=z2;
}
static XrQuaternionf AV_V222Mat3ToQuat(const float m[9]){
    XrQuaternionf q{}; float tr=m[0]+m[4]+m[8];
    if(tr>0){float S=sqrtf(tr+1.0f)*2.0f;q.w=0.25f*S;q.x=(m[7]-m[5])/S;q.y=(m[2]-m[6])/S;q.z=(m[3]-m[1])/S;}
    else if(m[0]>m[4]&&m[0]>m[8]){float S=sqrtf(1.0f+m[0]-m[4]-m[8])*2.0f;q.w=(m[7]-m[5])/S;q.x=0.25f*S;q.y=(m[1]+m[3])/S;q.z=(m[2]+m[6])/S;}
    else if(m[4]>m[8]){float S=sqrtf(1.0f+m[4]-m[0]-m[8])*2.0f;q.w=(m[2]-m[6])/S;q.x=(m[1]+m[3])/S;q.y=0.25f*S;q.z=(m[5]+m[7])/S;}
    else{float S=sqrtf(1.0f+m[8]-m[0]-m[4])*2.0f;q.w=(m[3]-m[1])/S;q.x=(m[2]+m[6])/S;q.y=(m[5]+m[7])/S;q.z=0.25f*S;}
    return AV_V220QNorm(q);
}
static AV_V220V3 AV_V222RotateGameVec(const float r[9],AV_V220V3 v){
    return {r[0]*v.x+r[1]*v.y+r[2]*v.z,r[3]*v.x+r[4]*v.y+r[5]*v.z,r[6]*v.x+r[7]*v.y+r[8]*v.z};
}

// TArray. Bone/socket attachments are updated outside BuildRefToLocal, so a
// render-only SpaceBases override can visually move ItemPalm while an attached
// component still follows the game's unmodified pose. Dump the live attachment
// records so the next hook can target the real held-object update path.
static void AV_V234DumpAttachments(uintptr_t comp){
    if(g_avV234DumpedAttachments || !FP_V111PlausiblePtr(comp)) return;
    if(!AV_V1ResolvePropertyOffset(comp,"Attachments",g_avV234AttachmentsOffset)) return;
    AV_V27TArray64 a{};
    if(!FP_ReadMemory((const void*)(comp+(uintptr_t)g_avV234AttachmentsOffset),&a,sizeof(a))) return;
    char h[256]{}; sprintf_s(h,sizeof(h),"AV_V240_ATTACHMENTS_BEGIN comp=%p off=0x%X data=%p count=%d max=%d\n",(void*)comp,g_avV234AttachmentsOffset,(void*)a.Data,a.Count,a.Max); FP_Log(h);
    if(a.Count<0 || a.Count>128 || a.Max<a.Count || !FP_V111PlausiblePtr(a.Data)){ g_avV234DumpedAttachments=true; return; }
    // UE3 x64 FAttachment starts with Component* then FName BoneName. Different
    const int strides[2]={0x38,0x40};
    for(int si=0;si<2;++si){
        const int stride=strides[si];
        for(int i=0;i<a.Count;++i){
            uintptr_t e=a.Data+(uintptr_t)i*stride, child=0; int32_t nameIdx=-1,nameNum=0;
            FP_ReadMemory((const void*)e,&child,sizeof(child));
            FP_ReadMemory((const void*)(e+8),&nameIdx,sizeof(nameIdx));
            FP_ReadMemory((const void*)(e+12),&nameNum,sizeof(nameNum));
            char bone[128]="?", obj[128]="?";
            if(nameIdx>=0) FP_V125RawNameFromIndex(nameIdx,bone,sizeof(bone));
            if(FP_V111PlausiblePtr(child)) AV_V1ReadRawObjectName(child,obj,sizeof(obj));
            char l[512]{}; sprintf_s(l,sizeof(l),"AV_V240_ATTACHMENT stride=0x%X i=%d entry=%p child=%p childName=\"%s\" boneIdx=%d boneNum=%d bone=\"%s\"\n",stride,i,(void*)e,(void*)child,obj,nameIdx,nameNum,bone); FP_Log(l);
        }
    }
    FP_Log("AV_V240_ATTACHMENTS_END -- look for bip_ItemPalmR01 / hand/socket attachment records\n");
    g_avV234DumpedAttachments=true;
}

static bool AV_V260ReadParentTable(uintptr_t component,int* parents,int count)
{
    if(!parents || count<70 || !FP_V111PlausiblePtr(component)) return false;
    if(g_avV1SkeletalMeshOffset<0 &&
       !AV_V1ResolvePropertyOffset(component,"SkeletalMesh",g_avV1SkeletalMeshOffset))
        return false;

    uintptr_t skeletalMesh=0;
    if(!FP_V18ReadPtr(component+(uintptr_t)g_avV1SkeletalMeshOffset,skeletalMesh) ||
       !FP_V111PlausiblePtr(skeletalMesh))
        return false;

    // RELEASE-PERF: the RefSkeleton hierarchy is immutable for a SkeletalMesh.
    // means hundreds of guarded memory reads per render pass. Cache the complete
    // 70-bone parent table per mesh and make the hot path a memcpy.
    static uintptr_t s_cachedMesh=0;
    static int s_cachedParents[70]{};
    static bool s_cachedValid=false;
    if(s_cachedValid && s_cachedMesh==skeletalMesh){
        memcpy(parents,s_cachedParents,sizeof(s_cachedParents));
        return true;
    }

    if(g_avV12RefSkeletonOffset<0 &&
       !AV_V1ResolvePropertyOffset(skeletalMesh,"RefSkeleton",g_avV12RefSkeletonOffset))
        return false;

    struct TArray64 { uintptr_t Data; int32_t Count; int32_t Max; } bones{};
    if(!FP_ReadMemory((const void*)(skeletalMesh+(uintptr_t)g_avV12RefSkeletonOffset),
                      &bones,sizeof(bones)) ||
       bones.Count<count || bones.Count>512 || !FP_V111PlausiblePtr(bones.Data))
        return false;

    // the exact executable proves this game's in-memory RefSkeleton entries are
    // 0x60 bytes (the FName at +0 repeats at that stride), but the old +0x5C
    // "parentProbe" was only a guess and reads -1 for every bone.
    //
    // Rather than guess another offset, identify ParentIndex from the live
    // FMeshBone records themselves. A real ParentIndex field must satisfy a large
    // set of unambiguous Hat Kid hierarchy relationships simultaneously.
    struct Pair { int bone,parent; };
    static const Pair expected[]={
        {1,0},{2,1},{3,2},
        {18,2},{19,18},{20,19},
        {21,20},{22,21},{23,20},{24,23},{25,20},{26,25},
        {27,20},{28,27},{29,20},{30,29},
        {32,2},{33,32},{34,33},
        {35,34},{36,35},{37,34},{38,37},{39,34},{40,39},
        {41,34},{42,41},{43,34},{44,43},{45,34},
        {60,0},{61,60},{62,61},{63,62},
        {66,0},{67,66},{68,67},{69,68}
    };

    constexpr uintptr_t stride=0x60;
    int bestScore=-1;
    uintptr_t bestOff=0;

    // ParentIndex in UE3 FMeshBone is a 32-bit integer. Scan every aligned DWORD
    // after the FName instead of relying on an assumed SDK layout.
    for(uintptr_t off=0x08;off<=0x5C;off+=4){
        int score=0;
        bool readable=true;
        for(const Pair& e:expected){
            int32_t v=-9999;
            if(!FP_ReadMemory((const void*)(bones.Data+(uintptr_t)e.bone*stride+off),
                              &v,sizeof(v))){
                readable=false; break;
            }
            if(v==e.parent) ++score;
        }
        if(readable && score>bestScore){ bestScore=score; bestOff=off; }
    }

    // 38 relationships are checked. Requiring 30 makes accidental matches to
    // transform floats/flags effectively impossible while allowing a few unusual
    // attachment-parent differences.
    if(bestScore<30){
        static bool s_loggedFail=false;
        if(!s_loggedFail){
            s_loggedFail=true;
            char l[256]{};
            sprintf_s(l,sizeof(l),
                "AV_V260_PARENT_AUTODETECT_FAIL bestOff=0x%llX score=%d/%d\n",
                (unsigned long long)bestOff,bestScore,(int)(sizeof(expected)/sizeof(expected[0])));
            FP_Log(l);
        }
        return false;
    }

    for(int i=0;i<count;++i){
        int32_t p=-1;
        if(!FP_ReadMemory((const void*)(bones.Data+(uintptr_t)i*stride+bestOff),
                          &p,sizeof(p)))
            return false;
        parents[i]=(int)p;
    }

    // Cache only after the entire table has been read successfully. If Hat Kid's
    // skeletal mesh changes, the cache naturally rebuilds on the next call.
    memcpy(s_cachedParents,parents,sizeof(s_cachedParents));
    s_cachedMesh=skeletalMesh;
    s_cachedValid=true;

    static uintptr_t s_loggedOff=(uintptr_t)-1;
    if(s_loggedOff!=bestOff){
        s_loggedOff=bestOff;
        char l[384]{};
        sprintf_s(l,sizeof(l),
            "AV_V260_PARENT_AUTODETECT_OK offset=0x%llX score=%d/%d armChainL=[%d,%d,%d] armChainR=[%d,%d,%d]\n",
            (unsigned long long)bestOff,bestScore,(int)(sizeof(expected)/sizeof(expected[0])),
            parents[18],parents[19],parents[20],
            parents[32],parents[33],parents[34]);
        FP_Log(l);
    }
    return true;
}

struct AV_V16MasterRig {
    uintptr_t mesh=0;
    int count=0;
    int lUpper=-1,lFore=-1,lHand=-1;
    int rUpper=-1,rFore=-1,rHand=-1;
    bool valid=false;
    bool standard70=false;
};

static bool AV_V16ResolveMasterRig(uintptr_t component, AV_V16MasterRig& out)
{
    out={};
    if(!FP_V111PlausiblePtr(component)) return false;

    if(g_avV1SkeletalMeshOffset<0 &&
       !AV_V1ResolvePropertyOffset(component,"SkeletalMesh",g_avV1SkeletalMeshOffset))
        return false;

    uintptr_t mesh=0;
    if(!FP_V18ReadPtr(component+(uintptr_t)g_avV1SkeletalMeshOffset,mesh) ||
       !FP_V111PlausiblePtr(mesh))
        return false;

    if(g_avV12RefSkeletonOffset<0 &&
       !AV_V1ResolvePropertyOffset(mesh,"RefSkeleton",g_avV12RefSkeletonOffset))
        return false;

    struct Cache { uintptr_t mesh; AV_V16MasterRig map; };
    static Cache cache[32]{};
    static int cacheCount=0;
    for(int i=0;i<cacheCount;++i){
        if(cache[i].mesh==mesh){ out=cache[i].map; return out.valid; }
    }

    struct TArray64 { uintptr_t Data; int32_t Count; int32_t Max; } bones{};
    if(!FP_ReadMemory((const void*)(mesh+(uintptr_t)g_avV12RefSkeletonOffset),
                      &bones,sizeof(bones)) ||
       bones.Count<=0 || bones.Count>128 || bones.Max<bones.Count ||
       !FP_V111PlausiblePtr(bones.Data))
        return false;

    struct Wanted { const char* name; int32_t nameIndex; int* result; };
    int lu=-1,lf=-1,lh=-1,ru=-1,rf=-1,rh=-1;

    // expensive name-table searches for EVERY new player mesh, which was the
    // remaining model-swap hitch. Resolve each known arm name once per process.
    static bool s_armNamesAttempted=false;
    static int32_t s_armNameIndex[6]={-1,-1,-1,-1,-1,-1};
    static const char* s_armNameText[6]={
        "bip_armL02","bip_armL03","bip_handL_base",
        "bip_armR02","bip_armR03","bip_handR_base"
    };
    if(!s_armNamesAttempted){
        s_armNamesAttempted=true;
        LARGE_INTEGER nt0{},nt1{};
        QueryPerformanceCounter(&nt0);
        for(int i=0;i<6;++i){
            int32_t idx=-1;
            if(FP_V123FindRawNameIndex(s_armNameText[i],idx) && idx>=0)
                s_armNameIndex[i]=idx;
        }
        QueryPerformanceCounter(&nt1);
        LARGE_INTEGER nf{}; QueryPerformanceFrequency(&nf);
        const double nms=nf.QuadPart>0 ?
            (double)(nt1.QuadPart-nt0.QuadPart)*1000.0/(double)nf.QuadPart : 0.0;
        char nl[320]{};
        sprintf_s(nl,sizeof(nl),
            "AV_V103_ARM_NAMES_ONCE ms=%.3f idx=[%d %d %d %d %d %d]\\n",
            nms,
            s_armNameIndex[0],s_armNameIndex[1],s_armNameIndex[2],
            s_armNameIndex[3],s_armNameIndex[4],s_armNameIndex[5]);
        FP_Log(nl);
    }

    Wanted wanted[]={
        {s_armNameText[0],s_armNameIndex[0],&lu},
        {s_armNameText[1],s_armNameIndex[1],&lf},
        {s_armNameText[2],s_armNameIndex[2],&lh},
        {s_armNameText[3],s_armNameIndex[3],&ru},
        {s_armNameText[4],s_armNameIndex[4],&rf},
        {s_armNameText[5],s_armNameIndex[5],&rh}
    };

    constexpr uintptr_t stride=0x60;
    for(int b=0;b<bones.Count;++b){
        int32_t raw=-1;
        if(!FP_ReadMemory((const void*)(bones.Data+(uintptr_t)b*stride),
                          &raw,sizeof(raw)))
            return false;
        for(auto& w:wanted){
            if(w.nameIndex>=0 && raw==w.nameIndex) *w.result=b;
        }
    }

    out.mesh=mesh;
    out.count=bones.Count;
    out.lUpper=lu; out.lFore=lf; out.lHand=lh;
    out.rUpper=ru; out.rFore=rf; out.rHand=rh;
    out.valid=
        lu>=0 && lf>=0 && lh>=0 &&
        ru>=0 && rf>=0 && rh>=0 &&
        lu<bones.Count && lf<bones.Count && lh<bones.Count &&
        ru<bones.Count && rf<bones.Count && rh<bones.Count;
    // standard path is retained only when those names resolve to the canonical
    // Hat Kid chain.  No outfit is promoted into that path merely because it
    // happens to have 70 bones.
    out.standard70=
        out.valid &&
        lu==18 && lf==19 && lh==20 &&
        ru==32 && rf==33 && rh==34;

    if(cacheCount<32) cache[cacheCount++]={mesh,out};

    char line[512]{};
    sprintf_s(line,sizeof(line),
        "AV_V32_NAMED_ARM_MAP mesh=%p bones=%d L=[%d %d %d] R=[%d %d %d] standard70=%d valid=%d\n",
        (void*)mesh,bones.Count,lu,lf,lh,ru,rf,rh,
        out.standard70?1:0,out.valid?1:0);
    FP_Log(line);
    return out.valid;
}

// this is the deterministic neutral pose we want for VR.  Unlike SpaceBases at
// F5 time, it cannot be contaminated by look-at, locomotion, item-holding, or
// attack animation.  AHiT's FMeshBone layout is discovered conservatively at
// runtime: ParentIndex is already autodetected by AV_V260ReadParentTable; here
// we scan the remaining record for the parent-relative FQuat + FVector pair.
// if discovery fails we simply keep the old captured-pose fallback.
static bool AV_V280ReadReferencePose(uintptr_t component,AV_V27Atom48* out,int count)
{
    if(!out || count<70 || !FP_V111PlausiblePtr(component)) return false;
    if(g_avV1SkeletalMeshOffset<0 &&
       !AV_V1ResolvePropertyOffset(component,"SkeletalMesh",g_avV1SkeletalMeshOffset))
        return false;
    uintptr_t mesh=0;
    if(!FP_V18ReadPtr(component+(uintptr_t)g_avV1SkeletalMeshOffset,mesh) ||
       !FP_V111PlausiblePtr(mesh)) return false;

    static uintptr_t cachedMesh=0;
    static AV_V27Atom48 cached[70]{};
    static bool cachedValid=false;
    if(cachedValid && cachedMesh==mesh){ memcpy(out,cached,sizeof(cached)); return true; }

    if(g_avV12RefSkeletonOffset<0 &&
       !AV_V1ResolvePropertyOffset(mesh,"RefSkeleton",g_avV12RefSkeletonOffset))
        return false;
    struct TArray64 { uintptr_t Data; int32_t Count; int32_t Max; } bones{};
    if(!FP_ReadMemory((const void*)(mesh+(uintptr_t)g_avV12RefSkeletonOffset),&bones,sizeof(bones)) ||
       bones.Count<70 || bones.Count>512 || !FP_V111PlausiblePtr(bones.Data)) return false;

    int parents[70]{};
    if(!AV_V260ReadParentTable(component,parents,70)) return false;

    constexpr uintptr_t stride=0x60;
    int bestScore=-1; uintptr_t bestOff=0;
    // FMeshBone contains a parent-relative quaternion immediately followed by
    // its position.  Find that block by normalized-quaternion/plausibility score
    // instead of hard-coding a game-specific SDK offset.
    for(uintptr_t off=0x08; off+28<=stride; off+=4){
        int score=0, tested=0;
        for(int b=0;b<70;++b){
            float v[7]{};
            if(!FP_ReadMemory((const void*)(bones.Data+(uintptr_t)b*stride+off),v,sizeof(v))) continue;
            bool finite=true; for(float f:v) finite &= std::isfinite(f);
            if(!finite) continue;
            const float qn=v[0]*v[0]+v[1]*v[1]+v[2]*v[2]+v[3]*v[3];
            const float pm=fabsf(v[4])+fabsf(v[5])+fabsf(v[6]);
            ++tested;
            if(qn>0.80f && qn<1.20f && pm<2000.0f) ++score;
        }
        if(tested>=60 && score>bestScore){ bestScore=score; bestOff=off; }
    }
    if(bestScore<60) return false;

    AV_V27Atom48 local[70]{};
    for(int b=0;b<70;++b){
        float v[7]{};
        if(!FP_ReadMemory((const void*)(bones.Data+(uintptr_t)b*stride+bestOff),v,sizeof(v))) return false;
        local[b].scale[0]=local[b].scale[1]=local[b].scale[2]=local[b].scale[3]=1.0f;
        XrQuaternionf q=AV_V220QNorm({v[0],v[1],v[2],v[3]});
        local[b].rotation[0]=q.x; local[b].rotation[1]=q.y;
        local[b].rotation[2]=q.z; local[b].rotation[3]=q.w;
        local[b].translation[0]=v[4]; local[b].translation[1]=v[5];
        local[b].translation[2]=v[6]; local[b].translation[3]=0.0f;
    }

    auto rot=[](const XrQuaternionf& q,AV_V220V3 v)->AV_V220V3{
        XrQuaternionf p{v.x,v.y,v.z,0};
        XrQuaternionf r=AV_V220QMul(AV_V220QMul(q,p),AV_V220QConj(q));
        return {r.x,r.y,r.z};
    };
    for(int b=0;b<70;++b){
        const int p=parents[b];
        if(b==0 || p<0 || p>=b){ cached[b]=local[b]; continue; }
        const XrQuaternionf pq=AV_V220QNorm({cached[p].rotation[0],cached[p].rotation[1],cached[p].rotation[2],cached[p].rotation[3]});
        const XrQuaternionf lq=AV_V220QNorm({local[b].rotation[0],local[b].rotation[1],local[b].rotation[2],local[b].rotation[3]});
        const XrQuaternionf cq=AV_V220QNorm(AV_V220QMul(pq,lq));
        cached[b]=local[b];
        cached[b].rotation[0]=cq.x; cached[b].rotation[1]=cq.y;
        cached[b].rotation[2]=cq.z; cached[b].rotation[3]=cq.w;
        AV_V220SetAtomPos(cached[b],AV_V220Add(AV_V220AtomPos(cached[p]),rot(pq,AV_V220AtomPos(local[b]))));
    }
    cachedMesh=mesh; cachedValid=true;
    memcpy(out,cached,sizeof(cached));
    char l[192]{}; sprintf_s(l,sizeof(l),"AV_V280_REFPOSE_OK mesh=%p transformOff=0x%llX score=%d/70 -- deterministic bind/T pose active\\n",(void*)mesh,(unsigned long long)bestOff,bestScore); FP_Log(l);
    return true;
}

static bool AV_V259IsDescendantOf(int bone,int ancestor,const int* parents,int count)
{
    if(!parents || bone<0 || bone>=count || ancestor<0 || ancestor>=count) return false;
    int p=parents[bone];
    for(int guard=0;guard<count && p>=0 && p<count;++guard){
        if(p==ancestor) return true;
        const int next=parents[p];
        if(next==p) break;
        p=next;
    }
    return false;
}

static bool AV_V16ReadNonstandardRefPose(
    uintptr_t component,const AV_V16MasterRig& rig,
    int* parents,AV_V27Atom48* out)
{
    // Nyakuza/Mixed, Bow, HCR and compatible modded masters all decode their OWN
    // RefSkeleton through this exact path.
    if(!rig.valid || !parents || !out || rig.count<=0 || rig.count>128)
        return false;

    // Cache the fully composed component-space reference pose once per mesh instead
    // of rescanning transform offsets and recomposing up to 128 bones in the render hook.
    struct AV_V111RefCacheEntry {
        uintptr_t mesh{}; int count{}; bool valid{};
        int parents[128]{}; AV_V27Atom48 ref[128]{};
    };
    static AV_V111RefCacheEntry v111Cache[16]{};
    static int v111CacheCount=0;
    for(int i=0;i<v111CacheCount;++i){
        if(v111Cache[i].valid && v111Cache[i].mesh==rig.mesh && v111Cache[i].count==rig.count){
            memcpy(parents,v111Cache[i].parents,sizeof(int)*rig.count);
            memcpy(out,v111Cache[i].ref,sizeof(AV_V27Atom48)*rig.count);
            return true;
        }
    }
    if(g_avV12RefSkeletonOffset<0 &&
       !AV_V1ResolvePropertyOffset(rig.mesh,"RefSkeleton",g_avV12RefSkeletonOffset))
        return false;
    struct TArray64 { uintptr_t Data; int32_t Count; int32_t Max; } bones{};
    if(!FP_ReadMemory((const void*)(rig.mesh+(uintptr_t)g_avV12RefSkeletonOffset),
                      &bones,sizeof(bones)) ||
       bones.Count<rig.count || !FP_V111PlausiblePtr(bones.Data)) return false;
    constexpr uintptr_t stride=0x60;
    constexpr uintptr_t parentOff=0x50; // v13.1 live autodetect: 38/38 on Hat Kid.
    for(int b=0;b<rig.count;++b){
        int32_t p=-1;
        if(!FP_ReadMemory((const void*)(bones.Data+(uintptr_t)b*stride+parentOff),
                          &p,sizeof(p))) return false;
        parents[b]=(int)p;
        if(b==0){ if(!(p==-1 || p==0)) return false; }
        else if(p<0 || p>=b) return false;
    }
    if(parents[rig.lFore]!=rig.lUpper || parents[rig.lHand]!=rig.lFore ||
       parents[rig.rFore]!=rig.rUpper || parents[rig.rHand]!=rig.rFore) return false;

    int bestScore=-1; uintptr_t bestOff=0;
    for(uintptr_t off=0x08;off+28<=stride;off+=4){
        int score=0,tested=0;
        for(int b=0;b<rig.count;++b){
            float v[7]{};
            if(!FP_ReadMemory((const void*)(bones.Data+(uintptr_t)b*stride+off),v,sizeof(v))) continue;
            bool finite=true; for(float f:v) finite &= std::isfinite(f);
            if(!finite) continue;
            float qn=v[0]*v[0]+v[1]*v[1]+v[2]*v[2]+v[3]*v[3];
            float pm=fabsf(v[4])+fabsf(v[5])+fabsf(v[6]);
            ++tested; if(qn>0.80f && qn<1.20f && pm<2000.0f) ++score;
        }
        if(tested>=rig.count-2 && score>bestScore){bestScore=score;bestOff=off;}
    }
    if(bestScore<rig.count-4) return false;
    AV_V27Atom48 local[128]{};
    for(int b=0;b<rig.count;++b){
        float v[7]{};
        if(!FP_ReadMemory((const void*)(bones.Data+(uintptr_t)b*stride+bestOff),v,sizeof(v))) return false;
        local[b].scale[0]=local[b].scale[1]=local[b].scale[2]=local[b].scale[3]=1.0f;
        XrQuaternionf q=AV_V220QNorm({v[0],v[1],v[2],v[3]});
        local[b].rotation[0]=q.x;local[b].rotation[1]=q.y;local[b].rotation[2]=q.z;local[b].rotation[3]=q.w;
        local[b].translation[0]=v[4];local[b].translation[1]=v[5];local[b].translation[2]=v[6];local[b].translation[3]=0;
    }
    auto rot=[](XrQuaternionf q,AV_V220V3 v)->AV_V220V3{
        XrQuaternionf p{v.x,v.y,v.z,0};
        XrQuaternionf r=AV_V220QMul(AV_V220QMul(q,p),AV_V220QConj(q));
        return {r.x,r.y,r.z};
    };
    for(int b=0;b<rig.count;++b){
        int p=parents[b];
        if(b==0 || p<0 || p>=b){out[b]=local[b];continue;}
        XrQuaternionf pq=AV_V220QNorm({out[p].rotation[0],out[p].rotation[1],out[p].rotation[2],out[p].rotation[3]});
        XrQuaternionf lq=AV_V220QNorm({local[b].rotation[0],local[b].rotation[1],local[b].rotation[2],local[b].rotation[3]});
        XrQuaternionf cq=AV_V220QNorm(AV_V220QMul(pq,lq));
        out[b]=local[b]; out[b].rotation[0]=cq.x;out[b].rotation[1]=cq.y;out[b].rotation[2]=cq.z;out[b].rotation[3]=cq.w;
        AV_V220SetAtomPos(out[b],AV_V220Add(AV_V220AtomPos(out[p]),rot(pq,AV_V220AtomPos(local[b]))));
    }
    static uintptr_t last=0;
    if(last!=rig.mesh){last=rig.mesh;char l[320]{};sprintf_s(l,sizeof(l),
        "AV_V103_UNIVERSAL_REFPOSE_OK mesh=%p bones=%d standard70=%d parentOff=0x50 transformOff=0x%llX score=%d\\n",
        (void*)rig.mesh,rig.count,rig.standard70?1:0,(unsigned long long)bestOff,bestScore);FP_Log(l);}
    {
        int slot=-1;
        for(int i=0;i<v111CacheCount;++i) if(!v111Cache[i].valid){ slot=i; break; }
        if(slot<0 && v111CacheCount<16) slot=v111CacheCount++;
        if(slot<0) slot=0; // extremely unlikely player-mesh churn; safe replacement
        v111Cache[slot].mesh=rig.mesh; v111Cache[slot].count=rig.count;
        memcpy(v111Cache[slot].parents,parents,sizeof(int)*rig.count);
        memcpy(v111Cache[slot].ref,out,sizeof(AV_V27Atom48)*rig.count);
        v111Cache[slot].valid=true;
    }
    return true;
}

// Reports once per second so profiling itself does not spam the log. Times are
// split around UE3's original BuildRefToLocal so game cost is not blamed on HatVR.
struct AV_PerfV2Accum {
    uint64_t calls=0, activeCalls=0;
    double setupMs=0.0;
    double hatvrActiveMs=0.0;
    double hatvrInactiveMs=0.0;
    double originalMs=0.0;
    double restoreMs=0.0;
    double postMs=0.0;
};
static AV_PerfV2Accum g_avPerfV2{};
static ULONGLONG g_avPerfV2LastReport=0;
static LARGE_INTEGER g_avPerfV2Freq{};
static bool g_avPerfV2FreqReady=false;

static double AV_PerfV2Ms(LARGE_INTEGER a,LARGE_INTEGER b)
{
    if(!g_avPerfV2FreqReady){
        QueryPerformanceFrequency(&g_avPerfV2Freq);
        g_avPerfV2FreqReady=g_avPerfV2Freq.QuadPart>0;
    }
    if(!g_avPerfV2FreqReady) return 0.0;
    return (double)(b.QuadPart-a.QuadPart)*1000.0/(double)g_avPerfV2Freq.QuadPart;
}

