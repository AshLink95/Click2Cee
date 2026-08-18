#include "server.hpp"

#if defined(_WIN32) || defined(_WIN64)
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_2.h>
#include <vpl/mfx.h>
#include <wrl/client.h>

#include <winsock.h>
#include <iphlpapi.h>
#include <stdio.h>
#pragma comment(lib, "Ws2_32.lib")

using Microsoft::WRL::ComPtr;
static const int  FPS          = 60;
static const int  BITRATE_KBPS = 15000;
static const int  GOP          = FPS * 1; // frames between IDRs, not seconds
static const UINT ACQUIRE_WAIT = 100;      // ms. idle wake interval, not a latency floor
// a datagram caps at 65507 bytes, so frames are split.
static const int FRAG = 1400; // 1400 keeps each one inside the ethernet MTU

// Built once by cap_init, reused by every capture(), released by cap_end.
// Rebuilding per frame would cost ~1.5s of device and session setup, and would
// discard the reference frames that make P-frames small.
static struct {
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    ctx;
    ComPtr<IDXGIOutputDuplication> dupl;
    mfxLoader  loader = nullptr;
    mfxSession ses    = nullptr;
    std::vector<uint8_t> buf; // encoder writes annex-B bytes in here
    UINT width = 0, height = 0;
    bool ready = false;
} cap;

