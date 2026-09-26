#if !(defined(GO) && defined(GOM) && defined(GO2) && defined(DATA))
#error Meh
#endif

/*
 * The engine's guaranteed native-module signature set.
 *
 * This is NOT a wrapped library: it has no library_list.inc, registers no
 * soname, and its placeholder names never reach a guest. It exists so that
 * box64's own wrapper generator (rebuild_wrappers.py) always emits the
 * wrapper functions the native-module seam can be asked for, even on an
 * engine-only build whose own wrapped libraries do not happen to use them.
 *
 * The list is deliberately Node-agnostic: only SysV signature tokens. The
 * provider (uninode) names guest symbols and picks one of these tokens; the
 * engine's wrapped path maps the token to the wrapper emitted here and refuses
 * any token that is not in this set (native_module_wrappers.c).
 *
 * Keep this in sync with the provider descriptors that expect to be fully
 * consumable; a missing token fails closed at symbol-resolution time, not at
 * load time, so it is visible as an unresolved guest symbol.
 *
 * GO  = ordinary entry: the engine bridges a dlsym()'d host symbol.
 * GOM = callback entry: the engine bridges a provider handler, which receives
 *       the emulator handle first, hence the E form of the signature.
 */

GO(seam_iFpIp, iFpIp)
GO(seam_iFpppLpp, iFpppLpp)
GO(seam_iFppi, iFppi)
GO(seam_iFpppp, iFpppp)
GO(seam_iFpp, iFpp)
GO(seam_iFppp, iFppp)
GO(seam_iFpUp, iFpUp)
GO(seam_iFppLp, iFppLp)
GO(seam_iFpLp, iFpLp)
GO(seam_iFpLpp, iFpLpp)
GO(seam_iFpLppp, iFpLppp)
GO(seam_iFpdp, iFpdp)
GO(seam_iFpip, iFpip)
GO(seam_iFppup, iFppup)
GO(seam_iFpiLpLp, iFpiLpLp)
GO(seam_iFpup, iFpup)
GO(seam_vFpLpL, vFpLpL)
GO(seam_iFppiiip, iFppiiip)
GO(seam_iFpppppp, iFpppppp)
GO(seam_iFppppppp, iFppppppp)
GO(seam_iFpppLp, iFpppLp)
GO(seam_iFppLpp, iFppLpp)
GO(seam_iFpi, iFpi)
GO(seam_iFp, iFp)
GO(seam_vFp, vFp)
GO(seam_pFi, pFi)
GOM(seam_iFEppLp, iFEppLp)
GOM(seam_iFEppp, iFEppp)
GOM(seam_iFEpppppp, iFEpppppp)
GOM(seam_iFEppppppp, iFEppppppp)
GOM(seam_iFEpppp, iFEpppp)
GOM(seam_iFEppppp, iFEppppp)
GOM(seam_iFEppLppp, iFEppLppp)
GOM(seam_iFEpLpppp, iFEpLpppp)
GOM(seam_iFEppppLLppppp, iFEppppLLppppp)
GOM(seam_iFEppLppLpp, iFEppLppLpp)
GOM(seam_vFEpp, vFEpp)
GOM(seam_iFEpip, iFEpip)
