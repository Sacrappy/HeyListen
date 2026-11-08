#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#define putenv_local _putenv_s
#else
#define putenv_local(name, value) setenv(name, value, 1)
#endif

#include "shaders_spv.h"

namespace {

constexpr std::uint32_t kDescriptorsPerDraw = 30;
constexpr std::uint32_t kBindlessSlots = 32;
constexpr std::uint32_t kTextureViews = 8;
constexpr std::uint32_t kRenderSize = 8;

int gValidationErrors = 0;
int gValidationWarnings = 0;
std::string gFirstValidationError;

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) {
    const char* message = (data && data->pMessage) ? data->pMessage : "(no message)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++gValidationErrors;
        if (gFirstValidationError.empty()) {
            gFirstValidationError = message;
        }
        std::fprintf(stderr, "validation error: %s\n", message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++gValidationWarnings;
        std::fprintf(stderr, "validation warning: %s\n", message);
    }
    return VK_FALSE;
}

void checkVk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "fatal: %s failed with VkResult %d\n", what, static_cast<int>(result));
        std::exit(2);
    }
}

std::string readEnvironment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
        std::string result(value);
        std::free(value);
        return result;
    }
    return {};
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

void configureLayerPath() {
#ifdef _WIN32
    if (readEnvironment("VK_LAYER_PATH").empty()) {
        const std::string sdk = readEnvironment("VULKAN_SDK");
        if (!sdk.empty()) {
            const std::string path = sdk + "\\Bin";
            putenv_local("VK_LAYER_PATH", path.c_str());
        }
    }
#endif
}

bool hasLayer(const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool hasInstanceExtension(const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool hasDeviceExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<VkExtensionProperties> enumerateDeviceExtensions(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
    return extensions;
}

std::uint32_t findGraphicsQueueFamily(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            return i;
        }
    }
    return std::numeric_limits<std::uint32_t>::max();
}

std::uint32_t findMemoryType(
    const VkPhysicalDeviceMemoryProperties& memoryProperties,
    std::uint32_t typeBits,
    VkMemoryPropertyFlags required) {
    for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const bool typeMatches = (typeBits & (1u << i)) != 0;
        const bool propertiesMatch =
            (memoryProperties.memoryTypes[i].propertyFlags & required) == required;
        if (typeMatches && propertiesMatch) {
            return i;
        }
    }
    std::fprintf(stderr, "fatal: no matching Vulkan memory type\n");
    std::exit(2);
}

VkDeviceSize roundUp(VkDeviceSize value, VkDeviceSize alignment) {
    if (alignment <= 1) {
        return value;
    }
    return (value + alignment - 1) / alignment * alignment;
}

template <typename T>
T loadDeviceProc(VkDevice device, const char* name) {
    return reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
}

struct Options {
    int frames = 10;
    int warmup = 2;
    std::uint32_t draws = 3000;
};

enum class Strategy {
    Push,
    Bindless,
    DescriptorBuffer,
};

const char* strategyName(Strategy strategy) {
    switch (strategy) {
        case Strategy::Push:
            return "push descriptors";
        case Strategy::Bindless:
            return "bindless indexing";
        case Strategy::DescriptorBuffer:
            return "descriptor buffer";
    }
    return "unknown";
}

struct StrategyResult {
    Strategy strategy;
    bool supported = false;
    std::string detail;
    double cpuUsPerDraw = 0.0;
};

