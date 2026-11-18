//=============================================================================
// triggerlook: console commands without tier1's convar.cpp (CS:TV's way): the parts of ConCommandBase /
// ConCommand a command needs, so the class and its vtable are exactly the SDK's, and our own registration
// through ICvar (VEngineCvar004). CON_COMMAND works as usual; every command registers in TL_RegisterCommands.
//=============================================================================
#include <vector>

#include "tier1/convar.h"
#include "icvar.h"

#include "tl_con.h"

static CVarDLLIdentifier_t s_DllId = -1;

static std::vector<ConCommand *> &Commands()
{
	static std::vector<ConCommand *> s_Commands;	// (constructed before the first static ConCommand needs it)
	return s_Commands;
}

ConCommandBase::ConCommandBase() : m_pNext( nullptr ), m_bRegistered( false ), m_pszName( nullptr ), m_pszHelpString( nullptr ), m_nFlags( 0 ) {}
ConCommandBase::~ConCommandBase() {}
bool ConCommandBase::IsCommand() const { return true; }
bool ConCommandBase::IsFlagSet( int flag ) const { return ( flag & m_nFlags ) != 0; }
void ConCommandBase::AddFlags( int flags ) { m_nFlags |= flags; }
const char *ConCommandBase::GetName() const { return m_pszName; }
const char *ConCommandBase::GetHelpText() const { return m_pszHelpString; }
bool ConCommandBase::IsRegistered() const { return m_bRegistered; }
CVarDLLIdentifier_t ConCommandBase::GetDLLIdentifier() const { return s_DllId; }
void ConCommandBase::CreateBase( const char *name, const char *help, int flags )
{
	m_bRegistered = false;
	m_pszName = name;
	m_pszHelpString = help ? help : "";
	m_nFlags = flags;
	m_pNext = nullptr;
}
void ConCommandBase::Init() {}

ConCommand::ConCommand( const char *name, FnCommandCallback_t cb, const char *help, int flags, FnCommandCompletionCallback )
{
	m_fnCommandCallback = cb;
	m_fnCompletionCallback = nullptr;
	m_bHasCompletionCallback = false;
	m_bUsingNewCommandCallback = true;
	m_bUsingCommandCallbackInterface = false;
	CreateBase( name, help, flags );
	Commands().push_back( this );
}
ConCommand::~ConCommand() {}
bool ConCommand::IsCommand() const { return true; }
int ConCommand::AutoCompleteSuggest( const char *, CUtlVector<CUtlString> & ) { return 0; }
bool ConCommand::CanAutoComplete() { return false; }
void ConCommand::Dispatch( const CCommand &command ) { m_fnCommandCallback( command ); }

bool TL_RegisterCommands( ICvar *pCvar )
{
	if ( !pCvar )
		return false;
	if ( s_DllId == -1 )
		s_DllId = pCvar->AllocateDLLIdentifier();
	for ( ConCommand *c : Commands() )
		if ( !c->IsRegistered() )
			pCvar->RegisterConCommand( c );
	return true;
}
