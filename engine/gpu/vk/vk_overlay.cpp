// The performance overlay (F10, vk_menu.cpp), drawn with Dear ImGui over the window's image at the last step of a present.
// Modes: FPS (the rate, the frame time and the worst frame in a small box, top right) and Full (a panel with the machine's GPU,
// VRAM, CPU and RAM load and temperatures, the frame rate and frame times with a graph, the waits and the picture lines).
// Taken from the oot-pc port's runtime/ui/menu.cpp (the performance overlay, MIT, Copyright (c) 2026 Jeff Aigner and the
// totk-pc-recomp contributors, in the style of SavageWindRecomp's perf_overlay_draw, MIT, Copyright (c) 2026 Revivedx), with
// the catalog's English texts in place and the numbers from vk_perf.cpp. ImGui runs on the presentation thread only.
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "vk_internal.h"   // (first: it sets the Win32 platform before any Vulkan header)
#include "vk_menu.h"
#include "vk_perf.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_vulkan.h"

namespace mkwvk {

HWND app_window();   // (vk_main.cpp)

namespace {

#include "imgui/roboto_ttf.inc"

bool ctx_ok = false, vk_ok = false;
VkFormat vk_fmt = VK_FORMAT_UNDEFINED;
float font_scale = 0;
ImGuiStyle base_style;

// The sizes are tuned for 1080 lines and follow the window's height. Under 720 lines at 100% display scale they stay at the
// 720 size; that floor grows with the window's DPI, so a small window on a 150% or 200% display still gets text as large as
// the display scale asks for.
unsigned window_dpi() {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn get = (Fn)(void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
    HWND w = app_window();
    return get && w ? get(w) : 96;
}
float overlay_scale(uint32_t h) {
    const float least = 720.0f * (float)window_dpi() / 96.0f;
    return ((float)h >= least ? (float)h : least) / 1080.0f;
}

// SavageWindRecomp's colours: dark green panels, gold accents.
void style_init(ImGuiStyle& s) {
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.WindowPadding = ImVec2(28, 22); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 18;
    ImVec4* c = s.Colors;
    ImVec4 gold(0.93f, 0.76f, 0.33f, 1), green(0.15f, 0.36f, 0.25f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.09f, 0.08f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.94f, 0.88f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.64f, 0.58f, 1);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.18f, 1);
    c[ImGuiCol_ButtonHovered] = green;
    c[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.18f, 0.15f, 1);
    c[ImGuiCol_TabHovered] = green;
    c[ImGuiCol_TabSelected] = ImVec4(0.21f, 0.44f, 0.31f, 1);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.36f, 0.30f, 1);
}

void make_context() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendPlatformName = "hookshot_overlay";
    style_init(base_style);
    ImGui::GetStyle() = base_style;
    ctx_ok = true;
}

void vk_down() {
    if (!vk_ok) return;
    ImGui_ImplVulkan_Shutdown();
    vk_ok = false;
    font_scale = 0;
}

PFN_vkVoidFunction load_fn(const char* name, void*) { return instance_proc()(dev.instance, name); }

bool vk_up(VkFormat fmt) {
    if (vk_ok && vk_fmt == fmt) return true;
    if (vk_ok) {   // the swapchain changed format: pipelines for the new one
        std::lock_guard qk(dev.present_mu);
        vkQueueWaitIdle(dev.queue);
        vk_down();
    }
    static bool loaded = false;
    if (!loaded) loaded = ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, load_fn, nullptr);
    if (!loaded) return false;
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = dev.instance;
    ii.PhysicalDevice = dev.physical;
    ii.Device = dev.device;
    ii.QueueFamily = dev.queue_family;
    ii.Queue = dev.queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 4;   // vertex buffers rotate per recorded frame: more than the three frames in flight
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    vk_fmt = fmt;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &vk_fmt;
    if (!ImGui_ImplVulkan_Init(&ii)) return false;
    vk_ok = true;
    return true;
}

// Roboto Medium (Latin, Cyrillic, dashes and quotes) at 26 px per 1080 lines. Made again when the window height changes; the
// old texture goes after the queue is idle (CreateFontsTexture).
const ImWchar kRanges[] = {0x0020, 0x00FF, 0x0400, 0x052F, 0x2010, 0x2027, 0};

bool fonts_up(float scale) {
    ImFontAtlas* fa = ImGui::GetIO().Fonts;
    fa->Clear();
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    fa->AddFontFromMemoryTTF((void*)roboto_ttf, (int)sizeof roboto_ttf, 26.0f * scale, &fc, kRanges);
    bool ok;
    {
        std::lock_guard qk(dev.present_mu);   // (the upload submits on the queue and waits for it)
        ok = ImGui_ImplVulkan_CreateFontsTexture();
    }
    fa->ClearInputData();   // the glyphs are in the texture: the font file and the pixels are not needed again
    fa->ClearTexData();
    logf("overlay: font atlas at %.1f px", 26.0f * scale);
    font_scale = ok ? scale : 0;
    return ok;
}

