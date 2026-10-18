// observer_data_functions.h — gui data-function callback pointers registered for
// Observer-related actions, extracted per ck-sml8 prerequisite §"Concrete next steps".
//
// Produced by: ck-sml8 (polecat furiosa, 2026-04-23)
// Method: scan .text for `LEA reg, [rip+disp32]` referencing each Observer-related
//         string rodata address, then within 250 bytes scan for
//         `LEA RAX, [rip+disp32]; MOV [RSP+??], RAX` — that RAX disp32 target is
//         the callback function pointer Paradox stores into the DataFunctionBinding
//         stack struct before the registration CALL.
//
// Address convention (matches ck3_offsets.h):
//   raw       = ghidra_va - 0x00100000
//   ghidra_va = raw + 0x00100000
// All uintptr_t values below are RAW (runtime, ASLR-base-relative).
//
// ─── Decomp verification (ck-sml8, 2026-04-23) ─────────────────────────────────
//
// ⚠ Key finding: NONE of these five gui-datafunction callbacks are the correct
// target for "start the game in Observer mode from character-select". They are
// all Observer-adjacent operations usable only AFTER Observer has been loaded
// (CJominiIdler singleton non-null) or are blocked via GameCommandDispatch
// (which only drains from on_tick, i.e. during active play).
//
// The Observe button on CFrontEndGameSetupView (character-select) dispatches
// through CFrontEndGameSetupView::OnStartInternal(EStartType::OBSERVER, ...) —
// a C++ method, NOT a named gui data-function. Finding that method's address
// and the EStartType enum value is follow-up RE (see re/findings/
// re_observer_button_bridge_first.md open questions).
//
// Table below is therefore REFERENCE ONLY for the post-Observer path — useful
// for future "change observed character / toggle observe mode" bridge actions
// but NOT for the ck-sml8 cold-boot auto-select path.

#pragma once
#include <cstdint>

namespace ck3::observer_data_functions {

// SetRandomPlayableObserverCharacter — picks a random observable character via
// the CJominiIdler pause-state singleton and sets them as the current observer.
// Thunk at 0x3dff010 delegates to body at 0x3de98c0.
//
// NOT USABLE AT CHARACTER-SELECT: body early-returns if `g_CJominiIdler_pause`
// (== IDLER_SINGLETON == ck3::globals::IDLER_SINGLETON at 0x0c5b53e8) is null.
// CJominiIdler singleton is only constructed AFTER Observer mode loads.
// Runtime validation (2026-04-23): bridge log shows "CJominiIdler singleton is
// null" at dormant state — consistent with the decomp early-return gate.
//
// Registered from _INIT_877 (Ghidra VA 0x018bb630) at LEA site 0x17bdd11.
constexpr uintptr_t SET_RANDOM_PLAYABLE_OBSERVER_THUNK = 0x3dff010;  // HIGH
constexpr uintptr_t SET_RANDOM_PLAYABLE_OBSERVER_BODY  = 0x3de98c0;  // HIGH

// GoToObserver — thin wrapper around FUN_04b7da60 which calls
// GameCommandDispatch(&g_CommandDispatchManager, &cmd, 0xe, 0) twice.
// NOT USABLE AT CHARACTER-SELECT: ProcessCommandQueue drains only from on_tick
// (= active-play only). Same structural gate as the in-game `observe` console
// command at 0x041f86d0.
constexpr uintptr_t GO_TO_OBSERVER_THUNK = 0x4a84ba0;  // HIGH
constexpr uintptr_t GO_TO_OBSERVER_BODY  = 0x4a7da60;  // HIGH

// ClearObserver — ends observer mode (calls into the dispatch/command pipeline).
// Post-Observer operation; not relevant to bridge-side auto-select.
constexpr uintptr_t CLEAR_OBSERVER_THUNK = 0x402ef00;  // HIGH

// IsObserver — read-only getter. Pass-through: FUN_06094f40 returns the
// observer-enabled flag. Safe to call from any thread, any time. Can be used
// as a bridge poll to detect when Observer mode has been entered.
constexpr uintptr_t IS_OBSERVER_BODY     = 0x5ce2890;  // HIGH

// ToggleObserve — toggles observer mode on/off (post-Observer only).
constexpr uintptr_t TOGGLE_OBSERVE_THUNK = 0x591a6d0;  // HIGH

// ─── Registration callsites (for signature hardening / patch-adapt) ────────────
// These are the LEA-rodata-to-string callsites identified by binary scan.
// The DataFunctionBinding is built on-stack just after each LEA and registered
// via a CALL a few dozen bytes later. Patterns survive minor compiler changes.
constexpr uintptr_t REG_LEA_SET_RANDOM_PLAYABLE = 0x17bdd11;  // _INIT_877
constexpr uintptr_t REG_LEA_GO_TO_OBSERVER      = 0x1d2ee71;
constexpr uintptr_t REG_LEA_CLEAR_OBSERVER      = 0x18b4271;
constexpr uintptr_t REG_LEA_IS_OBSERVER         = 0x26baa41;  // 4 sites total; this is the real handler (others are _INIT_ registration scaffolding)
constexpr uintptr_t REG_LEA_TOGGLE_OBSERVE      = 0x2416bad;  // _INIT_2880

}  // namespace ck3::observer_data_functions

