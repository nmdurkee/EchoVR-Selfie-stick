// capture_gpu.h - records "backbuffer -> READBACK buffer" on a caller-supplied D3D12 queue.
// Never creates a device (it uses the queue's own device), never waits on the GPU: reuse of the
// allocator / readback buffer is gated on the fence (Idle()), and the caller waits on Fence()
// from a worker thread. Used by selfie_capture.cpp on Echo's queue and by the WARP tests.
#pragma once
#include "capture_util.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace capture {

class ReadbackCopier {
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
public:
    enum Result { Submitted, Busy, Unsupported, Failed };
    const char* error = nullptr;      // last reason (static string)

    ID3D12Fence* Fence() const { return fence_.Get(); }
    UINT64 Serial() const { return serial_; }
    UINT64 BufferBytes() const { return bufferBytes_; }
    int BufferAllocations() const { return allocations_; }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    DXGI_FORMAT Format() const { return format_; }
    // True when no copy of ours is still executing on the GPU (safe to reuse everything).
    bool Idle() const { return !fence_ || fence_->GetCompletedValue() >= serial_; }

    // Records and submits on `queue`: `stateBefore` -> COPY_SOURCE, CopyTextureRegion into the
    // readback buffer, COPY_SOURCE -> `stateBefore`, ExecuteCommandLists, Signal(fence, ++serial).
    Result Record(ID3D12CommandQueue* queue, ID3D12Resource* source,
                  D3D12_RESOURCE_STATES stateBefore = D3D12_RESOURCE_STATE_PRESENT) {
        if (broken_) return fail(Failed, error ? error : "copier-broken");
        if (!queue || !source) return fail(Unsupported, "null-queue-or-source");
        if (!Idle()) return fail(Busy, "previous-copy-still-on-gpu");
        const D3D12_RESOURCE_DESC desc = source->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return fail(Unsupported, "backbuffer-not-texture2d");
        if (desc.SampleDesc.Count != 1) return fail(Unsupported, "backbuffer-multisampled");
        if (desc.DepthOrArraySize != 1) return fail(Unsupported, "backbuffer-array");
        if (desc.Width < 1 || desc.Width > 16384 || desc.Height < 1 || desc.Height > 16384) return fail(Unsupported, "backbuffer-size-out-of-range");
        if (Classify(desc.Format) == PixelKind::Unsupported) return fail(Unsupported, "backbuffer-format-unsupported");

        Ptr<ID3D12Device> device;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) return fail(Failed, "queue-get-device-failed");
        if (!device_) {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_))) ||
                FAILED(list_->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
                allocator_.Reset(); list_.Reset(); fence_.Reset();
                return fail(Failed, "copier-init-failed");
            }
            device_ = device;
        } else if (device.Get() != device_.Get()) {
            return fail(Unsupported, "queue-device-changed");
        }
        {
            Ptr<ID3D12Device> sourceDevice;
            if (FAILED(source->GetDevice(IID_PPV_ARGS(&sourceDevice))) || sourceDevice.Get() != device_.Get())
                return fail(Unsupported, "backbuffer-on-other-device");
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT rows = 0;
        UINT64 rowBytes = 0, total = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        if (!total || total == UINT64(-1) || rows != desc.Height || fp.Footprint.RowPitch < rowBytes ||
            rowBytes < UINT64(desc.Width) * BytesPerPixel(Classify(desc.Format)))
            return fail(Unsupported, "footprint-invalid");

        // (Re)allocate lazily: grow when too small, shrink when the backbuffer got much smaller.
        if (!readback_ || total > bufferBytes_ || total * 2 < bufferBytes_) {
            readback_.Reset();
            bufferBytes_ = 0;
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&readback_))))
                return fail(Failed, "readback-alloc-failed");
            bufferBytes_ = total;
            allocations_++;
        }

        if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr))) return fail(Failed, "list-reset-failed");
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = source;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = stateBefore;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list_->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = readback_.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = source;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = stateBefore;
        list_->ResourceBarrier(1, &b);
        if (FAILED(list_->Close())) return fail(Failed, "list-close-failed");
        ID3D12CommandList* lists[] = {list_.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence_.Get(), serial_ + 1))) {
            // The list was submitted but cannot be tracked: never touch these objects again.
            broken_ = true;
            return fail(Failed, "fence-signal-failed");
        }
        serial_++;
        footprint_ = fp;
        width_ = uint32_t(desc.Width);
        height_ = desc.Height;
        format_ = desc.Format;
        error = nullptr;
        return Submitted;
    }

    // Worker thread, after Fence() reached Serial(): map, convert to opaque BGRA8, unmap.
    bool ReadPixels(std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height) {
        if (!readback_ || !serial_) { error = "nothing-recorded"; return false; }
        if (!Idle()) { error = "copy-not-finished"; return false; }
        const UINT64 used = footprint_.Offset + UINT64(footprint_.Footprint.RowPitch) * height_;
        D3D12_RANGE read = {0, SIZE_T(used < bufferBytes_ ? used : bufferBytes_)};
        void* mapped = nullptr;
        if (FAILED(readback_->Map(0, &read, &mapped)) || !mapped) { error = "readback-map-failed"; return false; }
        const char* why = nullptr;
        const bool ok = ConvertToBgra(format_, static_cast<const uint8_t*>(mapped) + footprint_.Offset,
                                      size_t(bufferBytes_ - footprint_.Offset), footprint_.Footprint.RowPitch, width_, height_, bgra, &why);
        D3D12_RANGE none = {0, 0};
        readback_->Unmap(0, &none);
        if (!ok) { error = why ? why : "convert-failed"; return false; }
        width = width_;
        height = height_;
        return true;
    }

private:
    Result fail(Result r, const char* why) { error = why; return r; }
    Ptr<ID3D12Device> device_;
    Ptr<ID3D12CommandAllocator> allocator_;
    Ptr<ID3D12GraphicsCommandList> list_;
    Ptr<ID3D12Fence> fence_;
    Ptr<ID3D12Resource> readback_;
    UINT64 serial_ = 0, bufferBytes_ = 0;
    int allocations_ = 0;
    bool broken_ = false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_ = {};
    uint32_t width_ = 0, height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

}  // namespace capture
