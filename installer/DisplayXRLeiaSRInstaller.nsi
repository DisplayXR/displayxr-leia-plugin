; DisplayXR Leia SR Plug-in Installer Script
; Copyright 2026, DisplayXR
; SPDX-License-Identifier: Apache-2.0
;
; Ships the Leia SR display-processor plug-in DLL (issue #256 /
; ADR-019 / plan §4.6). Registers it at
; HKLM\Software\DisplayXR\DisplayProcessors\leia-sr so the runtime's
; registry-driven discovery (target_plugin_loader.c) picks it up at
; xrCreateInstance time.
;
; Order-independent (DisplayXR/displayxr-runtime#1803, P-e): no prerequisite.
;  - No DisplayXR runtime yet: the files and the DisplayProcessors
;    registration are written anyway. The registration key is the runtime's
;    public discovery contract and may exist before the runtime does; the
;    runtime picks the plug-in up once it is installed. The only runtime gate
;    is the ABI floor (exit 5), and it applies only when a runtime IS present.
;  - No Leia SR platform check, deliberately: the plug-in loads without SR and
;    reports the platform as absent at run time, so installing it before SR
;    (or on a box that gets SR later) is a supported order.
;  - Files held by running processes (the service, in-process OpenXR apps)
;    are released through Windows Restart Manager (dxr-rm-close.exe), which
;    also restarts what it closed. See docs/installer.md.
;
; The Leia conversion provider (DisplayXR-LeiaConversion.dll, the
; ConversionProviders\leia-conversion row) is NOT part of this installer any
; more: it ships in its own package. Releases of this installer that still
; bundled it left a copy of the DLL in $INSTDIR and owned that row; an upgrade
; or uninstall removes ONLY that leftover (the copy in $INSTDIR, and the row
; only while it still points at that copy) and never touches a row or files
; the separate package owns. See "Superseded conversion provider" below and
; docs/conversion-moved.md.

;--------------------------------
; Build-time definitions (passed from CMake):
;   VERSION, VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, BUILD_NUM
;   BIN_DIR        — _package/bin/ (DisplayXR-LeiaSR.dll lives in BIN_DIR/plugins/)
;   SR_VK_BETA_DLL — abs path to bundled SimulatedRealityVulkanBeta.dll
;   SOURCE_DIR     — repo root (for icon)
;   OUTPUT_DIR     — _package/

!ifndef VERSION
	!define VERSION "1.3.4"
!endif
!ifndef VERSION_MAJOR
	!define VERSION_MAJOR "1"
!endif
!ifndef VERSION_MINOR
	!define VERSION_MINOR "3"
!endif
!ifndef VERSION_PATCH
	!define VERSION_PATCH "4"
!endif
!ifndef BUILD_NUM
	!define BUILD_NUM "0"
!endif

;--------------------------------
; Code signing (SIGN_CMD passed from CMake; empty = unsigned build).
; SIGN_CMD carries no secret — on a signing-capable build machine it points
; at the configured signer; elsewhere it is empty and the build is unsigned.
;
; The installer .exe is signed via !finalize. The UNINSTALLER is signed via a
; two-pass build instead of !uninstfinalize: !uninstfinalize is unreliable —
; the uninstaller NSIS writes at install time is a self-copy of the (signed)
; installer's exe header, so it inherits the INSTALLER's cert-table pointer,
; which dangles past the smaller uninstaller file => effectively unsigned
; (signtool even refuses to re-sign it, 0x800700C1). Confirmed #664: the
; runtime's larger installer happened to survive, leia/shell's did not.
;
; Two-pass (the canonical NSIS recipe, kept in-script so the CMake `installer`
; target needs no change): compile an INNER installer whose only job is to
; WriteUninstaller to %TEMP% and Quit; run it; sign that %TEMP%\Uninstall.exe;
; then File-include the pre-signed uninstaller in the real pass (see .onInit
; and the WriteUninstaller site). INNER is RequestExecutionLevel user so it
; never triggers UAC when run from makensis on a non-elevated build host.
!ifndef INNER
	!ifdef SIGN_CMD
		!if "${SIGN_CMD}" != ""
			!finalize '${SIGN_CMD} "%1"'
			; Build the inner installer (passes every define it needs to
			; compile; OUTPUT_DIR/SIGN_CMD are deliberately omitted).
			!makensis '-DINNER "-DVERSION=${VERSION}" "-DVERSION_MAJOR=${VERSION_MAJOR}" "-DVERSION_MINOR=${VERSION_MINOR}" "-DVERSION_PATCH=${VERSION_PATCH}" "-DBUILD_NUM=${BUILD_NUM}" "-DSOURCE_DIR=${SOURCE_DIR}" "-DBIN_DIR=${BIN_DIR}" "-DSR_VK_BETA_DLL=${SR_VK_BETA_DLL}" "-DMIN_RUNTIME_VERSION=${MIN_RUNTIME_VERSION}" "${__FILE__}"' = 0
			; Run it: .onInit writes %TEMP%\Uninstall.exe then Quit (exit 2).
			!system '"$%TEMP%\DisplayXRLeiaSR_inner.exe"' = 2
			; Sign the emitted uninstaller, then File it in the real pass.
			!system '${SIGN_CMD} "$%TEMP%\Uninstall.exe"' = 0
			!define USE_PRESIGNED_UNINST
		!endif
	!endif
!endif

;--------------------------------
; General Attributes

Name "DisplayXR Leia SR Plug-in ${VERSION}"
!ifdef INNER
	; Throwaway inner installer: only emits the uninstaller to %TEMP%.
	OutFile "$%TEMP%\DisplayXRLeiaSR_inner.exe"
	RequestExecutionLevel user
!else
	OutFile "${OUTPUT_DIR}\DisplayXRLeiaSRSetup-${VERSION}.exe"
	RequestExecutionLevel admin
!endif
InstallDir "$PROGRAMFILES64\DisplayXR\Plugins\LeiaSR"
InstallDirRegKey HKLM "Software\DisplayXR\Plugins\LeiaSR" "InstallPath"
ShowInstDetails show
ShowUninstDetails show
; #461 (runtime repo): a silent install must NEVER skip a locked file and
; exit 0 — that's how the v0.14.0 bundle left an old DLL on disk under a
; new registry Version (displayxr-service had the plug-in mapped). With
; AllowSkipFiles off, a locked file aborts the install with a non-zero
; exit code instead, and the registry below is never written.
AllowSkipFiles off

; Modern UI
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "WordFunc.nsh"
!insertmacro VersionCompare

; ADR-054 N4b: the runtime's provenance gate enforces by default - a plug-in DLL
; must be Authenticode-signed or have its SHA-256 under
; HKLM\Software\DisplayXR\Trust\Allowlist. installer/gen_allowlist_nsh.cmake
; computes the rows for the runtime-loaded DLLs from the exact bytes packed; a hand-run makensis without
; the define writes nothing (a signed release needs no row).
!ifdef ALLOWLIST_NSH
	!include "${ALLOWLIST_NSH}"
!else
	!macro DXR_ALLOWLIST_WRITE
	!macroend
	!macro DXR_ALLOWLIST_REMOVE
	!macroend
!endif

; installer/CMakeLists.txt always passes this (derived from the runtime ABI
; pin, empty for a non-release pin). Default it so a hand-run makensis
; compiles instead of erroring on an unknown constant — an absent define
; means "no version floor", the same as the empty-string case.
!ifndef MIN_RUNTIME_VERSION
	!define MIN_RUNTIME_VERSION ""
!endif

;--------------------------------
; Abort a pre-flight check without ever showing a modal in silent mode.
;
; NSIS does NOT suppress MessageBox under /S — it shows and blocks forever.
; The meta-bundle runs every child installer as `ExecWait '<child> /S'`, so a
; modal here would hang the whole bundle chain with no visible window. In
; silent mode set a distinct exit code instead: the bundle already checks
; `$0 != 0` and reports "installer exited with code $0. Aborting bundle."
;
; Codes: 3 = not 64-bit, 5 = runtime below the ABI floor. (4 was "runtime
; absent" — retired: the plug-in now installs before the runtime. 2 is
; reserved — the INNER uninstaller-emitting pass exits 2 by design.)
!macro AbortWithReason CODE MSG
	${If} ${Silent}
		SetErrorLevel ${CODE}
	${Else}
		MessageBox MB_ICONSTOP "${MSG}" /SD IDOK
	${EndIf}
	Abort
!macroend

;--------------------------------
; Releasing the plug-in's files: Restart Manager (dxr-rm-close.exe).
;
; The plug-in DLL (and its Vulkan weavers) are mapped by displayxr-service.exe
; for its whole life, and by any in-process OpenXR app. Replacing or deleting
; them needs those processes closed. Windows Restart Manager does that
; generically: it finds the holders, closes them (the service exits cleanly on
; the RM close query and registered "--rm-restart" for RM to relaunch it,
; non-elevated), and after the files are swapped restarts exactly what it
; closed. Only the process that started an RM session may shut down/restart in
; it, so dxr-rm-close runs ONE "session" process across both phases and talks
; to us through files in $RmDir (closed.txt / go.txt / done.txt / log.txt).
;
; Fallback, only when RM itself fails or the helper is missing: taskkill /f
; the service and start it again through explorer.exe — never a plain Exec
; from this elevated installer, which would leave an ELEVATED service that
; normal-integrity apps cannot reach.

!define SVC_EXE "displayxr-service.exe"

Var RmHelper      ; path of the dxr-rm-close.exe copy we run (in $PLUGINSDIR)
Var RmDir         ; RM session state dir
Var RmClosedRc    ; closed.txt code: 0 closed, 10 nothing held, 1 RM failed, or none/timeout/noexec/nohelper
Var RmLogLines    ; log.txt lines already DetailPrinted
Var SvcWasRunning ; 1 = displayxr-service.exe was running before we touched anything
Var NoStart       ; 1 = /NOSTART: never start or restart anything

; Defines the RM helper functions for the installer (PFX="") and the
; uninstaller (PFX="un.") — NSIS requires separate copies.
!macro RM_FUNCTIONS PFX

; First line of $0 (a marker file), CR/LF trimmed -> $1 ("" when unreadable).
Function ${PFX}RmReadCode
	StrCpy $1 ""
	ClearErrors
	FileOpen $2 "$0" r
	${IfNot} ${Errors}
		FileRead $2 $1
		FileClose $2
	${EndIf}
	${Do}
		StrCpy $2 $1 1 -1
		${If} $2 == "$\r"
		${OrIf} $2 == "$\n"
			StrCpy $1 $1 -1
		${Else}
			${ExitDo}
		${EndIf}
	${Loop}
FunctionEnd

; DetailPrint the helper's log.txt lines not printed yet.
Function ${PFX}RmPrintLog
	ClearErrors
	FileOpen $2 "$RmDir\log.txt" r
	${If} ${Errors}
		Return
	${EndIf}
	StrCpy $3 0
	${Do}
		ClearErrors
		FileRead $2 $1
		${If} ${Errors}
			${ExitDo}
		${EndIf}
		IntOp $3 $3 + 1
		${If} $3 > $RmLogLines
			; trim CR/LF
			${Do}
				StrCpy $4 $1 1 -1
				${If} $4 == "$\r"
				${OrIf} $4 == "$\n"
					StrCpy $1 $1 -1
				${Else}
					${ExitDo}
				${EndIf}
			${Loop}
			DetailPrint "  [rm] $1"
		${EndIf}
	${Loop}
	FileClose $2
	StrCpy $RmLogLines $3
FunctionEnd

; Wait up to $0 ms for file $1. Sets error flag on timeout.
Function ${PFX}RmWaitFile
	StrCpy $2 0
	ClearErrors
	${Do}
		${If} ${FileExists} "$1"
			Return
		${EndIf}
		${If} $2 >= $0
			SetErrors
			Return
		${EndIf}
		Sleep 200
		IntOp $2 $2 + 200
	${Loop}
FunctionEnd

; Phase 1: close every process holding the plug-in's files in $INSTDIR.
; Requires $RmHelper; sets $SvcWasRunning and $RmClosedRc.
Function ${PFX}RmClose
	StrCpy $SvcWasRunning 0
	StrCpy $RmLogLines 0
	StrCpy $RmDir "$PLUGINSDIR\rm"
	${IfNot} ${FileExists} "$RmHelper"
		StrCpy $RmClosedRc "nohelper"
		DetailPrint "Restart Manager helper missing: falling back to taskkill."
		nsExec::ExecToLog 'taskkill /f /im ${SVC_EXE}'
		Pop $0
		${If} $0 == 0
			StrCpy $SvcWasRunning 1
			Sleep 1500 ; let the killed process release its file handles
		${EndIf}
		Return
	${EndIf}

	nsExec::Exec '"$RmHelper" is-running ${SVC_EXE}'
	Pop $0
	${If} $0 == 0
		StrCpy $SvcWasRunning 1
	${EndIf}

	RMDir /r "$RmDir"
	CreateDirectory "$RmDir"
	DetailPrint "Releasing the plug-in's files (Restart Manager)..."
	; Not waited on (it lives until RmFinish); ExecShell + SW_HIDE so the
	; console helper shows no window.
	ClearErrors
	ExecShell "open" '"$RmHelper"' 'session "$RmDir" "$INSTDIR\DisplayXR-LeiaSR.dll" "$INSTDIR\SimulatedRealityVulkan.dll" "$INSTDIR\SimulatedRealityVulkanBeta.dll"' SW_HIDE
	${If} ${Errors}
		StrCpy $RmClosedRc "noexec"
	${Else}
		; RmForceShutdown waits ~5 s per unresponsive window before its 30 s
		; force; 120 s is far beyond any measured close (<= 11 s).
		StrCpy $0 120000
		StrCpy $1 "$RmDir\closed.txt"
		Call ${PFX}RmWaitFile
		${If} ${Errors}
			StrCpy $RmClosedRc "timeout"
		${Else}
			StrCpy $0 "$RmDir\closed.txt"
			Call ${PFX}RmReadCode
			StrCpy $RmClosedRc $1
		${EndIf}
		Call ${PFX}RmPrintLog
	${EndIf}

	${If} $RmClosedRc == "0"
		DetailPrint "Restart Manager closed the processes holding the plug-in."
	${ElseIf} $RmClosedRc == "10"
		DetailPrint "No process holds the plug-in's files."
	${Else}
		; RM could not do it (or the helper did not run): last resort.
		DetailPrint "Restart Manager did not release the files ($RmClosedRc): falling back to taskkill."
		${If} $SvcWasRunning == 1
			nsExec::ExecToLog 'taskkill /f /im ${SVC_EXE}'
			Pop $0
			Sleep 1500 ; let the killed process release its file handles
		${EndIf}
	${EndIf}
FunctionEnd

; Phase 2: after the files were replaced/deleted. Unless /NOSTART, RM restarts
; what it closed; then, if the service was running before and is not now,
; start it through the shell (non-elevated).
Function ${PFX}RmFinish
	${If} $RmClosedRc == "0"
	${OrIf} $RmClosedRc == "1"
	${OrIf} $RmClosedRc == "timeout"
		; A session process is waiting for go.txt.
		${If} $NoStart == 1
			StrCpy $1 "end"
		${Else}
			StrCpy $1 "restart"
		${EndIf}
		FileOpen $2 "$RmDir\go.txt" w
		FileWrite $2 "$1$\r$\n"
		FileClose $2
		StrCpy $0 60000
		StrCpy $1 "$RmDir\done.txt"
		Call ${PFX}RmWaitFile
		${If} ${Errors}
			DetailPrint "Restart Manager helper did not finish in time."
		${EndIf}
		Call ${PFX}RmPrintLog
	${EndIf}

	${If} $NoStart == 1
		DetailPrint "Not starting the DisplayXR service (/NOSTART)."
		Return
	${EndIf}
	${If} $SvcWasRunning != 1
		; It was not running before: start nothing new. The next OpenXR app
		; (or the next logon) starts it.
		Return
	${EndIf}
	; RmRestart may relaunch it via a short-lived hand-off (an elevated
	; --rm-restart instance re-launches itself through explorer.exe), so give
	; it a few seconds to appear before deciding it is gone.
	${If} ${FileExists} "$RmHelper"
		nsExec::Exec '"$RmHelper" is-running ${SVC_EXE} 8000'
		Pop $0
	${Else}
		StrCpy $0 1
	${EndIf}
	${If} $0 == 0
		DetailPrint "DisplayXR service is running."
		Return
	${EndIf}
	SetRegView 64
	ReadRegStr $0 HKLM "Software\DisplayXR\Runtime" "InstallPath"
	${If} $0 != ""
	${AndIf} ${FileExists} "$0\${SVC_EXE}"
		; explorer.exe starts it in the desktop shell's non-elevated context.
		DetailPrint "Starting the DisplayXR service (non-elevated, via explorer.exe)..."
		Exec '"$WINDIR\explorer.exe" "$0\${SVC_EXE}"'
	${EndIf}
FunctionEnd

!macroend

!insertmacro RM_FUNCTIONS ""
!insertmacro RM_FUNCTIONS "un."

;--------------------------------
; Superseded conversion provider (handover to the separate package).
;
; Releases of this installer up to v2.23.4 (v2.24.0 is the first without it)
; shipped DisplayXR-LeiaConversion.dll in $INSTDIR and wrote
; HKLM\Software\DisplayXR\ConversionProviders\leia-conversion with THIS
; uninstaller in UninstallString. The provider now ships in its own package
; (its own directory, its own uninstaller), which takes that row over when it
; is installed. What is left for this installer and uninstaller to do:
;
;   - the copy in $INSTDIR is ours and is always removed;
;   - the row is removed ONLY while its Binary still names that copy — i.e.
;     nobody has taken it over. A row whose Binary is anywhere else belongs to
;     the separate package (or to whatever wrote it) and is never read past
;     that one value, never rewritten, never deleted;
;   - a Settings subkey the user created is kept either way, so the separate
;     package finds it again.
;
; Removing a registered copy follows the provider update sequence (runtime
; conversion-provider-discovery.md §3.8), because the conversion host may have
; it mapped:
;   1. create AND own Global\DisplayXR-ConvMaintenance-leia-conversion, held to
;      the end (the service spawns no host for the provider while it is held);
;   2. a Restart Manager session over that one file — it lists the host, which
;      exits 20 ("closed for update"); every app session stays alive;
;   3. delete the row's values (Version FIRST: the "row complete" marker) and
;      the file;
;   4. end the session WITHOUT restarting anything;
;   5. release the mutex.
; The mutex is owned by the thread that created it, so take and release it from
; the same section (NSIS runs sections and their Calls on one thread).
;
; After this, a box that has not installed the separate package has no Leia
; conversion: the runtime uses its next conversion provider. So when this
; installer has just removed a WORKING registration it tries to bring the
; separate package in (ConvChainPackage, at the very end of the section).

!define CONV_ID "leia-conversion"
!define CONV_DLL "DisplayXR-LeiaConversion.dll"
!define CONV_KEY "Software\DisplayXR\ConversionProviders\${CONV_ID}"
!define CONV_MUTEX "Global\DisplayXR-ConvMaintenance-${CONV_ID}"
; The separate package: its component name in the DisplayXR component catalog
; (what `dxr-get install` takes) and where a person gets it by hand.
!define CONV_CATALOG_COMPONENT "leia_conversion"
!define CONV_PKG_URL "https://github.com/DisplayXR/displayxr-leia-conversion/releases"

Var ConvMutex     ; owned maintenance mutex handle ("" = not held)
Var ConvRmDir     ; the superseded copy's own RM session state dir
Var ConvRmRc      ; its closed.txt code (as $RmClosedRc); "" = no session was started
Var ConvLegacy    ; 1 = a superseded copy of the provider is in $INSTDIR
Var ConvRowOurs   ; 1 = the row's Binary still names that copy (the row is this installer's leftover)
Var SvcRunning0   ; 1 = the service was running before ANY session touched it

