//=============================================================================
// triggerlook: the trigger menu (triggerlook_menu). Built like the author's paint menu (same panels,
// colours, fonts, colour blocks and custom colour picker) so the two look the same.
//
//   ON / OFF next to the title: draw this map's triggers at all (read from the map file, no server command);
//            OFF = the game's own trigger look. Also triggerlook_show (bindable).
//   SHOW     one button per trigger type (and CLIPS, INVISIBLES, NODRAWS: the map's player clips, invisible walls and
//            nodraw sides), two rows of four: white = drawn, grey = hidden
//   EDIT     the same types, each in its own colours (fill inside, border round it); the colour settings below
//            change the picked ones (click = only that one, shift + click = add / remove)
//   CUSTOM TRIGGERS  SELECT TRIGGER = pick one trigger in the world: it gets its own look (CT-1, CT-2, ... on this
//            map), picked with the types like another EDIT button; DELETE = the picked ones back to their type's look
//   FILL / BORDER  which part the colours and OPACITY change; WIDTH = the border's width (OFF = none)
//   SKYBOX   OFF / COLOUR / SPRAY / PICTURE in place of the map's sky (every map, ON / OFF or not): its page has the
//            colour blocks + CUSTOM COLOUR, this PC's sprays, or the pictures added with ADD A FILE (an in-game file
//            list: the system's file dialog would open behind a fullscreen game)
//
// Changes show at once on the triggers in front of you (the menu sits at the left and doesn't darken the game).
// Plain VGUI panels of the client, input polled (VGUI messages are KeyValues in the game's newer layout). Esc closes.
//=============================================================================
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include <algorithm>
#include <string>
#include <vector>

#include "tier0/dbg.h"
#include "tier0/platform.h"
#include "tier1/convar.h"
#include "vgui/IClientPanel.h"
#include "vgui/IPanel.h"
#include "vgui/ISurface.h"
#include "vgui/IVGui.h"
#include "vgui/IInput.h"
#include "ienginevgui.h"
#include "cdll_int.h"
#include "inputsystem/ButtonCode.h"
#include "inputsystem/iinputsystem.h"

#include "tl_look.h"
#include "tl_pics.h"
#include "tl_ui.h"
#include "tl_platform.h"
#include "tl_util.h"

using namespace vgui;

static ISurface *s_pSurface;
static IPanel *s_pPanel;
static IVGui *s_pVGui;
static IInput *s_pInput;
static IEngineVGui *s_pEngineVGui;
static IInputSystem *s_pInputSystem;	// the wheel (GetButtonPressedTick), as in the paint menu
static IVEngineClient *s_pEngine;

//-----------------------------------------------------------------------------
// Drawing helpers (the paint menu's)
//-----------------------------------------------------------------------------
struct Col { int r, g, b, a; };
static const Col C_BG = { 18, 17, 20, 250 }, C_TEXT = { 236, 230, 220, 255 }, C_SUB = { 150, 142, 132, 255 },
				 C_WOOD = { 222, 184, 135, 255 }, C_WOOD_LIGHT = { 240, 212, 170, 255 }, C_WOOD_DARK = { 92, 58, 30, 255 },
				 C_BTN = { 34, 31, 37, 255 }, C_BTN_HOVER = { 48, 44, 52, 255 }, C_BLACK = { 0, 0, 0, 255 },
				 C_WHITE = { 255, 255, 255, 255 }, C_OFF = { 70, 67, 74, 255 }, C_OFF_EDGE = { 104, 99, 108, 255 };

enum { F_SMALL, F_BODY, F_BOLD, F_TITLE, F_COUNT };
static HFont s_Font[F_COUNT];
static int s_nFontsForHeight;
static float s_flScale = 1;
static int S( float v ) { return (int)( v * s_flScale + 0.5f ); }

static void SetupFonts( int screenTall )
{
	static int haveMono = -1;
	if ( haveMono < 0 )
		haveMono = s_pSurface->AddCustomFontFile( "Roboto Mono", "resource/triggerlook/RobotoMono-Regular.ttf" ) &
				   s_pSurface->AddCustomFontFile( "Roboto Mono", "resource/triggerlook/RobotoMono-Bold.ttf" );
	static const struct { int px, weight; } def[F_COUNT] = { { 12, 400 }, { 14, 400 }, { 14, 700 }, { 20, 700 } };
	for ( int i = 0; i < F_COUNT; i++ )
	{
		if ( !s_Font[i] )
			s_Font[i] = s_pSurface->CreateFont();
		if ( !haveMono || !s_pSurface->SetFontGlyphSet( s_Font[i], "Roboto Mono", S( def[i].px ), def[i].weight, 0, 0, ISurface::FONTFLAG_ANTIALIAS ) )
			s_pSurface->SetFontGlyphSet( s_Font[i], "Verdana", S( def[i].px ), def[i].weight, 0, 0, ISurface::FONTFLAG_ANTIALIAS );
	}
	s_nFontsForHeight = screenTall;
}

static void Rect( int x, int y, int w, int h, Col c ) { s_pSurface->DrawSetColor( c.r, c.g, c.b, c.a ); s_pSurface->DrawFilledRect( x, y, x + w, y + h ); }
static void Outline( int x, int y, int w, int h, Col c ) { s_pSurface->DrawSetColor( c.r, c.g, c.b, c.a ); s_pSurface->DrawOutlinedRect( x, y, x + w, y + h ); }

// a box with pixel-art corners: a p-thick border whose corners step in by one pixel
static void PixelBox( int x, int y, int w, int h, int p, Col fill, Col border )
{
	Rect( x + p, y + p, w - 2 * p, h - 2 * p, fill );
	Rect( x + p, y, w - 2 * p, p, border );
	Rect( x + p, y + h - p, w - 2 * p, p, border );
	Rect( x, y + p, p, h - 2 * p, border );
	Rect( x + w - p, y + p, p, h - 2 * p, border );
}

// UTF-8 (our labels: ASCII and the middle dot) for ISurface
static std::wstring Widen( const char *s )
{
	std::wstring out;
	for ( const unsigned char *p = (const unsigned char *)s; *p; )
	{
		unsigned c = *p++;
		if ( c >= 0xC0 && c < 0xE0 && ( p[0] & 0xC0 ) == 0x80 )
			c = ( ( c & 0x1F ) << 6 ) | ( *p++ & 0x3F );
		else if ( c >= 0xE0 && c < 0xF0 && ( p[0] & 0xC0 ) == 0x80 && ( p[1] & 0xC0 ) == 0x80 )
		{
			c = ( ( c & 0x0F ) << 12 ) | ( ( p[0] & 0x3F ) << 6 ) | ( p[1] & 0x3F );
			p += 2;
		}
		else if ( c >= 0x80 )
			c = '?';
		out += (wchar_t)c;
	}
	return out;
}

static int TextW( int f, const char *s )
{
	int w = 0, h = 0;
	std::wstring t = Widen( s );
	if ( s_Font[f] && !t.empty() )
		s_pSurface->GetTextSize( s_Font[f], t.c_str(), w, h );
	return w;
}

static int TextH( int f ) { return s_Font[f] ? s_pSurface->GetFontTall( s_Font[f] ) : 0; }

static void Text( int f, int x, int y, Col c, const char *s )
{
	std::wstring t = Widen( s );
	if ( !s_Font[f] || t.empty() )
		return;
	s_pSurface->DrawSetTextFont( s_Font[f] );
	s_pSurface->DrawSetTextColor( c.r, c.g, c.b, c.a );
	s_pSurface->DrawSetTextPos( x, y );
	s_pSurface->DrawPrintText( t.c_str(), (int)t.size() );
}

static std::string FitRight( int f, const std::string &s, int w )
{
	if ( TextW( f, s.c_str() ) <= w )
		return s;
	for ( size_t n = s.size(); n > 0; n-- )
	{
		std::string t = s.substr( 0, n ) + "...";
		if ( TextW( f, t.c_str() ) <= w )
			return t;
	}
	return "...";
}

static Col Shade( Col c, float f ) { return { (int)( c.r * f ), (int)( c.g * f ), (int)( c.b * f ), c.a }; }
static Col Lift( Col c, float f ) { return { c.r + (int)( ( 255 - c.r ) * f ), c.g + (int)( ( 255 - c.g ) * f ), c.b + (int)( ( 255 - c.b ) * f ), c.a }; }
static bool Light( Col c ) { return c.r * 299 + c.g * 587 + c.b * 114 > 150 * 1000; }

//-----------------------------------------------------------------------------
// Immediate-mode input: the cursor, the left button and the wheel, read once per paint
//-----------------------------------------------------------------------------
static int s_mx, s_my;
static bool s_bClick, s_bDown;
static int s_nWheel;
static int s_nWheelUpTick = -1, s_nWheelDownTick = -1;

static bool Hover( int x, int y, int w, int h ) { return s_mx >= x && s_mx < x + w && s_my >= y && s_my < y + h; }
static bool ShiftDown() { return s_pInput->IsKeyDown( KEY_LSHIFT ) || s_pInput->IsKeyDown( KEY_RSHIFT ); }

static void ReadWheel( bool reset )
{
	if ( !s_pInputSystem )
		return;
	int up = s_pInputSystem->GetButtonPressedTick( MOUSE_WHEEL_UP ), down = s_pInputSystem->GetButtonPressedTick( MOUSE_WHEEL_DOWN );
	if ( !reset )
		s_nWheel = ( up != s_nWheelUpTick ? 1 : 0 ) - ( down != s_nWheelDownTick ? 1 : 0 );
	s_nWheelUpTick = up;
	s_nWheelDownTick = down;
}

// a button: pixel box, lighter when hovered, wood when "on", greyed out when not enabled. Returns true when clicked.
static bool Button( int x, int y, int w, int h, const char *label, bool on = false, bool enabled = true )
{
	bool hover = enabled && Hover( x, y, w, h );
	int p = MAX( 2, S( 3 ) );
	if ( enabled )
		PixelBox( x, y, w, h, p, on ? C_WOOD : hover ? C_BTN_HOVER : C_BTN, on ? C_WOOD_LIGHT : hover ? C_WOOD : C_WOOD_DARK );
	else
		PixelBox( x, y, w, h, p, C_BTN, C_OFF );
	Text( F_BOLD, x + ( w - TextW( F_BOLD, label ) ) / 2, y + ( h - TextH( F_BOLD ) ) / 2, !enabled ? C_OFF_EDGE : on ? C_BLACK : C_TEXT, label );
	return hover && s_bClick;
}

// a ring round a box, a gap away from it (white = picked, wood = under the cursor)
static void Ring( int x, int y, int w, int h, Col c )
{
	int p = MAX( 2, S( 3 ) ), g = MAX( 1, S( 2 ) );
	Rect( x - g - p, y - g - p, w + 2 * ( g + p ), p, c );
	Rect( x - g - p, y + h + g, w + 2 * ( g + p ), p, c );
	Rect( x - g - p, y - g, p, h + 2 * g, c );
	Rect( x + w + g, y - g, p, h + 2 * g, c );
}

// one colour block (the paint menu's: a ring when picked / hovered); true when clicked
static bool Swatch( int x, int y, int size, Col c, bool sel )
{
	Rect( x, y, size, size, c );
	Outline( x, y, size, size, { 255, 255, 255, 28 } );	// so black shows on the dark menu
	bool hover = Hover( x, y, size, size );
	if ( sel || hover )
	{
		int p2 = MAX( 2, S( 3 ) );
		Col ring = sel ? C_WHITE : C_WOOD;
		Rect( x, y - p2, size, p2, ring );
		Rect( x, y + size, size, p2, ring );
		Rect( x - p2, y, p2, size, ring );
		Rect( x + size, y, p2, size, ring );
	}
	return hover && s_bClick;
}

