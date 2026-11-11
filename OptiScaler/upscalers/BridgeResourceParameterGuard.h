// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>

namespace OptiBridge {
// Restore the caller's exact resource pointers, including enabled null entries, on every exit.
// The parameter type is a template so a host fixture can exercise the real guard without a GPU.
template<class Parameters, class Resource, class Result, std::size_t Count>
class ResourceParameterGuard final {
    Parameters* parameters_;
    std::array<const char*, Count> names_;
    std::array<Resource*, Count> originals_;
    std::array<bool, Count> enabled_;
    Result success_;
    bool armed_=true,restored_=false;
public:
    ResourceParameterGuard(Parameters* parameters, std::array<const char*, Count> names,
        std::array<Resource*, Count> originals, std::array<bool, Count> enabled, Result success)
        :parameters_(parameters),names_(names),originals_(originals),enabled_(enabled),success_(success) {}
    ResourceParameterGuard(const ResourceParameterGuard&)=delete;
    ResourceParameterGuard& operator=(const ResourceParameterGuard&)=delete;
    bool RestoreNow() noexcept {
        if(!armed_)return restored_;
        armed_=false;restored_=parameters_!=nullptr;
        if(!parameters_)return false;
        for(std::size_t i=0;i<Count;++i)if(enabled_[i]) {
            try {
                parameters_->Set(names_[i],static_cast<void*>(originals_[i]));
                Resource* observed=nullptr;
                auto result=parameters_->Get(names_[i],&observed);
                if(result!=success_) {
                    void* generic=nullptr;result=parameters_->Get(names_[i],&generic);
                    observed=static_cast<Resource*>(generic);
                }
                restored_=result==success_ && observed==originals_[i] && restored_;
            } catch(...) {restored_=false;}
        }
        return restored_;
    }
    ~ResourceParameterGuard() noexcept {RestoreNow();}
};
}