!macro CONV_FUNCTIONS PFX

; Step 1: create and own the maintenance mutex. ERROR_ALREADY_EXISTS (183) means
; another installer (the conversion provider's own package, or the conversion
; module's) holds it: wait for it.
Function ${PFX}ConvMaintenanceTake
	StrCpy $ConvMutex ""
	System::Call 'kernel32::CreateMutexW(p 0, i 1, w "${CONV_MUTEX}") p.r0 ?e'
	Pop $1
	${If} $0 == 0
		DetailPrint "Could not create the conversion maintenance mutex (error $1) - continuing without it."
		Return
	${EndIf}
	${If} $1 == 183
		DetailPrint "Another installer is updating the conversion provider; waiting..."
		System::Call 'kernel32::WaitForSingleObject(p r0, i 120000) i.r2'
		; 0 = acquired, 0x80 = abandoned by its owner (acquired too)
		${If} $2 != 0
		${AndIf} $2 != 128
			DetailPrint "The conversion maintenance mutex is still held after 120 s - continuing without it."
			System::Call 'kernel32::CloseHandle(p r0)'
			Return
		${EndIf}
	${EndIf}
	StrCpy $ConvMutex $0
	DetailPrint "Holding ${CONV_MUTEX}."
FunctionEnd

; Step 5.
Function ${PFX}ConvMaintenanceRelease
	${If} $ConvMutex != ""
		System::Call 'kernel32::ReleaseMutex(p $ConvMutex)'
		System::Call 'kernel32::CloseHandle(p $ConvMutex)'
		StrCpy $ConvMutex ""
		DetailPrint "Released ${CONV_MUTEX}."
	${EndIf}
FunctionEnd

; Step 2: Restart Manager over the superseded copy ONLY. No taskkill fallback:
; nothing here may stop the service — a file still held is removed at the next
; reboot instead (UnDeleteHeld).
Function ${PFX}ConvRmClose
	StrCpy $ConvRmDir "$PLUGINSDIR\rmconv"
	${IfNot} ${FileExists} "$RmHelper"
		StrCpy $ConvRmRc "nohelper"
		Return
	${EndIf}
	RMDir /r "$ConvRmDir"
	CreateDirectory "$ConvRmDir"
	DetailPrint "Releasing the conversion provider's files (Restart Manager)..."
	ClearErrors
	ExecShell "open" '"$RmHelper"' 'session "$ConvRmDir" "$INSTDIR\${CONV_DLL}"' SW_HIDE
	${If} ${Errors}
		StrCpy $ConvRmRc "noexec"
		Return
	${EndIf}
	StrCpy $0 120000
	StrCpy $1 "$ConvRmDir\closed.txt"
	Call ${PFX}RmWaitFile
	${If} ${Errors}
		StrCpy $ConvRmRc "timeout"
	${Else}
		StrCpy $0 "$ConvRmDir\closed.txt"
		Call ${PFX}RmReadCode
		StrCpy $ConvRmRc $1
	${EndIf}
	Call ${PFX}ConvRmPrintLog
	${If} $ConvRmRc == "0"
		DetailPrint "Restart Manager closed the conversion host (it exits 'closed for update')."
	${ElseIf} $ConvRmRc == "10"
		DetailPrint "No process holds the conversion provider's files."
	${Else}
		DetailPrint "Restart Manager did not release the conversion provider's files ($ConvRmRc)."
	${EndIf}
FunctionEnd

; Step 4: end the session; restart NOTHING. Safe to call when no session was
; started ($ConvRmRc is then "" or "ended").
Function ${PFX}ConvRmEnd
	${If} $ConvRmRc == "0"
	${OrIf} $ConvRmRc == "1"
	${OrIf} $ConvRmRc == "timeout"
		FileOpen $2 "$ConvRmDir\go.txt" w
		FileWrite $2 "end$\r$\n"
		FileClose $2
		StrCpy $0 60000
		StrCpy $1 "$ConvRmDir\done.txt"
		Call ${PFX}RmWaitFile
		Call ${PFX}ConvRmPrintLog
	${EndIf}
	StrCpy $ConvRmRc "ended"
FunctionEnd

; The helper's log for the provider session (RmPrintLog reads $RmDir).
Function ${PFX}ConvRmPrintLog
	Push $RmDir
	Push $RmLogLines
	StrCpy $RmDir $ConvRmDir
	StrCpy $RmLogLines 0
	Call ${PFX}RmPrintLog
	Pop $RmLogLines
	Pop $RmDir
FunctionEnd

; Find the leftover: $ConvLegacy / $ConvRowOurs (see the section header).
Function ${PFX}ConvLegacyDetect
	StrCpy $ConvLegacy 0
	StrCpy $ConvRowOurs 0
	StrCpy $ConvRmRc ""
	${If} ${FileExists} "$INSTDIR\${CONV_DLL}"
		StrCpy $ConvLegacy 1
	${EndIf}
	ReadRegStr $0 HKLM "${CONV_KEY}" "Binary"
	${If} $0 == "$INSTDIR\${CONV_DLL}"
		StrCpy $ConvRowOurs 1
	${ElseIf} $0 != ""
		DetailPrint "Conversion provider ${CONV_ID} is registered at $0 (its own package): left untouched."
	${EndIf}
FunctionEnd

; Steps 1-2, only when there is something of ours to remove.
Function ${PFX}ConvLegacyClose
	${If} $ConvLegacy == 1
	${OrIf} $ConvRowOurs == 1
		DetailPrint "Removing the conversion provider this plug-in used to bundle (it now ships in its own package)."
		Call ${PFX}ConvMaintenanceTake
		${If} $ConvLegacy == 1
			Call ${PFX}ConvRmClose
		${EndIf}
	${EndIf}
FunctionEnd

; Step 3 (registry half): drop the registration this installer wrote, and only
; that. Version goes first (the "row complete" marker), so the runtime never
; sees a half-removed row as a provider; the key itself is removed only when
; nothing else (a user's Settings subkey) is left in it.
Function ${PFX}ConvLegacyUnregister
	${If} $ConvRowOurs == 1
		DeleteRegValue HKLM "${CONV_KEY}" "Version"
		DeleteRegValue HKLM "${CONV_KEY}" "Binary"
		DeleteRegValue HKLM "${CONV_KEY}" "DisplayName"
		DeleteRegValue HKLM "${CONV_KEY}" "Vendor"
		DeleteRegValue HKLM "${CONV_KEY}" "UninstallString"
		DeleteRegValue HKLM "${CONV_KEY}" "Supersedes"
		DeleteRegValue HKLM "${CONV_KEY}" "ProbeOrder"
		DeleteRegKey /ifempty HKLM "${CONV_KEY}"
		DeleteRegKey /ifempty HKLM "Software\DisplayXR\ConversionProviders"
		DetailPrint "Removed this plug-in's ${CONV_ID} registration (the provider ships in its own package now)."
	${EndIf}
FunctionEnd

; Steps 4-5.
Function ${PFX}ConvLegacyDone
	Call ${PFX}ConvRmEnd
	Call ${PFX}ConvMaintenanceRelease
FunctionEnd

!macroend

!insertmacro CONV_FUNCTIONS ""
!insertmacro CONV_FUNCTIONS "un."

; Chain the separate conversion-provider package (installer only).
;
; When: this run removed this plug-in's own, working leia-conversion
; registration ($ConvRowOurs = 1: the box HAD Leia conversion and would lose
; it), or the caller asked with /WITHCONVERSION. Never under /NOCONVERSION (a
; bundle that installs the package itself passes it), and never when a
; leia-conversion registration exists by now.
;
; How: `dxr-get install leia_conversion`, the runtime's fetcher (runtime
; docs/specs/runtime/dxr-get.md): it downloads the installer named by the signed
; component catalog, checks size, SHA-256 and the Authenticode signer, and runs
; it silently; it never downgrades. This installer uses the copy the DisplayXR
; runtime installs next to itself, so there is no chain on a box without a
; runtime that has dxr-get. It runs after everything else (the maintenance mutex
; is released, the service is back): the chained installer takes that mutex and
; runs its own Restart Manager session.
;
; The PUBLISHED catalog has to carry a `leia_conversion` component for this to
; fetch anything. The package itself is released, but the catalog an installed
; dxr-get reads does not list it yet: dxr-get then exits 40 ("not in the
; catalog") and this prints where to get the installer (CONV_PKG_URL) instead.
; It starts fetching, with no change here, once the catalog carries the
; component. Whatever happens here, the plug-in's own install has already
; succeeded and stays successful: nothing below aborts.
;
; Interactive runs ask once before downloading; /S goes straight through.
Function ConvChainPackage
	${GetParameters} $R0
	ClearErrors
	${GetOptions} $R0 "/NOCONVERSION" $R1
	${IfNot} ${Errors}
		DetailPrint "Not fetching the Leia conversion provider (/NOCONVERSION)."
		Return
	${EndIf}
	StrCpy $R2 $ConvRowOurs
	ClearErrors
	${GetOptions} $R0 "/WITHCONVERSION" $R1
	${IfNot} ${Errors}
		StrCpy $R2 1
	${EndIf}
	${If} $R2 != 1
		Return
	${EndIf}
	ReadRegStr $0 HKLM "${CONV_KEY}" "Binary"
	${If} $0 != ""
		DetailPrint "Leia conversion provider already registered at $0."
		Return
	${EndIf}

	ReadRegStr $1 HKLM "Software\DisplayXR\Runtime" "InstallPath"
	${If} $1 == ""
	${OrIfNot} ${FileExists} "$1\dxr-get.exe"
		DetailPrint "The Leia conversion provider (2D-to-3D through NeurD) is a separate package now; no dxr-get on this box to fetch it."
		DetailPrint "Get it from ${CONV_PKG_URL}"
		Return
	${EndIf}

	MessageBox MB_YESNO|MB_ICONQUESTION "2D-to-3D conversion through NeurD now ships as a separate package, the Leia conversion provider.$\r$\n$\r$\nDownload and install it now?" /SD IDYES IDYES conv_fetch
		DetailPrint "Leia conversion provider not installed (declined). Get it later from ${CONV_PKG_URL}"
		Return
	conv_fetch:
	DetailPrint "Fetching the Leia conversion provider (dxr-get install ${CONV_CATALOG_COMPONENT})..."
	nsExec::ExecToLog '"$1\dxr-get.exe" install ${CONV_CATALOG_COMPONENT} --yes'
	Pop $0
	DetailPrint "  dxr-get install exit: $0"
	ReadRegStr $2 HKLM "${CONV_KEY}" "Binary"
	${If} $0 == "0"
	${AndIf} $2 != ""
		DetailPrint "Leia conversion provider installed ($2)."
	${Else}
		${If} $0 == "40"
			DetailPrint "The DisplayXR component catalog does not list the Leia conversion provider (yet)."
		${ElseIf} $0 == "10"
			DetailPrint "The DisplayXR component catalog could not be reached."
		${EndIf}
		DetailPrint "Leia conversion provider NOT installed. Get it from ${CONV_PKG_URL}"
	${EndIf}
FunctionEnd

; Delete FILE; when a process RM could not close still maps it, remove it at
; the next reboot rather than silently leaving it behind.
!macro UnDeleteHeld FILE
	ClearErrors
	Delete "${FILE}"
	${If} ${Errors}
		; Still mapped by a process RM could not close: remove it at reboot
		; rather than silently leaving it behind.
		Delete /REBOOTOK "${FILE}"
		DetailPrint "${FILE} is still in use; it will be removed at the next reboot."
	${EndIf}
!macroend

;--------------------------------
; Interface Settings

!define MUI_ABORTWARNING
!define MUI_ICON "${SOURCE_DIR}\assets\displayxr_white.ico"
!define MUI_UNICON "${SOURCE_DIR}\assets\displayxr_white.ico"

;--------------------------------
; Pages

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SOURCE_DIR}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

;--------------------------------
; Languages

!insertmacro MUI_LANGUAGE "English"

;--------------------------------
; Installer Sections

Section "Leia SR Plug-in" SecPlugin
	SectionIn RO

	; Force 64-bit registry view — NSIS is 32-bit and would otherwise
	; land at HKLM\Software\WOW6432Node\DisplayXR\* (mismatch with the
	; runtime's 64-bit-view contract).
	SetRegView 64

	; No runtime prerequisite (the runtime is not needed to install; it
	; loads the plug-in once it is there) — just say which case this is.
	ReadRegStr $0 HKLM "Software\DisplayXR\Runtime" "InstallPath"
	${If} $0 == ""
		DetailPrint "DisplayXR runtime not installed yet: the plug-in will be used once the DisplayXR runtime is installed."
	${Else}
		DetailPrint "DisplayXR runtime found at $0"
	${EndIf}

	; '/NOSTART' (passed by the bundle, which restarts the service ONCE after
	; the whole chain) — never start or restart anything. Unknown switches
	; are ignored by older installers, so callers can always pass it.
	StrCpy $NoStart 0
	${GetParameters} $R0
	ClearErrors
	${GetOptions} $R0 "/NOSTART" $R1
	${IfNot} ${Errors}
		StrCpy $NoStart 1
	${EndIf}

	; #461: the files must be released BEFORE the File steps — a locked DLL
	; aborts the install (AllowSkipFiles off above). Restart Manager closes
	; whoever maps them (the service, in-process apps) and restarts them once
	; the new files are in place (RmFinish below).
	InitPluginsDir
	File "/oname=$PLUGINSDIR\dxr-rm-close.exe" "${BIN_DIR}\tools\dxr-rm-close.exe"
	StrCpy $RmHelper "$PLUGINSDIR\dxr-rm-close.exe"
	nsExec::Exec '"$RmHelper" is-running ${SVC_EXE}'
	Pop $0
	StrCpy $SvcRunning0 0
	${If} $0 == 0
		StrCpy $SvcRunning0 1
	${EndIf}
	; A conversion provider left by an older release of this installer goes
	; first, under its maintenance mutex: its session closes the conversion
	; host only (see "Superseded conversion provider" above).
	Call ConvLegacyDetect
	Call ConvLegacyClose
	Call RmClose
	; The display plug-in's RmFinish restores the service only if it was running
	; before; count "before" from before EITHER session.
	${If} $SvcRunning0 == 1
		StrCpy $SvcWasRunning 1
	${EndIf}

	SetOutPath "$INSTDIR"

	; Install the plug-in DLL.
	File "${BIN_DIR}\plugins\DisplayXR-LeiaSR.dll"

	; Bundle the SR Vulkan weavers alongside the plug-in DLL. These are no
	; longer delay-loaded imports resolved by base name — the plug-in picks one
	; at runtime and LoadLibrary's it by absolute path from this directory
	; (leia_vk_weaver_select.cpp), because the weaver's vtable is not stable
	; across LeiaSR versions and the choice depends on the installed core.
	; Apps on boxes that only use the D3D11 weaver never load either DLL.
	;
	; Source the copy that build staging placed in BIN_DIR\plugins (the
	; tree the signing step covers) rather than the raw SDK dir, so the
	; bundled DLL ships code-signed — SAC checks it at delay-load time.
	; (${SR_VK_BETA_DLL} is the unsigned SDK dir; kept defined but unused.)
	;
	; TWO weavers ship, and the plug-in picks between them at runtime
	; (leia_vk_weaver_select.cpp):
	;   SimulatedRealityVulkanBeta.dll  pre-stamp vtable, for SR <= 1.34.x
	;   SimulatedRealityVulkan.dll      stamp-aware (ST-5318), for SR 1.36.x
	; SR >= 1.37 installs its own SimulatedRealityVulkan.dll on PATH and neither
	; of these is loaded. Shipping only the pre-stamp one is what made 1.36/1.37
	; machines render a coloured replica of the scene.
	File "${BIN_DIR}\plugins\SimulatedRealityVulkanBeta.dll"
	File "${BIN_DIR}\plugins\SimulatedRealityVulkan.dll"

	; The Restart Manager helper, next to Uninstall.exe: the uninstaller needs
	; it to release the DLL before deleting it.
	File "${BIN_DIR}\tools\dxr-rm-close.exe"

	; The superseded conversion provider: unregister it (only if the row is
	; still this installer's) and remove the copy an older release left here.
	Call ConvLegacyUnregister
	${If} $ConvLegacy == 1
		!insertmacro UnDeleteHeld "$INSTDIR\${CONV_DLL}"
	${EndIf}

	; ADR-054 N4b: digests of the DLLs just written, so the enforcing provenance
	; gate admits them even when this build is unsigned.
	!insertmacro DXR_ALLOWLIST_WRITE

	; -----------------------------------------------------------------
	; Register at HKLM\Software\DisplayXR\DisplayProcessors\leia-sr per
	; the contract in docs/specs/runtime/plugin-discovery.md.
	;
	; ProbeOrder=50 — lower than sim-display's 200, so on systems with
	; Leia hardware this plug-in's probe runs first and wins. On
	; systems without Leia hardware, probe declines via
	; XRT_ERROR_PROBER_NOT_SUPPORTED and sim-display takes over.
	;
	; UninstallString: informational for older runtime uninstallers that
	; cascade into vendor uninstallers. The plug-in's own uninstall (ARP)
	; works with or without the runtime present.
	; -----------------------------------------------------------------
	WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" \
		"Binary"          "$INSTDIR\DisplayXR-LeiaSR.dll"
	WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" \
		"DisplayName"     "DisplayXR Leia SR"
	WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" \
		"Vendor"          "Leia Inc."
	WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" \
		"Version"         "${VERSION}"
	WriteRegStr   HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" \
		"UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
	WriteRegDWORD HKLM "Software\DisplayXR\DisplayProcessors\leia-sr" \
		"ProbeOrder"      50

	; Track our own install location for the uninstaller.
	WriteRegStr HKLM "Software\DisplayXR\Plugins\LeiaSR" "InstallPath" "$INSTDIR"
	WriteRegStr HKLM "Software\DisplayXR\Plugins\LeiaSR" "Version"     "${VERSION}"

	; Write uninstaller. In a signed build, install the pre-signed uninstaller
	; produced by the inner pass (two-pass signing — see the code-signing block
	; in the header). Otherwise (unsigned build / CI) write it normally.
!ifdef USE_PRESIGNED_UNINST
	File "/oname=Uninstall.exe" "$%TEMP%\Uninstall.exe"
!else
	WriteUninstaller "$INSTDIR\Uninstall.exe"
!endif

	; Add to Add/Remove Programs.
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"DisplayName" "DisplayXR Leia SR Plug-in"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"InstallLocation" "$INSTDIR"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"DisplayIcon" "$INSTDIR\DisplayXR-LeiaSR.dll"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"Publisher" "Leia Inc."
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"DisplayVersion" "${VERSION}"
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"VersionMajor" ${VERSION_MAJOR}
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"VersionMinor" ${VERSION_MINOR}
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"NoModify" 1
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"NoRepair" 1

	; Calculate installed size.
	${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
	IntFmt $0 "0x%08X" $0
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR" \
		"EstimatedSize" "$0"

	; The superseded provider's session ends restarting nothing; then the
	; mutex goes.
	Call ConvLegacyDone

	; #461: bring back what Restart Manager closed (the service relaunches
	; itself non-elevated with --rm-restart), or — only if RM failed — start
	; the service via explorer.exe. Nothing is started that was not running
	; before, and nothing at all under /NOSTART.
	Call RmFinish

	; Last, and never fatal: bring in the separate conversion-provider package
	; when this run took a working Leia conversion away (see ConvChainPackage).
	Call ConvChainPackage

SectionEnd

; An aborted install (e.g. a file still locked) must not leave the processes
; RM closed down: release the waiting helpers, the maintenance mutex, and
; restore the service.
Function .onInstFailed
	Call ConvLegacyDone
	Call RmFinish
FunctionEnd

;--------------------------------
; Uninstaller

Section "Uninstall"
	SetRegView 64

	StrCpy $NoStart 0
	${GetParameters} $R0
	ClearErrors
	${GetOptions} $R0 "/NOSTART" $R1
	${IfNot} ${Errors}
		StrCpy $NoStart 1
	${EndIf}

	; Remove the registry subkey first so a service restarted below (or by
	; anyone else) does not find us; it then runs on the fallback display
	; processor.
	DeleteRegKey HKLM "Software\DisplayXR\DisplayProcessors\leia-sr"
	; ADR-054 N4b: drop only the allowlist rows this installer wrote.
	!insertmacro DXR_ALLOWLIST_REMOVE
	; A conversion-provider registration an older release of this installer
	; wrote, and only that one: a row the separate package owns stays.
	Call un.ConvLegacyDetect
	Call un.ConvLegacyUnregister

	; Release the DLL before deleting it: the service and in-process apps map
	; it, and a plain Delete of a mapped DLL fails (it used to fail silently and
	; leave the file behind). Works with or without the runtime installed —
	; with no service and no app holding the files RM simply finds nothing.
	; Run a copy of the helper from $PLUGINSDIR so $INSTDIR can be removed.
	InitPluginsDir
	StrCpy $RmHelper "$PLUGINSDIR\dxr-rm-close.exe"
	CopyFiles /SILENT "$INSTDIR\dxr-rm-close.exe" "$PLUGINSDIR"
	nsExec::Exec '"$RmHelper" is-running ${SVC_EXE}'
	Pop $0
	StrCpy $SvcRunning0 0
	${If} $0 == 0
		StrCpy $SvcRunning0 1
	${EndIf}
	Call un.ConvLegacyClose
	Call un.RmClose
	${If} $SvcRunning0 == 1
		StrCpy $SvcWasRunning 1
	${EndIf}

	; Remove our files.
	!insertmacro UnDeleteHeld "$INSTDIR\DisplayXR-LeiaSR.dll"
	!insertmacro UnDeleteHeld "$INSTDIR\SimulatedRealityVulkanBeta.dll"
	!insertmacro UnDeleteHeld "$INSTDIR\SimulatedRealityVulkan.dll"
	${If} $ConvLegacy == 1
		!insertmacro UnDeleteHeld "$INSTDIR\${CONV_DLL}"
	${EndIf}
	Delete "$INSTDIR\dxr-rm-close.exe"
	Delete "$INSTDIR\Uninstall.exe"

	Call un.ConvLegacyDone

	; Bring back what RM closed — the service comes back without the plug-in.
	Call un.RmFinish

	; Remove install dir (at reboot, if a held file was deferred above).
	RMDir /REBOOTOK "$INSTDIR"
	RMDir "$PROGRAMFILES64\DisplayXR\Plugins"
	; Don't RMDir $PROGRAMFILES64\DisplayXR — the runtime's uninstaller
	; owns that directory.

	; Remove tracking + Add/Remove entry.
	DeleteRegKey HKLM "Software\DisplayXR\Plugins\LeiaSR"
	DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRLeiaSR"

	; Drop the now-empty Plugins parent if no other plug-ins remain.
	DeleteRegKey /ifempty HKLM "Software\DisplayXR\Plugins"

SectionEnd

Function un.onUninstFailed
	Call un.ConvLegacyDone
	Call un.RmFinish
FunctionEnd

;--------------------------------
; Section Descriptions

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
	!insertmacro MUI_DESCRIPTION_TEXT ${SecPlugin} "DisplayXR Leia SR display-processor plug-in (required)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

;--------------------------------
; Installer Functions

Function .onInit
!ifdef INNER
	; Inner pass only: emit the uninstaller to %TEMP% (this is the binary that
	; gets signed and File-included by the real pass) then bail immediately —
	; no UI, no install. This whole path is absent from the real installer.
	SetSilent silent
	WriteUninstaller "$%TEMP%\Uninstall.exe"
	Quit
!endif
	${IfNot} ${RunningX64}
		!insertmacro AbortWithReason 3 "DisplayXR Leia SR Plug-in requires 64-bit Windows."
	${EndIf}

	; -----------------------------------------------------------------
	; ABI-pairing floor — only when a runtime IS installed.
	;
	; No runtime at all is fine (#1803 P-e): the plug-in is installed and
	; registered, and the runtime loads it once it is installed. There is
	; deliberately no Leia SR platform check either: the plug-in loads
	; without SR and reports the platform as absent at run time, so SR may
	; be installed before or after this.
	;
	; The runtime's loader rejects a plug-in whose plug-in-API major differs
	; from its own and then falls back to sim-display (ProbeOrder 200) — the
	; user sees "3D stopped working" with the only evidence a WARN line in a
	; log they'll never open. ADR-020 rule 3. This plug-in is compiled against
	; runtime ${MIN_RUNTIME_VERSION} headers, so installing it over an older
	; runtime guarantees that silent fallback.
	;
	; The meta-bundle installs the matching pair and can't reach this state,
	; but it only compares VERSIONS, never ABI — so a standalone run of this
	; installer on an older runtime is the one path left. Refuse it here,
	; loudly, instead of shipping a plug-in that will be skipped at runtime.
	;
	; NSIS is a 32-bit binary: HKLM reads are redirected through WOW6432Node
	; by default, while the runtime writes the 64-bit view. Match it.
	SetRegView 64
	ReadRegStr $0 HKLM "Software\DisplayXR\Runtime" "InstallPath"
	ReadRegStr $1 HKLM "Software\DisplayXR\Runtime" "Version"
	SetRegView 32

	; MIN_RUNTIME_VERSION is empty when the plug-in was built against a
	; non-release runtime pin (branch/SHA) — there is no ordered version to
	; compare, so the floor is omitted rather than guessed. See
	; installer/CMakeLists.txt.
	!if "${MIN_RUNTIME_VERSION}" != ""
		; $R0: 0 = equal, 1 = installed newer, 2 = floor newer (too old).
		; Installed-first, floor-second — inverting the operands silently
		; inverts the gate.
		; Gate only on a recorded Version: no runtime (or none recorded) means
		; nothing to be incompatible with yet.
		${VersionCompare} "$1" "${MIN_RUNTIME_VERSION}" $R0
		${If} $1 != ""
		${AndIf} $R0 == 2
			!insertmacro AbortWithReason 5 "DisplayXR runtime $1 is too old for this plug-in.$\r$\n$\r$\nLeia SR ${VERSION} is built against runtime ${MIN_RUNTIME_VERSION} and needs ${MIN_RUNTIME_VERSION} or later. Installing it on $1 would leave the runtime unable to load it — 3D would silently fall back to the simulated display.$\r$\n$\r$\nUpdate the runtime (or install the DisplayXR bundle, which keeps the pair matched):$\r$\nhttps://github.com/DisplayXR/displayxr-runtime/releases"
		${EndIf}
	!endif
FunctionEnd

;--------------------------------
; Version Information

VIProductVersion "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.0"
VIAddVersionKey "ProductName" "DisplayXR Leia SR Plug-in"
VIAddVersionKey "CompanyName" "Leia Inc."
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 Leia Inc."
VIAddVersionKey "FileDescription" "DisplayXR Leia SR Display Processor Plug-in Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