// ═══════════════════════════════════════════════════════════════════════════
// Frontend-idler vtables (for Path B1 infrastructure — ck-sml8 §Deliverable 2)
// Extracted via .rela.dyn scan: find RELATIVE relocs writing each class's
// typeinfo-name pointer (§classname strings), walk back to the __si_class_type_info
// struct address, find the vtable whose typeinfo-slot writes that struct address.
// ═══════════════════════════════════════════════════════════════════════════

namespace ck3::frontend_idler {

// CFrontendInterfaceIdler — inherits from CJominiIdler.
// Runtime VAs. Vtable layout: [offset_to_top, typeinfo_ptr, vfn_0, vfn_1, …].
// offset_to_top @ 0xb70b570, typeinfo_ptr @ 0xb70b578, first vfn @ 0xb70b580.
constexpr uintptr_t FRONT_IF_IDLER_VTABLE       = 0xb70b570;  // HIGH
constexpr uintptr_t FRONT_IF_IDLER_TYPEINFO     = 0xb70b628;  // HIGH (__si_class_type_info, base = CJominiIdler @ 0xbe1eb30)
//   vfn[0]: 0x3f90020  — destructor (D2, confirmed by decomp: writes UNK_0b80b580 vptr, cleanup)
//   vfn[1]: 0x3f90040  — destructor+delete (D0)
//   vfn[2]: 0x7747b00  — empty (ret)
//   vfn[3]: 0x75fd560  — forwarder: if this+0x10 != 0, call (*(this+0x10))->vtable[8]()
//   vfn[4]: 0x7747b10  — small guarded forwarder (FUN_07829420 gate then FUN_07829380)
//   vfn[5]: 0x7747b30, vfn[6]: 0x7747b50, vfn[7]: 0x7747b80, vfn[8]: 0x7747ba0
//   vfn[9]: 0x3b06170 — longer function (uninspected this session)
//   vfn[10+]: mix of 3b06xxx / 3f8fxxx / 7747xxx / 75fdxxx
// The per-frame "Update"/"Tick" slot is NOT YET IDENTIFIED.
// Candidates (by shape): vfn[3] forwarder pattern looks most tick-like (delegates to
// an owned subobject's vtable slot 8). Follow-up RE needed — decompile and
// behavior-match against CJominiIdler tick patterns.

// CFrontendInterfaceIdlerGfx — second variant (Gfx-backend idler).
// offset_to_top @ 0xb70d418, typeinfo_ptr @ 0xb70d420, first vfn @ 0xb70d428.
constexpr uintptr_t FRONT_IF_IDLER_GFX_VTABLE   = 0xb70d418;  // HIGH
constexpr uintptr_t FRONT_IF_IDLER_GFX_TYPEINFO = 0xb70d490;  // HIGH (__si_class_type_info)

// CFrontendConsoleIdler — console variant.
// offset_to_top @ 0xbc8ab48, typeinfo_ptr @ 0xbc8ab50, first vfn @ 0xbc8ab58.
constexpr uintptr_t FRONT_CONSOLE_IDLER_VTABLE   = 0xbc8ab48;  // HIGH
constexpr uintptr_t FRONT_CONSOLE_IDLER_TYPEINFO = 0xbc8ac00;  // HIGH (__class_type_info)

// Typeinfo-name rodata addresses (for signature hardening).
constexpr uintptr_t TN_FRONT_IF_IDLER      = 0x84e67a4;  // "23CFrontendInterfaceIdler"
constexpr uintptr_t TN_FRONT_IF_IDLER_GFX  = 0x84e7e27;  // "26CFrontendInterfaceIdlerGfx"
constexpr uintptr_t TN_FRONT_CONSOLE_IDLER = 0x8b0a442;  // "21CFrontendConsoleIdler"

// CJominiIdler base typeinfo (shared parent).
constexpr uintptr_t CJOMINI_IDLER_TYPEINFO = 0xbe1eb30;  // __si_class_type_info (base = SIdlerMessageListener @ 0xbe2b598)

// Global singleton for CFrontendInterfaceIdler: NOT YET IDENTIFIED this session.
// Approach for follow-up: find constructor of CFrontendInterfaceIdler (writes vtable
// pointer 0xb70b570+0x10 to `this+0`), then xref to a static global that stores
// the constructed pointer. This is the address the bridge polls during dormant
// to detect when the frontend-idler is live.

}  // namespace ck3::frontend_idler

