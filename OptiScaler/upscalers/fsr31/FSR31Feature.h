#pragma once

#include "ffx_upscale.h"

#include <upscalers/IFeature.h>

inline static void FfxLogCallback(uint32_t type, const wchar_t* message)
{
    std::wstring string(message);
    LOG_DEBUG("FSR Runtime: {0}", wstring_to_string(string));
}

class FSR31Feature : public virtual IFeature
{
  private:
    double _lastFrameTime;
    static inline feature_version _version { 3, 1, 2 };

  protected:
    std::string _name = "FSR";


    virtual bool InitFSR3(const NVSDK_NGX_Parameter* InParameters) = 0;

    double GetDeltaTime();

    static inline void parse_version(const char* version_str) { _version.parse_version(version_str); }

    float _velocity = 1.0f;

  public:
    feature_version Version() override { return _version; }

    FSR31Feature(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);

    ~FSR31Feature();
};
