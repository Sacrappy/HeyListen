// The Vulkan back end's start: vulkan-1.dll, the instance, the device (Vulkan 1.3 and the extensions the NVN replay
// wants where the GPU has them: mesh shaders, host memory import, graphics pipeline libraries), the window's
// swapchain, and the rx_* table (vk_api.h). The NVN replay itself is vk_replay.cpp.
#include "vk_paths.h"
#include "vk_loader.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/sysinfo.h>   // (commit_left_mb)
#include <dlfcn.h>   // (create_device: hos_posix_submit_thread_note, looked up in the exe)
#endif

#include "vk_api.h"
#include "vk_draw.h"
#include "vk_framegen.h"
#include "vk_internal.h"
#include "gpu_choice.h"
#include "present_mode.h"
#include "sparse_queue.h"
#include "vk_menu.h"
#include "perf_report.h"
#include "report_info.h"
#include "vk_dlss.h"
#include "vk_fsr4.h"
#include "vk_reflex.h"
#include "vk_gpu_clock.h"
#include "vk_progress.h"
#include "mesh_mode.h"
#include "vk_tl_clock.h"   // (RX_VK_TL: the timeline's host time domain)
#include "vk_push_fix.h"

namespace vkx {
#define VKX_DEFINE(f) PFN_##f f;
VKX_GLOBAL(VKX_DEFINE)
VKX_INSTANCE(VKX_DEFINE)
VKX_DEVICE(VKX_DEFINE)
VKX_DEVICE2(VKX_DEFINE)
VKX_DEVICE3(VKX_DEFINE)
#undef VKX_DEFINE

static PFN_vkGetInstanceProcAddr get_instance_proc;

bool load_global() {
#ifdef _WIN32   // (hookshot: the loader from System32 only, not from the current folder or PATH)
    HMODULE m = LoadLibraryExA("vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
    HMODULE m = LoadLibraryA("vulkan-1.dll");   // (linux: libvulkan.so.1, host/posix/win32_posix.c)
#endif
    if (!m) return false;
    get_instance_proc = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(m, "vkGetInstanceProcAddr");
    if (!get_instance_proc) return false;
#define VKX_LOAD(f) f = (PFN_##f)get_instance_proc(nullptr, #f);
    VKX_GLOBAL(VKX_LOAD)
#undef VKX_LOAD
    return vkCreateInstance != nullptr;
}
PFN_vkGetInstanceProcAddr instance_proc_addr() { return get_instance_proc; }
void load_instance(VkInstance instance) {
#define VKX_LOAD(f) f = (PFN_##f)get_instance_proc(instance, #f);
    VKX_INSTANCE(VKX_LOAD)
#undef VKX_LOAD
}
void load_device(VkDevice device) {
#define VKX_LOAD(f) f = (PFN_##f)vkGetDeviceProcAddr(device, #f);
    VKX_DEVICE(VKX_LOAD)
    VKX_DEVICE2(VKX_LOAD)
    VKX_DEVICE3(VKX_LOAD)
#undef VKX_LOAD
}
} // namespace vkx

namespace mkwvk {

std::vector<std::string> gpu_device_names;
const MkwVkHost* host;
SplitCount prof_ns[PROF_COUNT];
Device dev;
PFN_vkGetInstanceProcAddr instance_proc() { return vkx::instance_proc_addr(); }

void logf(const char* fmt, ...) {
    char b[1024];
    std::va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (host) host->log(b);
}

// (issue 55 and 62, Longshot 1280) MB of commit left: RAM and page file on Windows, free RAM and swap elsewhere; -1 when unknown
static long long commit_left_mb() {
#ifdef _WIN32
    MEMORYSTATUSEX m = {};
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? (long long)(m.ullAvailPageFile >> 20) : -1;
#else
    struct sysinfo si;
    return sysinfo(&si) ? -1 : (long long)(((unsigned long long)si.freeram + si.bufferram + si.freeswap) * si.mem_unit >> 20);
#endif
}

// (issue 55 and 62, Longshot 1280) The renderer's C++ allocations at the commit limit. An `operator new` that malloc refuses calls
// this each time and tries again when it returns. Before, nothing did: the refusal was a std::bad_alloc that no catch
// held (exception 0x20474343 in hookshot_vk.dll, "An unexpected error" or the crash window on a 32 GB PC), with none of the wait
// that the host's own allocations have at the limit (host/mkw_glue.c hos_alloc_failed). Now a refusal with the commit nearly
// all used waits 50 ms and tries again, for up to 5 s on this thread (a refusal more than 250 ms after its last try starts a
// new 5 s), then the host's out-of-memory line and notice, and the game stops. A refusal with plenty of commit left is not
// the limit (a size gone wrong, as a bad shader's): the std::bad_alloc as before, for the catch that holds it.
static void new_refused() {
    using clock = std::chrono::steady_clock;
    static thread_local clock::time_point first, last;
    static std::atomic<int> told{0};
    const clock::time_point now = clock::now();
    if (last == clock::time_point() || now - last > std::chrono::milliseconds(250)) first = now;
    last = now;
    const long long left = commit_left_mb();
    if (left < 0 || left > 4096) throw std::bad_alloc();
    if (now - first >= std::chrono::seconds(5)) {
        logf("mem: the renderer's C++ allocations: still refused after 5 s at the commit limit (%lld MB of commit left)", left);
        if (host && host->out_of_memory) host->out_of_memory("the renderer's C++ allocations");
        throw std::bad_alloc();   // (a host with no out_of_memory: as before)
    }
    if (told.fetch_add(1) < 8)
        logf("vk: a C++ allocation refused at the commit limit (%lld MB of commit left): tried again for up to 5 s", left);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

namespace {

// (Hookshot b9b48dfd, pixl27; issue 43) A driver that faults in vkCreateShaderModule. On an RX 570 (Polaris, AMD's driver 2.0.220 and
// 2.0.279) the game closed with no message during its first shader preparation, one crash line per preparation worker. The shader
// translator calls vkCreateShaderModule (vk_shader.cpp), which no guard covered (pipelines have guarded_graphics / guarded_compute,
// vk_draw.cpp's own calls have guarded_shader_module). Its pointer goes through this guard now: a fault is that shader failing (the
// translator takes a null module for a failed program), logged with the SPIR-V kept beside the game (the first two), and said once in
// a box. RX_VK_NO_MODULE_GUARD=1: the driver's pointer as before
#ifdef _WIN32
PFN_vkCreateShaderModule driver_create_shader_module = nullptr;
std::atomic<int> module_faults{0};
__declspec(noinline) VkResult module_call(VkDevice d, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* a,
                                          VkShaderModule* m, int* faulted) {
    __try {
        return driver_create_shader_module(d, ci, a, m);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_ILLEGAL_INSTRUCTION ||
                        GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO ? 1 : 0) {
        *faulted = 1;
        return VK_ERROR_UNKNOWN;
    }
}
void module_fault(const VkShaderModuleCreateInfo* ci) {
    const int n = ++module_faults;
    char saved[MAX_PATH + 48] = "";
    if (n <= 2 && ci && ci->pCode && ci->codeSize) {   // (the SPIR-V the driver could not take: spirv-val / spirv-dis it here)
        char path[MAX_PATH];
        std::string dir(path, game_module_path(path, MAX_PATH));
        dir = dir.substr(0, dir.find_last_of("\\/") + 1);
        snprintf(saved, sizeof saved, "%sheylisten_driver_fault_%d.spv", dir.c_str(), n);
        if (FILE* f = fopen(saved, "wb")) fwrite(ci->pCode, 1, ci->codeSize, f), fclose(f);
        else saved[0] = 0;
    }
    if (n <= 8 || (n & (n - 1)) == 0)
        logf("vk: THE GRAPHICS DRIVER CRASHED reading a shader (vkCreateShaderModule, %zu bytes of SPIR-V%s%s): it is left out "
             "(%d so far)", ci ? (size_t)ci->codeSize : 0, saved[0] ? ", kept as " : "", saved, n);
    if (n != 1 || dev.offscreen || getenv("HOS_HEADLESS")) return;
    std::thread([name = std::string(dev.props.deviceName), old = !dev.mesh_shader] {
        std::string text = "The graphics driver of your " + name + " crashed while HeyListen handed it one of the game's shaders. "
            "HeyListen leaves such shaders out and goes on, but parts of the picture may be missing, and the driver may fail again.";
        if (old)
            text += "\n\nThis card has no mesh shaders and is below what HeyListen supports (NVIDIA GeForce RTX 20 series or newer, "
                    "AMD Radeon RX 6000 series or newer, or Intel Arc). Updating its driver is worth a try, but it may not run "
                    "the game.";
        text += "\n\nIf you report it on HeyListen's GitHub page, please attach the log and the heylisten_driver_fault_1.spv file "
                "from the game folder.";
        MessageBoxA(nullptr, text.c_str(), "HeyListen: graphics driver", MB_OK | MB_ICONWARNING | MB_TOPMOST);
    }).detach();
}
VKAPI_ATTR VkResult VKAPI_CALL guarded_create_shader_module(VkDevice d, const VkShaderModuleCreateInfo* ci,
                                                            const VkAllocationCallbacks* a, VkShaderModule* m) {
    int faulted = 0;
    const VkResult r = module_call(d, ci, a, m, &faulted);
    if (faulted) {
        if (m) *m = VK_NULL_HANDLE;
        module_fault(ci);
    }
    return r;
}
void guard_shader_modules() {
    if (getenv("RX_VK_NO_MODULE_GUARD") || !vkx::vkCreateShaderModule) return;
    driver_create_shader_module = vkx::vkCreateShaderModule;
    vkx::vkCreateShaderModule = guarded_create_shader_module;
}
#else
void guard_shader_modules() {}
#endif

// (hookshot, issue 640) The mesh shader fallback's task programs read the task payloads' address from the push constants by the
// member's byte offset (64) where SPIR-V takes the member's index (3): an access chain past the block's last member. Such a module
// is invalid, and NVIDIA's compiler faulted on it (vkCreateComputePipelines, "a fault in the driver", 10 ms). A GTX 1060's and a
// GTX 1070's logs ended there: their first task + mesh draw (a5da9f84b72d1471, Kokiri Forest after a save is loaded) "could not be
// made", the next fallback compile never returned, and the picture stayed black with the game running behind it. Measured with
// RX_VK_FORCE_MESH_FALLBACK=1 on an RTX 3060 Ti: the same fault on a5da9f84b72d1471, then the render thread's last line. The
// translator makes them so, and the program caches players have keep them: here, as each module is made, such an index becomes
// the index of the member at that offset (vk_push_fix.h). Only on a GPU that runs the fallback. RX_VK_NO_PUSH_FIX=1: the modules
// as they are
PFN_vkCreateShaderModule unfixed_create_shader_module = nullptr;
std::atomic<int> push_fixes_told{0};
VKAPI_ATTR VkResult VKAPI_CALL fixing_create_shader_module(VkDevice d, const VkShaderModuleCreateInfo* ci,
                                                           const VkAllocationCallbacks* a, VkShaderModule* m) {
    std::vector<uint32_t> w;
    const size_t fixed = ci && ci->pCode && !(ci->codeSize % 4) ? push_offset_fix(ci->pCode, ci->codeSize / 4, w) : 0;
    if (!fixed) return unfixed_create_shader_module(d, ci, a, m);
    if (push_fixes_told++ < 4)
        logf("vk: fallback: %zu push constant access%s by byte offset made by member index (issue 640)", fixed, fixed == 1 ? "" : "es");
    VkShaderModuleCreateInfo with = *ci;
    with.pCode = w.data();
    with.codeSize = w.size() * 4;
    return unfixed_create_shader_module(d, &with, a, m);
}
void fix_fallback_modules() {
    if (!dev.mesh_fallback || getenv("RX_VK_NO_PUSH_FIX") || !vkx::vkCreateShaderModule) return;
    unfixed_create_shader_module = vkx::vkCreateShaderModule;   // (the guard's, or the driver's)
    vkx::vkCreateShaderModule = fixing_create_shader_module;
}

bool has_extension(const std::vector<VkExtensionProperties>& all, const char* name) {
    for (const auto& e : all)
        if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}

bool create_instance() {
    uint32_t version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3) {
        logf("vk: the Vulkan loader is %u.%u: 1.3 is needed", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version));
        return false;
    }
#ifdef _WIN32
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    const uint32_t n_exts = 2;
#else
    // (linux: every window system's surface the loader has, for whichever one SDL's window is on: X11 or Wayland)
    const char* exts[4] = {VK_KHR_SURFACE_EXTENSION_NAME};
    uint32_t n_exts = 1;
    {
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> all(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, all.data());
        for (const char* want : {"VK_KHR_xlib_surface", "VK_KHR_xcb_surface", "VK_KHR_wayland_surface"})
            if (has_extension(all, want)) exts[n_exts++] = want;
    }
#endif
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mkw-recomp";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    std::vector<const char*> ext_list(exts, exts + n_exts);
    {   // (hookshot) what NVIDIA DLSS needs (vk_dlss.cpp; nothing in a build without it)
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> all(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, all.data());
        dlss_instance_extensions(ext_list, all);
        // (hookshot) the HDR10 colour space for the Insert menu's HDR (the menu's settings are read after the instance is made, so
        // whenever the driver has it; the SDR choice in create_swapchain keeps to the sRGB surface)
        if (has_extension(all, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
            ext_list.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME), dev.swapchain_colorspace = true;
    }
    ci.enabledExtensionCount = (uint32_t)ext_list.size();
    ci.ppEnabledExtensionNames = ext_list.data();
    // (Longshot c392df3a sets Mesa's Intel driconf shader_spilling_rate to 2 here, before the driver loads. Not ported: the
    // renderer sets no environment variable from here, because a `setenv` while another thread may be in `getenv` is undefined,
    // and the switches that need it are decided in the host before its first thread. A player can still set
    // shader_spilling_rate=2 themselves; create_device logs it, and the README's fixes line says what it is worth on Mesa's
    // Intel compiler)
    // RX_VK_VALIDATION=1: the Khronos validation layer (when installed)
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (getenv("RX_VK_VALIDATION")) ci.enabledLayerCount = 1, ci.ppEnabledLayerNames = &layer;
    if (vkCreateInstance(&ci, nullptr, &dev.instance) != VK_SUCCESS) {
        logf("vk: vkCreateInstance failed");
        return false;
    }
    load_instance(dev.instance);
    return true;
}

static_assert(gpu_choice::TYPE_CPU == (int)VK_PHYSICAL_DEVICE_TYPE_CPU && gpu_choice::TYPE_DISCRETE == (int)VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
              gpu_choice::TYPE_INTEGRATED == (int)VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
              gpu_choice::TYPE_VIRTUAL == (int)VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU && gpu_choice::TYPE_OTHER == (int)VK_PHYSICAL_DEVICE_TYPE_OTHER,
              "gpu_choice.h's device types are Vulkan's");

// (the one device there is is a software renderer, and RX_VK_ALLOW_CPU is not set: pick_device fails, and mkwvk_init says why)
static std::string software_only_name;

bool pick_device() {
    menu_init();
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(dev.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    vkEnumeratePhysicalDevices(dev.instance, &n, all.data());

    // (a software renderer, VK_PHYSICAL_DEVICE_TYPE_CPU: Mesa's llvmpipe and lavapipe, SwiftShader. Without the GPU's own Vulkan driver
    // (Mesa's RADV or ANV, a vendor's) the loader lists only that one, and the game started on it and crashed (a Ryzen 5 5600H with
    // llvmpipe, v0.4.2). Never taken while a GPU is there, nor alone, unless RX_VK_ALLOW_CPU=1 (developers: a run with no GPU))
    const bool allow_cpu = gpu_choice::allow_cpu(getenv("RX_VK_ALLOW_CPU"));
    std::vector<gpu_choice::Candidate> cands(n);
    std::vector<std::string> names(n);
    std::vector<uint32_t> valid_gpus;
    gpu_device_names.clear();
    software_only_name.clear();
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(all[i], &p);
        const bool software = gpu_choice::is_software((int)p.deviceType);
        logf("vk: device %u: %s, Vulkan %u.%u, driver %#x, type %d%s", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion), p.driverVersion, (int)p.deviceType, software ? " (a software renderer)" : "");
        cands[i] = {p.apiVersion >= VK_API_VERSION_1_3, (int)p.deviceType, p.driverVersion};
        names[i] = p.deviceName;
        if (gpu_choice::usable(cands[i], allow_cpu)) {
            valid_gpus.push_back(i);
            gpu_device_names.push_back(p.deviceName);
        }
    }
    menu_set_gpu_count((int)valid_gpus.size());

    // (AMD laptops) With AMD graphics in the processor and an AMD card, Windows has two AMD drivers installed, one an
    // adapter, and each one's Vulkan driver lists every AMD GPU: the card shows up twice. It came first through the
    // processor graphics' driver, which the card's own update leaves as it was: on an RX 7700S that older driver
    // (amdvlk64.dll 32.0.11036.4002) crashed in its shader compiler (0xc0000409) on the game's compute pipelines while
    // the card's 32.0.32015.7029 built them. The same kind of GPU twice: the one with the newer driver wins.
    // RX_VK_DEVICE=<n> or opts.gpu_device: that device, as listed in the log
    int target_raw_idx = -1;
    if (const char* want = getenv("RX_VK_DEVICE")) {
        if (*want) target_raw_idx = atoi(want);
    } else if (const int k = gpu_choice::menu_device(opts.gpu_device, (int)valid_gpus.size()); k >= 0) {
        target_raw_idx = (int)valid_gpus[k];
    }

    const gpu_choice::Pick pick = gpu_choice::choose(cands.data(), (int)n, target_raw_idx, allow_cpu);
    if (pick.target_unavailable)
        logf("vk: preferred device %d not available, lacks Vulkan 1.3 or is a software renderer (RX_VK_ALLOW_CPU=1 allows one), "
             "falling back to auto selection", target_raw_idx);
    for (uint32_t i = 0; i < n; i++)
        if (gpu_choice::is_software(cands[i].type) && pick.device >= 0 && !allow_cpu)
            logf("vk: device %u (%s) is a software renderer: not used while a GPU is there", i, names[i].c_str());
    if (pick.device < 0 && pick.software_only) {
        software_only_name = names[0];
        logf("vk: only a software renderer was found (%s), which cannot run the game: the GPU's Vulkan driver is missing, or "
             "hidden from this process (Linux: Mesa's Vulkan driver for the GPU, such as mesa-vulkan-drivers, vulkan-radeon or "
             "vulkan-intel, or NVIDIA's; Windows: the driver from AMD, NVIDIA or Intel). RX_VK_ALLOW_CPU=1 runs on it anyway",
             names[0].c_str());
        return false;
    }
    if (pick.device < 0) {
        logf("vk: no Vulkan 1.3 GPU");
        return false;
    }
    if (gpu_choice::is_software(cands[pick.device].type))
        logf("vk: RX_VK_ALLOW_CPU: running on the software renderer %s: far too slow to play, and it can crash", names[pick.device].c_str());
    dev.physical = all[pick.device];
    vkGetPhysicalDeviceProperties(dev.physical, &dev.props);
    vkGetPhysicalDeviceMemoryProperties(dev.physical, &dev.memory);
    menu_first_run_model(dev.props.vendorID, dev.props.deviceID);   // (the DLSS model row's first-run default, by the GPU)
    return true;
}

/// (longshot #2306, #444) Whether the shaders' float adds to storage buffers are asked to be the GPU's own atomic (create_device):
/// RX_VK_NATIVE_FADD (0, false, off, no or empty is off, any other value on); unset, with Intel's Windows driver on the Arc
/// B-series cards, Battlemage (PCI device 0xE2xx; run on a B580, 0xE20B, in Kokiri Forest: the compare-exchange loop lost the
/// device in the sky's light sums, on entering it in every run), from the driver that was run, 101.9154, on (Intel's Windows
/// driverVersion is 101 << 14 | 9154). Longshot also turns it on by default with NVIDIA's driver (0.4-0.6 ms of GPU time less,
/// 7%, in Kakariko on an RTX 4090); here NVIDIA keeps the loop until it is measured on this renderer (RX_VK_NATIVE_FADD=1),
/// so that nothing changes there, nor the translator's stamp. Every other GPU keeps the loop too
static bool env_off(const char* v) {
    std::string s(v);
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s.empty() || s == "0" || s == "false" || s == "off" || s == "no";
}
static bool native_fadd_wanted(const char* env, VkDriverId driver, uint32_t device, uint32_t version) {
    if (env) return !env_off(env);
    if (driver != VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) return false;
    // (longshot #191, #816) also Meteor Lake and Arrow Lake iGPUs (0x7Dxx, 101.9175 on: the loop lost the device in Kokiri Forest,
    // Kakariko and on Death Mountain Trail) and Panther Lake's Xe3 iGPUs (0xB0xx, 101.9150 on: on an Arc B390 the loop lost adds of
    // the exposure's light sums, a milky white haze from the sky on; the native add gave a B580's picture)
    return ((device & 0xFF00u) == 0x7D00u && version >= (101u << 14 | 9175u)) ||   // (Meteor Lake, Arrow Lake)
           ((device & 0xFF00u) == 0xE200u && version >= (101u << 14 | 9154u)) ||   // (Battlemage)
           ((device & 0xFF00u) == 0xB000u && version >= (101u << 14 | 9150u));     // (Panther Lake)
}

bool create_device() {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(dev.physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(dev.physical, nullptr, &n, exts.data());
    std::vector<const char*> want = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    auto optional = [&](const char* name, bool& flag) {
        flag = has_extension(exts, name);
        if (flag) want.push_back(name);
    };
    // (RX_VK_FORCE_MESH_FALLBACK=1, a test: mesh shaders not used where they are, so the no-mesh fallback the GTX 10xx and RX 5000
    // cards run (issue 640) can be tried on any GPU)
    // (hookshot, issue 2245; the switch is Longshot's 8694e3c6) mesh_mode.h: RX_VK_MESH_SHADERS=native or compute chooses, unset is
    // Automatic, and RX_VK_FORCE_MESH_FALLBACK=1 is Compute over it. Automatic is the compute path on Linux with an RDNA 2 or
    // RDNA 3.5 AMD GPU (RADV, mesh_mode.h's ID list: its native task/mesh draws lose the GPU on RDNA 2, issue 51, and draw white
    // foliage and glows on RDNA 3.5, issue 81; other RDNA 3 and 4 keep native): native keeps them there. A GPU without mesh
    // shaders runs the compute path whatever the choice
#ifdef _WIN32
    constexpr bool linux_os = false;
#else
    constexpr bool linux_os = true;
#endif
    const mesh_rule::Choice mesh_choice = mesh_rule::effective(
        mesh_rule::choose(getenv("RX_VK_MESH_SHADERS"), getenv("RX_VK_FORCE_MESH_FALLBACK")), linux_os, dev.props.vendorID,
        dev.props.deviceID);
    if (mesh_choice.mode == 2) dev.mesh_shader = false;
    else optional(VK_EXT_MESH_SHADER_EXTENSION_NAME, dev.mesh_shader);
    optional(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, dev.external_memory_host);
    optional(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, dev.external_memory_fd);   // (guest memory as udmabufs: vk_memory.cpp import_dma_buf)
    if (dev.external_memory_fd) optional(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, dev.dma_buf_import);
    optional(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, dev.pipeline_library);
    if (dev.pipeline_library) optional(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, dev.graphics_pipeline_library);
    optional(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, dev.custom_border_color);
    optional(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, dev.dynamic_state3);
    optional(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, dev.depth_clip_control);
    optional(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, dev.null_descriptor);
    optional(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, dev.index_uint8);
    optional(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, dev.push_descriptor);
    optional(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, dev.conditional_rendering);
    optional(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, dev.provoking_vertex);
    optional(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, dev.vertex_divisor);
    optional(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME, dev.stencil_export);
    optional(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, dev.memory_budget);
    optional(VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME, dev.view_2d_of_3d);
    dev.pci_bus_info = has_extension(exts, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);   // (properties only: nothing to enable)
    // (hookshot) on by default: with a marker before each draw and dispatch the device has not been lost (five
    // runs); without them it was, every run, shortly after OoT's first gameplay frames (nvlddmkm event 153; the
    // extension alone, RX_VK_CHECKPOINTS=ext, did not keep it). The race they hide is not found yet.
    // RX_VK_CHECKPOINTS=0: off
    if (!getenv("RX_VK_CHECKPOINTS") || strcmp(getenv("RX_VK_CHECKPOINTS"), "0") != 0)
        optional(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, dev.checkpoints);
    // (asked for only: a marker written twice a draw, where the NV ones are not there to hide a race)
    if (const char* c = getenv("RX_VK_CHECKPOINTS"); !dev.checkpoints && c && strcmp(c, "0") != 0 && strcmp(c, "ext") != 0)
        optional(VK_AMD_BUFFER_MARKER_EXTENSION_NAME, dev.buffer_marker);
    optional(VK_EXT_DEVICE_FAULT_EXTENSION_NAME, dev.device_fault);
    optional(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, dev.coop_matrix);
    // (longshot #2306) the GPU's own float add on storage buffers: the game's RED/ATOM.ADD.F32 become one OpAtomicFAddEXT instead
    // of a compare-exchange loop, which gives up after CAS_LOOP_MAX_TRIES attempts and loses that add (eden
    // spirv_emit_context.cpp DefineStorageBuffers). By default on Battlemage only (native_fadd_wanted); RX_VK_NATIVE_FADD=1
    // asks for it on any GPU, =0 keeps the loop on every one, the extension not even enabled
    const char* const fadd_env = getenv("RX_VK_NATIVE_FADD");
    VkPhysicalDeviceDriverProperties fadd_driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    {
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &fadd_driver;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    }
    const bool fadd_wanted = native_fadd_wanted(fadd_env, fadd_driver.driverID, dev.props.deviceID, dev.props.driverVersion);
    // (Longshot c392df3a: says the spilling rate Mesa's Intel compiler (ANV) will use. The renderer does not set it here: see
    // create_instance's note. The driverID alone tells the driver, so no platform guard is needed)
    if (fadd_driver.driverID == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA && getenv("shader_spilling_rate"))
        logf("vk: Mesa's Intel compiler spills at rate %s (driconf shader_spilling_rate: set by the player; 2 was measured as "
             "the best on an Iris Xe, Longshot c392df3a)", getenv("shader_spilling_rate"));
    bool atomic_float = false;
    if (fadd_wanted) optional(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME, atomic_float);
    optional(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME, dev.swap_mutable);   // (hookshot: the overlays, vk_imgui.cpp)
    if (getenv("RX_VK_TL") || getenv("RX_VK_TL_S")) optional(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, dev.calibrated_ts);   // (stream R2: the timeline's GPU side)
#ifndef _WIN32
    // (Linux) tl_gpu_dump asks for the device's and the host clock's time (vk_tl_clock.h: CLOCK_MONOTONIC, the shim's
    // QueryPerformanceCounter); a domain the device does not list may not be asked for, so the GPU side (and RX_VK_TL_PROBE) is off
    if (dev.calibrated_ts) {
        const auto domains_fn = (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)instance_proc()(dev.instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
        uint32_t nd = 0;
        VkTimeDomainEXT domains[16];
        if (domains_fn && domains_fn(dev.physical, &nd, nullptr) == VK_SUCCESS) {
            nd = std::min<uint32_t>(nd, 16);
            if (domains_fn(dev.physical, &nd, domains) < 0) nd = 0;
        } else {
            nd = 0;
        }
        std::string names;
        for (uint32_t i = 0; i < nd; i++) names += std::string(i ? ", " : "") + tl_time_domain_name(domains[i]);
        if (tl_time_domains_usable(domains, nd)) {
            logf("vk: tl: GPU side calibrated against %s (domains: %s)", tl_time_domain_name(tl_host_time_domain()), names.c_str());
        } else {
            logf("vk: tl: GPU side off: the device lists no %s time domain beside DEVICE (domains: %s)", tl_time_domain_name(tl_host_time_domain()),
                 names.empty() ? "none" : names.c_str());
            dev.calibrated_ts = false;
        }
    }
#endif
    dlss_device_extensions(want, exts);   // (hookshot: NVIDIA DLSS's, vk_dlss.cpp)
    dlss_fg_device_extensions(want, exts);   // (and DLSS Frame Generation's, which NGX names: vk_dlss.cpp)
    bool fsr_descriptor_buffer = false;   // (hookshot, linux: only with a Vulkan FSR 4 library beside the game, vk_fsr4.cpp)
    if (fsr4_wants_descriptor_buffer()) optional(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, fsr_descriptor_buffer);
    reflex_device_extensions(want, exts);   // (hookshot: NVIDIA Reflex's, vk_reflex.cpp)

    // the queue: one family for graphics, compute and transfer
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physical, &n, families.data());
    dev.queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < n && dev.queue_family == UINT32_MAX; i++)
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            dev.queue_family = i;
    if (dev.queue_family == UINT32_MAX) return false;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = dev.queue_family;
    const float priorities[2] = {getenv("RX_VK_QUEUE_PRIO") ? (float)atof(getenv("RX_VK_QUEUE_PRIO")) : 1.0f, 1.0f};   // (stream R2: RX_VK_QUEUE_PRIO, a diagnostic)
    q.queueCount = 1;
    if (getenv("RX_VK_TL_PROBE") && atoi(getenv("RX_VK_TL_PROBE")) >= 2 && families[dev.queue_family].queueCount >= 2) q.queueCount = 2;   // (stream R2)
    q.pQueuePriorities = priorities;
    // (hookshot) The presents on a queue of a compute family of their own, beside the replay's (RX_VK_PRESENT_QUEUE=1 / 0: on /
    // off; on by default with NVIDIA's driver on Windows, where it was measured). On the replay's queue vkQueuePresentKHR blocked
    // with dev.present_mu held, ~0.8 ms a present with frame interpolation at 4x and up to 8, and the game's main and presentation
    // threads waited there to submit. A second queue of the graphics family took that lock off the presents, but its presents
    // blocked longer (~2-4 ms: the graphics work of the two queues takes turns on the GPU) and the made frames were shown late
    // four times as often. A compute family's queue runs beside the graphics work: in Hyrule Field (a 14700K and an RTX 5070 Ti,
    // 4x, 175 Hz) the game went from ~52 to ~58 fps standing and ~46.5 to ~53 running, its frames over 22 ms from ~13% to ~1%
    // standing and over 40 ms from ~4% to ~0.2% running, the made frames as evenly shown standing. The swapchain's images are
    // shared by both families (vk_main.cpp create_swapchain), and a family that cannot present to the window leaves it off.
    uint32_t compute_family = UINT32_MAX;
    {
        bool want = false;
#ifdef _WIN32
        VkPhysicalDeviceDriverProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &dp;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        want = false;   // (v2.0.9: off by default with the write-back diff until issue 1161's device lost is placed; was:
        (void)dp;       //  NVIDIA's driver on Windows. RX_VK_PRESENT_QUEUE=1: on)
#endif
        if (const char* e = getenv("RX_VK_PRESENT_QUEUE")) want = atoi(e) != 0;
        for (uint32_t i = 0; want && i < n && compute_family == UINT32_MAX; i++)
            if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                compute_family = i;
    }
    VkDeviceQueueCreateInfo qs[3] = {q, {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}, {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
    uint32_t queue_infos = 1;
    if (compute_family != UINT32_MAX) {
        qs[queue_infos].queueFamilyIndex = compute_family;
        qs[queue_infos].queueCount = 1;
        qs[queue_infos].pQueuePriorities = &priority;
        queue_infos++;
    }
    // (Linux, issue 14: RX_VK_SPARSE_QUEUE=1) a queue of another family for the binds of the pools made of several dma-bufs,
    // whose fence on the replay's queue waited for every frame queued before it (sparse_queue.h). Off: no queue more
    dev.sparse_family = UINT32_MAX;
#ifdef __linux__
    if (const char* e = getenv("RX_VK_SPARSE_QUEUE"); e && atoi(e) != 0) {
        static_assert(sparse_queue::kSparseBinding == VK_QUEUE_SPARSE_BINDING_BIT && sparse_queue::kGraphics == VK_QUEUE_GRAPHICS_BIT);
        std::vector<uint32_t> flags(n);
        for (uint32_t i = 0; i < n; i++) flags[i] = families[i].queueFlags;
        dev.sparse_family = sparse_queue::pick_family(flags.data(), n, dev.queue_family, compute_family);
        if (dev.sparse_family != UINT32_MAX) {
            qs[queue_infos].queueFamilyIndex = dev.sparse_family;
            qs[queue_infos].queueCount = 1;
            qs[queue_infos].pQueuePriorities = &priority;
            queue_infos++;
            logf("vk: RX_VK_SPARSE_QUEUE: the pools' sparse binds on a queue of family %u (flags %#x), not the replay's",
                 dev.sparse_family, flags[dev.sparse_family]);
        } else
            logf("vk: RX_VK_SPARSE_QUEUE: the pools' sparse binds stay on the replay's queue (no other family has sparse binding)");
    }
#endif

    // features: what the hardware has of what the replay uses (all of them on the GPUs MKW-class hardware maps to)
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT robust2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT u8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &f11, f11.pNext = &f12, f12.pNext = &f13;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT cond{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    void** tail = &f13.pNext;
    if (dev.mesh_shader) *tail = &mesh, tail = &mesh.pNext;
    if (dev.null_descriptor) *tail = &robust2, tail = &robust2.pNext;
    if (dev.index_uint8) *tail = &u8, tail = &u8.pNext;
    if (dev.dynamic_state3) *tail = &eds3, tail = &eds3.pNext;
    if (dev.custom_border_color) *tail = &border, tail = &border.pNext;
    if (dev.depth_clip_control) *tail = &clip, tail = &clip.pNext;
    if (dev.conditional_rendering) *tail = &cond, tail = &cond.pNext;
    if (dev.provoking_vertex) *tail = &pv, tail = &pv.pNext;
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vdiv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    if (dev.vertex_divisor) *tail = &vdiv, tail = &vdiv.pNext;
    if (dev.graphics_pipeline_library) *tail = &gpl, tail = &gpl.pNext;
    VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    if (dev.device_fault) *tail = &fault, tail = &fault.pNext;
    VkPhysicalDeviceImage2DViewOf3DFeaturesEXT v2d3d{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT};
    if (dev.view_2d_of_3d) *tail = &v2d3d, tail = &v2d3d.pNext;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    if (dev.coop_matrix) *tail = &coop, tail = &coop.pNext;
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    if (fsr_descriptor_buffer) *tail = &descriptor_buffer, tail = &descriptor_buffer.pNext;
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT afloat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    if (atomic_float) *tail = &afloat, tail = &afloat.pNext;
    reflex_device_features(tail);   // (present ids, when NVIDIA Reflex's extensions are there)
    vkGetPhysicalDeviceFeatures2(dev.physical, &f);
    descriptor_buffer.descriptorBufferCaptureReplay = VK_FALSE;
    dev.view_2d_of_3d = dev.view_2d_of_3d && v2d3d.sampler2DViewOf3D;
    coop.cooperativeMatrixRobustBufferAccess = VK_FALSE;
    dev.coop_matrix = dev.coop_matrix && coop.cooperativeMatrix &&
                      f12.vulkanMemoryModel && f12.vulkanMemoryModelDeviceScope;
    {   // (only the float add on buffers on: the one the translator emits)
        dev.float_atomic_add = atomic_float && afloat.shaderBufferFloat32AtomicAdd;
        void* const next = afloat.pNext;
        afloat = VkPhysicalDeviceShaderAtomicFloatFeaturesEXT{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
        afloat.pNext = next;
        afloat.shaderBufferFloat32AtomicAdd = dev.float_atomic_add ? VK_TRUE : VK_FALSE;
        const char* const how = dev.float_atomic_add ? "native (VK_EXT_shader_atomic_float)"
                                : !fadd_wanted       ? "compare-exchange loop"
                                : atomic_float       ? "compare-exchange loop (no shaderBufferFloat32AtomicAdd)"
                                                     : "compare-exchange loop (no VK_EXT_shader_atomic_float)";
        if (fadd_env) logf("vk: float atomic add on storage buffers: %s, RX_VK_NATIVE_FADD=%s", how, fadd_env);
        else logf("vk: float atomic add on storage buffers: %s, %s", how,
                  fadd_wanted ? "Intel's Windows driver, Battlemage"
                              : "by default only with Intel's Windows driver on Battlemage (RX_VK_NATIVE_FADD=1: native where the GPU has it)");
    }
    // (bounds-checked buffer access: an out-of-range access dropped instead of faulting the device. On by default: the GPU
    // culling pass 53caac5f faulted the device while a save loaded, about one start in three, and the scene's lighting read garbage
    // for single frames (the black flashes); with it, neither in A/B runs at the same GPU time. Lichii's OoT branch keeps it on too.
    // The 'Robust buffers' row of hookshot_options.ini chooses (0 Auto: on, on every vendor; 1 Off; 2 On); RX_VK_ROBUST=0 or 1
    // over it. Off is null descriptors only: some NVIDIA cards ran slower with it on, issues 394, 418, 419)
    static const bool robust = []() -> bool {
        if (const char* e = getenv("RX_VK_ROBUST")) return atoi(e) != 0;
        char exe[MAX_PATH];
        const DWORD n = game_module_path(exe, MAX_PATH);   // (the game folder, not bin beside the launcher: vk_paths.h)
        std::string ini(exe, n);
        ini = ini.substr(0, ini.find_last_of("\\/") + 1) + "hookshot_options.ini";
        int v = 0;   // (Auto)
        if (FILE* f = fopen(ini.c_str(), "r")) {
            char line[80];
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "robust_buffers=", 15)) {
                    v = atoi(line + 15);
                    break;
                }
            fclose(f);   // (once: the PR closed it twice when the row said Auto)
        }
        return v != 1;   // (Auto and On: on)
    }();
    const bool robust_ok = robust && robust2.robustBufferAccess2 && f.features.robustBufferAccess;
    robust2.robustBufferAccess2 = robust_ok ? VK_TRUE : VK_FALSE;
    robust2.robustImageAccess2 = VK_FALSE;
    dev.null_descriptor = dev.null_descriptor && robust2.nullDescriptor;
    pv.transformFeedbackPreservesProvokingVertex = VK_FALSE;
    dev.provoking_vertex = dev.provoking_vertex && pv.provokingVertexLast;
    dev.vertex_divisor = dev.vertex_divisor && vdiv.vertexAttributeInstanceRateDivisor;   // (a divisor over 1 needs the feature, not only the extension; Longshot 89447c39)
    dev.conditional_rendering = dev.conditional_rendering && cond.conditionalRendering;
    dev.custom_border_color = dev.custom_border_color && border.customBorderColors && border.customBorderColorWithoutFormat;
    dev.depth_clip_control = dev.depth_clip_control && clip.depthClipControl;
    dev.graphics_pipeline_library = dev.graphics_pipeline_library && gpl.graphicsPipelineLibrary;
    if (dev.graphics_pipeline_library) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT lp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &lp;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        dev.graphics_pipeline_library = lp.graphicsPipelineLibraryFastLinking != VK_FALSE;
    }
    dev.eds3_polygon = dev.dynamic_state3 && eds3.extendedDynamicState3PolygonMode;
    dev.eds3_clamp = dev.dynamic_state3 && eds3.extendedDynamicState3DepthClampEnable;
    dev.eds3_blend = dev.dynamic_state3 && eds3.extendedDynamicState3ColorBlendEnable &&
                     eds3.extendedDynamicState3ColorBlendEquation && eds3.extendedDynamicState3ColorWriteMask;
    dev.eds3_a2c = dev.dynamic_state3 && eds3.extendedDynamicState3AlphaToCoverageEnable;
    {   // (hookshot) which denorm modes a shader may declare: all programs say them, and not every device has them (strip_float_controls)
        VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &p12;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        dev.denorm_preserve_widths = (p12.shaderDenormPreserveFloat16 ? 1u : 0u) | (p12.shaderDenormPreserveFloat32 ? 2u : 0u) |
                                     (p12.shaderDenormPreserveFloat64 ? 4u : 0u);
        dev.denorm_flush_widths = (p12.shaderDenormFlushToZeroFloat16 ? 1u : 0u) | (p12.shaderDenormFlushToZeroFloat32 ? 2u : 0u) |
                                  (p12.shaderDenormFlushToZeroFloat64 ? 4u : 0u);
        logf("vk: float controls: denorm preserve widths %#x, flush to zero %#x (modes the device lacks are taken out of the shaders)",
             dev.denorm_preserve_widths, dev.denorm_flush_widths);
    }
    dev.vk12 = f12;
    dev.vk13 = f13;
    {   // (hookshot, after TotK Recomp's vk_caps.c) NVN's programs are written for 32 lanes: the translator is told 32, and the lane
        // shuffles of OoT's networks and the lane masks of its mesh programs depend on it. NVIDIA's GPUs have no other size; AMD's
        // default to 64 where they like, and the networks' output came out as coloured noise (Hookshot issues 10 and 14)
        VkPhysicalDeviceSubgroupSizeControlProperties sc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        sg.pNext = &sc;
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &sg;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        const bool pin = f13.subgroupSizeControl && sc.minSubgroupSize <= 32 && sc.maxSubgroupSize >= 32 && !getenv("RX_VK_NO_SUBGROUP_PIN");
        dev.sg32_stages = pin ? sc.requiredSubgroupSizeStages : 0;
        logf("vk: subgroups: default %u lanes, %u-%u, pinned to 32 on stages %#x", sg.subgroupSize, sc.minSubgroupSize, sc.maxSubgroupSize,
             (unsigned)dev.sg32_stages);
        {   // (issues 67 and 37) a stage the GPU does not hold to 32 lanes, with a default of more, runs the programs' lane masks and
            // shuffles on the wrong lanes unless the translator emulates 32-lane warps (yuzu's way). Per stage: GCN / Polaris (64 only, no
            // size control) takes every stage; RDNA 3.5 on AMD's Windows driver pins fragment, compute, task and mesh (0xf0) but not the
            // vertex stage, which runs at 64. Stages that run at 32 (NVIDIA, Arc, Intel: default 32; every pinned stage) translate as
            // before. RX_VK_WARP_WIDE=1 forces every stage, a test: on a 32-lane GPU the emulation must change nothing in the picture
            const bool forced = getenv("RX_VK_WARP_WIDE") && atoi(getenv("RX_VK_WARP_WIDE")) != 0;
            constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT |
                                                   VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
            if (forced) dev.warp_wide_stages = kStages;
            else if (sg.subgroupSize > 32) dev.warp_wide_stages = kStages & ~dev.sg32_stages;
            if (dev.warp_wide_stages) {
                dev.warp_lanes = std::max<uint32_t>({sg.subgroupSize, sc.maxSubgroupSize, forced ? 64u : 33u});   // (forced: 64, so the wide code is made even on 32 lanes)
                logf("vk: subgroups wider than the guest's 32 lanes on stages %#x: programs translated for %u lanes, 32-lane warps emulated%s",
                     (unsigned)dev.warp_wide_stages, dev.warp_lanes, forced ? " (RX_VK_WARP_WIDE)" : "");
            }
        }
    }
    if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore) {
        logf("vk: the GPU lacks dynamic rendering, synchronization2 or timeline semaphores");
        return false;
    }
    mesh.multiviewMeshShader = mesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;   // (not used)
    dev.mesh_shader = dev.mesh_shader && mesh.meshShader && mesh.taskShader;
    // (hookshot, issue 640) RDNA1 and older GPUs: no mesh shaders, but task and mesh programs run as compute shaders
    // that write their vertices to buffers, drawn through an ordinary graphics pipeline. It needs buffer device addresses.
    dev.mesh_fallback = !dev.mesh_shader && f12.bufferDeviceAddress && !getenv("RX_VK_NO_MESH_FALLBACK");
    if (dev.mesh_fallback) logf("vk: no mesh shaders: task and mesh programs run as compute shaders (issue 640 fallback)");
    // (hookshot, issue 996 and HeyListen issue 57; Hookshot d1d6caa7) a GPU with mesh shaders whose draws with a task stage run as
    // compute: the default on AMD's Windows driver for RDNA 2 (Navi 21 to 23, mesh_mode.h's device list; the RX 6700 XT drew the Market
    // dog as a flat pale silhouette), RX_VK_TASK_MESH_FALLBACK=0/1 decides over it, and Native (RX_VK_MESH_SHADERS=native) turns the
    // default off. Mesh-only draws stay native. The Compute choice has no mesh shaders here: the whole compute path runs
    bool task_mesh_fallback = mesh_rule::task_mesh_fallback_wanted(!linux_os, dev.props.vendorID, dev.props.deviceID, mesh_choice.mode,
                                                                   getenv("RX_VK_TASK_MESH_FALLBACK"));
    dev.task_mesh_fallback = task_mesh_fallback && dev.mesh_shader && f12.bufferDeviceAddress && !getenv("RX_VK_NO_MESH_FALLBACK");
    if (dev.task_mesh_fallback) logf("vk: task + mesh programs use the compute fallback (RX_VK_TASK_MESH_FALLBACK=0: native)");
    logf("vk: mesh shaders: %s, from %s: %s", mesh_choice.mode == 2 ? "Compute" : mesh_choice.mode == 1 ? "Native" : "Automatic",
         mesh_choice.why, dev.mesh_shader ? (dev.task_mesh_fallback ? "native, task + mesh draws on the compute path" : "native")
                         : dev.mesh_fallback ? "the compute path" : "none");
    if (robust) logf("vk: robust buffer access %s", robust_ok ? "on" : "not supported");
    f.features.robustBufferAccess = robust_ok ? VK_TRUE : VK_FALSE;   // (off by default: the game's accesses stay in their buffers)
    dev.features = f.features;
    dev.sparse_images = (families[dev.queue_family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) && f.features.sparseBinding &&
                        f.features.sparseResidencyImage2D && !(getenv("RX_VK_SPARSE") && !atoi(getenv("RX_VK_SPARSE")));
    dev.sparse_buffers = (families[dev.queue_family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) && f.features.sparseBinding;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f;
    ci.queueCreateInfoCount = queue_infos;
    ci.pQueueCreateInfos = qs;
    ci.enabledExtensionCount = (uint32_t)want.size();
    ci.ppEnabledExtensionNames = want.data();
#ifndef _WIN32
    {   // (Linux) Mesa's submit thread, read by Mesa in this vkCreateDevice: decided at process start, before any thread, and
        // set for the whole process (host/hos_submit_thread.h, hos_main.c submit_thread_at_start): only said here, with the
        // device now known. Nothing to say without an AMD card (the host's note is empty) or an older host (no note)
        using Note = const char* (*)();
        const Note note = (Note)dlsym(RTLD_DEFAULT, "hos_posix_submit_thread_note");
        const char* said = note ? note() : nullptr;
        if (said && *said) {
            if (fadd_driver.driverID == VK_DRIVER_ID_MESA_RADV) logf("vk: RADV: Mesa's submit thread %s (decided at start)", said);
            else logf("vk: the picked device is not RADV (driver %d): Mesa's submit thread was decided at start for the AMD card (%s), "
                      "it reaches this device only if it is another Mesa driver", (int)fadd_driver.driverID, said);
        }
    }
#endif
    const VkResult created = vkCreateDevice(dev.physical, &ci, nullptr, &dev.device);
    if (created != VK_SUCCESS) {
        logf("vk: vkCreateDevice failed");
        return false;
    }


    load_device(dev.device);
    guard_shader_modules();   // (Hookshot b9b48dfd)
    // (hookshot, issue 640) after the load, in front of the driver's: our own module guard (guarded_shader_module, vk_draw.cpp)
    // calls the pointer this chains onto, so a module is made through both
    fix_fallback_modules();
    vkGetDeviceQueue(dev.device, dev.queue_family, 0, &dev.queue);
    dev.probe_queue = VK_NULL_HANDLE;
    if (q.queueCount >= 2) vkGetDeviceQueue(dev.device, dev.queue_family, 1, &dev.probe_queue);
    dev.present_queue = VK_NULL_HANDLE;
    dev.present_family = dev.queue_family;
    if (compute_family != UINT32_MAX) vkGetDeviceQueue(dev.device, compute_family, 0, &dev.present_queue), dev.present_family = compute_family;
    dev.sparse_queue = VK_NULL_HANDLE;
    if (dev.sparse_family != UINT32_MAX) vkGetDeviceQueue(dev.device, dev.sparse_family, 0, &dev.sparse_queue);
    if (dev.buffer_marker) buffer_markers_init();
    coopmat_init();
    reflex_device_created();
    // (longshot: on the Steam Deck the GPU held at amdgpu's peak power state while the game runs (RX_VK_GPU_CLOCK=0: not), gpu_clock.h;
    // the device's render node from VK_EXT_physical_device_drm, which needs no enabling to be asked)
    gpu_clock_device_created(has_extension(exts, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME));
    if (dev.push_descriptor) {
        VkPhysicalDevicePushDescriptorPropertiesKHR pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &pp;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        dev.max_push_descriptors = pp.maxPushDescriptors;
    }
    if (dev.vertex_divisor) {   // (Longshot b46d7732: a divisor is at most maxVertexAttribDivisor, VUID-...-divisor-01870)
        VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT vp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &vp;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        dev.max_vertex_divisor = vp.maxVertexAttribDivisor;
    }
    return true;
}

} // namespace

/// (hookshot) The UNORM format of an sRGB one (the same bytes, read and written as they are), else the format itself
VkFormat unorm_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return f;
    }
}
/// (hookshot) The views of the swapchain's images let go (before the swapchain they view: the device idle already)
void swap_views_drop() {
    for (auto view : dev.swap_views)
        if (view) vkDestroyImageView(dev.device, view, nullptr);
    dev.swap_views.clear();
}
/// (hookshot) A view of each of the swapchain's images, for Dear ImGui's overlays (vk_imgui.cpp), when they can be drawn into,
/// in the format given (the swapchain's, or its UNORM one when the swapchain was made mutable)
void swap_views_make(VkImageUsageFlags usage, VkFormat format) {
    swap_views_drop();
    dev.swap_views.assign(dev.swap_images.size(), VK_NULL_HANDLE);
    dev.swap_view_format = format;
    if (!(usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return;
    for (size_t i = 0; i < dev.swap_images.size(); i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = dev.swap_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(dev.device, &vi, nullptr, &dev.swap_views[i]) != VK_SUCCESS) dev.swap_views[i] = VK_NULL_HANDLE;
    }
}

bool create_swapchain(uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps;
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.physical, dev.surface, &caps);
    if (result != VK_SUCCESS) {
        logf("vk: querying surface capabilities failed (VkResult %d)", (int)result);
        return false;
    }
    if (caps.currentExtent.width != UINT32_MAX) width = caps.currentExtent.width, height = caps.currentExtent.height;
    // (a minimised window has a 0x0 surface and the game presents every frame: said once, not a thousand times)
    static bool told_empty;
    if (width && height) told_empty = false;
    if ((width && height) || !told_empty) {
        told_empty = !width || !height;
        logf("vk: surface caps images %u..%u extent %ux%u..%ux%u usage %#x alpha %#x transform %#x",
             caps.minImageCount, caps.maxImageCount, caps.minImageExtent.width, caps.minImageExtent.height,
             caps.maxImageExtent.width, caps.maxImageExtent.height, caps.supportedUsageFlags,
             caps.supportedCompositeAlpha, caps.supportedTransforms);
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        logf("vk: surface does not support transfer-destination swapchain images");
        return false;
    }
    if (!width || !height) return false;
    uint32_t n = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical, dev.surface, &n, nullptr);
    if (result != VK_SUCCESS || !n) {
        logf("vk: querying surface formats failed (VkResult %d, count %u)", (int)result, n);
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical, dev.surface, &n, formats.data());
    VkSurfaceFormatKHR fmt = formats[0];
    for (const auto& f : formats)   // (an UNORM target: the game's window textures are presented as they are)
        // (in sRGB: RADV on Wayland lists every format again with PASS_THROUGH_EXT and the compositor's colour spaces,
        // and the last UNORM entry left the compositor an untagged surface, a grey picture. Still the last sRGB one, so
        // a driver that lists only sRGB (NVIDIA, X11, Windows) keeps the format it had.)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            fmt = f;
    // (hookshot) the Insert menu's HDR: a 10-bit HDR10 surface when the display lists one (Windows: "Use HDR" on in its display
    // settings), the picture then written by framegen_hdr. Otherwise the SDR one above, said once
    dev.hdr = false;
    // (HeyListen) whether the display lists one is looked at with the HDR row Off too: the menu says so, and shows its HDR rows only
    // where HDR can run (menu_rows.h hdr_effective)
    VkSurfaceFormatKHR hdr_fmt = fmt;
    dev.hdr_listed = false;
    for (const auto& f : formats)
        if ((f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) &&
            f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
            hdr_fmt = f, dev.hdr_listed = true;
    if (opts.hdr) {
        if (dev.hdr_listed) fmt = hdr_fmt, dev.hdr = true;
        static bool told;
        if (!told) {
            told = true;
            if (dev.hdr)
                logf("vk: HDR: an HDR10 surface (format %d), paper white %d nits, peak %d nits", (int)fmt.format, opts.hdr_white,
                     opts.hdr_peak);
            else
                logf("vk: HDR: the display lists no HDR10 surface%s: SDR",
                     dev.swapchain_colorspace ? " (is HDR on in the display settings?)" : " (no VK_EXT_swapchain_colorspace)");
        }
    }
    // mailbox when the driver has it (latest frame, no tear). The game paces itself (nn_vi.c).
    n = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical, dev.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes;
    if (result == VK_SUCCESS && n) {
        modes.resize(n);
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical, dev.surface, &n, modes.data());
    }
    if (result != VK_SUCCESS || modes.empty())
        logf("vk: querying present modes failed (VkResult %d, count %u); trying mandatory FIFO mode", (int)result, n);
    // (the Insert menu's V-Sync and V-Sync mode rows, RX_VK_FIFO and RX_VK_IMMEDIATE over them: present_mode.h)
    unsigned offered = 0;
    for (auto m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) offered |= present_mode::kHasMailbox;
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) offered |= present_mode::kHasImmediate;
    }
    dev.present_modes_offered = offered;
    dev.present_modes_known = true;   // (the V-Sync rows say when they cannot have what they ask)
    dev.swap_min_images = caps.minImageCount;
    dev.swap_max_images = caps.maxImageCount;
    present_mode::Input pin;
    pin.row = opts.vsync;
    pin.buffering = opts.vsync_mode;
    const present_mode::VrrState vrr = vrr_resolved(true);   // (logs its choice and why, once for each change)
    pin.vrr = vrr.active;
    pin.env_fifo = getenv("RX_VK_FIFO") != nullptr;   // (as on a card or driver with vsync forced)
    pin.env_immediate = getenv("RX_VK_IMMEDIATE") != nullptr;
    pin.offered = offered;
