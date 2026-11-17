// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The lens belongs to its SR instance: one lock and one "live" flag that
 *         every lens call goes through, so no lens call can reach an instance
 *         that is being torn down or is already gone (#541).
 *
 * Why this exists. A v2 `SrLens` is a handle over a `SwitchableLensHint` that the
 * instance's SRContext owns and frees in `srDestroyInstance` (`~SRContext` ->
 * `deleteAllSenses`). The D3D11 arm rebuilds its instance IN PLACE after an SR
 * Service restart (#158), on a worker thread, while the lens worker (#215) and the
 * runtime's own threads can still be calling into the lens. A call that lands
 * after (or races) `srDestroyInstance` touches freed memory: an access violation
 * in the SR client that no try/catch can stop (LeiaInc/LeiaSR#598, ST-5973).
 * LeiaSR#892 makes the SR side refuse such a call, but SR runtimes up to and
 * including 1.39.x do not have it, so the plug-in must be safe on its own.
 *
 * The rule, enforced here:
 *  - The gate starts RETIRED. The owner PUBLISHES it once the lens on the current
 *    instance exists, and RETIRES it before it destroys that lens and instance.
 *  - Every lens call runs under the gate's lock and only while the gate is live.
 *    `retire()` takes the same lock, so it waits for a call in flight and
 *    refuses every later one: by the time the instance goes, no lens call is
 *    inside it and none can start.
 *  - A call that arrives while retired is a no-op that the gate remembers;
 *    `publish()` reports it, and the owner re-requests the latest wish on the
 *    new lens.
 *  - A lens call that fails (SR_ERROR_LENS_NOT_AVAILABLE, or any failure from an
 *    SR runtime that predates LeiaSR#892) recreates the lens on the current
 *    instance and retries once. The owner's warn callback runs once per failure
 *    episode, never per call.
 *
 * Lock order: the gate's lock is a LEAF. While holding it, never take another
 * plug-in lock, never post to or join a lens worker, and never call into the
 * runtime. Callers may hold their own locks above it (the runtime's render mutex
 * on the synchronous lens path, the arm's lens-thread mutex while joining a
 * worker that takes it), never the reverse.
 *
 * Header-only and free of SR types, so the policy is host-tested without an SDK:
 * tests/test_sr_lens_gate.cpp (built and run by the Linux CI job).
 */

#pragma once

#include <atomic>
#include <mutex>

namespace leia {

//! What a lens call through the gate came to.
enum class lens_gate_result
{
	applied,  //!< ran on the live lens and succeeded
	deferred, //!< the gate was retired: no SR call was made; publish() reports it
	failed,   //!< ran, failed, and the recreate + retry did not help
};

class lens_gate
{
public:
	/*!
	 * Is there a lens to switch? Lock-free, for frame-path callers that must not
	 * wait behind a lens call in flight. False while retired.
	 */
	bool
	present() const
	{
		return has_lens_.load(std::memory_order_acquire);
	}

	/*!
	 * Before the lens and its instance go: wait for a lens call in flight, refuse
	 * every later one, then run @p destroy_lens (destroy the lens handle) under
	 * the lock. The instance itself may be destroyed after this returns.
	 */
	template <class DestroyLens>
	void
	retire(DestroyLens &&destroy_lens)
	{
		std::lock_guard<std::mutex> g(mtx_);
		live_ = false;
		has_lens_.store(false, std::memory_order_release);
		destroy_lens();
	}

	/*!
	 * The lens on the current instance exists (or the instance has none:
	 * @p lens_exists false). Lens calls run from now on.
	 *
	 * @return true when a call arrived while the gate was retired: the owner
	 *         re-requests the latest wish on the new lens.
	 */
	bool
	publish(bool lens_exists)
	{
		std::lock_guard<std::mutex> g(mtx_);
		live_ = true;
		has_lens_.store(lens_exists, std::memory_order_release);
		recover_warned_ = false;
		const bool deferred = deferred_;
		deferred_ = false;
		return deferred;
	}

	/*!
	 * A lens call. @p body runs under the lock, only while the gate is live, and
	 * receives the gate so it can call apply_with_recovery(). It returns true on
	 * success.
	 */
	template <class Body>
	lens_gate_result
	call(Body &&body)
	{
		std::lock_guard<std::mutex> g(mtx_);
		if (!live_) {
			deferred_ = true;
			return lens_gate_result::deferred;
		}
		return body(*this) ? lens_gate_result::applied : lens_gate_result::failed;
	}

	/*!
	 * A read of the lens for a frame-path caller: runs @p query under the lock
	 * only if the lock is free right now and the gate is live. Returns false
	 * otherwise (busy, retired) or when @p query does.
	 */
	template <class Query>
	bool
	try_query(Query &&query)
	{
		std::unique_lock<std::mutex> g(mtx_, std::try_to_lock);
		if (!g.owns_lock() || !live_) {
			return false;
		}
		return query();
	}

	/*!
	 * Inside call(): @p apply switches the lens (true on success). On failure
	 * @p recreate replaces the lens on the CURRENT instance (true when one now
	 * exists) and @p apply is retried once. @p warn runs once per failure
	 * episode: the first failure after a success or a publish.
	 */
	template <class Apply, class Recreate, class Warn>
	bool
	apply_with_recovery(Apply &&apply, Recreate &&recreate, Warn &&warn)
	{
		if (apply()) {
			recover_warned_ = false;
			return true;
		}
		if (!recover_warned_) {
			recover_warned_ = true;
			warn();
		}
		const bool exists = recreate();
		has_lens_.store(exists, std::memory_order_release);
		if (exists && apply()) {
			recover_warned_ = false;
			return true;
		}
		return false;
	}

private:
	std::mutex mtx_;                    //!< the lens lock (a leaf, see the file comment)
	bool live_ = false;                 //!< guarded by mtx_
	bool deferred_ = false;             //!< guarded by mtx_: a call came while retired
	bool recover_warned_ = false;       //!< guarded by mtx_: this failure episode has warned
	std::atomic<bool> has_lens_{false}; //!< mirror of "live and a lens exists"
};

} // namespace leia