// a slider (lo..hi, whole steps): drag along it, or the wheel over it; true when the value changed
static int s_nDragSlider = -1;
static bool Slider( int id, int x, int y, int w, int h, int &value, int lo, int hi )
{
	const int p = MAX( 2, S( 3 ) );
	const int inner = w - 2 * p;
	bool hover = Hover( x, y - S( 6 ), w, h + S( 12 ) );
	if ( hover && s_bClick )
		s_nDragSlider = id;
	int v = value;
	if ( s_nDragSlider == id )
		v = lo + (int)floorf( ( s_mx - x - p ) * (float)( hi - lo ) / MAX( 1, inner ) + 0.5f );
	else if ( hover && s_nWheel )
		v += s_nWheel;
	v = clamp( v, lo, hi );
	PixelBox( x, y, w, h, p, hover || s_nDragSlider == id ? C_BTN_HOVER : C_BTN, hover || s_nDragSlider == id ? C_WOOD : C_WOOD_DARK );
	int fillW = inner * ( v - lo ) / MAX( 1, hi - lo );
	Rect( x + p, y + p, fillW, h - 2 * p, Shade( C_WOOD, 0.75f ) );
	int kw = MAX( 4, S( 8 ) );
	Rect( clamp( x + p + fillW - kw / 2, x + p, x + w - p - kw ), y + p, kw, h - 2 * p, C_WOOD_LIGHT );
	bool changed = v != value;
	value = v;
	return changed;
}

//-----------------------------------------------------------------------------
// colours
//-----------------------------------------------------------------------------
struct Preset_t { const char *name; int r, g, b; };
// the paint menu's 14 colours, in its grid order (light to dark, warm to cool; 2 rows of 7)
static const Preset_t s_Presets[14] =
{
	{ "White", 255, 255, 255 }, { "Light Pink", 255, 182, 193 }, { "Pink", 255, 20, 147 }, { "Red", 255, 0, 0 },
	{ "Orange", 255, 165, 0 }, { "Yellow", 255, 255, 0 }, { "Brown", 139, 69, 19 }, { "Green", 0, 255, 0 },
	{ "Dark Green", 0, 100, 0 }, { "Cyan", 0, 255, 255 }, { "Light Blue", 135, 206, 250 }, { "Blue", 0, 0, 255 },
	{ "Purple", 128, 0, 128 }, { "Black", 0, 0, 0 },
};

static void HsvToRgb( float h, float s, float v, int *rgb )
{
	h = fmodf( h, 360.0f ) / 60.0f;
	int i = (int)h;
	float f = h - i, p = v * ( 1 - s ), q = v * ( 1 - s * f ), t = v * ( 1 - s * ( 1 - f ) );
	float r, g, b;
	switch ( i )
	{
		case 0: r = v; g = t; b = p; break;
		case 1: r = q; g = v; b = p; break;
		case 2: r = p; g = v; b = t; break;
		case 3: r = p; g = q; b = v; break;
		case 4: r = t; g = p; b = v; break;
		default: r = v; g = p; b = q; break;
	}
	rgb[0] = (int)( r * 255 + 0.5f ); rgb[1] = (int)( g * 255 + 0.5f ); rgb[2] = (int)( b * 255 + 0.5f );
}

static void RgbToHsv( const int *rgb, float *h, float *s, float *v )
{
	float r = rgb[0] / 255.0f, g = rgb[1] / 255.0f, b = rgb[2] / 255.0f;
	float mx = MAX( r, MAX( g, b ) ), mn = MIN( r, MIN( g, b ) ), d = mx - mn;
	*v = mx;
	*s = mx > 0 ? d / mx : 0;
	if ( d <= 0 ) *h = 0;
	else if ( mx == r ) *h = 60 * fmodf( ( g - b ) / d + 6, 6 );
	else if ( mx == g ) *h = 60 * ( ( b - r ) / d + 2 );
	else *h = 60 * ( ( r - g ) / d + 4 );
}

//-----------------------------------------------------------------------------
// A bare VGUI panel (what vgui_controls' Panel does for us normally) - the paint menu's
//-----------------------------------------------------------------------------
class CTLBasePanel : public IClientPanel
{
public:
	CTLBasePanel( const char *name ) : m_VPanel( 0 ), m_pszName( name ) {}

	bool Create( bool popup )
	{
		if ( m_VPanel )
			return true;
		m_VPanel = s_pVGui->AllocPanel();
		if ( !m_VPanel )
			return false;
		s_pPanel->Init( m_VPanel, this );
		s_pPanel->SetParent( m_VPanel, s_pEngineVGui->GetPanel( PANEL_CLIENTDLL ) );
		int sw, sh;
		s_pSurface->GetScreenSize( sw, sh );
		s_pPanel->SetPos( m_VPanel, 0, 0 );
		s_pPanel->SetSize( m_VPanel, sw, sh );
		if ( popup )
		{
			s_pSurface->CreatePopup( m_VPanel, false, false, false, true, true );
			s_pPanel->SetMouseInputEnabled( m_VPanel, true );
			s_pPanel->SetKeyBoardInputEnabled( m_VPanel, true );
		}
		else
		{
			s_pPanel->SetMouseInputEnabled( m_VPanel, false );
			s_pPanel->SetKeyBoardInputEnabled( m_VPanel, false );
		}
		s_pPanel->SetVisible( m_VPanel, false );
		s_pVGui->AddTickSignal( m_VPanel, 0 );	// OnTick every frame
		return true;
	}

	void FitScreen()
	{
		int sw, sh, w, h;
		s_pSurface->GetScreenSize( sw, sh );
		s_pPanel->GetSize( m_VPanel, w, h );
		if ( w != sw || h != sh )
			s_pPanel->SetSize( m_VPanel, sw, sh );
	}

	virtual void Paint() = 0;

	// IClientPanel
	virtual VPANEL GetVPanel() { return m_VPanel; }
	virtual void Think() {}
	virtual void PerformApplySchemeSettings() {}
	virtual void PaintTraverse( bool forceRepaint, bool allowForce )
	{
		if ( !m_VPanel || !s_pPanel->IsVisible( m_VPanel ) )
			return;
		FitScreen();
		s_pSurface->PushMakeCurrent( m_VPanel, false );
		Paint();
		s_pSurface->PopMakeCurrent( m_VPanel );
	}
	virtual void Repaint() { s_pSurface->Invalidate( m_VPanel ); }
	virtual VPANEL IsWithinTraverse( int x, int y, bool traversePopups )
	{
		if ( !m_VPanel || !s_pPanel->IsVisible( m_VPanel ) || !s_pPanel->IsMouseInputEnabled( m_VPanel ) )
			return 0;
		return m_VPanel;
	}
	virtual void GetInset( int &top, int &left, int &right, int &bottom ) { top = left = right = bottom = 0; }
	virtual void GetClipRect( int &x0, int &y0, int &x1, int &y1 ) { s_pPanel->GetClipRect( m_VPanel, x0, y0, x1, y1 ); }
	virtual void OnChildAdded( VPANEL child ) {}
	virtual void OnSizeChanged( int newWide, int newTall ) {}
	virtual void InternalFocusChanged( bool lost ) {}
	virtual bool RequestInfo( KeyValues *outputData ) { return false; }
	virtual void RequestFocus( int direction ) {}
	virtual bool RequestFocusPrev( VPANEL existingPanel ) { return false; }
	virtual bool RequestFocusNext( VPANEL existingPanel ) { return false; }
	// (messages are KeyValues in the game's newer layout: we don't read them; input is polled instead)
	virtual void OnMessage( const KeyValues *params, VPANEL ifromPanel ) {}
	virtual VPANEL GetCurrentKeyFocus() { return 0; }
	virtual int GetTabPosition() { return 0; }
	virtual const char *GetName() { return m_pszName; }
	virtual const char *GetClassName() { return m_pszName; }
	virtual HScheme GetScheme() { return 0; }
	virtual bool IsProportional() { return false; }
	virtual bool IsAutoDeleteSet() { return false; }
	virtual void DeletePanel() {}
	virtual void *QueryInterface( EInterfaceID id ) { return NULL; }
	virtual Panel *GetPanel() { return NULL; }
	virtual const char *GetModuleName() { return "triggerlook"; }
	virtual void OnTick() {}

	bool IsOpen() const { return m_VPanel && s_pPanel->IsVisible( m_VPanel ); }
	void Show( bool show )
	{
		if ( !m_VPanel )
			return;
		s_pPanel->SetVisible( m_VPanel, show );
		if ( show )
		{
			s_pPanel->MoveToFront( m_VPanel );
			if ( s_pPanel->IsPopup( m_VPanel ) )
				s_pSurface->MovePopupToFront( m_VPanel );
		}
	}

protected:
	VPANEL m_VPanel;
	const char *m_pszName;
};

//-----------------------------------------------------------------------------
// The ticker: an always-shown panel; its OnTick runs the runtime every frame, it draws the SELECT TRIGGER bar
//-----------------------------------------------------------------------------
static bool s_bPicking;		// SELECT TRIGGER (below)
static void PaintPickBar();

class CTLTicker : public CTLBasePanel
{
public:
	CTLTicker() : CTLBasePanel( "TriggerLookTicker" ) {}
	virtual void Paint() { PaintPickBar(); }
	virtual void PaintTraverse( bool forceRepaint, bool allowForce ) { if ( s_bPicking ) CTLBasePanel::PaintTraverse( forceRepaint, allowForce ); }
	virtual void OnTick() { TL_Tick(); }
};
static CTLTicker s_Ticker;

// a custom trigger's name: the player's, else CT-n (its place among this map's)
static std::string CTName( int index )
{
	if ( index >= 0 && index < (int)g_TLCT.size() && !g_TLCT[index].name.empty() )
		return g_TLCT[index].name;
	char n[16];
	snprintf( n, sizeof( n ), "CT-%d", TL_CTLabel( index ) );
	return n;
}

// typing a name: the keys that make a character (US layout), as the input system sees them (a key pressed and let go
// within one frame still counts: its press tick changes)
struct TypeKey_t { ButtonCode_t code; char c, shifted; };
static std::vector<TypeKey_t> TypeKeys()
{
	std::vector<TypeKey_t> k;
	for ( int i = 0; i < 26; i++ )
		k.push_back( { (ButtonCode_t)( KEY_A + i ), (char)( 'a' + i ), (char)( 'A' + i ) } );
	for ( int i = 0; i < 10; i++ )
	{
		k.push_back( { (ButtonCode_t)( KEY_0 + i ), (char)( '0' + i ), ")!@#$%^&*("[i] } );
		k.push_back( { (ButtonCode_t)( KEY_PAD_0 + i ), (char)( '0' + i ), (char)( '0' + i ) } );
	}
	const TypeKey_t more[] = { { KEY_SPACE, ' ', ' ' }, { KEY_MINUS, '-', '_' }, { KEY_EQUAL, '=', '+' }, { KEY_LBRACKET, '[', '{' },
		{ KEY_RBRACKET, ']', '}' }, { KEY_SEMICOLON, ';', ':' }, { KEY_APOSTROPHE, '\'', '"' }, { KEY_COMMA, ',', '<' },
		{ KEY_PERIOD, '.', '>' }, { KEY_SLASH, '/', '?' }, { KEY_BACKSLASH, '\\', '|' }, { KEY_PAD_DIVIDE, '/', '/' },
		{ KEY_PAD_MULTIPLY, '*', '*' }, { KEY_PAD_MINUS, '-', '-' }, { KEY_PAD_PLUS, '+', '+' }, { KEY_PAD_DECIMAL, '.', '.' } };
	k.insert( k.end(), more, more + sizeof( more ) / sizeof( more[0] ) );
	return k;
}

//-----------------------------------------------------------------------------
// The menu
//-----------------------------------------------------------------------------
static void SetMenuOpen( bool open );
static void StartPick();

// the start of a long text cut off ("...folder/file") so its end shows
static std::string FitLeft( int f, const std::string &s, int w )
{
	for ( size_t n = 0; n < s.size(); n++ )
	{
		std::string t = ( n ? "..." : "" ) + s.substr( n );
		if ( TextW( f, t.c_str() ) <= w )
			return t;
	}
	return "...";
}

// a sky picture's / spray's thumbnail: its material as a VGUI texture (one each, kept)
static void Thumb( const TLPic_t &p, int x, int y, int size )
{
	static std::vector<std::pair<std::string, int> > s_Ids;
	int id = 0;
	for ( const auto &t : s_Ids )
		if ( t.first == p.material )
			id = t.second;
	if ( !id )
	{
		id = s_pSurface->CreateNewTextureID( false );
		s_pSurface->DrawSetTextureFile( id, p.material.c_str(), true, false );
		s_Ids.push_back( { p.material, id } );
	}
	Rect( x, y, size, size, { 36, 34, 40, 255 } );
	const float a = p.aspect;
	const int w = a >= 1 ? size : (int)( size * a ), h = a >= 1 ? (int)( size / a ) : size;
	s_pSurface->DrawSetTexture( id );
	s_pSurface->DrawSetColor( 255, 255, 255, 255 );
	s_pSurface->DrawTexturedRect( x + ( size - w ) / 2, y + ( size - h ) / 2, x + ( size + w ) / 2, y + ( size + h ) / 2 );
}

