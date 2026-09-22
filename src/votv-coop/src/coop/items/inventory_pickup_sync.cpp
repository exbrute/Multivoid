// coop/items/inventory_pickup_sync.cpp -- see coop/items/inventory_pickup_sync.h.

#include "coop/items/inventory_pickup_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_save_data.h"
#include "coop/props/prop_sound.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

namespace coop::inventory_pickup_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace SG = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

// Resolution / registration latches (Install retries throttled until done).
void*   g_playSound2DFn = nullptr;
void*   g_inventoryCue  = nullptr;  // the cue OBJECT -- the observer predicate is a pointer compare
int32_t g_offSound = -1;
int32_t g_offPitch = -1;
int32_t g_offWco   = -1;
bool    g_observerRegistered = false;
uint32_t g_resolveN = 0;  // ~1 Hz throttle on the resolve walk (Install runs per pump tick)

// A successful putObjectInventory2 captures the actor's getData into the personal store and then
// destroys that actor. The destroy alone used to cross the wire, leaving the host's mutable record
// stale; a second player could therefore pocket a full mirror and later drop full food. Watch the
// body rather than ProcessEvent so Blueprint-local calls are visible. Publishing on an attempt
// that the inventory later refuses is harmless: it only refreshes this same keyed prop.
constexpr const wchar_t* kPocketVerb = L"putObjectInventory2";
constexpr int kPocketTag = 0x504F434B;  // 'POCK'
bool g_pocketWatchRegistered = false;

SG::Verdict OnPocketPre(const SG::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !call.locals || !call.function) return SG::Verdict::Run;
    void* local = coop::players::Registry::Get().Local();
    if (!local || call.object != local) return SG::Verdict::Run;

    static void* s_fn = nullptr;
    static int32_t s_inputOff = -1;
    if (s_fn != call.function) {
        s_fn = call.function;
        s_inputOff = R::FindParamOffset(call.function, L"InputPin");
        if (s_inputOff < 0)
            UE_LOGW("inventory_pickup: %ls InputPin did not resolve -- mutable pocket state "
                    "cannot be published", kPocketVerb);
    }
    if (s_inputOff < 0) return SG::Verdict::Run;
    void* actor = *reinterpret_cast<void* const*>(call.locals + s_inputOff);
    if (!actor || !R::IsLive(actor) || !ue_wrap::prop::IsKeyedInteractable(actor))
        return SG::Verdict::Run;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(actor);
    if (key.empty() || key == L"None") return SG::Verdict::Run;
    if (coop::prop_save_data::Publish(s, actor, key))
        UE_LOGI("inventory_pickup: published mutable state before pocketing key='%ls' cls='%ls'",
                key.c_str(), R::ClassNameOf(actor).c_str());
    return SG::Verdict::Run;
}

