//=============================================================================
// triggerlook: the trigger menu (tl_ui.cpp) and what it needs from the runtime (tl_runtime.cpp)
//=============================================================================
#ifndef TL_UI_H
#define TL_UI_H

#include <vector>

class IVEngineClient;

// the menu + the ticker panel; false while VGUI isn't up yet (call again later)
bool TLUI_Init( IVEngineClient *pEngine );
bool TLUI_Ready();
void TLUI_SetOpen( bool open );
bool TLUI_IsOpen();

// runtime: every frame (the ticker panel), and how many triggers of a type the server shows right now
void TL_Tick();
int TL_InMap( int type );		// this map's triggers (or clips) of a type
bool TL_Active();		// triggerlook_enable and in game with the map read
void TL_Start();		// panels up as soon as VGUI is (no-op once done)
void TL_RunScript();	// tl_ui.cpp: the next due lines of a triggerlook_selftest script

// custom triggers (tl_look.h) of the current map, in order: indices into g_TLCT (CT-1 = the first)
const std::vector<int> &TL_MapCTs();
int TL_CTLabel( int index );		// its n in CT-n on this map, 0 = not on this map
int TL_CTType( int index );			// the type of its trigger, -1 = not on this map
int TL_ModelType( int model );		// the type of trigger *model, -1 = not a trigger
// SELECT TRIGGER: while picking, the drawn trigger under the crosshair is lit up
void TL_SetPicking( bool on );
int TL_PickHover();					// its model number, -1 = none
int TL_ModelCT( int model );			// trigger *model's custom trigger, -1 = none
int TL_PickedCT( int model );		// that trigger's custom trigger (made from its type's look if it has none), -1 = none
void TLUI_Tick();					// tl_ui.cpp: from TL_Tick

#endif // TL_UI_H