// ═══════════════════════════════════════════════════════════════════════════
// OnStartInternal (the real character-select Observe-button dispatch target)
// ═══════════════════════════════════════════════════════════════════════════
//
// BLOCKER: The binary is heavily stripped — .symtab is absent and only .dynsym
// remains (external symbols). The member function
//   CFrontEndGameSetupView::OnStartInternal(EStartType, ETutorialMode,
//                                           const CBookmarkCharacter*)
// has NO surviving function symbol in the binary. Ghidra's `symbol.by_name`
// search for "OnStartInternal" / "FrontEndGameSetupView" returns 0 hits.
//
// What survives:
//   - Typeinfo name at 0x85d6314 for an anonymous lambda class defined INSIDE
//     OnStartInternal: `ZN22CFrontEndGameSetupView15OnStartInternalENS_10E…E3$_2`.
//     This is the 3rd ($_2) lambda inside the method.
//   - The lambda's typeinfo struct at 0xb7bddf0 (__class_type_info, no base),
//     name_ptr reloc at 0xb7bddf8 → 0x85d6314. NO .text relocations reference
//     this typeinfo struct (addend scan in readelf -rW returned zero hits),
//     suggesting the lambda's code is inlined into OnStartInternal and its
//     typeinfo is only reachable via std::function's target_type() TLS path,
//     if at all.
//   - CFrontEndGameSetupView's class typeinfo name at 0x85d5911 ("22CFrontEnd…"),
//     typeinfo struct at either 0x2e8b98 or 0xb7bc208 — BOTH with zero xrefs
//     in the RELA table AND zero xrefs in static rodata. The CFrontEndGameSetupView
//     vtable is built dynamically at runtime (via .init_array constructors) and
//     its static symbol is unrecoverable from binary alone.
//
// Next-step RE (est 1-2hr, NOT done this session):
//   1. Use Ghidra's decompiler on the CFrontEndGameSetupView constructor —
//      find it by searching `.text` for `LEA reg, [rip+disp32]` where disp32
//      targets 0x85d5911 (class typeinfo name) from a code site. Any writes
//      to `*this = vtable_addr` reveal the vtable address of CFrontEndGameSetupView.
//   2. Once the vtable is known, its slots contain method pointers including
//      OnStartInternal. Determine slot index by decomp — OnStartInternal is the
//      only 3-arg method with that signature shape (enum, enum, pointer).
//   3. Decompile OnStartInternal to identify EStartType::OBSERVER — it will
//      contain a switch or if-chain on param_1; one arm will call the observer-
//      start command path (vs random-character / bookmark-start paths).
//   4. Record addresses in this header, then implement frontend-idler hook
//      (Path B1) to invoke OnStartInternal(this_view, OBSERVER, TUTORIAL_OFF, nullptr)
//      from a safe engine-thread moment.
//
// UPDATE (ck-rr27 session3, polecat:rictus, 2026-04-24): this premise is
// likely wrong. See re/findings/re_observer_autoselect_session3.md.
// `frontend_bookmarks.gui` contains no "Observe" button; the post-load
// prompt our uinput fallback accepts is owned by `CFrontEndLoadView`
// (reached via the main-menu Continue button or `--continuelastsave`).
// See namespace `ck3::frontend_load` below for the recovered
// FrontEndLoadView gui-datafunction callbacks.
// ═══════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════
// CFrontEndLoadView gui-datafunction callbacks (ck-rr27 session3, 2026-04-24)
// ═══════════════════════════════════════════════════════════════════════════
//
// Source: `/mnt/gsg/ck3/titus/source/interface/frontendloadview.cpp` (path
// embedded in rodata at 0x85d9680 — confirms class identity). The class is
// bound to `[FrontEndLoadView.<Method>]` gui-datafunction references in
// `frontend_main.gui` (Continue button) and `frontend_load.gui` (save browser).
//
// Registration init: `_INIT_1789` @ Ghidra VA 0x01c3e3d0 (raw 0x1b3e3d0),
// body ends at Ghidra VA 0x01c40e88. Each method-name string (below) is
// referenced by an `LEA reg, [rip+disp32]` in this init; the callback body
// is the 3rd `FUN_` in each registration block (following the standard
// std::function-binding pattern: thunk pointer stored in pcStack_310/318,
// registrar call's first arg is the member-function body).
//
// Raw VAs (matches ck3_offsets.h convention; Ghidra VA = raw + 0x100000).