// POST observer on UGameplayStatics::PlaySound2D, which dispatches at human-event rate game-wide
// (UI clicks, 2D cues); the body is three cached-offset reads and compares, exiting on the first
// mismatch. The predicate:
//   Sound == inventory_Cue    pointer compare against the resolved cue object
//   1.05 < pitch < 1.2        the collect plays 1.1; the same cue on a climb plays 0.9
//   WorldContext == LOCAL     the collector. A puppet has no input stack and can never dispatch
//                             this; and the gamemode's own collect helper plays the same cue at
//                             the same pitch with ITSELF as the context, so this test is what
//                             makes the cue-and-pitch pair unambiguous.
// putObjectInventory2 plays the cue once, on its single success path, so one dispatch that
// passes all three tests is one collect.
void OnPlaySound2DPost(void* /*self*/, void* /*function*/, void* params) {
    if (!GT::IsGameThread() || !params) return;
    if (!g_inventoryCue) return;  // cue not resolved yet -> predicate undecidable
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;

    const auto* p = static_cast<const uint8_t*>(params);
    void* sound = *reinterpret_cast<void* const*>(p + g_offSound);
    if (sound != g_inventoryCue) return;
    const float pitch = *reinterpret_cast<const float*>(p + g_offPitch);
    if (pitch <= 1.05f || pitch >= 1.2f) return;  // rejects the climb play at 0.9
    void* wco = *reinterpret_cast<void* const*>(p + g_offWco);
    void* local = coop::players::Registry::Get().Local();
    if (!local || wco != local) return;

    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(local);
    if (!std::isfinite(loc.X) || !std::isfinite(loc.Y) || !std::isfinite(loc.Z)) return;
    coop::net::InventoryPickupPayload payload{loc.X, loc.Y, loc.Z};
    s->SendReliable(coop::net::ReliableKind::InventoryPickup, &payload, sizeof(payload));
    UE_LOGI("inventory_pickup: broadcast collect blip at (%.0f, %.0f, %.0f)",
            loc.X, loc.Y, loc.Z);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);

    if (!g_pocketWatchRegistered &&
        SG::WatchName(kPocketVerb, kPocketTag, &OnPocketPre, nullptr)) {
        g_pocketWatchRegistered = true;
        UE_LOGI("inventory_pickup: watching %ls at the script-body gate -- mutable item state "
                "publishes before the personal-store capture", kPocketVerb);
    }
    SG::ResolvePendingNames();
    if (session && session->running()) SG::SetEnabled(true);

    if (g_observerRegistered && g_inventoryCue) return;

    // Throttle the GUObjectArray walks to ~1 Hz until everything resolves
    // (the cue asset streams in with gameplay; GameplayStatics exists at boot).
    if ((g_resolveN++ % 125) != 0) return;

    if (!g_inventoryCue) {
        g_inventoryCue = R::FindObject(L"inventory_Cue", L"SoundCue");
        if (g_inventoryCue)
            UE_LOGI("inventory_pickup: resolved inventory_Cue=%p", g_inventoryCue);
    }
    if (g_observerRegistered) return;

    if (!g_playSound2DFn) {
        if (void* cls = R::FindClass(L"GameplayStatics"))
            g_playSound2DFn = R::FindFunction(cls, L"PlaySound2D");
        if (!g_playSound2DFn) return;  // engine class not indexed yet -- retry
        g_offSound = R::FindParamOffset(g_playSound2DFn, L"Sound");
        g_offPitch = R::FindParamOffset(g_playSound2DFn, L"PitchMultiplier");
        g_offWco   = R::FindParamOffset(g_playSound2DFn, L"WorldContextObject");
        if (g_offSound < 0 || g_offPitch < 0 || g_offWco < 0) {
            UE_LOGW("inventory_pickup: PlaySound2D param offsets unresolved "
                    "(Sound=%d Pitch=%d WCO=%d) -- blip sync disabled",
                    g_offSound, g_offPitch, g_offWco);
            // Permanently DISABLED (no retry): a recook changed the signature;
            // re-walking it would yield the same miss. The observer is never
            // registered in this state, so the -1 offsets are unreachable.
            g_playSound2DFn = nullptr;
            g_observerRegistered = true;
            return;
        }
    }
    if (!GT::RegisterPostObserver(g_playSound2DFn, &OnPlaySound2DPost)) {
        UE_LOGE("inventory_pickup: POST observer registration FAILED (table full?) -- retrying");
        return;
    }
    g_observerRegistered = true;
    UE_LOGI("inventory_pickup: observer installed on GameplayStatics::PlaySound2D @ %p "
            "(offs Sound=%d Pitch=%d WCO=%d, cue=%p)",
            g_playSound2DFn, g_offSound, g_offPitch, g_offWco, g_inventoryCue);
}

void OnReliable(const coop::net::InventoryPickupPayload& payload) {
    if (!GT::IsGameThread()) { UE_LOGW("inventory_pickup: OnReliable off-game-thread -- dropping"); return; }
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z)) return;
    void* worldCtx = coop::players::Registry::Get().Local();
    if (!worldCtx) return;  // no local pawn yet -> no world to play in
    coop::prop_sound::PlayInventoryBlipAt(worldCtx, ue_wrap::FVector{payload.x, payload.y, payload.z});
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    // The observer stays registered; it self-gates on a connected session.
}

}  // namespace coop::inventory_pickup_sync
