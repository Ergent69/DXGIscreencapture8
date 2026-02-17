/* Obs settings (Only for libx264 mode):
    Type: Custom Output(FFmpeg)
    FFmpeg Output Type : Output to URL
    File path or URL : \\.\pipe\obs_video
    Container Format : mpegts
    Video Bitrate : 6000 Kbps
    Keyframe interval(frames) : 60
    Video Encoder : libx264
    Audio Bitrate : 160 Kbps
    Audio Encoder : aac
*/

#define WM_VIDEO_RESIZE (WM_USER + 1)
#define WM_OBS_STARTED  (WM_USER + 2)

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS 
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fstream>
#include <filesystem>
#include <direct.h>
#include <d3d11_3.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <sstream>
#include <wrl/client.h>
#include <algorithm> 
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swresample.lib")
#pragma comment(lib, "swscale.lib")

// Linker settings required for typical setup
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:mainCRTStartup")

namespace fs = std::filesystem;
// FFmpeg headers
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

using Microsoft::WRL::ComPtr;

// --- КОНСТАНТЫ И ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ---
const int WINDOW_WIDTH = 1000;
const int WINDOW_HEIGHT = 800;
const int TOP_PANEL_HEIGHT = 90;
const int CONSOLE_HEIGHT = 140;

const DWORD PIPE_BUFFER_SIZE = 1024 * 1024 * 16;
const int UDP_PORT = 8221;
const int UDP_PACKET_SIZE = 1316; // MPEG-TS friendly size (188 * 7)

// Control IDs
#define ID_EDIT_RES     101
#define ID_COMBO_FPS    102
#define ID_BTN_APPLY    103
#define ID_CONSOLE_BOX  104
#define ID_COMBO_CODEC  105
#define ID_CHK_SHOW     106
#define ID_CHK_STREAM   107
// New Controls
#define ID_CHK_CUSTOM   108
#define ID_COMBO_RES    109
#define ID_EDIT_FPS     110

// Структура для кодеков
struct CodecOption {
    std::string name;
    std::string ffmpegName;
    int id; // Внутренний ID (0 - OBS/x264, 1 - DXGI Raw)
    int obsId; // ID для конфига OBS
};

const std::vector<CodecOption> AVAILABLE_CODECS = {
    { "libx264 (Default)", "libx264", 0, 27 },       // Режим 0: OBS + Pipe
    { "Raw (DXGI Screen Capture)", "rawvideo", 1, 0 } // Режим 1: Без OBS, прямой захват
};

const std::vector<int> AVAILABLE_FPS = { 15, 30, 45, 60, 90, 120, 144, 165 };
const std::vector<std::pair<int, int>> AVAILABLE_RESOLUTIONS = {
    {512, 288}, {640, 360}, {854, 480}, {960, 540}, {1024, 576},
    {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}
};

std::atomic<bool> g_Running(true);
std::atomic<bool> g_RestartRequested(false);
std::atomic<bool> g_IsShowStream(false);
std::atomic<bool> g_IsStreamNetwork(false);

HWND g_hMainWindow = nullptr;
HWND g_hVideoWindow = nullptr;
HWND g_hConsoleWindow = nullptr;
HWND g_hEditRes = nullptr;
HWND g_hComboRes = nullptr;
HWND g_hComboFps = nullptr;
HWND g_hEditFps = nullptr;
HWND g_hBtnApply = nullptr;
HWND g_hComboCodec = nullptr;
HWND g_hChkShow = nullptr;
HWND g_hChkStream = nullptr;
HWND g_hChkCustom = nullptr;

HANDLE g_hJob = nullptr;
SOCKET g_UdpSocket = INVALID_SOCKET;
sockaddr_in g_UdpDestAddr;

void LogToGUI(const std::string& message) {
    if (!g_hConsoleWindow) return;
    int len = GetWindowTextLengthA(g_hConsoleWindow);
    SendMessageA(g_hConsoleWindow, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::string line = message + "\r\n";
    SendMessageA(g_hConsoleWindow, EM_REPLACESEL, 0, (LPARAM)line.c_str());
}

// ==========================================
// THREAD-SAFE PACKET QUEUE
// ==========================================
class PacketQueue {
private:
    std::queue<AVPacket*> q;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> finished;
public:
    PacketQueue() : finished(false) {}

    void push(AVPacket* pkt) {
        std::lock_guard<std::mutex> lock(m);
        q.push(pkt);
        cv.notify_one();
    }

    AVPacket* pop(bool& isFinished) {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [this] { return !q.empty() || finished; });

        if (q.empty()) {
            isFinished = finished;
            return nullptr;
        }

        AVPacket* pkt = q.front();
        q.pop();
        isFinished = false;
        return pkt;
    }

    void setFinished() {
        std::lock_guard<std::mutex> lock(m);
        finished = true;
        cv.notify_all();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m);
        while (!q.empty()) {
            AVPacket* pkt = q.front();
            q.pop();
            av_packet_free(&pkt);
        }
    }
};

// ==========================================
// СЕТЕВАЯ ЧАСТЬ
// ==========================================
void InitNetwork() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    g_UdpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_UdpSocket != INVALID_SOCKET) {
        BOOL broadcast = TRUE;
        setsockopt(g_UdpSocket, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));

        int sendBuff = 1024 * 1024 * 16;
        setsockopt(g_UdpSocket, SOL_SOCKET, SO_SNDBUF, (char*)&sendBuff, sizeof(sendBuff));

        g_UdpDestAddr.sin_family = AF_INET;
        g_UdpDestAddr.sin_port = htons(UDP_PORT);
        g_UdpDestAddr.sin_addr.s_addr = INADDR_BROADCAST;
    }
}

