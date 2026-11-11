// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace FramePipeline {
// Exact-sized, aligned CPU storage shared by a mapping transaction and its
// returned immutable snapshot. It owns no graphics resource or completion fact.
class CpuSnapshotBytes final {
    struct Free {void operator()(unsigned char* p)const noexcept {::operator delete(p,std::align_val_t{16});}};
    std::unique_ptr<unsigned char,Free> data_;
    size_t size_=0;
public:
    CpuSnapshotBytes()=default;
    explicit CpuSnapshotBytes(size_t size,bool zeroInitialize=true):
        data_(size?static_cast<unsigned char*>(::operator new(size,std::align_val_t{16})):nullptr),size_(size) {
        if(size && zeroInitialize)std::memset(data_.get(),0,size);
    }
    CpuSnapshotBytes(CpuSnapshotBytes&& other)noexcept:
        data_(std::move(other.data_)),size_(std::exchange(other.size_,0)){}
    CpuSnapshotBytes& operator=(CpuSnapshotBytes&& other)noexcept {
        if(this!=&other){data_=std::move(other.data_);size_=std::exchange(other.size_,0);}return *this;
    }
    unsigned char* data()noexcept {return data_.get();}
    const unsigned char* data()const noexcept {return data_.get();}
    size_t size()const noexcept {return size_;}
};
} // namespace FramePipeline
