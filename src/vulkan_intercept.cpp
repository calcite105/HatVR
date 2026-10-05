#include "vulkan_intercept.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <unordered_map>

namespace ahitvr
{
namespace
{
    using PFN_vkAllocateMemory_T = VkResult (VKAPI_PTR*)(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory*);
    using PFN_vkFreeMemory_T = void (VKAPI_PTR*)(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*);
    using PFN_vkCreateImage_T = VkResult (VKAPI_PTR*)(VkDevice,const VkImageCreateInfo*,const VkAllocationCallbacks*,VkImage*);
    using PFN_vkDestroyImage_T = void (VKAPI_PTR*)(VkDevice,VkImage,const VkAllocationCallbacks*);
    using PFN_vkBindImageMemory_T = VkResult (VKAPI_PTR*)(VkDevice,VkImage,VkDeviceMemory,VkDeviceSize);
    using PFN_vkBindImageMemory2_T = VkResult (VKAPI_PTR*)(VkDevice,uint32_t,const VkBindImageMemoryInfo*);
    using PFN_vkCreateDevice_T = VkResult (VKAPI_PTR*)(VkPhysicalDevice,const VkDeviceCreateInfo*,const VkAllocationCallbacks*,VkDevice*);
    using PFN_vkDestroyDevice_T = void (VKAPI_PTR*)(VkDevice,const VkAllocationCallbacks*);
    using PFN_vkGetDeviceQueue_T = void (VKAPI_PTR*)(VkDevice,uint32_t,uint32_t,VkQueue*);

    PFN_vkAllocateMemory_T g_realAllocateMemory=nullptr;
    PFN_vkFreeMemory_T g_realFreeMemory=nullptr;
    PFN_vkCreateImage_T g_realCreateImage=nullptr;
    PFN_vkDestroyImage_T g_realDestroyImage=nullptr;
    PFN_vkCreateDevice_T g_realCreateDevice=nullptr;
    PFN_vkDestroyDevice_T g_realDestroyDevice=nullptr;
    PFN_vkGetDeviceQueue_T g_realGetDeviceQueue=nullptr;

    // function pointers obtained from vkGetDeviceProcAddr, bypassing the loader
    // exports hooked above. On a given ICD these entry points are shared by
    // devices, so resolving them from HatVR's NVIDIA device lets us observe
    // later DXVK image/allocation activity without modifying DXVK itself.
    PFN_vkAllocateMemory_T g_dispatchAllocateMemory=nullptr;
    PFN_vkFreeMemory_T g_dispatchFreeMemory=nullptr;
    PFN_vkCreateImage_T g_dispatchCreateImage=nullptr;
    PFN_vkDestroyImage_T g_dispatchDestroyImage=nullptr;
    PFN_vkBindImageMemory_T g_dispatchBindImageMemory=nullptr;
    PFN_vkBindImageMemory2_T g_dispatchBindImageMemory2=nullptr;

    std::atomic<bool> g_started{false};
    std::atomic<bool> g_dispatchStarted{false};
    std::atomic<VkDevice> g_hatvrDevice{VK_NULL_HANDLE};
    std::atomic<VkQueue> g_hatvrQueue{VK_NULL_HANDLE};
    std::atomic<unsigned long long> g_allocSeq{0},g_imageSeq{0},g_dispatchImageSeq{0};
    std::mutex g_stateMutex;
    struct DevStats { unsigned long long liveBytes=0,peakBytes=0,allocs=0,frees=0,images=0; };
    struct ImageInfo { uint32_t w=0,h=0; VkFormat format=VK_FORMAT_UNDEFINED; VkImageUsageFlags usage=0; };
    std::unordered_map<VkDevice,DevStats> g_devStats;
    std::unordered_map<VkDeviceMemory,VkDeviceSize> g_memSizes;
    std::unordered_map<VkImage,ImageInfo> g_images;

    struct D3D9CorrelationScope
    {
        bool active=false;
        const char* label=nullptr;
        uint32_t width=0,height=0,d3dFormat=0;
        unsigned long long serial=0;
    };
    thread_local D3D9CorrelationScope g_d3d9Scope{};
    std::atomic<unsigned long long> g_d3d9CorrelationSerial{0};

    const char* DevLabel(VkDevice d){ return d&&d==g_hatvrDevice.load(std::memory_order_acquire)?"HATVR-XR":"DXVK-CANDIDATE"; }
    bool InterestingImage(const VkImageCreateInfo* ci){ return ci && ((ci->extent.width==2688&&ci->extent.height==1440)||(ci->extent.width==1344&&ci->extent.height==1440)||(ci->extent.width==1600&&ci->extent.height==900)); }

