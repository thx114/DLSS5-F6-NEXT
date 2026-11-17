// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <utility>

namespace PrivateGraphics
{
// Covers only synchronous calls made on behalf of an F6-owned graphics runtime.
// It is not an object identity and must not be used to classify another thread.
inline thread_local bool active = false;

class Scope final
{
  public:
    Scope() noexcept : previous_(active) { active = true; }
    ~Scope() { active = previous_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    bool previous_;
};

inline bool Active() noexcept { return active; }

template <class F> decltype(auto) Invoke(F&& operation)
{
    Scope scope;
    return std::forward<F>(operation)();
}
}