namespace ck3::frontend_load {

// Method name string table (contiguous in rodata starting at 0x85d9564):
constexpr uintptr_t STR_FRONTENDLOADVIEW        = 0x85d9564;  // HIGH — class name
constexpr uintptr_t STR_ON_LOAD_SAVED_GAME_BTN  = 0x85d9575;  // HIGH
constexpr uintptr_t STR_ON_CONTINUE             = 0x85d95a6;  // HIGH
constexpr uintptr_t STR_POST_CONTINUE_FADE      = 0x85d95b1;  // HIGH
constexpr uintptr_t STR_CAN_DO_CONTINUE         = 0x85d95c2;  // HIGH
constexpr uintptr_t STR_PRE_CONTINUE_SAVE_CHECK = 0x85d95d0;  // HIGH
constexpr uintptr_t STR_GET_CONTINUE_SAVE       = 0x85d95e5;  // HIGH

// Callback body addresses (what the gui-datafunction dispatch calls).
// Each is a C free-function with signature `(CFrontEndLoadView* this, ...)`.
// Decomp-verified for OnContinue; others extracted by binding-pattern match.
constexpr uintptr_t CB_ON_LOAD_SAVED_GAME_BTN  = 0x4645d60;  // HIGH — OnLoadSavedGameButton
constexpr uintptr_t CB_ON_CONTINUE             = 0x4645ec0;  // HIGH — OnContinue (verified: *(this+0xec)=0, saves-list access via this+0x108, branch on FUN_041e9940 gate, missing-save msg uses loc key via UNK_086d9652)
constexpr uintptr_t CB_POST_CONTINUE_FADE      = 0x46460e0;  // HIGH — PostContinueFade
constexpr uintptr_t CB_CAN_DO_CONTINUE         = 0x46460f0;  // HIGH — CanDoContinue (uses a different registrar FUN_047497a0 vs FUN_04748d20 for the others; bool-returning predicate shape)
constexpr uintptr_t CB_PRE_CONTINUE_SAVE_CHECK = 0x4646370;  // HIGH — PreContinueSaveCheck
constexpr uintptr_t CB_GET_CONTINUE_SAVE       = 0x4646700;  // HIGH — GetContinueSave (also uses a different registrar FUN_0474a0e0; getter returning a saved-game item)

// Registration callsites (LEA-to-method-name, inside _INIT_1789):
constexpr uintptr_t REG_LEA_FRONTENDLOADVIEW       = 0x1b3fd89;  // LEA RDI, [rip+disp] → FrontEndLoadView
constexpr uintptr_t REG_LEA_ON_LOAD_SAVED_GAME_BTN = 0x1b3fe72;
constexpr uintptr_t REG_LEA_ON_CONTINUE            = 0x1b40281;
constexpr uintptr_t REG_LEA_POST_CONTINUE_FADE     = 0x1b40381;
constexpr uintptr_t REG_LEA_CAN_DO_CONTINUE        = 0x1b40481;
constexpr uintptr_t REG_LEA_PRE_CONTINUE_SAVE_CHK  = 0x1b40981;
constexpr uintptr_t REG_LEA_GET_CONTINUE_SAVE     = 0x1b40c81;

// Class typeinfo name (Itanium `__class_type_info`-style, 17-char class name):
// "17CFrontEndLoadView" appears in rodata at three offsets. The canonical
// name used by the typeinfo struct was not identified this session — walking
// one of these xrefs to the typeinfo struct would reveal the runtime vtable
// once Ghidra chases the relocation (same path ck-sml8 attempted for
// CFrontEndGameSetupView).

}  // namespace ck3::frontend_load