    void Trace(const char* fmt,...)
    {
        char line[1792]{}; va_list a; va_start(a,fmt); _vsnprintf_s(line,sizeof(line),_TRUNCATE,fmt,a); va_end(a);
        HMODULE self=nullptr; char path[MAX_PATH]{};
        if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCSTR>(&Trace),&self)||!GetModuleFileNameA(self,path,MAX_PATH))return;
        char* slash=nullptr; for(char* p=path;*p;++p)if(*p=='\\'||*p=='/')slash=p; if(!slash)return; *(slash+1)=0; lstrcatA(path,"AHiTVR_VKBRIDGE_V5.log");
        HANDLE f=CreateFileA(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr); if(f==INVALID_HANDLE_VALUE)return;
        DWORD n=0; WriteFile(f,line,(DWORD)lstrlenA(line),&n,nullptr); WriteFile(f,"\r\n",2,&n,nullptr); CloseHandle(f);
    }

    void AccountAlloc(VkDevice d,VkDeviceMemory m,VkDeviceSize size,uint32_t type,const char* path)
    {
        unsigned long long live=0,peak=0,seq=++g_allocSeq;
        {std::lock_guard<std::mutex> l(g_stateMutex);auto&s=g_devStats[d];s.allocs++;s.liveBytes+=(unsigned long long)size;if(s.liveBytes>s.peakBytes)s.peakBytes=s.liveBytes;live=s.liveBytes;peak=s.peakBytes;g_memSizes[m]=size;}
        Trace("VKBRIDGE-V5 %s-ALLOC #%llu device=%p class=%s memory=%p sizeMB=%.2f type=%u liveMB=%.2f peakMB=%.2f",path,seq,d,DevLabel(d),m,(double)size/1048576.0,type,(double)live/1048576.0,(double)peak/1048576.0);
    }
    void AccountFree(VkDevice d,VkDeviceMemory m,const char* path)
    {
        VkDeviceSize sz=0;{std::lock_guard<std::mutex>l(g_stateMutex);auto it=g_memSizes.find(m);if(it!=g_memSizes.end()){sz=it->second;g_memSizes.erase(it);}auto&s=g_devStats[d];s.frees++;s.liveBytes=s.liveBytes>=sz?s.liveBytes-(unsigned long long)sz:0;}
        Trace("VKBRIDGE-V5 %s-FREE device=%p class=%s memory=%p sizeMB=%.2f",path,d,DevLabel(d),m,(double)sz/1048576.0);
    }
    void RecordImage(VkDevice d,VkImage image,const VkImageCreateInfo* ci,const char* path)
    {
        unsigned long long seq=++g_imageSeq;{std::lock_guard<std::mutex>l(g_stateMutex);g_devStats[d].images++;g_images[image]={ci->extent.width,ci->extent.height,ci->format,ci->usage};}
        Trace("VKBRIDGE-V5 %s-IMAGE #%llu device=%p class=%s image=%p %ux%ux%u format=%d usage=0x%08X samples=%u tiling=%u%s",path,seq,d,DevLabel(d),image,ci->extent.width,ci->extent.height,ci->extent.depth,(int)ci->format,(unsigned)ci->usage,(unsigned)ci->samples,(unsigned)ci->tiling,InterestingImage(ci)?" ***CANDIDATE***":"");
        if(g_d3d9Scope.active)
            Trace("VKBRIDGE-V5 D3D9-CORRELATE serial=%llu label=%s requested=%ux%u d3dfmt=%u -> vkImage=%p vk=%ux%u vkfmt=%d usage=0x%08X device=%p class=%s",
                g_d3d9Scope.serial,g_d3d9Scope.label?g_d3d9Scope.label:"?",g_d3d9Scope.width,g_d3d9Scope.height,g_d3d9Scope.d3dFormat,
                image,ci->extent.width,ci->extent.height,(int)ci->format,(unsigned)ci->usage,d,DevLabel(d));
    }

    VkResult VKAPI_PTR HookAllocateMemory(VkDevice d,const VkMemoryAllocateInfo* ai,const VkAllocationCallbacks* ac,VkDeviceMemory* out){VkResult r=g_realAllocateMemory(d,ai,ac,out);if(r==VK_SUCCESS&&ai&&out&&*out)AccountAlloc(d,*out,ai->allocationSize,ai->memoryTypeIndex,"EXPORT");return r;}
    void VKAPI_PTR HookFreeMemory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks* ac){AccountFree(d,m,"EXPORT");g_realFreeMemory(d,m,ac);}
    VkResult VKAPI_PTR HookCreateImage(VkDevice d,const VkImageCreateInfo* ci,const VkAllocationCallbacks* ac,VkImage* out){VkResult r=g_realCreateImage(d,ci,ac,out);if(r==VK_SUCCESS&&ci&&out&&*out)RecordImage(d,*out,ci,"EXPORT");return r;}
    void VKAPI_PTR HookDestroyImage(VkDevice d,VkImage i,const VkAllocationCallbacks* ac){{std::lock_guard<std::mutex>l(g_stateMutex);g_images.erase(i);}Trace("VKBRIDGE-V5 EXPORT-IMAGE-DESTROY device=%p class=%s image=%p",d,DevLabel(d),i);g_realDestroyImage(d,i,ac);}
    VkResult VKAPI_PTR HookCreateDevice(VkPhysicalDevice p,const VkDeviceCreateInfo* ci,const VkAllocationCallbacks* ac,VkDevice* out){VkResult r=g_realCreateDevice(p,ci,ac,out);Trace("VKBRIDGE-V5 CREATE-DEVICE physical=%p result=%d device=%p queues=%u extensions=%u",p,(int)r,(r==VK_SUCCESS&&out)?*out:VK_NULL_HANDLE,ci?ci->queueCreateInfoCount:0,ci?ci->enabledExtensionCount:0);return r;}
    void VKAPI_PTR HookDestroyDevice(VkDevice d,const VkAllocationCallbacks* ac){DevStats s{};{std::lock_guard<std::mutex>l(g_stateMutex);auto it=g_devStats.find(d);if(it!=g_devStats.end())s=it->second;}Trace("VKBRIDGE-V5 DESTROY-DEVICE device=%p class=%s liveMB=%.2f peakMB=%.2f allocs=%llu frees=%llu images=%llu",d,DevLabel(d),(double)s.liveBytes/1048576.0,(double)s.peakBytes/1048576.0,s.allocs,s.frees,s.images);g_realDestroyDevice(d,ac);}
    void VKAPI_PTR HookGetDeviceQueue(VkDevice d,uint32_t f,uint32_t i,VkQueue*q){g_realGetDeviceQueue(d,f,i,q);Trace("VKBRIDGE-V5 GET-QUEUE device=%p class=%s family=%u index=%u queue=%p",d,DevLabel(d),f,i,q?*q:VK_NULL_HANDLE);}

    VkResult VKAPI_PTR HookDispatchAllocateMemory(VkDevice d,const VkMemoryAllocateInfo* ai,const VkAllocationCallbacks* ac,VkDeviceMemory*out){VkResult r=g_dispatchAllocateMemory(d,ai,ac,out);if(r==VK_SUCCESS&&ai&&out&&*out)AccountAlloc(d,*out,ai->allocationSize,ai->memoryTypeIndex,"DISPATCH");return r;}
    void VKAPI_PTR HookDispatchFreeMemory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks*ac){AccountFree(d,m,"DISPATCH");g_dispatchFreeMemory(d,m,ac);}
    VkResult VKAPI_PTR HookDispatchCreateImage(VkDevice d,const VkImageCreateInfo*ci,const VkAllocationCallbacks*ac,VkImage*out){VkResult r=g_dispatchCreateImage(d,ci,ac,out);if(r==VK_SUCCESS&&ci&&out&&*out){++g_dispatchImageSeq;RecordImage(d,*out,ci,"DISPATCH");}return r;}
    void VKAPI_PTR HookDispatchDestroyImage(VkDevice d,VkImage i,const VkAllocationCallbacks*ac){{std::lock_guard<std::mutex>l(g_stateMutex);g_images.erase(i);}Trace("VKBRIDGE-V5 DISPATCH-IMAGE-DESTROY device=%p class=%s image=%p",d,DevLabel(d),i);g_dispatchDestroyImage(d,i,ac);}
    VkResult VKAPI_PTR HookDispatchBindImageMemory(VkDevice d,VkImage i,VkDeviceMemory m,VkDeviceSize off){VkResult r=g_dispatchBindImageMemory(d,i,m,off);ImageInfo info{};bool have=false;{std::lock_guard<std::mutex>l(g_stateMutex);auto it=g_images.find(i);if(it!=g_images.end()){info=it->second;have=true;}}if(have&&(info.w==2688||info.w==1344||info.w==1600))Trace("VKBRIDGE-V5 BIND-CANDIDATE device=%p class=%s image=%p %ux%u memory=%p offset=%llu result=%d",d,DevLabel(d),i,info.w,info.h,m,(unsigned long long)off,(int)r);return r;}
    VkResult VKAPI_PTR HookDispatchBindImageMemory2(VkDevice d,uint32_t count,const VkBindImageMemoryInfo* infos){VkResult r=g_dispatchBindImageMemory2(d,count,infos);for(uint32_t n=0;infos&&n<count;++n){ImageInfo info{};bool have=false;{std::lock_guard<std::mutex>l(g_stateMutex);auto it=g_images.find(infos[n].image);if(it!=g_images.end()){info=it->second;have=true;}}if(have&&(info.w==2688||info.w==1344||info.w==1600))Trace("VKBRIDGE-V5 BIND2-CANDIDATE device=%p class=%s image=%p %ux%u memory=%p offset=%llu result=%d",d,DevLabel(d),infos[n].image,info.w,info.h,infos[n].memory,(unsigned long long)infos[n].memoryOffset,(int)r);}return r;}

    bool AddHook(HMODULE vk,const char*name,void*hook,void**orig){void*t=(void*)GetProcAddress(vk,name);if(!t){Trace("VKBRIDGE-V5 hook target missing %s",name);return false;}MH_STATUS c=MH_CreateHook(t,hook,orig);if(c!=MH_OK&&c!=MH_ERROR_ALREADY_CREATED){Trace("VKBRIDGE-V5 MH_CreateHook %s=%d",name,(int)c);return false;}MH_STATUS e=MH_EnableHook(t);bool ok=e==MH_OK||e==MH_ERROR_ENABLED;Trace("VKBRIDGE-V5 hook %s target=%p create=%d enable=%d active=%u",name,t,(int)c,(int)e,ok?1:0);return ok;}
    bool AddAddressHook(const char*name,void*target,void*hook,void**orig){if(!target){Trace("VKBRIDGE-V5 dispatch target missing %s",name);return false;}MH_STATUS c=MH_CreateHook(target,hook,orig);if(c==MH_ERROR_ALREADY_CREATED){Trace("VKBRIDGE-V5 dispatch %s target=%p already hooked; export path covers it",name,target);return true;}if(c!=MH_OK){Trace("VKBRIDGE-V5 dispatch MH_CreateHook %s target=%p status=%d",name,target,(int)c);return false;}MH_STATUS e=MH_EnableHook(target);bool ok=e==MH_OK||e==MH_ERROR_ENABLED;Trace("VKBRIDGE-V5 dispatch hook %s target=%p create=%d enable=%d active=%u",name,target,(int)c,(int)e,ok?1:0);return ok;}

    void InstallDeviceDispatchHooks(VkDevice device)
    {
        bool expected=false;if(!g_dispatchStarted.compare_exchange_strong(expected,true))return;
        HMODULE vk=GetModuleHandleW(L"vulkan-1.dll");if(!vk){Trace("VKBRIDGE-V5 PHASE2 no vulkan loader");return;}
        auto gdpa=(PFN_vkGetDeviceProcAddr)GetProcAddress(vk,"vkGetDeviceProcAddr");if(!gdpa){Trace("VKBRIDGE-V5 PHASE2 vkGetDeviceProcAddr missing");return;}
        Trace("VKBRIDGE-V5 PHASE2 DISPATCH-SCAN device=%p",device);
        void*a=(void*)gdpa(device,"vkAllocateMemory"),*f=(void*)gdpa(device,"vkFreeMemory"),*ci=(void*)gdpa(device,"vkCreateImage"),*di=(void*)gdpa(device,"vkDestroyImage"),*bi=(void*)gdpa(device,"vkBindImageMemory"),*bi2=(void*)gdpa(device,"vkBindImageMemory2");
        Trace("VKBRIDGE-V5 PHASE2 TARGETS alloc=%p free=%p createImage=%p destroyImage=%p bind=%p bind2=%p",a,f,ci,di,bi,bi2);
        AddAddressHook("vkAllocateMemory",a,(void*)&HookDispatchAllocateMemory,(void**)&g_dispatchAllocateMemory);
        AddAddressHook("vkFreeMemory",f,(void*)&HookDispatchFreeMemory,(void**)&g_dispatchFreeMemory);
        AddAddressHook("vkCreateImage",ci,(void*)&HookDispatchCreateImage,(void**)&g_dispatchCreateImage);
        AddAddressHook("vkDestroyImage",di,(void*)&HookDispatchDestroyImage,(void**)&g_dispatchDestroyImage);
        AddAddressHook("vkBindImageMemory",bi,(void*)&HookDispatchBindImageMemory,(void**)&g_dispatchBindImageMemory);
        if(bi2)AddAddressHook("vkBindImageMemory2",bi2,(void*)&HookDispatchBindImageMemory2,(void**)&g_dispatchBindImageMemory2);
        Trace("VKBRIDGE-V5 PHASE2 READY - watching device-dispatch Vulkan calls for DXVK candidates");
    }
}

bool StartVulkanGpuBridgeTrace()
{
    // created AHiTVR_VKBRIDGE_V5.log and could grow it by hundreds of MB.
    return false;
}

void VulkanTraceMarkHatVRDevice(VkDevice, VkQueue, uint32_t)
{
    // Intentionally inert. Do NOT call Trace() or install dispatch hooks here.
}

void VulkanTraceBeginD3D9Resource(const char*, uint32_t, uint32_t, uint32_t)
{
    // Intentionally inert.
}

void VulkanTraceEndD3D9Resource(const char*, long, const void*)
{
    // Intentionally inert.
}

} // namespace ahitvr