struct Bench {
    Options options;

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    bool validationEnabled = false;
    bool debugUtilsEnabled = false;

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};

    bool pushSupported = false;
    bool bindlessSupported = false;
    bool descriptorBufferSupported = false;
    std::uint32_t maxPushDescriptors = 0;
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptorBufferProperties{};

    PFN_vkCmdPushDescriptorSetKHR cmdPushDescriptorSet = nullptr;
    PFN_vkCmdBindDescriptorBuffersEXT cmdBindDescriptorBuffers = nullptr;
    PFN_vkCmdSetDescriptorBufferOffsetsEXT cmdSetDescriptorBufferOffsets = nullptr;
    PFN_vkGetDescriptorSetLayoutSizeEXT getDescriptorSetLayoutSize = nullptr;
    PFN_vkGetDescriptorSetLayoutBindingOffsetEXT getDescriptorSetLayoutBindingOffset = nullptr;
    PFN_vkGetDescriptorEXT getDescriptor = nullptr;
    PFN_vkGetBufferDeviceAddress getBufferDeviceAddress = nullptr;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence frameFence = VK_NULL_HANDLE;

    VkImage renderImage = VK_NULL_HANDLE;
    VkDeviceMemory renderImageMemory = VK_NULL_HANDLE;
    VkImageView renderImageView = VK_NULL_HANDLE;
    VkImage textureImage = VK_NULL_HANDLE;
    VkDeviceMemory textureImageMemory = VK_NULL_HANDLE;
    std::array<VkImageView, kTextureViews> textureViews{};
    VkSampler sampler = VK_NULL_HANDLE;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;

    VkShaderModule vertexModule = VK_NULL_HANDLE;
    VkShaderModule fixedFragmentModule = VK_NULL_HANDLE;
    VkShaderModule bindlessFragmentModule = VK_NULL_HANDLE;

    VkDescriptorSetLayout pushLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorBufferLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout bindlessLayout = VK_NULL_HANDLE;

    VkPipelineLayout pushPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout descriptorBufferPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout bindlessPipelineLayout = VK_NULL_HANDLE;

    VkPipeline pushPipeline = VK_NULL_HANDLE;
    VkPipeline descriptorBufferPipeline = VK_NULL_HANDLE;
    VkPipeline bindlessPipeline = VK_NULL_HANDLE;

    VkDescriptorPool bindlessPool = VK_NULL_HANDLE;
    VkDescriptorSet bindlessSet = VK_NULL_HANDLE;

    VkBuffer descriptorBuffer = VK_NULL_HANDLE;
    VkDeviceMemory descriptorBufferMemory = VK_NULL_HANDLE;
    void* descriptorBufferMapped = nullptr;
    VkDeviceSize descriptorBlockSize = 0;
    VkDeviceSize descriptorSize = 0;
    std::array<VkDeviceSize, kDescriptorsPerDraw> bindingOffsets{};

    bool createInstance();
    bool selectPhysicalDevice();
    bool createDevice();
    bool createResources();
    void run();
    void destroy();

    StrategyResult runStrategy(Strategy strategy);
    double recordFrame(Strategy strategy);
    void recordDescriptorWork(VkCommandBuffer cmd, Strategy strategy, std::uint32_t draw);
    void uploadStaticDescriptors();
};

bool Bench::createInstance() {
    configureLayerPath();

    const bool validation = hasLayer("VK_LAYER_KHRONOS_validation");
    const bool debugUtils = hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    validationEnabled = validation;
    debugUtilsEnabled = debugUtils && validation;

    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr) {
        vkEnumerateInstanceVersion(&loaderVersion);
    }
    const std::uint32_t apiVersion =
        std::min(loaderVersion, static_cast<std::uint32_t>(VK_API_VERSION_1_3));

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "HeyListen descriptor bench";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "HeyListen";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = apiVersion;

    std::vector<const char*> layers;
    if (validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    std::vector<const char*> extensions;
    if (debugUtilsEnabled) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
    messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messengerInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messengerInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messengerInfo.pfnUserCallback = debugCallback;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames = layers.data();
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    if (debugUtilsEnabled) {
        createInfo.pNext = &messengerInfo;
    }

    checkVk(vkCreateInstance(&createInfo, nullptr, &instance), "vkCreateInstance");

    if (debugUtilsEnabled) {
        auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger != nullptr) {
            checkVk(createMessenger(instance, &messengerInfo, nullptr, &messenger),
                    "vkCreateDebugUtilsMessengerEXT");
        }
    }
    return true;
}

