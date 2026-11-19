/* (hookshot) An AV1 decoder on Media Foundation, for nn::codec's AV1 decoder (host/nn_video.c).

   OoT's movies (romfs/Movie: the intro, ...) are AV1, and the static FFmpeg the build links has no AV1 decoder (its
   native one needs a hardware context; dav1d / libaom are not in it). Windows has one: the decoder MFT of the AV1
   Video Extension (Microsoft.AV1VideoExtension), in hardware on GPUs that decode AV1 and in software otherwise.
   The game submits AV1 temporal units (low-overhead OBUs, as in MP4/IVF); pictures come out as NV12.

   The API is synchronous and single-stream: mf_av1_decode takes one temporal unit (or NULL at the end, which
   drains) and calls back once per picture. A hardware MFT is asynchronous; one is used only if no synchronous
   decoder exists (HOS_AV1_ASYNC=1 to prefer it: untested). HOS_VIDEO_LOG=1 logs the decoder chosen. */
#define COBJMACROS
#include "hos.h"
#include <windows.h>
#include <shellapi.h>
#include <initguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>

#ifndef MFVideoFormat_AV1
DEFINE_GUID(MFVideoFormat_AV1_local, 0x31305641, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
#define MFVideoFormat_AV1 MFVideoFormat_AV1_local
#endif

#ifndef MF_LOW_LATENCY
DEFINE_GUID(MF_LOW_LATENCY_local, 0x9c27891a, 0xed7a, 0x40e1, 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee);
#define MF_LOW_LATENCY MF_LOW_LATENCY_local
#endif
/* the AV1 profile 0 decoder profile of DXVA (D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0) */
DEFINE_GUID(HL_DECODER_AV1_PROFILE0, 0xb8be4ccb, 0xcf53, 0x46ba, 0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a);

/* a picture: luma rows and the interleaved chroma rows, both `stride` bytes apart */
typedef void (*mf_av1_picture_fn)(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts);

typedef struct {
    IMFTransform *mft;
    int w, h, stride;
    int want_w, want_h;   /* (the stream's size: the decoder's frames may be padded, 1088 rows for 1080) */
    int provides_samples;
    DWORD out_size;
    /* (HeyListen) hardware decoding: the AV1 Video Extension's decoder runs on the GPU (DXVA) once it is given a D3D11 device;
       its pictures are then textures, read back to system memory through a staging texture */
    ID3D11Device *dev;
    ID3D11DeviceContext *dctx;
    IMFDXGIDeviceManager *mgr;
    ID3D11Texture2D *staging;
    UINT stg_w, stg_h;
    int hw;                       /* the decoder was given the device */
    int hw_pictures;              /* pictures read back from a texture (the first one is logged) */
} MfAv1;

static int com_ready(void) {
    static volatile LONG started;
    const HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* (per thread; S_FALSE / RPC_E_CHANGED_MODE: fine) */
    (void)co;
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) { started = 2; return 0; }
    }
    return started == 1;
}

static int set_output_nv12(MfAv1 *m) {
    for (DWORD i = 0;; i++) {
        IMFMediaType *t = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(m->mft, 0, i, &t))) return 0;
        GUID sub;
        int ok = SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub)) && IsEqualGUID(&sub, &MFVideoFormat_NV12) &&
                 SUCCEEDED(IMFTransform_SetOutputType(m->mft, 0, t, 0));
        if (ok) {
            UINT64 fs = 0;
            if (SUCCEEDED(IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &fs))) m->w = (int)(fs >> 32), m->h = (int)(UINT32)fs;
            UINT32 st = 0;
            m->stride = SUCCEEDED(IMFMediaType_GetUINT32(t, &MF_MT_DEFAULT_STRIDE, &st)) ? (int)st : m->w;
        }
        IMFMediaType_Release(t);
        if (ok) break;
    }
    MFT_OUTPUT_STREAM_INFO info = {0};
    if (SUCCEEDED(IMFTransform_GetOutputStreamInfo(m->mft, 0, &info))) {
        m->provides_samples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        m->out_size = info.cbSize;
    }
    return 1;
}

/* (hookshot) Without the AV1 Video Extension the movies have no decoder: their pictures stay empty, which shows as a green screen, and the
   intro never ends. Said once, on its own thread (the game goes on), with the way to the Store page. HOS_AV1_MISSING=1: as if it were missing */