// the folder ADD A FILE opened last
static std::string LastDirFile() { return std::string( TLP_CacheDir() ) + "/lastdir.txt"; }
static std::string LastDir()
{
	char buf[1024] = "";
	if ( FILE *f = TLP_Open( LastDirFile().c_str(), "r" ) )
	{
		if ( !fgets( buf, sizeof( buf ), f ) )
			buf[0] = 0;
		fclose( f );
	}
	buf[strcspn( buf, "\r\n" )] = 0;
	return buf;
}

enum { VIEW_MAIN, VIEW_CUSTOM, VIEW_SKY, VIEW_FILES };
enum { PART_FILL, PART_BORDER };

static const int PAGE_W = 576;		// the content width at 1080p (the paint menu's)

class CTLMenu : public CTLBasePanel
{
public:
	CTLMenu() : CTLBasePanel( "TriggerLookMenu" ) {}

	void Opened()
	{
		m_nView = VIEW_MAIN;
		m_bPickOnRelease = false;
		m_nRename = -1;
		s_nDragSlider = -1;
		ReadWheel( true );
	}

	// tests / scripts (triggerlook_ui)
	void SetSelection( int mask, const std::vector<int> &cts )
	{
		if ( !mask && cts.empty() )
			return;
		m_nSel = mask;
		m_SelCT = cts;
		ValidateSel();
	}
	// a trigger just picked in the world: only it, and its button in sight
	void PickCT( int index )
	{
		m_nSel = 0;
		m_SelCT.assign( 1, index );
		m_nShowCT = index;
	}
	// DELETE: the picked custom triggers go (their triggers get their type's look again)
	void DeleteSelected()
	{
		std::vector<int> del = m_SelCT;
		std::sort( del.rbegin(), del.rend() );
		for ( int i : del )
			if ( i >= 0 && i < (int)g_TLCT.size() )
				g_TLCT.erase( g_TLCT.begin() + i );
		m_SelCT.clear();
		TL_CTListChanged();
		ValidateSel();
	}
	void SetPart( int part ) { m_nPart = part; }
	void SetView( int view )
	{
		if ( view == VIEW_CUSTOM )
			StartCustom();
		if ( view == VIEW_SKY )
			OpenSky( g_TLSky.mode );
		if ( view == VIEW_FILES )
			OpenFiles();
		m_nView = view;
	}

	// Esc: the engine opens its pause menu on Esc before VGUI sees the key (same as the paint menu)
	virtual void OnTick()
	{
		if ( !IsOpen() )
			return;
		if ( s_pEngineVGui->IsGameUIVisible() || s_pInput->WasKeyPressed( KEY_ESCAPE ) )
		{
			if ( s_pEngineVGui->IsGameUIVisible() )
				s_pEngine->ClientCmd_Unrestricted( "gameui_hide" );
			if ( m_nRename >= 0 )
				EndRename( false );		// Esc while naming: only the naming stops
			else
				SetMenuOpen( false );
		}
	}

	// naming a custom trigger: ends with Enter / a click outside / Esc (false: the old name stays)
	void EndRename( bool keep )
	{
		if ( m_nRename < 0 )
			return;
		if ( keep && m_nRename < (int)g_TLCT.size() )
		{
			g_TLCT[m_nRename].name = TL_CleanName( m_Rename );
			TL_LookChanged( 0 );
		}
		m_nRename = -1;
	}

	virtual void Paint()
	{
		if ( !s_pEngine || !s_pEngine->IsInGame() )
		{
			SetMenuOpen( false );	// left the server with it open
			return;
		}
		if ( s_pEngineVGui->IsGameUIVisible() || s_pInput->WasKeyPressed( KEY_ESCAPE ) )
		{
			if ( s_pEngineVGui->IsGameUIVisible() )
				s_pEngine->ClientCmd_Unrestricted( "gameui_hide" );
			if ( m_nRename < 0 )
			{
				SetMenuOpen( false );
				return;
			}
			EndRename( false );
		}
		int sw, sh;
		s_pSurface->GetScreenSize( sw, sh );
		s_flScale = sh / 1080.0f;
		if ( s_nFontsForHeight != sh || !s_Font[F_BODY] || !s_pSurface->GetFontTall( s_Font[F_BODY] ) )
			SetupFonts( sh );
		ValidateSel();
		s_pInput->GetCursorPos( s_mx, s_my );
		bool down = s_pInput->IsMouseDown( MOUSE_LEFT );
		s_bClick = down && !s_bDown;
		s_bDown = down;
		if ( m_nRename >= 0 )
			RenameInput();
		// the custom trigger buttons: as many rows as fit on the screen, the wheel scrolls the rest
		const int gapL = S( 10 ), wL = S( PAGE_W );
		LayoutCTs( wL, ( wL - 4 * gapL ) / 5, gapL );
		m_nCTRows = MIN( m_nCTRowsAll, MAX( 1, ( sh - S( 48 ) - S( 54 ) - S( 22 ) - ContentH( 0 ) ) / S( 52 ) ) );
		m_nCTScroll = clamp( m_nCTScroll, 0, m_nCTRowsAll - m_nCTRows );
		if ( !down )
			s_nDragSlider = -1;
		ReadWheel( false );
		// SELECT TRIGGER: the menu goes once the mouse button is up again (the release stays with the menu)
		if ( m_bPickOnRelease && !down )
		{
			m_bPickOnRelease = false;
			StartPick();
			return;
		}

		// no dimming: the triggers behind are what you're looking at. At the left so the middle stays clear.
		const int pad = S( 22 );
		const int W = S( PAGE_W ) + 2 * pad, H = S( 54 ) + ContentH( m_nCTRows ) + pad;
		int x0 = S( 48 ), y0 = ( sh - H ) / 2;
		PixelBox( x0, y0, W, H, MAX( 2, S( 4 ) ), C_BG, C_WOOD_DARK );
		static const char *const s_Titles[] = { "TRIGGERS", "CUSTOM COLOUR", "SKYBOX", "ADD A FILE" };
		Text( F_TITLE, x0 + pad, y0 + S( 14 ), C_WOOD_LIGHT, s_Titles[m_nView] );
		if ( Button( x0 + W - pad - S( 32 ), y0 + S( 12 ), S( 32 ), S( 28 ), "X" ) )
		{
			SetMenuOpen( false );
			return;
		}
		if ( m_nView != VIEW_MAIN && Button( x0 + W - pad - S( 32 ) - S( 8 ) - S( 84 ), y0 + S( 12 ), S( 84 ), S( 28 ), "BACK" ) )
		{
			m_nView = m_nView == VIEW_FILES || ( m_nView == VIEW_CUSTOM && m_bSkyColour ) ? VIEW_SKY : VIEW_MAIN;
			s_bClick = false;
		}
		// ON / OFF: draw the map's triggers at all
		if ( m_nView == VIEW_MAIN &&
			 ShowButton( x0 + pad + TextW( F_TITLE, "TRIGGERS" ) + S( 16 ), y0 + S( 12 ), S( 72 ), S( 28 ), g_bTLShow ? "ON" : "OFF", g_bTLShow ) )
		{
			g_bTLShow = !g_bTLShow;
			TL_LookChanged( 0 );
		}
		int cx = x0 + pad, cy = y0 + S( 54 ), cw = W - 2 * pad;
		if ( m_nView == VIEW_CUSTOM )
			CustomView( cx, cy, cw );
		else if ( m_nView == VIEW_SKY )
			SkyView( cx, cy, cw, ContentH( m_nCTRows ) );
		else if ( m_nView == VIEW_FILES )
			FilesView( cx, cy, cw, ContentH( m_nCTRows ) );
		else
			MainView( cx, cy, cw );
	}

private:
	int m_nView = VIEW_MAIN;
	int m_nSel = 1 << TL_TELEPORT;		// the types EDIT picked
	std::vector<int> m_SelCT;			// the custom triggers picked (indices into g_TLCT)
	int m_nPart = PART_FILL;
	int m_nHover = -1;					// type under the cursor (for the line at the bottom)
	int m_nHoverCT = -1;				// custom trigger under the cursor
	int m_nCTRows = 0, m_nCTScroll = 0;	// custom trigger rows shown, the first one shown
	int m_nShowCT = -1;					// scroll to this custom trigger's button
	std::vector<int> m_CTX, m_CTRow, m_CTW;	// this map's custom trigger buttons: x from the left, row, width
	int m_nCTRowsAll = 0;
	int m_nLastClickCT = -1;			// double click = two clicks on one button within 0.4 s
	double m_flLastClick = 0;
	int m_nRename = -1;					// the custom trigger being named (index into g_TLCT), -1 = none
	std::string m_Rename;				// its name so far
	bool m_bRenameAll = false;			// all of it is selected: typing replaces it
	int m_RenameRect[4] = {};			// the name field (last frame): a click outside it ends the naming
	std::vector<int> m_KeyTick;			// the input system's press ticks of TypeKeys(), backspace, enter...
	double m_flBackHeld = -1;			// backspace held since (repeats)
	bool m_bPickOnRelease = false;		// SELECT TRIGGER clicked
	// custom colour picker
	enum { SV_CELLS = 24, HUE_CELLS = 24 };
	int m_nHue = 0, m_nSat = 0, m_nVal = 0;
	bool m_bGreys = false;
	bool m_bSkyColour = false;			// the picker edits the sky's colour (opened from the SKYBOX page)
	// SKYBOX page: the list shown (TL_SKY_*), scrolled by rows; ADD A FILE's folder and what's in it
	int m_nSkyTab = TL_SKY_COLOUR, m_nSkyScroll = 0, m_nFileScroll = 0;
	std::string m_Dir;
	std::vector<TLDirEntry_t> m_Entries;

	// the same height on every page: SHOW + EDIT (two rows of 4 types each), CUSTOM TRIGGERS, FILL / BORDER, colours,
	// CUSTOM, the sliders, SKYBOX, the line at the bottom
	int ContentH( int ctRows ) const
	{
		return S( 22 + 80 + 14 ) * 2 + S( 22 + 42 + 14 ) + ctRows * S( 52 ) + S( 42 + 14 ) + S( 22 + 2 * 80 ) + S( 22 + 70 + 14 ) +
			   S( 22 + 28 + 14 ) + S( 22 + 42 + 14 ) + S( 22 );
	}
	// type t's button in a grid of 4 per row
	void TypeRect( int x, int y, int w, int t, int &bx, int &by, int &bw, int &bh ) const
	{
		const int gap = S( 10 );
		bw = ( w - 3 * gap ) / 4;
		bh = S( 36 );
		bx = x + ( t % 4 ) * ( bw + gap );
		by = y + ( t / 4 ) * ( bh + S( 8 ) );
	}

	bool CTPicked( int index ) const { return std::find( m_SelCT.begin(), m_SelCT.end(), index ) != m_SelCT.end(); }

	// everything picked: the types, then the custom triggers (never empty: ValidateSel)
	std::vector<TLLook_t *> Picked()
	{
		std::vector<TLLook_t *> out;
		for ( int t = 0; t < TL_TYPES; t++ )
			if ( m_nSel & ( 1 << t ) )
				out.push_back( &g_TLLook[t] );
		for ( int i : m_SelCT )
			out.push_back( &g_TLCT[i].look );
		return out;
	}
	TLLook_t &FirstLook() { return *Picked()[0]; }
	void Changed()
	{
		TL_LookChanged( m_nSel );
		for ( int i : m_SelCT )
			TL_CTChanged( i );
	}

	// custom triggers not on this map (another map, deleted) drop out; one thing always stays picked
	void ValidateSel()
	{
		const std::vector<int> &cts = TL_MapCTs();
		for ( size_t k = 0; k < m_SelCT.size(); )
			if ( std::find( cts.begin(), cts.end(), m_SelCT[k] ) == cts.end() )
				m_SelCT.erase( m_SelCT.begin() + k );
			else
				k++;
		if ( !m_nSel && m_SelCT.empty() )
			m_nSel = 1 << TL_TELEPORT;
		if ( m_nRename >= 0 && std::find( cts.begin(), cts.end(), m_nRename ) == cts.end() )
			m_nRename = -1;
	}

