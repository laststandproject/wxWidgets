/////////////////////////////////////////////////////////////////////////////
// Name:        src/msw/toolbar.cpp
// Purpose:     wxToolBar
// Author:      Julian Smart
// Modified by:
// Created:     04/01/98
// Copyright:   (c) Julian Smart
// Licence:     wxWindows licence
/////////////////////////////////////////////////////////////////////////////

// ============================================================================
// declarations
// ============================================================================

// ----------------------------------------------------------------------------
// headers
// ----------------------------------------------------------------------------

// For compilers that support precompilation, includes "wx.h".
#include "wx/wxprec.h"

#ifdef __BORLANDC__
    #pragma hdrstop
#endif

#if wxUSE_TOOLBAR && wxUSE_TOOLBAR_NATIVE

#include "wx/toolbar.h"

#ifndef WX_PRECOMP
    #include "wx/msw/wrapcctl.h" // include <commctrl.h> "properly"
    #include "wx/dynarray.h"
    #include "wx/frame.h"
    #include "wx/log.h"
    #include "wx/intl.h"
    #include "wx/settings.h"
    #include "wx/bitmap.h"
    #include "wx/region.h"
    #include "wx/dcmemory.h"
    #include "wx/control.h"
    #include "wx/app.h"         // for GetComCtl32Version
    #include "wx/image.h"
    #include "wx/stattext.h"
#endif

#include "wx/artprov.h"
#include "wx/sysopt.h"
#include "wx/dcclient.h"
#include "wx/rawbmp.h"
#include "wx/scopedarray.h"

#include <windowsx.h> // needed by GET_X_LPARAM and GET_Y_LPARAM macros

#include "wx/msw/private.h"
#include "wx/msw/winundef.h"
#include "wx/msw/dc.h"
#include "wx/msw/dib.h"

#if wxUSE_UXTHEME
#include "wx/msw/uxtheme.h"
#endif

// ----------------------------------------------------------------------------
// constants
// ----------------------------------------------------------------------------

// these standard constants are not always defined in compilers headers

// Styles
#ifndef TBSTYLE_FLAT
    #define TBSTYLE_LIST            0x1000
    #define TBSTYLE_FLAT            0x0800
#endif

#ifndef TBSTYLE_TRANSPARENT
    #define TBSTYLE_TRANSPARENT     0x8000
#endif

#ifndef TBSTYLE_TOOLTIPS
    #define TBSTYLE_TOOLTIPS        0x0100
#endif

// Messages
#ifndef TB_GETSTYLE
    #define TB_SETSTYLE             (WM_USER + 56)
    #define TB_GETSTYLE             (WM_USER + 57)
#endif

#ifndef TB_HITTEST
    #define TB_HITTEST              (WM_USER + 69)
#endif

#ifndef TB_GETMAXSIZE
    #define TB_GETMAXSIZE           (WM_USER + 83)
#endif

// ----------------------------------------------------------------------------
// wxWin macros
// ----------------------------------------------------------------------------

wxIMPLEMENT_DYNAMIC_CLASS(wxToolBar, wxControl);

/*
    TOOLBAR PROPERTIES
        tool
            bitmap
            bitmap2
            tooltip
            longhelp
            radio (bool)
            toggle (bool)
        separator
        style ( wxNO_BORDER | wxTB_HORIZONTAL)
        bitmapsize
        margins
        packing
        separation

        dontattachtoframe
*/

wxBEGIN_EVENT_TABLE(wxToolBar, wxToolBarBase)
    EVT_MOUSE_EVENTS(wxToolBar::OnMouseEvent)
    EVT_SYS_COLOUR_CHANGED(wxToolBar::OnSysColourChanged)
    EVT_ERASE_BACKGROUND(wxToolBar::OnEraseBackground)
wxEND_EVENT_TABLE()

// ----------------------------------------------------------------------------
// module globals
// ----------------------------------------------------------------------------

// This is used to check if the toolbar itself doesn't get destroyed while
// handling its event.
static wxToolBar* gs_liveToolbar = NULL;

// ----------------------------------------------------------------------------
// private classes
// ----------------------------------------------------------------------------

class wxToolBarTool : public wxToolBarToolBase
{
public:
    wxToolBarTool(wxToolBar *tbar,
                  int id,
                  const wxString& label,
                  const wxBitmap& bmpNormal,
                  const wxBitmap& bmpDisabled,
                  wxItemKind kind,
                  wxObject *clientData,
                  const wxString& shortHelp,
                  const wxString& longHelp)
        : wxToolBarToolBase(tbar, id, label, bmpNormal, bmpDisabled, kind,
                            clientData, shortHelp, longHelp)
    {
        m_staticText = NULL;
        m_toBeDeleted  = false;
    }

    wxToolBarTool(wxToolBar *tbar, wxControl *control, const wxString& label)
        : wxToolBarToolBase(tbar, control, label)
    {
        if ( IsControl() && !m_label.empty() )
        {
            // Create a control to render the control's label.
            // It has the same witdh as the control.
            wxSize size(control->GetSize().GetWidth(), wxDefaultCoord);
            m_staticText = new wxStaticText
                               (
                                 m_tbar,
                                 wxID_ANY,
                                 m_label,
                                 wxDefaultPosition,
                                 size,
                                 wxALIGN_CENTRE | wxST_NO_AUTORESIZE
                               );
        }
        else // no label
        {
            m_staticText = NULL;
        }

        m_toBeDeleted  = false;
    }

    virtual ~wxToolBarTool()
    {
        delete m_staticText;
    }

    virtual void SetLabel(const wxString& label) wxOVERRIDE
    {
        wxASSERT_MSG( IsControl() || IsButton(),
           wxS("Label can be set for control or button tool only") );

        if ( label == m_label )
            return;

        wxToolBarToolBase::SetLabel(label);

        if ( IsControl() )
        {
            if ( m_staticText )
            {
                 if ( !label.empty() )
                 {
                    m_staticText->SetLabel(label);
                 }
                 else
                 {
                    delete m_staticText;
                    m_staticText = NULL;
                 }
            }
            else
            {
                 if ( !label.empty() )
                 {
                    // Create a control to render the control's label.
                    // It has the same witdh as the control.
                    wxSize size(m_control->GetSize().GetWidth(), wxDefaultCoord);
                    m_staticText = new wxStaticText(m_tbar, wxID_ANY, label,
                                        wxDefaultPosition, size,
                                        wxALIGN_CENTRE | wxST_NO_AUTORESIZE);
                 }
            }
        }
        else if ( IsButton() )
        {
            // Because new label can have different length than the old one
            // so updating button's label with TB_SETBUTTONINFO would require
            // also manual re-positionining items in the control tools located
            // to the right in the toolbar and recalculation of stretchable
            // spacers so it is easier just to recreate the toolbar with
            // Realize(). Performance penalty should be negligible.
            m_tbar->Realize();
        }
    }

    wxStaticText* GetStaticText()
    {
        wxASSERT_MSG( IsControl(),
                      wxT("only makes sense for embedded control tools") );

        return m_staticText;
    }

    // we need ids for the spacers which we want to modify later on, this
    // function will allocate a valid/unique id for a spacer if not done yet
    void AllocSpacerId()
    {
        if ( m_id == wxID_SEPARATOR )
            m_id = wxWindow::NewControlId();
    }

    // this method is used for controls only and offsets the control by the
    // given amount (in pixels) in horizontal direction
    void MoveBy(int offset)
    {
        wxControl * const control = GetControl();

        control->Move(control->GetPosition().x + offset, wxDefaultCoord);

        if ( m_staticText )
        {
            m_staticText->Move(m_staticText->GetPosition().x + offset,
                               wxDefaultCoord);
        }
    }

    void ToBeDeleted(bool toBeDeleted = true) { m_toBeDeleted = toBeDeleted; }
    bool IsToBeDeleted() const { return m_toBeDeleted; }

private:
    wxStaticText *m_staticText;
    bool m_toBeDeleted;

    wxDECLARE_NO_COPY_CLASS(wxToolBarTool);
};

// ----------------------------------------------------------------------------
// helper functions
// ----------------------------------------------------------------------------

// Return the rectangle of the item at the given index and, if specified, with
// the given id.
//
// Returns an empty (0, 0, 0, 0) rectangle if fails so the caller may compare
// r.right or r.bottom with 0 to check for this.
static RECT wxGetTBItemRect(HWND hwnd, int index, int id = wxID_NONE)
{
    RECT r;

    // note that we use TB_GETITEMRECT and not TB_GETRECT because the latter
    // only appeared in v4.70 of comctl32.dll
    if ( !::SendMessage(hwnd, TB_GETITEMRECT, index, (LPARAM)&r) )
    {
        // This call can return false status even when there is no real error,
        // e.g. for a hidden button, so check for this to avoid spurious logs.
        const DWORD err = ::GetLastError();
        if ( err != ERROR_SUCCESS )
        {
            bool reportError = true;

            if ( id != wxID_NONE )
            {
                const LRESULT state = ::SendMessage(hwnd, TB_GETSTATE, id, 0);
                if ( state != -1 && (state & TBSTATE_HIDDEN) )
                {
                    // There is no real error to report after all.
                    reportError = false;
                }
                else // It is not hidden.
                {
                    // So it must have been a real error, report it with the
                    // original error code and not the one from TB_GETSTATE.
                    ::SetLastError(err);
                }
            }

            if ( reportError )
                wxLogLastError(wxT("TB_GETITEMRECT"));
        }

        ::SetRectEmpty(&r);
    }

    return r;
}