#ifdef _WIN32
    pin.windows = true;
#endif
    // (Linux: immediate on NVIDIA's X11 path turns vblank off for every output, so the other monitor and other programs tear:
    //  only asked for, by RX_VK_IMMEDIATE=1 or the row)
    const present_mode::Mode picked = present_mode::pick(pin);
    const VkPresentModeKHR mode = picked == present_mode::kMailboxMode ? VK_PRESENT_MODE_MAILBOX_KHR
                                  : picked == present_mode::kImmediateMode ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                                            : VK_PRESENT_MODE_FIFO_KHR;
    dev.vrr_active = vrr.active && !pin.env_fifo && !pin.env_immediate;
    if (vrr.active && picked != present_mode::kFifo)
        logf("vk: VRR: %s is used, the environment's choice: no FIFO", present_mode::mode_name(picked));
    // (the V-Sync mode row's Mailbox, where there is none: AMD's Windows driver lists no MAILBOX. Said once, and the row's
    // hint says it while it is chosen)
    const bool want_mailbox = opts.vsync == present_mode::kOn && opts.vsync_mode == present_mode::kMailbox;
    if (want_mailbox && picked != present_mode::kMailboxMode) {
        if (pin.env_fifo || pin.env_immediate)
            logf("vk: V-Sync: RX_VK_FIFO / RX_VK_IMMEDIATE decide the present mode: %s, not Mailbox", present_mode::mode_name(picked));
        else if (!(offered & present_mode::kHasMailbox))
            logf("vk: V-Sync: this surface offers no Mailbox: FIFO (triple buffering) is used");
        else
            logf("vk: V-Sync: VRR is on: FIFO, not Mailbox");
    }
    // (the image count: FIFO double 2, FIFO triple 3, Mailbox 3 (4 with frame generation); frame generation's images kept
    // whatever the row says, then the surface's minImageCount/maxImageCount)
    const present_mode::Images plan = present_mode::image_plan(pin, picked, framegen_on(), caps.minImageCount, caps.maxImageCount);
    if (plan.framegen_raised)
        logf("vk: V-Sync: frame generation needs %d images: %s asks for %d, %d used", plan.count,
             present_mode::buffering_name(opts.vsync_mode), plan.want, plan.count);
    if (plan.min_raised)
        logf("vk: V-Sync: the surface asks for at least %u images%s: %d used", caps.minImageCount,
             opts.vsync == present_mode::kOn && opts.vsync_mode == present_mode::kFifoDouble ? " (FIFO double buffering is not possible here)" : "",
             plan.count);
    if (plan.max_capped) logf("vk: V-Sync: the surface allows at most %u images: %d used", caps.maxImageCount, plan.count);
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = dev.surface;
    ci.minImageCount = (uint32_t)plan.count;
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = {width, height};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // The renderer copies into swapchain images. Readback is optional and only available on surfaces that allow it.
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // (hookshot) the overlays Dear ImGui draws (vk_imgui.cpp) render into it. Every surface allows it (the Vulkan spec)
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        ci.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha)) {
        for (VkCompositeAlphaFlagBitsKHR alpha : {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                                  VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                                                  VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (caps.supportedCompositeAlpha & alpha) { ci.compositeAlpha = alpha; break; }
    }
    ci.presentMode = mode;
    const uint32_t sharing[2] = {dev.queue_family, dev.present_family};
    if (dev.present_queue && dev.present_family != dev.queue_family) {   // (presented from another family: no ownership moves)
        ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices = sharing;
    }
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = dev.swapchain;
    // (hookshot) an sRGB swapchain (some drivers list no UNORM format) made mutable when the driver can: the overlays draw
    // through UNORM views, their colours as they are, as on a UNORM swapchain. Without it, vk_imgui.cpp makes them linear
    const VkFormat view_formats[2] = {fmt.format, unorm_of(fmt.format)};
    VkImageFormatListCreateInfo format_list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    const bool mutable_view = dev.swap_mutable && view_formats[1] != fmt.format && (ci.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (mutable_view) {
        ci.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
        format_list.viewFormatCount = 2;
        format_list.pViewFormats = view_formats;
        format_list.pNext = ci.pNext;
        ci.pNext = &format_list;
    }
    reflex_swapchain_info(ci);   // (NVIDIA Reflex: latency mode on)
    VkSwapchainKHR sc;
    if (dev.swapchain) perf_event(perf_report::kSwapchainRecreated);   // (made again in play: the performance summary counts it)
    result = vkCreateSwapchainKHR(dev.device, &ci, nullptr, &sc);
    // Passing oldSwapchain retires it even when creation fails. Nothing may acquire from it again.
    if (dev.swapchain) {
        device_wait_idle_locked();   // (the present queue's presents of the old swapchain done too)
        reflex_swapchain_destroyed();   // (no latency sleep or markers may use the destroyed chain)
        swap_views_drop();   // (hookshot: the overlays' views of its images, before it)
        for (auto semaphore : dev.swap_rendered) vkDestroySemaphore(dev.device, semaphore, nullptr);
        vkDestroySwapchainKHR(dev.device, dev.swapchain, nullptr);
        dev.swapchain = VK_NULL_HANDLE;
        dev.swap_rendered.clear();
        dev.swap_images.clear();
    }
    if (result != VK_SUCCESS) {
        logf("vk: vkCreateSwapchainKHR failed (VkResult %d, extent %ux%u, format %d, usage %#x, mode %d, images %u)",
             (int)result, width, height, (int)fmt.format, ci.imageUsage, (int)mode, ci.minImageCount);
        return false;
    }
    if (vkGetSwapchainImagesKHR(dev.device, sc, &n, nullptr) != VK_SUCCESS || !n) {
        vkDestroySwapchainKHR(dev.device, sc, nullptr);
        return false;
    }
    std::vector<VkImage> images(n);
    if (vkGetSwapchainImagesKHR(dev.device, sc, &n, images.data()) != VK_SUCCESS || !n) {
        vkDestroySwapchainKHR(dev.device, sc, nullptr);
        return false;
    }
    images.resize(n);
    std::vector<VkSemaphore> rendered(n, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& semaphore : rendered) {
        if (vkCreateSemaphore(dev.device, &si, nullptr, &semaphore) != VK_SUCCESS) {
            for (auto made : rendered)
                if (made) vkDestroySemaphore(dev.device, made, nullptr);
            vkDestroySwapchainKHR(dev.device, sc, nullptr);
            return false;
        }
    }
    dev.swapchain = sc;
    dev.swap_rendered = std::move(rendered);
    dev.swap_format = fmt.format;
    dev.swap_extent = {width, height};
    dev.present_mode_name = mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                                                                                                                      : dev.vrr_active ? "fifo (VRR)" : "fifo";
    dev.swap_images = std::move(images);
    // (one line: V-Sync On or Off, the mode the row asks for, the present mode and the images the swapchain has)
    logf("vk: swapchain %ux%u, %u images, %s (V-Sync %s, %s%s)", width, height, n, present_mode::mode_name(picked),
         opts.vsync == present_mode::kOn ? "On" : "Off",
         opts.vsync == present_mode::kOn ? present_mode::buffering_name(opts.vsync_mode) : "no mode",
         dev.vrr_active ? ", VRR" : "");
    swap_views_make(ci.imageUsage, mutable_view ? view_formats[1] : fmt.format);
    reflex_swapchain_created(sc);   // (NVIDIA Reflex: its sleep mode on the new swapchain)
    return true;
}

namespace {

HWND main_window = nullptr;
// (hookshot) hookshot --prepare-shaders (a builder, after a build): the same work with no game, its progress as
// "@@progress" lines on stdout. A skip there leaves hookshot_prepare_skipped beside the game: its own starts then
// leave the work out too (what was not prepared is built when first drawn), until a --prepare-shaders run ends it
bool prepare_only() { return getenv("HOOKSHOT_PREPARE_ONLY") != nullptr; }
std::string skipped_path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string path(exe, n);
    return path.substr(0, path.find_last_of("\\/") + 1) + "hookshot_prepare_skipped";
}
// (hookshot) The first start after an update prepares every shader (prepare_run) and the pipelines the last sessions noted
// (precompile_run): a minute and more, before the game's first frame. They run on a thread of their own while this one, the
// renderer's start, shows the window at once with what they are doing on it, and keeps it shown and sized; on Windows it is also
// the window's thread, and handles its messages meanwhile (it would not answer otherwise). Closing the window stops them
// (mkwvk_close_asked): what was made is kept, the next start goes on from there. RX_VK_STARTUP_SCREEN=0: as before, all
// of it before the window shows anything (and a console with the count on Windows)
std::atomic<int> prep_stage{0};   // 0 the programs, 1 the pipelines the notes predict (a first start), 2 the noted pipelines
std::atomic<size_t> prep_done{0}, prep_total{0};
std::atomic<bool> prep_running{false}, prep_finished{false};
HANDLE prep_ended = nullptr;
#ifndef _WIN32
const MkwVkWindow* prep_window = nullptr;   // (its size)
#endif
void prep_progress(size_t done, size_t total) {
    prep_done.store(done, std::memory_order_relaxed);
    prep_total.store(total, std::memory_order_relaxed);
}
bool screen_wanted() {
    const char* e = getenv("RX_VK_STARTUP_SCREEN");
    return !e || atoi(e) != 0;
}
/// The window's size now (0x0: minimized, or unknown)
void window_size(uint32_t* w, uint32_t* h) {
    *w = *h = 0;
#ifdef _WIN32
    RECT r{};
    if (main_window && GetClientRect(main_window, &r)) *w = (uint32_t)(r.right - r.left), *h = (uint32_t)(r.bottom - r.top);
#else
    int width = 0, height = 0;
    if (prep_window) prep_window->drawable_size(prep_window->window, &width, &height);
    if (width > 0 && height > 0) *w = (uint32_t)width, *h = (uint32_t)height;
#endif
}
void prepare_with_screen() {
    prep_ended = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    prep_running.store(true);
    std::thread worker([] {
        const bool update = prepare_is_update();   // (an update, RX_VK_PREPARE_BACKGROUND=1: what the notes name now, the rest in play)
        const bool prepared = !update && prepare_run(prep_progress, false);
        if (!update && !prepared && !prepare_stopped() && state_list_pending()) state_list_run();
        if (prepared && !prepare_stopped()) {   // (a first start only: the pipelines the notes predict, into the driver's cache)
            prep_done.store(0), prep_total.store(0);
            prep_stage.store(1);
            predict_run(prep_progress);
        }
        if (!prepare_stopped()) {
            prep_done.store(0), prep_total.store(0);
            prep_stage.store(2);
            precompile_run(prep_progress);
        }
        if (prepare_stopped()) pipeline_cache_save(true);   // (the driver's part built so far, for the next start)
        else if (update) prepare_background();
        prep_finished.store(true, std::memory_order_release);
        SetEvent(prep_ended);
    });
    // (the screen is drawn and presented on a thread of its own: a present can wait for as long as the window cannot be seen (FIFO
    // on Wayland, a locked screen), and that must hold neither the window's messages nor the game's start)
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    static std::atomic<bool> paint_stop{false};
    HANDLE painted = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    std::thread painter([t0, painted] {
        ProgressClock eta;
        int eta_stage = -1;
        double next_log = 10;
        bool prepared = false;   // (this start prepares the programs: a first start after an update, two steps)
        uint32_t last_w = dev.swap_extent.width, last_h = dev.swap_extent.height;
        for (;;) {
            const bool finished = prep_finished.load(std::memory_order_acquire);
            const double secs = std::chrono::duration<double>(clock::now() - t0).count();
            const int stage = prep_stage.load();
            const size_t done = prep_done.load(std::memory_order_relaxed), total = prep_total.load(std::memory_order_relaxed);
            if (stage != eta_stage) eta.reset(), eta_stage = stage;
            if (total) eta.note(secs, done, total);
            const double left = total ? eta.left() : -1;
            if (secs >= next_log && total) {
                logf("vk: start-up: %s %zu of %zu%s%s", stage == 2 ? "pipelines" : stage ? "predicted pipelines" : "shaders", done, total, left >= 0 ? ", " : "",
                     left_text(left).c_str());
                next_log = secs + 10;
            }
            if (stage == 0 && total) prepared = true;
            StartupState st;
            st.stage = stage, st.done = done, st.total = total, st.left = left, st.secs = secs;
            st.prepared = prepared, st.finished = finished, st.stopping = prepare_stopped();
            const StartupView view = startup_view(st);
            startup_screen(&view);
            uint32_t w, h;
            window_size(&w, &h);
            if (w && h && (w != last_w || h != last_h)) {   // (resized: the window's thread leaves it to the renderer, not started yet)
                dev.swap_resize.store((uint64_t)w << 32 | h, std::memory_order_release);
                last_w = w, last_h = h;
            }
            if (w && h) present_image(nullptr, dev.swap_extent.width, dev.swap_extent.height, nullptr);
            if (finished || paint_stop.load()) break;
            Sleep(33);
        }
        SetEvent(painted);
    });
    // (this thread: the window's messages on Windows, until the work is done)
    for (;;) {
#ifdef _WIN32
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
#endif
        if (WaitForSingleObject(prep_ended, 33) == WAIT_OBJECT_0) break;
    }
    worker.join();
    paint_stop.store(true);
    if (WaitForSingleObject(painted, 1000) == WAIT_OBJECT_0) painter.join();
    else {   // (its present waits for the window to be seen: the game's own presents will, too)
        logf("vk: start-up: the window is not being shown; the game starts without waiting for it");
        painter.detach();
    }
    startup_screen(nullptr);
    prep_running.store(false);
    logf("vk: start-up: the shaders took %.1f s", std::chrono::duration<double>(clock::now() - t0).count());
}
bool started_up() {
    if (const char* e = getenv("RX_VK_START_DELAY")) Sleep((DWORD)atoi(e) * 1000);   // (diagnosis: a long start, as the first one is)
    if (!replay_start()) return false;
    if (prepare_only()) {
        // (three parts: the programs, the pipelines the notes predict, then those they name; most of the time is the first)
        static int part;
        const auto progress = [](size_t done, size_t total) {
            static size_t last = ~(size_t)0;
            if (!total) return;
            static const size_t from[3] = {0, 700, 900}, span[3] = {700, 200, 100};
            // (the programs by what they cost, not by count: see prepare_work.h. Not smaller than the count's, so it never goes back)
            const double cost = part == 0 ? prepare_progress_by_cost() : -1.0;
            const size_t at = from[part] + (cost >= 0 ? (size_t)(cost * span[part]) : done * span[part] / total);
            if (last != ~(size_t)0 && at < last) return;
            if (at == last) return;
            last = at;
            printf("@@progress %.3f\n", at / 1000.0);
            fflush(stdout);
        };
        printf("@@stage shaders Preparing shaders\n");
        fflush(stdout);
        part = 0;
        const bool prepared = prepare_run(progress, false);
        if (!prepared && state_list_pending()) state_list_run();   // (the builder's step: a list shipped after the last prepare)
        part = 1;
        if (prepared) predict_run(progress);
        part = 2;
        precompile_run(progress);
        pipeline_cache_save(true);
        remove(skipped_path().c_str());
        return true;
    }
    if (FILE* f = fopen(skipped_path().c_str(), "rb")) {
        fclose(f);
        logf("vk: the shaders' preparation was skipped at the build: left to when each is first drawn");
        return true;
    }
    if (precompile_wanted()) {   // (a release build, before the game: nothing to build during play)
        if (!dev.offscreen && screen_wanted()) {
            prepare_with_screen();
            return true;
        }
        // (a console while it lasts: there is no window yet, and a long wait with nothing on screen looks like a hang)
        static HANDLE console = nullptr;
        const auto progress = [](size_t done, size_t total) {
            static bool tried = false;
            if (!total) return;
            if (!tried) {
                tried = true;
                if (AllocConsole()) {
#ifdef _WIN32
                    // This console is ours. Selection must not block preparation's progress writes.
                    const HANDLE input = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                     nullptr, OPEN_EXISTING, 0, nullptr);
                    if (input != INVALID_HANDLE_VALUE) {
                        DWORD mode;
                        if (GetConsoleMode(input, &mode))
                            SetConsoleMode(input, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
                        CloseHandle(input);
                    }
#endif
                    SetConsoleTitleA("HeyListen");
                    console = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                    if (console == INVALID_HANDLE_VALUE) console = nullptr;
                }
            }
            {   // (in the log too, every 10 s: a run with no window has nothing else to show it)
                static DWORD next_log = 0;
                const DWORD now = GetTickCount();
                if (!next_log) next_log = now + 10000;
                if ((int32_t)(now - next_log) >= 0) {
                    logf("vk: start-up: %zu of %zu", done, total);
                    next_log = now + 10000;
                }
            }
            if (!console) return;
            char line[80];
            const int n = snprintf(line, sizeof line, "%cPreparing shaders: %zu of %zu (%zu%%)   ", 13, done, total, done * 100 / total);
            DWORD wrote = 0;
            WriteFile(console, line, (DWORD)n, &wrote, nullptr);
        };
        const bool update = prepare_is_update();   // (an update: what the notes name now, the rest in play)
        if (!update && prepare_run(progress, false)) predict_run(progress);
        else if (!update && state_list_pending()) state_list_run();
        if (update && !prepare_is_lazy_update()) state_list_run();   // (a lazy update does it in play, first)
        precompile_run(progress, precompile_blocking());   // (TICK-008: queued and built behind the game unless waited for)
        if (update) prepare_background();
        if (console) CloseHandle(console), console = nullptr, FreeConsole();
    }
    return true;
}
// (the window's surface made: the presents' own queue kept where its family can present to it, create_device)
static void choose_present_queue() {
    if (!dev.present_queue) return;
    VkBool32 ok = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical, dev.present_family, dev.surface, &ok) != VK_SUCCESS || !ok) {
        logf("vk: queue family %u cannot present to the window: presents on the replay's queue", dev.present_family);
        dev.present_queue = VK_NULL_HANDLE, dev.present_family = dev.queue_family;
        return;
    }
    logf("vk: presents on a queue of their own, of compute family %u (RX_VK_PRESENT_QUEUE=0: off)", dev.present_family);
}