	// ---- custom trigger buttons: left to right, as wide as their names, a new row when one doesn't fit --------
	void LayoutCTs( int w, int minW, int gap )
	{
		const std::vector<int> &cts = TL_MapCTs();
		m_CTX.clear(); m_CTRow.clear(); m_CTW.clear();
		int x = 0, row = 0;
		for ( int gi : cts )
		{
			std::string label = gi == m_nRename ? m_Rename + "W" : CTName( gi );	// (room for the caret)
			int cw = clamp( TextW( F_BOLD, label.c_str() ) + S( 28 ), minW, w );
			if ( x > 0 && x + cw > w )
			{
				row++;
				x = 0;
			}
			m_CTX.push_back( x );
			m_CTRow.push_back( row );
			m_CTW.push_back( cw );
			x += cw + gap;
		}
		m_nCTRowsAll = cts.empty() ? 0 : row + 1;
	}

	// ---- naming a custom trigger (double click its button) ----------------------------------------------------
	void StartRename( int index )
	{
		m_nRename = index;
		m_Rename = CTName( index );
		m_bRenameAll = true;
		m_RenameRect[2] = m_RenameRect[3] = 0;
		m_flBackHeld = -1;
		ReadKeys( true );
	}

	// the keys pressed since the last frame, in the order pressed: [ (tick, code index) ]
	std::vector<TypeKey_t> m_Keys;
	std::vector<std::pair<int, int> > ReadKeys( bool reset )
	{
		if ( m_Keys.empty() )
		{
			m_Keys = TypeKeys();
			const ButtonCode_t special[] = { KEY_BACKSPACE, KEY_DELETE, KEY_ENTER, KEY_PAD_ENTER };
			for ( ButtonCode_t c : special )
				m_Keys.push_back( { c, 0, 0 } );
		}
		std::vector<std::pair<int, int> > pressed;
		m_KeyTick.resize( m_Keys.size(), 0 );
		for ( size_t i = 0; i < m_Keys.size() && s_pInputSystem; i++ )
		{
			int tick = s_pInputSystem->GetButtonPressedTick( m_Keys[i].code );
			if ( !reset && tick != m_KeyTick[i] )
				pressed.push_back( std::make_pair( tick, (int)i ) );
			m_KeyTick[i] = tick;
		}
		std::sort( pressed.begin(), pressed.end() );
		return pressed;
	}

	void RenameInput()
	{
		// a click outside the field: done (the click still does what it was for); inside: the selection goes
		if ( s_bClick )
		{
			const int *r = m_RenameRect;
			if ( r[2] && !Hover( r[0], r[1], r[2], r[3] ) )
			{
				EndRename( true );
				return;
			}
			m_bRenameAll = false;
		}
		const bool shift = s_pInputSystem && ( s_pInputSystem->IsButtonDown( KEY_LSHIFT ) || s_pInputSystem->IsButtonDown( KEY_RSHIFT ) );
		const bool caps = s_pInputSystem && s_pInputSystem->IsButtonDown( KEY_CAPSLOCKTOGGLE );
		const bool ctrl = s_pInputSystem && ( s_pInputSystem->IsButtonDown( KEY_LCONTROL ) || s_pInputSystem->IsButtonDown( KEY_RCONTROL ) );
		for ( const std::pair<int, int> &p : ReadKeys( false ) )
		{
			const TypeKey_t &k = m_Keys[p.second];
			if ( k.code == KEY_ENTER || k.code == KEY_PAD_ENTER )
			{
				EndRename( true );
				return;
			}
			if ( k.code == KEY_BACKSPACE || k.code == KEY_DELETE )
			{
				if ( m_bRenameAll )
					m_Rename.clear();
				else if ( k.code == KEY_BACKSPACE && !m_Rename.empty() )
					m_Rename.pop_back();
				m_bRenameAll = false;
				if ( k.code == KEY_BACKSPACE )
					m_flBackHeld = Plat_FloatTime();
				continue;
			}
			if ( ctrl )
			{
				if ( k.code == KEY_A )
					m_bRenameAll = true;		// ctrl + A: all of it
				continue;
			}
			const bool upper = k.code >= KEY_A && k.code <= KEY_Z ? shift != caps : shift;
			if ( m_bRenameAll )
				m_Rename.clear();
			m_bRenameAll = false;
			if ( (int)m_Rename.size() < TL_NAME_MAX )
				m_Rename += upper ? k.shifted : k.c;
		}
		// backspace held: one character every 35 ms after 0.45 s
		if ( m_flBackHeld >= 0 )
		{
			const double now = Plat_FloatTime();
			if ( !s_pInputSystem || !s_pInputSystem->IsButtonDown( KEY_BACKSPACE ) )
				m_flBackHeld = -1;
			else if ( now - m_flBackHeld > 0.45 )
			{
				if ( !m_Rename.empty() )
					m_Rename.pop_back();
				m_flBackHeld = now - 0.45 + 0.035;
			}
		}
	}

	// the field in place of the button: the name (selected at first), a blinking caret
	void RenameField( int x, int y, int w, int h )
	{
		m_RenameRect[0] = x; m_RenameRect[1] = y; m_RenameRect[2] = w; m_RenameRect[3] = h;
		PixelBox( x, y, w, h, MAX( 2, S( 3 ) ), C_BTN, C_WOOD_LIGHT );
		const int tw = TextW( F_BOLD, m_Rename.c_str() ), th = TextH( F_BOLD );
		const int tx = x + ( w - tw ) / 2, ty = y + ( h - th ) / 2;
		if ( m_bRenameAll && !m_Rename.empty() )
		{
			Rect( tx - S( 2 ), ty, tw + S( 4 ), th, C_WOOD );
			Text( F_BOLD, tx, ty, C_BLACK, m_Rename.c_str() );
		}
		else
		{
			Text( F_BOLD, tx, ty, C_TEXT, m_Rename.c_str() );
			if ( (long long)( Plat_FloatTime() * 10 ) % 10 < 6 )		// (fmod would need a newer glibc)
				Rect( tx + tw + S( 1 ), ty, MAX( 1, S( 2 ) ), th, C_TEXT );
		}
		Ring( x, y, w, h, C_WHITE );
	}

	// click = only this one, shift + click = add / remove (one always stays picked)
	void ClickItem( int typeBit, int ct )
	{
		std::vector<int> cts;
		if ( !ShiftDown() )
		{
			m_nSel = typeBit;
			if ( ct >= 0 )
				cts.push_back( ct );
			m_SelCT = cts;
			return;
		}
		cts = m_SelCT;
		if ( ct >= 0 )
		{
			std::vector<int>::iterator it = std::find( cts.begin(), cts.end(), ct );
			if ( it != cts.end() )
				cts.erase( it );
			else
				cts.push_back( ct );
		}
		int sel = m_nSel ^ typeBit;
		if ( sel || !cts.empty() )
		{
			m_nSel = sel;
			m_SelCT = cts;
		}
	}

	unsigned char *Part( TLLook_t &l ) { return m_nPart == PART_FILL ? l.fill : l.border; }

	// does everything picked have this colour in the current part?
	bool AllHave( int r, int g, int b )
	{
		for ( TLLook_t *l : Picked() )
			if ( Part( *l )[0] != r || Part( *l )[1] != g || Part( *l )[2] != b )
				return false;
		return true;
	}

	// a colour for the current part of everything picked; a part that was invisible gets seen (a border gets a width)
	void ApplyRGB( int r, int g, int b )
	{
		for ( TLLook_t *l : Picked() )
		{
			unsigned char *c = Part( *l );
			c[0] = (unsigned char)r; c[1] = (unsigned char)g; c[2] = (unsigned char)b;
			if ( !c[3] )
				c[3] = m_nPart == PART_FILL ? 115 : 255;
			if ( m_nPart == PART_BORDER && !l->width )
				l->width = 2;
		}
		Changed();
	}