bool Bench::selectPhysicalDevice() {
    std::uint32_t deviceCount = 0;
    checkVk(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr),
            "vkEnumeratePhysicalDevices");
    if (deviceCount == 0) {
        std::fprintf(stderr, "fatal: no Vulkan physical devices found\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    checkVk(vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()),
            "vkEnumeratePhysicalDevices");

    int bestScore = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties candidateProperties{};
        vkGetPhysicalDeviceProperties(candidate, &candidateProperties);
        const std::uint32_t candidateQueueFamily = findGraphicsQueueFamily(candidate);
        if (candidateQueueFamily == std::numeric_limits<std::uint32_t>::max()) {
            continue;
        }

        const std::vector<VkExtensionProperties> extensions = enumerateDeviceExtensions(candidate);
        const bool hasPushExtension = hasDeviceExtension(extensions, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        const bool hasDescriptorBufferExtension =
            hasDeviceExtension(extensions, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);

        VkPhysicalDeviceFeatures baseFeatures{};
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        VkPhysicalDeviceVulkan12Features vulkan12{};
        vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        if (candidateProperties.apiVersion >= VK_API_VERSION_1_2) {
            features2.pNext = &vulkan12;
        }
        vkGetPhysicalDeviceFeatures2(candidate, &features2);
        baseFeatures = features2.features;

        std::uint32_t candidateMaxPushDescriptors = 0;
        if (hasPushExtension) {
            VkPhysicalDevicePushDescriptorPropertiesKHR pushProperties{};
            pushProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR;
            VkPhysicalDeviceProperties2 properties2{};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &pushProperties;
            vkGetPhysicalDeviceProperties2(candidate, &properties2);
            candidateMaxPushDescriptors = pushProperties.maxPushDescriptors;
        }

        bool candidateBindless = candidateProperties.apiVersion >= VK_API_VERSION_1_2 &&
                                 vulkan12.runtimeDescriptorArray == VK_TRUE &&
                                 vulkan12.descriptorBindingPartiallyBound == VK_TRUE &&
                                 baseFeatures.shaderSampledImageArrayDynamicIndexing == VK_TRUE &&
                                 candidateProperties.limits.maxPerStageDescriptorSamplers >= kBindlessSlots &&
                                 candidateProperties.limits.maxPerStageDescriptorSampledImages >= kBindlessSlots &&
                                 candidateProperties.limits.maxPushConstantsSize >=
                                     static_cast<std::uint32_t>(sizeof(std::uint32_t) * kDescriptorsPerDraw);

        bool candidateDescriptorBuffer = false;
        VkPhysicalDeviceDescriptorBufferPropertiesEXT candidateDescriptorBufferProperties{};
        candidateDescriptorBufferProperties.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT;
        if (hasDescriptorBufferExtension && candidateProperties.apiVersion >= VK_API_VERSION_1_2 &&
            vulkan12.bufferDeviceAddress == VK_TRUE) {
            VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBufferFeatures{};
            descriptorBufferFeatures.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 descriptorBufferFeatures2{};
            descriptorBufferFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            descriptorBufferFeatures2.pNext = &descriptorBufferFeatures;
            vkGetPhysicalDeviceFeatures2(candidate, &descriptorBufferFeatures2);

            VkPhysicalDeviceProperties2 properties2{};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &candidateDescriptorBufferProperties;
            vkGetPhysicalDeviceProperties2(candidate, &properties2);

            candidateDescriptorBuffer =
                descriptorBufferFeatures.descriptorBuffer == VK_TRUE &&
                candidateDescriptorBufferProperties.combinedImageSamplerDescriptorSize > 0 &&
                candidateDescriptorBufferProperties.descriptorBufferOffsetAlignment > 0 &&
                candidateProperties.limits.maxPerStageDescriptorSamplers >= kDescriptorsPerDraw &&
                candidateProperties.limits.maxPerStageDescriptorSampledImages >= kDescriptorsPerDraw;
        }

        const bool candidatePush = hasPushExtension &&
                                   candidateMaxPushDescriptors >= kDescriptorsPerDraw &&
                                   candidateProperties.limits.maxPerStageDescriptorSamplers >= kDescriptorsPerDraw &&
                                   candidateProperties.limits.maxPerStageDescriptorSampledImages >= kDescriptorsPerDraw;

        int score = 0;
        switch (candidateProperties.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                score = 1000;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                score = 800;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                score = 600;
                break;
            default:
                score = 100;
                break;
        }
        score += (candidatePush ? 3 : 0) + (candidateBindless ? 3 : 0) + (candidateDescriptorBuffer ? 3 : 0);

        if (score > bestScore) {
            bestScore = score;
            physical = candidate;
            queueFamily = candidateQueueFamily;
            properties = candidateProperties;
            pushSupported = candidatePush;
            bindlessSupported = candidateBindless;
            descriptorBufferSupported = candidateDescriptorBuffer;
            maxPushDescriptors = candidateMaxPushDescriptors;
            descriptorBufferProperties = candidateDescriptorBufferProperties;
        }
    }

    if (physical == VK_NULL_HANDLE) {
        std::fprintf(stderr, "fatal: no suitable Vulkan device with a graphics queue\n");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    return true;
}

bool Bench::createDevice() {
    const std::vector<VkExtensionProperties> extensions = enumerateDeviceExtensions(physical);

    std::vector<const char*> enabledExtensions;
    if (pushSupported && hasDeviceExtension(extensions, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        enabledExtensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    }
    if (descriptorBufferSupported &&
        hasDeviceExtension(extensions, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME)) {
        enabledExtensions.push_back(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    }

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    VkPhysicalDeviceVulkan12Features vulkan12{};
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12.descriptorIndexing = bindlessSupported ? VK_TRUE : VK_FALSE;
    vulkan12.runtimeDescriptorArray = bindlessSupported ? VK_TRUE : VK_FALSE;
    vulkan12.descriptorBindingPartiallyBound = bindlessSupported ? VK_TRUE : VK_FALSE;
    vulkan12.bufferDeviceAddress = descriptorBufferSupported ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBufferFeatures{};
    descriptorBufferFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
    descriptorBufferFeatures.descriptorBuffer =
        descriptorBufferSupported ? VK_TRUE : VK_FALSE;

    if (properties.apiVersion >= VK_API_VERSION_1_2) {
        features2.pNext = &vulkan12;
        if (descriptorBufferSupported) {
            vulkan12.pNext = &descriptorBufferFeatures;
        }
    }

    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.shaderSampledImageArrayDynamicIndexing =
        bindlessSupported ? VK_TRUE : VK_FALSE;
    features2.features = enabledFeatures;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &features2;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(enabledExtensions.size());
    deviceInfo.ppEnabledExtensionNames = enabledExtensions.data();

    checkVk(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    if (pushSupported) {
        cmdPushDescriptorSet = loadDeviceProc<PFN_vkCmdPushDescriptorSetKHR>(
            device, "vkCmdPushDescriptorSetKHR");
        if (cmdPushDescriptorSet == nullptr) {
            pushSupported = false;
        }
    }
    if (descriptorBufferSupported) {
        cmdBindDescriptorBuffers = loadDeviceProc<PFN_vkCmdBindDescriptorBuffersEXT>(
            device, "vkCmdBindDescriptorBuffersEXT");
        cmdSetDescriptorBufferOffsets = loadDeviceProc<PFN_vkCmdSetDescriptorBufferOffsetsEXT>(
            device, "vkCmdSetDescriptorBufferOffsetsEXT");
        getDescriptorSetLayoutSize = loadDeviceProc<PFN_vkGetDescriptorSetLayoutSizeEXT>(
            device, "vkGetDescriptorSetLayoutSizeEXT");
        getDescriptorSetLayoutBindingOffset =
            loadDeviceProc<PFN_vkGetDescriptorSetLayoutBindingOffsetEXT>(
                device, "vkGetDescriptorSetLayoutBindingOffsetEXT");
        getDescriptor = loadDeviceProc<PFN_vkGetDescriptorEXT>(device, "vkGetDescriptorEXT");
        getBufferDeviceAddress =
            loadDeviceProc<PFN_vkGetBufferDeviceAddress>(device, "vkGetBufferDeviceAddress");
        if (cmdBindDescriptorBuffers == nullptr || cmdSetDescriptorBufferOffsets == nullptr ||
            getDescriptorSetLayoutSize == nullptr ||
            getDescriptorSetLayoutBindingOffset == nullptr || getDescriptor == nullptr ||
            getBufferDeviceAddress == nullptr) {
            descriptorBufferSupported = false;
        }
    }
    return true;
}

VkImage createImage2D(
    VkDevice device,
    const VkPhysicalDeviceMemoryProperties& memoryProperties,
    VkFormat format,
    VkExtent2D extent,
    VkImageUsageFlags usage,
    VkDeviceMemory& memory) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    checkVk(vkCreateImage(device, &imageInfo, nullptr, &image), "vkCreateImage");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex =
        findMemoryType(memoryProperties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checkVk(vkAllocateMemory(device, &allocateInfo, nullptr, &memory), "vkAllocateMemory(image)");
    checkVk(vkBindImageMemory(device, image, memory, 0), "vkBindImageMemory");
    return image;
}

VkImageView createImageView(VkDevice device, VkImage image, VkFormat format) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    checkVk(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");
    return view;
}

VkDescriptorSetLayout createFixedDescriptorSetLayout(
    VkDevice device,
    VkDescriptorSetLayoutCreateFlags flags) {
    std::array<VkDescriptorSetLayoutBinding, kDescriptorsPerDraw> bindings{};
    for (std::uint32_t i = 0; i < kDescriptorsPerDraw; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.flags = flags;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    checkVk(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout),
            "vkCreateDescriptorSetLayout");
    return layout;
}

VkPipelineLayout createPipelineLayout(
    VkDevice device,
    VkDescriptorSetLayout setLayout,
    const VkPushConstantRange* pushConstantRange) {
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    if (pushConstantRange != nullptr) {
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = pushConstantRange;
    }
    VkPipelineLayout layout = VK_NULL_HANDLE;
    checkVk(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout),
            "vkCreatePipelineLayout");
    return layout;
}

VkPipeline createPipeline(
    VkDevice device,
    VkRenderPass renderPass,
    VkPipelineLayout pipelineLayout,
    VkShaderModule vertexShader,
    VkShaderModule fragmentShader,
    VkPipelineCreateFlags flags = 0) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentShader;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.width = static_cast<float>(kRenderSize);
    viewport.height = static_cast<float>(kRenderSize);
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.extent = {kRenderSize, kRenderSize};

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.flags = flags;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    checkVk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
    return pipeline;
}

bool Bench::createResources() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    checkVk(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo commandAllocateInfo{};
    commandAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandAllocateInfo.commandPool = commandPool;
    commandAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocateInfo.commandBufferCount = 1;
    checkVk(vkAllocateCommandBuffers(device, &commandAllocateInfo, &commandBuffer),
            "vkAllocateCommandBuffers");

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    checkVk(vkCreateFence(device, &fenceInfo, nullptr, &frameFence), "vkCreateFence");

    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    renderImage = createImage2D(
        device, memoryProperties, colorFormat, {kRenderSize, kRenderSize},
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, renderImageMemory);
    renderImageView = createImageView(device, renderImage, colorFormat);

    textureImage = createImage2D(
        device, memoryProperties, colorFormat, {kRenderSize, kRenderSize},
        VK_IMAGE_USAGE_SAMPLED_BIT, textureImageMemory);
    for (VkImageView& view : textureViews) {
        view = createImageView(device, textureImage, colorFormat);
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    checkVk(vkCreateSampler(device, &samplerInfo, nullptr, &sampler), "vkCreateSampler");

    VkAttachmentDescription attachment{};
    attachment.format = colorFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorReference{};
    colorReference.attachment = 0;
    colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &attachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    checkVk(vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass),
            "vkCreateRenderPass");

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &renderImageView;
    framebufferInfo.width = kRenderSize;
    framebufferInfo.height = kRenderSize;
    framebufferInfo.layers = 1;
    checkVk(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer),
            "vkCreateFramebuffer");

    VkShaderModuleCreateInfo vertexModuleInfo{};
    vertexModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertexModuleInfo.codeSize = sizeof(kFullscreenVertSpv);
    vertexModuleInfo.pCode = kFullscreenVertSpv;
    checkVk(vkCreateShaderModule(device, &vertexModuleInfo, nullptr, &vertexModule),
            "vkCreateShaderModule(vertex)");

    VkShaderModuleCreateInfo fixedModuleInfo{};
    fixedModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fixedModuleInfo.codeSize = sizeof(kFixed30FragSpv);
    fixedModuleInfo.pCode = kFixed30FragSpv;
    checkVk(vkCreateShaderModule(device, &fixedModuleInfo, nullptr, &fixedFragmentModule),
            "vkCreateShaderModule(fixed)");

    VkShaderModuleCreateInfo bindlessModuleInfo{};
    bindlessModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    bindlessModuleInfo.codeSize = sizeof(kBindlessFragSpv);
    bindlessModuleInfo.pCode = kBindlessFragSpv;
    checkVk(vkCreateShaderModule(device, &bindlessModuleInfo, nullptr, &bindlessFragmentModule),
            "vkCreateShaderModule(bindless)");

    if (pushSupported) {
        pushLayout = createFixedDescriptorSetLayout(
            device, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
        pushPipelineLayout = createPipelineLayout(device, pushLayout, nullptr);
        pushPipeline = createPipeline(
            device, renderPass, pushPipelineLayout, vertexModule, fixedFragmentModule);
    }

    if (bindlessSupported) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = kBindlessSlots;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
        bindingFlagsInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bindingFlagsInfo.bindingCount = 1;
        bindingFlagsInfo.pBindingFlags = &bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext = &bindingFlagsInfo;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        checkVk(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &bindlessLayout),
                "vkCreateDescriptorSetLayout(bindless)");

        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.size = static_cast<std::uint32_t>(sizeof(std::uint32_t) * kDescriptorsPerDraw);
        bindlessPipelineLayout = createPipelineLayout(device, bindlessLayout, &pushConstantRange);
        bindlessPipeline = createPipeline(
            device, renderPass, bindlessPipelineLayout, vertexModule, bindlessFragmentModule);

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = kBindlessSlots;
        VkDescriptorPoolCreateInfo poolCreateInfo{};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolCreateInfo.maxSets = 1;
        poolCreateInfo.poolSizeCount = 1;
        poolCreateInfo.pPoolSizes = &poolSize;
        checkVk(vkCreateDescriptorPool(device, &poolCreateInfo, nullptr, &bindlessPool),
                "vkCreateDescriptorPool");

        VkDescriptorSetAllocateInfo setAllocateInfo{};
        setAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAllocateInfo.descriptorPool = bindlessPool;
        setAllocateInfo.descriptorSetCount = 1;
        setAllocateInfo.pSetLayouts = &bindlessLayout;
        checkVk(vkAllocateDescriptorSets(device, &setAllocateInfo, &bindlessSet),
                "vkAllocateDescriptorSets");
    }

    if (descriptorBufferSupported) {
        descriptorBufferLayout = createFixedDescriptorSetLayout(
            device, VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT);
        descriptorBufferPipelineLayout =
            createPipelineLayout(device, descriptorBufferLayout, nullptr);
        descriptorBufferPipeline = createPipeline(
            device, renderPass, descriptorBufferPipelineLayout, vertexModule, fixedFragmentModule,
            VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT);

        VkDeviceSize setSize = 0;
        getDescriptorSetLayoutSize(device, descriptorBufferLayout, &setSize);
        for (std::uint32_t i = 0; i < kDescriptorsPerDraw; ++i) {
            getDescriptorSetLayoutBindingOffset(device, descriptorBufferLayout, i, &bindingOffsets[i]);
        }
        const VkDeviceSize alignment =
            descriptorBufferProperties.descriptorBufferOffsetAlignment;
        descriptorBlockSize = roundUp(setSize, alignment);
        descriptorSize = descriptorBufferProperties.combinedImageSamplerDescriptorSize;
        const VkDeviceSize bufferSize = descriptorBlockSize * options.draws;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVk(vkCreateBuffer(device, &bufferInfo, nullptr, &descriptorBuffer),
                "vkCreateBuffer(descriptorBuffer)");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, descriptorBuffer, &requirements);
        VkMemoryAllocateFlagsInfo allocateFlags{};
        allocateFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocateFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.pNext = &allocateFlags;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = findMemoryType(
            memoryProperties, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        checkVk(vkAllocateMemory(device, &allocateInfo, nullptr, &descriptorBufferMemory),
                "vkAllocateMemory(descriptorBuffer)");
        checkVk(vkBindBufferMemory(device, descriptorBuffer, descriptorBufferMemory, 0),
                "vkBindBufferMemory(descriptorBuffer)");
        checkVk(vkMapMemory(device, descriptorBufferMemory, 0, VK_WHOLE_SIZE, 0,
                            &descriptorBufferMapped),
                "vkMapMemory(descriptorBuffer)");
    }

    VkCommandBuffer transitionCommand = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo transitionAllocateInfo{};
    transitionAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    transitionAllocateInfo.commandPool = commandPool;
    transitionAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    transitionAllocateInfo.commandBufferCount = 1;
    checkVk(vkAllocateCommandBuffers(device, &transitionAllocateInfo, &transitionCommand),
            "vkAllocateCommandBuffers(transition)");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(transitionCommand, &beginInfo),
            "vkBeginCommandBuffer(transition)");

    VkImageMemoryBarrier barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = renderImage;
    barriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.layerCount = 1;

    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = textureImage;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[1].subresourceRange.levelCount = 1;
    barriers[1].subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(
        transitionCommand, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 2, barriers);
    checkVk(vkEndCommandBuffer(transitionCommand), "vkEndCommandBuffer(transition)");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &transitionCommand;
    checkVk(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit(transition)");
    checkVk(vkQueueWaitIdle(queue), "vkQueueWaitIdle(transition)");
    vkFreeCommandBuffers(device, commandPool, 1, &transitionCommand);

    uploadStaticDescriptors();
    return true;
}

