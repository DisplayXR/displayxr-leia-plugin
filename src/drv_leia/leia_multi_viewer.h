// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Multi-viewer weaving on the Windows SR v2 weavers (#446): the
 *         "LeiaSR multi-viewer" 3D mode and the MULTI_VIEWER setting.
 *
 * Multi-viewer weaving is the same atlas as "LeiaSR" (two views, 2x1) with the
 * weaver told to weave for every tracked viewer. It is offered two ways:
 *
 * 1. As a 3D MODE (runtime ADR-066 C1, runtime v2.70.7+). When the installed
 *    LeiaSR runtime reports multi-viewer weaving, the plug-in declares three
 *    rendering-mode entries instead of two:
 *
 *    | entry | name                | views | weaver override                    |
 *    |-------|---------------------|-------|------------------------------------|
 *    | 0     | 2D                  | 1     | (lens off)                         |
 *    | 1     | LeiaSR              | 2     | none, or OFF with MULTI_VIEWER=off |
 *    | 2     | LeiaSR multi-viewer | 2     | ON                                 |
 *
 *    Apps still see two modes (0 = 2D, 1 = 3D). Which entry the 3D slot shows
 *    is the screen's "3D mode": the DisplayXR Dashboard's picker, or
 *
 *        displayxr-cli dp mode set leiasr_multi_viewer --screen <key>
 *
 *    The two 3D entries have the same view count, scale and tiles, so the
 *    switch applies live to running apps. On a machine whose LeiaSR runtime
 *    does not report multi-viewer weaving, and on a build against runtime
 *    headers older than v2.70.7, the list is the two entries it always was.
 *
 * 2. As the MULTI_VIEWER SETTING, kept as an alias:
 *
 *        displayxr-cli dp set leia-sr MULTI_VIEWER on
 *
 *    read through the host's `get_setting` (`DXR_DP_LEIA_SR_MULTI_VIEWER` in
 *    the hosting process's environment wins over the per-user store, which
 *    wins over the machine store). Values:
 *
 *    - `system` (default): "LeiaSR" follows the LeiaSR system setting (no
 *      override call).
 *    - `on`: "LeiaSR multi-viewer" is the DEFAULT 3D mode of a screen with no
 *      stored choice. Where the mode list has two entries (see above), it
 *      forces multi-viewer weaving on, as before.
 *    - `off`: "LeiaSR" forces multi-viewer weaving off.
 *
 *    A stored per-screen 3D mode wins over the setting: with `on` and
 *    "LeiaSR" chosen for a screen, that screen follows the system setting.
 *
 * When this build's SR SDK or the installed SR runtime cannot weave for more
 * than one viewer, a non-default value is ignored with one WARN at device
 * creation (leia_multi_viewer_effective()).
 *
 * The rules are plain C with no SR or runtime types, host-tested by
 * tests/test_multi_viewer.c (Linux CI). The process-wide state the device,
 * the display processors and the weavers read lives in leia_plugin.c, Windows
 * only.
 *
 * @ingroup drv_leia
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The setting's key (`DXR_DP_LEIA_SR_MULTI_VIEWER`, `Settings\MULTI_VIEWER`).
#define LEIA_MULTI_VIEWER_KEY "MULTI_VIEWER"
//! The compiled default, as the CLI lists it.
#define LEIA_MULTI_VIEWER_DEFAULT "system"
//! One line of help for `displayxr-cli dp options leia-sr` (enum: the words).
#define LEIA_MULTI_VIEWER_HELP                                                                                         \
	"system|on|off -- multi-viewer weaving: follow the LeiaSR system setting, make 'LeiaSR multi-viewer' the "   \
	"default 3D mode, or force it off in 'LeiaSR' (Windows SR v2 weavers)"

/*!
 * The MULTI_VIEWER values. Numeric values are internal only (never stored).
 */
enum leia_multi_viewer
{
	LEIA_MULTI_VIEWER_SYSTEM = 0, //!< follow the SR system setting (no override call)
	LEIA_MULTI_VIEWER_ON = 1,     //!< force multi-viewer weaving on
	LEIA_MULTI_VIEWER_OFF = 2,    //!< force multi-viewer weaving off
};

/*!
 * Parse a stored value. Case-insensitive, surrounding blanks ignored.
 *
 * @param s    The value as the host returned it; NULL or "" = unset.
 * @param out  The parsed value; LEIA_MULTI_VIEWER_SYSTEM unless the return
 *             is true and the word says otherwise.
 * @return true for `system` / `on` / `off`; false for NULL, "", or any other
 *         word (the caller warns about a non-empty unknown word).
 */
bool
leia_multi_viewer_parse(const char *s, enum leia_multi_viewer *out);

/*!
 * The value the weavers actually apply: @p setting when @p weave_supported,
 * else LEIA_MULTI_VIEWER_SYSTEM (the setting is ignored — the caller logs
 * that once when @p setting was not already SYSTEM).
 */
enum leia_multi_viewer
leia_multi_viewer_effective(enum leia_multi_viewer setting, bool weave_supported);

//! "system" / "on" / "off" (the stored word), "?" for anything else.
const char *
leia_multi_viewer_name(enum leia_multi_viewer v);

/*
 *
 * The declared 3D entries (runtime ADR-066 C1). Pure rules.
 *
 */