	// ---- the main page ----------------------------------------------------------------------------------
	void MainView( int x, int y, int w )
	{
		const int gap = S( 10 ), label = S( 22 ), bh = S( 42 ), hw = ( w - gap ) / 2;
		int cy = y, bx, by, tbw, tbh;
		m_nHover = m_nHoverCT = -1;

		// SHOW: white = drawn, grey = hidden
		Text( F_BOLD, x, cy, C_WOOD, "SHOW" );
		cy += label;
		for ( int t = 0; t < TL_TYPES; t++ )
		{
			TypeRect( x, cy, w, t, bx, by, tbw, tbh );
			if ( ShowButton( bx, by, tbw, tbh, g_TLTypeLabel[t], g_TLLook[t].show ) )
			{
				g_TLLook[t].show = !g_TLLook[t].show;
				TL_LookChanged( 0 );	// (drawn or not: no mesh change)
			}
			if ( Hover( bx, by, tbw, tbh ) )
				m_nHover = t;
		}
		cy += S( 80 + 14 );

		// EDIT: each type in its own colours; the settings below change the picked ones
		Text( F_BOLD, x, cy, C_WOOD, "EDIT" );
		const char *note = "hold shift + click to pick more than one";
		Text( F_SMALL, x + w - TextW( F_SMALL, note ), cy + ( TextH( F_BOLD ) - TextH( F_SMALL ) ) / 2, C_SUB, note );
		cy += label;
		for ( int t = 0; t < TL_TYPES; t++ )
		{
			TypeRect( x, cy, w, t, bx, by, tbw, tbh );
			if ( ItemButton( bx, by, tbw, tbh, g_TLLook[t], g_TLTypeLabel[t], ( m_nSel & ( 1 << t ) ) != 0 ) )
				ClickItem( 1 << t, -1 );
			if ( Hover( bx, by, tbw, tbh ) )
				m_nHover = t;
		}
		cy += S( 80 + 14 );

		// CUSTOM TRIGGERS: SELECT TRIGGER picks one in the world (CT-1, CT-2, ... in the order picked); their buttons
		// are picked like the EDIT ones; DELETE = the picked ones get their type's look again
		const std::vector<int> &cts = TL_MapCTs();
		const int ctRows = m_nCTRowsAll;
		Text( F_BOLD, x, cy, C_WOOD, "CUSTOM TRIGGERS" );
		if ( ctRows > m_nCTRows )
		{
			const char *more = "wheel = more";
			Text( F_SMALL, x + w - TextW( F_SMALL, more ), cy + ( TextH( F_BOLD ) - TextH( F_SMALL ) ) / 2, C_SUB, more );
		}
		cy += label;
		if ( Button( x, cy, hw, bh, "SELECT TRIGGER" ) )
			m_bPickOnRelease = true;
		if ( Button( x + hw + gap, cy, hw, bh, "DELETE", false, !m_SelCT.empty() ) )
			DeleteSelected();
		cy += bh;
		for ( size_t k = 0; k < cts.size() && k < m_CTRow.size(); k++ )
			if ( cts[k] == m_nShowCT || cts[k] == m_nRename )
				m_nCTScroll = clamp( m_nCTScroll, m_CTRow[k] - m_nCTRows + 1, m_CTRow[k] );
		m_nShowCT = -1;
		if ( m_nCTRows && Hover( x, cy, w, m_nCTRows * S( 52 ) + S( 10 ) ) && s_nWheel )
			m_nCTScroll = clamp( m_nCTScroll - s_nWheel, 0, ctRows - m_nCTRows );
		for ( size_t k = 0; k < cts.size() && k < m_CTX.size(); k++ )
		{
			const int row = m_CTRow[k] - m_nCTScroll, gi = cts[k];
			if ( row < 0 || row >= m_nCTRows )
				continue;
			const int bx = x + m_CTX[k], by = cy + S( 10 ) + row * S( 52 ), bw = m_CTW[k];
			if ( gi == m_nRename )
			{
				RenameField( bx, by, bw, bh );
				continue;
			}
			if ( ItemButton( bx, by, bw, bh, g_TLCT[gi].look, CTName( gi ).c_str(), CTPicked( gi ) ) )
			{
				// a second click on the same button soon after: name it
				const double now = Plat_FloatTime();
				if ( gi == m_nLastClickCT && now - m_flLastClick < 0.4 && !ShiftDown() )
				{
					ClickItem( 0, gi );
					StartRename( gi );
					m_nLastClickCT = -1;
				}
				else
				{
					ClickItem( 0, gi );
					m_nLastClickCT = gi;
					m_flLastClick = now;
				}
			}
			if ( Hover( bx, by, bw, bh ) )
				m_nHoverCT = gi;
		}
		cy += m_nCTRows * S( 52 ) + S( 14 );

		// FILL / BORDER: which part the colours and OPACITY change
		if ( Button( x, cy, hw, bh, "FILL", m_nPart == PART_FILL ) )
			m_nPart = PART_FILL;
		if ( Button( x + hw + gap, cy, hw, bh, "BORDER", m_nPart == PART_BORDER ) )
			m_nPart = PART_BORDER;
		cy += bh + S( 14 );

		// the paint menu's colour blocks
		const int thumb = S( 70 ), perRow = 7;
		Text( F_BOLD, x, cy, C_WOOD, m_nPart == PART_FILL ? "FILL COLOUR" : "BORDER COLOUR" );
		cy += label;
		for ( int i = 0; i < 14; i++ )
		{
			const Preset_t &pr = s_Presets[i];
			int tx = x + ( i % perRow ) * ( thumb + gap ), ty = cy + ( i / perRow ) * ( thumb + gap );
			if ( Swatch( tx, ty, thumb, { pr.r, pr.g, pr.b, 255 }, AllHave( pr.r, pr.g, pr.b ) ) )
				ApplyRGB( pr.r, pr.g, pr.b );
		}
		cy += 2 * ( thumb + gap );

		// your custom colour (the last one from CUSTOM COLOUR) and the picker
		Text( F_BOLD, x, cy, C_WOOD, "CUSTOM" );
		cy += label;
		if ( Swatch( x, cy, thumb, { g_TLCustom[0], g_TLCustom[1], g_TLCustom[2], 255 }, AllHave( g_TLCustom[0], g_TLCustom[1], g_TLCustom[2] ) ) )
			ApplyRGB( g_TLCustom[0], g_TLCustom[1], g_TLCustom[2] );
		int px = x + thumb + gap;
		if ( Button( px, cy + ( thumb - bh ) / 2, x + w - px, bh, "CUSTOM COLOUR" ) )
		{
			m_bSkyColour = false;
			StartCustom();
			m_nView = VIEW_CUSTOM;
			s_bClick = false;
		}
		cy += thumb + S( 14 );

		// OPACITY (the current part) and WIDTH (the border), side by side
		TLLook_t &first = FirstLook();
		int sx2 = x + hw + gap;
		int pct = (int)( Part( first )[3] * 100 / 255.0f + 0.5f );
		char val[32];
		snprintf( val, sizeof( val ), "%d%%", pct );
		Text( F_BOLD, x, cy, C_WOOD, m_nPart == PART_FILL ? "FILL OPACITY" : "BORDER OPACITY" );
		Text( F_BOLD, x + hw - TextW( F_BOLD, val ), cy, C_TEXT, val );
		if ( first.width )
			snprintf( val, sizeof( val ), "%d", first.width );
		else
			TL_StrCopy( val, "OFF", sizeof( val ) );
		Text( F_BOLD, sx2, cy, C_WOOD, "BORDER WIDTH" );
		Text( F_BOLD, x + w - TextW( F_BOLD, val ), cy, C_TEXT, val );
		cy += label;
		if ( Slider( 0, x, cy, hw, S( 28 ), pct, 0, 100 ) )
		{
			for ( TLLook_t *l : Picked() )
				Part( *l )[3] = (unsigned char)( pct * 255 / 100.0f + 0.5f );
			Changed();
		}
		int width = first.width;
		if ( Slider( 1, sx2, cy, hw, S( 28 ), width, 0, TL_MAX_WIDTH ) )
		{
			for ( TLLook_t *l : Picked() )
			{
				l->width = width;
				if ( width && !l->border[3] )
					l->border[3] = 255;
			}
			Changed();
		}
		cy += S( 28 ) + S( 14 );

		// SKYBOX: what's in place of the map's sky (its page picks the colour / spray / picture)
		Text( F_BOLD, x, cy, C_WOOD, "SKYBOX" );
		const std::string now = SkyText();
		Text( F_SMALL, x + w - TextW( F_SMALL, now.c_str() ), cy + ( TextH( F_BOLD ) - TextH( F_SMALL ) ) / 2, C_SUB, now.c_str() );
		cy += label;
		SkyModes( x, cy, w );
		cy += bh + S( 14 );

		// the line at the bottom: why nothing shows, else what the type / custom trigger under the cursor (or the
		// first picked) has
		char line[256];
		int inMap = 0;
		for ( int t = 0; t < TL_TYPES; t++ )
			inMap += TL_InMap( t );
		if ( m_nRename >= 0 )
			snprintf( line, sizeof( line ), "type a name (up to %d)  \xC2\xB7  Enter: done  \xC2\xB7  Esc: keep the old one  \xC2\xB7  empty = CT-%d",
						(int)TL_NAME_MAX, TL_CTLabel( m_nRename ) );
		else if ( !g_bTLShow )
			TL_StrCopy( line, "OFF: click ON at the top to draw this map's triggers (or bind a key to triggerlook_show)", sizeof( line ) );
		else if ( !TL_Active() )
			TL_StrCopy( line, "off: triggerlook_enable is 0 (the game's own trigger look)", sizeof( line ) );
		else if ( !inMap )
			TL_StrCopy( line, "this map has no triggers or clips", sizeof( line ) );
		else
		{
			const int ct = m_nHoverCT >= 0 ? m_nHoverCT : m_nHover < 0 && !m_nSel ? m_SelCT[0] : -1;
			const int t = m_nHover >= 0 ? m_nHover : m_nSel ? __builtin_ctz( m_nSel ) : 0;
			const TLLook_t &l = ct >= 0 ? g_TLCT[ct].look : g_TLLook[t];
			char what[64], border[64];
			if ( ct >= 0 )
			{
				int type = TL_CTType( ct );
				snprintf( what, sizeof( what ), "%s (%s)", CTName( ct ).c_str(), type >= 0 ? g_TLTypeLabel[type] : "?" );
			}
			else
				snprintf( what, sizeof( what ), "%s  \xC2\xB7  %d in the map", g_TLTypeLabel[t], TL_InMap( t ) );
			if ( l.width && l.border[3] )
				snprintf( border, sizeof( border ), "border #%02X%02X%02X %d%% width %d", l.border[0], l.border[1], l.border[2],
							(int)( l.border[3] * 100 / 255.0f + 0.5f ), l.width );
			else
				TL_StrCopy( border, "no border", sizeof( border ) );
			snprintf( line, sizeof( line ), "%s  \xC2\xB7  fill #%02X%02X%02X %d%%  \xC2\xB7  %s%s", what, l.fill[0], l.fill[1], l.fill[2],
						(int)( l.fill[3] * 100 / 255.0f + 0.5f ), border, ct < 0 && !l.show ? "  \xC2\xB7  hidden" : "" );
		}
		Text( F_SMALL, x, cy + ( S( 22 ) - TextH( F_SMALL ) ) / 2, C_SUB, FitRight( F_SMALL, line, w ).c_str() );
	}

	// SHOW: white = on, grey = off
	bool ShowButton( int x, int y, int w, int h, const char *label, bool on )
	{
		bool hover = Hover( x, y, w, h );
		int p = MAX( 2, S( 3 ) );
		Col fill = on ? C_TEXT : C_OFF, edge = on ? C_WHITE : C_OFF_EDGE;
		if ( hover )
			fill = on ? Shade( fill, 0.88f ) : Lift( fill, 0.12f );
		PixelBox( x, y, w, h, p, fill, edge );
		Text( F_BOLD, x + ( w - TextW( F_BOLD, label ) ) / 2, y + ( h - TextH( F_BOLD ) ) / 2, on ? C_BLACK : C_SUB, label );
		return hover && s_bClick;
	}

	// EDIT / CUSTOM TRIGGERS: the fill colour inside, the border colour round it (a darker / lighter edge without a
	// border), a white ring when picked
	bool ItemButton( int x, int y, int w, int h, const TLLook_t &l, const char *label, bool sel )
	{
		bool hover = Hover( x, y, w, h );
		Col fill = { l.fill[0], l.fill[1], l.fill[2], 255 };
		Col edge = l.width && l.border[3] ? Col{ l.border[0], l.border[1], l.border[2], 255 } : Light( fill ) ? Shade( fill, 0.6f ) : Lift( fill, 0.3f );
		int p = MAX( 2, S( 3 ) ) * ( l.width && l.border[3] ? 2 : 1 );
		PixelBox( x, y, w, h, p, fill, edge );
		if ( hover )
			Rect( x + p, y + p, w - 2 * p, h - 2 * p, Light( fill ) ? Col{ 0, 0, 0, 30 } : Col{ 255, 255, 255, 30 } );
		Text( F_BOLD, x + ( w - TextW( F_BOLD, label ) ) / 2, y + ( h - TextH( F_BOLD ) ) / 2, Light( fill ) ? C_BLACK : C_WHITE, label );
		if ( sel || hover )
			Ring( x, y, w, h, sel ? C_WHITE : C_WOOD );
		return hover && s_bClick;
	}

	// ---- custom colour: the paint menu's picker; every change shows on the triggers at once --------------
	void StartCustom()
	{
		const unsigned char *c = m_bSkyColour ? g_TLSky.rgb : Part( FirstLook() );
		int rgb[3] = { c[0], c[1], c[2] };
		float hh, ss, vv;
		RgbToHsv( rgb, &hh, &ss, &vv );
		m_nHue = (int)( hh / 360.0f * HUE_CELLS + 0.5f ) % HUE_CELLS;
		m_nSat = clamp( (int)( ss * ( SV_CELLS - 1 ) + 0.5f ), 0, SV_CELLS - 1 );
		m_nVal = clamp( (int)( ( 1 - vv ) * ( SV_CELLS - 1 ) + 0.5f ), 0, SV_CELLS - 1 );
		m_bGreys = ss < 0.02f;
	}

	void PickedRGB( int *rgb )
	{
		float s = m_bGreys ? 0.0f : m_nSat / (float)( SV_CELLS - 1 ), v = 1.0f - m_nVal / (float)( SV_CELLS - 1 );
		HsvToRgb( m_nHue * 360.0f / HUE_CELLS, s, v, rgb );
	}

