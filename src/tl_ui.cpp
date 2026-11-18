//=============================================================================
// triggerlook: the trigger menu (triggerlook_menu). Built like the author's paint menu (same panels,
// colours, fonts, colour blocks and custom colour picker) so the two look the same.
//
//   SHOW     one button per trigger type: white = drawn, grey = hidden
//   EDIT     the same types, each in its own colours (fill inside, border round it); the colour settings below
//            change the picked ones (click = only that one, shift + click = add / remove)
//   FILL / BORDER  which part the colours and OPACITY change; WIDTH = the border's width (OFF = none)
//
// Changes show at once on the triggers in front of you (the menu sits at the left and doesn't darken the game).
// Plain VGUI panels of the client, input polled (VGUI messages are KeyValues in the game's newer layout). Esc closes.
//=============================================================================
#include <limits.h>
#include <math.h>
#include <stdlib.h>

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

// a button: pixel box, lighter when hovered, wood when "on". Returns true when clicked.
static bool Button( int x, int y, int w, int h, const char *label, bool on = false )
{
	bool hover = Hover( x, y, w, h );
	int p = MAX( 2, S( 3 ) );
	PixelBox( x, y, w, h, p, on ? C_WOOD : hover ? C_BTN_HOVER : C_BTN, on ? C_WOOD_LIGHT : hover ? C_WOOD : C_WOOD_DARK );
	Text( F_BOLD, x + ( w - TextW( F_BOLD, label ) ) / 2, y + ( h - TextH( F_BOLD ) ) / 2, on ? C_BLACK : C_TEXT, label );
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
// The ticker: an always-shown panel that draws nothing; its OnTick runs the runtime every frame
//-----------------------------------------------------------------------------
class CTLTicker : public CTLBasePanel
{
public:
	CTLTicker() : CTLBasePanel( "TriggerLookTicker" ) {}
	virtual void Paint() {}
	virtual void OnTick() { TL_Tick(); }
};
static CTLTicker s_Ticker;

//-----------------------------------------------------------------------------
// The menu
//-----------------------------------------------------------------------------
static void SetMenuOpen( bool open );

enum { VIEW_MAIN, VIEW_CUSTOM };
enum { PART_FILL, PART_BORDER };

static const int PAGE_W = 576;		// the content width at 1080p (the paint menu's)

class CTLMenu : public CTLBasePanel
{
public:
	CTLMenu() : CTLBasePanel( "TriggerLookMenu" ) {}

	void Opened()
	{
		m_nView = VIEW_MAIN;
		s_nDragSlider = -1;
		ReadWheel( true );
	}

	// tests / scripts (triggerlook_ui_*)
	void SetSelection( int mask ) { if ( mask ) m_nSel = mask; }
	void SetPart( int part ) { m_nPart = part; }
	void SetView( int view ) { if ( view == VIEW_CUSTOM ) StartCustom(); m_nView = view; }

	// Esc: the engine opens its pause menu on Esc before VGUI sees the key (same as the paint menu)
	virtual void OnTick()
	{
		if ( !IsOpen() )
			return;
		if ( s_pEngineVGui->IsGameUIVisible() )
		{
			s_pEngine->ClientCmd_Unrestricted( "gameui_hide" );
			SetMenuOpen( false );
		}
		else if ( s_pInput->WasKeyPressed( KEY_ESCAPE ) )
			SetMenuOpen( false );
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
			SetMenuOpen( false );
			return;
		}
		int sw, sh;
		s_pSurface->GetScreenSize( sw, sh );
		s_flScale = sh / 1080.0f;
		if ( s_nFontsForHeight != sh || !s_Font[F_BODY] || !s_pSurface->GetFontTall( s_Font[F_BODY] ) )
			SetupFonts( sh );
		s_pInput->GetCursorPos( s_mx, s_my );
		bool down = s_pInput->IsMouseDown( MOUSE_LEFT );
		s_bClick = down && !s_bDown;
		s_bDown = down;
		if ( !down )
			s_nDragSlider = -1;
		ReadWheel( false );

		// no dimming: the triggers behind are what you're looking at. At the left so the middle stays clear.
		const int pad = S( 22 );
		const int W = S( PAGE_W ) + 2 * pad, H = S( 54 ) + ContentH() + pad;
		int x0 = S( 48 ), y0 = ( sh - H ) / 2;
		PixelBox( x0, y0, W, H, MAX( 2, S( 4 ) ), C_BG, C_WOOD_DARK );
		Text( F_TITLE, x0 + pad, y0 + S( 14 ), C_WOOD_LIGHT, m_nView == VIEW_CUSTOM ? "CUSTOM COLOUR" : "TRIGGERS" );
		if ( Button( x0 + W - pad - S( 32 ), y0 + S( 12 ), S( 32 ), S( 28 ), "X" ) )
		{
			SetMenuOpen( false );
			return;
		}
		if ( m_nView != VIEW_MAIN && Button( x0 + W - pad - S( 32 ) - S( 8 ) - S( 84 ), y0 + S( 12 ), S( 84 ), S( 28 ), "BACK" ) )
		{
			m_nView = VIEW_MAIN;
			s_bClick = false;
		}
		int cx = x0 + pad, cy = y0 + S( 54 ), cw = W - 2 * pad;
		if ( m_nView == VIEW_CUSTOM )
			CustomView( cx, cy, cw );
		else
			MainView( cx, cy, cw );
	}

private:
	int m_nView = VIEW_MAIN;
	int m_nSel = 1 << TL_TELEPORT;		// the types EDIT picked
	int m_nPart = PART_FILL;
	int m_nHover = -1;					// type under the cursor (for the line at the bottom)
	// custom colour picker
	enum { SV_CELLS = 24, HUE_CELLS = 24 };
	int m_nHue = 0, m_nSat = 0, m_nVal = 0;
	bool m_bGreys = false;

	// the same height on both pages
	int ContentH() const
	{
		return S( 22 + 42 + 14 ) * 2 + S( 42 + 14 ) + S( 22 + 2 * 80 ) + S( 22 + 70 + 14 ) + S( 22 + 28 + 14 ) + S( 22 );
	}

	int First() const { return __builtin_ctz( m_nSel ); }
	unsigned char *Part( int t ) { return m_nPart == PART_FILL ? g_TLLook[t].fill : g_TLLook[t].border; }

	// do all picked types have this colour in the current part?
	bool AllHave( int r, int g, int b )
	{
		for ( int t = 0; t < TL_TYPES; t++ )
			if ( ( m_nSel & ( 1 << t ) ) && ( Part( t )[0] != r || Part( t )[1] != g || Part( t )[2] != b ) )
				return false;
		return true;
	}

	// a colour for the current part of every picked type; a part that was invisible gets seen (a border gets a width)
	void ApplyRGB( int r, int g, int b )
	{
		for ( int t = 0; t < TL_TYPES; t++ )
		{
			if ( !( m_nSel & ( 1 << t ) ) )
				continue;
			unsigned char *c = Part( t );
			c[0] = (unsigned char)r; c[1] = (unsigned char)g; c[2] = (unsigned char)b;
			if ( !c[3] )
				c[3] = m_nPart == PART_FILL ? 115 : 255;
			if ( m_nPart == PART_BORDER && !g_TLLook[t].width )
				g_TLLook[t].width = 2;
		}
		TL_LookChanged( m_nSel );
	}

	// ---- the main page ----------------------------------------------------------------------------------
	void MainView( int x, int y, int w )
	{
		const int gap = S( 10 ), label = S( 22 ), bh = S( 42 ), tbw = ( w - 4 * gap ) / 5;
		int cy = y;
		m_nHover = -1;

		// SHOW: white = drawn, grey = hidden
		Text( F_BOLD, x, cy, C_WOOD, "SHOW" );
		cy += label;
		for ( int t = 0; t < TL_TYPES; t++ )
		{
			int bx = x + t * ( tbw + gap );
			if ( ShowButton( bx, cy, tbw, bh, g_TLTypeLabel[t], g_TLLook[t].show ) )
			{
				g_TLLook[t].show = !g_TLLook[t].show;
				TL_LookChanged( 0 );	// (drawn or not: no mesh change)
			}
			if ( Hover( bx, cy, tbw, bh ) )
				m_nHover = t;
		}
		cy += bh + S( 14 );

		// EDIT: each type in its own colours; the settings below change the picked ones
		Text( F_BOLD, x, cy, C_WOOD, "EDIT" );
		const char *note = "hold shift + click to pick more than one";
		Text( F_SMALL, x + w - TextW( F_SMALL, note ), cy + ( TextH( F_BOLD ) - TextH( F_SMALL ) ) / 2, C_SUB, note );
		cy += label;
		for ( int t = 0; t < TL_TYPES; t++ )
		{
			int bx = x + t * ( tbw + gap );
			if ( TypeButton( bx, cy, tbw, bh, t, ( m_nSel & ( 1 << t ) ) != 0 ) )
			{
				if ( ShiftDown() )
				{
					int sel = m_nSel ^ ( 1 << t );
					if ( sel )
						m_nSel = sel;		// at least one stays picked
				}
				else
					m_nSel = 1 << t;
			}
			if ( Hover( bx, cy, tbw, bh ) )
				m_nHover = t;
		}
		cy += bh + S( 14 );

		// FILL / BORDER: which part the colours and OPACITY change
		int hw = ( w - gap ) / 2;
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
			StartCustom();
			m_nView = VIEW_CUSTOM;
			s_bClick = false;
		}
		cy += thumb + S( 14 );

		// OPACITY (the current part) and WIDTH (the border), side by side
		const TLLook_t &first = g_TLLook[First()];
		int sx2 = x + hw + gap;
		int pct = (int)( Part( First() )[3] * 100 / 255.0f + 0.5f );
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
			for ( int t = 0; t < TL_TYPES; t++ )
				if ( m_nSel & ( 1 << t ) )
					Part( t )[3] = (unsigned char)( pct * 255 / 100.0f + 0.5f );
			TL_LookChanged( m_nSel );
		}
		int width = first.width;
		if ( Slider( 1, sx2, cy, hw, S( 28 ), width, 0, TL_MAX_WIDTH ) )
		{
			for ( int t = 0; t < TL_TYPES; t++ )
				if ( m_nSel & ( 1 << t ) )
				{
					g_TLLook[t].width = width;
					if ( width && !g_TLLook[t].border[3] )
						g_TLLook[t].border[3] = 255;
				}
			TL_LookChanged( m_nSel );
		}
		cy += S( 28 ) + S( 14 );

		// the line at the bottom: why nothing shows, else what the type under the cursor (or the first picked) has
		char line[256];
		int shown = 0;
		for ( int t = 0; t < TL_TYPES; t++ )
			shown += TL_ShownNow( t );
		if ( !TL_Active() )
			TL_StrCopy( line, "off: triggerlook_enable is 0 (the game's own trigger look)", sizeof( line ) );
		else if ( !shown )
			TL_StrCopy( line, "the server isn't showing triggers here: type !st in chat (or !sts)", sizeof( line ) );
		else
		{
			int t = m_nHover >= 0 ? m_nHover : First();
			const TLLook_t &l = g_TLLook[t];
			char border[64];
			if ( l.width && l.border[3] )
				snprintf( border, sizeof( border ), "border #%02X%02X%02X %d%% width %d", l.border[0], l.border[1], l.border[2],
							(int)( l.border[3] * 100 / 255.0f + 0.5f ), l.width );
			else
				TL_StrCopy( border, "no border", sizeof( border ) );
			snprintf( line, sizeof( line ), "%s  \xC2\xB7  %d shown  \xC2\xB7  fill #%02X%02X%02X %d%%  \xC2\xB7  %s%s", g_TLTypeLabel[t],
						TL_ShownNow( t ), l.fill[0], l.fill[1], l.fill[2], (int)( l.fill[3] * 100 / 255.0f + 0.5f ), border, l.show ? "" : "  \xC2\xB7  hidden" );
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

	// EDIT: the type's fill colour inside, its border colour round it (a darker / lighter edge without a border),
	// a white ring when picked
	bool TypeButton( int x, int y, int w, int h, int t, bool sel )
	{
		const TLLook_t &l = g_TLLook[t];
		bool hover = Hover( x, y, w, h );
		Col fill = { l.fill[0], l.fill[1], l.fill[2], 255 };
		Col edge = l.width && l.border[3] ? Col{ l.border[0], l.border[1], l.border[2], 255 } : Light( fill ) ? Shade( fill, 0.6f ) : Lift( fill, 0.3f );
		int p = MAX( 2, S( 3 ) ) * ( l.width && l.border[3] ? 2 : 1 );
		PixelBox( x, y, w, h, p, fill, edge );
		if ( hover )
			Rect( x + p, y + p, w - 2 * p, h - 2 * p, Light( fill ) ? Col{ 0, 0, 0, 30 } : Col{ 255, 255, 255, 30 } );
		const char *label = g_TLTypeLabel[t];
		Text( F_BOLD, x + ( w - TextW( F_BOLD, label ) ) / 2, y + ( h - TextH( F_BOLD ) ) / 2, Light( fill ) ? C_BLACK : C_WHITE, label );
		if ( sel || hover )
			Ring( x, y, w, h, sel ? C_WHITE : C_WOOD );
		return hover && s_bClick;
	}

	// ---- custom colour: the paint menu's picker; every change shows on the triggers at once --------------
	void StartCustom()
	{
		const unsigned char *c = Part( First() );
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
		std::string on = m_nPart == PART_FILL ? "FILL OF" : "BORDER OF";
		int ly = y + S( 124 ) + TextH( F_BOLD ) + TextH( F_SMALL ) + S( 18 );
		Text( F_SMALL, rx, ly, C_SUB, on.c_str() );
		for ( int t = 0; t < TL_TYPES; t++ )
			if ( m_nSel & ( 1 << t ) )
			{
				ly += TextH( F_SMALL ) + S( 2 );
				Text( F_SMALL, rx, ly, C_TEXT, g_TLTypeLabel[t] );
			}
		if ( Button( rx, y + sv - S( 42 ), rw, S( 42 ), "DONE" ) )
			m_nView = VIEW_MAIN;
	}
};

