//
//  BackedPanel.cpp
//  Audacity
//
//  Created by Paul Licameli on 5/7/16.
//
//


#include "BackedPanel.h"

BackedPanel::BackedPanel(wxWindow * parent, wxWindowID id,
            const wxPoint & pos,
            const wxSize & size,
            long style)
: wxPanelWrapper(parent, id, pos, size, style)
, mBacking{ std::make_unique<wxBitmap>(1, 1, 24) }
{
   // Preinit the backing DC and bitmap so routines that require it will
   // not cause a crash if they run before the panel is fully initialized.
   mBackingDC.SelectObject(*mBacking);
}

BackedPanel::~BackedPanel()
{
   if (mBacking)
      mBackingDC.SelectObject( wxNullBitmap );
}

wxDC &BackedPanel::GetBackingDC()
{
   return mBackingDC;
}

wxDC &BackedPanel::GetBackingDCForRepaint()
{
   if (mResizeBacking)
   {
      // Reset
      mResizeBacking = false;

      ResizeBacking();
   }

   return mBackingDC;
}

void BackedPanel::ResizeBacking()
{
   if (mBacking)
      mBackingDC.SelectObject(wxNullBitmap);

   wxSize sz = GetClientSize();
   const double scale = GetContentScaleFactor();
   mBacking = std::make_unique<wxBitmap>();
   // Bug 2040 - Avoid 0 x 0 bitmap when minimized.
   // Create the backing store at physical (device) resolution by tagging it
   // with the window's content scale factor.  Otherwise the whole TrackPanel
   // is drawn into a 1x bitmap and then stretched to the Retina backing store,
   // which makes text (track names, clip titles) blurry on HiDPI displays.
   // This mirrors how wxBufferedDC builds its own back-buffer (dcbufcmn.cpp).
   mBacking->CreateScaled(std::max(sz.x,1), std::max(sz.y,1), 24, scale);
   mBackingDC.SelectObject(*mBacking);
}

void BackedPanel::RepairBitmap(wxDC &dc, wxCoord x, wxCoord y, wxCoord width, wxCoord height)
{
   dc.Blit(x, y, width, height, &mBackingDC, x, y);
}

void BackedPanel::DisplayBitmap(wxDC &dc)
{
   if( mBacking )
   {
      // The backing bitmap is stored at physical resolution (see
      // ResizeBacking), so blit using its logical size; the copy then maps 1:1
      // onto the equally-scaled target DC.
      const wxSize logical = mBacking->GetScaledSize();
      RepairBitmap(dc, 0, 0, logical.GetWidth(), logical.GetHeight());
   }
}

void BackedPanel::OnSize(wxSizeEvent & event)
{
   // Tell OnPaint() to recreate the backing bitmap
   mResizeBacking = true;
   event.Skip();
   // Refresh the entire area.  Really only need to refresh when
   // expanding...is it worth the trouble?
   Refresh();
}

BEGIN_EVENT_TABLE(BackedPanel, wxPanelWrapper)
   EVT_SIZE(BackedPanel::OnSize)
END_EVENT_TABLE()