static bool MSWShouldBeChecked(const wxToolBarToolBase *tool)
{
    // Apparently, "checked" state image overrides the "disabled" image
    // so we need to enforce our custom "disabled" image (if there is any)
    // to be drawn for checked and disabled button tool.
    // Note: We believe this erroneous overriding is fixed in MSW 8.
    if ( wxGetWinVersion() <= wxWinVersion_7 &&
            tool->GetKind() == wxITEM_CHECK &&
                tool->GetDisabledBitmap().IsOk() &&
                    !tool->IsEnabled() )
    {
        return false;
    }

    return tool->IsToggled();
}

// ============================================================================
// implementation
// ============================================================================

// ----------------------------------------------------------------------------
// wxToolBarTool
// ----------------------------------------------------------------------------

wxToolBarToolBase *wxToolBar::CreateTool(int id,
                                         const wxString& label,
                                         const wxBitmap& bmpNormal,
                                         const wxBitmap& bmpDisabled,
                                         wxItemKind kind,
                                         wxObject *clientData,
                                         const wxString& shortHelp,
                                         const wxString& longHelp)
{
    return new wxToolBarTool(this, id, label, bmpNormal, bmpDisabled, kind,
                             clientData, shortHelp, longHelp);
}

wxToolBarToolBase *
wxToolBar::CreateTool(wxControl *control, const wxString& label)
{
    return new wxToolBarTool(this, control, label);
}

// ----------------------------------------------------------------------------
// wxToolBar construction
// ----------------------------------------------------------------------------

void wxToolBar::Init()
{
    m_hBitmap = 0;
    m_disabledImgList = NULL;

    m_nButtons = 0;
    m_totalFixedSize = 0;

    // even though modern Windows applications typically use 24*24 (or even
    // 32*32) size for their bitmaps, the native control itself still uses the
    // old 16*15 default size (see TB_SETBITMAPSIZE documentation in MSDN), so
    // default to it so that we don't call SetToolBitmapSize() unnecessarily in
    // wxToolBarBase::AdjustToolBitmapSize()
    m_defaultWidth = 16;
    m_defaultHeight = 15;

    m_pInTool = NULL;
}

bool wxToolBar::Create(wxWindow *parent,
                       wxWindowID id,
                       const wxPoint& pos,
                       const wxSize& size,
                       long style,
                       const wxString& name)
{
    // common initialisation
    if ( !CreateControl(parent, id, pos, size, style, wxDefaultValidator, name) )
        return false;

    FixupStyle();

    // MSW-specific initialisation
    if ( !MSWCreateToolbar(pos, size) )
        return false;

    wxSetCCUnicodeFormat(GetHwnd());

    // we always erase our background on WM_PAINT so there is no need to do it
    // in WM_ERASEBKGND too (by default this won't be done but if the toolbar
    // has a non default background colour, then it would be used in both
    // places resulting in flicker)
    if (wxApp::GetComCtl32Version() >= 600)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
    }

    return true;
}

void wxToolBar::MSWSetPadding(WXWORD padding)
{
    DWORD curPadding = ::SendMessage(GetHwnd(), TB_GETPADDING, 0, 0);
    // Preserve orthogonal padding
    DWORD newPadding = IsVertical() ? MAKELPARAM(LOWORD(curPadding), padding)
                                    : MAKELPARAM(padding, HIWORD(curPadding));
    ::SendMessage(GetHwnd(), TB_SETPADDING, 0, newPadding);
}

