#pragma once
#include "SysUtils.h"

class DxgiHooks
{
  private:
    static inline std::mutex hookMutex;

  public:
    static void Hook();
    static bool PrepareD3D11On12Boundary();
};
