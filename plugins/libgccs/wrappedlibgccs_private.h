#if !(defined(GO) && defined(GOM) && defined(GO2) && defined(DATA))
#error Meh
#endif

/* Host libgcc_s.so.1 — prefer wrap over emulated /usr/lib/box64-x86_64-linux-gnu/libgcc_s.so.1 */

GO(_Unwind_Backtrace, iFpp)
GO(_Unwind_DeleteException, vFp)
GO(_Unwind_FindEnclosingFunction, pFp)
/* wine's x86_64-unix ntdll.so imports this for its DWARF<->SEH bridge;
 * the emulated libgcc a standalone box64 falls back to carries it, so a
 * host wrap must too or the ntdll relocation fails and wine cannot boot. */
GO(_Unwind_Find_FDE, pFpp)
GO(_Unwind_GetCFA, LFp)
GO(_Unwind_GetDataRelBase, pFp)
GO(_Unwind_GetIP, pFp)
GO(_Unwind_GetIPInfo, pFpp)
GO(_Unwind_GetLanguageSpecificData, pFp)
GO(_Unwind_GetRegionStart, pFp)
GO(_Unwind_GetTextRelBase, pFp)
GO(_Unwind_RaiseException, iFp)
GO(_Unwind_Resume, vFp)
GO(_Unwind_Resume_or_Rethrow, iFp)
GO(_Unwind_SetGR, vFpiL)
GO(_Unwind_SetIP, vFpp)
