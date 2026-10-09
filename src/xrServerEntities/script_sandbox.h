#pragma once

// Script sandbox: what the scripts of the game and of its mods may open.
// The rules and the reasons are in script_storage.cpp, under "Script sandbox".

// what is LUAL_GUARD_READ (0) or LUAL_GUARD_WRITE (1). Returns false to refuse.
bool script_path_allowed(const char* path, int what);
// The same for a path given the way CLocatorAPI takes one: a root alias and a name.
bool script_path_allowed(const char* initial, const char* name, int what);
// Tells LuaJIT and lfs to ask the above. Safe to call more than once.
void script_sandbox_install();
