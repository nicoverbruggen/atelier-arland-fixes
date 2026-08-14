// SPDX-License-Identifier: MIT
#pragma once

namespace atfix {

// CPU writes that cannot safely reach a busy GPU resource are redirected to a
// readable staging shadow, then flushed back before the next GPU consumer. A
// dynamic destination may require WRITE_DISCARD, which makes mapping it before
// the shadow dangerous: if the later shadow read fails, the game's usable
// contents have already been discarded. Read the source first and retain a
// failed dirty entry for a later flush. The queue owns the resource reference;
// requeue() acquires one only when no concurrent dirty mark already did.
//
// UpdateSubresource can also replace the caller's bytes with a patched copy.
// The game resource and staging mirror must receive that same effective copy or
// a later staging read can restore the unpatched values. These small policies
// stay independent of D3D types so every failure and retry order can be
// injected deterministically while the DLL uses the exact same implementation.

enum class ShadowUploadResult {
  Copied,
  SourceMapFailed,
  DestinationMapFailed,
};

template <typename Operations>
ShadowUploadResult uploadDirtyShadow(Operations& operations) {
  if (!operations.mapSource()) {
    operations.requeue();
    return ShadowUploadResult::SourceMapFailed;
  }
  if (!operations.mapDestination()) {
    operations.unmapSource();
    operations.requeue();
    return ShadowUploadResult::DestinationMapFailed;
  }
  operations.copy();
  operations.unmapSource();
  operations.unmapDestination();
  return ShadowUploadResult::Copied;
}

template <typename Resource, typename Submit, typename Acquire, typename Release>
void updateMirroredSubresource(Resource* resource,
                               const void* effectiveData,
                               Submit&& submit,
                               Acquire&& acquire,
                               Release&& release) {
  submit(resource, effectiveData);
  Resource* shadow = acquire(resource);
  if (shadow) {
    submit(shadow, effectiveData);
    release(shadow);
  }
}

}  // namespace atfix
