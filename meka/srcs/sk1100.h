//-----------------------------------------------------------------------------
// MEKA - sk1100.h
// SK-1100 (Sega Keyboard) / SC-3000 Keyboard Emulation - Headers
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Functions
//-----------------------------------------------------------------------------

void    SK1100_Switch();
void    SK1100_Clear();
void    SK1100_Update();

// Direct SC-3000 / SK-1100 matrix injection (DAP / automation; no host focus).
void    SK1100_InjectAllegroKey(int allegro_key, bool down);
void    SK1100_InjectAllegroKeyPress(int allegro_key, int duration_ms);

//-----------------------------------------------------------------------------

