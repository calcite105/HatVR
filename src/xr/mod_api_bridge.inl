// current build: GameModInstances lives at these EXE offsets.
// discovery reads that list directly instead of asking the script side to enumerate mods.

static constexpr int kHatVrModApiVersion = 1;
static constexpr int kHatVrModApiCapabilities = 0x1F;
static constexpr uintptr_t kHatVrGameModInstancesDataRva = 0x129AC80;
static constexpr uintptr_t kHatVrGameModInstancesCountRva = 0x129AC88;

struct HatVrApiInitParams { int32_t ApiVersion; int32_t Capabilities; };
struct HatVrApiStateParams { int32_t ApiVersion; int32_t StateFlags; int32_t TrackingMask; };
struct HatVrApiPoseParams { int32_t Device; int32_t Valid; float X,Y,Z; float Qx,Qy,Qz,Qw; int32_t Pitch,Yaw,Roll; };
struct HatVrApiConsumer { uintptr_t Object=0, InitFn=0, StateFn=0; uintptr_t FrameFn[3]{}; uintptr_t Hz20Fn[3]{}; };
static HatVrApiConsumer g_hatVrApiConsumers[32]{};
static int g_hatVrApiConsumerCount=0;
static ULONGLONG g_hatVrApiLastDiscoveryMs=0, g_hatVrApiLast20HzMs=0;
static uint64_t g_hatVrApiLastFrame=~uint64_t(0);
static int32_t g_hatVrApiLastSeenGameModCount=-1, g_hatVrApiLastStateFlags=-1, g_hatVrApiLastTrackingMask=-1;
// api callback names may not exist in GNames at all. walk each new GameMod class once
// instead of repeatedly searching the global name table.
static uintptr_t g_hatVrApiRejected[128]{}; static int g_hatVrApiRejectedCount=0;

static bool HatVrApiReadFieldName(uintptr_t field,char* out,size_t cap){
    if(!out||cap<2||!FP_V111PlausiblePtr(field))return false;out[0]=0;
    int32_t ni=-1;if(!FP_V123ObjectRawNameIndex(field,ni)||ni<0)return false;
    const uintptr_t exe=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t names=0;if(!exe||!FP_V18ReadPtr(exe+0x125CCE8,names)||!FP_V111PlausiblePtr(names))return false;
    uintptr_t entry=0;if(!FP_V18ReadPtr(names+uintptr_t(ni)*8,entry)||!FP_V111PlausiblePtr(entry))return false;
    const size_t n=(std::min)(cap-1,size_t(127));
    if(!FP_ReadMemory(reinterpret_cast<const void*>(entry+0x14),out,n))return false;
    out[n]=0;return out[0]!=0;
}

static int HatVrApiCallbackSlot(const char* name){
    if(!name)return -1;
    static const char* k[8]={"HatVRAPI_Initialize","HatVRAPI_StateChanged","HatVRAPI_HMDFrame","HatVRAPI_LeftControllerFrame","HatVRAPI_RightControllerFrame","HatVRAPI_HMD20Hz","HatVRAPI_LeftController20Hz","HatVRAPI_RightController20Hz"};
    for(int i=0;i<8;++i)if(strcmp(name,k[i])==0)return i;return -1;
}

static void HatVrApiScanCallbacks(uintptr_t object,uintptr_t fn[8]){
    for(int i=0;i<8;++i)fn[i]=0;if(!FP_V111PlausiblePtr(object))return;
    uintptr_t cls=0;if(!FP_V110ObjectClass(object,cls))return;
    for(int depth=0;FP_V111PlausiblePtr(cls)&&depth<32;++depth){
        uintptr_t children=0,super=0;FP_V18ReadPtr(cls+0x80,children);FP_V18ReadPtr(cls+0x78,super);
        if(FP_V111PlausiblePtr(children)){
            uintptr_t queue[8192]{};int qr=0,qw=0;queue[qw++]=children;
            while(qr<qw&&qr<8192){
                const uintptr_t field=queue[qr++];if(!FP_V111PlausiblePtr(field))continue;
                bool dup=false;for(int i=0;i<qr-1;++i)if(queue[i]==field){dup=true;break;}if(dup)continue;
                char name[128]{};if(HatVrApiReadFieldName(field,name,sizeof(name))){const int slot=HatVrApiCallbackSlot(name);if(slot>=0&&!fn[slot])fn[slot]=field;}
                const unsigned int offs[2]={0x58,0x60};for(unsigned int off:offs){uintptr_t next=0;if(FP_V18ReadPtr(field+off,next)&&FP_V111PlausiblePtr(next)&&next!=field&&qw<8192)queue[qw++]=next;}
            }
        }
        if(!FP_V111PlausiblePtr(super)||super==cls)break;cls=super;
    }
}