	void CustomView( int x, int y, int w )
	{
		int before[3];
		PickedRGB( before );
		const int cell = S( 13 ), sv = SV_CELLS * cell;
		// saturation (across) x brightness (down) for the chosen hue
		bool dragging = s_bDown && Hover( x, y, sv, sv );
		for ( int j = 0; j < SV_CELLS; j++ )
			for ( int i = 0; i < SV_CELLS; i++ )
			{
				int rgb[3];
				HsvToRgb( m_nHue * 360.0f / HUE_CELLS, m_bGreys ? 0.0f : i / (float)( SV_CELLS - 1 ), 1.0f - j / (float)( SV_CELLS - 1 ), rgb );
				Rect( x + i * cell, y + j * cell, cell, cell, { rgb[0], rgb[1], rgb[2], 255 } );
			}
		if ( dragging )
		{
			m_nSat = clamp( ( s_mx - x ) / cell, 0, SV_CELLS - 1 );
			m_nVal = clamp( ( s_my - y ) / cell, 0, SV_CELLS - 1 );
		}
		int mx0 = x + ( m_bGreys ? 0 : m_nSat ) * cell, my0 = y + m_nVal * cell;
		Outline( mx0 - 1, my0 - 1, cell + 2, cell + 2, C_BLACK );
		Outline( mx0, my0, cell, cell, C_WHITE );

		// hue: a column of cells, plus one grey cell at the bottom
		int hx = x + sv + S( 14 ), hw = S( 26 ), hc = sv / ( HUE_CELLS + 1 );
		for ( int i = 0; i <= HUE_CELLS; i++ )
		{
			int rgb[3];
			if ( i < HUE_CELLS )
				HsvToRgb( i * 360.0f / HUE_CELLS, 1, 1, rgb );
			else
				rgb[0] = rgb[1] = rgb[2] = 150;
			int cy = y + i * hc;
			Rect( hx, cy, hw, hc, { rgb[0], rgb[1], rgb[2], 255 } );
			bool sel = i < HUE_CELLS ? !m_bGreys && m_nHue == i : m_bGreys;
			if ( sel )
			{
				Outline( hx - 2, cy - 1, hw + 4, hc + 2, C_BLACK );
				Outline( hx - 1, cy, hw + 2, hc, C_WHITE );
			}
			if ( s_bDown && Hover( hx - S( 4 ), cy, hw + S( 8 ), hc ) )
			{
				m_bGreys = i == HUE_CELLS;
				if ( !m_bGreys )
					m_nHue = i;
			}
		}

		// live: the picked types change as you drag
		int rgb[3];
		PickedRGB( rgb );
		if ( rgb[0] != before[0] || rgb[1] != before[1] || rgb[2] != before[2] )
		{
			if ( m_bSkyColour )
				SetSkyColour( rgb[0], rgb[1], rgb[2] );
			else
				ApplyRGB( rgb[0], rgb[1], rgb[2] );
			g_TLCustom[0] = rgb[0]; g_TLCustom[1] = rgb[1]; g_TLCustom[2] = rgb[2];
			TL_LookChanged( 0 );
		}

		// the result and what it's on
		int rx = hx + hw + S( 26 ), rw = x + w - rx;
		Col c = { rgb[0], rgb[1], rgb[2], 255 };
		PixelBox( rx, y, rw, S( 110 ), MAX( 2, S( 3 ) ), c, C_WOOD_DARK );
		char hex[64];
		snprintf( hex, sizeof( hex ), "#%02X%02X%02X", rgb[0], rgb[1], rgb[2] );
		Text( F_BOLD, rx, y + S( 124 ), C_TEXT, hex );
		snprintf( hex, sizeof( hex ), "%d %d %d", rgb[0], rgb[1], rgb[2] );
		Text( F_SMALL, rx, y + S( 124 ) + TextH( F_BOLD ) + S( 4 ), C_SUB, hex );
		std::string on = m_bSkyColour ? "THE SKYBOX" : m_nPart == PART_FILL ? "FILL OF" : "BORDER OF";
		int ly = y + S( 124 ) + TextH( F_BOLD ) + TextH( F_SMALL ) + S( 18 );
		Text( F_SMALL, rx, ly, C_SUB, on.c_str() );
		for ( int t = 0; t < TL_TYPES && !m_bSkyColour; t++ )
			if ( ( m_nSel & ( 1 << t ) ) && ly + 2 * ( TextH( F_SMALL ) + S( 2 ) ) < y + sv - S( 42 ) )
			{
				ly += TextH( F_SMALL ) + S( 2 );
				Text( F_SMALL, rx, ly, C_TEXT, g_TLTypeLabel[t] );
			}
		// (as many custom triggers as fit above DONE)
		for ( size_t k = 0; k < m_SelCT.size() && !m_bSkyColour && ly + 2 * ( TextH( F_SMALL ) + S( 2 ) ) < y + sv - S( 42 ); k++ )
		{
			ly += TextH( F_SMALL ) + S( 2 );
			Text( F_SMALL, rx, ly, C_TEXT, CTName( m_SelCT[k] ).c_str() );
		}
		if ( Button( rx, y + sv - S( 42 ), rw, S( 42 ), "DONE" ) )
			m_nView = m_bSkyColour ? VIEW_SKY : VIEW_MAIN;
	}

	// ---- the skybox ------------------------------------------------------------------------------------
	static void SetSkyColour( int r, int g, int b )
	{
		g_TLSky.mode = TL_SKY_COLOUR;
		g_TLSky.rgb[0] = (unsigned char)r; g_TLSky.rgb[1] = (unsigned char)g; g_TLSky.rgb[2] = (unsigned char)b;
		TL_SkyChanged();
	}

	// "colour #RRGGBB", "spray cat", ...
	static std::string SkyText()
	{
		char t[96];
		const TLSky_t &k = g_TLSky;
		if ( k.mode == TL_SKY_COLOUR )
			snprintf( t, sizeof( t ), "colour #%02X%02X%02X, every map", k.rgb[0], k.rgb[1], k.rgb[2] );
		else if ( k.mode == TL_SKY_OFF )
			TL_StrCopy( t, "off: the map's own sky", sizeof( t ) );
		else
			snprintf( t, sizeof( t ), "%s %s, every map", k.mode == TL_SKY_SPRAY ? "spray" : "picture", ( k.mode == TL_SKY_SPRAY ? k.spray : k.picture ).c_str() );
		return t;
	}

	// a mode picked (main page or the SKYBOX page): it shows at once (the last colour / spray / picture, at first your
	// own spray / the first picture) and its page opens
	void OpenSky( int mode )
	{
		TLPics_Refresh();
		TLSky_t &k = g_TLSky;
		if ( mode == TL_SKY_SPRAY && !TLPics_Find( true, k.spray ) )
			k.spray = TLPics_Find( true, TL_PlayerSpray() ) ? TL_PlayerSpray() : TLPics_List( true ).empty() ? "" : TLPics_List( true )[0].name;
		if ( mode == TL_SKY_PICTURE && !TLPics_Find( false, k.picture ) )
			k.picture = TLPics_List( false ).empty() ? "" : TLPics_List( false )[0].name;
		if ( mode != TL_SKY_SPRAY && mode != TL_SKY_PICTURE ? mode != k.mode : !( mode == TL_SKY_SPRAY ? k.spray : k.picture ).empty() )
		{
			k.mode = mode;
			TL_SkyChanged();
		}
		if ( mode != TL_SKY_OFF )
			m_nSkyTab = mode;
		m_nSkyScroll = 0;
	}

	// OFF | COLOUR | SPRAY | PICTURE (the one shown is lit)
	void SkyModes( int x, int y, int w )
	{
		static const char *const s_Labels[] = { "OFF", "COLOUR", "SPRAY", "PICTURE" };
		const int gap = S( 10 ), bw = ( w - 3 * gap ) / 4;
		for ( int k = 0; k < 4; k++ )
			if ( Button( x + k * ( bw + gap ), y, bw, S( 42 ), s_Labels[k], g_TLSky.mode == k ) )
			{
				OpenSky( k );
				if ( k != TL_SKY_OFF )
					m_nView = VIEW_SKY;
				s_bClick = false;
			}
	}

	void SkyView( int x, int y, int w, int h )
	{
		const int gap = S( 10 ), label = S( 22 ), bottom = y + h - S( 22 );
		SkyModes( x, y, w );
		int cy = y + S( 42 + 14 );
		std::string line;
		if ( m_nSkyTab == TL_SKY_COLOUR )
		{
			// the paint menu's colour blocks, your custom colour and the picker: the sky changes at once
			const int thumb = S( 70 ), perRow = 7;
			Text( F_BOLD, x, cy, C_WOOD, "COLOUR" );
			cy += label;
			for ( int i = 0; i < 14; i++ )
			{
				const Preset_t &pr = s_Presets[i];
				const int tx = x + ( i % perRow ) * ( thumb + gap ), ty = cy + ( i / perRow ) * ( thumb + gap );
				const bool sel = g_TLSky.mode == TL_SKY_COLOUR && g_TLSky.rgb[0] == pr.r && g_TLSky.rgb[1] == pr.g && g_TLSky.rgb[2] == pr.b;
				if ( Swatch( tx, ty, thumb, { pr.r, pr.g, pr.b, 255 }, sel ) )
					SetSkyColour( pr.r, pr.g, pr.b );
			}
			cy += 2 * ( thumb + gap );
			Text( F_BOLD, x, cy, C_WOOD, "CUSTOM" );
			cy += label;
			const int *c = g_TLCustom;
			if ( Swatch( x, cy, thumb, { c[0], c[1], c[2], 255 }, g_TLSky.mode == TL_SKY_COLOUR && g_TLSky.rgb[0] == c[0] && g_TLSky.rgb[1] == c[1] && g_TLSky.rgb[2] == c[2] ) )
				SetSkyColour( c[0], c[1], c[2] );
			if ( Button( x + thumb + gap, cy + ( thumb - S( 42 ) ) / 2, w - thumb - gap, S( 42 ), "CUSTOM COLOUR" ) )
			{
				m_bSkyColour = true;
				StartCustom();
				m_nView = VIEW_CUSTOM;
				s_bClick = false;
			}
			line = SkyText();
		}
		else
		{
			// SPRAY: this PC's sprays (yours marked); PICTURE: the pictures added + ADD A FILE
			const bool spray = m_nSkyTab == TL_SKY_SPRAY;
			if ( !spray )
			{
				const TLPicStatus_t st = TLPics_Status();
				if ( Button( x, cy, S( 190 ), S( 36 ), st.state == 1 ? "ADDING..." : "+ ADD A FILE", false, st.state != 1 ) )
				{
					OpenFiles();
					m_nView = VIEW_FILES;
					s_bClick = false;
					return;
				}
				if ( st.state )
					Text( F_SMALL, x + S( 204 ), cy + ( S( 36 ) - TextH( F_SMALL ) ) / 2, st.state == 3 ? Col{ 230, 90, 80, 255 } : st.state == 2 ? C_WOOD_LIGHT : C_SUB,
						  FitRight( F_SMALL, st.text, w - S( 204 ) ).c_str() );
				cy += S( 36 + 14 );
			}
			const std::vector<TLPic_t> &list = TLPics_List( spray );
			const int thumb = S( 70 ), perRow = MAX( 1, ( w + gap ) / ( thumb + gap ) ), rows = ( (int)list.size() + perRow - 1 ) / perRow;
			const int shown = MAX( 1, ( bottom - cy - label ) / ( thumb + gap ) );
			Text( F_BOLD, x, cy, C_WOOD, spray ? "SPRAYS ON THIS PC" : "PICTURES" );
			if ( rows > shown )
				Text( F_SMALL, x + w - TextW( F_SMALL, "wheel = more" ), cy + ( TextH( F_BOLD ) - TextH( F_SMALL ) ) / 2, C_SUB, "wheel = more" );
			cy += label;
			if ( Hover( x, cy, w, bottom - cy ) && s_nWheel )
				m_nSkyScroll -= s_nWheel;
			m_nSkyScroll = clamp( m_nSkyScroll, 0, MAX( 0, rows - shown ) );
			if ( list.empty() )
				Text( F_SMALL, x, cy, C_SUB, spray ? "no sprays in materials/vgui/logos" : "none yet: ADD A FILE turns a JPEG, PNG, GIF, WEBP or VTF into one" );
			const std::string &picked = spray ? g_TLSky.spray : g_TLSky.picture, mine = TL_PlayerSpray();
			const TLPic_t *hover = NULL;
			for ( int i = m_nSkyScroll * perRow; i < (int)list.size() && i < ( m_nSkyScroll + shown ) * perRow; i++ )
			{
				const TLPic_t &p = list[i];
				const int tx = x + ( i % perRow ) * ( thumb + gap ), ty = cy + ( i / perRow - m_nSkyScroll ) * ( thumb + gap );
				Thumb( p, tx, ty, thumb );
				if ( spray && !TL_StrICmp( p.name.c_str(), mine.c_str() ) )
				{
					Rect( tx, ty, TextW( F_SMALL, "YOURS" ) + S( 8 ), TextH( F_SMALL ) + S( 2 ), C_WOOD );
					Text( F_SMALL, tx + S( 4 ), ty + S( 1 ), C_BLACK, "YOURS" );
				}
				const bool sel = g_TLSky.mode == m_nSkyTab && !TL_StrICmp( picked.c_str(), p.name.c_str() );
				if ( Hover( tx, ty, thumb, thumb ) )
				{
					hover = &p;
					if ( s_bClick )
					{
						g_TLSky.mode = m_nSkyTab;
						( spray ? g_TLSky.spray : g_TLSky.picture ) = p.name;
						TL_SkyChanged();
					}
				}
				if ( sel || hover == &p )
					Ring( tx, ty, thumb, thumb, sel ? C_WHITE : C_WOOD );
			}
			const TLPic_t *about = hover ? hover : TLPics_Find( spray, picked );
			if ( about )
			{
				char t[160];
				const float a = about->aspect;
				snprintf( t, sizeof( t ), "%s%s  \xC2\xB7  %dx%d%s%s%s", hover ? "" : "picked: ", about->name.c_str(), about->width, about->height,
						  about->frames > 1 ? "  \xC2\xB7  animated" : "", !spray && a >= 1.9f && a <= 2.1f ? "  \xC2\xB7  2:1, wraps all the way round" : "",
						  spray && !TL_StrICmp( about->name.c_str(), mine.c_str() ) ? "  \xC2\xB7  your spray" : "" );
				line = t;
			}
		}
		Text( F_SMALL, x, bottom + ( S( 22 ) - TextH( F_SMALL ) ) / 2, C_SUB, FitRight( F_SMALL, line, w ).c_str() );
	}

