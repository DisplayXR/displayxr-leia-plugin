// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Frozen Leia/Dimenco panel EDID identifier table — the single
 *         source of truth shared by every platform's panel probe
 *         (Windows: leia_edid_probe.c via os_display_edid_*; Linux:
 *         drv_leia_linux/leia_edid_probe_linux.c via /sys/class/drm).
 *
 * Data-only header: the array is static const, so each including TU gets
 * its own copy — no link-time coupling between the platform probes.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#pragma once

#include <stdint.h>

// clang-format off

/*!
 * Known Leia/Dimenco 3D display EDID identifiers.
 * Each entry is {manufacturer_id, product_id} from EDID bytes 8-11.
 *
 * Manufacturer IDs:
 *   DELL=44048  ACC=44806  ACR=29188  LG=27934   SDC=33612
 *   BOE=58633   LEN=44592  BNQ=53513  AUS=45830  SHP=4173
 *   INL=44557   KDX=38956  VSC=25434  OOO=61245  RTK=35658
 *   BDS=37640   WAM=11612  ICD=25636  TPV=37694  CSO=28430
 *   NVT=54330   KJL=19501  SAM=11596
 */
static const uint16_t leia_edid_table[][2] = {
	// Dell SR Pro / SR Pro 2 (AJ, CZ)
	{44048, 16711},   // DELL_8K32

	// Acer Ghibli (CJ)
	{44806, 12779},   // ACER_GHIBLI

	// Acer Coldplay / Sparks / Evoque / GLA / Sportage (AL, AM, AY, CM, DQ)
	{44806, 45460},   // ACER_COLDPLAY / SPARKS / EVOQUE / GLA / SPORTAGE
	{44806, 14756},   // ACER_COLDPLAY_2 / SPARKS_2 / EVOQUE_2 / GLA_2 / SPORTAGE_2

	// Acer Sparks Innolux (CW)
	{44557, 5439},    // ACER_SPARKS_INNOLUX

	// MSI FHD (AS)
	{44806, 44944},   // MSI_FHD

	// Dell 16" 3K (CE, CI, DB)
	{44806, 51614},   // DELL_3K16
	{44806, 40849},   // DELL_3K16_2

	// Dell 14" 2K (CF)
	{44806, 11163},   // DELL_2K14

	// Dell WUXGA 14" (CG)
	{44806, 9627},    // DELL_WUXGA14

	// Dell portable 14" (DY)
	{44048, 61773},   // DELL_2K14_PORTABLE_P1424H_LEFT
	{44048, 61772},   // DELL_2K14_PORTABLE_P1424H_RIGHT
	{44048, 61740},   // DELL_2K14_PORTABLE_C1422H_LEFT
	{44048, 61739},   // DELL_2K14_PORTABLE_C1422H_RIGHT

	// Acer DS1 (D1) — range 0-1
	{29188, 0},       // ACR + ACER_DS1
	{29188, 1},       // ACR + ACER_DS1 (alt)

	// Acer DS2 (DS) — range 2-3
	{29188, 2},       // ACR + ACER_DS2
	{29188, 3},       // ACR + ACER_DS2 (alt)

	// Acer D2/D3 prototypes (D2, ER, D3)
	{25636, 9992},    // ICD + ACER_D2_4k27 / D3_4k27

	// LG 4K 27" / Dimenco / SAGA (AN, BP)
	{27934, 30470},   // LG_4K27
	{27934, 30471},   // LG_4K27_DP

	// Deepub 4K 32" (AP)
	{38956, 48},      // KDX + DEEPUB_4K32

	// ASUS 4K 27" (BH)
	{45830, 10144},   // ASUS_4K27_HDMI
	{45830, 10148},   // ASUS_4K27_DP

	// ASUS OLED laptop (BE)
	{33612, 16733},   // ASUS_OLEDLAPTOP

	// ASUS 3.2K 16" OLED (CN, CO, D5, CV, CU, CX, CY)
	{33612, 16760},   // ASUS_3K16_OLED

	// BenQ 4K 27" / 32" (BC, BF)
	{53513, 32679},   // BENQ_4k27
	{53513, 32678},   // BENQ_4k32

	// Lenovo 4K 27" / 28" (BJ, BN)
	{44592, 25291},   // LENOVO_4K27
	{44592, 25062},   // LENOVO_4K28

	// Lenovo 16" 4K OLED Thinkpad (DP)
	{44592, 16710},   // LENOVO_4K16_OLED

	// Lenovo 16" 3K (EB)
	{28430, 5673},    // LENOVO_3K16 (CSO)
	{28430, 5670},    // LENOVO_3K16_2 (CSO)

	// Lenovo 27" AIO (EN, EZ)
	{54330, 615},     // LENOVO_4K27_AIO (NVT)
	{44592, 58521},   // LENOVO_4K27_AIO_2

	// ViewSonic reference 4K 27" (BR, CT)
	{25434, 14019},   // REFERENCE_4K27_DP (VSC)
	{61245, 12800},   // REFERENCE_4K27_HDMI (OOO)

	// Barco 4K 27" (BT, DM)
	{37640, 55040},   // BARCO_4K27_HDMI (BDS)
	{37640, 179},     // BARCO_4K27_DP (BDS)

	// Sharp 8K 60" (AX)
	{4173, 4490},     // SHARP_8K60_HDMI

	// ONSOR 2.2K 14" (CQ, CR, CS, DK, EE)
	{58633, 2353},    // ONSOR_2K14 (BOE)
	{58633, 3208},    // ONSOR_2K14_2 (BOE)

	// JPIK 4K 27" AIO (EH, EI)
	{58633, 1751},    // JPIK_4K27 (BOE)
	{11612, 9984},    // JPIK_4K27_2 (WAM)

	// TPV reference 4K 27" (D7, DU, DV)
	{37694, 10464},   // TPV_REFERENCE_4K27
	{45830, 10464},   // TPV_REFERENCE_4K27 (AUS path)
	{37694, 10288},   // TPV_REFERENCE_4K27_165Hz

	// Samsung 4K 27" 144Hz (ET) — range of product IDs
	{19501, 57447},   // SAMSUNG_4K27_144HZ (KJL)
	{19501, 57448},
	{19501, 57449},
	{19501, 57450},
	{19501, 57451},
	{19501, 57452},
	{19501, 57453},
	{19501, 57454},
	{19501, 57455},
	{19501, 57456},
	{19501, 57457},
	{19501, 57458},
	{11596, 57447},   // SAMSUNG_4K27_144HZ (SAM)
	{11596, 57448},
	{11596, 57449},
	{11596, 57450},
	{11596, 57451},
	{11596, 57452},
	{11596, 57453},
	{11596, 57454},
	{11596, 57455},
	{11596, 57456},
	{11596, 57457},
	{11596, 57458},
	{19501, 30809},   // SAMSUNG_4K27_144HZ_2 (KJL)
	{19501, 30810},
	{19501, 30811},
	{19501, 30812},
	{19501, 30813},
	{19501, 30814},
	{19501, 30815},
	{19501, 30816},
	{19501, 30817},
	{19501, 30818},
	{19501, 30819},
	{19501, 30820},
	{19501, 30821},
	{19501, 30822},
	{11596, 30809},   // SAMSUNG_4K27_144HZ_2 (SAM)
	{11596, 30810},
	{11596, 30811},
	{11596, 30812},
	{11596, 30813},
	{11596, 30814},
	{11596, 30815},
	{11596, 30816},
	{11596, 30817},
	{11596, 30818},
	{11596, 30819},
	{11596, 30820},
	{11596, 30821},
	{11596, 30822},

	// RTK SR Light (AH) — test device
	{35658, 14400},   // RTK_SRLIGHT
};

// clang-format on

#define LEIA_EDID_TABLE_LEN (sizeof(leia_edid_table) / sizeof(leia_edid_table[0]))
