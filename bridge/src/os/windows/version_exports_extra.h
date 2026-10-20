/* Exports of Windows' own System32 version.dll that the generated list
 * (version_exports.h, read from Proton's builtin version.dll) does not have.
 * Hand-kept, because no real System32 version.dll was available to generate
 * from (lane-p4, 2026-10-19: every version.dll in the Proton prefix and the
 * Proton tree is a Wine builtin; the xwin SDK carries only the import library
 * version.lib, which lists 12 public API names, a subset of the 16). Replace
 * this file by regenerating version_exports.h from a real System32 copy
 * (gen_exports.py) on the first real-Windows run (ADR phase P7).
 *
 * Under Wine/Proton these do not resolve (the builtin lacks them): the DLL logs
 * them as missing and their forward fails the call with ERROR_PROC_NOT_FOUND.
 * ck3.exe 1.18.3.1 does not import them.
 *
 * X(name, ordinal_in_source_dll); 0 = not read from a source DLL. */
X(GetFileVersionInfoByHandle, 0)