void Bench::uploadStaticDescriptors() {
    if (bindlessSupported) {
        std::array<VkDescriptorImageInfo, kBindlessSlots> imageInfos{};
        std::array<VkWriteDescriptorSet, kBindlessSlots> writes{};
        for (std::uint32_t i = 0; i < kBindlessSlots; ++i) {
            imageInfos[i].sampler = sampler;
            imageInfos[i].imageView = textureViews[i % kTextureViews];
            imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = bindlessSet;
            writes[i].dstBinding = 0;
            writes[i].dstArrayElement = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &imageInfos[i];
        }
        vkUpdateDescriptorSets(device, kBindlessSlots, writes.data(), 0, nullptr);
    }
}

void Bench::recordDescriptorWork(VkCommandBuffer cmd, Strategy strategy, std::uint32_t draw) {
    switch (strategy) {
        case Strategy::Push: {
            std::array<VkDescriptorImageInfo, kDescriptorsPerDraw> imageInfos{};
            std::array<VkWriteDescriptorSet, kDescriptorsPerDraw> writes{};
            for (std::uint32_t i = 0; i < kDescriptorsPerDraw; ++i) {
                imageInfos[i].sampler = sampler;
                imageInfos[i].imageView = textureViews[(draw + i) % kTextureViews];
                imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = VK_NULL_HANDLE;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &imageInfos[i];
            }
            cmdPushDescriptorSet(
                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pushPipelineLayout, 0,
                kDescriptorsPerDraw, writes.data());
            break;
        }
        case Strategy::Bindless: {
            std::array<std::uint32_t, kDescriptorsPerDraw> indices{};
            for (std::uint32_t i = 0; i < kDescriptorsPerDraw; ++i) {
                indices[i] = (draw + i) % kDescriptorsPerDraw;
            }
            vkCmdPushConstants(
                cmd, bindlessPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                static_cast<std::uint32_t>(sizeof(indices)), indices.data());
            break;
        }
        case Strategy::DescriptorBuffer: {
            std::array<VkDescriptorImageInfo, kDescriptorsPerDraw> imageInfos{};
            auto* base = static_cast<std::uint8_t*>(descriptorBufferMapped) +
                         static_cast<VkDeviceSize>(draw) * descriptorBlockSize;
            for (std::uint32_t i = 0; i < kDescriptorsPerDraw; ++i) {
                imageInfos[i].sampler = sampler;
                imageInfos[i].imageView = textureViews[(draw + i) % kTextureViews];
                imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkDescriptorGetInfoEXT getInfo{};
                getInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT;
                getInfo.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                getInfo.data.pCombinedImageSampler = &imageInfos[i];
                getDescriptor(device, &getInfo, descriptorSize, base + bindingOffsets[i]);
            }
            std::uint32_t bufferIndex = 0;
            const VkDeviceSize offset = static_cast<VkDeviceSize>(draw) * descriptorBlockSize;
            cmdSetDescriptorBufferOffsets(
                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, descriptorBufferPipelineLayout, 0, 1,
                &bufferIndex, &offset);
            break;
        }
    }
}