//! Declared entry 0: the flat 2D entry.
#define LEIA_MV_ENTRY_2D 0u
//! Declared entry 1: "LeiaSR".
#define LEIA_MV_ENTRY_3D 1u
//! Declared entry 2: "LeiaSR multi-viewer", only in a three-entry list.
#define LEIA_MV_ENTRY_MULTI_VIEWER 2u

//! The entry names apps and the Dashboard see. The runtime's choice names
//! (`dp mode set <name>`) are their slugs: `leiasr`, `leiasr_multi_viewer`.
#define LEIA_MV_NAME_2D "2D"
#define LEIA_MV_NAME_3D "LeiaSR"
#define LEIA_MV_NAME_MULTI_VIEWER "LeiaSR multi-viewer"

/*!
 * How many rendering-mode entries the plug-in declares: 3 (2D, LeiaSR, LeiaSR
 * multi-viewer) when the SR runtime weaves for several viewers AND the runtime
 * headers carry the declared-list contract, else 2 (2D, LeiaSR), as before.
 *
 * @param weave_supported     This build's SR SDK has multi-viewer weaving and
 *                            the installed SR runtime reports it.
 * @param has_entry_contract  Built against runtime headers with the declared
 *                            list and the output-entry slots (v2.70.7+).
 */
uint32_t
leia_multi_viewer_entry_count(bool weave_supported, bool has_entry_contract);

/*!
 * The entry the plug-in's own configuration puts in the 3D slot of a screen
 * with no stored choice: "LeiaSR multi-viewer" for MULTI_VIEWER=on in a
 * three-entry list, else "LeiaSR".
 */
uint32_t
leia_multi_viewer_default_entry(enum leia_multi_viewer setting, uint32_t entry_count);

/*!
 * The 3D entry in effect after the runtime names @p incoming: a 3D entry of
 * the list replaces @p current; the 2D entry and an index outside the list
 * keep it (the weaver option is a 3D matter, and the lens is off in 2D, so a
 * 2D/3D toggle costs no weaver call).
 */
uint32_t
leia_multi_viewer_latch_entry(uint32_t current, uint32_t incoming, uint32_t entry_count);

/*!
 * The multi-viewer value a weaver applies while it draws @p entry.
 *
 * @param entry        The 3D entry in effect (see leia_multi_viewer_latch_entry).
 * @param entry_count  leia_multi_viewer_entry_count().
 * @param effective    leia_multi_viewer_effective() of the setting.
 *
 * In a two-entry list the setting decides, as before. In a three-entry list
 * "LeiaSR multi-viewer" is ON whatever the setting says, and "LeiaSR" is the
 * setting's `system` or `off`; with the setting `on` (which only names the
 * default entry there) "LeiaSR" follows the system setting.
 */
enum leia_multi_viewer
leia_multi_viewer_for_entry(uint32_t entry, uint32_t entry_count, enum leia_multi_viewer effective);

//! "2D" / "LeiaSR" / "LeiaSR multi-viewer", "?" for anything else.
const char *
leia_multi_viewer_entry_name(uint32_t entry);


/*
 *
 * Process-wide state (leia_plugin.c, Windows arm only: not defined on the
 * Linux / macOS / Android arms, which have no multi-viewer weave). Cheap, any
 * thread.
 *
 */

/*!
 * How many entries the Leia head device declares: decided at each device
 * creation (leia_multi_viewer_entry_count()); 0 before the first one.
 */
uint32_t
leia_multi_viewer_declared_entries(void);

/*!
 * The plug-in's default 3D entry NOW: MULTI_VIEWER is read again on every
 * call, so a change of the setting applies live where the runtime polls for
 * it (XRT_DEVICE_PROPERTY_CONFIGURED_3D_MODE). LEIA_MV_ENTRY_3D before the
 * first device creation.
 */
uint32_t
leia_multi_viewer_configured_entry(void);

/*!
 * The 3D entry the head device is told to draw (its OUTPUT_MODE property),
 * which every display processor with no entry of its own follows.
 */
void
leia_multi_viewer_set_head_entry(uint32_t entry);

//! The 3D entry the head device draws; LEIA_MV_ENTRY_3D until told.
uint32_t
leia_multi_viewer_head_entry(void);

/*!
 * One display processor's own entry after its `set_output_entry` slot named
 * @p incoming (leia_multi_viewer_latch_entry() against the declared list).
 * Logs one WARN when the entry changes. @p current 0 = never told.
 */
uint32_t
leia_multi_viewer_dp_entry(uint32_t current, uint32_t incoming, const char *backend);

/*!
 * The multi-viewer value a weaver applies right before a weave.
 *
 * @param dp_entry  The weaver's display processor's own entry
 *                  (leia_multi_viewer_dp_entry()), 0 = never told: the head
 *                  device's entry then.
 */
enum leia_multi_viewer
leia_multi_viewer_wanted(uint32_t dp_entry);

/*!
 * The multi-viewer value of the head device's entry:
 * leia_multi_viewer_wanted(0). LEIA_MULTI_VIEWER_SYSTEM until a device has
 * been created.
 */
enum leia_multi_viewer
leia_multi_viewer_active(void);

#ifdef __cplusplus
}
#endif
