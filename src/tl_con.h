//=============================================================================
// triggerlook: console commands (tl_con.cpp)
//=============================================================================
#ifndef TL_CON_H
#define TL_CON_H

class ICvar;

// registers every CON_COMMAND of the module (main thread, once VEngineCvar004 is there)
bool TL_RegisterCommands( ICvar *pCvar );

#endif // TL_CON_H
