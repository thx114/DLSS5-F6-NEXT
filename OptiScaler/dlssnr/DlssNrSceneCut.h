#pragma once

// Scene cuts the game does not flag, on DLSS-NR's game input: what to make of a cut the detector found
// (motion/SceneCut_Dx12.h, run by shaders/dlssnr/DlssNr_SceneCut.inl). No D3D here, so a host test includes it.
//
// The detector's answer arrives a few evaluates after the frame it judged (a readback, read once the GPU is past it).
// By then the game may have flagged a reset itself: a cut it flagged on the frame before, on, or after the cut frame
// counts as flagged by the game, and is not acted on. One it did not flag resets the model, in the Act mode only, and
// at most once in kQuiet evaluates (a white flash is two cuts: into the white and back out).

#include <array>
#include <cstdint>

namespace DlssNrSceneCut
{
// [DlssNr] SceneCut. Count is the default: the detector runs and the log and the menu count what it finds, while the
// picture is left as it is.
enum class Mode : uint32_t
{
    Off = 0,
    Count = 1,
    Act = 2,
};

constexpr Mode ModeFrom(uint32_t setting) { return setting == 0 ? Mode::Off : setting >= 2 ? Mode::Act : Mode::Count; }

// The execution API's clock is deliberately not a mode input. A DX12 bridge
// can keep its DXGI clock while its Vulkan game origin disables this feature.
constexpr Mode NormalizeMode(uint32_t setting, bool unsupportedVulkanOrigin)
{
    return unsupportedVulkanOrigin ? Mode::Off : ModeFrom(setting);
}

// CPU bookkeeping for shared-route optional resets, under g_nrMutex. Success
// at the model call records intent only. The post-Execute hook alone can accept
// it, after every selected model pass of one attempt was actually submitted.
// No D3D ownership or GPU-completion/visual guarantee is implied by a receipt.
class SubmissionReceipts
{
  public:
    static constexpr unsigned kPasses = 6;
    static constexpr unsigned kAttempts = 32;
    struct Token { uint64_t id = 0, epoch = 0; };

    bool Pending() const { return _pending; }
    void Request() { _pending = true; } // coalesce cuts while an earlier request waits
    void Cancel()
    {
        ++_epoch;
        _pending = false;
        for (auto& attempt : _attempts) attempt = {};
    }

    Token Begin(uint64_t evaluate, unsigned passes, bool proxy)
    {
        if (!_pending || passes == 0 || passes > kPasses || (proxy && passes != 1)) return {};
        for (auto& attempt : _attempts)
            if (attempt.token.id == 0)
            {
                attempt.token = { ++_next, _epoch };
                attempt.evaluate = evaluate;
                attempt.passes = passes;
                attempt.proxy = proxy;
                return attempt.token;
            }
        return {}; // bounded capacity: retain the request, never invent acceptance
    }

    bool Record(Token token, unsigned pass, uintptr_t commands, uintptr_t model, bool reset, bool success)
    {
        auto* attempt = Find(token);
        if (!attempt) return false;
        if (!reset || !success || commands == 0 || pass >= attempt->passes || (!attempt->proxy && model == 0))
        {
            *attempt = {}; // failed/abandoned attempt cannot be assembled from a later frame
            return false;
        }
        auto& receipt = attempt->models[pass];
        if (receipt.commands != 0) return false; // one immutable receipt per attempt/model pass
        receipt.commands = commands;
        receipt.model = model; // proxy owns its private model; proxy+pass identifies that consumer
        return true;
    }

    void End(Token token)
    {
        auto* attempt = Find(token);
        if (!attempt) return;
        for (unsigned pass = 0; pass < attempt->passes; ++pass)
            if (attempt->models[pass].commands == 0)
            {
                *attempt = {}; // shortened/abandoned chain: retry the pending request
                return;
            }
    }