void SendUdpData(const uint8_t* data, int size) {
    if (!g_IsStreamNetwork || g_UdpSocket == INVALID_SOCKET) return;
    int sent = 0;
    while (sent < size) {
        int chunkSize = std::min(UDP_PACKET_SIZE, size - sent);
        sendto(g_UdpSocket, (const char*)(data + sent), chunkSize, 0, (sockaddr*)&g_UdpDestAddr, sizeof(g_UdpDestAddr));
        sent += chunkSize;
    }
}

// ==========================================
// ЛОГИКА РЕСАЙЗА (GUI)
// ==========================================
void UpdateVideoLayout(int contentW, int contentH) {
    if (!g_hMainWindow || !g_hVideoWindow) return;
    if (contentW <= 0 || contentH <= 0) return;

    RECT rcMain;
    GetClientRect(g_hMainWindow, &rcMain);

    int availW = rcMain.right - rcMain.left;
    int startY = TOP_PANEL_HEIGHT + CONSOLE_HEIGHT;
    int availH = (rcMain.bottom - rcMain.top) - startY;

    if (availH <= 0) return;
    float scaleX = (float)availW / contentW;
    float scaleY = (float)availH / contentH;
    float scale = std::min(scaleX, scaleY);
    int newW = (int)(contentW * scale);
    int newH = (int)(contentH * scale);

    int x = (availW - newW) / 2;
    int y = startY + (availH - newH) / 2;

    SetWindowPos(g_hVideoWindow, NULL, x, y, newW, newH, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ==========================================
// ШЕЙДЕРЫ И РЕНДЕР
// ==========================================

const char* HLSL_NV12_TO_RGB = R"(
    Texture2D<float> InputY : register(t0);
    Texture2D<float2> InputUV : register(t1);
    RWTexture2D<float4> OutputRGB : register(u0);

    [numthreads(8, 8, 1)]
    void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
        uint2 pos = dispatchThreadId.xy;
        uint width, height;
        OutputRGB.GetDimensions(width, height);
        if (pos.x >= width || pos.y >= height) return;
        
        float Y = InputY[uint2(pos.x, pos.y)];
        float2 UV = InputUV[uint2(pos.x / 2, pos.y / 2)];
        float U = UV.x - 0.5f;
        float V = UV.y - 0.5f;

        float r = Y + 1.13983f * V;
        float g = Y - 0.39465f * U - 0.58060f * V;
        float b = Y + 2.03211f * U;
        OutputRGB[pos] = float4(r, g, b, 1.0f);
    }
)";

const char* HLSL_RESIZE = R"(
    Texture2D<float4> Input : register(t0);
    RWTexture2D<float4> Output : register(u0);
    cbuffer Params : register(b0) { uint srcW; uint srcH; uint dstW; uint dstH; };

    [numthreads(16, 16, 1)]
    void CSMain(uint3 id : SV_DispatchThreadID) {
        if (id.x >= dstW || id.y >= dstH) return;
        uint sx = (id.x * srcW) / dstW;
        uint sy = (id.y * srcH) / dstH;
        Output[id.xy] = Input[uint2(sx, sy)];
    }
)";

#define ALIGN_32(x) (((x) + 31) & ~31)

struct ResizeParams {
    uint32_t srcW, srcH, dstW, dstH;
};

class D3DRenderer {
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain1> m_swapChain;

    // OBS Mode
    ComPtr<ID3D11ComputeShader> m_csNv12ToRgb;
    ComPtr<ID3D11Texture2D> m_workTexture;

    // DXGI Mode
    ComPtr<ID3D11ComputeShader> m_csResize;
    ComPtr<ID3D11Buffer> m_cbResizeParams;
    ComPtr<ID3D11Texture2D> m_scaledTexture;
    ComPtr<ID3D11Texture2D> m_captureCopyTexture;

    ComPtr<ID3D11Texture2D> m_stagingTexture;
    struct SwsContext* m_swsCtx = nullptr;
    int m_swsWidth = 0;
    int m_swsHeight = 0;
    enum AVPixelFormat m_swsFmt = AV_PIX_FMT_NONE;
    uint8_t* m_nv12Buffer = nullptr;
    int m_nv12Stride = 0;

    int m_width = 0, m_height = 0;
    HWND m_hwndVideo;
    HWND m_hwndMain;
public:
    D3DRenderer(HWND hVideo, HWND hMain) : m_hwndVideo(hVideo), m_hwndMain(hMain) {
        InitD3D(hVideo);
        InitShaders();
    }

    ~D3DRenderer() {
        if (m_swsCtx) sws_freeContext(m_swsCtx);
        if (m_nv12Buffer) _aligned_free(m_nv12Buffer);
    }

    ID3D11Device* GetDevice() { return m_device.Get(); }

    void ResizeSwapChain(int w, int h) {
        if (m_width == w && m_height == h) return;

        m_context->ClearState();
        m_context->OMSetRenderTargets(0, nullptr, nullptr);

        m_width = w;
        m_height = h;

        m_workTexture.Reset();
        m_scaledTexture.Reset();
        m_captureCopyTexture.Reset();

        if (m_nv12Buffer) { _aligned_free(m_nv12Buffer); m_nv12Buffer = nullptr; }

        m_nv12Stride = ALIGN_32(w);
        size_t dataSize = m_nv12Stride * h + m_nv12Stride * (h / 2);
        m_nv12Buffer = (uint8_t*)_aligned_malloc(dataSize, 32);
        if (m_nv12Buffer) memset(m_nv12Buffer, 0, dataSize);

        if (m_swapChain) {
            m_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        }
        PostMessage(m_hwndMain, WM_VIDEO_RESIZE, (WPARAM)w, (LPARAM)h);
    }