double Bench::recordFrame(Strategy strategy) {
    checkVk(vkWaitForFences(device, 1, &frameFence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    checkVk(vkResetFences(device, 1, &frameFence), "vkResetFences");
    checkVk(vkResetCommandBuffer(commandBuffer, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer");

    VkClearValue clearValue{};
    clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = framebuffer;
    renderPassInfo.renderArea.extent = {kRenderSize, kRenderSize};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    switch (strategy) {
        case Strategy::Push:
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pushPipeline);
            break;
        case Strategy::Bindless:
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, bindlessPipeline);
            vkCmdBindDescriptorSets(
                commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, bindlessPipelineLayout, 0, 1,
                &bindlessSet, 0, nullptr);
            break;
        case Strategy::DescriptorBuffer: {
            vkCmdBindPipeline(
                commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, descriptorBufferPipeline);
            VkBufferDeviceAddressInfo addressInfo{};
            addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            addressInfo.buffer = descriptorBuffer;
            VkDescriptorBufferBindingInfoEXT bindingInfo{};
            bindingInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT;
            bindingInfo.address = getBufferDeviceAddress(device, &addressInfo);
            bindingInfo.usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;
            cmdBindDescriptorBuffers(commandBuffer, 1, &bindingInfo);
            break;
        }
    }

    const auto start = std::chrono::steady_clock::now();
    for (std::uint32_t draw = 0; draw < options.draws; ++draw) {
        recordDescriptorWork(commandBuffer, strategy, draw);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }
    const auto end = std::chrono::steady_clock::now();

    vkCmdEndRenderPass(commandBuffer);
    checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    checkVk(vkQueueSubmit(queue, 1, &submitInfo, frameFence), "vkQueueSubmit");

    return std::chrono::duration<double>(end - start).count();
}

StrategyResult Bench::runStrategy(Strategy strategy) {
    StrategyResult result{};
    result.strategy = strategy;

    const bool supported =
        (strategy == Strategy::Push && pushSupported) ||
        (strategy == Strategy::Bindless && bindlessSupported) ||
        (strategy == Strategy::DescriptorBuffer && descriptorBufferSupported);
    if (!supported) {
        result.supported = false;
        switch (strategy) {
            case Strategy::Push:
                result.detail = "VK_KHR_push_descriptor unavailable";
                break;
            case Strategy::Bindless:
                result.detail = "descriptor indexing features unavailable";
                break;
            case Strategy::DescriptorBuffer:
                result.detail = "VK_EXT_descriptor_buffer unavailable";
                break;
        }
        return result;
    }

    result.supported = true;
    for (int frame = 0; frame < options.warmup; ++frame) {
        (void)recordFrame(strategy);
    }

    double totalSeconds = 0.0;
    for (int frame = 0; frame < options.frames; ++frame) {
        totalSeconds += recordFrame(strategy);
    }
    checkVk(vkWaitForFences(device, 1, &frameFence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    const double totalDraws = static_cast<double>(options.frames) * options.draws;
    result.cpuUsPerDraw = totalSeconds / totalDraws * 1e6;
    return result;
}

void Bench::run() {
    std::array<Strategy, 3> strategies{
        Strategy::Push, Strategy::Bindless, Strategy::DescriptorBuffer};
    std::array<StrategyResult, 3> results{};
    for (std::size_t i = 0; i < strategies.size(); ++i) {
        results[i] = runStrategy(strategies[i]);
    }

    const char* deviceType = "other";
    switch (properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            deviceType = "discrete";
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            deviceType = "integrated";
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            deviceType = "virtual";
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            deviceType = "cpu";
            break;
        default:
            break;
    }

    std::printf("device: %s (%s)\n", properties.deviceName, deviceType);
    std::printf(
        "vulkan: %u.%u.%u\n", VK_API_VERSION_MAJOR(properties.apiVersion),
        VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion));
    std::printf(
        "validation: %s (errors: %d, warnings: %d)\n", validationEnabled ? "enabled" : "disabled",
        gValidationErrors, gValidationWarnings);
    std::printf(
        "draws/frame: %u, descriptors/draw: %u, frames: %d, warmup: %d\n\n", options.draws,
        kDescriptorsPerDraw, options.frames, options.warmup);

    std::printf(
        "%-22s %-14s %14s %14s\n", "strategy", "status", "cpu us/draw", "total draws");
    std::printf(
        "%-22s %-14s %14s %14s\n", "----------------------", "--------------",
        "--------------", "--------------");
    for (const StrategyResult& result : results) {
        const double totalDraws = static_cast<double>(options.frames) * options.draws;
        if (result.supported) {
            std::printf(
                "%-22s %-14s %14.3f %14.0f\n", strategyName(result.strategy), "ok",
                result.cpuUsPerDraw, totalDraws);
        } else {
            std::printf(
                "%-22s %-14s %14s %14s\n", strategyName(result.strategy), "unsupported", "-", "-");
        }
    }
    std::printf("\n");
    for (const StrategyResult& result : results) {
        if (!result.supported) {
            std::printf("%s: %s\n", strategyName(result.strategy), result.detail.c_str());
        }
    }
    std::printf(
        "cpu us/draw measures command-buffer recording only; queue submit and GPU wait are "
        "excluded.\n");

    if (gValidationErrors > 0) {
        std::printf("\nvalidation errors: %d (first: %s)\n", gValidationErrors,
                    gFirstValidationError.c_str());
    }
}

void Bench::destroy() {
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        if (descriptorBufferMapped != nullptr) {
            vkUnmapMemory(device, descriptorBufferMemory);
        }
        if (descriptorBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, descriptorBuffer, nullptr);
        }
        if (descriptorBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device, descriptorBufferMemory, nullptr);
        }
        if (bindlessPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, bindlessPool, nullptr);
        }
        if (bindlessPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, bindlessPipeline, nullptr);
        }
        if (descriptorBufferPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, descriptorBufferPipeline, nullptr);
        }
        if (pushPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pushPipeline, nullptr);
        }
        if (bindlessPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, bindlessPipelineLayout, nullptr);
        }
        if (descriptorBufferPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, descriptorBufferPipelineLayout, nullptr);
        }
        if (pushPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pushPipelineLayout, nullptr);
        }
        if (bindlessLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, bindlessLayout, nullptr);
        }
        if (descriptorBufferLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, descriptorBufferLayout, nullptr);
        }
        if (pushLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, pushLayout, nullptr);
        }
        if (bindlessFragmentModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, bindlessFragmentModule, nullptr);
        }
        if (fixedFragmentModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, fixedFragmentModule, nullptr);
        }
        if (vertexModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, vertexModule, nullptr);
        }
        if (frameFence != VK_NULL_HANDLE) {
            vkDestroyFence(device, frameFence, nullptr);
        }
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, sampler, nullptr);
        }
        for (VkImageView view : textureViews) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, view, nullptr);
            }
        }
        if (textureImage != VK_NULL_HANDLE) {
            vkDestroyImage(device, textureImage, nullptr);
        }
        if (textureImageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device, textureImageMemory, nullptr);
        }
        if (renderImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, renderImageView, nullptr);
        }
        if (renderImage != VK_NULL_HANDLE) {
            vkDestroyImage(device, renderImage, nullptr);
        }
        if (renderImageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device, renderImageMemory, nullptr);
        }
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        if (renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, renderPass, nullptr);
        }
        if (commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, commandPool, nullptr);
        }
        vkDestroyDevice(device, nullptr);
    }
    if (messenger != VK_NULL_HANDLE) {
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger != nullptr) {
            destroyMessenger(instance, messenger, nullptr);
        }
    }
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
    }
}