int start(void* hwnd) {
    main_window = (HWND)hwnd;
    if (!hwnd) {   // (HOS_HEADLESS: no window; the frames are rendered all the same, RX_VK_SHOTS writes them out)
        dev.offscreen = true;
        dev.swap_extent = {1920, 1080};
        dev.swap_format = VK_FORMAT_R8G8B8A8_UNORM;
        logf("vk: no window: presents render offscreen (%ux%u)", dev.swap_extent.width, dev.swap_extent.height);
        return started_up() ? 1 : 0;
    }
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = GetModuleHandleA(nullptr);
    si.hwnd = (HWND)hwnd;
    if (vkCreateWin32SurfaceKHR(dev.instance, &si, nullptr, &dev.surface) != VK_SUCCESS) return 0;
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical, dev.queue_family, dev.surface, &ok);
    if (!ok) return 0;
    choose_present_queue();
    RECT r{};
    GetClientRect((HWND)hwnd, &r);
    if (!create_swapchain((uint32_t)(r.right - r.left), (uint32_t)(r.bottom - r.top))) return 0;
#else
    const MkwVkWindow* w = (const MkwVkWindow*)hwnd;
    prep_window = w;
    if (w->version < 1 || w->version > MKWVK_WINDOW_VERSION) {   // (1: no refresh_mhz)
        logf("vk: unsupported SDL window bridge version %u (expected %u)", w->version, MKWVK_WINDOW_VERSION);
        return 0;
    }
    if (!w->create_surface(w->window, (void*)dev.instance, (void*)&dev.surface)) {
        logf("vk: SDL_Vulkan_CreateSurface failed; check SDL_VIDEODRIVER and the Vulkan window-system extensions");
        return 0;
    }
    VkBool32 ok = VK_FALSE;
    VkResult present = vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical, dev.queue_family, dev.surface, &ok);
    if (present != VK_SUCCESS || !ok) {
        logf("vk: queue-family present support failed (VkResult %d, supported %u)", (int)present, ok);
        return 0;
    }
    choose_present_queue();
    int width = 0, height = 0;
    w->drawable_size(w->window, &width, &height);
    if (width <= 0 || height <= 0) {
        logf("vk: SDL returned zero drawable size %dx%d", width, height);
        return 0;
    }
    if (!create_swapchain((uint32_t)width, (uint32_t)height)) {
        logf("vk: swapchain creation failed for drawable %dx%d", width, height);
        return 0;
    }