static void HatVrApiRemoveConsumer(uintptr_t object){for(int i=0;i<g_hatVrApiConsumerCount;++i)if(g_hatVrApiConsumers[i].Object==object){g_hatVrApiConsumers[i]=g_hatVrApiConsumers[g_hatVrApiConsumerCount-1];g_hatVrApiConsumers[--g_hatVrApiConsumerCount]={};return;}}

static void HatVrApiRegisterConsumer(uintptr_t object){
    if(!g_hatVrModApiEnabled||!FP_V111PlausiblePtr(object))return;
    for(int i=0;i<g_hatVrApiConsumerCount;++i)if(g_hatVrApiConsumers[i].Object==object)return;
    for(int i=0;i<g_hatVrApiRejectedCount;++i)if(g_hatVrApiRejected[i]==object)return;
    uintptr_t fn[8]{};HatVrApiScanCallbacks(object,fn);if(!fn[0]){if(g_hatVrApiRejectedCount<int(_countof(g_hatVrApiRejected)))g_hatVrApiRejected[g_hatVrApiRejectedCount++]=object;return;}
    if(g_hatVrApiConsumerCount>=int(_countof(g_hatVrApiConsumers)))return;
    HatVrApiConsumer c{};c.Object=object;c.InitFn=fn[0];c.StateFn=fn[1];
    for(int d=0;d<3;++d){c.FrameFn[d]=fn[2+d];c.Hz20Fn[d]=fn[5+d];}
    g_hatVrApiConsumers[g_hatVrApiConsumerCount++]=c;
    HatVrApiInitParams ip{kHatVrModApiVersion,kHatVrModApiCapabilities};FP_V127CallUFunctionParams(object,c.InitFn,&ip);
    if(c.StateFn){int32_t m=(g_xrViewsValidThisFrame&&g_xrTrackingOriginSet?1:0)|(g_controllerGripPoseValid[0]&&g_xrTrackingOriginSet?2:0)|(g_controllerGripPoseValid[1]&&g_xrTrackingOriginSet?4:0);int32_t sf=(g_xrSessionRunning?1:0)|((g_fpV1Enabled&&g_avV29ProbeEnabled)?2:0);HatVrApiStateParams sp{kHatVrModApiVersion,sf,m};FP_V127CallUFunctionParams(object,c.StateFn,&sp);}
    LogCategory("MODAPI","Registered consumer=%p frame=%d%d%d 20Hz=%d%d%d",reinterpret_cast<void*>(object),c.FrameFn[0]!=0,c.FrameFn[1]!=0,c.FrameFn[2]!=0,c.Hz20Fn[0]!=0,c.Hz20Fn[1]!=0,c.Hz20Fn[2]!=0);
}

static void HatVrApiDiscoverConsumers(){
    if(!g_hatVrModApiEnabled)return;
    const ULONGLONG now=GetTickCount64();if(now-g_hatVrApiLastDiscoveryMs<2000ULL&&g_hatVrApiLastSeenGameModCount>=0)return;g_hatVrApiLastDiscoveryMs=now;
    const uintptr_t exe=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));if(!exe)return;
    uintptr_t data=0;int32_t count=0;if(!FP_V18ReadPtr(exe+kHatVrGameModInstancesDataRva,data)||!FP_V18ReadI32(exe+kHatVrGameModInstancesCountRva,count)||count<0||count>4096)return;
    if(count!=g_hatVrApiLastSeenGameModCount){g_hatVrApiRejectedCount=0;for(auto& r:g_hatVrApiRejected)r=0;}
    g_hatVrApiLastSeenGameModCount=count;
    for(int i=g_hatVrApiConsumerCount-1;i>=0;--i){bool alive=false;for(int j=0;j<count;++j){uintptr_t o=0;if(FP_V18ReadPtr(data+uintptr_t(j)*8,o)&&o==g_hatVrApiConsumers[i].Object){alive=true;break;}}if(!alive)HatVrApiRemoveConsumer(g_hatVrApiConsumers[i].Object);}
    for(int j=0;j<count;++j){uintptr_t o=0;if(FP_V18ReadPtr(data+uintptr_t(j)*8,o)&&FP_V111PlausiblePtr(o))HatVrApiRegisterConsumer(o);}
}

static void HatVrApiDisable(){g_hatVrApiConsumerCount=0;for(auto& c:g_hatVrApiConsumers)c={};g_hatVrApiRejectedCount=0;for(auto& r:g_hatVrApiRejected)r=0;g_hatVrApiLastSeenGameModCount=-1;g_hatVrApiLastDiscoveryMs=0;}