bool wxToolBar::MSWCreateToolbar(const wxPoint& pos, const wxSize& size)
{
    if ( !MSWCreateControl(TOOLBARCLASSNAME, wxEmptyString, pos, size) )
        return false;

    // toolbar-specific post initialisation
    ::SendMessage(GetHwnd(), TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

#ifdef TB_SETEXTENDEDSTYLE
    ::SendMessage(GetHwnd(), TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS);
#endif

    // Retrieve or apply/restore tool packing value.
    if ( m_toolPacking <= 0 )
    {
        // Retrieve packing value if it hasn't been yet set with SetToolPacking.
        DWORD padding = ::SendMessage(GetHwnd(), TB_GETPADDING, 0, 0);
        m_toolPacking = IsVertical() ? HIWORD(padding) : LOWORD(padding);
    }
    else
    {
        // Apply packing value if it has been already set with SetToolPacking.
        MSWSetPadding(m_toolPacking);
    }

    return true;
}

void wxToolBar::Recreate()
{
    const HWND hwndOld = GetHwnd();
    if ( !hwndOld )
    {
        // we haven't been created yet, no need to recreate
        return;
    }

    // get the position and size before unsubclassing the old toolbar
    const wxPoint pos = GetPosition();
    const wxSize size = GetSize();

    UnsubclassWin();

    if ( !MSWCreateToolbar(pos, size) )
    {
        // what can we do?
        wxFAIL_MSG( wxT("recreating the toolbar failed") );

        return;
    }

    // reparent all our children under the new toolbar
    for ( wxWindowList::compatibility_iterator node = m_children.GetFirst();
          node;
          node = node->GetNext() )
    {
        wxWindow *win = node->GetData();
        if ( !win->IsTopLevel() )
            ::SetParent(GetHwndOf(win), GetHwnd());
    }

    // only destroy the old toolbar now --
    // after all the children had been reparented
    ::DestroyWindow(hwndOld);

    // it is for the old bitmap control and can't be used with the new one
    if ( m_hBitmap )
    {
        ::DeleteObject((HBITMAP) m_hBitmap);
        m_hBitmap = 0;
    }

    wxDELETE(m_disabledImgList);

    Realize();
}

wxToolBar::~wxToolBar()
{
    // Indicate to the code in MSWCommand() that the toolbar is destroyed.
    if ( gs_liveToolbar == this )
        gs_liveToolbar = NULL;

    // we must refresh the frame size when the toolbar is deleted but the frame
    // is not - otherwise toolbar leaves a hole in the place it used to occupy
    SendSizeEventToParent();

    if ( m_hBitmap )
        ::DeleteObject((HBITMAP) m_hBitmap);

    delete m_disabledImgList;
}

wxSize wxToolBar::DoGetBestSize() const
{
    wxSize sizeBest;

    SIZE size;
    if ( !::SendMessage(GetHwnd(), TB_GETMAXSIZE, 0, (LPARAM)&size) )
    {
        // maybe an old (< 0x400) Windows version? try to approximate the
        // toolbar size ourselves
        sizeBest = GetToolSize();
        sizeBest.y += 2 * ::GetSystemMetrics(SM_CYBORDER); // Add borders
        sizeBest.x *= GetToolsCount();

        // reverse horz and vertical components if necessary
        if ( IsVertical() )
        {
            wxSwap(sizeBest.x, sizeBest.y);
        }
    }
    else // TB_GETMAXSIZE succeeded
    {
        // but it could still return an incorrect result due to what appears to
        // be a bug in old comctl32.dll versions which don't handle controls in
        // the toolbar correctly, so work around it (see SF patch 1902358)
        if ( !IsVertical() && wxApp::GetComCtl32Version() < 600 )
        {
            // calculate the toolbar width in alternative way
            const RECT rcFirst = wxGetTBItemRect(GetHwnd(), 0);
            const RECT rcLast = wxGetTBItemRect(GetHwnd(), GetToolsCount() - 1);

            const int widthAlt = rcLast.right - rcFirst.left;
            if ( widthAlt > size.cx )
                size.cx = widthAlt;
        }

        sizeBest.x = size.cx;
        sizeBest.y = size.cy;
    }

    if ( !IsVertical() )
    {
        wxToolBarToolsList::compatibility_iterator node;
        for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
        {
            wxToolBarTool * const
                tool = static_cast<wxToolBarTool *>(node->GetData());
            if (tool->IsControl())
            {
                int y = tool->GetControl()->GetSize().y;
                // Approximate border size
                if (y > (sizeBest.y - 4))
                    sizeBest.y = y + 4;
            }
        }

        // Without the extra height, DoGetBestSize can report a size that's
        // smaller than the actual window, causing windows to overlap slightly
        // in some circumstances, leading to missing borders (especially noticeable
        // in AUI layouts).
        if (!(GetWindowStyle() & wxTB_NODIVIDER))
            sizeBest.y += 2;
        sizeBest.y ++;
    }

    return sizeBest;
}

WXDWORD wxToolBar::MSWGetStyle(long style, WXDWORD *exstyle) const
{
    // toolbars never have border, giving one to them results in broken
    // appearance
    WXDWORD msStyle = wxControl::MSWGetStyle
                      (
                        (style & ~wxBORDER_MASK) | wxBORDER_NONE, exstyle
                      );

    if ( !(style & wxTB_NO_TOOLTIPS) )
        msStyle |= TBSTYLE_TOOLTIPS;

    if ( style & wxTB_FLAT )
        msStyle |= TBSTYLE_FLAT;

    if ( style & wxTB_HORZ_LAYOUT )
        msStyle |= TBSTYLE_LIST;

    if ( style & wxTB_NODIVIDER )
        msStyle |= CCS_NODIVIDER;

    if ( style & wxTB_NOALIGN )
        msStyle |= CCS_NOPARENTALIGN;

    if ( style & wxTB_VERTICAL )
        msStyle |= CCS_VERT;

    if( style & wxTB_BOTTOM )
        msStyle |= CCS_BOTTOM;

    if ( style & wxTB_RIGHT )
        msStyle |= CCS_RIGHT;

    // always use TBSTYLE_TRANSPARENT because the background is not drawn
    // correctly without it in all themes and, for whatever reason, the control
    // also flickers horribly when it is resized if this style is not used
    //
    // note that this is implicitly enabled by the native toolbar itself when
    // TBSTYLE_FLAT is used (i.e. it's impossible to use TBSTYLE_FLAT without
    // TBSTYLE_TRANSPARENT) but turn it on explicitly in any case
    msStyle |= TBSTYLE_TRANSPARENT;

    return msStyle;
}

// ----------------------------------------------------------------------------
// adding/removing tools
// ----------------------------------------------------------------------------

bool wxToolBar::DoInsertTool(size_t WXUNUSED(pos),
                             wxToolBarToolBase *tool)
{
    // We might be inserting back a tool previously removed from the toolbar,
    // make sure to reset its "to be deleted" flag to ensure that we do take it
    // into account during our layout even in this case.
    static_cast<wxToolBarTool*>(tool)->ToBeDeleted(false);

    // nothing special to do here - we really create the toolbar buttons in
    // Realize() later
    InvalidateBestSize();
    return true;
}

bool wxToolBar::DoDeleteTool(size_t pos, wxToolBarToolBase *tool)
{
    // get the size of the button we're going to delete
    const RECT r = wxGetTBItemRect(GetHwnd(), pos);

    int delta = IsVertical() ? r.bottom - r.top : r.right - r.left;

    m_totalFixedSize -= delta;

    // do delete the button
    m_nButtons--;
    if ( !::SendMessage(GetHwnd(), TB_DELETEBUTTON, pos, 0) )
    {
        wxLogLastError(wxT("TB_DELETEBUTTON"));

        return false;
    }

    static_cast<wxToolBarTool*>(tool)->ToBeDeleted();

    // and finally rearrange the tools:

    // by shifting left all controls on the right hand side
    wxToolBarToolsList::compatibility_iterator node;
    for ( node = m_tools.Find(tool); node; node = node->GetNext() )
    {
        wxToolBarTool * const ctool = static_cast<wxToolBarTool*>(node->GetData());

        if ( ctool->IsToBeDeleted() )
            continue;

        if ( ctool->IsControl() )
        {
            ctool->MoveBy(-delta);
        }
    }

    // by recalculating stretchable spacers, if there are any
    UpdateStretchableSpacersSize();

    InvalidateBestSize();

    return true;
}

void wxToolBar::CreateDisabledImageList()
{
    wxDELETE(m_disabledImgList);

    // search for the first disabled button img in the toolbar, if any
    for ( wxToolBarToolsList::compatibility_iterator
            node = m_tools.GetFirst(); node; node = node->GetNext() )
    {
        wxToolBarToolBase *tool = node->GetData();
        wxBitmap bmpDisabled = tool->GetDisabledBitmap();
        if ( bmpDisabled.IsOk() )
        {
            const wxSize sizeBitmap = bmpDisabled.GetSize();
            m_disabledImgList = new wxImageList
                                    (
                                        sizeBitmap.x,
                                        sizeBitmap.y,
                                        // Don't use mask if we have alpha
                                        // (wxImageList will fall back to
                                        // mask if alpha not supported)
                                        !bmpDisabled.HasAlpha(),
                                        GetToolsCount()
                                    );
            break;
        }
    }
}

bool wxToolBar::Realize()
{
    if ( !wxToolBarBase::Realize() )
        return false;

    const size_t nTools = GetToolsCount();

    // don't change the values of these constants, they can be set from the
    // user code via wxSystemOptions
    enum
    {
        Remap_None = -1,
        Remap_Bg,
        Remap_Buttons,
        Remap_TransparentBg
    };

    // the user-specified option overrides anything, but if it wasn't set, only
    // remap the buttons on 8bpp displays as otherwise the bitmaps usually look
    // much worse after remapping
    static const wxChar *remapOption = wxT("msw.remap");
    int remapValue = wxSystemOptions::HasOption(remapOption)
                          ? wxSystemOptions::GetOptionInt(remapOption)
                          : wxDisplayDepth() <= 8 ? Remap_Buttons
                                                  : Remap_None;


    // delete all old buttons, if any
    for ( size_t pos = 0; pos < m_nButtons; pos++ )
    {
        if ( !::SendMessage(GetHwnd(), TB_DELETEBUTTON, 0, 0) )
        {
            wxLogDebug(wxT("TB_DELETEBUTTON failed"));
        }
    }

    // First, add the bitmap: we use one bitmap for all toolbar buttons
    // ----------------------------------------------------------------

    wxToolBarToolsList::compatibility_iterator node;
    int bitmapId = 0;

    if ( !HasFlag(wxTB_NOICONS) )
    {
        // if we already have a bitmap, we'll replace the existing one --
        // otherwise we'll install a new one
        HBITMAP oldToolBarBitmap = (HBITMAP)m_hBitmap;

        const wxCoord totalBitmapWidth  = m_defaultWidth *
                                          wx_truncate_cast(wxCoord, nTools),
                      totalBitmapHeight = m_defaultHeight;

        // Create a bitmap and copy all the tool bitmaps into it
        wxMemoryDC dcAllButtons;
        wxBitmap bitmap(totalBitmapWidth, totalBitmapHeight);

        for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
        {
            wxToolBarToolBase *tool = node->GetData();
            if ( tool->IsButton() &&
                 tool->GetNormalBitmap().IsOk() && tool->GetNormalBitmap().HasAlpha() )
            {
                // By default bitmaps don't have alpha in wxMSW, but if we
                // use a bitmap tool with alpha, we should use alpha for
                // the combined bitmap as well.
                bitmap.UseAlpha();
#ifdef wxHAS_RAW_BITMAP
                // Clear the combined bitmap to have (0,0,0,0) pixels so that
                // alpha blending bitmaps onto it doesn't change their appearance.
                wxAlphaPixelData data(bitmap);
                if ( data )
                {
                    wxAlphaPixelData::Iterator p(data);
                    for (int y = 0; y < totalBitmapHeight; y++)
                    {
                        wxAlphaPixelData::Iterator rowStart = p;
                        for (int x = 0; x < totalBitmapWidth; ++x, ++p)
                        {
                            p.Red() = p.Green() = p.Blue() = p.Alpha() = 0;
                        }
                        p = rowStart;
                        p.OffsetY(data, 1);
                    }
#endif
                }
                break;
            }
        }

        dcAllButtons.SelectObject(bitmap);

        if ( remapValue != Remap_TransparentBg && !bitmap.HasAlpha() )
        {
            dcAllButtons.SetBackground(GetBackgroundColour());
            dcAllButtons.Clear();
        }

        HBITMAP hBitmap = GetHbitmapOf(bitmap);

        if ( remapValue == Remap_Bg )
        {
            dcAllButtons.SelectObject(wxNullBitmap);

            // Even if we're not remapping the bitmap
            // content, we still have to remap the background.
            hBitmap = (HBITMAP)MapBitmap((WXHBITMAP) hBitmap,
                totalBitmapWidth, totalBitmapHeight);

            dcAllButtons.SelectObject(bitmap);
        }

        // the button position
        wxCoord x = 0;

        // the number of buttons (not separators)
        int nButtons = 0;

        CreateDisabledImageList();
        for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
        {
            wxToolBarToolBase *tool = node->GetData();
            if ( tool->IsButton() )
            {
                wxBitmap bmp = tool->GetNormalBitmap();

                const int w = bmp.GetWidth();
                const int h = bmp.GetHeight();

                if ( bmp.IsOk() )
                {
                    int xOffset = wxMax(0, (m_defaultWidth - w)/2);
                    int yOffset = wxMax(0, (m_defaultHeight - h)/2);

#if wxUSE_IMAGE
                    // If a mix of icons with alpha and without is used,
                    // convert them all to use alpha.
                    if (bitmap.HasAlpha() && !bmp.HasAlpha())
                    {
                        wxImage img = bmp.ConvertToImage();
                        img.InitAlpha();
                        bmp = wxBitmap(img);
                    }
#endif

                    // notice the last parameter: do use mask
                    dcAllButtons.DrawBitmap(bmp, x + xOffset, yOffset, true);

                    // Handle of the bitmap could have changed inside
                    // DrawBitmap() call if it had to convert it from DDB to
                    // DIB internally, as is necessary if the bitmap being
                    // drawn had alpha channel.
                    hBitmap = GetHbitmapOf(bitmap);
                }
                else
                {
                    wxFAIL_MSG( wxT("invalid tool button bitmap") );
                }

                // also deal with disabled bitmap if we want to use them
                if ( m_disabledImgList )
                {
                    wxBitmap bmpDisabled = tool->GetDisabledBitmap();
#if wxUSE_IMAGE && wxUSE_WXDIB
                    if ( !bmpDisabled.IsOk() )
                    {
                        // no disabled bitmap specified but we still need to
                        // fill the space in the image list with something, so
                        // we grey out the normal bitmap
                        wxImage
                          imgGreyed = bmp.ConvertToImage().ConvertToGreyscale();

                        if ( remapValue == Remap_Buttons )
                        {
                            // we need to have light grey background colour for
                            // MapBitmap() to work correctly
                            for ( int y = 0; y < h; y++ )
                            {
                                for ( int xx = 0; xx < w; xx++ )
                                {
                                    if ( imgGreyed.IsTransparent(xx, y) )
                                        imgGreyed.SetRGB(xx, y,
                                                         wxLIGHT_GREY->Red(),
                                                         wxLIGHT_GREY->Green(),
                                                         wxLIGHT_GREY->Blue());
                                }
                            }
                        }

                        bmpDisabled = wxBitmap(imgGreyed);
                    }
#endif // wxUSE_IMAGE

                    if ( remapValue == Remap_Buttons )
                        MapBitmap(bmpDisabled.GetHBITMAP(), w, h);

                    // @UE3 This is a fix that is adapted from this forum post, but cleaned up better.
                    // https://forums.wxwidgets.org/viewtopic.php?p=54268&sid=102b2bb734341841a6e37f4bcd8be3b4#p54268
                    // We're eventually going to stop using wxwidgets, so this is fine as a bandaid fix.
                    // wxWidget's has a fix (I think) in commit 97f6c85d9bb9d11c4d635f0422b1fc8d6cb11509, but I don't
                    // think we're really able to pull that in ourselves.
                    // @TODO Should we actually move this above to affect the main bmp instead?
                    if (w != m_defaultWidth || h != m_defaultHeight)
                    {
                        //wxLogDebug("Disabled bitmap did not match m_defaultWidth or m_defaultHeight. Resizing!");

                        int xOffset = wxMax(0, (m_defaultWidth - w) / 2);
                        int yOffset = wxMax(0, (m_defaultHeight - h) / 2);

                        wxImage img = bmpDisabled.ConvertToImage();
                        bmpDisabled = wxBitmap(img.Resize(wxSize(m_defaultWidth, m_defaultHeight), wxPoint(xOffset, yOffset)));
                    }

                    m_disabledImgList->Add(bmpDisabled);
                }

                // still inc width and number of buttons because otherwise the
                // subsequent buttons will all be shifted which is rather confusing
                // (and like this you'd see immediately which bitmap was bad)
                x += m_defaultWidth;
                nButtons++;
            }
        }

        dcAllButtons.SelectObject(wxNullBitmap);

#if wxUSE_WXDIB
        if ( bitmap.HasAlpha() )
        {
            // Strangely, toolbar expects bitmaps with transparency to not
            // be premultiplied, unlike most of the rest of win32. Without this
            // conversion, e.g. antialiased lines would be subtly, but
            // noticeably misrendered.
            hBitmap = wxDIB(bitmap.ConvertToImage(),
                            wxDIB::PixelFormat_NotPreMultiplied).Detach();
        }
        else
#endif
        {
            hBitmap = GetHbitmapOf(bitmap);
            // don't delete this HBITMAP!
            bitmap.SetHBITMAP(0);
        }

        if ( remapValue == Remap_Buttons )
        {
            // Map to system colours
            hBitmap = (HBITMAP)MapBitmap((WXHBITMAP) hBitmap,
                                         totalBitmapWidth, totalBitmapHeight);
        }

        m_hBitmap = hBitmap;

        bool addBitmap = true;

        if ( oldToolBarBitmap )
        {
#ifdef TB_REPLACEBITMAP
            TBREPLACEBITMAP replaceBitmap;
            replaceBitmap.hInstOld = NULL;
            replaceBitmap.hInstNew = NULL;
            replaceBitmap.nIDOld = (UINT_PTR)oldToolBarBitmap;
            replaceBitmap.nIDNew = (UINT_PTR)hBitmap;
            replaceBitmap.nButtons = nButtons;
            if ( !::SendMessage(GetHwnd(), TB_REPLACEBITMAP,
                                0, (LPARAM) &replaceBitmap) )
            {
                wxFAIL_MSG(wxT("Could not replace the old bitmap"));
            }

            ::DeleteObject(oldToolBarBitmap);

            // already done
            addBitmap = false;
#else
            // we can't replace the old bitmap, so we will add another one
            // (awfully inefficient, but what else to do?) and shift the bitmap
            // indices accordingly
            addBitmap = true;

            bitmapId = m_nButtons;
#endif // TB_REPLACEBITMAP
        }

        if ( addBitmap ) // no old bitmap or we can't replace it
        {
            TBADDBITMAP tbAddBitmap;
            tbAddBitmap.hInst = 0;
            tbAddBitmap.nID = (UINT_PTR)hBitmap;
            if ( ::SendMessage(GetHwnd(), TB_ADDBITMAP,
                               (WPARAM) nButtons, (LPARAM)&tbAddBitmap) == -1 )
            {
                wxFAIL_MSG(wxT("Could not add bitmap to toolbar"));
            }
        }

        HIMAGELIST hil = m_disabledImgList
                            ? GetHimagelistOf(m_disabledImgList)
                            : 0;

        // notice that we set the image list even if don't have one right
        // now as we could have it before and need to reset it in this case
        HIMAGELIST oldImageList = (HIMAGELIST)
          ::SendMessage(GetHwnd(), TB_SETDISABLEDIMAGELIST, 0, (LPARAM)hil);

        // delete previous image list if any
        if ( oldImageList )
            ::DeleteObject(oldImageList);
    }


    // Next add the buttons and separators
    // -----------------------------------

    wxScopedArray<TBBUTTON> buttons(nTools);

    // this array will hold the indices of all controls in the toolbar
    wxArrayInt controlIds;

    bool lastWasRadio = false;
    int i = 0;
    for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
    {
        wxToolBarTool *tool = static_cast<wxToolBarTool *>(node->GetData());

        TBBUTTON& button = buttons[i];

        wxZeroMemory(button);

        bool isRadio = false;
        switch ( tool->GetStyle() )
        {
            case wxTOOL_STYLE_CONTROL:
                if ( wxStaticText *staticText = tool->GetStaticText() )
                {
                    // Display control and its label only if buttons have icons
                    // and texts as otherwise there is not enough room on the
                    // toolbar to fit the label.
                    staticText->
                        Show(HasFlag(wxTB_TEXT) && !HasFlag(wxTB_NOICONS));
                }

                // Set separator width/height to fit the control width/height
                // taking into account tool padding value.
                // (height is not used but it is set for the sake of consistency).
                {
                    const wxSize sizeControl = tool->GetControl()->GetSize();
                    button.iBitmap = m_toolPacking + (IsVertical() ? sizeControl.y : sizeControl.x);
                }

                wxFALLTHROUGH;

            case wxTOOL_STYLE_SEPARATOR:
                if ( tool->IsStretchableSpace() )
                {
                    // we're going to modify the size of this button later and
                    // so we need a valid id for it and not wxID_SEPARATOR
                    // which is used by spacers by default
                    tool->AllocSpacerId();
                }

                button.idCommand = tool->GetId();

                // We don't embed controls in the vertical toolbar but for
                // every control there must exist a corresponding button to
                // keep indexes the same as in the horizontal case.
                if ( IsVertical() && tool->IsControl() )
                    button.fsState = TBSTATE_HIDDEN;
                else
                    button.fsState = TBSTATE_ENABLED;
                button.fsStyle = TBSTYLE_SEP;
                break;

            case wxTOOL_STYLE_BUTTON:
                if ( !HasFlag(wxTB_NOICONS) )
                    button.iBitmap = bitmapId;

                if ( HasFlag(wxTB_TEXT) )
                {
                    const wxString& label = tool->GetLabel();
                    if ( !label.empty() )
                        button.iString = (INT_PTR) wxMSW_CONV_LPCTSTR(label);
                }

                button.idCommand = tool->GetId();

                if ( tool->IsEnabled() )
                    button.fsState |= TBSTATE_ENABLED;
                if ( MSWShouldBeChecked(tool) )
                    button.fsState |= TBSTATE_CHECKED;

                switch ( tool->GetKind() )
                {
                    case wxITEM_RADIO:
                        button.fsStyle = TBSTYLE_CHECKGROUP;

                        if ( !lastWasRadio )
                        {
                            // the first item in the radio group is checked by
                            // default to be consistent with wxGTK and the menu
                            // radio items
                            button.fsState |= TBSTATE_CHECKED;

                            if (tool->Toggle(true))
                            {
                                DoToggleTool(tool, true);
                            }
                        }
                        else if ( tool->IsToggled() )
                        {
                            wxToolBarToolsList::compatibility_iterator nodePrev = node->GetPrevious();
                            int prevIndex = i - 1;
                            while ( nodePrev )
                            {
                                TBBUTTON& prevButton = buttons[prevIndex];
                                wxToolBarToolBase *toolPrev = nodePrev->GetData();
                                if ( !toolPrev->IsButton() || toolPrev->GetKind() != wxITEM_RADIO )
                                    break;

                                if ( toolPrev->Toggle(false) )
                                    DoToggleTool(toolPrev, false);

                                prevButton.fsState &= ~TBSTATE_CHECKED;
                                nodePrev = nodePrev->GetPrevious();
                                prevIndex--;
                            }
                        }

                        isRadio = true;
                        break;

                    case wxITEM_CHECK:
                        button.fsStyle = TBSTYLE_CHECK;
                        break;

                    case wxITEM_NORMAL:
                        button.fsStyle = TBSTYLE_BUTTON;
                        break;

                   case wxITEM_DROPDOWN:
                        button.fsStyle = TBSTYLE_DROPDOWN;
                        break;

                    default:
                        wxFAIL_MSG( wxT("unexpected toolbar button kind") );
                        button.fsStyle = TBSTYLE_BUTTON;
                        break;
                }

                // When toolbar has wxTB_HORZ_LAYOUT style then
                // instead of using fixed widths for all buttons, size them
                // automatically according to the size of their bitmap and text
                // label, if present. They look hideously ugly without autosizing
                // when the labels have even slightly different lengths.
                if ( HasFlag(wxTB_HORZ_LAYOUT) )
                {
                    button.fsStyle |= TBSTYLE_AUTOSIZE;
                }

                bitmapId++;
                break;
        }

        lastWasRadio = isRadio;

        i++;
    }

    if ( !::SendMessage(GetHwnd(), TB_ADDBUTTONS, i, (LPARAM)buttons.get()) )
    {
        wxLogLastError(wxT("TB_ADDBUTTONS"));
    }


    // Adjust controls and stretchable spaces
    // --------------------------------------

    // adjust the controls size to fit nicely in the toolbar and compute its
    // total size while doing it
    m_totalFixedSize = 0;
    int toolIndex = 0;
    for ( node = m_tools.GetFirst(); node; node = node->GetNext(), toolIndex++ )
    {
        wxToolBarTool * const tool = (wxToolBarTool*)node->GetData();

        const RECT r = wxGetTBItemRect(GetHwnd(), toolIndex, tool->GetId());

        if ( !tool->IsControl() )
        {
            if ( IsVertical() )
                m_totalFixedSize += r.bottom - r.top;
            else
                m_totalFixedSize += r.right - r.left;

            continue;
        }

        wxControl * const control = tool->GetControl();
        if ( IsVertical() )
        {
            // don't embed controls in the vertical toolbar, this doesn't look
            // good and wxGTK doesn't do it neither (and the code below can't
            // deal with this case)
            control->Hide();
            continue;
        }

        control->Show();
        wxStaticText * const staticText = tool->GetStaticText();

        wxSize size = control->GetSize();
        wxSize staticTextSize;
        if ( staticText && staticText->IsShown() )
        {
            staticTextSize = staticText->GetSize();
            staticTextSize.y += 3; // margin between control and its label
        }

        // position the control itself correctly vertically centering it on the
        // icon area of the toolbar
        int height = r.bottom - r.top - staticTextSize.y;

        int diff = height - size.y;
        if ( diff < 0 || !HasFlag(wxTB_TEXT) )
        {
            // not enough room for the static text
            if ( staticText )
                staticText->Hide();

            // recalculate height & diff without the staticText control
            height = r.bottom - r.top;
            diff = height - size.y;
            if ( diff < 0 )
            {
                // the control is too high, resize to fit
                // Actually don't set the size, otherwise we can never fit
                // the toolbar around the controls.
                // control->SetSize(wxDefaultCoord, height - 2);

                diff = 2;
            }
        }
        else // enough space for both the control and the label
        {
            if ( staticText )
                staticText->Show();
        }

        // Take also into account tool padding value.
        control->Move(r.left + m_toolPacking/2, r.top + (diff + 1) / 2);
        if ( staticText )
        {
            staticText->Move(r.left + m_toolPacking/2 + (size.x - staticTextSize.x)/2,
                             r.bottom - staticTextSize.y);
        }

        m_totalFixedSize += r.right - r.left;
    }

    // the max index is the "real" number of buttons - i.e. counting even the
    // separators which we added just for aligning the controls
    m_nButtons = toolIndex;

    if ( !IsVertical() )
    {
        if ( m_maxRows == 0 )
            // if not set yet, only one row
            SetRows(1);
    }
    else if ( m_nButtons > 0 ) // vertical non empty toolbar
    {
        // if not set yet, have one column
        m_maxRows = 1;
        SetRows(m_nButtons);
    }

    InvalidateBestSize();
    UpdateSize();

    if ( IsVertical() )
    {
        // For vertical toolbar heights of buttons are incorrect
        // unless TB_AUTOSIZE in invoked.
        // We need to recalculate fixed elements size again.
        m_totalFixedSize = 0;
        toolIndex = 0;
        for ( node = m_tools.GetFirst(); node; node = node->GetNext(), toolIndex++ )
        {
            wxToolBarTool * const tool = (wxToolBarTool*)node->GetData();
            if ( !tool->IsStretchableSpace() )
            {
                const RECT r = wxGetTBItemRect(GetHwnd(), toolIndex);
                if ( !IsVertical() )
                    m_totalFixedSize += r.right - r.left;
                else if ( !tool->IsControl() )
                    m_totalFixedSize += r.bottom - r.top;
            }
        }
        // Enforce invoking UpdateStretchableSpacersSize() with correct value of fixed elements size.
        UpdateSize();
    }

    return true;
}

void wxToolBar::UpdateStretchableSpacersSize()
{
    // check if we have any stretchable spacers in the first place
    unsigned numSpaces = 0;
    wxToolBarToolsList::compatibility_iterator node;
    int toolIndex = 0;
    for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
    {
        wxToolBarTool * const tool = (wxToolBarTool*)node->GetData();

        if ( tool->IsToBeDeleted() )
            continue;

        if ( tool->IsStretchableSpace() )
        {
            // Count only enabled items
            const RECT rcItem = wxGetTBItemRect(GetHwnd(), toolIndex);
            if ( !::IsRectEmpty(&rcItem) )
                numSpaces++;
        }

        toolIndex++;
    }

    if ( !numSpaces )
        return;

    // we do, adjust their size: either distribute the extra size among them or
    // reduce their size if there is not enough place for all tools
    const int totalSize = IsVertical() ? GetClientSize().y : GetClientSize().x;
    const int extraSize = totalSize - m_totalFixedSize;
    const int sizeSpacer = extraSize > 0 ? extraSize / numSpaces : 1;

    // the last spacer should consume all remaining space if we have too much
    // of it (which can be greater than sizeSpacer because of the rounding)
    const int sizeLastSpacer = extraSize > 0
                                ? extraSize - (numSpaces - 1)*sizeSpacer
                                : 1;

    // cumulated offset by which we need to move all the following controls to
    // the right: while the toolbar takes care of the normal items, we must
    // move the controls manually ourselves to ensure they remain at the
    // correct place
    int offset = 0;
    toolIndex = 0;
    for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
    {
        wxToolBarTool * const tool = (wxToolBarTool*)node->GetData();

        if ( tool->IsToBeDeleted() )
            continue;

        if ( tool->IsControl() && offset )
        {
            tool->MoveBy(offset);
            toolIndex++;
            continue;
        }

        if ( !tool->IsStretchableSpace() )
        {
            toolIndex++;
            continue;
        }

        const RECT rcOld = wxGetTBItemRect(GetHwnd(), toolIndex);

        const int oldSize = IsVertical()? (rcOld.bottom - rcOld.top): (rcOld.right - rcOld.left);
        const int newSize = --numSpaces ? sizeSpacer : sizeLastSpacer;
        if ( newSize != oldSize)
        {
            if ( !::SendMessage(GetHwnd(), TB_DELETEBUTTON, toolIndex, 0) )
            {
                wxLogLastError(wxT("TB_DELETEBUTTON (separator)"));
            }
            else
            {
                TBBUTTON button;
                wxZeroMemory(button);

                button.idCommand = tool->GetId();
                button.iBitmap = newSize; // set separator width/height
                button.fsState = TBSTATE_ENABLED;
                button.fsStyle = TBSTYLE_SEP;
                if ( IsVertical() )
                    button.fsState |= TBSTATE_WRAP;
                if ( !::SendMessage(GetHwnd(), TB_INSERTBUTTON, toolIndex, (LPARAM)&button) )
                {
                    wxLogLastError(wxT("TB_INSERTBUTTON (separator)"));
                }
                else
                {
                    // We successfully replaced this seprator, move all the controls after it
                    // by the corresponding amount (may be positive or negative)
                    offset += newSize - oldSize;
                }
            }
        }

        toolIndex++;
    }
}

// ----------------------------------------------------------------------------
// message handlers
// ----------------------------------------------------------------------------

bool wxToolBar::MSWCommand(WXUINT WXUNUSED(cmd), WXWORD id_)
{
    // cast to signed is important as we compare this id with (signed) ints in
    // FindById() and without the cast we'd get a positive int from a
    // "negative" (i.e. > 32767) WORD
    const int id = (signed short)id_;

    wxToolBarToolBase *tool = FindById(id);
    if ( !tool )
        return false;

    bool toggled = false; // just to suppress warnings

    LRESULT state = ::SendMessage(GetHwnd(), TB_GETSTATE, id, 0);

    if ( tool->CanBeToggled() )
    {
        toggled = (state & TBSTATE_CHECKED) != 0;

        // ignore the event when a radio button is released, as this doesn't
        // seem to happen at all, and is handled otherwise
        if ( tool->GetKind() == wxITEM_RADIO && !toggled )
            return true;

        tool->Toggle(toggled);
        UnToggleRadioGroup(tool);
    }

    // Without the two lines of code below, if the toolbar was repainted during
    // OnLeftClick(), then it could end up without the tool bitmap temporarily
    // (see http://lists.nongnu.org/archive/html/lmi/2008-10/msg00014.html).
    // The Update() call below ensures that this won't happen, by repainting
    // invalidated areas of the toolbar immediately.
    //
    // To complicate matters, the tool would be drawn in depressed state (this
    // code is called when mouse button is released, not pressed). That's not
    // ideal, having the tool pressed for the duration of OnLeftClick()
    // provides the user with useful visual clue that the app is busy reacting
    // to the event. So we manually put the tool into pressed state, handle the
    // event and then finally restore tool's original state.
    ::SendMessage(GetHwnd(), TB_SETSTATE, id, MAKELONG(state | TBSTATE_PRESSED, 0));
    Update();

    // Before calling the event handler, store a pointer to this toolbar in the
    // global variable: if it gets reset from our dtor, we will know that the
    // toolbar was destroyed by this handler and that we can't use this object
    // any more.
    gs_liveToolbar = this;

    bool allowLeftClick = OnLeftClick(id, toggled);

    if ( gs_liveToolbar != this )
    {
        // Bail out, we can't touch any member fields in the already
        // destroyed object anyhow.
        return true;
    }

    gs_liveToolbar = NULL;

    // Check if the tool hasn't been deleted in the event handler (notice that
    // it's also possible that this tool was deleted and a new tool with the
    // same ID was created, so we really need to check if the pointer to the
    // tool with the given ID didn't change, not just that it's non null).
    if ( FindById(id) != tool )
    {
        // The rest of this event handler deals with updating the tool and must
        // not be executed if the tool doesn't exist any more.
        return true;
    }

    // Restore the unpressed state. Enabled/toggled state might have been
    // changed since so take care of it.
    if (tool->IsEnabled())
        state |= TBSTATE_ENABLED;
    else
        state &= ~TBSTATE_ENABLED;
    if ( MSWShouldBeChecked(tool) )
        state |= TBSTATE_CHECKED;
    else
        state &= ~TBSTATE_CHECKED;
    ::SendMessage(GetHwnd(), TB_SETSTATE, id, MAKELONG(state, 0));

    // OnLeftClick() can veto the button state change - for buttons which
    // may be toggled only, of course.
    if ( !allowLeftClick && tool->CanBeToggled() )
    {
        // revert back
        tool->Toggle(!toggled);

        ::SendMessage(GetHwnd(), TB_CHECKBUTTON, id,
                      MAKELONG(MSWShouldBeChecked(tool), 0));
    }

    return true;
}

bool wxToolBar::MSWOnNotify(int WXUNUSED(idCtrl),
                            WXLPARAM lParam,
                            WXLPARAM *WXUNUSED(result))
{
    LPNMHDR hdr = (LPNMHDR)lParam;
    if ( hdr->code == TBN_DROPDOWN )
    {
        LPNMTOOLBAR tbhdr = (LPNMTOOLBAR)lParam;

        wxCommandEvent evt(wxEVT_TOOL_DROPDOWN, tbhdr->iItem);
        if ( HandleWindowEvent(evt) )
        {
            // Event got handled, don't display default popup menu
            return false;
        }

        const wxToolBarToolBase * const tool = FindById(tbhdr->iItem);
        wxCHECK_MSG( tool, false, wxT("drop down message for unknown tool") );

        wxMenu * const menu = tool->GetDropdownMenu();
        if ( !menu )
            return false;

        // Display popup menu below button
        const RECT r = wxGetTBItemRect(GetHwnd(), GetToolPos(tbhdr->iItem));
        if ( r.right )
            PopupMenu(menu, r.left, r.bottom);

        return true;
    }


    if( !HasFlag(wxTB_NO_TOOLTIPS) )
    {
#if wxUSE_TOOLTIPS
        // First check if this applies to us

        // the tooltips control created by the toolbar is sometimes Unicode, even
        // in an ANSI application - this seems to be a bug in comctl32.dll v5
        UINT code = hdr->code;
        if ( (code != (UINT) TTN_NEEDTEXTA) && (code != (UINT) TTN_NEEDTEXTW) )
            return false;

        HWND toolTipWnd = (HWND)::SendMessage(GetHwnd(), TB_GETTOOLTIPS, 0, 0);
        if ( toolTipWnd != hdr->hwndFrom )
            return false;

        LPTOOLTIPTEXT ttText = (LPTOOLTIPTEXT)lParam;
        int id = (int)ttText->hdr.idFrom;

        wxToolBarToolBase *tool = FindById(id);
        if ( tool )
            return HandleTooltipNotify(code, lParam, tool->GetShortHelp());
#else
        wxUnusedVar(lParam);
#endif
    }

    return false;
}

// ----------------------------------------------------------------------------
// toolbar geometry
// ----------------------------------------------------------------------------

void wxToolBar::SetToolBitmapSize(const wxSize& size)
{
    // Leave the effective size as (0, 0) if we are not showing bitmaps at all.
    wxSize effectiveSize;

    if ( !HasFlag(wxTB_NOICONS) )
        effectiveSize = size;

    wxToolBarBase::SetToolBitmapSize(size);

    ::SendMessage(GetHwnd(), TB_SETBITMAPSIZE, 0, MAKELONG(size.x, size.y));
}

void wxToolBar::SetRows(int nRows)
{
    if ( nRows == m_maxRows )
    {
        // avoid resizing the frame uselessly
        return;
    }

    // TRUE in wParam means to create at least as many rows, FALSE -
    // at most as many
    RECT rect;
    ::SendMessage(GetHwnd(), TB_SETROWS,
                  MAKEWPARAM(nRows, !(GetWindowStyle() & wxTB_VERTICAL)),
                  (LPARAM) &rect);

    m_maxRows = nRows;

    // Enable stretchable spacers only for single-row horizontal toobar or
    // single-column vertical toolbar, they don't work correctly when the extra
    // space can be redistributed among multiple columns or rows at any moment.
    const bool enable = (!IsVertical() && m_maxRows == 1) ||
                           (IsVertical() && (size_t)m_maxRows == m_nButtons);

    const LPARAM state = MAKELONG(enable ? TBSTATE_ENABLED : TBSTATE_HIDDEN, 0);
    wxToolBarToolsList::compatibility_iterator node;
    for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
    {
        wxToolBarTool * const tool = (wxToolBarTool*)node->GetData();
        if ( tool->IsStretchableSpace() )
        {
            if ( !::SendMessage(GetHwnd(), TB_SETSTATE, tool->GetId(), state) )
            {
                wxLogLastError(wxT("TB_SETSTATE (stretchable spacer)"));
            }
        }
    }

    UpdateSize();
}

// The button size is bigger than the bitmap size
wxSize wxToolBar::GetToolSize() const
{
    DWORD dw = ::SendMessage(GetHwnd(), TB_GETBUTTONSIZE, 0, 0);

    return wxSize(LOWORD(dw), HIWORD(dw));
}

wxToolBarToolBase *wxToolBar::FindToolForPosition(wxCoord x, wxCoord y) const
{
    POINT pt;
    pt.x = x;
    pt.y = y;
    int index = (int)::SendMessage(GetHwnd(), TB_HITTEST, 0, (LPARAM)&pt);

    // MBN: when the point ( x, y ) is close to the toolbar border
    //      TB_HITTEST returns m_nButtons ( not -1 )
    if ( index < 0 || (size_t)index >= m_nButtons )
        // it's a separator or there is no tool at all there
        return NULL;

    return m_tools.Item((size_t)index)->GetData();
}

void wxToolBar::UpdateSize()
{
    wxPoint pos = GetPosition();
    ::SendMessage(GetHwnd(), TB_AUTOSIZE, 0, 0);
    if (pos != GetPosition())
        Move(pos);

    // In case Realize is called after the initial display (IOW the programmer
    // may have rebuilt the toolbar) give the frame the option of resizing the
    // toolbar to full width again, but only if the parent is a frame and the
    // toolbar is managed by the frame.  Otherwise assume that some other
    // layout mechanism is controlling the toolbar size and leave it alone.
    SendSizeEventToParent();
}

// ----------------------------------------------------------------------------
// toolbar styles
// ---------------------------------------------------------------------------

// get the TBSTYLE of the given toolbar window
long wxToolBar::GetMSWToolbarStyle() const
{
    return ::SendMessage(GetHwnd(), TB_GETSTYLE, 0, 0L);
}

void wxToolBar::SetWindowStyleFlag(long style)
{
    // the style bits whose changes force us to recreate the toolbar
    static const long MASK_NEEDS_RECREATE = wxTB_TEXT | wxTB_NOICONS;

    const long styleOld = GetWindowStyle();

    wxToolBarBase::SetWindowStyleFlag(style);

    // don't recreate an empty toolbar: not only this is unnecessary, but it is
    // also fatal as we'd then try to recreate the toolbar when it's just being
    // created
    if ( GetToolsCount() &&
            (style & MASK_NEEDS_RECREATE) != (styleOld & MASK_NEEDS_RECREATE) )
    {
        // to remove the text labels, simply re-realizing the toolbar is enough
        // but I don't know of any way to add the text to an existing toolbar
        // other than by recreating it entirely
        Recreate();
    }
}

// ----------------------------------------------------------------------------
// tool state
// ----------------------------------------------------------------------------

void wxToolBar::DoEnableTool(wxToolBarToolBase *tool, bool enable)
{
    if ( tool->IsButton() )
    {
        ::SendMessage(GetHwnd(), TB_ENABLEBUTTON,
                      (WPARAM)tool->GetId(), (LPARAM)MAKELONG(enable, 0));

        // Adjust displayed checked state -- it could have changed if the tool is
        // disabled and has a custom "disabled state" bitmap.
        DoToggleTool(tool, tool->IsToggled());
    }
    else if ( tool->IsControl() )
    {
        wxToolBarTool* tbTool = static_cast<wxToolBarTool*>(tool);

        tbTool->GetControl()->Enable(enable);
        wxStaticText* text = tbTool->GetStaticText();
        if ( text )
            text->Enable(enable);
    }
}

void wxToolBar::DoToggleTool(wxToolBarToolBase *tool,
                             bool WXUNUSED_UNLESS_DEBUG(toggle))
{
    wxASSERT_MSG( tool->IsToggled() == toggle, wxT("Inconsistent tool state") );

    ::SendMessage(GetHwnd(), TB_CHECKBUTTON,
                  (WPARAM)tool->GetId(),
                  (LPARAM)MAKELONG(MSWShouldBeChecked(tool), 0));
}

void wxToolBar::DoSetToggle(wxToolBarToolBase *WXUNUSED(tool), bool WXUNUSED(toggle))
{
    // VZ: AFAIK, the button has to be created either with TBSTYLE_CHECK or
    //     without, so we really need to delete the button and recreate it here
    wxFAIL_MSG( wxT("not implemented") );
}

void wxToolBar::SetToolNormalBitmap( int id, const wxBitmap& bitmap )
{
    wxToolBarTool* tool = static_cast<wxToolBarTool*>(FindById(id));
    if ( tool )
    {
        wxCHECK_RET( tool->IsButton(), wxT("Can only set bitmap on button tools."));

        tool->SetNormalBitmap(bitmap);
        Realize();
    }
}

void wxToolBar::SetToolDisabledBitmap( int id, const wxBitmap& bitmap )
{
    wxToolBarTool* tool = static_cast<wxToolBarTool*>(FindById(id));
    if ( tool )
    {
        wxCHECK_RET( tool->IsButton(), wxT("Can only set bitmap on button tools."));

        tool->SetDisabledBitmap(bitmap);
        Realize();
    }
}

void wxToolBar::SetToolPacking(int packing)
{
    if ( packing > 0 && packing != m_toolPacking )
    {
        m_toolPacking = packing;
        if ( GetHwnd() )
        {
            MSWSetPadding(packing);
            Realize();
        }
    }
}

// ----------------------------------------------------------------------------
// event handlers
// ----------------------------------------------------------------------------

// Responds to colour changes, and passes event on to children.
void wxToolBar::OnSysColourChanged(wxSysColourChangedEvent& event)
{
    wxRGBToColour(m_backgroundColour, ::GetSysColor(COLOR_BTNFACE));

    // Remap the buttons
    Realize();

    // Relayout the toolbar
    int nrows = m_maxRows;
    m_maxRows = 0;      // otherwise SetRows() wouldn't do anything
    SetRows(nrows);

    Refresh();

    // let the event propagate further
    event.Skip();
}

void wxToolBar::OnMouseEvent(wxMouseEvent& event)
{
    if ( event.Leaving() )
    {
        if ( m_pInTool )
        {
            OnMouseEnter(wxID_ANY);
            m_pInTool = NULL;
        }

        event.Skip();
        return;
    }

    if ( event.RightDown() )
    {
        // find the tool under the mouse
        wxCoord x = 0, y = 0;
        event.GetPosition(&x, &y);

        wxToolBarToolBase *tool = FindToolForPosition(x, y);
        OnRightClick(tool ? tool->GetId() : -1, x, y);
    }
    else
    {
        event.Skip();
    }
}

// This handler is needed to fix problems with painting the background of
// toolbar icons with comctl32.dll < 6.0.
void wxToolBar::OnEraseBackground(wxEraseEvent& event)
{
#ifdef wxHAS_MSW_BACKGROUND_ERASE_HOOK
    MSWDoEraseBackground(event.GetDC()->GetHDC());
#endif // wxHAS_MSW_BACKGROUND_ERASE_HOOK
}

bool wxToolBar::HandleSize(WXWPARAM WXUNUSED(wParam), WXLPARAM lParam)
{
    // wait until we have some tools
    const int toolsCount = GetToolsCount();
    if ( toolsCount == 0 )
        return false;

    // calculate our minor dimension ourselves - we're confusing the standard
    // logic (TB_AUTOSIZE) with our horizontal toolbars and other hacks
    // Find bounding box for all rows.
    RECT r;
    ::SetRectEmpty(&r);
    // Bounding box for single (current) row
    RECT rcRow;
    ::SetRectEmpty(&rcRow);
    int rowPosX = INT_MIN;
    wxToolBarToolsList::compatibility_iterator node;
    int i = 0;
    for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
    {
        wxToolBarTool * const
            tool = static_cast<wxToolBarTool *>(node->GetData());
        if ( tool->IsToBeDeleted() )
            continue;

        // Skip hidden buttons
        const RECT rcItem = wxGetTBItemRect(GetHwnd(), i);
        if ( ::IsRectEmpty(&rcItem) )
        {
            i++;
            continue;
        }

        if ( rcItem.top > rowPosX )
        {
            // We have the next row.
            rowPosX = rcItem.top;

            // Shift origin to (0, 0) to make it the same as for the total rect.
            ::OffsetRect(&rcRow, -rcRow.left, -rcRow.top);

            // And update the bounding box for all rows.
            ::UnionRect(&r, &r, &rcRow);

            // Reset the current row bounding box for the next row.
            ::SetRectEmpty(&rcRow);
        }

        // Separators shouldn't be taken into account as they are sometimes
        // reported to have the width of the entire client area by the toolbar.
        // And we know that they are not the biggest items in the toolbar in
        // any case, so just skip them.
        if( !tool->IsSeparator() )
        {
            // Update bounding box of current row
            ::UnionRect(&rcRow, &rcRow, &rcItem);
        }

        i++;
    }

    // Take into account the last row rectangle too.
    ::OffsetRect(&rcRow, -rcRow.left, -rcRow.top);
    ::UnionRect(&r, &r, &rcRow);

    if ( !r.right )
        return false;

    int w, h;

    if ( IsVertical() )
    {
        w = r.right - r.left;
        h = HIWORD(lParam);
    }
    else
    {
        w = LOWORD(lParam);
        if (HasFlag( wxTB_FLAT ))
            h = r.bottom - r.top - 3;
        else
            h = r.bottom - r.top;

        // Take control height into account
        for ( node = m_tools.GetFirst(); node; node = node->GetNext() )
        {
            wxToolBarTool * const
                tool = static_cast<wxToolBarTool *>(node->GetData());
            if (tool->IsControl())
            {
                int y = (tool->GetControl()->GetSize().y - 2); // -2 since otherwise control height + 4 (below) is too much
                if (y > h)
                    h = y;
            }
        }

        if ( m_maxRows )
        {
            // FIXME: hardcoded separator line height...
            h += HasFlag(wxTB_NODIVIDER) ? 4 : 6;
            h *= m_maxRows;
        }
    }

    if ( MAKELPARAM(w, h) != lParam )
    {
        // size really changed
        SetSize(w, h);
    }

    UpdateStretchableSpacersSize();

    // message processed
    return true;
}

#ifdef wxHAS_MSW_BACKGROUND_ERASE_HOOK

bool wxToolBar::HandlePaint(WXWPARAM wParam, WXLPARAM lParam)
{
    // we must prevent the dummy separators corresponding to controls or
    // stretchable spaces from being seen: we used to do it by painting over
    // them but this, unsurprisingly, resulted in a lot of flicker so now we
    // prevent the toolbar from painting them at all

    // compute the region containing all dummy separators which we don't want
    // to be seen
    wxRegion rgnDummySeps;
    const wxRect rectTotal = GetClientRect();
    int toolIndex = 0;
    for ( wxToolBarToolsList::compatibility_iterator node = m_tools.GetFirst();
          node;
          node = node->GetNext(), toolIndex++ )
    {
        wxToolBarTool * const
            tool = static_cast<wxToolBarTool *>(node->GetData());

        if ( tool->IsToBeDeleted() )
            continue;

        if ( tool->IsControl() || tool->IsStretchableSpace() )
        {
            // for some reason TB_GETITEMRECT returns a rectangle 1 pixel
            // shorter than the full window size (at least under Windows 7)
            // but we need to erase the full width/height below
            RECT rcItem = wxGetTBItemRect(GetHwnd(), toolIndex);

            // Skip hidden buttons
            if ( ::IsRectEmpty(&rcItem) )
                continue;

            if ( IsVertical() )
            {
                rcItem.left = 0;
                rcItem.right = rectTotal.width;
            }
            else
            {
                rcItem.bottom = rcItem.top + rectTotal.height / m_maxRows;
            }

            // Apparently, regions of height < 3 are not taken into account
            // in clipping so we need to extend them for this purpose.
            if ( rcItem.bottom - rcItem.top > 0 && rcItem.bottom - rcItem.top < 3 )
                rcItem.bottom = rcItem.top + 3;

            rgnDummySeps.Union(wxRectFromRECT(rcItem));
        }
    }

    if ( rgnDummySeps.IsOk() )
    {
        // exclude the area occupied by the controls and stretchable spaces
        // from the update region to prevent the toolbar from drawing
        // separators in it
        if ( !::ValidateRgn(GetHwnd(), GetHrgnOf(rgnDummySeps)) )
        {
            wxLogLastError(wxT("ValidateRgn()"));
        }
    }

    // still let the native control draw everything else normally but set up a
    // hook to be able to process the next WM_ERASEBKGND sent to our parent
    // because toolbar will ask it to erase its background from its WM_PAINT
    // handler (when using TBSTYLE_TRANSPARENT which we do always use)
    //
    // installing hook is not completely trivial as all kinds of strange
    // situations are possible: sometimes we can be called recursively from
    // inside the native toolbar WM_PAINT handler so the hook might already be
    // installed and sometimes the native toolbar might not send WM_ERASEBKGND
    // to the parent at all for whatever reason, so deal with all these cases
    wxWindow * const parent = GetParent();
    const bool hadHook = parent->MSWHasEraseBgHook();
    if ( !hadHook )
        GetParent()->MSWSetEraseBgHook(this);

    MSWDefWindowProc(WM_PAINT, wParam, lParam);

    if ( !hadHook )
        GetParent()->MSWSetEraseBgHook(NULL);


    if ( rgnDummySeps.IsOk() )
    {
        // erase the dummy separators region ourselves now as nobody painted
        // over them
        WindowHDC hdc(GetHwnd());
        ::SelectClipRgn(hdc, GetHrgnOf(rgnDummySeps));
        MSWDoEraseBackground(hdc);
    }

    return true;
}

WXHBRUSH wxToolBar::MSWGetToolbarBgBrush()
{
    // we conservatively use a solid brush here but we could also use a themed
    // brush by using DrawThemeBackground() to create a bitmap brush (it'd need
    // to be invalidated whenever the toolbar is resized and, also, correctly
    // aligned using SetBrushOrgEx() before each use -- there is code for doing
    // this in wxNotebook already so it'd need to be refactored into wxWindow)
    //
    // however inasmuch as there is a default background for the toolbar at all
    // (and this is not a trivial question as different applications use very
    // different colours), it seems to be a solid one and using REBAR
    // background brush as we used to do before doesn't look good at all under
    // Windows 7 (and probably Vista too), so for now we just keep it simple
    wxColour const
        colBg = m_hasBgCol ? GetBackgroundColour()
                           : wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
    wxBrush * const
        brush = wxTheBrushList->FindOrCreateBrush(colBg);

    return brush ? static_cast<WXHBRUSH>(brush->GetResourceHandle()) : 0;
}

WXHBRUSH wxToolBar::MSWGetBgBrushForChild(WXHDC hDC, wxWindowMSW *child)
{
    WXHBRUSH hbr = wxToolBarBase::MSWGetBgBrushForChild(hDC, child);
    if ( hbr )
        return hbr;

    // the base class version only returns a brush for erasing children
    // background if we have a non-default background colour but as the toolbar
    // doesn't erase its own background by default, we need to always do it for
    // (semi-)transparent children
    if ( child->GetParent() == this && child->HasTransparentBackground() )
        return MSWGetToolbarBgBrush();

    return 0;
}

void wxToolBar::MSWDoEraseBackground(WXHDC hDC)
{
    wxFillRect(GetHwnd(), (HDC)hDC, (HBRUSH)MSWGetToolbarBgBrush());
}

bool wxToolBar::MSWEraseBgHook(WXHDC hDC)
{
    // toolbar WM_PAINT handler offsets the DC origin before sending
    // WM_ERASEBKGND to the parent but as we handle it in the toolbar itself,
    // we need to reset it back
    HDC hdc = (HDC)hDC;
    POINT ptOldOrg;
    if ( !::SetWindowOrgEx(hdc, 0, 0, &ptOldOrg) )
    {
        wxLogLastError(wxT("SetWindowOrgEx(tbar-bg-hdc)"));
        return false;
    }

    MSWDoEraseBackground(hDC);

    ::SetWindowOrgEx(hdc, ptOldOrg.x, ptOldOrg.y, NULL);

    return true;
}

#endif // wxHAS_MSW_BACKGROUND_ERASE_HOOK

void wxToolBar::HandleMouseMove(WXWPARAM WXUNUSED(wParam), WXLPARAM lParam)
{
    wxCoord x = GET_X_LPARAM(lParam),
            y = GET_Y_LPARAM(lParam);
    wxToolBarToolBase* tool = FindToolForPosition( x, y );

    // has the current tool changed?
    if ( tool != m_pInTool )
    {
        m_pInTool = tool;
        OnMouseEnter(tool ? tool->GetId() : wxID_ANY);
    }
}

WXLRESULT wxToolBar::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
    switch ( nMsg )
    {
        case WM_MOUSEMOVE:
            // we don't handle mouse moves, so always pass the message to
            // wxControl::MSWWindowProc (HandleMouseMove just calls OnMouseEnter)
            HandleMouseMove(wParam, lParam);
            break;

        case WM_SIZE:
            if ( HandleSize(wParam, lParam) )
                return 0;
            break;

#ifdef wxHAS_MSW_BACKGROUND_ERASE_HOOK
        case WM_PAINT:
            if ( HandlePaint(wParam, lParam) )
                return 0;
            break;
#endif // wxHAS_MSW_BACKGROUND_ERASE_HOOK

        case WM_PRINTCLIENT:
            wxFillRect(GetHwnd(), (HDC)wParam, MSWGetToolbarBgBrush());
            return 1;
    }

    return wxControl::MSWWindowProc(nMsg, wParam, lParam);
}