	// ---- ADD A FILE: the in-game file list (p2paint's) ----------------------------------------------------
	void OpenFiles()
	{
		std::string dir = LastDir();
		if ( dir.empty() || !TLP_IsDir( dir ) )
			dir = TLP_IsDir( TLP_Home() + "/Pictures" ) ? TLP_Home() + "/Pictures" : TLP_Home();
		GoTo( dir );
	}

	void GoTo( const std::string &dir )
	{
		std::vector<TLDirEntry_t> entries;
		if ( !TLPics_ListDir( dir, entries ) )
			return;		// can't open: stay where we are
		m_Dir = dir;
		m_Entries.swap( entries );
		m_nFileScroll = 0;
		if ( FILE *f = TLP_Open( LastDirFile().c_str(), "w" ) )
		{
			fprintf( f, "%s\n", m_Dir.c_str() );
			fclose( f );
		}
	}

	void FilesView( int x, int y, int w, int h )
	{
		Text( F_SMALL, x, y, C_SUB, FitLeft( F_SMALL, m_Dir.empty() ? "drives" : m_Dir, w ).c_str() );
		// the folder above (from a drive's root, Windows: the drives "") and the usual places
		const std::string home = TLP_Home(), root = TLP_Root( m_Dir );
		const size_t cut = m_Dir.find_last_of( '/' );
		const std::string up = m_Dir.size() <= root.size() ? ( root == "/" ? root : "" ) : cut == std::string::npos || cut < root.size() ? root : m_Dir.substr( 0, cut );
		const struct { const char *label; std::string path; } places[] = { { "UP", up }, { "HOME", home }, { "PICTURES", home + "/Pictures" },
			{ "DOWNLOADS", home + "/Downloads" }, { "DESKTOP", home + "/Desktop" } };
		const int qy = y + S( 22 ), qh = S( 30 ), n = 5, qw = ( w - S( 8 ) * ( n - 1 ) ) / n;
		for ( int i = 0; i < n; i++ )
			if ( Button( x + i * ( qw + S( 8 ) ), qy, qw, qh, places[i].label, false, ( !i && up != m_Dir ) || TLP_IsDir( places[i].path ) ) )
				GoTo( places[i].path );
		// the list: folders, then the pictures; a click opens a folder or adds the file
		const int ly = qy + qh + S( 12 ), lh = h - ( ly - y ), rowH = S( 30 ), visible = MAX( 1, lh / rowH ), rows = (int)m_Entries.size();
		if ( Hover( x, ly, w, lh ) && s_nWheel )
			m_nFileScroll -= s_nWheel * 3;
		m_nFileScroll = clamp( m_nFileScroll, 0, MAX( 0, rows - visible ) );
		if ( m_Entries.empty() )
			Text( F_BODY, x, ly + S( 6 ), C_SUB, "no JPEG, PNG, GIF, WEBP, VTF or folders here" );
		std::string open;
		for ( int r = 0; r < visible && m_nFileScroll + r < rows; r++ )
		{
			const TLDirEntry_t &e = m_Entries[m_nFileScroll + r];
			const int ry = ly + r * rowH, ip = MAX( 1, S( 3 ) ), ix = x + S( 10 ), iy = ry + ( rowH - S( 2 ) - 5 * ip ) / 2;
			const bool hover = Hover( x, ry, w - S( 14 ), rowH - S( 2 ) );
			Rect( x, ry, w - S( 14 ), rowH - S( 2 ), hover ? C_BTN_HOVER : C_BTN );
			if ( e.dir )	// a little pixel folder / picture
			{
				Rect( ix, iy, 3 * ip, ip, C_WOOD );
				Rect( ix, iy + ip, 6 * ip, 4 * ip, C_WOOD );
			}
			else
			{
				Rect( ix, iy, 6 * ip, 5 * ip, C_TEXT );
				Rect( ix + ip, iy + ip, 4 * ip, 3 * ip, { 90, 160, 230, 255 } );
				Rect( ix + ip, iy + 3 * ip, 4 * ip, ip, { 80, 170, 80, 255 } );
			}
			Text( F_BODY, x + S( 40 ), ry + ( rowH - S( 2 ) - TextH( F_BODY ) ) / 2, e.dir ? C_WOOD_LIGHT : C_TEXT,
				  FitRight( F_BODY, e.dir ? e.name + "/" : e.name, w - S( 64 ) ).c_str() );
			if ( hover && s_bClick )
				open = e.name;
		}
		if ( rows > visible )
		{
			const int bh = MAX( S( 30 ), lh * visible / rows );
			Rect( x + w - S( 6 ), ly, S( 6 ), lh, C_BTN );
			Rect( x + w - S( 6 ), ly + ( lh - bh ) * m_nFileScroll / MAX( 1, rows - visible ), S( 6 ), bh, C_WOOD_DARK );
		}
		if ( !open.empty() )
		{
			const std::string path = m_Dir.empty() ? open + "/" : m_Dir.back() == '/' ? m_Dir + open : m_Dir + "/" + open;
			if ( TLP_IsDir( path ) )
				GoTo( path );
			else if ( TLPics_Add( path ) )
			{
				m_nSkyTab = TL_SKY_PICTURE;
				m_nView = VIEW_SKY;
			}
			s_bClick = false;
		}
	}
};

static CTLMenu s_Menu;

//-----------------------------------------------------------------------------
// SELECT TRIGGER: the menu closes, you look around, and the +attack key (left click) picks the drawn trigger under
// the crosshair (lit up white, tl_runtime.cpp): it becomes a custom trigger and the menu opens again with it picked.
// The key is borrowed while picking (bound to +triggerlook_pick, so it doesn't shoot) and given back after. Its own
// bind is saved in the settings file at once: the game closing mid-pick (Alt+F4) gets it back on the next map (a key
// still bound to +triggerlook_pick without a saved bind gets +attack). Esc = back to the menu.
//-----------------------------------------------------------------------------
static std::string s_PickKey, s_PickKeyOld;	// the borrowed key ("MOUSE1") and its bind
static bool s_bReopenMenu;					// Esc: back to the menu once the pause menu is gone
static int s_nPickDown = -1;				// the trigger under the crosshair when the pick key went down
static int s_nPicked = -1;					// ... when it came up: picked
static char s_szBindsChecked[MAX_PATH];		// the map the left-over bind was checked on

static void SetBind( const char *key, const std::string &cmd )
{
	char line[600];
	if ( cmd.empty() )
		snprintf( line, sizeof( line ), "unbind \"%s\"\n", key );
	else
	{
		std::string c = cmd;
		std::replace( c.begin(), c.end(), '"', '\'' );
		snprintf( line, sizeof( line ), "bind \"%s\" \"%s\"\n", key, c.c_str() );
	}
	s_pEngine->ClientCmd_Unrestricted( line );
}

static void StartPick()
{
	if ( s_bPicking || !s_pEngine )
		return;
	// MOUSE1 when it attacks, else whichever key does; no attack key at all: MOUSE1 anyway
	const char *mouse1 = s_pEngine->Key_BindingForKey( MOUSE_LEFT );
	const char *key = mouse1 && !TL_StrICmp( mouse1, "+attack" ) ? "MOUSE1" : s_pEngine->Key_LookupBinding( "+attack" );
	s_PickKey = key && *key ? key : "MOUSE1";
	s_PickKeyOld = key && *key ? "+attack" : mouse1 ? mouse1 : "";
	if ( s_PickKeyOld == "+triggerlook_pick" )
		s_PickKeyOld = "+attack";
	g_TLPickKey = s_PickKey;
	g_TLPickBind = s_PickKeyOld;
	TL_LookSave();
	SetBind( s_PickKey.c_str(), "+triggerlook_pick" );
	s_nPickDown = s_nPicked = -1;
	SetMenuOpen( false );
	s_bPicking = true;
	TL_SetPicking( true );
}

// model >= 0: that trigger was picked (its custom trigger is picked in the menu); reopen: the menu again
static void EndPick( bool reopen, int model )
{
	if ( !s_bPicking )
		return;
	s_bPicking = false;
	TL_SetPicking( false );
	SetBind( s_PickKey.c_str(), s_PickKeyOld );
	if ( s_pEngine->IsInGame() )
	{
		// (leaving the game: that bind command can run after the game wrote its config - the next map checks it)
		g_TLPickKey.clear();
		g_TLPickBind.clear();
		TL_LookSave();
	}
	const int ct = model >= 0 ? TL_PickedCT( model ) : -1;
	if ( !reopen )
		return;
	SetMenuOpen( true );
	if ( ct >= 0 )
		s_Menu.PickCT( ct );
}

// The trigger under the crosshair when the key goes down is picked when it comes up (the menu opens with the button
// up, on the next tick: not in the middle of the game's key handling).
static void CmdPickDown( const CCommand &args )
{
	if ( !s_bPicking )
	{
		if ( s_pEngine )
			s_pEngine->ClientCmd_Unrestricted( "+attack\n" );	// a bind left over: still shoots
		return;
	}
	s_nPickDown = TL_PickHover();
}
static void CmdPickUp( const CCommand &args )
{
	if ( !s_bPicking )
	{
		if ( s_pEngine )
			s_pEngine->ClientCmd_Unrestricted( "-attack\n" );
		return;
	}
	s_nPicked = s_nPickDown;		// (TLUI_Tick ends the pick: not in the middle of the game's key handling)
	s_nPickDown = -1;
}
static ConCommand s_cmdPickDown( "+triggerlook_pick", CmdPickDown, "triggerlook: SELECT TRIGGER picks the trigger under the crosshair (bound while picking)" );
static ConCommand s_cmdPickUp( "-triggerlook_pick", CmdPickUp, "triggerlook: (the key went up)" );

// every frame (TL_Tick): a picture ADD A FILE made (it becomes the sky); leaving / Esc / triggerlook off while
// picking; a left-over bind
void TLUI_Tick()
{
	if ( !s_pEngine || !TLUI_Ready() )
		return;
	static int s_nAddSerial;
	const TLPicStatus_t st = TLPics_Status();
	if ( st.serial != s_nAddSerial )
	{
		s_nAddSerial = st.serial;
		if ( st.state == 2 )
		{
			TLPics_Refresh();
			g_TLSky.mode = TL_SKY_PICTURE;
			g_TLSky.picture = st.added;
			TL_SkyChanged();
		}
		if ( st.state >= 2 )
			Msg( "triggerlook: %s\n", st.text.c_str() );
	}
	if ( s_bPicking )
	{
		if ( s_nPicked >= 0 )
		{
			const int model = s_nPicked;
			s_nPicked = -1;
			EndPick( true, model );
		}
		else if ( !s_pEngine->IsInGame() )
			EndPick( false, -1 );
		else if ( s_pEngineVGui->IsGameUIVisible() )
		{
			// Esc: the engine opens its pause menu first; hide it, the menu opens once it's gone
			s_pEngine->ClientCmd_Unrestricted( "gameui_hide\n" );
			EndPick( false, -1 );
			s_bReopenMenu = true;
		}
		else if ( !TL_Active() )
			EndPick( true, -1 );
	}
	else if ( s_bReopenMenu )
	{
		if ( !s_pEngine->IsInGame() )
			s_bReopenMenu = false;
		else if ( !s_pEngineVGui->IsGameUIVisible() )
		{
			s_bReopenMenu = false;
			SetMenuOpen( true );
		}
	}
	else if ( s_pEngine->IsInGame() && strcmp( s_szBindsChecked, s_pEngine->GetLevelName() ) )
	{
		TL_StrCopy( s_szBindsChecked, s_pEngine->GetLevelName(), sizeof( s_szBindsChecked ) );
		if ( !g_TLPickKey.empty() )
		{
			const char *still = s_pEngine->Key_LookupBinding( "+triggerlook_pick" );
			if ( still && !TL_StrICmp( still, g_TLPickKey.c_str() ) )
			{
				SetBind( g_TLPickKey.c_str(), g_TLPickBind );
				Msg( "triggerlook: gave %s its bind back (the game closed while picking a trigger)\n", g_TLPickKey.c_str() );
			}
			g_TLPickKey.clear();
			g_TLPickBind.clear();
			TL_LookSave();
		}
		else if ( const char *key = s_pEngine->Key_LookupBinding( "+triggerlook_pick" ) )
		{
			SetBind( key, "+attack" );
			Msg( "triggerlook: gave %s back to +attack (the game closed while picking a trigger)\n", key );
		}
	}
}