static DWORD WINAPI av1_missing_notice(LPVOID unused) {
    (void)unused;
    if (MessageBoxA(NULL, "HeyListen's movies need Microsoft's free AV1 Video Extension, which this PC does not have. Without it they show a green "
                          "screen and the game can stop at a black screen.\n\nOpen its Microsoft Store page now? Install it, then restart the game.",
                    "HeyListen", MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST) == IDYES)
        ShellExecuteA(NULL, "open", "ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V", NULL, NULL, SW_SHOWNORMAL);
    return 0;
}

static void av1_missing(void) {
    static LONG told;
    if (InterlockedExchange(&told, 1)) return;
    hos_log("video: no AV1 decoder in Media Foundation: the AV1 Video Extension is not installed, movies will not show");
    CreateThread(NULL, 0, av1_missing_notice, NULL, 0, NULL);
}

/* (HeyListen) A D3D11 device on the first adapter whose video decoder does AV1 (DXVA profile 0), for the decoder to run on. 0 when
   there is none (the decoder then decodes in software, as it always did). HOS_AV1_NO_HW=1: none, on purpose. */
static int hw_device(MfAv1 *m) {
    typedef HRESULT(WINAPI * create_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                         ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
    typedef HRESULT(WINAPI * factory_fn)(REFIID, void **);
    typedef HRESULT(WINAPI * manager_fn)(UINT *, IMFDXGIDeviceManager **);
    if (getenv("HOS_AV1_NO_HW")) return 0;
    HMODULE d3d = LoadLibraryA("d3d11.dll"), dxgi = LoadLibraryA("dxgi.dll"), mfp = GetModuleHandleA("mfplat.dll");
    if (!d3d || !dxgi || !mfp) return 0;
    const create_fn create = (create_fn)(void *)GetProcAddress(d3d, "D3D11CreateDevice");
    const factory_fn make_factory = (factory_fn)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
    const manager_fn make_manager = (manager_fn)(void *)GetProcAddress(mfp, "MFCreateDXGIDeviceManager");
    if (!create || !make_factory || !make_manager) return 0;
    IDXGIFactory1 *fac = NULL;
    if (FAILED(make_factory(&IID_IDXGIFactory1, (void **)&fac))) return 0;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    for (UINT i = 0; !m->dev; i++) {
        IDXGIAdapter1 *ad = NULL;
        if (IDXGIFactory1_EnumAdapters1(fac, i, &ad) != S_OK) break;
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter1_GetDesc1(ad, &desc);
        ID3D11Device *dev = NULL;
        ID3D11DeviceContext *dc = NULL;
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(create((IDXGIAdapter *)ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2,
                             D3D11_SDK_VERSION, &dev, NULL, &dc))) {
            int av1 = 0;
            ID3D11VideoDevice *vd = NULL;
            if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11VideoDevice, (void **)&vd))) {
                const UINT n = ID3D11VideoDevice_GetVideoDecoderProfileCount(vd);
                for (UINT k = 0; k < n && !av1; k++) {
                    GUID g;
                    if (SUCCEEDED(ID3D11VideoDevice_GetVideoDecoderProfile(vd, k, &g)) && IsEqualGUID(&g, &HL_DECODER_AV1_PROFILE0)) av1 = 1;
                }
                ID3D11VideoDevice_Release(vd);
            }
            if (av1) {
                ID3D10Multithread *mt = NULL;
                if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D10Multithread, (void **)&mt))) {
                    ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
                    ID3D10Multithread_Release(mt);
                }
                m->dev = dev, m->dctx = dc;
                hos_log("video: AV1 decoding on the GPU: %ls", desc.Description);
            } else {
                ID3D11DeviceContext_Release(dc);
                ID3D11Device_Release(dev);
            }
        }
        IDXGIAdapter1_Release(ad);
    }
    IDXGIFactory1_Release(fac);
    if (!m->dev) { hos_log("video: no GPU with an AV1 decoder: decoding in software"); return 0; }
    UINT token = 0;
    if (FAILED(make_manager(&token, &m->mgr)) || FAILED(IMFDXGIDeviceManager_ResetDevice(m->mgr, (IUnknown *)m->dev, token))) return 0;
    return 1;
}
static void hw_release(MfAv1 *m) {
    if (m->staging) ID3D11Texture2D_Release(m->staging), m->staging = NULL;
    if (m->mgr) IMFDXGIDeviceManager_Release(m->mgr), m->mgr = NULL;
    if (m->dctx) ID3D11DeviceContext_Release(m->dctx), m->dctx = NULL;
    if (m->dev) ID3D11Device_Release(m->dev), m->dev = NULL;
    m->hw = 0;
}