log_entry cap_init() {
    // 1. One D3D11 device for everything. Duplication and the encoder both hang
    //    off it, so a texture from one is directly usable by the other and never
    //    round-trips through RAM. VIDEO_SUPPORT for oneVPL, BGRA for the desktop.
    if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &cap.device, nullptr, &cap.ctx))) {
        return log_entry(false, "D3D11CreateDevice failed");
    }

    // 2. oneVPL uses this device from its own threads; D3D11 is not thread safe
    //    by default.
    ComPtr<ID3D10Multithread> mt;
    cap.ctx.As(&mt);
    mt->SetMultithreadProtected(TRUE);

    // 3. device -> adapter (gpu) -> output (monitor) -> IDXGIOutput1, which is
    //    what exposes desktop duplication.
    ComPtr<IDXGIDevice> dxgiDev;   cap.device.As(&dxgiDev);
    ComPtr<IDXGIAdapter> adapter;  dxgiDev->GetAdapter(&adapter);
    ComPtr<IDXGIOutput> output;    adapter->EnumOutputs(0, &output);
    ComPtr<IDXGIOutput1> output1;  output.As(&output1); //TODO: explore multi monitor options

    // 4. Subscribe to the desktop. One duplication per output per process.
    if (FAILED(output1->DuplicateOutput(cap.device.Get(), &cap.dupl))) {
        return log_entry(false, "DuplicateOutput failed");
    }

    // 5. ModeDesc is the real pixel size. DXGI_OUTPUT_DESC gives the DPI-scaled
    //    desktop rect, which would not match the texture we receive.
    DXGI_OUTDUPL_DESC dd;
    cap.dupl->GetDesc(&dd);
    cap.width  = dd.ModeDesc.Width;
    cap.height = dd.ModeDesc.Height;

    // 6. Load the dispatcher and filter for what we want: hardware, D3D11, and
    //    our own device. MFXVideoCORE_SetHandle is the documented way to pass a
    //    device but this runtime rejects it, having already made its own by the
    //    time a session exists. As a loader property we get in first.
    cap.loader = MFXLoad();
    auto set_u32 = [&](const char* name, mfxU32 value) {
        mfxVariant var{};
        var.Type = MFX_VARIANT_TYPE_U32;
        var.Data.U32 = value;
        MFXSetConfigFilterProperty(MFXCreateConfig(cap.loader), (const mfxU8*)name, var);
    };
    set_u32("mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE);
    set_u32("mfxImplDescription.AccelerationMode", MFX_ACCEL_MODE_VIA_D3D11);
    set_u32("mfxHandleType", MFX_HANDLE_D3D11_DEVICE);
    mfxVariant hdl_var{};
    hdl_var.Type = MFX_VARIANT_TYPE_PTR;
    hdl_var.Data.Ptr = (mfxHDL)cap.device.Get();
    MFXSetConfigFilterProperty(MFXCreateConfig(cap.loader), (const mfxU8*)"mfxHDL", hdl_var);

    MFXCreateSession(cap.loader, 0, &cap.ses);

    // 7. Encoder, fed BGRA straight from duplication. A VPP colour conversion
    //    pass would cost ~6ms a frame; the encoder does it internally for free.
    //    Surfaces are 16-aligned, Crop* is the real picture inside that.
    //    GopRefDist 1 = no B-frames, so nothing is reordered. AsyncDepth 1 = no
    //    second frame queued behind the first.
    mfxVideoParam enc{};
    enc.mfx.CodecId                   = MFX_CODEC_AVC;
    enc.mfx.TargetUsage               = MFX_TARGETUSAGE_BEST_SPEED;
    enc.mfx.RateControlMethod         = MFX_RATECONTROL_CBR;
    enc.mfx.TargetKbps                = BITRATE_KBPS;
    enc.mfx.GopPicSize                = GOP;
    enc.mfx.GopRefDist                = 1;
    enc.mfx.NumRefFrame               = 1;
    enc.mfx.FrameInfo.FourCC          = MFX_FOURCC_RGB4;
    enc.mfx.FrameInfo.ChromaFormat    = MFX_CHROMAFORMAT_YUV444;
    enc.mfx.FrameInfo.PicStruct       = MFX_PICSTRUCT_PROGRESSIVE;
    enc.mfx.FrameInfo.CropW           = cap.width;
    enc.mfx.FrameInfo.CropH           = cap.height;
    enc.mfx.FrameInfo.Width           = (cap.width  + 15) & ~15;
    enc.mfx.FrameInfo.Height          = (cap.height + 15) & ~15;
    enc.mfx.FrameInfo.FrameRateExtN   = FPS;
    enc.mfx.FrameInfo.FrameRateExtD   = 1;
    enc.IOPattern                     = MFX_IOPATTERN_IN_VIDEO_MEMORY;
    enc.AsyncDepth                    = 1;

    // 8. Latency knobs. HRD off stops the encoder holding bits back to fit a
    //    virtual buffer model, LowDelayBRC keeps the rate honest without it,
    //    MaxDecFrameBuffering 1 tells the decoder never to reorder, RepeatPPS
    //    lets a client resync at the next IDR with no side channel.
    mfxExtCodingOption co{};
    co.Header.BufferId      = MFX_EXTBUFF_CODING_OPTION;
    co.Header.BufferSz      = sizeof(co);
    co.NalHrdConformance    = MFX_CODINGOPTION_OFF;
    co.PicTimingSEI         = MFX_CODINGOPTION_OFF;
    co.MaxDecFrameBuffering = 1;
    mfxExtCodingOption2 co2{};
    co2.Header.BufferId     = MFX_EXTBUFF_CODING_OPTION2;
    co2.Header.BufferSz     = sizeof(co2);
    co2.RepeatPPS           = MFX_CODINGOPTION_ON;
    mfxExtCodingOption3 co3{};
    co3.Header.BufferId     = MFX_EXTBUFF_CODING_OPTION3;
    co3.Header.BufferSz     = sizeof(co3);
    co3.LowDelayBRC         = MFX_CODINGOPTION_ON;
    mfxExtBuffer* ext[]     = {&co.Header, &co2.Header, &co3.Header};
    enc.ExtParam            = ext;
    enc.NumExtParam         = 3;

    if (MFXVideoENCODE_Init(cap.ses, &enc) < MFX_ERR_NONE) {
        return log_entry(false,  "ENCODE_Init failed");
    }

    // 9. One bitstream buffer for the life of the stream.
    cap.buf.resize((size_t)cap.width * cap.height * 2);

    cap.ready = true;
    return log_entry(true, std::format("capture ready, {0}x{1}", cap.width, cap.height));
}