// ═══════════════════════════════════════════════════════════════════════════
// CFrontEndLoadView struct layout + load-state-machine entry points
//   (ck-dq7ym.7 session 4, polecat:furiosa, 2026-05-22)
//
// Decompiled by disassembling each callback via Ghidra MCP and tracing the
// post-fade orchestrator + load-orchestrator + state-machine primitive.
// See re/sessions/ck-dq7ym.7/SESSION-REPORT.md for the full walkthrough.
//
// All offsets verified by reading the disassembly directly — HIGH confidence.
// ═══════════════════════════════════════════════════════════════════════════

namespace ck3::frontend_load::layout {

// CFrontEndLoadView struct offsets (matching ck3_offsets.h convention).
constexpr uintptr_t STATE_MACHINE_OFFSET     = 0xd0;   // CStateMachine*
constexpr uintptr_t SAVE_METADATA_OFFSET     = 0xc8;   // void* (input to save-classifier)
constexpr uintptr_t SELECTED_INDEX_OFFSET    = 0xec;   // int — index into saves array
constexpr uintptr_t SAVES_ARRAY_OFFSET       = 0x108;  // SaveEntry[count], entries 0x60 bytes each
constexpr uintptr_t SAVE_COUNT_OFFSET        = 0x114;  // int
constexpr uintptr_t POST_FADE_GATE_OFFSET    = 0x129;  // byte — PRE_CONTINUE_SAVE_CHECK sets, POST_CONTINUE_FADE reads
constexpr uintptr_t VIEW_MUTEX_OFFSET        = 0x130;  // mutex bytes; lock=FUN_08247470, unlock=FUN_082474b0
constexpr uintptr_t LOAD_KIND_BYTE_OFFSET    = 0x158;  // (*field+0x2) is the "this is observer/regular" discriminator

// Post-fade orchestrator (CB_POST_CONTINUE_FADE tail-jumps here when gate set).
// On the safe branch ((*(this+0x158))[2] == 2) it constructs a 24-byte
// std::function capturing `this` and schedules FUN_04747340 via the engine
// scheduler.
constexpr uintptr_t FUN_POST_FADE_ORCHESTRATOR = 0x4647420;  // raw — HIGH

// Load orchestrator scheduled by the post-fade path. Looks up the save by
// index, classifies it via FUN_041e72b0, then transitions the state machine.
constexpr uintptr_t FUN_LOAD_ORCHESTRATOR      = 0x4647340;  // raw — HIGH

// Save classifier: returns 0 (invalid), 1 (current-version), or 2 (compatible).
// Used by FUN_04747340 to decide whether to call the state-machine primitive
// or the compatible-version handler.
constexpr uintptr_t FUN_SAVE_CLASSIFIER        = 0x40e72b0;  // raw — HIGH

// State-machine transition primitive: signature
//   void(*)(CStateMachine* sm, int new_state, int force_flag)
// Generic — used throughout the engine, NOT observer-specific.
// FUN_04747340 calls this with (state_machine, 0, 1) for kind==1 saves.
constexpr uintptr_t FUN_STATE_MACHINE_TRANS    = 0x73d8150;  // raw — HIGH

// Engine scheduler used by FUN_04747420 to defer FUN_04747340 onto a safe
// thread. Signature inferred:
//   void(*)(CStateMachine* sm, std::function* fn, int mode);
// where mode=2 is "defer until next safe tick".
constexpr uintptr_t FUN_SCHEDULE_DEFERRED      = 0x5914160;  // raw — HIGH

// std::function manager vtables used by the safe-branch deferral target.
// fn ptr stored at [target+0x0], manager (copy/move/destroy) at [target+0x10],
// final-state vtable at [target+0x20].
constexpr uintptr_t FN_TARGET_VTABLE_PRIMARY   = 0x464c7c0;  // raw — HIGH
constexpr uintptr_t FN_TARGET_VTABLE_MANAGER   = 0x464c7e0;  // raw — HIGH

// std::function pair passed by CB_ON_CONTINUE to FUN_041debe0 as the
// load-failure callback. The invoker just shows the
// "MISSING_CONTINUE_SAVE_GAME_TITLE" toast (loc key @ 0x86d9652, len=0x12).
constexpr uintptr_t FN_LOAD_FAIL_INVOKER       = 0x4647880;  // raw — HIGH
constexpr uintptr_t FN_LOAD_FAIL_MANAGER       = 0x46478b0;  // raw — HIGH (switch on EDX: 0=fill, 1=copy, 2=move, 3=destroy)

// SaveGameReloader / missing-save error dialog constructor (used by
// PRE_CONTINUE_SAVE_CHECK's error branch — NOT the Observer modal).
// Allocates a 352-byte modal-ish object backed by vtables at 0xb8c1088,
// 0xb8c1100, 0xb8c1140; publishes via FUN_03d12700.
constexpr uintptr_t FUN_SAVE_RELOADER_CTOR     = 0x464c1f0;  // raw — HIGH

}  // namespace ck3::frontend_load::layout

