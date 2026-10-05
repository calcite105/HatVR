#pragma once
// HatVR avatar_system.inl
//
// F5 starts a bounded capture of EVERY 0x6A4980 invocation, rather than filtering
// to Pawn.Mesh before logging. Each pass records component, caller, thread,
// SpaceBases, ParentAnimComponent, and whether it is the currently selected mesh.
// the +80 bone-34 mutation remains restricted to the selected mesh for safety.

static uintptr_t g_avV1LastPawn = 0;
static ULONGLONG g_avV1LastDumpTick = 0;
static int g_avV1CustomizableMeshesOffset = -1;
static int g_avV1SkeletalMeshOffset = -1;
static int g_avV11ComponentsOffset = -1;
static int g_avV11AllComponentsOffset = -1;
static int g_avV12RefSkeletonOffset = -1;
static int g_avV14SpaceBasesOffset = -1;
static int g_avV14LocalAtomsOffset = -1;
static int g_avV14ParentAnimComponentOffset = -1;
static int g_avV291AttachParentOffset = -1;
static bool g_avV291PrevF6Down = false;
static int g_avV234AttachmentsOffset = -1;
static bool g_avV234DumpedAttachments = false;
static bool g_avV29ProbeEnabled=false;

// solve unchanged. Later calls in that same frame replay the solved master
// SpaceBases, let native UE3 build the requested ReferenceToLocal array, then
// restore the game's incoming animation pose.
static unsigned long long g_avV84CacheFrame=~0ULL;
static uintptr_t g_avV84CacheOwner=0;
static int g_avV84CacheCount=0;
static bool g_avV84CacheValid=false;
static uint64_t g_avV84Solves=0;
static uint64_t g_avV84Hits=0;
static ULONGLONG g_avV84LastReport=0;

static bool AV_V1ReadRawObjectName(uintptr_t object, char* out, size_t outSize)
{
    if (!out || outSize == 0 || !FP_V111PlausiblePtr(object))
        return false;
    int32_t idx = -1;
    if (!FP_ReadMemory(reinterpret_cast<const void*>(object + 0x48),
                       &idx, sizeof(idx)))
        return false;
    return FP_V125RawNameFromIndex(idx, out, outSize);
}

static bool AV_V1ResolvePropertyOffset(
    uintptr_t object, const char* propertyName, int& cachedOffset)
{
    if (cachedOffset >= 0)
        return true;
    if (!FP_V111PlausiblePtr(object))
        return false;

    int32_t nameIndex = -1;
    uintptr_t cls = 0, field = 0;
    if (!FP_V123FindRawNameIndex(propertyName, nameIndex) ||
        nameIndex < 0 ||
        !FP_V18ReadPtr(object + 0x50, cls) ||
        !FP_V111PlausiblePtr(cls) ||
        !FP_V123FindFieldByRawNameIndex(cls, nameIndex, field) ||
        !FP_V111PlausiblePtr(field))
        return false;

    int32_t offset = -1;
    if (!FP_ReadMemory(reinterpret_cast<const void*>(field + 0x8C),
                       &offset, sizeof(offset)) ||
        offset < 0 || offset > 0x4000)
        return false;

    cachedOffset = offset;
    char line[224] = {};
    sprintf_s(line, sizeof(line),
        "AV_V1_PROPERTY %s offset=0x%X field=%p class=%p\n",
        propertyName, cachedOffset,
        reinterpret_cast<void*>(field), reinterpret_cast<void*>(cls));
    FP_Log(line);
    return true;
}

// Pawn.Mesh remains the live master pose component. We suppress only the slave
// visual head component whose ParentAnimComponent points at Pawn.Mesh.
static int g_avV246OwnerNoSeeOffset=-1;
static uintptr_t g_avV246OwnerNoSeeField=0;
static uint32_t g_avV246OwnerNoSeeMask=0;
static uintptr_t g_avV246HiddenHeadVisual=0;
static uintptr_t g_avV246HiddenHat=0;
static bool g_avV246WasActive=false;
static bool g_avV246Logged=false;

static bool AV_V246ResolveBoolProperty(uintptr_t object,const char* propertyName,
    int& cachedOffset,uintptr_t& cachedField,uint32_t& cachedMask)
{
    if(cachedOffset>=0 && cachedField && cachedMask) return true;
    if(!FP_V111PlausiblePtr(object)) return false;
    int32_t nameIndex=-1; uintptr_t cls=0,field=0;
    if(!FP_V123FindRawNameIndex(propertyName,nameIndex) || nameIndex<0 ||
       !FP_V18ReadPtr(object+0x50,cls) || !FP_V111PlausiblePtr(cls) ||
       !FP_V123FindFieldByRawNameIndex(cls,nameIndex,field) ||
       !FP_V111PlausiblePtr(field)) return false;
    int32_t off=-1;
    if(!FP_ReadMemory((const void*)(field+0x8C),&off,sizeof(off)) ||
       off<0 || off>0x4000) return false;
    uint32_t foundMask=0; uintptr_t foundAt=0;
    for(uintptr_t q=field+0x90;q<=field+0xC0;q+=4){
        uint32_t v=0;
        if(!FP_ReadMemory((const void*)q,&v,sizeof(v))) continue;
        if(v && (v&(v-1u))==0u){ foundMask=v; foundAt=q; }
    }
    if(!foundMask) return false;
    cachedOffset=off; cachedField=field; cachedMask=foundMask;
    char l[320]{};
    sprintf_s(l,sizeof(l),
        "AV_V246_BOOL_PROPERTY name=%s off=0x%X field=%p mask=0x%08X maskAt=+0x%llX\n",
        propertyName,off,(void*)field,foundMask,
        (unsigned long long)(foundAt-field));
    FP_Log(l);
    return true;
}