/// grab one frame and encode it. Waits until the desktop actually draws.
bool capture(std::vector<uint8_t>& out, std::string& msg) {
    out.clear();
    if (!cap.ready) {
        msg = "capture not initialised";
        return false;
    }

    // 1. Wait for the desktop to present. Returns the instant the compositor
    //    draws, so this is the lowest latency wakeup there is. A timeout means
    //    nothing changed and a zero present time means only the cursor moved,
    //    so in both cases keep waiting until there is a real image.
    ComPtr<IDXGIResource> res;
    DXGI_OUTDUPL_FRAME_INFO info{};
    HRESULT hr = DXGI_ERROR_WAIT_TIMEOUT;
    while (hr == DXGI_ERROR_WAIT_TIMEOUT || (SUCCEEDED(hr) && info.LastPresentTime.QuadPart == 0)) {
        if (SUCCEEDED(hr)) cap.dupl->ReleaseFrame();
        hr = cap.dupl->AcquireNextFrame(ACQUIRE_WAIT, &info, res.ReleaseAndGetAddressOf());
    }
    // 2. Mode change, UAC secure desktop or a fullscreen switch killed the
    //    duplication. cap_init has to run again before the next frame.
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        cap.ready = false;
        msg = "duplication lost";
        return false;
    }
    if (FAILED(hr)) {
        msg = "AcquireNextFrame failed";
        return false;
    }

    // 3. What we acquired is a D3D11 texture already in VRAM.
    ComPtr<ID3D11Texture2D> gpuTex; res.As(&gpuTex);

    // 4. Borrow an encoder-owned BGRA surface and unwrap its ID3D11Texture2D.
    //    The runtime is on our device, so it and gpuTex are siblings.
    mfxFrameSurface1* rgb;
    mfxHDL hdl;
    mfxResourceType rt;
    MFXMemory_GetSurfaceForEncode(cap.ses, &rgb);
    rgb->FrameInterface->GetNativeHandle(rgb, &hdl, &rt);

    // 5. The one copy in the pipeline, VRAM to VRAM. CopySubresourceRegion and
    //    not CopyResource because the encoder surface is padded to the 16
    //    alignment. Release straight after: holding it blocks the next present.
    D3D11_BOX box{0, 0, 0, cap.width, cap.height, 1};
    cap.ctx->CopySubresourceRegion((ID3D11Texture2D*)hdl, 0, 0, 0, 0, gpuTex.Get(), 0, &box);
    cap.ctx->Flush();
    cap.dupl->ReleaseFrame();

    // 6. Encode. MORE_DATA means the encoder took the frame but has nothing out
    //    yet, which is normal mid-stream.
    mfxBitstream bs{};
    bs.Data      = cap.buf.data();
    bs.MaxLength = (mfxU32)cap.buf.size();
    mfxSyncPoint sp = nullptr;
    // No per frame control: GopPicSize already puts an IDR every GOP frames,
    // which is what a client joining late or recovering from a lost frame waits
    // for. P-frames in between are what keep the picture sharp inside the
    // bitrate.
    mfxEncodeCtrl* pctrl = nullptr;
    mfxStatus sts = MFXVideoENCODE_EncodeFrameAsync(cap.ses, pctrl, rgb, &bs, &sp);
    rgb->FrameInterface->Release(rgb);
    if (sts < MFX_ERR_NONE || !sp) {
        msg = sts == MFX_ERR_MORE_DATA ? "buffered" : "EncodeFrameAsync failed";
        return sts == MFX_ERR_MORE_DATA;
    }

    // 7. Wait for the GPU, then take the bytes. First and only time this data
    //    touches the CPU.
    MFXVideoCORE_SyncOperation(cap.ses, sp, 10000);
    out.assign(bs.Data + bs.DataOffset, bs.Data + bs.DataOffset + bs.DataLength);
    msg = std::format("{0} bytes", out.size());
    return true;
}

log_entry cap_end() {
    // reverse order: encoder, session, dispatcher. D3D11 objects are ComPtrs
    // and release themselves.
    if (cap.ses) {
        MFXVideoENCODE_Close(cap.ses);
        MFXClose(cap.ses);
        cap.ses = nullptr;
    }
    if (cap.loader) {
        MFXUnload(cap.loader);
        cap.loader = nullptr;
    }
    cap.dupl.Reset();
    cap.ctx.Reset();
    cap.device.Reset();
    cap.buf.clear();
    cap.ready = false;
    return log_entry(true, "capture closed");
}

// Opened once, reused every frame. Per frame we only touch net.sock.
static struct {
    SOCKET   sock = INVALID_SOCKET;
    uint32_t seq  = 0; // frame counter, for fragment headers
} net;