    // --- GPU DOWNSCALE & SEND (DXGI MODE) ---
    void ProcessDXGIFrame(ID3D11Texture2D* srcTexture, int targetW, int targetH) {
        if (!srcTexture) return;
        D3D11_TEXTURE2D_DESC srcDesc;
        srcTexture->GetDesc(&srcDesc);

        ID3D11Texture2D* texToProcess = srcTexture;

        if (srcDesc.Width != (UINT)targetW || srcDesc.Height != (UINT)targetH) {
            EnsureTexture(m_scaledTexture, targetW, targetH, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);

            if (!(srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
                EnsureTexture(m_captureCopyTexture, srcDesc.Width, srcDesc.Height, srcDesc.Format, D3D11_BIND_SHADER_RESOURCE);
                m_context->CopyResource(m_captureCopyTexture.Get(), srcTexture);
                texToProcess = m_captureCopyTexture.Get();
            }

            PerformResize(texToProcess, m_scaledTexture.Get(), srcDesc.Width, srcDesc.Height, targetW, targetH);
            texToProcess = m_scaledTexture.Get();
        }
        else {
            EnsureTexture(m_scaledTexture, targetW, targetH, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
            m_context->CopyResource(m_scaledTexture.Get(), srcTexture);
            texToProcess = m_scaledTexture.Get();
        }

        if (g_IsShowStream) {
            ResizeSwapChain(targetW, targetH);
            ComPtr<ID3D11Texture2D> backBuffer;
            HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
            if (SUCCEEDED(hr) && backBuffer) {
                m_context->CopySubresourceRegion(backBuffer.Get(), 0, 0, 0, 0, texToProcess, 0, nullptr);
            }
            m_swapChain->Present(0, 0);
        }

        if (g_IsStreamNetwork) {
            SendTextureOverUDP(texToProcess, targetW, targetH);
        }
    }

    void RenderFrame(AVFrame* frame) {
        if (!frame || !m_swapChain) return;
        if (!g_IsShowStream) return;

        ResizeSwapChain(frame->width, frame->height);
        if (frame->format == AV_PIX_FMT_D3D11) {
            ID3D11Texture2D* srcTexture = (ID3D11Texture2D*)frame->data[0];
            int srcIndex = (intptr_t)frame->data[1];
            EnsureTexture(m_workTexture, frame->width, frame->height, DXGI_FORMAT_NV12, D3D11_BIND_SHADER_RESOURCE);
            m_context->CopySubresourceRegion(m_workTexture.Get(), 0, 0, 0, 0, srcTexture, srcIndex, nullptr);
            RunComputeShaderNV12();
        }
        else {
            RenderSoftwareFrame(frame);
        }
        m_swapChain->Present(0, 0);
    }

private:
    void PerformResize(ID3D11Texture2D* input, ID3D11Texture2D* output, int srcW, int srcH, int dstW, int dstH) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_context->Map(m_cbResizeParams.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            ResizeParams* p = (ResizeParams*)mapped.pData;
            p->srcW = srcW; p->srcH = srcH; p->dstW = dstW; p->dstH = dstH;
            m_context->Unmap(m_cbResizeParams.Get(), 0);
        }

        ComPtr<ID3D11ShaderResourceView> srv;
        m_device->CreateShaderResourceView(input, nullptr, &srv);

        ComPtr<ID3D11UnorderedAccessView> uav;
        m_device->CreateUnorderedAccessView(output, nullptr, &uav);

        m_context->CSSetShader(m_csResize.Get(), nullptr, 0);
        m_context->CSSetConstantBuffers(0, 1, m_cbResizeParams.GetAddressOf());
        m_context->CSSetShaderResources(0, 1, srv.GetAddressOf());
        m_context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);

        UINT threadsX = (dstW + 15) / 16;
        UINT threadsY = (dstH + 15) / 16;
        m_context->Dispatch(threadsX, threadsY, 1);

        ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
        m_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
        ID3D11ShaderResourceView* nullSRV[] = { nullptr };
        m_context->CSSetShaderResources(0, 1, nullSRV);
    }

