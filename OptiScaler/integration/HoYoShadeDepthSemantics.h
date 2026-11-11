#pragma once
#include <cstdint>

namespace reshade::api { struct effect_runtime; }
namespace HoYoShade::Selection { struct Source; }
namespace HoYoShade::Depth { struct Access; }
namespace HoYoShade::Depth::Semantics
{
inline constexpr const char* Reversed = "RESHADE_DEPTH_INPUT_IS_REVERSED";
inline constexpr const char* UpsideDown = "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN";

enum class Outcome { ColorOnly, Pending, Ready };
struct Result {
    Outcome outcome = Outcome::ColorOnly;
    bool invalidateSelection = false;
};
struct Key {
    uintptr_t runtime = 0;
    uint64_t runtimeGeneration = 0, producer = 0, producerGeneration = 0, epoch = 0;
    uint64_t selectionSnapshot = 0, selectionGraph = 0;
    bool valid = false, reversed = false, yFlipped = false;
    bool operator==(const Key&) const = default;
    bool SameRuntime(const Key& b) const noexcept {
        return runtime == b.runtime && runtimeGeneration == b.runtimeGeneration;
    }
    bool SameProjection(const Key& b) const noexcept {
        // Compile inputs only. Producer/frame identity still changes the full
        // key and is authenticated by the caller for every real depth access.
        return SameRuntime(b) && selectionSnapshot == b.selectionSnapshot &&
            selectionGraph == b.selectionGraph && reversed == b.reversed && yFlipped == b.yFlipped;
    }
};

// CPU scheduling only. A serial is supplied by the ACTUAL core reload callback,
// not by the setter, getter, submission counter, elapsed frames or this class.
// Port::Matches reads only the two named definitions (global + selected effects).
// Mutations happen exclusively in the caller's idle private-runtime interval.
class Controller final
{
    Key key_{};
    bool haveKey_ = false, pending_ = false, mutationFailed_ = false, compiled_ = false;
    uint64_t baseline_ = 0, seenReload_ = 0, observedReload_ = 0;
public:
    template<class Port>
    Result Update(Port& port, const Key& wanted) noexcept {
        Result out;
        bool mutating = false;
        try {
            if (haveKey_ && !key_.SameRuntime(wanted)) {
                *this = Controller{};
                out.invalidateSelection = true;
            }
            const uint64_t serial = port.ReloadSerial();
            bool checkedKey = false;
            // The callback invalidates handles even while private preset
            // initialization is incomplete. A guard refusal is retryable.
            if (haveKey_ && serial != observedReload_) {
                observedReload_ = serial;
                out.invalidateSelection = true;
            }
            if (pending_) {
                // Finish the preceding request before scheduling a different
                // direction/epoch. Do not chase changing input every frame.
                if (serial <= baseline_) { out.outcome = Outcome::Pending; return out; }
                if (!port.PrivateIdle()) { out.outcome = Outcome::Pending; return out; }
                // Read before committing completion: transient read/guard
                // failure cannot consume the real callback or poison this key.
                const bool matches = !mutationFailed_ && port.Matches(key_);
                pending_ = false;
                seenReload_ = serial;
                out.invalidateSelection = true;
                compiled_ = matches;
                checkedKey = true;
            }
            if (!wanted.valid) return out; // no capture/compile cost without real depth
            if (!haveKey_ || !(key_ == wanted)) {
                const bool priorCompiled = compiled_ && key_.SameProjection(wanted);
                // Acquire can precede the private core's preset warmup.
                // Keep the preceding witness/key until this guard really
                // admits projection. Pending lets the caller advance the core.
                if (!port.PrivateIdle()) { out.outcome = Outcome::Pending; return out; }
                const bool match = port.Matches(wanted);
                key_ = wanted; haveKey_ = true; mutationFailed_ = false;
                observedReload_ = serial;
                // A later reload notification invalidates handles above, but
                // is not a request to compile the same projection AGAIN. A new
                // producer generation reaches this branch before the unchanged-
                // key path below can consume that serial. Requiring equality
                // here requeued reload after each real completion and prevented
                // the private core from reaching a stable technique graph.
                // Retain only an already-witnessed, unchanged compile scope
                // whose definitions still read back. Selection::Apply must
                // independently find/enable the actual loaded techniques.
                if (match && priorCompiled) {
                    seenReload_ = serial;
                    compiled_ = true; out.outcome = Outcome::Ready; return out;
                }
                compiled_ = false;
                // Mark the in-progress mutation BEFORE any SDK setter: a
                // partial setter failure may already have queued recompiles.
                baseline_ = serial;
                pending_ = true;
                mutating = true;
                if (!match) port.Project(key_);
                // Queue once even if values already match on first contact:
                // definition readback is not proof of compiled effect code.
                out.invalidateSelection = true;
                port.QueueReload();
                baseline_ = port.ReloadSerial(); // AFTER setters and queue call
                pending_ = true;
                mutating = false;
                out.outcome = Outcome::Pending;
                return out;
            }
            if (mutationFailed_) return out; // no per-frame setter/retry loop
            if (!checkedKey && (serial != seenReload_ || !compiled_)) {
                if (!port.PrivateIdle()) { out.outcome = Outcome::Pending; return out; }
                const bool matches = port.Matches(key_);
                if (serial != seenReload_) out.invalidateSelection = true;
                seenReload_ = serial;
                compiled_ = matches;
                // A completed reload with unavailable definitions can be
                // re-read; this path NEVER queues another compilation.
            }
            if (compiled_) out.outcome = Outcome::Ready;
            return out;
        } catch (...) {
            out.invalidateSelection = true;
            if (mutating) {
                // Only an uncertain setter/queue mutation is sticky. Keep
                // driving the core until its actual reload callback.
                mutationFailed_ = true; compiled_ = false;
            }
            // Read/ordinary state failures retry the same key without losing
            // the preceding compile witness or inventing core readiness.
            out.outcome = Outcome::Pending;
            return out;
        }
    }
};
struct State { Controller controller; };
using ReloadCounter = uint64_t (*)(reshade::api::effect_runtime*) noexcept;

// Caller holds the genuine exclusive owned lease AFTER its preceding GPU fence
// completes and BEFORE any effect work is recorded. The current preset must be
// the Selection-owned private copy. No natural runtime is a legal target.
// Ready also requires Selection::Apply to rebuild/find the actual compiled graph
// in the caller; neither this module nor a reload event proves GPU sampling.
Result Project(State&, reshade::api::effect_runtime*, uint64_t runtimeGeneration,
    Selection::Source&, const Access* realInput, ReloadCounter) noexcept;
}