    void ResetRecording(uintptr_t commands)
    {
        if (commands == 0) return;
        for (auto& attempt : _attempts)
            for (const auto& receipt : attempt.models)
                if (receipt.commands == commands && !receipt.submitted)
                {
                    attempt = {}; // revoke the entire attempt, including partial submissions
                    break;        // leave the optional request pending for a fresh attempt
                }
    }

    bool Submitted(uintptr_t commands)
    {
        if (!_pending || commands == 0) return false;
        for (auto& attempt : _attempts)
        {
            if (attempt.token.id == 0) continue;
            bool complete = true;
            for (unsigned pass = 0; pass < attempt.passes; ++pass)
            {
                auto& receipt = attempt.models[pass];
                if (receipt.commands == commands) receipt.submitted = true;
                complete = complete && receipt.commands != 0 && receipt.submitted;
            }
            if (complete)
            {
                Cancel(); // accept this request once, across all lists/attempts/callback duplicates
                return true;
            }
        }
        return false;
    }

  private:
    struct ModelReceipt { uintptr_t commands = 0, model = 0; bool submitted = false; };
    struct Attempt
    {
        Token token;
        uint64_t evaluate = 0;
        unsigned passes = 0;
        bool proxy = false;
        std::array<ModelReceipt, kPasses> models {};
    };
    Attempt* Find(Token token)
    {
        if (!_pending || token.id == 0 || token.epoch != _epoch) return nullptr;
        for (auto& attempt : _attempts)
            if (attempt.token.id == token.id && attempt.token.epoch == token.epoch) return &attempt;
        return nullptr;
    }
    std::array<Attempt, kAttempts> _attempts {};
    uint64_t _epoch = 1, _next = 0;
    bool _pending = false;
};

constexpr unsigned long long kQuiet = 6; // evaluates after a reset of ours in which no other is made

// The divergence past which a frame is a cut: Optical F5Low's (OpticalFlowDx12::Settings::sceneCutThreshold).
constexpr float kThreshold = 0.45f;

struct Outcome
{
    bool gameFlagged = false; // the game reset within a frame of the cut
    bool reset = false;       // reset the model now
};

class Policy
{
  public:
    // The game asked for a reset on this evaluate.
    void GameReset(unsigned long long evaluate) { _gameResets[_next++ % kKept] = evaluate + 1; }

    // The detector found a cut on evaluate `cut`; this is evaluate `now`.
    Outcome Found(unsigned long long cut, unsigned long long now, Mode mode)
    {
        Outcome o;
        o.gameFlagged = GameResetBetween(cut == 0 ? 0 : cut - 1, cut + 1);
        ++found;
        ++(o.gameFlagged ? flagged : silent);

        // Not when the game reset since the cut anyway, nor right after a reset of ours.
        if (mode == Mode::Act && !o.gameFlagged && !GameResetBetween(cut, now) && now >= _quietUntil)
        {
            o.reset = true;
            ++resets;
            _quietUntil = now + kQuiet;
        }

        return o;
    }

    void Forget()
    {
        for (unsigned long long& r : _gameResets)
            r = 0;
        _quietUntil = 0;
    }

    unsigned long long found = 0;   // cuts the detector found
    unsigned long long flagged = 0; // ... that the game flagged too
    unsigned long long silent = 0;  // ... that it did not
    unsigned long long resets = 0;  // reset requests made for them (Act); acceptance is tracked by each consumer

  private:
    static constexpr unsigned kKept = 16;

    bool GameResetBetween(unsigned long long from, unsigned long long to) const
    {
        for (const unsigned long long r : _gameResets)
            if (r != 0 && r - 1 >= from && r - 1 <= to)
                return true;
        return false;
    }

    unsigned long long _gameResets[kKept] = {}; // evaluate + 1 of the latest game resets; 0 = none
    unsigned _next = 0;
    unsigned long long _quietUntil = 0;
};
} // namespace DlssNrSceneCut