// ----------------------------------------------------------------------------
// private functions
// ----------------------------------------------------------------------------

WXHBITMAP wxToolBar::MapBitmap(WXHBITMAP bitmap, int width, int height)
{
    MemoryHDC hdcMem;

    if ( !hdcMem )
    {
        wxLogLastError(wxT("CreateCompatibleDC"));

        return bitmap;
    }

    SelectInHDC bmpInHDC(hdcMem, (HBITMAP)bitmap);

    if ( !bmpInHDC )
    {
        wxLogLastError(wxT("SelectObject"));

        return bitmap;
    }

    wxCOLORMAP *cmap = wxGetStdColourMap();

    for ( int i = 0; i < width; i++ )
    {
        for ( int j = 0; j < height; j++ )
        {
            COLORREF pixel = ::GetPixel(hdcMem, i, j);

            for ( size_t k = 0; k < wxSTD_COL_MAX; k++ )
            {
                COLORREF col = cmap[k].from;
                if ( abs(GetRValue(pixel) - GetRValue(col)) < 10 &&
                     abs(GetGValue(pixel) - GetGValue(col)) < 10 &&
                     abs(GetBValue(pixel) - GetBValue(col)) < 10 )
                {
                    if ( cmap[k].to != pixel )
                        ::SetPixel(hdcMem, i, j, cmap[k].to);
                    break;
                }
            }
        }
    }

    return bitmap;
}

#endif // wxUSE_TOOLBAR