#endif
    if (!started_up()) {
        logf("vk: renderer startup failed after window and swapchain initialization");
        return 0;
    }
    return 1;
}

// (the window's thread: only noted. The swapchain is remade by whoever presents next, in present_image: made here, it went from under a
// frame between its acquire and its submit, and the frame's commands named images of the swapchain just destroyed: a lost device)
void resize(int width, int height) {
    if (dev.offscreen || width <= 0 || height <= 0) return;
    dev.swap_resize.store((uint64_t)(uint32_t)width << 32 | (uint32_t)height, std::memory_order_release);
}

const char* describe() {
    static std::string d;
    d = std::string(dev.props.deviceName) + " (Vulkan, native)";
    return d.c_str();
}

MkwVkRenderer table;

} // namespace
/// (heylisten) The shaders are being prepared (the start-up screen): the performance summary's "map" then (perf_report.cpp)
bool shaders_preparing() { return prep_running.load(std::memory_order_relaxed); }
} // namespace mkwvk

// (hookshot) The window is being closed (host/display.c, host/posix/display_sdl.c, before they end the process): while the
// shaders are prepared, their workers are stopped and what they made is kept first (10 s at most: a driver can take longer on one
// pipeline). Looked up by name, so an executable and a back end of different versions still close
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void mkwvk_close_asked(void) {
    using namespace mkwvk;
    perf_report_final(-1);   // (the performance summary's last: ExitProcess is _exit on Linux, so no atexit runs there)
    if (!prep_running.load() || prepare_stopped()) return;
    logf("vk: the window was closed while the shaders were prepared: stopping, what is done is kept");
    prepare_stop();
    if (WaitForSingleObject(prep_ended, 10000) != WAIT_OBJECT_0) logf("vk: the preparation did not stop within 10 s: closing anyway");
}