    void SendTextureOverUDP(ID3D11Texture2D* tex, int w, int h) {
        EnsureStagingTexture(w, h);
        m_context->CopyResource(m_stagingTexture.Get(), tex);

        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = m_context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            uint8_t* ptr = (uint8_t*)mapped.pData;
            std::vector<uint8_t> rgbBuffer;
            rgbBuffer.resize(w * h * 3);

            for (int y = 0; y < h; y++) {
                uint8_t* rowSrc = ptr + y * mapped.RowPitch;
                uint8_t* rowDst = rgbBuffer.data() + y * w * 3;
                for (int x = 0; x < w; x++) {
                    uint8_t b = rowSrc[x * 4 + 0];
                    uint8_t g = rowSrc[x * 4 + 1];
                    uint8_t r = rowSrc[x * 4 + 2];
                    rowDst[x * 3 + 0] = r;
                    rowDst[x * 3 + 1] = g;
                    rowDst[x * 3 + 2] = b;
                }
            }
            SendUdpData(rgbBuffer.data(), (int)rgbBuffer.size());
            m_context->Unmap(m_stagingTexture.Get(), 0);
        }
    }

    void RenderSoftwareFrame(AVFrame* frame) {
        EnsureTexture(m_workTexture, frame->width, frame->height, DXGI_FORMAT_NV12, D3D11_BIND_SHADER_RESOURCE);
        if (!m_nv12Buffer) return;

        if (!m_swsCtx || m_swsWidth != frame->width || m_swsHeight != frame->height || m_swsFmt != frame->format) {
            if (m_swsCtx) sws_freeContext(m_swsCtx);
            m_swsWidth = frame->width;
            m_swsHeight = frame->height;
            m_swsFmt = (AVPixelFormat)frame->format;
            m_swsCtx = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format, frame->width, frame->height, AV_PIX_FMT_NV12, SWS_POINT, nullptr, nullptr, nullptr);
        }
        if (!m_swsCtx) return;

        uint8_t* dstData[4] = { m_nv12Buffer, m_nv12Buffer + (m_nv12Stride * frame->height), nullptr, nullptr };
        int dstLinesize[4] = { m_nv12Stride, m_nv12Stride, 0, 0 };
        sws_scale(m_swsCtx, frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize);
        m_context->UpdateSubresource(m_workTexture.Get(), 0, nullptr, m_nv12Buffer, m_nv12Stride, 0);
        RunComputeShaderNV12();
    }

    void RunComputeShaderNV12() {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
        if (FAILED(hr)) return;

        ComPtr<ID3D11UnorderedAccessView> backBufferUAV;
        m_device->CreateUnorderedAccessView(backBuffer.Get(), nullptr, &backBufferUAV);

        ComPtr<ID3D11Device3> device3;
        m_device.As(&device3);

        D3D11_SHADER_RESOURCE_VIEW_DESC1 srvDesc1 = {};
        srvDesc1.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc1.Texture2D.MipLevels = 1;
        srvDesc1.Format = DXGI_FORMAT_R8_UNORM;
        ComPtr<ID3D11ShaderResourceView1> ySRV;
        device3->CreateShaderResourceView1(m_workTexture.Get(), &srvDesc1, &ySRV);

        srvDesc1.Format = DXGI_FORMAT_R8G8_UNORM;
        srvDesc1.Texture2D.PlaneSlice = 1;
        ComPtr<ID3D11ShaderResourceView1> uvSRV;
        device3->CreateShaderResourceView1(m_workTexture.Get(), &srvDesc1, &uvSRV);

        ID3D11ShaderResourceView* inputSRVs[] = { ySRV.Get(), uvSRV.Get() };
        m_context->CSSetShader(m_csNv12ToRgb.Get(), nullptr, 0);
        m_context->CSSetShaderResources(0, 2, inputSRVs);
        ID3D11UnorderedAccessView* outputUAVs[] = { backBufferUAV.Get() };
        m_context->CSSetUnorderedAccessViews(0, 1, outputUAVs, nullptr);
        m_context->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);

        ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
        m_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
        ID3D11ShaderResourceView* nullSRV[] = { nullptr, nullptr };
        m_context->CSSetShaderResources(0, 2, nullSRV);
    }

    void EnsureTexture(ComPtr<ID3D11Texture2D>& tex, int width, int height, DXGI_FORMAT fmt, UINT bindFlags) {
        if (tex) {
            D3D11_TEXTURE2D_DESC desc;
            tex->GetDesc(&desc);
            if (desc.Width == width && desc.Height == height && desc.Format == fmt) return;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = bindFlags;
        m_device->CreateTexture2D(&desc, nullptr, &tex);
    }

    void EnsureStagingTexture(int width, int height) {
        if (m_stagingTexture) {
            D3D11_TEXTURE2D_DESC desc;
            m_stagingTexture->GetDesc(&desc);
            if (desc.Width == width && desc.Height == height) return;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        m_device->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
    }

    void InitD3D(HWND hwnd) {
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &m_device, nullptr, &m_context);

        ComPtr<IDXGIFactory2> factory;
        CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&factory);
        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scDesc.SampleDesc.Count = 1;
        scDesc.BufferUsage = DXGI_USAGE_UNORDERED_ACCESS | DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = 2;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.Scaling = DXGI_SCALING_STRETCH;

        factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &scDesc, nullptr, nullptr, &m_swapChain);
    }

    void InitShaders() {
        ComPtr<ID3DBlob> blob, err;
        D3DCompile(HLSL_NV12_TO_RGB, strlen(HLSL_NV12_TO_RGB), nullptr, nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &blob, &err);
        if (blob) m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_csNv12ToRgb);
        blob.Reset();

        D3DCompile(HLSL_RESIZE, strlen(HLSL_RESIZE), nullptr, nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &blob, &err);
        if (blob) m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_csResize);

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(ResizeParams);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_device->CreateBuffer(&bd, nullptr, &m_cbResizeParams);
    }
};

// ==========================================
// ЛОГИКА КОНФИГА
// ==========================================