log_entry init_network_snd(std::string addr, uint16_t port, std::string caddr, uint16_t port_play) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return log_entry(false, "WSAStartup failed");
    }

    net.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (net.sock == INVALID_SOCKET) {
        return log_entry(false, std::format("socket failed ({})", WSAGetLastError()));
    }

    // We own this address and port. Nothing else may take it, so a clash is
    // fatal rather than silent.
    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_port        = htons(port);
    local.sin_addr.s_addr = inet_addr(addr.c_str());
    if (local.sin_addr.s_addr == INADDR_NONE) {
        closesocket(net.sock);
        net.sock = INVALID_SOCKET;
        return log_entry(false, "bad address");
    }
    if (bind(net.sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        closesocket(net.sock);
        net.sock = INVALID_SOCKET;
        return log_entry(false, std::format("bind failed ({})", WSAGetLastError()));
    }

    // The player sits at a known address and port, so we aim at it up front and
    // never have to learn anything at runtime. udp connect is not a handshake,
    // it just pins the destination: send() per fragment, no route lookup, and a
    // client that restarts comes back to the same place with nothing to redo.
    sockaddr_in peer{};
    peer.sin_family      = AF_INET;
    peer.sin_port        = htons(port_play);
    peer.sin_addr.s_addr = inet_addr(caddr.c_str());
    if (peer.sin_addr.s_addr == INADDR_NONE) {
        closesocket(net.sock);
        net.sock = INVALID_SOCKET;
        return log_entry(false, "bad client address");
    }
    if (connect(net.sock, (sockaddr*)&peer, sizeof(peer)) == SOCKET_ERROR) {
        closesocket(net.sock);
        net.sock = INVALID_SOCKET;
        return log_entry(false, std::format("connect failed ({})", WSAGetLastError()));
    }

    // a keyframe is ~150KB of fragments going out back to back, well past the
    // 64KB default
    int sndbuf = 4 << 20;
    setsockopt(net.sock, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));

    // non blocking last, so nothing above had to deal with WOULDBLOCK. From here
    // a full send buffer drops the frame instead of stalling the capture loop.
    u_long nonblock = 1;
    ioctlsocket(net.sock, FIONBIO, &nonblock);

    return log_entry(true, std::format("udp {}:{} to {}:{}", addr, port, caddr, port_play));
}

bool send_frame(std::vector<uint8_t>& out) {
    if (net.sock == INVALID_SOCKET) return false;

    uint32_t seq   = net.seq++;
    uint16_t count = (uint16_t)((out.size() + FRAG - 1) / FRAG);
    uint8_t  pkt[8 + FRAG];

    // seq, index and count are all the receiver needs to reassemble a frame and
    // throw away one that came in short
    memcpy(pkt, &(seq = htonl(seq)), 4);
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t idx_n = htons(i), cnt_n = htons(count);
        memcpy(pkt + 4, &idx_n, 2);
        memcpy(pkt + 6, &cnt_n, 2);

        size_t off  = (size_t)i * FRAG;
        size_t left = out.size() - off;
        int    n    = (int)(left < FRAG ? left : FRAG);
        memcpy(pkt + 8, out.data() + off, n);

        // socket is non blocking, so this queues the datagram and returns. A
        // full buffer drops it rather than stalling us, and the frame is lost.
        // CONNRESET is the icmp unreachable from a client that went away. It
        // comes back when it does, on the same port, so this is not fatal.
        if (send(net.sock, (const char*)pkt, 8 + n, 0) == SOCKET_ERROR)
            return WSAGetLastError() == WSAECONNRESET;
    }
    return true;
}

log_entry cleanup_network_snd() {
    if (net.sock != INVALID_SOCKET) closesocket(net.sock);
    net.sock = INVALID_SOCKET;
    WSACleanup();
    return log_entry(true, "WSACleanup succeeded");
}

#elif defined(__linux__)
//TODO: for linux, capture with pipewire
log_entry cap_init(std::string& msg) {
    return log_entry(false, "cap_init failed");
}
bool capture(std::vector<uint8_t>& out, std::string& msg) {
    return false;
}
log_entry cap_end(std::string& msg) {
    return log_entry(false, "cap_end failed");
}

log_entry init_network_snd(std::string addr, uint16_t port, std::string caddr, uint16_t cport) {
    return log_entry(true, "init_network_snd succeeded");
}
bool send_frame(std::vector<uint8_t>& out) {
    return false;
}
log_entry cleanup_network_snd() { return log_entry(true, "cleanup_network_snd succeeded"); }
#endif


/// one turn of the send loop: grab a frame, encode it, put it on the wire.
/// cap_init must have run first; the server owns that.
log_entry snd() {
    std::vector<uint8_t> out;
    std::string message;

    if (!capture(out, message)) return log_entry(false, message);
    if (!send_frame(out)) return log_entry(false, "failed to send "+message);

    return log_entry(true, "sent "+message);
}
