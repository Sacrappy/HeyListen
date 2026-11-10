// Optional bounded hardware check. The Python runner inserts the actual copy-layout functions at the marker.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL:%d %s\n",__LINE__,#x); std::exit(1); } } while (0)
#define APIS(X) X(vkCreateInstance) X(vkDestroyInstance) X(vkEnumerateInstanceLayerProperties) \
X(vkEnumerateInstanceExtensionProperties) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceQueue) \
X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateCommandPool) \
X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdFillBuffer) \
X(vkCreateFence) X(vkDestroyFence) X(vkQueueSubmit) X(vkWaitForFences)
#define DECLARE(n) PFN_##n n;
APIS(DECLARE)
struct Texture {};
struct NvnState { int64_t copy_stride=16; };
struct BufferRef { VkBuffer buffer{}; uint64_t offset=0,size=0; explicit operator bool() const { return buffer!=VK_NULL_HANDLE; } };
VkBuffer copy_buffer;
std::atomic<int> validation_errors{0};
void warn(int,const char* text) { std::fprintf(stderr,"%s\n",text); }
BufferRef gpu_buffer_bind(uint64_t addr,uint64_t size,bool) {
    return addr==0x1000 && size<=24 ? BufferRef{copy_buffer,0,size} : BufferRef{};
}
// PRODUCTION_COPY_LAYOUT