void ModifyBasicIni(int width, int height, int fps, int codecIndex) {
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(NULL, buffer, MAX_PATH) == 0) return;

    fs::path exeDir = fs::path(buffer).parent_path();
    fs::path iniPath = exeDir / "config" / "obs-studio" / "basic" / "profiles" / "Untitled" / "basic.ini";

    if (!fs::exists(iniPath)) return;
    const CodecOption& selCodec = AVAILABLE_CODECS[codecIndex];
    std::vector<std::string> lines;
    std::ifstream inFile(iniPath);
    std::string line;
    std::string sRes = std::to_string(width) + "x" + std::to_string(height);
    std::string sFps = std::to_string(fps);
    int idToWrite = (selCodec.id == 1) ? 9999 : selCodec.obsId;

    if (inFile.is_open()) {
        while (std::getline(inFile, line)) {
            if (line.find("RescaleRes=") == 0) line = "RescaleRes=" + sRes;
            else if (line.find("RecRescaleRes=") == 0) line = "RecRescaleRes=" + sRes;
            else if (line.find("FFRescaleRes=") == 0) line = "FFRescaleRes=" + sRes;
            else if (line.find("BaseCX=") == 0) line = "BaseCX=" + std::to_string(width);
            else if (line.find("BaseCY=") == 0) line = "BaseCY=" + std::to_string(height);
            else if (line.find("OutputCX=") == 0) line = "OutputCX=" + std::to_string(width);
            else if (line.find("OutputCY=") == 0) line = "OutputCY=" + std::to_string(height);
            else if (line.find("FPSNum=") == 0) line = "FPSNum=" + sFps;
            else if (line.find("FFVEncoderId=") == 0) line = "FFVEncoderId=" + std::to_string(idToWrite);
            lines.push_back(line);
        }
        inFile.close();
    }

    std::ofstream outFile(iniPath);
    if (outFile.is_open()) {
        for (const auto& l : lines) outFile << l << "\n";
        outFile.close();
    }
}

void ReadConfigSettings(int& width, int& height, int& fps, int& codecId) {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    fs::path iniPath = fs::path(buffer).parent_path() / "config" / "obs-studio" / "basic" / "profiles" / "Untitled" / "basic.ini";

    width = 1280; height = 720; fps = 60; codecId = 0;
    int encoderId = 27;

    std::ifstream inFile(iniPath);
    if (inFile.is_open()) {
        std::string line;
        while (std::getline(inFile, line)) {
            try {
                if (line.find("BaseCX=") == 0) width = std::stoi(line.substr(7));
                else if (line.find("BaseCY=") == 0) height = std::stoi(line.substr(7));
                else if (line.find("FPSNum=") == 0) fps = std::stoi(line.substr(7));
                else if (line.find("FFVEncoderId=") == 0) encoderId = std::stoi(line.substr(13));
            }
            catch (...) {}
        }
    }
    codecId = (encoderId == 9999) ? 1 : 0;
}

void SetupPortableOBS() {
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(NULL, buffer, MAX_PATH) == 0) return;
    fs::path exeDir = fs::path(buffer).parent_path();
    fs::path sourceConfig = exeDir / "config";
    fs::path targetConfig = exeDir / "obs-studio" / "config";
    if (fs::exists(targetConfig)) fs::remove_all(targetConfig);
    if (fs::exists(sourceConfig)) {
        fs::create_directories(targetConfig.parent_path());
        fs::copy(sourceConfig, targetConfig, fs::copy_options::recursive);
    }
}

void LaunchOBS() {
    SetupPortableOBS();
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    fs::path exeDir = fs::path(buffer).parent_path();
    fs::path obsExePath = exeDir / "obs-studio" / "bin" / "64bit" / "obs64.exe";
    if (!fs::exists(obsExePath)) return;
    if (!g_hJob) {
        g_hJob = CreateJobObject(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = { 0 };
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }
    std::wstring cmdLine = L"\"" + obsExePath.wstring() + L"\" --portable --profile \"Untitled\" --collection \"Untitled\" --startrecording --minimize-to-tray";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, FALSE, 0, nullptr, obsExePath.parent_path().c_str(), &si, &pi)) {
        if (g_hJob) AssignProcessToJobObject(g_hJob, pi.hProcess);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

// ==========================================
// ДЕКОДИНГ И ЗАХВАТ
// ==========================================

void RunDXGICaptureLoop(D3DRenderer* renderer) {
    int targetW, targetH, targetFps, codecId;
    ReadConfigSettings(targetW, targetH, targetFps, codecId);
    if (targetW < 100) targetW = 1280;
    if (targetH < 100) targetH = 720;
    if (targetFps < 1) targetFps = 30;

    LogToGUI("Starting DXGI Capture: " + std::to_string(targetW) + "x" + std::to_string(targetH) + " @ " + std::to_string(targetFps) + " FPS");

    ID3D11Device* device = renderer->GetDevice();
    ComPtr<IDXGIDevice> dxgiDevice;
    device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&adapter);
    ComPtr<IDXGIOutput> output;
    adapter->EnumOutputs(0, &output);
    ComPtr<IDXGIOutput1> output1;
    output.As(&output1);
    ComPtr<IDXGIOutputDuplication> duplication;
    HRESULT hr = output1->DuplicateOutput(device, &duplication);

    if (FAILED(hr)) {
        LogToGUI("Failed to DuplicateOutput. Make sure OBS is not blocking it.");
        return;
    }

    PostMessage(g_hMainWindow, WM_OBS_STARTED, 0, 0);

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    ComPtr<IDXGIResource> desktopResource;
    ComPtr<ID3D11Texture2D> frameTexture;

    using namespace std::chrono;
    auto frameInterval = microseconds(1000000 / targetFps);
    auto nextFrameTime = steady_clock::now();

    while (g_Running && !g_RestartRequested) {
        auto now = steady_clock::now();
        if (now < nextFrameTime) {
            std::this_thread::sleep_for(milliseconds(1));
            continue;
        }
        nextFrameTime += frameInterval;

        desktopResource.Reset();
        frameTexture.Reset();

        hr = duplication->AcquireNextFrame(100, &frameInfo, &desktopResource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) {
            duplication.Reset();
            output1->DuplicateOutput(device, &duplication);
            continue;
        }

        desktopResource.As(&frameTexture);
        if (frameTexture) {
            renderer->ProcessDXGIFrame(frameTexture.Get(), targetW, targetH);
        }
        duplication->ReleaseFrame();
    }
}