static bool AV_V246SetOwnerNoSee(uintptr_t comp,bool hidden)
{
    if(!FP_V111PlausiblePtr(comp)) return false;
    if(!AV_V246ResolveBoolProperty(comp,"OwnerNoSee",
        g_avV246OwnerNoSeeOffset,g_avV246OwnerNoSeeField,g_avV246OwnerNoSeeMask) &&
       !AV_V246ResolveBoolProperty(comp,"bOwnerNoSee",
        g_avV246OwnerNoSeeOffset,g_avV246OwnerNoSeeField,g_avV246OwnerNoSeeMask))
        return false;
    const uintptr_t addr=comp+(uintptr_t)g_avV246OwnerNoSeeOffset;
    uint32_t word=0;
    if(!FP_ReadMemory((const void*)addr,&word,sizeof(word))) return false;
    const uint32_t next=hidden ? (word|g_avV246OwnerNoSeeMask)
                               : (word&~g_avV246OwnerNoSeeMask);
    if(next!=word){
        __try{ *(volatile uint32_t*)addr=next; }
        __except(EXCEPTION_EXECUTE_HANDLER){ return false; }
    }
    return true;
}

static uintptr_t AV_V246FindSlaveHeadVisual(uintptr_t pawn,uintptr_t master)
{
    if(!FP_V111PlausiblePtr(pawn)||!FP_V111PlausiblePtr(master)) return 0;
    if(!AV_V1ResolvePropertyOffset(pawn,"Components",g_avV11ComponentsOffset) ||
       !AV_V1ResolvePropertyOffset(master,"ParentAnimComponent",g_avV14ParentAnimComponentOffset) ||
       !AV_V1ResolvePropertyOffset(master,"SkeletalMesh",g_avV1SkeletalMeshOffset))
        return 0;
    struct TArray64{uintptr_t Data;int32_t Count;int32_t Max;} arr{};
    if(!FP_ReadMemory((const void*)(pawn+(uintptr_t)g_avV11ComponentsOffset),&arr,sizeof(arr)) ||
       arr.Count<0||arr.Count>512||arr.Max<arr.Count||!FP_V111PlausiblePtr(arr.Data))
        return 0;
    uintptr_t masterMesh=0;
    FP_V18ReadPtr(master+(uintptr_t)g_avV1SkeletalMeshOffset,masterMesh);
    for(int i=0;i<arr.Count;++i){
        uintptr_t comp=0;
        if(!FP_V18ReadPtr(arr.Data+(uintptr_t)i*sizeof(uintptr_t),comp) ||
           !FP_V111PlausiblePtr(comp)||comp==master) continue;
        uintptr_t cls=0; char className[128]{};
        FP_V18ReadPtr(comp+0x50,cls);
        AV_V1ReadRawObjectName(cls,className,sizeof(className));
        if(!strstr(className,"SkeletalMeshComponent")) continue;
        uintptr_t parent=0;
        FP_V18ReadPtr(comp+(uintptr_t)g_avV14ParentAnimComponentOffset,parent);
        if(parent!=master) continue;
        uintptr_t childMesh=0;
        FP_V18ReadPtr(comp+(uintptr_t)g_avV1SkeletalMeshOffset,childMesh);
        if(masterMesh && childMesh==masterMesh) return comp;
    }
    return 0;
}

static uintptr_t AV_V246FindHatAttachment(uintptr_t master)
{
    if(!FP_V111PlausiblePtr(master) ||
       !AV_V1ResolvePropertyOffset(master,"Attachments",g_avV234AttachmentsOffset))
        return 0;
    struct TArray64{uintptr_t Data;int32_t Count;int32_t Max;} a{};
    if(!FP_ReadMemory((const void*)(master+(uintptr_t)g_avV234AttachmentsOffset),&a,sizeof(a)) ||
       a.Count<0||a.Count>128||a.Max<a.Count||!FP_V111PlausiblePtr(a.Data)) return 0;
    for(int i=0;i<a.Count;++i){
        const uintptr_t e=a.Data+(uintptr_t)i*0x40;
        uintptr_t child=0; int32_t nameIdx=-1;
        FP_ReadMemory((const void*)e,&child,sizeof(child));
        FP_ReadMemory((const void*)(e+8),&nameIdx,sizeof(nameIdx));
        if(!FP_V111PlausiblePtr(child)||nameIdx<0) continue;
        char bone[128]{};
        if(!FP_V125RawNameFromIndex(nameIdx,bone,sizeof(bone))) continue;
        if(strstr(bone,"bip_hat")==bone||strstr(bone,"Bip_hat")==bone||
           strstr(bone,"bip_Hat")==bone) return child;
    }
    return 0;
}