uint32_t memory_type(const VkPhysicalDeviceMemoryProperties& props,uint32_t bits,VkMemoryPropertyFlags required) {
    for(uint32_t i=0;i<props.memoryTypeCount;++i)
        if((bits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&required)==required) return i;
    return UINT32_MAX;
}
int skip(const char* reason) {
    std::printf("SKIP: %s\n",reason);
    return 0;
}
std::string lower(std::string s) {
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return (char)std::tolower(c);});
    return s;
}
int main(int argc,char** argv) {
    const char* requested_device=nullptr;
    for(int i=1;i<argc;++i) {
        if(std::strcmp(argv[i],"--device")==0 && i+1<argc) requested_device=argv[++i];
        else { std::fprintf(stderr,"usage: %s [--device NAME_SUBSTRING]\n",argv[0]); return 2; }
    }
#ifdef _WIN32
    auto loader=LoadLibraryW(L"vulkan-1.dll");
    auto symbol=[&](const char* name) { return loader ? (void*)GetProcAddress(loader,name) : nullptr; };
#else
    auto loader=dlopen("libvulkan.so.1",RTLD_NOW);
    auto symbol=[&](const char* name) { return loader ? dlsym(loader,name) : nullptr; };
#endif
    if(!loader) return skip("Vulkan loader unavailable");
    bool api_ready=true;
#define LOAD(n) n=reinterpret_cast<PFN_##n>(symbol(#n)); api_ready=api_ready && n;
    APIS(LOAD)
#undef LOAD
    auto gip=reinterpret_cast<PFN_vkGetInstanceProcAddr>(symbol("vkGetInstanceProcAddr"));
    if(!api_ready || !gip) {
#ifdef _WIN32
        FreeLibrary(loader);
#else
        dlclose(loader);
#endif
        return skip("required Vulkan entry points unavailable");
    }
    uint32_t nl=0; CHECK(vkEnumerateInstanceLayerProperties(&nl,nullptr)==VK_SUCCESS);
    std::vector<VkLayerProperties> layers_list(nl);
    CHECK(vkEnumerateInstanceLayerProperties(&nl,layers_list.data())==VK_SUCCESS);
    uint32_t ne=0; CHECK(vkEnumerateInstanceExtensionProperties(nullptr,&ne,nullptr)==VK_SUCCESS);
    std::vector<VkExtensionProperties> extensions(ne);
    CHECK(vkEnumerateInstanceExtensionProperties(nullptr,&ne,extensions.data())==VK_SUCCESS);
    bool validation=false,debug_utils=false;
    for(const auto& layer:layers_list) if(!std::strcmp(layer.layerName,"VK_LAYER_KHRONOS_validation")) validation=true;
    for(const auto& ext:extensions) if(!std::strcmp(ext.extensionName,VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) debug_utils=true;
    const char* layer="VK_LAYER_KHRONOS_validation";
    const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT db{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    db.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    db.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    db.pfnUserCallback=[](VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* data,void*) -> VkBool32 {
        if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            validation_errors.fetch_add(1,std::memory_order_relaxed);
            std::fprintf(stderr,"validation: %s\n",data->pMessage);
        }
        return VK_FALSE;
    };
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
    if(validation) { ici.enabledLayerCount=1;ici.ppEnabledLayerNames=&layer; }
    if(validation && debug_utils) { ici.enabledExtensionCount=1;ici.ppEnabledExtensionNames=&extension;ici.pNext=&db; }
    VkInstance instance{};
    const VkResult instance_result=vkCreateInstance(&ici,nullptr,&instance);
    if(instance_result==VK_ERROR_INCOMPATIBLE_DRIVER || instance_result==VK_ERROR_INITIALIZATION_FAILED) {
#ifdef _WIN32
        FreeLibrary(loader);
#else
        dlclose(loader);
#endif
        return skip("Vulkan instance or compatible driver unavailable");
    }
    CHECK(instance_result==VK_SUCCESS);
    VkDebugUtilsMessengerEXT messenger{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug=nullptr;
    if(validation && debug_utils) {
        auto create_debug=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(gip(instance,"vkCreateDebugUtilsMessengerEXT"));
        destroy_debug=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(gip(instance,"vkDestroyDebugUtilsMessengerEXT"));
        CHECK(create_debug && destroy_debug && create_debug(instance,&db,nullptr,&messenger)==VK_SUCCESS);
    }
    std::printf("Khronos validation: %s\n",validation ? (debug_utils ? "enabled" : "enabled; debug utils unavailable") : "unavailable");
    uint32_t np=0;
    const VkResult enumerate_result=vkEnumeratePhysicalDevices(instance,&np,nullptr);
    if(enumerate_result==VK_ERROR_INITIALIZATION_FAILED) {
        if(messenger) { destroy_debug(instance,messenger,nullptr); }
        vkDestroyInstance(instance,nullptr);
#ifdef _WIN32
        FreeLibrary(loader);
#else
        dlclose(loader);
#endif
        return skip("Vulkan physical-device enumeration unavailable");
    }
    CHECK(enumerate_result==VK_SUCCESS);
    if(!np) {
        if(messenger) destroy_debug(instance,messenger,nullptr);
        vkDestroyInstance(instance,nullptr);
#ifdef _WIN32
        FreeLibrary(loader);
#else
        dlclose(loader);
#endif
        return skip("no Vulkan physical devices found");
    }
    std::vector<VkPhysicalDevice> devices(np);
    CHECK(vkEnumeratePhysicalDevices(instance,&np,devices.data())==VK_SUCCESS);
    VkPhysicalDevice physical{}; VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{}; uint32_t family=UINT32_MAX;
    const std::string wanted=requested_device ? lower(requested_device) : std::string{};
    for(auto candidate:devices) {
        VkPhysicalDeviceProperties candidate_properties{};
        vkGetPhysicalDeviceProperties(candidate,&candidate_properties);
        if(candidate_properties.apiVersion < VK_API_VERSION_1_1) continue;
        if(!wanted.empty() && lower(candidate_properties.deviceName).find(wanted)==std::string::npos) continue;
        VkFormatProperties format{};
        vkGetPhysicalDeviceFormatProperties(candidate,VK_FORMAT_R8G8B8A8_UNORM,&format);
        const VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if((format.optimalTilingFeatures&needed)!=needed) continue;
        uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(candidate,&nq,nullptr);
        std::vector<VkQueueFamilyProperties> queues(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate,&nq,queues.data());
        uint32_t candidate_family=UINT32_MAX;
        for(uint32_t q=0;q<nq;++q)
            if(queues[q].queueCount && (queues[q].queueFlags&VK_QUEUE_TRANSFER_BIT)) { candidate_family=q;break; }
        if(candidate_family==UINT32_MAX) continue;
        VkPhysicalDeviceMemoryProperties candidate_memory{};
        vkGetPhysicalDeviceMemoryProperties(candidate,&candidate_memory);
        if(memory_type(candidate_memory,UINT32_MAX,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)==UINT32_MAX)
            continue;
        physical=candidate; properties=candidate_properties; memory=candidate_memory; family=candidate_family; break;
    }
    if(!physical) {
        if(messenger) destroy_debug(instance,messenger,nullptr);
        vkDestroyInstance(instance,nullptr);
#ifdef _WIN32
        FreeLibrary(loader);
#else
        dlclose(loader);
#endif
        return requested_device ? skip("no compatible Vulkan device matched the requested name")
                                : skip("no Vulkan device supports the required queue, format, and host memory");
    }
    std::printf("GPU: %s; Vulkan %u.%u.%u; driver %#x\n",properties.deviceName,VK_VERSION_MAJOR(properties.apiVersion),VK_VERSION_MINOR(properties.apiVersion),VK_VERSION_PATCH(properties.apiVersion),properties.driverVersion);
    float priority=1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;
    VkDevice device{};CHECK(vkCreateDevice(physical,&dci,nullptr,&device)==VK_SUCCESS);
    VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bci.size=24;bci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CHECK(vkCreateBuffer(device,&bci,nullptr,&copy_buffer)==VK_SUCCESS);
    VkMemoryRequirements br{};vkGetBufferMemoryRequirements(device,copy_buffer,&br);
    const uint32_t bmt=memory_type(memory,br.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(bmt==UINT32_MAX) {
        vkDestroyBuffer(device,copy_buffer,nullptr);vkDestroyDevice(device,nullptr);
        if(messenger) { destroy_debug(instance,messenger,nullptr); }
        vkDestroyInstance(instance,nullptr);
#ifdef _WIN32
        FreeLibrary(loader);
#else
        dlclose(loader);
#endif
        return skip("selected device has no coherent host-visible memory for the copy buffer");
    }
    VkDeviceMemory bm{};VkMemoryAllocateInfo ba{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ba.allocationSize=br.size;ba.memoryTypeIndex=bmt;
    CHECK(vkAllocateMemory(device,&ba,nullptr,&bm)==VK_SUCCESS);CHECK(vkBindBufferMemory(device,copy_buffer,bm,0)==VK_SUCCESS);
    void* mapped=nullptr;CHECK(vkMapMemory(device,bm,0,24,0,&mapped)==VK_SUCCESS);
    const uint32_t input[]={0xff0000ff,0xff00ff00,0,0,0xffff0000,0xffffffff};std::memcpy(mapped,input,24);
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image_info.imageType=VK_IMAGE_TYPE_2D;image_info.format=VK_FORMAT_R8G8B8A8_UNORM;image_info.extent={2,2,1};image_info.mipLevels=1;image_info.arrayLayers=1;image_info.samples=VK_SAMPLE_COUNT_1_BIT;image_info.tiling=VK_IMAGE_TILING_OPTIMAL;image_info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image{};CHECK(vkCreateImage(device,&image_info,nullptr,&image)==VK_SUCCESS);
    VkMemoryRequirements ir{};vkGetImageMemoryRequirements(device,image,&ir);
    const uint32_t imt=memory_type(memory,ir.memoryTypeBits,0);
    if(imt==UINT32_MAX) {
        vkDestroyImage(device,image,nullptr);vkUnmapMemory(device,bm);vkDestroyBuffer(device,copy_buffer,nullptr);vkFreeMemory(device,bm,nullptr);vkDestroyDevice(device,nullptr);
        if(messenger) { destroy_debug(instance,messenger,nullptr); }
        vkDestroyInstance(instance,nullptr);
#ifdef _WIN32
        FreeLibrary(loader);
#else
        dlclose(loader);
#endif
        return skip("selected device has no compatible image memory type");
    }
    VkDeviceMemory im{};VkMemoryAllocateInfo ia{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ia.allocationSize=ir.size;ia.memoryTypeIndex=imt;
    CHECK(vkAllocateMemory(device,&ia,nullptr,&im)==VK_SUCCESS);CHECK(vkBindImageMemory(device,image,im,0)==VK_SUCCESS);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=family;VkCommandPool pool{};CHECK(vkCreateCommandPool(device,&pci,nullptr,&pool)==VK_SUCCESS);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;VkCommandBuffer cmd{};CHECK(vkAllocateCommandBuffers(device,&cai,&cmd)==VK_SUCCESS);
    NvnState state;Sub s;s.w=s.h=2;s.d=s.layers=1;s.bpb=4;s.aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    uint64_t size=0;VkBufferImageCopy copy{};BufferRef span;
    CHECK(buffer_side(state,s,0x1000,size,copy,span,false) && size==24);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(cmd,&begin)==VK_SUCCESS);
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;ib.newLayout=VK_IMAGE_LAYOUT_GENERAL;ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ib.image=image;ib.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};ib.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
    vkCmdCopyBufferToImage(cmd,copy_buffer,image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,nullptr,0,nullptr);
    vkCmdFillBuffer(cmd,copy_buffer,0,24,0x12345678);
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,nullptr,0,nullptr);
    CHECK(buffer_side(state,s,0x1000,size,copy,span,true) && size==24);
    vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_GENERAL,copy_buffer,1,&copy);
    mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);
    CHECK(vkEndCommandBuffer(cmd)==VK_SUCCESS);VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence{};CHECK(vkCreateFence(device,&fci,nullptr,&fence)==VK_SUCCESS);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;CHECK(vkQueueSubmit(queue,1,&submit,fence)==VK_SUCCESS);
    CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull)==VK_SUCCESS);
    const uint32_t expected[]={input[0],input[1],0x12345678,0x12345678,input[4],input[5]};CHECK(!std::memcmp(mapped,expected,24));
    vkDestroyFence(device,fence,nullptr);vkDestroyCommandPool(device,pool,nullptr);vkUnmapMemory(device,bm);vkDestroyBuffer(device,copy_buffer,nullptr);vkFreeMemory(device,bm,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,im,nullptr);vkDestroyDevice(device,nullptr);
    if(messenger) { destroy_debug(instance,messenger,nullptr); }
    vkDestroyInstance(instance,nullptr);
#ifdef _WIN32
    FreeLibrary(loader);
#else
    dlclose(loader);
#endif
    CHECK(validation_errors.load(std::memory_order_relaxed)==0);
    std::puts("PASS: 24-byte production copy layout, four GPU texels correct and padding untouched");
}
