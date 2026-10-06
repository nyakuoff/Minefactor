// Minecraft's overlay frame (first-person hand, HUD, open screens) drawn over Abiotic Factor's
// picture, D3D12 port of SkyCraft's D3D11 Overlay.cpp. We hook the swap chain's Present (vtable
// taken from a throwaway swap chain of our own) and the command queue's ExecuteCommandLists (to find
// the queue the game renders with), then draw one fullscreen triangle before each Present.
#include "Overlay.hpp"

#include <algorithm>
#include <cstring>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <windows.h>

#include "Link.hpp"
#include "Log.hpp"
#include "Perf.hpp"

namespace mf::overlay
{
    namespace
    {
        constexpr char kShader[] = R"(
cbuffer Params : register(b0) { float2 cursor; float2 viewport; float cursorOn; float flipY; float2 pad; float4 invertRect; };
Texture2D overlay : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
bool InInvertRect(float2 p) { return all(p >= invertRect.xy) && all(p < invertRect.zw); }
float4 Overlay(float2 uv) {
    if (flipY > 0.5) uv.y = 1 - uv.y;
    return overlay.Sample(samp, uv);   // premultiplied alpha straight from Minecraft
}
// Minecraft's crosshair: its invert blend against the game's picture, in a pass of its own.
float4 PSInvert(VSOut i) : SV_Target {
    if (!InInvertRect(i.pos.xy)) discard;
    return float4(Overlay(i.uv).rgb, 0);
}
float4 PSMain(VSOut i) : SV_Target {
    if (InInvertRect(i.pos.xy)) return 0;
    float4 c = Overlay(i.uv);
    if (cursorOn > 0.5) {
        float2 p = i.pos.xy - cursor;
        if (p.x >= 0 && p.y >= 0 && p.y < 18 && p.x <= p.y * 0.6) {
            bool edge = p.x < 1.5 || p.x > p.y * 0.6 - 1.5 || p.y > 16.5;
            c = float4(edge ? float3(0, 0, 0) : float3(1, 1, 1), 1);
        }
    }
    return c;
}
)";

        struct Params
        {
            float cursor[2], viewport[2], cursorOn, flipY, pad[2], invertRect[4];
        };

        template <class T>
        void Release(T*& p)
        {
            if (p)
            {
                p->Release();
                p = nullptr;
            }
        }

        using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
        using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
        PresentFn origPresent = nullptr;
        Present1Fn origPresent1 = nullptr;
        ExecuteFn origExecute = nullptr;

        std::atomic<ID3D12CommandQueue*> gameQueue{nullptr};

        // Everything below is only touched on the thread that presents.
        bool failed = false;
        ID3D12Device* device = nullptr;
        ID3D12RootSignature* rootSig = nullptr;
        ID3D12PipelineState* psoMain = nullptr;
        ID3D12PipelineState* psoInvert = nullptr;
        DXGI_FORMAT psoFormat = DXGI_FORMAT_UNKNOWN;
        ID3D12DescriptorHeap* rtvHeap = nullptr;
        ID3D12DescriptorHeap* srvHeap = nullptr;
        constexpr UINT kFrames = 4;  // command allocators in flight
        ID3D12CommandAllocator* allocators[kFrames]{};
        UINT64 allocatorFence[kFrames]{};
        UINT frameSlot = 0;
        ID3D12GraphicsCommandList* list = nullptr;
        ID3D12Fence* fence = nullptr;
        UINT64 fenceValue = 0;
        HANDLE fenceEvent = nullptr;
        ID3D12Resource* texture = nullptr;  // overlay, R8G8B8A8
        // Staging copies, rotated so writing a new frame never waits on the GPU's copy of the last one.
        constexpr UINT kUploads = 3;
        ID3D12Resource* upload[kUploads]{};
        std::uint8_t* uploadPtr[kUploads]{};
        UINT64 uploadFence[kUploads]{};
        UINT uploadNext = 0, uploadReady = 0;
        UINT texW = 0, texH = 0, uploadPitch = 0;
        bool texReady = false, flipY = false, pendingCopy = false;