void *mf_av1_open(int w, int h) {
    if (!com_ready()) return NULL;
    if (getenv("HOS_AV1_MISSING")) { av1_missing(); return NULL; }
    MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, MFVideoFormat_AV1};
    IMFActivate **acts = NULL;
    UINT32 n = 0;
    const int prefer_async = getenv("HOS_AV1_ASYNC") != NULL;
    const UINT32 tries[2] = {prefer_async ? MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT : MFT_ENUM_FLAG_SYNCMFT,
                             prefer_async ? MFT_ENUM_FLAG_SYNCMFT : MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT};
    IMFTransform *mft = NULL;
    for (int k = 0; k < 2 && !mft; k++) {
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, tries[k] | MFT_ENUM_FLAG_SORTANDFILTER, &in, NULL, &acts, &n)))
            continue;
        for (UINT32 i = 0; i < n && !mft; i++) {
            if (SUCCEEDED(IMFActivate_ActivateObject(acts[i], &IID_IMFTransform, (void **)&mft))) {
                WCHAR name[128] = L"?";
                IMFActivate_GetString(acts[i], &MFT_FRIENDLY_NAME_Attribute, name, 128, NULL);
                hos_log("video: AV1 on Media Foundation: %ls (%s)", name, k == 0 && !prefer_async ? "synchronous" : "asynchronous");
                if (tries[k] & MFT_ENUM_FLAG_ASYNCMFT) {   /* (an async MFT must be unlocked to be used) */
                    IMFAttributes *a = NULL;
                    if (SUCCEEDED(IMFTransform_GetAttributes(mft, &a)) && a) {
                        IMFAttributes_SetUINT32(a, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                        IMFAttributes_Release(a);
                    }
                }
            }
        }
        for (UINT32 i = 0; i < n; i++) IMFActivate_Release(acts[i]);
        CoTaskMemFree(acts);
        acts = NULL, n = 0;
    }
    if (!mft) { av1_missing(); return NULL; }
    IMFMediaType *t = NULL;
    MFCreateMediaType(&t);
    IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_AV1);
    if (w > 0 && h > 0) IMFMediaType_SetUINT64(t, &MF_MT_FRAME_SIZE, ((UINT64)(UINT32)w << 32) | (UINT32)h);
    const HRESULT hr = IMFTransform_SetInputType(mft, 0, t, 0);
    IMFMediaType_Release(t);
    if (FAILED(hr)) { hos_log("video: the AV1 decoder refused its input type (%#lx)", (unsigned long)hr); IMFTransform_Release(mft); return NULL; }
    MfAv1 *m = (MfAv1 *)calloc(1, sizeof *m);
    m->mft = mft;
    m->w = w, m->h = h, m->stride = w;
    m->want_w = w, m->want_h = h;
    {   /* (HeyListen) pictures out as they are decoded, not after some more input; and on the GPU where it decodes AV1 */
        IMFAttributes *a = NULL;
        if (SUCCEEDED(IMFTransform_GetAttributes(mft, &a)) && a) {
            IMFAttributes_SetUINT32(a, &MF_LOW_LATENCY, TRUE);
            IMFAttributes_Release(a);
        }
        if (hw_device(m)) {
            if (SUCCEEDED(IMFTransform_ProcessMessage(mft, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)m->mgr))) m->hw = 1;
            else { hos_log("video: the AV1 decoder did not take the GPU: decoding in software"); hw_release(m); }
        }
    }
    set_output_nv12(m);
    IMFTransform_ProcessMessage(mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return m;
}

