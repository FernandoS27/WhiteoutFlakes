-- ============================================================================
-- WhiteoutDex Launcher
-- The .dlx contains everything — just call ndxStart() / ndxStop()
-- ============================================================================

callbacks.removeScripts id:#WhiteoutDexCleanup
callbacks.addScript #systemPreReset "try(ndxStop())catch()" id:#WhiteoutDexCleanup
callbacks.addScript #systemPreNew   "try(ndxStop())catch()" id:#WhiteoutDexCleanup
callbacks.addScript #filePreOpen    "try(ndxStop())catch()" id:#WhiteoutDexCleanup

format "\n=== WhiteoutDex Preview ===\n"
format "  ndxStart()  — extract + render + sync\n"
format "  ndxStop()   — stop\n"
format "=====================\n\n"

ndxStart()