// ---- what the picture lines show: set by the present path
std::atomic<int> image_w{0}, image_h{0};

// ---- the FPS box: the rate over the presents of the last half second, the frame time and the worst frame of the window
uint64_t fps_last_ns, fps_win_ns, fps_max_ns;
int fps_frames;
char fps_text[96];

void fps_tick(uint64_t now) {
    const uint64_t d = now - fps_last_ns;
    if (!fps_last_ns || d > 2000000000ull) {   // first frame, or a stall the window should not average in
        fps_win_ns = fps_max_ns = 0;
        fps_frames = 0;
    } else {
        fps_win_ns += d;
        fps_frames++;
        if (d > fps_max_ns) fps_max_ns = d;
        if (fps_win_ns >= 500000000ull) {
            snprintf(fps_text, sizeof fps_text, "%.0f fps  %.1f ms  max %.1f ms", fps_frames * 1e9 / fps_win_ns,
                     fps_win_ns / 1e6 / fps_frames, fps_max_ns / 1e6);
            fps_win_ns = fps_max_ns = 0;
            fps_frames = 0;
        }
    }
    fps_last_ns = now;
}

// The panel keeps its width while the numbers change: every column is as wide as the widest text it can show, measured on
// samples of 8s (Roboto's digits share one width), and the numbers are right-aligned in it.
struct PerfCol { const char* fmt; float sample; };

const char* kNone = "-";

float perf_text_w(const char* fmt, float v) {
    char b[64];
    snprintf(b, sizeof b, fmt, v);
    return ImGui::CalcTextSize(b).x;
}

// A fixed column as wide as the widest of the formats (with their samples) and labels given.
void perf_column(const char* id, const PerfCol* cols, int n, const char* const* labels = nullptr, int nl = 0) {
    float w = ImGui::CalcTextSize(kNone).x;
    for (int i = 0; i < n; i++) w = fmaxf(w, perf_text_w(cols[i].fmt, cols[i].sample));
    for (int i = 0; i < nl; i++) w = fmaxf(w, ImGui::CalcTextSize(labels[i]).x);
    ImGui::TableSetupColumn(id, ImGuiTableColumnFlags_WidthFixed, w);
}