// "MOUSE1" -> "Left click"
static std::string KeyLabel( const std::string &key )
{
	if ( !TL_StrICmp( key.c_str(), "MOUSE1" ) ) return "Left click";
	if ( !TL_StrICmp( key.c_str(), "MOUSE2" ) ) return "Right click";
	if ( !TL_StrICmp( key.c_str(), "MOUSE3" ) ) return "Middle click";
	std::string s = key;
	for ( char &c : s )
		c = (char)toupper( (unsigned char)c );
	return s;
}

// the bar at the bottom while picking (the paint keys' hint bar)
static void PaintPickBar()
{
	if ( !s_pEngine->IsInGame() || s_pEngineVGui->IsGameUIVisible() )
		return;
	int sw, sh;
	s_pSurface->GetScreenSize( sw, sh );
	s_flScale = sh / 1080.0f;
	if ( s_nFontsForHeight != sh || !s_Font[F_BODY] || !s_pSurface->GetFontTall( s_Font[F_BODY] ) )
		SetupFonts( sh );
	const int model = TL_PickHover(), type = TL_ModelType( model ), ct = TL_ModelCT( model );
	int inMap = 0;
	for ( int t = 0; t < TL_TYPES; t++ )
		inMap += TL_InMap( t );
	const bool shown = g_bTLShow && inMap;
	char head[160], keys[200];
	if ( ct >= 0 )
		snprintf( head, sizeof( head ), "SELECT TRIGGER  \xC2\xB7  %s (%s)", CTName( ct ).c_str(), g_TLTypeLabel[type] );
	else if ( type >= 0 )
		snprintf( head, sizeof( head ), "SELECT TRIGGER  \xC2\xB7  %s", g_TLTypeLabel[type] );
	else
		TL_StrCopy( head, "SELECT TRIGGER", sizeof( head ) );
	if ( shown )
		snprintf( keys, sizeof( keys ), "%s a trigger to pick it  \xC2\xB7  Esc: back to the menu", KeyLabel( s_PickKey ).c_str() );
	else
		TL_StrCopy( keys, g_bTLShow ? "this map has no triggers  \xC2\xB7  Esc: back to the menu" : "triggers are OFF: turn them ON in the menu  \xC2\xB7  Esc: back to the menu", sizeof( keys ) );
	int bw = MIN( sw - S( 40 ), MAX( TextW( F_BOLD, head ), TextW( F_SMALL, keys ) ) + S( 56 ) );
	int bh = S( 16 ) + TextH( F_BOLD ) + S( 6 ) + TextH( F_SMALL ) + S( 14 );
	int bx = ( sw - bw ) / 2, by = sh - bh - S( 140 );
	PixelBox( bx, by, bw, bh, MAX( 2, S( 3 ) ), { 18, 17, 20, 225 }, C_WOOD_DARK );
	Rect( bx + S( 12 ), by + S( 14 ), S( 8 ), TextH( F_BOLD ), type >= 0 ? C_WHITE : C_WOOD );
	Text( F_BOLD, bx + S( 28 ), by + S( 14 ), C_TEXT, head );
	Text( F_SMALL, bx + S( 28 ), by + S( 14 ) + TextH( F_BOLD ) + S( 6 ), C_SUB, keys );
}

static void SetMenuOpen( bool open )
{
	if ( !s_pSurface || !s_Menu.GetVPanel() )
		return;
	if ( open )
	{
		EndPick( false, -1 );		// (triggerlook_menu while picking: back to the menu)
		s_bReopenMenu = false;
		if ( s_pEngineVGui && s_pEngineVGui->IsGameUIVisible() )
		{
			// typed in the console: the menu would see the pause UI on its first frame and close again
			if ( s_pEngine )
				s_pEngine->ClientCmd_Unrestricted( "hideconsole\ngameui_hide\n" );
			s_bReopenMenu = true;
			return;
		}
	}
	if ( open && s_pEngine && s_pEngine->Con_IsVisible() )
		s_pEngine->ClientCmd_Unrestricted( "hideconsole" );
	s_bDown = true;		// a click that opened it doesn't count
	if ( open && !s_Menu.IsOpen() )
		s_Menu.Opened();
	if ( !open )
	{
		s_Menu.EndRename( true );
		TL_LookFlush();		// (a quit right after closing keeps the change)
		TL_PrebuildMeshes();
	}
	s_Menu.Show( open );
	if ( open )
		TL_ReadMap();	// before its first frame paints
}

void TLUI_SetOpen( bool open ) { SetMenuOpen( open ); }
bool TLUI_IsOpen() { return s_Menu.IsOpen(); }
bool TLUI_Ready() { return s_Menu.GetVPanel() != 0; }

bool TLUI_Init( IVEngineClient *pEngine )
{
	if ( TLUI_Ready() )
		return true;
	s_pEngine = pEngine;
	if ( !s_pSurface )		s_pSurface = (ISurface *)TLP_Interface( "vguimatsurface", VGUI_SURFACE_INTERFACE_VERSION );
	if ( !s_pPanel )		s_pPanel = (IPanel *)TLP_Interface( "vgui2", VGUI_PANEL_INTERFACE_VERSION );
	if ( !s_pVGui )			s_pVGui = (IVGui *)TLP_Interface( "vgui2", VGUI_IVGUI_INTERFACE_VERSION );
	if ( !s_pInput )		s_pInput = (IInput *)TLP_Interface( "vgui2", VGUI_INPUT_INTERFACE_VERSION );
	if ( !s_pEngineVGui )	s_pEngineVGui = (IEngineVGui *)TLP_Interface( "engine", VENGINE_VGUI_VERSION );
	if ( !s_pInputSystem )	s_pInputSystem = (IInputSystem *)TLP_Interface( "inputsystem", INPUTSYSTEM_INTERFACE_VERSION );
	if ( !s_pSurface || !s_pPanel || !s_pVGui || !s_pInput || !s_pEngineVGui )
		return false;
	if ( !s_pEngineVGui->GetPanel( PANEL_CLIENTDLL ) )
		return false;	// VGUI not up yet
	if ( !s_Ticker.Create( false ) || !s_Menu.Create( true ) )
		return false;
	s_Ticker.Show( true );
	return true;
}

//-----------------------------------------------------------------------------
// Test helpers: the menu's state without clicking, and a timed console script (cfg "wait" doesn't work at
// uncapped fps): lines "<seconds after the previous line> <command>", "#" comments
//-----------------------------------------------------------------------------
CON_COMMAND( triggerlook_ui, "triggerlook_ui select <types,ct1,...> | part fill|border | view main|custom|sky|files | pickmode | delete | mark <text>: set the menu's state, or mark the info file (testing)" )
{
	if ( args.ArgC() == 2 && !TL_StrICmp( args[1], "pickmode" ) )
	{
		StartPick();		// = SELECT TRIGGER (then +triggerlook_pick = the click)
		return;
	}
	if ( args.ArgC() == 2 && !TL_StrICmp( args[1], "delete" ) )
	{
		s_Menu.DeleteSelected();
		return;
	}
	if ( args.ArgC() < 3 )
	{
		Msg( "usage: triggerlook_ui select <type|ctN[,...]|all> | part fill|border | view main|custom|sky|files | pickmode | delete\n" );
		return;
	}
	if ( !TL_StrICmp( args[1], "select" ) )
	{
		int mask = 0;
		std::vector<int> cts;
		const std::vector<int> &mapCTs = TL_MapCTs();
		char list[256];
		TL_StrCopy( list, args.ArgS() + strlen( args[1] ) + 1, sizeof( list ) );
		for ( char *p = strtok( list, ", " ); p; p = strtok( NULL, ", " ) )
		{
			int n = !TL_StrNICmp( p, "ct", 2 ) ? atoi( p + 2 + ( p[2] == '-' ) ) : 0;
			if ( n > 0 && n <= (int)mapCTs.size() )
				cts.push_back( mapCTs[n - 1] );
			else
				mask |= TL_TypeMask( p );
		}
		s_Menu.SetSelection( mask, cts );
	}
	else if ( !TL_StrICmp( args[1], "part" ) )
		s_Menu.SetPart( !TL_StrICmp( args[2], "border" ) ? PART_BORDER : PART_FILL );
	else if ( !TL_StrICmp( args[1], "view" ) )
		s_Menu.SetView( !TL_StrICmp( args[2], "custom" ) ? VIEW_CUSTOM : !TL_StrICmp( args[2], "sky" ) ? VIEW_SKY : !TL_StrICmp( args[2], "files" ) ? VIEW_FILES : VIEW_MAIN );
	else if ( !TL_StrICmp( args[1], "mark" ) )
	{
		// a line in the info file, for a test helper outside the game to wait for
		char path[1024];
		snprintf( path, sizeof( path ), "%s/info.txt", TLP_CacheDir() );
		if ( FILE *f = TLP_Open( path, "a" ) )
		{
			fprintf( f, "mark %s\n", args[2] );
			fclose( f );
		}
	}
}

struct ScriptLine_t { float delay; std::string cmd; };
static std::vector<ScriptLine_t> s_Script;
static size_t s_nScriptPos;
static double s_flScriptNext;

void TL_RunScript()	// from TL_Tick
{
	if ( s_nScriptPos >= s_Script.size() || !s_pEngine )
		return;
	double now = Plat_FloatTime();
	while ( s_nScriptPos < s_Script.size() && now >= s_flScriptNext )
	{
		const ScriptLine_t &l = s_Script[s_nScriptPos];
		// "!ingame": wait until a map is up and (triggers ON) its triggers read
		if ( l.cmd == "!ingame" && !( g_bTLShow ? TL_Active() : s_pEngine->IsInGame() ) )
			return;
		s_nScriptPos++;
		Msg( "triggerlook_selftest: %s\n", l.cmd.c_str() );
		if ( l.cmd[0] != '!' )
			s_pEngine->ClientCmd_Unrestricted( ( l.cmd + "\n" ).c_str() );
		if ( s_nScriptPos < s_Script.size() )
			s_flScriptNext = now + s_Script[s_nScriptPos].delay;
	}
}

CON_COMMAND( triggerlook_selftest, "triggerlook_selftest <file>: run timed console commands from a file (testing; see tl_ui.cpp)" )
{
	if ( args.ArgC() < 2 )
		return;
	TL_Start();		// (from the command line: before the first setting change)
	FILE *f = fopen( args[1], "r" );
	if ( !f )
	{
		Warning( "triggerlook_selftest: can't open %s\n", args[1] );
		return;
	}
	s_Script.clear();
	char line[1024];
	while ( fgets( line, sizeof( line ), f ) )
	{
		char *p = line;
		while ( *p == ' ' || *p == '\t' ) p++;
		if ( !*p || *p == '#' || *p == '\n' )
			continue;
		char *end;
		float delay = strtof( p, &end );
		if ( end == p )
			continue;
		while ( *end == ' ' || *end == '\t' ) end++;
		std::string cmd = end;
		while ( !cmd.empty() && ( cmd.back() == '\n' || cmd.back() == '\r' ) )
			cmd.pop_back();
		s_Script.push_back( { delay, cmd } );
	}
	fclose( f );
	s_nScriptPos = 0;
	s_flScriptNext = Plat_FloatTime() + ( s_Script.empty() ? 0 : s_Script[0].delay );
	Msg( "triggerlook_selftest: %d commands from %s\n", (int)s_Script.size(), args[1] );
}
