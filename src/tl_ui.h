//=============================================================================
// triggerlook: the trigger menu (tl_ui.cpp) and what it needs from the runtime (tl_runtime.cpp)
//=============================================================================
#ifndef TL_UI_H
#define TL_UI_H

class IVEngineClient;

// the menu + the ticker panel; false while VGUI isn't up yet (call again later)
bool TLUI_Init( IVEngineClient *pEngine );
bool TLUI_Ready();
void TLUI_SetOpen( bool open );
bool TLUI_IsOpen();

// runtime: every frame (the ticker panel), and how many triggers of a type the server shows right now
void TL_Tick();
int TL_ShownNow( int type );
bool TL_Active();		// triggerlook_enable and in game with the map read
void TL_Start();		// panels up as soon as VGUI is (no-op once done)
void TL_RunScript();	// tl_ui.cpp: the next due lines of a triggerlook_selftest script

#endif // TL_UI_H
