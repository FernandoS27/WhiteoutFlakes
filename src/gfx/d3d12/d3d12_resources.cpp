#include "d3d12_resources.h"

namespace whiteout::flakes::gfx::d3d12 {

bool UploadRing::Init(ID3D12Device* device, u64 sizeBytes) {
    capacity_ = sizeBytes;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = capacity_;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&resource_));
    if (FAILED(hr))
        return false;

    D3D12_RANGE readRange{0, 0};
    hr = resource_->Map(0, &readRange, reinterpret_cast<void**>(&mapped_));
    if (FAILED(hr)) {
        SafeRelease(resource_);
        return false;
    }

    gpuBase_ = resource_->GetGPUVirtualAddress();
    head_ = 0;
    tail_ = 0;
    return true;
}

void UploadRing::Release() {
    if (resource_) {
        resource_->Unmap(0, nullptr);
        SafeRelease(resource_);
    }
    mapped_ = nullptr;
    gpuBase_ = 0;
    capacity_ = 0;
    head_ = 0;
    tail_ = 0;
    retiredQueue_.clear();
}

void UploadRing::BeginFrame(u64 nextFenceValue) {
    currentFrameFence_ = nextFenceValue;
}

void UploadRing::EndFrame(u64 signaledFenceValue) {
    retiredQueue_.push_back({signaledFenceValue, head_});
}

void UploadRing::Retire(u64 completedFenceValue) {

    while (!retiredQueue_.empty() && retiredQueue_.front().fenceValue <= completedFenceValue) {
        tail_ = retiredQueue_.front().head;
        retiredQueue_.erase(retiredQueue_.begin());
    }
}

UploadRing::Allocation UploadRing::Allocate(u64 size, u64 alignment) {
    Allocation out{};
    if (size == 0 || size > capacity_)
        return out;

    u64 pos = head_ % capacity_;
    u64 aligned = AlignUp(pos, alignment);
    u64 pad = aligned - pos;

    if (aligned + size > capacity_) {
        pad += (capacity_ - aligned);
        aligned = 0;
    }

    out.cpu = mapped_ + aligned;
    out.gpu = gpuBase_ + aligned;
    out.resource = resource_;
    out.offset = aligned;
    head_ += pad + size;
    return out;
}

bool DescriptorHeapRing::Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                              u32 totalSize) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = type;
    hd.NumDescriptors = totalSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_));
    if (FAILED(hr))
        return false;

    stride_ = device->GetDescriptorHandleIncrementSize(type);
    capacity_ = totalSize;
    head_ = 0;
    tail_ = 0;
    cpuBase_ = heap_->GetCPUDescriptorHandleForHeapStart();
    gpuBase_ = heap_->GetGPUDescriptorHandleForHeapStart();
    return true;
}

void DescriptorHeapRing::Release() {
    SafeRelease(heap_);
    stride_ = 0;
    capacity_ = 0;
    head_ = 0;
    tail_ = 0;
    cpuBase_ = {0};
    gpuBase_ = {0};
    retiredQueue_.clear();
}

void DescriptorHeapRing::BeginFrame(u64 nextFenceValue) {
    currentFrameFence_ = nextFenceValue;
}

void DescriptorHeapRing::EndFrame(u64 signaledFenceValue) {
    retiredQueue_.push_back({signaledFenceValue, head_});
}

void DescriptorHeapRing::Retire(u64 completedFenceValue) {
    while (!retiredQueue_.empty() && retiredQueue_.front().fenceValue <= completedFenceValue) {
        tail_ = retiredQueue_.front().head;
        retiredQueue_.erase(retiredQueue_.begin());
    }
}

DescriptorHeapRing::Slice DescriptorHeapRing::Allocate(u32 count) {
    Slice s{};
    if (count == 0 || count > capacity_)
        return s;

    u32 pos = head_ % capacity_;
    u32 pad = 0;
    if (pos + count > capacity_) {
        pad = capacity_ - pos;
        pos = 0;
    }

    s.cpu.ptr = cpuBase_.ptr + static_cast<SIZE_T>(pos) * stride_;
    s.gpu.ptr = gpuBase_.ptr + static_cast<UINT64>(pos) * stride_;
    head_ += pad + count;
    return s;
}

bool CpuDescriptorPool::Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = type;
    hd.NumDescriptors = capacity;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_));
    if (FAILED(hr))
        return false;

    stride_ = device->GetDescriptorHandleIncrementSize(type);
    capacity_ = capacity;
    head_ = 0;
    cpuBase_ = heap_->GetCPUDescriptorHandleForHeapStart();
    return true;
}

void CpuDescriptorPool::Release() {
    SafeRelease(heap_);
    stride_ = 0;
    capacity_ = 0;
    head_ = 0;
    cpuBase_ = {0};
    freeList_.clear();
}

D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptorPool::Allocate() {
    u32 idx;
    if (!freeList_.empty()) {
        idx = freeList_.back();
        freeList_.pop_back();
    } else {
        assert(head_ < capacity_ && "CpuDescriptorPool exhausted");
        idx = head_++;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE out{};
    out.ptr = cpuBase_.ptr + static_cast<SIZE_T>(idx) * stride_;
    return out;
}

void CpuDescriptorPool::Free(D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (!h.ptr || !cpuBase_.ptr)
        return;
    SIZE_T delta = h.ptr - cpuBase_.ptr;
    u32 idx = static_cast<u32>(delta / stride_);
    if (idx < head_)
        freeList_.push_back(idx);
}

} // namespace whiteout::flakes::gfx::d3d12