static HatVrApiPoseParams HatVrApiBuildPose(int d){HatVrApiPoseParams p{};p.Device=d;p.Qw=1.0f;XrPosef x{};x.orientation.w=1.0f;if(d==0){p.Valid=(g_xrViewsValidThisFrame&&g_xrTrackingOriginSet)?1:0;if(p.Valid){x.position.x=(g_xrViews[0].pose.position.x+g_xrViews[1].pose.position.x)*.5f;x.position.y=(g_xrViews[0].pose.position.y+g_xrViews[1].pose.position.y)*.5f;x.position.z=(g_xrViews[0].pose.position.z+g_xrViews[1].pose.position.z)*.5f;x.orientation=g_xrViews[0].pose.orientation;x=XrPoseRelativeToTrackingOrigin(x);}}else{int h=d-1;p.Valid=(h>=0&&h<2&&g_controllerGripPoseValid[h]&&g_xrTrackingOriginSet)?1:0;if(p.Valid)x=XrPoseRelativeToTrackingOrigin(g_controllerGripLocations[h].pose);}if(p.Valid){p.X=-x.position.z*g_xrWorldUnitsPerMeter;p.Y=x.position.x*g_xrWorldUnitsPerMeter;p.Z=x.position.y*g_xrWorldUnitsPerMeter;const XrQuaternionf& q=x.orientation;const float sp=2.0f*(q.w*q.x-q.y*q.z);const float pr=asinf((std::max)(-1.0f,(std::min)(1.0f,sp)));const float yr=atan2f(2.0f*(q.w*q.y+q.x*q.z),1.0f-2.0f*(q.x*q.x+q.y*q.y));const float rr=atan2f(2.0f*(q.w*q.z+q.x*q.y),1.0f-2.0f*(q.x*q.x+q.z*q.z));constexpr float rd=57.29577951308232f;p.Pitch=DegreesToUnrealRotator(pr*rd);p.Yaw=DegreesToUnrealRotator(-yr*rd);p.Roll=DegreesToUnrealRotator(-rr*rd);if(d>0&&g_avV250FinalHandValid[d-1]){const XrQuaternionf& hq=g_avV250FinalHandQ[d-1];p.Qx=hq.x;p.Qy=hq.y;p.Qz=hq.z;p.Qw=hq.w;}else{p.Qx=q.x;p.Qy=q.y;p.Qz=q.z;p.Qw=q.w;}}return p;}

static void HatVrApiDeliverTracking(void*){
    if(!g_hatVrModApiEnabled){HatVrApiDisable();return;}HatVrApiDiscoverConsumers();if(g_hatVrApiConsumerCount<=0)return;
    if(g_hatVrApiLastFrame==g_presentFrameNumber)return;g_hatVrApiLastFrame=g_presentFrameNumber;
    const ULONGLONG now=GetTickCount64();const bool send20=(now-g_hatVrApiLast20HzMs)>=50ULL;if(send20)g_hatVrApiLast20HzMs=now;
    const int32_t tm=(g_xrViewsValidThisFrame&&g_xrTrackingOriginSet?1:0)|(g_controllerGripPoseValid[0]&&g_xrTrackingOriginSet?2:0)|(g_controllerGripPoseValid[1]&&g_xrTrackingOriginSet?4:0);const int32_t sf=(g_xrSessionRunning?1:0)|((g_fpV1Enabled&&g_avV29ProbeEnabled)?2:0);
    if(sf!=g_hatVrApiLastStateFlags||tm!=g_hatVrApiLastTrackingMask){g_hatVrApiLastStateFlags=sf;g_hatVrApiLastTrackingMask=tm;HatVrApiStateParams sp{kHatVrModApiVersion,sf,tm};for(int i=0;i<g_hatVrApiConsumerCount;++i)if(g_hatVrApiConsumers[i].StateFn)FP_V127CallUFunctionParams(g_hatVrApiConsumers[i].Object,g_hatVrApiConsumers[i].StateFn,&sp);}
    HatVrApiPoseParams pose[3];bool built[3]={false,false,false};for(int i=0;i<g_hatVrApiConsumerCount;++i){auto& c=g_hatVrApiConsumers[i];for(int d=0;d<3;++d){uintptr_t fn=c.FrameFn[d];if(!fn&&send20)fn=c.Hz20Fn[d];if(!fn)continue;if(!built[d]){pose[d]=HatVrApiBuildPose(d);built[d]=true;}FP_V127CallUFunctionParams(c.Object,fn,&pose[d]);}}
}