// ═══════════════════════════════════════════════════════════════════════════
// PREMISE CORRECTION (ck-dq7ym.7 session 4)
// ═══════════════════════════════════════════════════════════════════════════
//
// The actual "Observe" button the uinput fallback clicks at (540, 1060) is
// the `JominiLobbyViewButton` in `multiplayer_lobby.gui:1081-1165`, owned by
// **`CMultiplayerLobbyView`** (or whichever Paradox class is bound to
// `LobbyView` data-functions). NOT CFrontEndLoadView, NOT CFrontEndGameSetupView.
//
// The button has two onclick handlers fired in order:
//   onclick = "[CloseGameView('find_title')]"
//   onclick = "[LobbyView.ToggleObserve]"
//
// LobbyView.ToggleObserve is already in ck3::observer_data_functions above
// (TOGGLE_OBSERVE_THUNK @ 0x591a6d0) — it's GameCommandDispatch-bound but the
// queue does drain once unpaused (per ck-ionq runtime probe).
//
// CloseGameView is a separate gui-datafunction not yet RE'd. It is the prime
// candidate for the actual state-mutation step (immediate, on-thread).
//
// CFrontEndLoadView remains relevant for the programmatic-Continue path
// (replacing --continuelastsave with a bridge-driven trigger) but does NOT
// dismiss the lobby Observe screen.