// --- FFMPEG PIPE READER ---
struct ReaderCtx { HANDLE hPipe; };
int ReadPacket(void* opaque, uint8_t* buf, int buf_size) {
    ReaderCtx* ctx = (ReaderCtx*)opaque;
    DWORD bytesRead = 0;
    if (!ReadFile(ctx->hPipe, buf, buf_size, &bytesRead, NULL)) return AVERROR_EOF;
    if (bytesRead == 0) return AVERROR_EOF;
    if (g_IsStreamNetwork) {
        SendUdpData(buf, bytesRead);
    }
    return bytesRead;
}

static enum AVPixelFormat GetHwFormat(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    const enum AVPixelFormat* p;
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == AV_PIX_FMT_D3D11) return *p;
    }
    return AV_PIX_FMT_NV12;
}

void RunPacketReaderThread(AVFormatContext* fmtCtx, PacketQueue* queue, int videoStreamIdx) {
    AVPacket* pkt = av_packet_alloc();
    while (g_Running && !g_RestartRequested) {
        if (av_read_frame(fmtCtx, pkt) < 0) break;
        if (pkt->stream_index == videoStreamIdx) {
            AVPacket* newPkt = av_packet_alloc();
            av_packet_ref(newPkt, pkt);
            queue->push(newPkt);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    queue->setFinished();
}

void RunFFmpegLoop(D3DRenderer* renderer) {
    LogToGUI("Creating Named Pipe...");
    HANDLE hPipe = CreateNamedPipeA("\\\\.\\pipe\\obs_video", PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, PIPE_BUFFER_SIZE, PIPE_BUFFER_SIZE, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) {
        LogToGUI("Error: Failed to create pipe.");
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return;
    }

    std::thread obsThread(LaunchOBS);
    obsThread.detach();

    LogToGUI("Waiting for OBS connection...");
    // ConnectNamedPipe блокирует поток до подключения клиента (OBS)
    if (!ConnectNamedPipe(hPipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        LogToGUI("Error: ConnectNamedPipe failed.");
        CloseHandle(hPipe);
        return;
    }
    LogToGUI("OBS Connected to pipe.");
    PostMessage(g_hMainWindow, WM_OBS_STARTED, 0, 0);

    size_t ioBufferSize = 1024 * 1024; // Increase buffer for stability
    unsigned char* ioBuffer = (unsigned char*)av_malloc(ioBufferSize + AV_INPUT_BUFFER_PADDING_SIZE);
    ReaderCtx ctx = { hPipe };
    AVIOContext* avioCtx = avio_alloc_context(ioBuffer, ioBufferSize, 0, &ctx, ReadPacket, nullptr, nullptr);

    AVFormatContext* fmtCtx = avformat_alloc_context();
    fmtCtx->pb = avioCtx;

    // Use MPEGTS detection
    const AVInputFormat* in_fmt = av_find_input_format("mpegts");

    // Critical options for PIPE reading
    AVDictionary* options = nullptr;
    // FIX: Probesize must be > 188*2 to detect MPEG-TS. 5MB is safe.
    av_dict_set(&options, "probesize", "5000000", 0);
    av_dict_set(&options, "analyzeduration", "5000000", 0);
    av_dict_set(&options, "fflags", "nobuffer", 0);
    // Removed "low_delay" from flags to prevent header drop on startup if buffer is slow
    // av_dict_set(&options, "flags", "low_delay", 0); 

    LogToGUI("Opening input stream...");
    int err = avformat_open_input(&fmtCtx, nullptr, in_fmt, &options);
    if (err < 0) {
        char errBuf[128];
        av_strerror(err, errBuf, 128);
        LogToGUI(std::string("Error opening input: ") + errBuf);
        CloseHandle(hPipe);
        return;
    }
    av_dict_free(&options);
    LogToGUI("Stream Opened Successfully.");

    int videoStreamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStreamIdx < 0) {
        LogToGUI("Error: No video stream found.");
        CloseHandle(hPipe);
        return;
    }

    AVCodecParameters* codecPar = fmtCtx->streams[videoStreamIdx]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(codecPar->codec_id);
    AVCodecContext* decCtx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(decCtx, codecPar);
    AVBufferRef* hwDeviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (hwDeviceRef) {
        AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)hwDeviceRef->data;
        AVD3D11VADeviceContext* d3d11Ctx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;
        d3d11Ctx->device = renderer->GetDevice();
        d3d11Ctx->device->AddRef();
        av_hwdevice_ctx_init(hwDeviceRef);
        decCtx->hw_device_ctx = av_buffer_ref(hwDeviceRef);
        av_buffer_unref(&hwDeviceRef);
        decCtx->get_format = GetHwFormat;
    }
    decCtx->thread_count = 1;
    decCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    if (avcodec_open2(decCtx, decoder, nullptr) < 0) {
        LogToGUI("Error: Could not open codec.");
        return;
    }

    PacketQueue packetQueue;
    std::thread readerThread(RunPacketReaderThread, fmtCtx, &packetQueue, videoStreamIdx);
    AVFrame* frame = av_frame_alloc();
    bool finished = false;

    LogToGUI("Starting loop...");
    while (g_Running && !g_RestartRequested) {
        AVPacket* pkt = packetQueue.pop(finished);
        if (finished && !pkt) break;
        if (!pkt) continue;

        if (avcodec_send_packet(decCtx, pkt) >= 0) {
            while (avcodec_receive_frame(decCtx, frame) >= 0) {
                renderer->RenderFrame(frame);
                av_frame_unref(frame);
            }
        }
        av_packet_free(&pkt);
    }

    packetQueue.setFinished();
    if (readerThread.joinable()) readerThread.join();
    packetQueue.clear();
    av_frame_free(&frame);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);
    CloseHandle(hPipe);
    LogToGUI("FFmpeg Loop Ended.");
}