static int g_avV249HatHiddenGameOffset=-1;
static uintptr_t g_avV249HatHiddenGameField=0;
static uint32_t g_avV249HatHiddenGameMask=0;

static bool AV_V249SetHatHiddenGame(uintptr_t comp,bool hidden)
{
    if(!FP_V111PlausiblePtr(comp)) return false;
    if(!AV_V246ResolveBoolProperty(comp,"HiddenGame",
        g_avV249HatHiddenGameOffset,g_avV249HatHiddenGameField,g_avV249HatHiddenGameMask) &&
       !AV_V246ResolveBoolProperty(comp,"bHiddenGame",
        g_avV249HatHiddenGameOffset,g_avV249HatHiddenGameField,g_avV249HatHiddenGameMask))
        return false;

    const uintptr_t addr=comp+(uintptr_t)g_avV249HatHiddenGameOffset;
    uint32_t word=0;
    if(!FP_ReadMemory((const void*)addr,&word,sizeof(word))) return false;
    const uint32_t next=hidden ? (word|g_avV249HatHiddenGameMask)
                               : (word&~g_avV249HatHiddenGameMask);
    if(next!=word){
        __try{ *(volatile uint32_t*)addr=next; }
        __except(EXCEPTION_EXECUTE_HANDLER){ return false; }
    }
    return true;
}

static void AV_V248UpdateHatVisibility(uintptr_t pawn)
{
    const bool active=g_fpV1Enabled && g_avV29ProbeEnabled;
    if(!active){
        if(g_avV246HiddenHat){
            const bool okOwner=AV_V246SetOwnerNoSee(g_avV246HiddenHat,false);
            const bool okHidden=AV_V249SetHatHiddenGame(g_avV246HiddenHat,false);
            const bool ok=okOwner || okHidden;
            char l[256]{};
            sprintf_s(l,sizeof(l),
                "AV_V249_HAT_RESTORE hat=%p restored=%d\n",
                (void*)g_avV246HiddenHat,ok?1:0);
            FP_Log(l);
        }
        g_avV246HiddenHat=0;
        return;
    }

    if(!FP_V111PlausiblePtr(pawn)) return;
    uintptr_t master=0;
    if(!FP_V18ReadPtr(pawn+0x530,master)||!FP_V111PlausiblePtr(master)) return;

    const uintptr_t hat=AV_V246FindHatAttachment(master);
    if(g_avV246HiddenHat && g_avV246HiddenHat!=hat){
        AV_V246SetOwnerNoSee(g_avV246HiddenHat,false);
        AV_V249SetHatHiddenGame(g_avV246HiddenHat,false);
    }

    bool ok=true;
    if(hat){
        const bool okOwner=AV_V246SetOwnerNoSee(hat,true);
        const bool okHidden=AV_V249SetHatHiddenGame(hat,true);
        ok=okOwner || okHidden;
    }
    if(hat && ok) g_avV246HiddenHat=hat;

    static uintptr_t s_loggedHat=0;
    if(hat!=s_loggedHat || !ok){
        char l[288]{};
        sprintf_s(l,sizeof(l),
            "AV_V249_HAT_HIDE master=%p hat=%p hidden=%d\n",
            (void*)master,(void*)hat,ok?1:0);
        FP_Log(l);
        if(ok) s_loggedHat=hat;
    }
}

static int AV_V14FindBoneIndex(uintptr_t skeletalMesh, const char* wanted)
{
    if (!FP_V111PlausiblePtr(skeletalMesh) || !wanted ||
        !AV_V1ResolvePropertyOffset(skeletalMesh, "RefSkeleton", g_avV12RefSkeletonOffset))
        return -1;
    struct TArray64 { uintptr_t Data; int32_t Count; int32_t Max; } a{};
    if (!FP_ReadMemory((const void*)(skeletalMesh + (uintptr_t)g_avV12RefSkeletonOffset), &a, sizeof(a)) ||
        a.Count <= 0 || a.Count > 512 || !FP_V111PlausiblePtr(a.Data)) return -1;
    for (int i=0;i<a.Count;++i) {
        int32_t ni=-1; char n[128]{};
        if (FP_ReadMemory((const void*)(a.Data + (uintptr_t)i*0x60), &ni, sizeof(ni)) &&
            FP_V125RawNameFromIndex(ni,n,sizeof(n)) && !strcmp(n,wanted)) return i;
    }
    return -1;
}