/* every picture the decoder has ready */
static void pull(MfAv1 *m, mf_av1_picture_fn fn, void *user) {
    for (int guard = 0; guard < 64; guard++) {
        MFT_OUTPUT_DATA_BUFFER ob = {0};
        IMFSample *own = NULL;
        if (!m->provides_samples) {
            IMFMediaBuffer *b = NULL;
            const DWORD size = m->out_size ? m->out_size : (DWORD)((size_t)(m->stride > 0 ? m->stride : m->w) * m->h * 3 / 2);
            if (FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer(size, &b))) { if (own) IMFSample_Release(own); return; }
            IMFSample_AddBuffer(own, b);
            IMFMediaBuffer_Release(b);
            ob.pSample = own;
        }
        DWORD status = 0;
        const HRESULT hr = IMFTransform_ProcessOutput(m->mft, 0, 1, &ob, &status);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (own) IMFSample_Release(own);
            if (ob.pEvents) IMFCollection_Release(ob.pEvents);
            set_output_nv12(m);
            continue;
        }
        if (FAILED(hr)) {   /* MF_E_TRANSFORM_NEED_MORE_INPUT: all out */
            if (own) IMFSample_Release(own);
            if (ob.pEvents) IMFCollection_Release(ob.pEvents);
            return;
        }
        IMFSample *s = ob.pSample;
        LONGLONG t100 = 0;
        IMFSample_GetSampleTime(s, &t100);
        IMFMediaBuffer *b = NULL;
        IMFDXGIBuffer *db = NULL;
        if (m->hw && SUCCEEDED(IMFSample_GetBufferByIndex(s, 0, &b)) &&
            SUCCEEDED(IMFMediaBuffer_QueryInterface(b, &IID_IMFDXGIBuffer, (void **)&db))) {
            /* (HeyListen) a texture the GPU decoded into: copied to a staging texture and read from there */
            ID3D11Texture2D *tex = NULL;
            UINT sub = 0;
            if (SUCCEEDED(IMFDXGIBuffer_GetResource(db, &IID_ID3D11Texture2D, (void **)&tex)) &&
                SUCCEEDED(IMFDXGIBuffer_GetSubresourceIndex(db, &sub))) {
                D3D11_TEXTURE2D_DESC desc;
                ID3D11Texture2D_GetDesc(tex, &desc);
                if (!m->staging || m->stg_w != desc.Width || m->stg_h != desc.Height) {
                    if (m->staging) ID3D11Texture2D_Release(m->staging), m->staging = NULL;
                    D3D11_TEXTURE2D_DESC sd = desc;
                    sd.ArraySize = 1, sd.MipLevels = 1, sd.BindFlags = 0, sd.MiscFlags = 0;
                    sd.Usage = D3D11_USAGE_STAGING, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    if (SUCCEEDED(ID3D11Device_CreateTexture2D(m->dev, &sd, NULL, &m->staging))) m->stg_w = desc.Width, m->stg_h = desc.Height;
                }
                D3D11_MAPPED_SUBRESOURCE mp;
                if (m->staging) {
                    ID3D11DeviceContext_CopySubresourceRegion(m->dctx, (ID3D11Resource *)m->staging, 0, 0, 0, 0, (ID3D11Resource *)tex, sub, NULL);
                    if (SUCCEEDED(ID3D11DeviceContext_Map(m->dctx, (ID3D11Resource *)m->staging, 0, D3D11_MAP_READ, 0, &mp))) {
                        const int ow = m->want_w > 0 && m->want_w < (int)desc.Width ? m->want_w : (int)desc.Width;
                        const int oh = m->want_h > 0 && m->want_h < (int)desc.Height ? m->want_h : (int)desc.Height;
                        const BYTE *y = (const BYTE *)mp.pData;
                        fn(user, y, y + (size_t)mp.RowPitch * desc.Height, ow, oh, (int)mp.RowPitch, (int64_t)t100 * 100);
                        ID3D11DeviceContext_Unmap(m->dctx, (ID3D11Resource *)m->staging, 0);
                        if (!m->hw_pictures++) hos_log("video: the first AV1 picture read back from the GPU (%ux%u texture)", desc.Width, desc.Height);
                    }
                }
            }
            if (tex) ID3D11Texture2D_Release(tex);
            IMFDXGIBuffer_Release(db);
            IMFMediaBuffer_Release(b);
            if (ob.pEvents) IMFCollection_Release(ob.pEvents);
            IMFSample_Release(s);
            continue;
        }
        if (b) IMFMediaBuffer_Release(b), b = NULL;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(s, &b))) {
            BYTE *p = NULL;
            DWORD len = 0;
            IMFMediaBuffer_GetCurrentLength(b, &len);
            IMF2DBuffer *b2 = NULL;
            LONG pitch = 0;
            const int locked2d = SUCCEEDED(IMFMediaBuffer_QueryInterface(b, &IID_IMF2DBuffer, (void **)&b2)) &&
                                 SUCCEEDED(IMF2DBuffer_Lock2D(b2, &p, &pitch));
            if (!locked2d && SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, &len)))
                pitch = m->stride > 0 ? m->stride : m->w;
            if (p && pitch > 0) {
                /* the chroma plane follows the luma rows the buffer has (the padded height): rows = 1.5 x that */
                const DWORD rows = len / (DWORD)pitch;
                const int luma_rows = rows >= (DWORD)m->h * 3 / 2 ? (int)(rows * 2 / 3) : m->h;
                const int ow = m->want_w > 0 && m->want_w < m->w ? m->want_w : m->w;
                const int oh = m->want_h > 0 && m->want_h < m->h ? m->want_h : m->h;
                fn(user, p, p + (size_t)pitch * luma_rows, ow, oh, (int)pitch, (int64_t)t100 * 100);
                if (locked2d) IMF2DBuffer_Unlock2D(b2);
                else IMFMediaBuffer_Unlock(b);
            }
            if (b2) IMF2DBuffer_Release(b2);
            IMFMediaBuffer_Release(b);
        }
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        IMFSample_Release(s);   /* (own, or the decoder's) */
    }
}

