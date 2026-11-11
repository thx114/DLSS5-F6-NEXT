// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinSystemHookTransaction.h"

// Preserve the input API while sharing the checked system transaction implementation.
namespace GenshinInputHooks {
using GenshinSystemHooks::Enabled;
using GenshinSystemHooks::Transaction;
}