void printUsage(const char* executable) {
    std::printf(
        "usage: %s [--frames N] [--warmup N] [--draws N]\n"
        "  --frames  measured frames (default 10)\n"
        "  --warmup  warmup frames per strategy (default 2)\n"
        "  --draws   draws per frame (default 3000)\n",
        executable);
}

}

int main(int argc, char** argv) {
    Bench bench;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            return 0;
        }
        const bool hasValue = i + 1 < argc;
        if (argument == "--frames" && hasValue) {
            bench.options.frames = std::max(1, std::atoi(argv[++i]));
        } else if (argument == "--warmup" && hasValue) {
            bench.options.warmup = std::max(0, std::atoi(argv[++i]));
        } else if (argument == "--draws" && hasValue) {
            bench.options.draws = static_cast<std::uint32_t>(std::max(1, std::atoi(argv[++i])));
        } else {
            std::fprintf(stderr, "unrecognised argument: %s\n", argument.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }

    bench.createInstance();
    if (!bench.selectPhysicalDevice()) {
        bench.destroy();
        return 1;
    }
    if (!bench.createDevice()) {
        bench.destroy();
        return 1;
    }
    if (!bench.createResources()) {
        bench.destroy();
        return 1;
    }
    bench.run();
    bench.destroy();

    if (gValidationErrors > 0) {
        return 2;
    }
    return 0;
}