// ═══════════════════════════════════════════════════════════════════════════
// CJominiMultiplayerLobbyView class + ownership chain (ck-dq7ym.8, 2026-05-22)
//
// Recovered by static byte-scan of the CK3 binary
// (/home/admin/workspace/ck3proj/root/ck3proj/ck3/binaries/ck3,
//  BuildID e61ff2642f07094defd140fcc98e078d42541275).
//
// Convention: file_offset == raw VA for the .text segment (LOAD 0x161b000+).
// For the RW segment (LOAD 0xb64c130 file → 0xb64d130 vaddr), runtime VA =
// file_offset + 0x1000. Values below are RUNTIME VAs unless noted; bridge
// resolves them as `g_base_addr + raw`.
// ═══════════════════════════════════════════════════════════════════════════

namespace ck3::lobby_view {

// RTTI: re/rtti/rtti_classes.txt contains "27CJominiMultiplayerLobbyView".
// (And "40CJominiMultiplayerIngameInterfaceHandler", "23CIngameInterfaceHandler" —
// these are higher levels in the ownership chain.)
constexpr uintptr_t STR_TYPEINFO_NAME       = 0x88e1ff0;  // HIGH — "27CJominiMultiplayerLobbyView"

// Typeinfo struct: __si_class_type_info shape:
//   +0x00: vtable of __si_class_type_info (RELATIVE-reloc-filled at load)
//   +0x08: ptr to name string (0x88e1ff0)
//   +0x10: ptr to base class typeinfo (0xba920e0)
constexpr uintptr_t TYPEINFO_STRUCT          = 0xba91440;  // HIGH

// Primary vtable: vptr value (= address of vfn0 in the vtable struct).
// Vtable struct layout: [offset_to_top, typeinfo_ptr (= TYPEINFO_STRUCT),
//                        vfn0 (destructor), vfn1, ...]
// Header at 0xba91390 (offset_to_top=0), 0xba91398 (typeinfo_ptr),
// vfn0 at 0xba913a0 — vfn0 = 0x5916e40 (destructor)
//                     vfn1 = 0x5917110, vfn2 = 0x5917520, etc.
constexpr uintptr_t PRIMARY_VTABLE_VPTR      = 0xba913a0;  // HIGH

// Secondary base vtable (offset_to_top = -0x10), header at 0xba91408..0xba91418,
// vfn0 at 0xba91420. Used for the base subobject at this+0x10.
constexpr uintptr_t SECONDARY_VTABLE_VPTR    = 0xba91420;  // HIGH

// Constructor body: CJominiMultiplayerLobbyView::CJominiMultiplayerLobbyView(...).
// Signature inferred from the prologue: (this=rdi, arg2=rsi, arg3=rdx).
// At entry it stores RDI to RBX, then writes PRIMARY_VTABLE_VPTR to *this+0
// and SECONDARY_VTABLE_VPTR to *this+0x10. Stores arg3 (rdx → r14) at *this+0xd0.
// Single caller: the wrapper at CONSTRUCTOR_WRAPPER below.
constexpr uintptr_t CONSTRUCTOR_BODY         = 0x59169a0;  // HIGH

// new() + ctor wrapper. Signature: (owner_obj=rdi, arg2=rsi, arg3=rdx).
// Allocates 0x208 bytes via operator new, calls CONSTRUCTOR_BODY,
// then stores the new singleton pointer at *(owner_obj+0x10).
// Owner_obj is set up by the caller to a `&outer->lobby_field_at_+0x58`.
constexpr uintptr_t CONSTRUCTOR_WRAPPER      = 0x5914210;  // HIGH

// The single caller of CONSTRUCTOR_WRAPPER is the constructor of the outer
// class CIngameInterfaceHandler, at 0x3826810. It calls the wrapper with
// `rdi = this + 0x58` where `this` is the outer CIngameInterfaceHandler.
// After CONSTRUCTOR_WRAPPER returns, the new lobby-view singleton lives at
// `outer + 0x58 + 0x10 = outer + 0x68`.
constexpr uintptr_t OUTER_CONSTRUCTOR        = 0x3826810;  // HIGH
constexpr uintptr_t SINGLETON_OFFSET_IN_OUTER = 0x68;       // HIGH

// CIngameInterfaceHandler primary vtable vptr (class typeinfo recovered):
// name "23CIngameInterfaceHandler". Typeinfo at runtime VA 0xb66a0c8.
constexpr uintptr_t OUTER_VTABLE_VPTR        = 0xb66a018;  // HIGH

// NOT YET IDENTIFIED:
//   • Static global pointer for the CIngameInterfaceHandler singleton itself.
//     (Without that, the LobbyView singleton can only be captured via a
//      runtime constructor hook — see bridge/src/lobby_view_hook.cpp.)
//   • CJominiMultiplayerLobbyView struct field layout past +0xd0.

}  // namespace ck3::lobby_view