static CTLMenu s_Menu;

static void SetMenuOpen( bool open )
{
	if ( !s_pSurface || !s_Menu.GetVPanel() )
		return;
	if ( open && s_pEngine && s_pEngine->Con_IsVisible() )
		s_pEngine->ClientCmd_Unrestricted( "hideconsole" );
	s_bDown = true;		// a click that opened it doesn't count
	if ( open && !s_Menu.IsOpen() )
		s_Menu.Opened();
	if ( !open )
		TL_LookFlush();		// (a quit right after closing keeps the change)
	s_Menu.Show( open );
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
CON_COMMAND( triggerlook_ui, "triggerlook_ui select <types,...> | part fill|border | view main|custom | mark <text>: set the menu's state, or mark the info file (testing)" )
{
	if ( args.ArgC() < 3 )
	{
		Msg( "usage: triggerlook_ui select <type[,type...]|all> | part fill|border | view main|custom\n" );
		return;
	}
	if ( !TL_StrICmp( args[1], "select" ) )
	{
		int mask = 0;
		char list[256];
		TL_StrCopy( list, args.ArgS() + strlen( args[1] ) + 1, sizeof( list ) );
		for ( char *p = strtok( list, ", " ); p; p = strtok( NULL, ", " ) )
			mask |= TL_TypeMask( p );
		s_Menu.SetSelection( mask );
	}
	else if ( !TL_StrICmp( args[1], "part" ) )
		s_Menu.SetPart( !TL_StrICmp( args[2], "border" ) ? PART_BORDER : PART_FILL );
	else if ( !TL_StrICmp( args[1], "view" ) )
		s_Menu.SetView( !TL_StrICmp( args[2], "custom" ) ? VIEW_CUSTOM : VIEW_MAIN );
	else if ( !TL_StrICmp( args[1], "mark" ) )
	{
		// a line in the info file, for a test helper outside the game to wait for
		char path[1024];
		snprintf( path, sizeof( path ), "%s/info.txt", TLP_CacheDir() );
		if ( FILE *f = fopen( path, "a" ) )
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
		// "!ingame": wait until a map is up and its triggers read
		if ( l.cmd == "!ingame" && !TL_Active() )
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
