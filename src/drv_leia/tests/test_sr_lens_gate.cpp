// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Unit test for the lens gate (leia_sr_lens_gate.h, #541).
 *
 * No SDK, no D3D, no panel. A fake SR backend models the one thing that
 * matters: a lens points into its instance, and destroying the instance frees
 * what the lens points at (LeiaInc/LeiaSR#598). A "freed" instance is marked
 * dead instead of deleted, so a lens call that reaches it is COUNTED rather than
 * crashing the test. The cases replay what the D3D11 arm does: a lens worker
 * switching the lens while another thread tears the instance down and rebuilds
 * it in place (#158), a call already inside the SR round trip when the teardown
 * starts, and SR_ERROR_LENS_NOT_AVAILABLE recreating the lens.
 *
 * Built and run by the Linux CI job (src/drv_leia_linux/CMakeLists.txt), like
 * the other Windows-arm host tests.
 */

#include "leia_sr_lens_gate.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                  \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

constexpr int FAKE_SR_SUCCESS = 0;
constexpr int FAKE_SR_ERROR_LENS_NOT_AVAILABLE = -12; // sr_result.h

//! One SrInstance. `alive` false = srDestroyInstance ran: its hint is freed.
struct fake_instance
{
	std::atomic<bool> alive{true};
	std::atomic<int> inside{0};           //!< lens calls currently inside its hint
	std::atomic<bool> freed_under_call{false};
};

//! One SrLens: a handle into its instance's hint.
struct fake_lens
{
	fake_instance *inst;
	int serial; //!< which srCreateLens made it (a freed address can be reused)
};

int g_lens_serial = 0;

//! The owner: what struct leiasr_d3d11 holds.
struct owner
{
	leia::lens_gate gate;
	std::vector<std::unique_ptr<fake_instance>> instances; // never freed: "dead" instead
	fake_instance *inst = nullptr;
	std::unique_ptr<fake_lens> lens;

	std::atomic<int> calls_on_dead{0}; //!< lens calls that reached a destroyed instance
	std::atomic<int> sr_calls{0};
	int fail_next = 0;      //!< the next N switches fail (guarded by the gate)
	int recreate_ok = 1;    //!< recreate succeeds (guarded by the gate)
	int recreates = 0;      //!< (guarded by the gate)
	int warns = 0;          //!< (guarded by the gate)
	int lens_state = -1;    //!< what the panel shows (guarded by the gate)

	void
	create_instance_and_lens()
	{
		instances.emplace_back(new fake_instance());
		inst = instances.back().get();
		lens.reset(new fake_lens{inst, ++g_lens_serial});
	}

	//! srDestroyInstance: frees the hint every lens of it points into.
	void
	destroy_instance()
	{
		if (inst->inside.load() != 0) {
			inst->freed_under_call = true;
		}
		inst->alive = false;
		inst = nullptr;
	}

	//! srLensEnable / srLensDisable: an SR round trip through the lens's hint.
	int
	sr_switch(bool enable, std::chrono::microseconds round_trip)
	{
		sr_calls++;
		fake_instance *i = lens->inst;
		if (!i->alive.load()) {
			calls_on_dead++;
			return FAKE_SR_ERROR_LENS_NOT_AVAILABLE;
		}
		i->inside++;
		std::this_thread::sleep_for(round_trip);
		if (!i->alive.load()) {
			calls_on_dead++; // freed while we were inside it
		}
		i->inside--;
		if (fail_next > 0) {
			fail_next--;
			return FAKE_SR_ERROR_LENS_NOT_AVAILABLE;
		}
		lens_state = enable ? 1 : 0;
		return FAKE_SR_SUCCESS;
	}

	//! lens_set: the arm's call through the gate, with the #541 recovery.
	leia::lens_gate_result
	set(bool enable, std::chrono::microseconds round_trip = std::chrono::microseconds(0))
	{
		return gate.call([&](leia::lens_gate &g) {
			if (!lens) {
				return true;
			}
			auto apply = [&]() { return sr_switch(enable, round_trip) == FAKE_SR_SUCCESS; };
			auto recreate = [&]() {
				recreates++;
				lens.reset();
				if (recreate_ok) {
					lens.reset(new fake_lens{inst, ++g_lens_serial});
				}
				return lens != nullptr;
			};
			auto warn = [&]() { warns++; };
			return g.apply_with_recovery(apply, recreate, warn);
		});
	}

	//! destroy_sdk_objects: retire (lens first), then the instance.
	void
	teardown()
	{
		gate.retire([&]() { lens.reset(); });
		if (inst != nullptr) {
			destroy_instance();
		}
	}

	//! create_weaver_attempt: new instance + lens, then publish.
	bool
	rebuild()
	{
		create_instance_and_lens();
		return gate.publish(lens != nullptr);
	}
};

void
test_starts_retired_and_defers()
{
	std::printf("A lens call before the first publish is held, then re-requested\n");
	owner o;
	CHECK(!o.gate.present());
	CHECK(o.set(true) == leia::lens_gate_result::deferred);
	CHECK(o.sr_calls.load() == 0);
	CHECK(o.rebuild()); // reports the held call
	CHECK(o.gate.present());
	CHECK(o.set(true) == leia::lens_gate_result::applied);
	CHECK(o.lens_state == 1);
	CHECK(!o.gate.publish(true)); // nothing held since: no re-request
}

void
test_teardown_waits_for_call_in_flight()
{
	std::printf("srDestroyInstance waits for a lens call already inside the SR round trip\n");
	owner o;
	o.rebuild();
	std::atomic<bool> in_call{false};
	std::atomic<bool> teardown_done{false};
	std::thread worker([&] {
		o.gate.call([&](leia::lens_gate &) {
			in_call = true;
			// A slow round trip: the teardown below must not get past retire().
			return o.sr_switch(true, std::chrono::milliseconds(80)) == FAKE_SR_SUCCESS;
		});
	});
	while (!in_call.load()) {
		std::this_thread::yield();
	}
	fake_instance *old = o.inst;
	std::thread tear([&] {
		o.teardown();
		teardown_done = true;
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	CHECK(!teardown_done.load()); // still waiting out the call
	worker.join();
	tear.join();
	CHECK(teardown_done.load());
	CHECK(!old->freed_under_call.load());
	CHECK(o.calls_on_dead.load() == 0);
	// After the teardown: a no-op, never a call into the dead instance.
	CHECK(o.set(false) == leia::lens_gate_result::deferred);
	CHECK(o.calls_on_dead.load() == 0);
	CHECK(o.rebuild());
}

void
test_rebuild_race()
{
	std::printf("A lens worker switching while the instance is rebuilt in place, 300 times\n");
	owner o;
	o.rebuild();
	std::atomic<bool> stop{false};
	std::atomic<int> applied{0}, deferred{0}, failed{0};
	std::thread worker([&] {
		bool on = false;
		while (!stop.load()) {
			on = !on;
			switch (o.set(on, std::chrono::microseconds(150))) {
			case leia::lens_gate_result::applied: applied++; break;
			case leia::lens_gate_result::deferred:
				deferred++;
				// Held while the instance is rebuilt: back off instead of spinning.
				std::this_thread::sleep_for(std::chrono::microseconds(50));
				break;
			case leia::lens_gate_result::failed: failed++; break;
			}
			// Like the real worker, which waits for the next wish: let go of
			// the gate between calls so the rebuilding thread is not starved
			// (std::mutex is not fair).
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
	});
	int republished = 0;
	for (int i = 0; i < 300; i++) {
		o.teardown();
		std::this_thread::sleep_for(std::chrono::microseconds(200));
		if (o.rebuild()) {
			republished++;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	stop = true;
	worker.join();

	CHECK(o.calls_on_dead.load() == 0);
	for (auto &inst : o.instances) {
		CHECK(!inst->freed_under_call.load());
	}
	CHECK(failed.load() == 0);
	CHECK(applied.load() > 0);
	// Every held call is reported by the publish that ends its window (several
	// in one window coalesce into one re-request).
	CHECK(republished <= deferred.load());
	CHECK((deferred.load() == 0) == (republished == 0));
	std::printf("  applied %d, held %d, re-requested at %d publishes\n", applied.load(), deferred.load(),
	            republished);
}

void
test_lens_not_available_recreates()
{
	std::printf("SR_ERROR_LENS_NOT_AVAILABLE recreates the lens and retries, one WARN per episode\n");
	owner o;
	o.rebuild();
	const int first = o.lens->serial;

	o.fail_next = 1;
	CHECK(o.set(true) == leia::lens_gate_result::applied);
	CHECK(o.recreates == 1);
	CHECK(o.lens->serial != first); // a NEW lens
	CHECK(o.lens->inst == o.inst); // on the CURRENT instance
	CHECK(o.warns == 1);
	CHECK(o.lens_state == 1);

	// A new episode after a success warns again — once.
	o.fail_next = 1;
	CHECK(o.set(false) == leia::lens_gate_result::applied);
	CHECK(o.warns == 2);

	// The recreate fails: no lens, switching unavailable, still one WARN.
	o.fail_next = 1;
	o.recreate_ok = 0;
	CHECK(o.set(true) == leia::lens_gate_result::failed);
	CHECK(o.warns == 3);
	CHECK(!o.gate.present());
	CHECK(o.set(true) == leia::lens_gate_result::applied); // no lens: nothing to switch
	CHECK(o.warns == 3);

	// The next rebuild publishes a fresh lens and re-arms the WARN.
	o.recreate_ok = 1;
	o.teardown();
	o.rebuild();
	CHECK(o.gate.present());
	o.fail_next = 2; // the switch AND its retry fail
	CHECK(o.set(true) == leia::lens_gate_result::failed);
	CHECK(o.warns == 4);
	o.fail_next = 2;
	CHECK(o.set(true) == leia::lens_gate_result::failed); // same episode: no new WARN
	CHECK(o.warns == 4);
	CHECK(o.calls_on_dead.load() == 0);
}

void
test_query_never_waits()
{
	std::printf("A frame-path read never waits behind a lens call and never reads a retired lens\n");
	owner o;
	auto read = [&]() { return o.gate.try_query([&]() { return o.inst != nullptr && o.inst->alive.load(); }); };
	CHECK(!read()); // retired before the first publish
	o.rebuild();
	CHECK(read());

	std::atomic<bool> in_call{false};
	std::atomic<bool> release{false};
	std::thread worker([&] {
		o.gate.call([&](leia::lens_gate &) {
			in_call = true;
			while (!release.load()) {
				std::this_thread::yield();
			}
			return true;
		});
	});
	while (!in_call.load()) {
		std::this_thread::yield();
	}
	const auto t0 = std::chrono::steady_clock::now();
	CHECK(!read()); // busy: "unknown", at once
	CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50));
	release = true;
	worker.join();
	CHECK(read());

	o.teardown();
	CHECK(!read());
}

} // namespace

int
main()
{
	test_starts_retired_and_defers();
	test_teardown_waits_for_call_in_flight();
	test_rebuild_race();
	test_lens_not_available_recreates();
	test_query_never_waits();

	if (g_failures != 0) {
		std::fprintf(stderr, "test_sr_lens_gate: %d check(s) FAILED\n", g_failures);
		return 1;
	}
	std::printf("test_sr_lens_gate: all checks passed\n");
	return 0;
}