/* one temporal unit (pts in ns), or data NULL: the stream's end (the rest out) */
void mf_av1_decode(void *h, const uint8_t *data, size_t n, int64_t pts, mf_av1_picture_fn fn, void *user) {
    MfAv1 *m = (MfAv1 *)h;
    if (!m) return;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (!data) {
        IMFTransform_ProcessMessage(m->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        IMFTransform_ProcessMessage(m->mft, MFT_MESSAGE_COMMAND_DRAIN, 0);
        pull(m, fn, user);
        IMFTransform_ProcessMessage(m->mft, MFT_MESSAGE_COMMAND_FLUSH, 0);
        IMFTransform_ProcessMessage(m->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return;
    }
    IMFSample *s = NULL;
    IMFMediaBuffer *b = NULL;
    if (FAILED(MFCreateSample(&s)) || FAILED(MFCreateMemoryBuffer((DWORD)n, &b))) { if (s) IMFSample_Release(s); return; }
    BYTE *p = NULL;
    IMFMediaBuffer_Lock(b, &p, NULL, NULL);
    memcpy(p, data, n);
    IMFMediaBuffer_Unlock(b);
    IMFMediaBuffer_SetCurrentLength(b, (DWORD)n);
    IMFSample_AddBuffer(s, b);
    IMFSample_SetSampleTime(s, pts / 100);
    for (int tries = 0; tries < 2; tries++) {
        const HRESULT hr = IMFTransform_ProcessInput(m->mft, 0, s, 0);
        if (hr == MF_E_NOTACCEPTING) { pull(m, fn, user); continue; }   /* (full: its pictures out first) */
        if (FAILED(hr)) {
            static int logged;
            if (logged++ < 4) hos_log("video: AV1 decoder ProcessInput failed (%#lx, %zu bytes)", (unsigned long)hr, n);
        }
        break;
    }
    IMFMediaBuffer_Release(b);
    IMFSample_Release(s);
    pull(m, fn, user);
}

void mf_av1_flush(void *h) {
    MfAv1 *m = (MfAv1 *)h;
    if (!m) return;
    IMFTransform_ProcessMessage(m->mft, MFT_MESSAGE_COMMAND_FLUSH, 0);
    IMFTransform_ProcessMessage(m->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

void mf_av1_close(void *h) {
    MfAv1 *m = (MfAv1 *)h;
    if (!m) return;
    IMFTransform_ProcessMessage(m->mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    IMFTransform_Release(m->mft);
    hw_release(m);
    free(m);
}