void perf_right(const char* s, bool dim) {
    const float pad = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(s).x;
    if (pad > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    if (dim) ImGui::TextDisabled("%s", s);
    else ImGui::TextUnformatted(s);
}

// One cell of the full panel's table: fmt (one number) with v, or "-" when the machine gives none.
void perf_cell(const char* fmt, float v) {
    ImGui::TableNextColumn();
    if (v < 0) return perf_right(kNone, true);
    char b[64];
    snprintf(b, sizeof b, fmt, v);
    perf_right(b, false);
}

void perf_label(const char* label, const ImVec4& c) {
    ImGui::TableNextColumn();
    ImGui::TextColored(c, "%s", label);
}

float perf_gb(float mb) { return mb < 0 ? -1 : mb / 1024; }

const char* kFmtPct = "%.0f%%";
const char* kFmtTemp = "%.0f \xC2\xB0""C";
const char* kFmtPower = "%.0f W";
const char* kFmtMhz = "%.0f MHz";
const char* kFmtGb = "%.1f GB";
const char* kFmtGbOf = "/ %.1f GB";
const char* kFmtGamePct = "game %.0f%%";
const char* kFmtGameGb = "game %.1f GB";
const char* kFmtFps = "%.0f FPS";
const char* kFmtFrameMs = "%.1f ms";
const char* kFmtMaxMs = "max %.1f ms";
const char* kFmtMs = "%.2f ms";
const char* kFmtPerFrame = "%.1f per frame";

// The picture lines: the window, the game image the present shows, how the present scales it, the frame limit and the
// presentation mode in use.
void perf_picture(uint32_t w, uint32_t h, const ImVec4& c) {
    char v[6][64];
    snprintf(v[0], sizeof v[0], "%ux%u", w, h);
    if (image_w.load() > 0) snprintf(v[1], sizeof v[1], "%ux%u", (unsigned)image_w.load(), (unsigned)image_h.load());
    else snprintf(v[1], sizeof v[1], "%s", kNone);
    snprintf(v[2], sizeof v[2], "%s", kNone);   // (the 3D scene's size: the game's dynamic resolution, not known here)
    static const char* const scaling[] = {"bilinear", "Catmull-Rom", "Lanczos"};
    snprintf(v[3], sizeof v[3], "%s", scaling[opts.upscale < 0 || opts.upscale > 2 ? 0 : opts.upscale]);
    snprintf(v[4], sizeof v[4], "%s", opts.framegen >= 2 ? "frame generation" : kNone);
    snprintf(v[5], sizeof v[5], "%s", dev.present_mode_name ? dev.present_mode_name : kNone);
    static const char* const labels[6] = {"Window", "Game image", "3D scene", "Scaling", "Frame limit", "Presentation"};
    if (!ImGui::BeginTable("##perfp", 2, ImGuiTableFlags_SizingFixedFit)) return;
    float vw = 0;
    for (auto& s : v) vw = fmaxf(vw, ImGui::CalcTextSize(s).x);
    ImGui::TableSetupColumn("##p0", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##p1", ImGuiTableColumnFlags_WidthFixed, vw);
    for (int i = 0; i < 6; i++) {
        perf_label(labels[i], c);
        ImGui::TableNextColumn();
        perf_right(v[i], false);
    }
    ImGui::EndTable();
}

// the waits of the render thread, per frame, averaged over the last half second; -1 reads "-" (the counters are not kept here)
PerfWaits waits_now() {
    PerfWaits w;
    perf_waits(&w);
    return w;
}

void perf_full_draw(uint32_t width, uint32_t height, float scale, bool menu_open) {
    const float w = (float)width;
    PerfStats s;
    perf_snapshot(&s);
    ImGui::SetNextWindowPos(ImVec2(w - 12 * scale, 12 * scale), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##perf", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    if (menu_open) ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
    else ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::SetWindowFontScale(0.72f);
    const ImVec4 gpu_c(0.55f, 0.83f, 0.31f, 1), cpu_c(0.18f, 0.59f, 0.80f, 1), ram_c(0.80f, 0.45f, 0.70f, 1),
        fps_c(0.95f, 0.95f, 0.95f, 1), wait_c(0.93f, 0.76f, 0.33f, 1), pic_c(0.62f, 0.70f, 0.95f, 1);
    if (ImGui::BeginTable("##perft", 4, ImGuiTableFlags_SizingFixedFit)) {
        static const char* const labels[] = {"GPU", "VRAM", "CPU", "RAM"};
        static const PerfCol c0[] = {{kFmtFps, 888}};
        static const PerfCol c1[] = {{kFmtPct, 100}, {kFmtGb, 888.8f}, {kFmtFrameMs, 888.8f}};
        static const PerfCol c2[] = {{kFmtTemp, 188}, {kFmtGbOf, 888.8f}, {kFmtMaxMs, 8888.8f}};
        static const PerfCol c3[] = {{kFmtPower, 888}, {kFmtMhz, 8888}, {kFmtGamePct, 100}, {kFmtGameGb, 888.8f}};
        perf_column("##c0", c0, 1, labels, 4);
        perf_column("##c1", c1, 3);
        perf_column("##c2", c2, 3);
        perf_column("##c3", c3, 4);
        perf_label("GPU", gpu_c);
        perf_cell(kFmtPct, s.gpu_pct);
        perf_cell(kFmtTemp, s.gpu_temp_c);
        perf_cell(kFmtPower, s.gpu_power_w);
        perf_label("VRAM", gpu_c);
        perf_cell(kFmtGb, perf_gb(s.vram_used_mb));
        perf_cell(kFmtGbOf, perf_gb(s.vram_total_mb));
        perf_cell(kFmtMhz, s.gpu_clock_mhz);
        perf_label("CPU", cpu_c);
        perf_cell(kFmtPct, s.cpu_total_pct);
        perf_cell(kFmtTemp, s.cpu_temp_c);
        perf_cell(kFmtGamePct, s.cpu_game_pct);
        perf_label("RAM", ram_c);
        perf_cell(kFmtGb, perf_gb(s.ram_used_mb));
        perf_cell(kFmtGbOf, perf_gb(s.ram_total_mb));
        perf_cell(kFmtGameGb, perf_gb(s.ram_game_mb));
        ImGui::TableNextColumn();   // the frame row: no label, the rate in its place
        char b[64];
        snprintf(b, sizeof b, kFmtFps, s.fps);
        ImGui::TextColored(fps_c, "%s", b);
        perf_cell(kFmtFrameMs, s.frame_ms_avg);
        perf_cell(kFmtMaxMs, s.frame_ms_max);
        ImGui::EndTable();
    }
    const float top = s.frame_ms_max > 50 ? s.frame_ms_max : 50;
    // as wide as the panel the tables size; the plot does not count toward that width, so the panel can still shrink
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const float max_x = win->DC.CursorMaxPos.x;
    ImGui::PlotLines("##ft", s.frame_ms, kPerfHistory, 0, nullptr, 0, top, ImVec2(-FLT_MIN, 48 * scale));
    win->DC.CursorMaxPos.x = max_x;
    // per frame, averaged over the last half second; the labels' column stretches, so the numbers end at the panel's right
    // edge, and the table still asks for no more than its widest label and number
    if (ImGui::BeginTable("##perfw", 2, ImGuiTableFlags_SizingFixedFit)) {
        static const PerfCol c1[] = {{kFmtMs, 888.88f}, {kFmtPerFrame, 888.8f}};
        const PerfWaits wt = waits_now();
        ImGui::TableSetupColumn("##w0", ImGuiTableColumnFlags_WidthStretch);
        perf_column("##w1", c1, 2);
        perf_label("GPU frame prep", wait_c);
        perf_cell(kFmtMs, wt.replay_ms);
        perf_label("GPU busy per frame", wait_c);
        perf_cell(kFmtMs, wt.gpu_ms);
        perf_label("Pipeline waits", wait_c);
        perf_cell(kFmtMs, wt.pipe_wait_ms);
        perf_label("Shader waits", wait_c);
        perf_cell(kFmtMs, wt.shader_wait_ms);
        perf_label("Texture uploads", wait_c);
        perf_cell(kFmtPerFrame, wt.tex_uploads);
        ImGui::EndTable();
    }
    perf_picture(width, height, pic_c);
    if (s.gpu_name[0]) ImGui::TextDisabled("%s", s.gpu_name);
    if (s.cpu_name[0]) ImGui::TextDisabled("%s", s.cpu_name);
    ImGui::End();
}

void perf_fps_draw(uint32_t width, float scale, bool menu_open) {
    if (!fps_text[0]) return;
    // with the menu open: under its window, else over everything
    ImDrawList* dl = menu_open ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
    const ImVec2 sz = ImGui::CalcTextSize(fps_text);
    const float pad = 6 * scale;
    const ImVec2 p((float)width - 12 * scale - sz.x - pad * 2, 12 * scale);   // the top right corner: the game's HUD leaves it empty
    dl->AddRectFilled(p, ImVec2(p.x + sz.x + pad * 2, p.y + sz.y + pad * 2), IM_COL32(0, 0, 0, 150), 4 * scale);
    dl->AddText(ImVec2(p.x + pad, p.y + pad), IM_COL32(255, 255, 255, 235), fps_text);
}

// ---- one view of each swapchain image, made when first drawn on
struct View { VkImage image; VkImageView view; };
std::vector<View> views;

VkImageView view_of(VkImage image, VkFormat fmt) {
    for (const View& v : views)
        if (v.image == image) return v.view;
    // (views of images the swapchain no longer has go first; the swapchain's change waited for the device)
    for (size_t i = 0; i < views.size();) {
        bool live = false;
        for (VkImage im : dev.swap_images) live |= im == views[i].image;
        if (live) { i++; continue; }
        vkDestroyImageView(dev.device, views[i].view, nullptr);
        views.erase(views.begin() + (long)i);
    }
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = fmt;
    ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &ci, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    views.push_back({image, v});
    return v;
}

void layout_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                    VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage, b.srcAccessMask = src_access, b.dstStageMask = dst_stage, b.dstAccessMask = dst_access;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

} // namespace

void overlay_note_image(int w, int h) { image_w = w, image_h = h; }

void overlay_record(VkCommandBuffer cmd, VkImage dst, uint32_t w, uint32_t h, int mode, bool menu_open) {
    if (mode < 1 || !w || !h) return;
    if (!ctx_ok) make_context();
    if (!vk_up(dev.swap_format)) return;
    const float scale = overlay_scale(h);
    if (std::fabs(scale - font_scale) > 1e-4f && !fonts_up(scale)) return;
    // the style follows the window's size (the sizes above are for 1080 lines)
    ImGui::GetStyle() = base_style;
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    static uint64_t last = 0;
    const uint64_t now = (uint64_t)prof_now();
    io.DeltaTime = last ? std::max(1e-4f, (float)((now - last) / 1e9)) : 1.0f / 60;
    last = now;
    if (mode == 1) fps_tick(now);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (mode == 2) perf_full_draw(w, h, scale, menu_open);
    else perf_fps_draw(w, scale, menu_open);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || !dd->TotalVtxCount) return;
    const VkImageView view = view_of(dst, dev.swap_format);
    if (!view) return;
    layout_barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    ImGui_ImplVulkan_RenderDrawData(dd, cmd);
    vkCmdEndRendering(cmd);
    layout_barrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

void overlay_release() {
    if (!ctx_ok) return;
    vkDeviceWaitIdle(dev.device);
    for (const View& v : views) vkDestroyImageView(dev.device, v.view, nullptr);
    views.clear();
    vk_down();
    ImGui::DestroyContext();
    ctx_ok = false;
}

} // namespace mkwvk
