(*
    Copyright (c) 2001, 2015
        David C.J. Matthews

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License version 2.1 as published by the Free Software Foundation.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*)

structure Brush:
  sig
    type HBITMAP and HBRUSH and HDC

    datatype
      HatchStyle =
          HS_BDIAGONAL
        | HS_CROSS
        | HS_DIAGCROSS
        | HS_FDIAGONAL
        | HS_HORIZONTAL
        | HS_VERTICAL

    datatype
      BrushStyle =
          BS_HATCHED of HatchStyle
        | BS_HOLLOW
        | BS_PATTERN of HBITMAP
        | BS_SOLID

    type COLORREF = Color.COLORREF

    type LOGBRUSH = BrushStyle * COLORREF
    type POINT = {x: int, y: int}
    type RasterOpCode = Bitmap.RasterOpCode

    datatype ColorType =
        COLOR_SCROLLBAR
    |   COLOR_BACKGROUND
    |   COLOR_ACTIVECAPTION
    |   COLOR_INACTIVECAPTION
    |   COLOR_MENU
    |   COLOR_WINDOW
    |   COLOR_WINDOWFRAME
    |   COLOR_MENUTEXT
    |   COLOR_WINDOWTEXT
    |   COLOR_CAPTIONTEXT
    |   COLOR_ACTIVEBORDER
    |   COLOR_INACTIVEBORDER
    |   COLOR_APPWORKSPACE
    |   COLOR_HIGHLIGHT
    |   COLOR_HIGHLIGHTTEXT
    |   COLOR_BTNFACE
    |   COLOR_BTNSHADOW
    |   COLOR_GRAYTEXT
    |   COLOR_BTNTEXT
    |   COLOR_INACTIVECAPTIONTEXT
    |   COLOR_BTNHIGHLIGHT
    |   COLOR_3DDKSHADOW
    |   COLOR_3DLIGHT
    |   COLOR_INFOTEXT
    |   COLOR_INFOBK

    val CreateBrushIndirect : LOGBRUSH -> HBRUSH
    val CreateHatchBrush : HatchStyle * COLORREF -> HBRUSH
    val CreatePatternBrush : HBITMAP -> HBRUSH
    val CreateSolidBrush : COLORREF -> HBRUSH
    val GetSysColorBrush : ColorType -> HBRUSH
    val GetBrushOrgEx : HDC -> POINT
    val PatBlt : HDC * int * int * int * int * RasterOpCode -> unit
    val SetBrushOrgEx : HDC * POINT -> POINT

  end =
struct
    local
        open Foreign Base