extern "C"
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const MkwVkRenderer* mkwvk_init(const MkwVkHost* h) {
    using namespace mkwvk;
    if (!h || h->version != MKWVK_API_VERSION) return nullptr;
    host = h;
    if (h->out_of_memory) std::set_new_handler(new_refused);   // (the renderer's C++ allocations at the commit limit: new_refused)
    if (!load_global()) {
        logf("vk: vulkan-1.dll not found");
        return nullptr;
    }
    if (!create_instance()) return nullptr;
    if (!pick_device()) {
        // (only a software renderer: a builder's --prepare-shaders run just fails (exit 3, as with no Vulkan at all, and it has no
        // one to show a window to); the game's run shows the window and ends, in the host: it must not fall back to WebGPU either)
#ifdef _WIN32
        constexpr bool windows = true;
#else
        constexpr bool windows = false;
#endif
        if (!software_only_name.empty() && !prepare_only() && h->no_usable_gpu)
            h->no_usable_gpu(gpu_choice::software_only_text(software_only_name.c_str(), windows).c_str());
        return nullptr;
    }
    if (!create_device()) return nullptr;
    logf("vk: %s, Vulkan %u.%u, driver %#x; mesh shaders %s, host memory import %s, pipeline libraries %s",
         dev.props.deviceName, VK_VERSION_MAJOR(dev.props.apiVersion), VK_VERSION_MINOR(dev.props.apiVersion),
         dev.props.driverVersion, dev.mesh_shader ? "yes" : "no", dev.external_memory_host ? "yes" : "no",
         dev.graphics_pipeline_library ? "yes" : "no");
    mkwvk::report_info::log_lines();   // (the crash report's facts: host/hos_crash_dialog.h reads "vk: driver:" and "report:" lines)
    if (!dev.mesh_shader && !dev.mesh_fallback && !getenv("RX_VK_NO_MESH_WARNING")) {
        // (the game draws its foliage, grass and many effects with mesh shaders: without them those draws are left out and the
        // picture is broken (an RX 580's "static"-like scenes, 2026-11-08). Said once, on its own thread, and the game goes on.)
        // (a GPU with no buffer device addresses has no fallback to run either)
        logf("vk: this GPU has no mesh shaders: the game's foliage, grass and many effects cannot be drawn");
#ifdef _WIN32
        std::thread([name = std::string(dev.props.deviceName)] {
            const std::string text = "Your graphics card (" + name + ") or its driver has no mesh shader support, which Ocarina of Time "
                "uses for foliage, grass and many effects, so parts of the picture will be missing or broken.\n\nHookshot needs a GPU "
                "with mesh shaders: NVIDIA GeForce RTX 20 series or newer, AMD Radeon RX 6000 series or newer, or Intel Arc. If your "
                "card is one of those, update its driver.";
            MessageBoxA(nullptr, text.c_str(), "HeyListen: graphics card", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        }).detach();
#endif
    }
    table.version = MKWVK_API_VERSION;
    table.start = start;
    table.resize = resize;
    table.describe = describe;
    fill_replay_table(table);
    return &table;
}

namespace mkwvk {
HWND app_window() { return main_window; }   // (hookshot: the Insert menu's Fullscreen row)

/// (hookshot) The refresh rate of the display the window is on, in mHz; 0 when not known (frame generation's Auto). Asked
/// again once a second at most: the window may move to another display, and Windows' answer costs a few system calls
int display_refresh_mhz() {
    static std::atomic<int> cached{0};
    static std::atomic<int64_t> asked_at{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t last = asked_at.load(std::memory_order_relaxed);
    if (last && now - last < 1000) return cached.load(std::memory_order_relaxed);
    asked_at.store(now, std::memory_order_relaxed);
    int mhz = 0;
#ifdef _WIN32
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (main_window && GetMonitorInfoW(MonitorFromWindow(main_window, MONITOR_DEFAULTTONEAREST), &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)   // (0 and 1: the default)
        // (whole Hz: a 59.94 Hz mode often says 59, and Auto would then make a frame too few a second. One under a
        // multiple of 6 (24, 30, 60, 120, 144, 240 Hz) is taken as that rate's /1.001)
        mhz = (dm.dmDisplayFrequency + 1) % 6 == 0 ? (int)((dm.dmDisplayFrequency + 1) * 1000000ull / 1001)
                                                    : (int)dm.dmDisplayFrequency * 1000;
#else
    const MkwVkWindow* w = (const MkwVkWindow*)main_window;
    if (w && w->version >= 2 && w->refresh_mhz) mhz = w->refresh_mhz(w->window);
#endif
    cached.store(mhz, std::memory_order_relaxed);
    return mhz;
}
} // namespace mkwvk

namespace mkwvk {
namespace {

#ifdef _WIN32
/// Whether the display the window is on lists a refresh range wide enough to be variable (FreeSync / G-Sync Compatible), from
/// its EDID in the registry (Enum\DISPLAY\<id>\<instance>\Device Parameters\EDID). -1: not found. Windows' own VRR flag has no
/// documented query outside DXGI's tearing support, which every modern GPU says yes to; the EDID range is the usable hint
int display_variable(int* lo, int* hi) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!main_window || !GetMonitorInfoW(MonitorFromWindow(main_window, MONITOR_DEFAULTTONEAREST), &mi)) return -1;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof dd;
    for (DWORD i = 0; EnumDisplayDevicesW(mi.szDevice, i, &dd, 0); i++) {
        std::wstring id = dd.DeviceID;   // (MONITOR\<hardware id>\{class guid}\<number>)
        const size_t a = id.find(L'\\'), b = a == std::wstring::npos ? a : id.find(L'\\', a + 1);
        if (b == std::wstring::npos) continue;
        const std::wstring hw = id.substr(a + 1, b - a - 1), drv = id.substr(b + 1);
        HKEY base = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\" + hw).c_str(), 0, KEY_READ, &base) !=
            ERROR_SUCCESS)
            continue;
        int result = -1;
        wchar_t inst[128];
        for (DWORD k = 0, n = 128; result < 0 && RegEnumKeyExW(base, k, inst, &n, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
             k++, n = 128) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(base, inst, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
            wchar_t driver[160];
            DWORD bytes = sizeof driver - sizeof(wchar_t), type = 0;
            if (RegQueryValueExW(key, L"Driver", nullptr, &type, (LPBYTE)driver, &bytes) == ERROR_SUCCESS && type == REG_SZ) {
                driver[bytes / sizeof(wchar_t)] = 0;
                HKEY par = nullptr;
                unsigned char edid[512];
                DWORD size = sizeof edid;
                if (!_wcsicmp(driver, drv.c_str()) && RegOpenKeyExW(key, L"Device Parameters", 0, KEY_READ, &par) == ERROR_SUCCESS) {
                    if (RegQueryValueExW(par, L"EDID", nullptr, nullptr, edid, &size) == ERROR_SUCCESS)
                        result = present_mode::edid_variable(edid, (int)size, lo, hi);
                    RegCloseKey(par);
                }
            }
            RegCloseKey(key);
        }
        RegCloseKey(base);
        if (result >= 0) return result;
    }
    return -1;
}
#endif

std::atomic<bool> vrr_touched{false};   // (a state other than the default was made: the Off path leaves everything alone until then)

} // namespace

present_mode::VrrState vrr_resolved(bool fresh) {
    using namespace present_mode;
    static std::mutex mu;
    static VrrState st;
    static int64_t at = 0;
    static std::atomic<int> logged_key{-1};
    if (opts.vrr == kVrrOff && !vrr_touched.load(std::memory_order_relaxed)) return VrrState{};
    std::lock_guard lk(mu);
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    // (cached: a row change shows within a quarter second, and the swapchain is made again then)
    if (!fresh && at && now - at < 250) return st;
    at = now;
    VrrInput in;
    in.row = opts.vrr;
    in.fullscreen = opts.fullscreen != 0;
    in.refresh_mhz = display_refresh_mhz();
    in.env_forced = getenv("RX_VK_FIFO") != nullptr || getenv("RX_VK_IMMEDIATE") != nullptr;
    int lo = 0, hi = 0;
    in.detected = -1;
#ifdef _WIN32
    static int detected = -2, dlo = 0, dhi = 0;
    static HMONITOR detected_for = nullptr;
    const HMONITOR mon = main_window ? MonitorFromWindow(main_window, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (in.row == kVrrAuto && (detected == -2 || detected_for != mon)) detected = display_variable(&dlo, &dhi), detected_for = mon;
    if (in.row == kVrrAuto) in.detected = detected, lo = dlo, hi = dhi;
#endif
    st = vrr_resolve(in);
    if (st.active || st.popup || vrr_touched.load()) vrr_touched = true;
#ifdef _WIN32
    static bool popup_applied = false;
    if (st.popup != popup_applied) {
        popup_applied = st.popup;
        // (host/display.c: the fullscreen window as a WS_POPUP that Windows can flip, or as it was; WM_APP + 10 on its thread)
        if (main_window) PostMessageA(main_window, WM_APP + 10, (WPARAM)(st.popup ? 1 : 0), 0);
    }
#endif
#ifdef _WIN32
    {   // (does the fullscreen window have what flip needs: WS_POPUP, no caption, the monitor's whole rectangle? Said when that
        // changes, and as "not" only when it stays so for a second evaluation: the window's thread takes the style a moment after)
        static int flip_said = -1, not_yet = 0;
        if (st.active && in.fullscreen && main_window) {
            const LONG style = GetWindowLongA(main_window, GWL_STYLE);
            RECT r{};
            MONITORINFO mi{};
            mi.cbSize = sizeof mi;
            const bool covers = GetWindowRect(main_window, &r) && GetMonitorInfoA(MonitorFromWindow(main_window, MONITOR_DEFAULTTONEAREST), &mi) &&
                                r.left == mi.rcMonitor.left && r.top == mi.rcMonitor.top && r.right == mi.rcMonitor.right && r.bottom == mi.rcMonitor.bottom;
            const int ok = (style & WS_POPUP) && !(style & WS_CAPTION) && covers ? 1 : 0;
            not_yet = ok ? 0 : not_yet + 1;
            if (ok != flip_said && (ok || not_yet >= 2)) {
                flip_said = ok;
                logf("vk: VRR: the fullscreen window %s", ok ? "is a borderless popup over the whole monitor: Windows can flip it" :
                     "is not a popup covering the monitor: Windows composes it and G-Sync / FreeSync may not follow");
            }
        } else {
            flip_said = -1, not_yet = 0;
        }
    }
#endif
    // (one line for each change: the row, the fullscreen row, the refresh, the choice)
    const int hz = (in.refresh_mhz + 500) / 1000;
    const int key = (in.row << 20) | (in.fullscreen << 19) | ((int)st.why << 15) | (st.active << 14) | (hz & 0x3fff);
    if (key != logged_key.load()) {
        logged_key = key;
        char why[160];
        switch (st.why) {
        case kWhyOff: snprintf(why, sizeof why, "off"); break;
        case kWhyForcedByEnv: snprintf(why, sizeof why, "RX_VK_FIFO / RX_VK_IMMEDIATE decide the present mode: VRR leaves the presents alone"); break;
        case kWhyAutoWindowed: snprintf(why, sizeof why, "Auto, windowed: off (fullscreen is needed to flip)"); break;
        case kWhyAutoNotFound: snprintf(why, sizeof why, "Auto: the display lists no variable refresh range in its EDID: off"); break;
        case kWhyAutoDetected: snprintf(why, sizeof why, "Auto: the display's EDID lists %d-%d Hz", lo, hi); break;
        default: snprintf(why, sizeof why, "On"); break;
        }
        if (st.active) {
            char cap[96];
            if (st.cap_below > 0) snprintf(cap, sizeof cap, "a frame rate limit above 60 held at %d fps (refresh %d Hz - %d)", hz - st.cap_below, hz, st.cap_below);
            else snprintf(cap, sizeof cap, "refresh rate unknown: no frame limit cap");
            logf("vk: VRR: %s; FIFO present mode (V-Sync row: %s), %s, %s; %s%s", why,
                 opts.vsync == kOn ? "On" : "Off, overridden",
                 in.fullscreen ? "borderless popup window (can flip)" : "windowed: the window cannot flip, G-Sync for windowed mode must be on in the driver", cap,
                 reflex_available() ? (opts.reflex ? "NVIDIA Reflex on" : "NVIDIA Reflex off (turn it on for the lowest delay)") : "no NVIDIA Reflex here",
                 framegen_on() ? "; frame generation presents are spaced to the same limit" : "");
        } else {
            logf("vk: VRR: %s", why);
        }
    }
    return st;
}

int present_choice_key() {
    const present_mode::VrrState s = vrr_resolved(false);
    // (the V-Sync row, the V-Sync mode row and the VRR row as one number: a change makes the swapchain again)
    return (opts.vsync * 8 + opts.vsync_mode) * 4 + (s.active ? 1 : 0) + (s.popup ? 2 : 0);
}

int vrr_fps_cap() {
    if (opts.vrr == present_mode::kVrrOff && !vrr_touched.load(std::memory_order_relaxed)) return 0;
    const present_mode::VrrState s = vrr_resolved(false);
    const int mhz = display_refresh_mhz();
    if (!s.active || s.cap_below <= 0 || mhz <= 0) return 0;
    const int cap = (mhz + 500) / 1000 - s.cap_below;
    return cap >= 60 ? cap : 0;
}

int64_t vrr_min_gap_ns() {
    if (opts.vrr == present_mode::kVrrOff && !vrr_touched.load(std::memory_order_relaxed)) return 0;
    const present_mode::VrrState s = vrr_resolved(false);
    const int mhz = display_refresh_mhz();
    if (!s.active || s.cap_below <= 0 || mhz <= 3000 * s.cap_below) return 0;
    return (int64_t)(1e12 / (double)(mhz - 1000 * s.cap_below));
}

} // namespace mkwvk