// ═══════════════════════════════════════════════════════════════════════════
// CloseGameView gui-datafunction (ck-dq7ym.8, 2026-05-22)
//
// Registered alongside several other GameView-class data-functions in a
// single _INIT_* registration block. Recovered via the standard recipe:
//   1. find LEA-to-"CloseGameView"-rodata-string in .text
//   2. within ~250 bytes find LEA RAX, [rip+disp]; MOV [RSP+??], RAX —
//      that RAX target is the callback body pointer.
//
// CloseGameView('find_title') is fired by the multiplayer-lobby Observe
// button's first onclick handler (see multiplayer_lobby.gui:1081-1165).
// ═══════════════════════════════════════════════════════════════════════════

namespace ck3::close_game_view {

constexpr uintptr_t STR_CLOSE_GAME_VIEW       = 0x83c9c57;  // HIGH — "CloseGameView"
constexpr uintptr_t STR_FIND_TITLE            = 0x8529dd5;  // HIGH — "find_title"

// LEA-to-name-string callsite, single occurrence (in _INIT_* registration):
constexpr uintptr_t REG_LEA_CLOSE_GAME_VIEW   = 0x167e8a1;  // HIGH

// Callback body — the on-thread state mutation that fires for
// `[CloseGameView('view_name')]` in any *.gui file.
// Signature observed at entry: (?=rdi, ?=rsi, args=rdx, ?=rcx) — RDX holds
// an arg-pack struct. The body dereferences `[rdx]` and indexes via the
// arg-count `[rdx+8]`, then reads a flag byte at runtime VA 0xbecefa8.
//   See first 80 bytes at re/sessions/ck-dq7ym.8/SESSION-REPORT.md for the
//   verbatim disassembly.
constexpr uintptr_t CALLBACK_BODY             = 0x388cc90;  // HIGH

// Adjacent LEA (likely the std::function manager pointer for the binding,
// stored at [rsp+0x20] right after the callback at [rsp+0x18]):
constexpr uintptr_t CALLBACK_MANAGER          = 0x388cbe0;  // HIGH

}  // namespace ck3::close_game_view

// ═══════════════════════════════════════════════════════════════════════════
// Implementation status (ck-dq7ym.8)
//
// What's hooked / captured at runtime:
//   • Constructor hook on CONSTRUCTOR_BODY captures the live LobbyView
//     singleton when the lobby UI is first opened. See
//     bridge/src/lobby_view_hook.cpp.
//
// What's still UNKNOWN and required for full pipe (file a follow-up bead):
//   • CloseGameView arg-pack construction recipe — the bridge needs to
//     synthesise an args object containing the string 'find_title' before
//     calling CALLBACK_BODY directly. Decompiling the body fully will reveal
//     the layout (looks like [vec_ptr, count] with each entry pointing to
//     a CScriptValue-shaped union).
//   • CSetObserverCommand / ToggleObserve enqueue recipe — confirm whether
//     the existing TOGGLE_OBSERVE_THUNK can be invoked from a non-on_tick
//     thread (the bridge's auto-unpause drains the queue, but the construct-
//     and-enqueue step must be thread-safe).
//   • Whether CloseGameView('find_title') alone (no ToggleObserve) is
//     sufficient when paired with the bridge's auto-unpause loop.
// ═══════════════════════════════════════════════════════════════════════════