(*
        fun gdicall_IW name CR (C1,C2) (a1) =
            let val (from1,to1,ctype1) = breakConversion C1
                val (from2,to2,ctype2) = breakConversion C2
                val (fromR,toR,ctypeR) = breakConversion CR
                val va1 = to1 a1
                val va2 = address (alloc 1 ctype2)
                val res = callgdi name [(ctype1,va1),(Cpointer ctype2,va2)] ctypeR
                val _: unit = fromR res
            in  (from2 (deref va2))
            end
        fun gdicall_IM name CR (C1,C2) (a1,a2) =
            let val (from1,to1,ctype1) = breakConversion C1
                val (from2,to2,ctype2) = breakConversion C2
                val (fromR,toR,ctypeR) = breakConversion CR
                val va1 = to1 a1
                val va2 = address (to2 a2)
                val res = callgdi name [(ctype1,va1),(Cpointer ctype2,va2)] ctypeR
                val _ : unit = fromR res
            in  from2 (deref va2)
            end

        val XCOORD = INT : int Conversion
        val YCOORD = INT: int Conversion
        val WIDTH = INT: int Conversion
        val HEIGHT = INT: int Conversion*)

    in
        type HBRUSH = HBRUSH and COLORREF = Color.COLORREF and HBITMAP = HBITMAP
        and HDC = HDC and POINT = POINT

        open GdiBase


        (* BRUSHES *)
        val CreateBrushIndirect = winCall1 (user "CreateBrushIndirect") (cConstStar cLOGBRUSH) cHBRUSH
        and CreateHatchBrush = winCall2 (gdi "CreateHatchBrush") (cHATCHSTYLE, cCOLORREF) cHBRUSH
        and CreateSolidBrush = winCall1 (gdi "CreateSolidBrush") (cCOLORREF) cHBRUSH
        
        local
            val getBrushOrgEx =
                winCall2 (gdi "GetBrushOrgEx") (cHDC, cStar cPoint) (successState "GetBrushOrgEx")
            and setBrushOrgEx =
                winCall4 (gdi "SetBrushOrgEx")(cHDC, cInt, cInt, cStar cPoint) (successState "SetBrushOrgEx")
        in
            fun GetBrushOrgEx hdc = let val v = ref{x=0, y=0} in getBrushOrgEx(hdc, v); !v end
            and SetBrushOrgEx(hdc, {x, y}) = let val v = ref{x=0, y=0} in setBrushOrgEx(hdc, x, y, v); !v end
        end
        val CreatePatternBrush         = winCall1 (gdi "CreatePatternBrush") (cHBITMAP) cHBRUSH
        val PatBlt                     = winCall6(gdi "PatBlt") (cHDC,cInt,cInt,cInt,cInt,cRASTEROPCODE)
                                            (successState "PatBlt")
        datatype ColorType =
            COLOR_SCROLLBAR
        |   COLOR_BACKGROUND
        |   COLOR_ACTIVECAPTION
        |   COLOR_INACTIVECAPTION
        |   COLOR_MENU
        |   COLOR_WINDOW
        |   COLOR_WINDOWFRAME
        |   COLOR_MENUTEXT
        |   COLOR_WINDOWTEXT
        |   COLOR_CAPTIONTEXT
        |   COLOR_ACTIVEBORDER
        |   COLOR_INACTIVEBORDER
        |   COLOR_APPWORKSPACE
        |   COLOR_HIGHLIGHT
        |   COLOR_HIGHLIGHTTEXT
        |   COLOR_BTNFACE
        |   COLOR_BTNSHADOW
        |   COLOR_GRAYTEXT
        |   COLOR_BTNTEXT
        |   COLOR_INACTIVECAPTIONTEXT
        |   COLOR_BTNHIGHLIGHT
        |   COLOR_3DDKSHADOW
        |   COLOR_3DLIGHT
        |   COLOR_INFOTEXT
        |   COLOR_INFOBK

        fun colourTypeToInt COLOR_SCROLLBAR = 0
         |  colourTypeToInt COLOR_BACKGROUND = 1
         |  colourTypeToInt COLOR_ACTIVECAPTION = 2
         |  colourTypeToInt COLOR_INACTIVECAPTION = 3
         |  colourTypeToInt COLOR_MENU = 4
         |  colourTypeToInt COLOR_WINDOW = 5
         |  colourTypeToInt COLOR_WINDOWFRAME = 6
         |  colourTypeToInt COLOR_MENUTEXT = 7
         |  colourTypeToInt COLOR_WINDOWTEXT = 8
         |  colourTypeToInt COLOR_CAPTIONTEXT = 9
         |  colourTypeToInt COLOR_ACTIVEBORDER = 10
         |  colourTypeToInt COLOR_INACTIVEBORDER = 11
         |  colourTypeToInt COLOR_APPWORKSPACE = 12
         |  colourTypeToInt COLOR_HIGHLIGHT = 13
         |  colourTypeToInt COLOR_HIGHLIGHTTEXT = 14
         |  colourTypeToInt COLOR_BTNFACE = 15
         |  colourTypeToInt COLOR_BTNSHADOW = 16
         |  colourTypeToInt COLOR_GRAYTEXT = 17
         |  colourTypeToInt COLOR_BTNTEXT = 18
         |  colourTypeToInt COLOR_INACTIVECAPTIONTEXT = 19
         |  colourTypeToInt COLOR_BTNHIGHLIGHT = 20
         |  colourTypeToInt COLOR_3DDKSHADOW = 21
         |  colourTypeToInt COLOR_3DLIGHT = 22
         |  colourTypeToInt COLOR_INFOTEXT = 23
         |  colourTypeToInt COLOR_INFOBK = 24
    
        (* Create a brush from a system colour. *)
        val GetSysColorBrush = winCall1 (user "GetSysColorBrush") (cInt) cHBRUSH o colourTypeToInt

        (*
            Other Brush functions:
                CreateDIBPatternBrushPt  
        *)
    end
end;
