//-----------------------------------------------------------------------------
// MEKA - inputs_inject.h
// DAP / automation keyboard injection into g_keyboard_state
//-----------------------------------------------------------------------------

#pragma once

// Resolve key name / alias / scancode string to Allegro scancode, or -1.
int     Inputs_InjectResolveKey(const char* key);

// Queue key down (true) or up (false); applied on main thread in Inputs_InjectProcess().
void    Inputs_InjectKey(int scancode, bool down);

// Key down, then automatic release after duration_ms.
void    Inputs_InjectKeyPress(int scancode, int duration_ms);

// Drain pending injections and timed releases (call from Inputs_Sources_Update).
void    Inputs_InjectProcess();

//-----------------------------------------------------------------------------