void DecoderManagerThread(D3DRenderer* renderer) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    av_log_set_level(AV_LOG_ERROR);
    while (g_Running) {
        g_RestartRequested = false;
        int w, h, fps, codecId;
        ReadConfigSettings(w, h, fps, codecId);

        if (codecId == 1) RunDXGICaptureLoop(renderer);
        else RunFFmpegLoop(renderer);

        if (g_RestartRequested) {
            LogToGUI("Restarting stream engine...");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        else if (g_Running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    CoUninitialize();
}

// ==========================================
// GUI WINDOW PROCEDURE
// ==========================================
void ToggleCustomControls(bool enable) {
    int cmdShowCustom = enable ? SW_SHOW : SW_HIDE;
    int cmdShowPreset = enable ? SW_HIDE : SW_SHOW;

    ShowWindow(g_hEditRes, cmdShowCustom);
    ShowWindow(g_hEditFps, cmdShowCustom);
    ShowWindow(g_hComboRes, cmdShowPreset);
    ShowWindow(g_hComboFps, cmdShowPreset);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE:
    {
        g_hMainWindow = hwnd;
        int y1 = 15;
        int y2 = 45;

        // Row 1: Controls
        CreateWindowA("STATIC", "Res:", WS_VISIBLE | WS_CHILD, 20, y1, 40, 20, hwnd, NULL, NULL, NULL);

        // Custom Input (Hidden by default)
        g_hEditRes = CreateWindowA("EDIT", "1280x720", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 70, y1, 100, 25, hwnd, (HMENU)ID_EDIT_RES, NULL, NULL);
        // Preset Dropdown (Visible by default)
        g_hComboRes = CreateWindowA("COMBOBOX", "", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 70, y1, 100, 300, hwnd, (HMENU)ID_COMBO_RES, NULL, NULL);
        for (const auto& r : AVAILABLE_RESOLUTIONS) {
            std::string s = std::to_string(r.first) + "x" + std::to_string(r.second);
            SendMessageA(g_hComboRes, CB_ADDSTRING, 0, (LPARAM)s.c_str());
        }
        SendMessage(g_hComboRes, CB_SETCURSEL, 5, 0); // Default 720p

        CreateWindowA("STATIC", "FPS:", WS_VISIBLE | WS_CHILD, 180, y1, 35, 20, hwnd, NULL, NULL, NULL);
        // Custom FPS (Hidden)
        g_hEditFps = CreateWindowA("EDIT", "60", WS_CHILD | WS_BORDER | ES_NUMBER | ES_CENTER, 220, y1, 50, 25, hwnd, (HMENU)ID_EDIT_FPS, NULL, NULL);
        // Preset FPS (Visible)
        g_hComboFps = CreateWindowA("COMBOBOX", "", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 220, y1, 50, 200, hwnd, (HMENU)ID_COMBO_FPS, NULL, NULL);
        for (int fps : AVAILABLE_FPS) SendMessageA(g_hComboFps, CB_ADDSTRING, 0, (LPARAM)std::to_string(fps).c_str());
        SendMessage(g_hComboFps, CB_SETCURSEL, 3, 0); // Default 60

        // Custom Values Checkbox
        g_hChkCustom = CreateWindowA("BUTTON", "Use custom values", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 280, y1, 140, 20, hwnd, (HMENU)ID_CHK_CUSTOM, NULL, NULL);

        // Codec Mode
        CreateWindowA("STATIC", "Mode:", WS_VISIBLE | WS_CHILD, 430, y1, 40, 20, hwnd, NULL, NULL, NULL);
        g_hComboCodec = CreateWindowA("COMBOBOX", "", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 480, y1, 180, 200, hwnd, (HMENU)ID_COMBO_CODEC, NULL, NULL);
        for (const auto& c : AVAILABLE_CODECS) SendMessageA(g_hComboCodec, CB_ADDSTRING, 0, (LPARAM)c.name.c_str());

        // Checkboxes
        g_hChkShow = CreateWindowA("BUTTON", "Show Stream", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 680, y1, 100, 20, hwnd, (HMENU)ID_CHK_SHOW, NULL, NULL);
        g_hChkStream = CreateWindowA("BUTTON", "Stream UDP", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 680, y1 + 25, 100, 20, hwnd, (HMENU)ID_CHK_STREAM, NULL, NULL);

        // Apply Button
        g_hBtnApply = CreateWindowA("BUTTON", "Apply & Restart", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 800, y1, 120, 25, hwnd, (HMENU)ID_BTN_APPLY, NULL, NULL);

        // Init UI State
        SendMessage(g_hChkShow, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessage(g_hChkStream, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessage(g_hChkCustom, BM_SETCHECK, BST_UNCHECKED, 0);

        // Console & Video
        g_hConsoleWindow = CreateWindowA("EDIT", "", WS_VISIBLE | WS_CHILD | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 0, TOP_PANEL_HEIGHT, WINDOW_WIDTH - 16, CONSOLE_HEIGHT, hwnd, (HMENU)ID_CONSOLE_BOX, NULL, NULL);
        g_hVideoWindow = CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD | SS_BLACKFRAME, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);

        // Load Settings
        int w, h, fps, codec;
        ReadConfigSettings(w, h, fps, codec);

        // Try to match preset resolution
        bool foundRes = false;
        for (int i = 0; i < AVAILABLE_RESOLUTIONS.size(); i++) {
            if (AVAILABLE_RESOLUTIONS[i].first == w && AVAILABLE_RESOLUTIONS[i].second == h) {
                SendMessage(g_hComboRes, CB_SETCURSEL, i, 0);
                foundRes = true;
                break;
            }
        }

        bool foundFps = false;
        for (int i = 0; i < AVAILABLE_FPS.size(); i++) {
            if (AVAILABLE_FPS[i] == fps) {
                SendMessage(g_hComboFps, CB_SETCURSEL, i, 0);
                foundFps = true;
                break;
            }
        }

        // If current config not in presets, enable Custom Mode
        if (!foundRes || !foundFps) {
            SendMessage(g_hChkCustom, BM_SETCHECK, BST_CHECKED, 0);
            ToggleCustomControls(true);
            std::string resStr = std::to_string(w) + "x" + std::to_string(h);
            SetWindowTextA(g_hEditRes, resStr.c_str());
            SetWindowTextA(g_hEditFps, std::to_string(fps).c_str());
        }

        SendMessage(g_hComboCodec, CB_SETCURSEL, (codec == 1) ? 1 : 0, 0);
        LogToGUI("System initialized. UDP Port: " + std::to_string(UDP_PORT));
        UpdateVideoLayout(w, h);
    }
    return 0;

    case WM_OBS_STARTED:
        LogToGUI("Stream Active.");
        if (g_hBtnApply) EnableWindow(g_hBtnApply, TRUE);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_CHK_SHOW) {
            g_IsShowStream = (SendMessage(g_hChkShow, BM_GETCHECK, 0, 0) == BST_CHECKED);
        }
        else if (LOWORD(wParam) == ID_CHK_STREAM) {
            g_IsStreamNetwork = (SendMessage(g_hChkStream, BM_GETCHECK, 0, 0) == BST_CHECKED);
        }
        else if (LOWORD(wParam) == ID_CHK_CUSTOM) {
            bool isCustom = (SendMessage(g_hChkCustom, BM_GETCHECK, 0, 0) == BST_CHECKED);
            ToggleCustomControls(isCustom);
        }
        else if (LOWORD(wParam) == ID_BTN_APPLY) {
            if (g_RestartRequested) return 0;

            int w = 0, h = 0, fps = 60;
            bool isCustom = (SendMessage(g_hChkCustom, BM_GETCHECK, 0, 0) == BST_CHECKED);

            if (isCustom) {
                char bufRes[64], bufFps[16];
                GetWindowTextA(g_hEditRes, bufRes, 64);
                GetWindowTextA(g_hEditFps, bufFps, 16);

                if (sscanf(bufRes, "%dx%d", &w, &h) != 2 || w <= 0 || w >= 10000 || h <= 0) {
                    LogToGUI("Invalid resolution format (WxH)!");
                    return 0;
                }
                try {
                    fps = std::stoi(bufFps);
                }
                catch (...) { fps = 0; }

                if (fps <= 0 || fps > 300) {
                    LogToGUI("FPS must be between 1 and 300!");
                    return 0;
                }
            }
            else {
                int rIdx = SendMessage(g_hComboRes, CB_GETCURSEL, 0, 0);
                if (rIdx >= 0 && rIdx < AVAILABLE_RESOLUTIONS.size()) {
                    w = AVAILABLE_RESOLUTIONS[rIdx].first;
                    h = AVAILABLE_RESOLUTIONS[rIdx].second;
                }
                int fIdx = SendMessage(g_hComboFps, CB_GETCURSEL, 0, 0);
                if (fIdx >= 0 && fIdx < AVAILABLE_FPS.size()) {
                    fps = AVAILABLE_FPS[fIdx];
                }
            }

            int codecIdx = SendMessage(g_hComboCodec, CB_GETCURSEL, 0, 0);
            if (codecIdx < 0) codecIdx = 0;

            LogToGUI("Applying: " + std::to_string(w) + "x" + std::to_string(h) + " @ " + std::to_string(fps) + " FPS");
            EnableWindow(g_hBtnApply, FALSE);
            UpdateVideoLayout(w, h);
            ModifyBasicIni(w, h, fps, codecIdx);
            g_RestartRequested = true;
            if (g_hJob) { CloseHandle(g_hJob); g_hJob = nullptr; }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int main() {
    InitNetwork();
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = L"OBSReceiverHub";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(LTGRAY_BRUSH);
    RegisterClassW(&wc);

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    g_hMainWindow = CreateWindowExW(0, L"OBSReceiverHub", L"OBS Stream Control Panel", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE, (scrW - WINDOW_WIDTH) / 2, (scrH - WINDOW_HEIGHT) / 2, WINDOW_WIDTH, WINDOW_HEIGHT, nullptr, nullptr, nullptr, nullptr);

    D3DRenderer renderer(g_hVideoWindow, g_hMainWindow);
    std::thread t(DecoderManagerThread, &renderer);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (!g_Running) break;
    }

    g_Running = false;
    if (g_hJob) CloseHandle(g_hJob);
    if (t.joinable()) t.join();
    if (g_UdpSocket != INVALID_SOCKET) closesocket(g_UdpSocket);
    WSACleanup();
    return 0;
}