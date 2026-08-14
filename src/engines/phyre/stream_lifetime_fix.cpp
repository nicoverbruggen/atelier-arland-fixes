// SPDX-License-Identifier: MIT
//
// Totori asynchronous vertex/index stream lifetime correction.
//
// The game's render producer serializes raw ktgl::CDX11VertexStream and
// ktgl::CDX11IndexStream pointers into commands 0x28 and 0x29. The worker later
// dereferences those wrappers, but the queue owns no reference. Transient effect
// geometry can therefore release a wrapper before its command is consumed.
//
// Both wrapper types use a plain, non-atomic refcount at +8. Keep every refcount
// operation on the producer thread: increment before publishing a command, run
// the original worker handler unchanged, then defer the matching decrement until
// that producer thread next changes a stream binding. This also preserves the
// vertex handler's write to CDX11VertexStream+0x20. The unchanged-draw fast paths
// perform only the same ordinary object loads and comparisons as vanilla.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../../core/config.h"
#include "../../core/log.h"
#include "stream_lifetime_fix.h"

namespace atfix {

extern Log log;  // main.cpp

namespace {

using IndexProducerProc = uintptr_t (STDMETHODCALLTYPE*)(uintptr_t);
using VertexProducerProc = void (STDMETHODCALLTYPE*)(
  uintptr_t, int32_t, uintptr_t, uint32_t, uint32_t);
using IndexConsumerProc = uintptr_t (STDMETHODCALLTYPE*)(
  uintptr_t, uintptr_t);
using VertexConsumerProc = uintptr_t (STDMETHODCALLTYPE*)(
  uintptr_t, uintptr_t, uint32_t);
using FinalizeProc = void (STDMETHODCALLTYPE*)(uintptr_t);

struct StreamAddrs {
  uintptr_t indexProducer;
  uintptr_t indexConsumer;
  uintptr_t vertexProducer;
  uintptr_t vertexConsumer;
  std::array<BYTE, 32> indexProducerExpected;
  std::array<BYTE, 32> indexConsumerExpected;
  std::array<BYTE, 32> vertexProducerExpected;
  std::array<BYTE, 32> vertexConsumerExpected;
};

constexpr StreamAddrs kTotoriEn = {
  0x6176a0,
  0x4ac9e0,
  0x61ab40,
  0x4ac980,
  {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
    0xd9, 0x48, 0x81, 0xc1, 0xa8, 0x07, 0x00, 0x00,
    0xe8, 0x4b, 0xf3, 0xff, 0xff, 0x48, 0x8b, 0x0d,
    0xfc, 0x9c, 0x9e, 0x4e, 0x4c, 0x8b, 0x83, 0x30,
  },
  {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
    0xda, 0x8b, 0x52, 0x08, 0x48, 0xc1, 0xe2, 0x20,
    0x8b, 0x43, 0x04, 0x48, 0x0b, 0xd0, 0xe8, 0x25,
    0x1c, 0x02, 0x00, 0x48, 0x8d, 0x43, 0x0c, 0x48,
  },
  {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x8b, 0x5c, 0x24,
    0x28, 0x4c, 0x8b, 0xd1, 0x4c, 0x63, 0xda, 0x4e,
    0x39, 0x84, 0xd9, 0xb0, 0x02, 0x00, 0x00, 0x75,
    0x14, 0x42, 0x39, 0x9c, 0x99, 0x30, 0x04, 0x00,
  },
  {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x8b, 0x42,
    0x08, 0x45, 0x8b, 0xd0, 0x45, 0x8b, 0xd8, 0x41,
    0xc1, 0xea, 0x15, 0x45, 0x8b, 0xc8, 0x41, 0xc1,
    0xeb, 0x10, 0x44, 0x8b, 0x42, 0x0c, 0x41, 0xf7,
  },
};

constexpr StreamAddrs kTotoriMulti = {
  0x894ca0,
  0x729fe0,
  0x898140,
  0x729f80,
  {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
    0xd9, 0x48, 0x81, 0xc1, 0xa8, 0x07, 0x00, 0x00,
    0xe8, 0x4b, 0xf3, 0xff, 0xff, 0x48, 0x8b, 0x0d,
    0x3c, 0x56, 0xb2, 0x4e, 0x4c, 0x8b, 0x83, 0x30,
  },
  {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
    0xda, 0x8b, 0x52, 0x08, 0x48, 0xc1, 0xe2, 0x20,
    0x8b, 0x43, 0x04, 0x48, 0x0b, 0xd0, 0xe8, 0x25,
    0x1c, 0x02, 0x00, 0x48, 0x8d, 0x43, 0x0c, 0x48,
  },
  {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x8b, 0x5c, 0x24,
    0x28, 0x4c, 0x8b, 0xd1, 0x4c, 0x63, 0xda, 0x4e,
    0x39, 0x84, 0xd9, 0xb0, 0x02, 0x00, 0x00, 0x75,
    0x14, 0x42, 0x39, 0x9c, 0x99, 0x30, 0x04, 0x00,
  },
  {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x8b, 0x42,
    0x08, 0x45, 0x8b, 0xd0, 0x45, 0x8b, 0xd8, 0x41,
    0xc1, 0xea, 0x15, 0x45, 0x8b, 0xc8, 0x41, 0xc1,
    0xeb, 0x10, 0x44, 0x8b, 0x42, 0x0c, 0x41, 0xf7,
  },
};

constexpr uintptr_t kRefcountOffset = 0x08;
constexpr uintptr_t kIndexCurrentOffset = 0xc30;
constexpr uintptr_t kIndexCacheOffset = 0x90;
// The command stream is a bank of buffers, not one: +0x2b18 is the base of the
// current buffer and +0x2b28 the write cursor, and the engine's check-and-
// advance routine moves both to the next buffer in the bank mid-call (totori-en
// 0x4af493, reached from 0x4acf1c and ten siblings; the same instruction pair is
// at totori-ml 0x72ca93). Two cursors read either side of a producer call can
// therefore sit in different allocations, so the base is read alongside the
// cursor and the search between them only runs when it has not moved.
constexpr uintptr_t kCommandBaseOffset = 0x2b18;
constexpr uintptr_t kCommandEndOffset = 0x2b28;
constexpr uintptr_t kVertexCacheOffset = 0x2b0;
constexpr uintptr_t kVertexStrideCacheOffset = 0x3b0;
constexpr uintptr_t kVertexOffsetCacheOffset = 0x430;
constexpr uint16_t kVertexCommand = 0x28;
constexpr uint16_t kIndexCommand = 0x29;
constexpr size_t kVertexPointerOffset = 0x08;
constexpr size_t kIndexPointerOffset = 0x04;
constexpr size_t kVertexCommandSize = 0x10;
constexpr size_t kIndexCommandSize = 0x0c;
constexpr size_t kMaxProducerAppend = 0x10000;

struct PinnedCommand {
  uintptr_t wrapper = 0;
  DWORD producerThread = 0;
  uint16_t type = 0;
};

struct PendingPublication {
  uint64_t id = 0;
  PinnedCommand pin{};
  bool consumed = false;
};

struct PublicationResult {
  bool consumed = false;
  PinnedCommand replaced{};
};

IndexProducerProc originalIndexProducer = nullptr;
VertexProducerProc originalVertexProducer = nullptr;
IndexConsumerProc originalIndexConsumer = nullptr;
VertexConsumerProc originalVertexConsumer = nullptr;
uintptr_t g_renderInterfaceSlot = 0;
std::mutex g_pinMutex;
std::unordered_map<uintptr_t, PinnedCommand> g_pins;
std::vector<PendingPublication> g_publications;
std::vector<PinnedCommand> g_deferredReleases;
uint64_t g_nextPublication = 1;
std::atomic<bool> g_loggedIndexPin = false;
std::atomic<bool> g_loggedIndexConsume = false;
std::atomic<bool> g_loggedVertexPin = false;
std::atomic<bool> g_loggedVertexConsume = false;

uintptr_t ripTarget(const BYTE* instruction, size_t displacementOffset,
                    size_t instructionLength) {
  int32_t displacement = 0;
  std::memcpy(&displacement, instruction + displacementOffset,
              sizeof(displacement));
  return reinterpret_cast<uintptr_t>(instruction) + instructionLength +
    displacement;
}

bool enabled() {
  const char* value = std::getenv("ARLAND_STREAM_LIFETIME_FIX");
  if (!value)
    value = std::getenv("ARLAND_INDEX_STREAM_FIX");
  return !value || value[0] != '0';
}

// Neither store below takes the writableRange guard mem.h asks of walked
// pointers, and the liveness proof is the same kind mem.h accepts for detour
// arguments. At pin time the engine is using the wrapper in the same producer
// call: the original this detour calls serializes that same pointer into the
// command stream. At release time the pinned refcount itself is the proof:
// the engine frees a wrapper only when this count reaches zero, so an object
// whose count the mod still holds cannot have been freed. A VirtualQuery per
// stream-binding change on the render thread would add cost without adding a
// fact either argument lacks.
void pinWrapper(uintptr_t wrapper) {
  ++*reinterpret_cast<int32_t*>(wrapper + kRefcountOffset);
}

void releaseWrapper(uintptr_t wrapper) {
  auto* refcount = reinterpret_cast<int32_t*>(wrapper + kRefcountOffset);
  if (--*refcount != 0)
    return;
  auto* vtable = *reinterpret_cast<uintptr_t**>(wrapper);
  reinterpret_cast<FinalizeProc>(vtable[2])(wrapper);
}

void deferRelease(const PinnedCommand& pin) {
  std::lock_guard<std::mutex> lock(g_pinMutex);
  g_deferredReleases.push_back(pin);
}

void drainDeferredReleases() {
  const DWORD thread = GetCurrentThreadId();
  std::vector<PinnedCommand> ready;
  {
    std::lock_guard<std::mutex> lock(g_pinMutex);
    // Neither container is ever cleared wholesale. Both reclaim implicitly: the
    // command bank cycles, so a stale pin is displaced and released the next
    // time its address carries a command, and the deferred queue is drained by
    // the thread that produced. Report the sizes so the reclaim can be seen to
    // work: flat over a session is the answer, rising names an event nobody has
    // been able to name.
    static uint32_t reported = 0;
    if (verboseLogging() && reported++ % 3600 == 0)
      log("STREAM LIFETIME pins=", g_pins.size(),
          " deferred=", g_deferredReleases.size());
    for (size_t i = 0; i < g_deferredReleases.size();) {
      if (g_deferredReleases[i].producerThread != thread) {
        ++i;
        continue;
      }
      ready.push_back(g_deferredReleases[i]);
      g_deferredReleases[i] = g_deferredReleases.back();
      g_deferredReleases.pop_back();
    }
  }
  for (const PinnedCommand& pin : ready)
    releaseWrapper(pin.wrapper);
}

uintptr_t findQueuedCommand(uintptr_t begin, uintptr_t end, uint16_t type,
                            uintptr_t wrapper, size_t pointerOffset,
                            size_t commandSize) {
  if (!begin || end < begin || end - begin > kMaxProducerAppend)
    return 0;
  for (uintptr_t command = begin; command + commandSize <= end; ++command) {
    uint32_t header = 0;
    uintptr_t queuedWrapper = 0;
    std::memcpy(&header, reinterpret_cast<const void*>(command), sizeof(header));
    std::memcpy(&queuedWrapper,
                reinterpret_cast<const void*>(command + pointerOffset),
                sizeof(queuedWrapper));
    if (uint16_t(header) == type && queuedWrapper == wrapper)
      return command;
  }
  return 0;
}

PinnedCommand recordPinLocked(uintptr_t command, const PinnedCommand& pin) {
  PinnedCommand replaced{};
  auto inserted = g_pins.emplace(command, pin);
  if (!inserted.second) {
    replaced = inserted.first->second;
    inserted.first->second = pin;
  }
  return replaced;
}

void logFirstPin(uintptr_t command, const PinnedCommand& pin) {
  std::atomic<bool>& logged = pin.type == kIndexCommand
    ? g_loggedIndexPin : g_loggedVertexPin;
  if (!logged.exchange(true)) {
    log("STREAM LIFETIME pinned first ",
        pin.type == kIndexCommand ? "index" : "vertex",
        " command=0x", std::hex, command,
        " wrapper=0x", pin.wrapper, std::dec,
        " producer_thread=", pin.producerThread);
  }
}

void releaseReplacedPin(const PinnedCommand& pin,
                        const PinnedCommand& replaced) {
  if (replaced.wrapper) {
    if (replaced.producerThread == pin.producerThread)
      releaseWrapper(replaced.wrapper);
    else
      deferRelease(replaced);
  }
}

uint64_t beginPublication(const PinnedCommand& pin) {
  std::lock_guard<std::mutex> lock(g_pinMutex);
  const uint64_t id = g_nextPublication++;
  g_publications.push_back({id, pin, false});
  return id;
}

PublicationResult finishPublication(uint64_t id, uintptr_t command,
                                    const PinnedCommand& pin) {
  std::lock_guard<std::mutex> lock(g_pinMutex);
  PublicationResult result{};
  for (size_t i = 0; i < g_publications.size(); ++i) {
    if (g_publications[i].id != id)
      continue;
    result.consumed = g_publications[i].consumed;
    g_publications[i] = g_publications.back();
    g_publications.pop_back();
    break;
  }
  if (command && !result.consumed)
    result.replaced = recordPinLocked(command, pin);
  return result;
}

bool takePin(uintptr_t command, uintptr_t wrapper, uint16_t type,
             PinnedCommand& pin) {
  std::lock_guard<std::mutex> lock(g_pinMutex);
  const auto found = g_pins.find(command);
  if (found != g_pins.end() && found->second.type == type) {
    pin = found->second;
    g_pins.erase(found);
    return true;
  }

  // The worker can reach a command before the original producer returns. Its
  // wrapper was already pinned, so claim that pending publication directly.
  for (PendingPublication& publication : g_publications) {
    if (!publication.consumed && publication.pin.type == type &&
        publication.pin.wrapper == wrapper) {
      publication.consumed = true;
      pin = publication.pin;
      return true;
    }
  }
  return false;
}

uintptr_t STDMETHODCALLTYPE correctedIndexProducer(uintptr_t core) {
  const uintptr_t renderer =
    *reinterpret_cast<const uintptr_t*>(g_renderInterfaceSlot);
  if (!renderer)
    return originalIndexProducer(core);

  const uintptr_t cached =
    *reinterpret_cast<const uintptr_t*>(renderer + kIndexCacheOffset);
  if (*reinterpret_cast<const uintptr_t*>(core + kIndexCurrentOffset) == cached)
    return originalIndexProducer(core);

  // The drain can run the engine's finalizer on a wrapper, so read the one we
  // are about to pin after it rather than before: a value read first could be
  // freed by the drain and pinned afterwards.
  drainDeferredReleases();
  const uintptr_t wrapper =
    *reinterpret_cast<const uintptr_t*>(core + kIndexCurrentOffset);
  if (!wrapper)
    return originalIndexProducer(core);

  const uintptr_t beforeBase =
    *reinterpret_cast<const uintptr_t*>(renderer + kCommandBaseOffset);
  const uintptr_t before =
    *reinterpret_cast<const uintptr_t*>(renderer + kCommandEndOffset);
  const PinnedCommand pin{
    wrapper, GetCurrentThreadId(), kIndexCommand};
  pinWrapper(wrapper);
  const uint64_t publication = beginPublication(pin);
  const uintptr_t result = originalIndexProducer(core);
  const uintptr_t afterBase =
    *reinterpret_cast<const uintptr_t*>(renderer + kCommandBaseOffset);
  const uintptr_t after =
    *reinterpret_cast<const uintptr_t*>(renderer + kCommandEndOffset);
  const uintptr_t command = beforeBase == afterBase
    ? findQueuedCommand(before, after, kIndexCommand, wrapper,
        kIndexPointerOffset, kIndexCommandSize)
    : 0;
  const PublicationResult published =
    finishPublication(publication, command, pin);
  if (command)
    logFirstPin(command, pin);
  releaseReplacedPin(pin, published.replaced);
  if (!command && !published.consumed)
    releaseWrapper(wrapper);
  return result;
}

void STDMETHODCALLTYPE correctedVertexProducer(
    uintptr_t renderer, int32_t slot, uintptr_t wrapper,
    uint32_t stride, uint32_t offset) {
  if (slot < 0 || slot >= 32) {
    originalVertexProducer(renderer, slot, wrapper, stride, offset);
    return;
  }
  const intptr_t signedSlot = slot;
  const uintptr_t cachedWrapper = *reinterpret_cast<const uintptr_t*>(
    renderer + kVertexCacheOffset + signedSlot * sizeof(uintptr_t));
  const uint32_t cachedStride = *reinterpret_cast<const uint32_t*>(
    renderer + kVertexStrideCacheOffset + signedSlot * sizeof(uint32_t));
  const uint32_t cachedOffset = *reinterpret_cast<const uint32_t*>(
    renderer + kVertexOffsetCacheOffset + signedSlot * sizeof(uint32_t));
  if (wrapper == cachedWrapper && stride == cachedStride &&
      offset == cachedOffset) {
    originalVertexProducer(renderer, slot, wrapper, stride, offset);
    return;
  }

  drainDeferredReleases();
  if (!wrapper) {
    originalVertexProducer(renderer, slot, wrapper, stride, offset);
    return;
  }

  const uintptr_t beforeBase =
    *reinterpret_cast<const uintptr_t*>(renderer + kCommandBaseOffset);
  const uintptr_t before =
    *reinterpret_cast<const uintptr_t*>(renderer + kCommandEndOffset);
  const PinnedCommand pin{
    wrapper, GetCurrentThreadId(), kVertexCommand};
  pinWrapper(wrapper);
  const uint64_t publication = beginPublication(pin);
  originalVertexProducer(renderer, slot, wrapper, stride, offset);
  const uintptr_t afterBase =
    *reinterpret_cast<const uintptr_t*>(renderer + kCommandBaseOffset);
  const uintptr_t after =
    *reinterpret_cast<const uintptr_t*>(renderer + kCommandEndOffset);
  const uintptr_t command = beforeBase == afterBase
    ? findQueuedCommand(before, after, kVertexCommand, wrapper,
        kVertexPointerOffset, kVertexCommandSize)
    : 0;
  const PublicationResult published =
    finishPublication(publication, command, pin);
  if (command)
    logFirstPin(command, pin);
  releaseReplacedPin(pin, published.replaced);
  if (!command && !published.consumed)
    releaseWrapper(wrapper);
}

bool beginConsume(uintptr_t command, uint16_t type, PinnedCommand& pin) {
  uintptr_t wrapper = 0;
  const size_t pointerOffset = type == kIndexCommand
    ? kIndexPointerOffset : kVertexPointerOffset;
  std::memcpy(&wrapper, reinterpret_cast<const void*>(command + pointerOffset),
              sizeof(wrapper));
  return takePin(command, wrapper, type, pin);
}

void finishConsume(uintptr_t command, uint16_t type,
                   const PinnedCommand& pin) {
  deferRelease(pin);

  std::atomic<bool>& logged = type == kIndexCommand
    ? g_loggedIndexConsume : g_loggedVertexConsume;
  if (!logged.exchange(true)) {
    log("STREAM LIFETIME consumed first ",
        type == kIndexCommand ? "index" : "vertex",
        " command=0x", std::hex, command,
        " wrapper=0x", pin.wrapper, std::dec,
        " worker_thread=", GetCurrentThreadId());
  }
}

uintptr_t STDMETHODCALLTYPE correctedIndexConsumer(uintptr_t renderer,
                                                   uintptr_t command) {
  PinnedCommand pin{};
  if (!beginConsume(command, kIndexCommand, pin))
    return originalIndexConsumer(renderer, command);
  const uintptr_t result = originalIndexConsumer(renderer, command);
  finishConsume(command, kIndexCommand, pin);
  return result;
}

uintptr_t STDMETHODCALLTYPE correctedVertexConsumer(uintptr_t renderer,
                                                    uintptr_t command,
                                                    uint32_t packedState) {
  PinnedCommand pin{};
  if (!beginConsume(command, kVertexCommand, pin))
    return originalVertexConsumer(renderer, command, packedState);
  // Unlike command 0x29, the 0x28 handler receives its packed slot/stride/state
  // header separately in R8. Dropping it binds the stream with garbage state
  // and leaves the frame without usable vertex input.
  const uintptr_t result =
    originalVertexConsumer(renderer, command, packedState);
  finishConsume(command, kVertexCommand, pin);
  return result;
}

}  // namespace

bool installStreamLifetimeFix(BYTE* base, const Game& game) {
  if (game.atlasVariant != AtlasTotori) {
    log("FIXES stream_lifetime=not_applicable");
    return false;
  }
  if (!enabled()) {
    log("FIXES stream_lifetime=off");
    return false;
  }

  const StreamAddrs& addrs = game.exeBuild == BuildEnglish
    ? kTotoriEn : kTotoriMulti;
  BYTE* indexProducer = base + addrs.indexProducer;
  BYTE* indexConsumer = base + addrs.indexConsumer;
  BYTE* vertexProducer = base + addrs.vertexProducer;
  BYTE* vertexConsumer = base + addrs.vertexConsumer;
  if (!matches(indexProducer, addrs.indexProducerExpected) ||
      !matches(indexConsumer, addrs.indexConsumerExpected) ||
      !matches(vertexProducer, addrs.vertexProducerExpected) ||
      !matches(vertexConsumer, addrs.vertexConsumerExpected)) {
    log("Stream-lifetime fix prologue mismatch; not installing");
    return false;
  }

  // indexProducer+0x15 is `mov rcx, [rip+disp32]`, loading the shared render
  // interface whose +0x2b28 field is the command-stream write cursor.
  g_renderInterfaceSlot = ripTarget(indexProducer + 0x15, 3, 7);
  g_pins.reserve(512);
  g_publications.reserve(16);
  g_deferredReleases.reserve(512);

  // Consumers alone are transparent. Install them first so a later producer
  // failure cannot publish a pin that no worker hook will consume.
  const bool indexConsumerOk = installMinHookDetour(
    indexConsumer, reinterpret_cast<void*>(&correctedIndexConsumer),
    reinterpret_cast<void**>(&originalIndexConsumer));
  const bool vertexConsumerOk = indexConsumerOk && installMinHookDetour(
    vertexConsumer, reinterpret_cast<void*>(&correctedVertexConsumer),
    reinterpret_cast<void**>(&originalVertexConsumer));
  const bool indexProducerOk = indexConsumerOk && vertexConsumerOk &&
    installMinHookDetour(
      indexProducer, reinterpret_cast<void*>(&correctedIndexProducer),
      reinterpret_cast<void**>(&originalIndexProducer));
  const bool vertexProducerOk = indexProducerOk && installMinHookDetour(
    vertexProducer, reinterpret_cast<void*>(&correctedVertexProducer),
    reinterpret_cast<void**>(&originalVertexProducer));
  const bool active = indexConsumerOk && vertexConsumerOk &&
    indexProducerOk && vertexProducerOk;
  log("FIXES stream_lifetime=", active ? "active" : "failed",
      " index_producer=", indexProducerOk,
      " index_consumer=", indexConsumerOk,
      " vertex_producer=", vertexProducerOk,
      " vertex_consumer=", vertexConsumerOk);
  return active;
}

}  // namespace atfix