        bool Compile(const char* entry, const char* target, ID3DBlob** out)
        {
            static auto compile = reinterpret_cast<pD3DCompile>(::GetProcAddress(::LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile"));
            if (!compile)
            {
                Log(L"overlay: no D3DCompile");
                return false;
            }
            ID3DBlob* errors = nullptr;
            if (FAILED(compile(kShader, sizeof(kShader) - 1, "minefactor_overlay", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors)))
            {
                Log(L"overlay: shader {} failed: {}", std::wstring(entry, entry + std::strlen(entry)),
                    errors ? std::wstring(static_cast<const char*>(errors->GetBufferPointer()), static_cast<const char*>(errors->GetBufferPointer()) + errors->GetBufferSize()) : L"?");
                Release(errors);
                return false;
            }
            Release(errors);
            return true;
        }

        bool CreatePipelines(DXGI_FORMAT format)
        {
            if (!rootSig)
            {
                D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
                D3D12_ROOT_PARAMETER params[2]{};
                params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[0].DescriptorTable = {1, &range};
                params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                params[1].Constants = {0, 0, sizeof(Params) / 4};
                params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                D3D12_STATIC_SAMPLER_DESC sampler{};
                sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sampler.MaxLOD = D3D12_FLOAT32_MAX;
                sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                D3D12_ROOT_SIGNATURE_DESC desc{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
                static auto serialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(::GetProcAddress(::GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
                ID3DBlob* blob = nullptr;
                if (!serialize || FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)) ||
                    FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig))))
                {
                    Release(blob);
                    Log(L"overlay: root signature failed");
                    return false;
                }
                Release(blob);
            }
            Release(psoMain);
            Release(psoInvert);
            ID3DBlob *vs = nullptr, *ps = nullptr, *psInv = nullptr;
            bool ok = Compile("VSMain", "vs_5_0", &vs) && Compile("PSMain", "ps_5_0", &ps) && Compile("PSInvert", "ps_5_0", &psInv);
            if (ok)
            {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
                pso.pRootSignature = rootSig;
                pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
                pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
                auto& rt = pso.BlendState.RenderTarget[0];
                rt.BlendEnable = TRUE;
                rt.SrcBlend = D3D12_BLEND_ONE;  // premultiplied
                rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                rt.BlendOp = D3D12_BLEND_OP_ADD;
                rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                rt.LogicOp = D3D12_LOGIC_OP_NOOP;
                rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pso.SampleMask = UINT_MAX;
                pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pso.RasterizerState.DepthClipEnable = TRUE;
                pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pso.NumRenderTargets = 1;
                pso.RTVFormats[0] = format;
                pso.SampleDesc.Count = 1;
                ok = SUCCEEDED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoMain)));
                // Minecraft's BlendFunction.INVERT; the back buffer's alpha is left alone.
                pso.PS = {psInv->GetBufferPointer(), psInv->GetBufferSize()};
                rt.SrcBlend = D3D12_BLEND_INV_DEST_COLOR;
                rt.DestBlend = D3D12_BLEND_INV_SRC_COLOR;
                rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
                rt.DestBlendAlpha = D3D12_BLEND_ONE;
                ok = ok && SUCCEEDED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&psoInvert)));
            }
            Release(vs);
            Release(ps);
            Release(psInv);
            if (!ok)
            {
                Log(L"overlay: pipeline for back buffer format {} failed", static_cast<int>(format));
                return false;
            }
            psoFormat = format;
            Log(L"overlay: ready (back buffer format {})", static_cast<int>(format));
            return true;
        }

        bool Init(IDXGISwapChain3* swap)
        {
            if (device)
            {
                return true;
            }
            if (FAILED(swap->GetDevice(IID_PPV_ARGS(&device))))
            {
                return false;
            }
            D3D12_DESCRIPTOR_HEAP_DESC rtv{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            D3D12_DESCRIPTOR_HEAP_DESC srv{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            bool ok = SUCCEEDED(device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtvHeap))) && SUCCEEDED(device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&srvHeap)));
            for (auto& a : allocators)
            {
                ok = ok && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
            }
            ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], nullptr, IID_PPV_ARGS(&list)));
            ok = ok && SUCCEEDED(list->Close());
            ok = ok && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
            fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!ok)
            {
                Log(L"overlay: D3D12 setup failed; overlay off");
                failed = true;
            }
            return ok;
        }

        void WaitFor(UINT64 value)
        {
            if (value && fence->GetCompletedValue() < value)
            {
                fence->SetEventOnCompletion(value, fenceEvent);
                ::WaitForSingleObject(fenceEvent, 1000);
            }
        }

        bool EnsureTexture(UINT w, UINT h)
        {
            if (texture && w == texW && h == texH)
            {
                return true;
            }
            WaitFor(fenceValue);  // nothing may still read the old ones (size changes are rare)
            Release(texture);
            for (UINT i = 0; i < kUploads; ++i)
            {
                Release(upload[i]);
                uploadPtr[i] = nullptr;
                uploadFence[i] = 0;
            }
            texReady = false;
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))))
            {
                return false;
            }
            uploadPitch = (w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
            D3D12_HEAP_PROPERTIES upHeap{D3D12_HEAP_TYPE_UPLOAD};
            D3D12_RESOURCE_DESC upDesc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UINT64(uploadPitch) * h, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
            for (UINT i = 0; i < kUploads; ++i)
            {
                if (FAILED(device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &upDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload[i]))) ||
                    FAILED(upload[i]->Map(0, nullptr, reinterpret_cast<void**>(&uploadPtr[i]))))
                {
                    Release(texture);
                    return false;
                }
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(texture, &view, srvHeap->GetCPUDescriptorHandleForHeapStart());
            texW = w;
            texH = h;
            Log(L"overlay: texture {}x{}", w, h);
            return true;
        }

        void Barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
            list->ResourceBarrier(1, &b);
        }

        void Draw(IDXGISwapChain* swapIn)
        {
            perf::Scope scope(perf::Overlay);
            ID3D12CommandQueue* queue = gameQueue.load();
            auto& link = Link::Get();
            if (failed || !queue || !link.Valid())
            {
                return;
            }
            if (reset.exchange(false))
            {
                link.ResetOverlay();
                texReady = false;
            }
            IDXGISwapChain3* swap = nullptr;
            if (FAILED(swapIn->QueryInterface(IID_PPV_ARGS(&swap))))
            {
                return;
            }
            DXGI_SWAP_CHAIN_DESC1 sd{};
            swap->GetDesc1(&sd);
            viewportW = static_cast<int>(sd.Width);
            viewportH = static_cast<int>(sd.Height);
            if (!Init(swap) || (sd.Format != psoFormat && !CreatePipelines(sd.Format)))
            {
                failed = failed || !psoMain;
                swap->Release();
                return;
            }

            // Delivery stats every 10 s (HUD smoothness): our presents vs Minecraft frames, worst gap.
            static UINT64 statPresents = 0, statFrames = 0, statStart = 0, lastFrameAt = 0, worstGap = 0;
            const UINT64 nowMs = ::GetTickCount64();
            if (!statStart)
            {
                statStart = nowMs;
            }
            ++statPresents;
            if (nowMs - statStart >= 10000)
            {
                if (show.load())
                {
                    Log(L"overlay: {} presents, {} Minecraft frames in {:.1f} s; longest gap between Minecraft frames {} ms", statPresents, statFrames,
                        (nowMs - statStart) / 1000.0, worstGap);
                }
                statPresents = statFrames = worstGap = 0;
                statStart = nowMs;
            }
            // Newest Minecraft frame into the staging buffer (the copy to the texture is recorded below).
            if (link.AcquireOverlayFrame())
            {
                ++statFrames;
                worstGap = std::max<UINT64>(worstGap, lastFrameAt ? nowMs - lastFrameAt : 0);
                lastFrameAt = nowMs;
                const auto* hdr = link.FrontHeader();
                if (hdr->width && hdr->height && hdr->width <= proto::kMaxOverlayW && hdr->height <= proto::kMaxOverlayH && EnsureTexture(hdr->width, hdr->height))
                {
                    const UINT u = uploadNext++ % kUploads;
                    WaitFor(uploadFence[u]);  // the copy out of this one was submitted kUploads frames ago
                    const auto* src = link.FrontPixels();
                    for (UINT y = 0; y < texH; ++y)
                    {
                        std::memcpy(uploadPtr[u] + std::size_t(y) * uploadPitch, src + std::size_t(y) * texW * 4, texW * 4);
                    }
                    uploadReady = u;
                    flipY = (hdr->flags & 1) != 0;
                    pendingCopy = true;
                }
            }
            if (!show.load() || !texture || (!texReady && !pendingCopy))
            {
                swap->Release();
                return;
            }

            int copiedFrom = -1;
            const UINT slot = frameSlot++ % kFrames;
            WaitFor(allocatorFence[slot]);
            allocators[slot]->Reset();
            list->Reset(allocators[slot], nullptr);

            if (pendingCopy)
            {
                if (texReady)
                {
                    Barrier(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                }
                D3D12_TEXTURE_COPY_LOCATION dst{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION src{upload[uploadReady], D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                src.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, texW, texH, 1, uploadPitch}};
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                copiedFrom = static_cast<int>(uploadReady);
                Barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                pendingCopy = false;
                texReady = true;
            }

            ID3D12Resource* backBuffer = nullptr;
            if (SUCCEEDED(swap->GetBuffer(swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer))))
            {
                const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
                device->CreateRenderTargetView(backBuffer, nullptr, rtv);
                Barrier(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

                Params p{};
                // The cursor lives in overlay pixels; scale if the overlay and back buffer differ.
                const float sx = float(sd.Width) / float(texW), sy = float(sd.Height) / float(texH);
                p.cursor[0] = cursorX.load() * sx;
                p.cursor[1] = cursorY.load() * sy;
                p.viewport[0] = float(sd.Width);
                p.viewport[1] = float(sd.Height);
                p.cursorOn = screenOpen.load() ? 1.0f : 0.0f;
                p.flipY = flipY ? 1.0f : 0.0f;
                // Around the screen centre, where Minecraft puts the crosshair (15 GUI pixels) and the
                // attack indicator under it (16 x 16 from 9 GUI pixels below the centre).
                const int scale = guiScale.load();
                const bool invert = crosshair.load() && scale > 0;
                if (invert)
                {
                    const float g = float(scale) * sx, cx = sd.Width * 0.5f, cy = sd.Height * 0.5f;
                    p.invertRect[0] = cx - 12.0f * g;
                    p.invertRect[1] = cy - 12.0f * g;
                    p.invertRect[2] = cx + 12.0f * g;
                    p.invertRect[3] = cy + 28.0f * g;
                }

                D3D12_VIEWPORT vp{0, 0, float(sd.Width), float(sd.Height), 0, 1};
                D3D12_RECT scissor{0, 0, LONG(sd.Width), LONG(sd.Height)};
                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                list->RSSetViewports(1, &vp);
                list->RSSetScissorRects(1, &scissor);
                list->SetGraphicsRootSignature(rootSig);
                list->SetDescriptorHeaps(1, &srvHeap);
                list->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
                list->SetGraphicsRoot32BitConstants(1, sizeof(Params) / 4, &p, 0);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->SetPipelineState(psoMain);
                list->DrawInstanced(3, 1, 0, 0);
                if (invert)
                {
                    list->SetPipelineState(psoInvert);
                    list->DrawInstanced(3, 1, 0, 0);
                }
                Barrier(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
                backBuffer->Release();
            }
            list->Close();
            ID3D12CommandList* lists[] = {list};
            origExecute(queue, 1, lists);
            queue->Signal(fence, ++fenceValue);
            allocatorFence[slot] = fenceValue;
            if (copiedFrom >= 0)
            {
                uploadFence[copiedFrom] = fenceValue;
            }
            swap->Release();
        }

        HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swap, UINT sync, UINT flags)
        {
            if (!(flags & DXGI_PRESENT_TEST))
            {
                Draw(swap);
            }
            return origPresent(swap, sync, flags);
        }

        HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* swap, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params)
        {
            if (!(flags & DXGI_PRESENT_TEST))
            {
                Draw(swap);
            }
            return origPresent1(swap, sync, flags, params);
        }

        // The game's graphics queue: the first DIRECT queue that submits work.
        void STDMETHODCALLTYPE HookExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
        {
            if (!gameQueue.load() && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
            {
                queue->AddRef();
                gameQueue = queue;
            }
            origExecute(queue, count, lists);
        }

        template <class Fn>
        void Patch(void** vtable, int index, void* hook, Fn& original)
        {
            DWORD old = 0;
            ::VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
            original = reinterpret_cast<Fn>(vtable[index]);
            vtable[index] = hook;
            ::VirtualProtect(&vtable[index], sizeof(void*), old, &old);
        }
    }

    void Install()
    {
        static bool done = false;
        if (done)
        {
            return;
        }
        done = true;
        auto createDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(::GetProcAddress(::LoadLibraryW(L"d3d12.dll"), "D3D12CreateDevice"));
        using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
        auto createFactory = reinterpret_cast<CreateFactoryFn>(::GetProcAddress(::LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1"));
        ID3D12Device* dev = nullptr;
        ID3D12CommandQueue* queue = nullptr;
        IDXGIFactory2* factory = nullptr;
        IDXGISwapChain1* swap = nullptr;
        HWND hwnd = ::CreateWindowExW(0, L"STATIC", L"minefactor-dummy", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
        D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
        DXGI_SWAP_CHAIN_DESC1 sd{64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD};
        const bool ok = createDevice && createFactory && hwnd && SUCCEEDED(createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) &&
                        SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && SUCCEEDED(createFactory(IID_PPV_ARGS(&factory))) &&
                        SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &swap));
        if (ok)
        {
            // Shared by every object of the same class, so this hooks the game's own queue and swap chain.
            Patch(*reinterpret_cast<void***>(swap), 8, reinterpret_cast<void*>(&HookPresent), origPresent);
            Patch(*reinterpret_cast<void***>(swap), 22, reinterpret_cast<void*>(&HookPresent1), origPresent1);
            Patch(*reinterpret_cast<void***>(queue), 10, reinterpret_cast<void*>(&HookExecute), origExecute);
            Log(L"overlay: hooked Present and ExecuteCommandLists");
        }
        else
        {
            Log(L"overlay: couldn't build a D3D12 swap chain to hook; overlay off");
        }
        Release(swap);
        Release(factory);
        Release(queue);
        Release(dev);
        if (hwnd)
        {
            ::DestroyWindow(hwnd);
        }
    }
}
