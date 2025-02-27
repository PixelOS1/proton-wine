/*
 * USER Input processing
 *
 * Copyright 1993 Bob Amstadt
 * Copyright 1996 Albrecht Kleine
 * Copyright 1997 David Faure
 * Copyright 1998 Morten Welinder
 * Copyright 1998 Ulrich Weigand
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>

#define NONAMELESSUNION

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winnls.h"
#include "winternl.h"
#include "winerror.h"
#include "win.h"
#include "user_private.h"
#include "dbt.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(win);
WINE_DECLARE_DEBUG_CHANNEL(keyboard);

/***********************************************************************
 *           get_key_state
 */
static WORD get_key_state(void)
{
    WORD ret = 0;

    if (GetSystemMetrics( SM_SWAPBUTTON ))
    {
        if (GetAsyncKeyState(VK_RBUTTON) & 0x80) ret |= MK_LBUTTON;
        if (GetAsyncKeyState(VK_LBUTTON) & 0x80) ret |= MK_RBUTTON;
    }
    else
    {
        if (GetAsyncKeyState(VK_LBUTTON) & 0x80) ret |= MK_LBUTTON;
        if (GetAsyncKeyState(VK_RBUTTON) & 0x80) ret |= MK_RBUTTON;
    }
    if (GetAsyncKeyState(VK_MBUTTON) & 0x80)  ret |= MK_MBUTTON;
    if (GetAsyncKeyState(VK_SHIFT) & 0x80)    ret |= MK_SHIFT;
    if (GetAsyncKeyState(VK_CONTROL) & 0x80)  ret |= MK_CONTROL;
    if (GetAsyncKeyState(VK_XBUTTON1) & 0x80) ret |= MK_XBUTTON1;
    if (GetAsyncKeyState(VK_XBUTTON2) & 0x80) ret |= MK_XBUTTON2;
    return ret;
}


/***********************************************************************
 *           get_locale_kbd_layout
 */
static HKL get_locale_kbd_layout(void)
{
    ULONG_PTR layout;
    LANGID langid;

    /* FIXME:
     *
     * layout = main_key_tab[kbd_layout].lcid;
     *
     * Winword uses return value of GetKeyboardLayout as a codepage
     * to translate ANSI keyboard messages to unicode. But we have
     * a problem with it: for instance Polish keyboard layout is
     * identical to the US one, and therefore instead of the Polish
     * locale id we return the US one.
     */

    layout = GetUserDefaultLCID();

    /*
     * Microsoft Office expects this value to be something specific
     * for Japanese and Korean Windows with an IME the value is 0xe001
     * We should probably check to see if an IME exists and if so then
     * set this word properly.
     */
    langid = PRIMARYLANGID( LANGIDFROMLCID( layout ) );
    if (langid == LANG_CHINESE || langid == LANG_JAPANESE || langid == LANG_KOREAN)
        layout = MAKELONG( layout, 0xe001 ); /* IME */
    else
        layout = MAKELONG( layout, layout );

    return (HKL)layout;
}


/**********************************************************************
 *		set_capture_window
 */
BOOL set_capture_window( HWND hwnd, UINT gui_flags, HWND *prev_ret )
{
    HWND previous = 0;
    UINT flags = 0;
    BOOL ret;

    if (gui_flags & GUI_INMENUMODE) flags |= CAPTURE_MENU;
    if (gui_flags & GUI_INMOVESIZE) flags |= CAPTURE_MOVESIZE;

    SERVER_START_REQ( set_capture_window )
    {
        req->handle = wine_server_user_handle( hwnd );
        req->flags  = flags;
        if ((ret = !wine_server_call_err( req )))
        {
            previous = wine_server_ptr_handle( reply->previous );
            hwnd = wine_server_ptr_handle( reply->full_handle );
        }
    }
    SERVER_END_REQ;

    if (ret)
    {
        USER_Driver->pSetCapture( hwnd, gui_flags );

        if (previous)
            SendMessageW( previous, WM_CAPTURECHANGED, 0, (LPARAM)hwnd );

        if (prev_ret) *prev_ret = previous;
    }
    return ret;
}


/***********************************************************************
 *		__wine_send_input  (USER32.@)
 *
 * Internal SendInput function to allow the graphics driver to inject real events.
 */
BOOL CDECL __wine_send_input( HWND hwnd, const INPUT *input, const RAWINPUT *rawinput )
{
    NTSTATUS status = send_hardware_message( hwnd, input, rawinput, 0 );
    if (status) SetLastError( RtlNtStatusToDosError(status) );
    return !status;
}


/***********************************************************************
 *		update_mouse_coords
 *
 * Helper for SendInput.
 */
static void update_mouse_coords( INPUT *input )
{
    if (!(input->u.mi.dwFlags & MOUSEEVENTF_MOVE)) return;

    if (input->u.mi.dwFlags & MOUSEEVENTF_ABSOLUTE)
    {
        DPI_AWARENESS_CONTEXT context = SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE );
        RECT rc;

        if (input->u.mi.dwFlags & MOUSEEVENTF_VIRTUALDESK)
            rc = get_virtual_screen_rect();
        else
            rc = get_primary_monitor_rect();

        input->u.mi.dx = rc.left + ((input->u.mi.dx * (rc.right - rc.left)) >> 16);
        input->u.mi.dy = rc.top  + ((input->u.mi.dy * (rc.bottom - rc.top)) >> 16);
        SetThreadDpiAwarenessContext( context );
    }
    else
    {
        int accel[3];

        /* dx and dy can be negative numbers for relative movements */
        SystemParametersInfoW(SPI_GETMOUSE, 0, accel, 0);

        if (!accel[2]) return;

        if (abs(input->u.mi.dx) > accel[0])
        {
            input->u.mi.dx *= 2;
            if ((abs(input->u.mi.dx) > accel[1]) && (accel[2] == 2)) input->u.mi.dx *= 2;
        }
        if (abs(input->u.mi.dy) > accel[0])
        {
            input->u.mi.dy *= 2;
            if ((abs(input->u.mi.dy) > accel[1]) && (accel[2] == 2)) input->u.mi.dy *= 2;
        }
    }
}

/***********************************************************************
 *		SendInput  (USER32.@)
 */
UINT WINAPI SendInput( UINT count, LPINPUT inputs, int size )
{
    UINT i;
    NTSTATUS status = STATUS_SUCCESS;
    RAWINPUT rawinput;

    if (size != sizeof(INPUT))
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return 0;
    }

    if (!count)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return 0;
    }

    if (!inputs)
    {
        SetLastError( ERROR_NOACCESS );
        return 0;
    }

    for (i = 0; i < count; i++)
    {
        INPUT input = inputs[i];
        switch (input.type)
        {
        case INPUT_MOUSE:
            /* we need to update the coordinates to what the server expects */
            update_mouse_coords( &input );
            /* fallthrough */
        case INPUT_KEYBOARD:
            status = send_hardware_message( 0, &input, &rawinput, SEND_HWMSG_INJECTED );
            break;
        case INPUT_HARDWARE:
            SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
            return 0;
        }

        if (status)
        {
            SetLastError( RtlNtStatusToDosError(status) );
            break;
        }
    }

    return i;
}


/***********************************************************************
 *		keybd_event (USER32.@)
 */
void WINAPI keybd_event( BYTE bVk, BYTE bScan,
                         DWORD dwFlags, ULONG_PTR dwExtraInfo )
{
    INPUT input;

    input.type = INPUT_KEYBOARD;
    input.u.ki.wVk = bVk;
    input.u.ki.wScan = bScan;
    input.u.ki.dwFlags = dwFlags;
    input.u.ki.time = 0;
    input.u.ki.dwExtraInfo = dwExtraInfo;
    SendInput( 1, &input, sizeof(input) );
}


/***********************************************************************
 *		mouse_event (USER32.@)
 */
void WINAPI mouse_event( DWORD dwFlags, DWORD dx, DWORD dy,
                         DWORD dwData, ULONG_PTR dwExtraInfo )
{
    INPUT input;

    input.type = INPUT_MOUSE;
    input.u.mi.dx = dx;
    input.u.mi.dy = dy;
    input.u.mi.mouseData = dwData;
    input.u.mi.dwFlags = dwFlags;
    input.u.mi.time = 0;
    input.u.mi.dwExtraInfo = dwExtraInfo;
    SendInput( 1, &input, sizeof(input) );
}


/***********************************************************************
 *		GetCursorPos (USER32.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetCursorPos( POINT *pt )
{
    BOOL ret = TRUE;
    DWORD last_change;
    UINT dpi;
    volatile struct desktop_shared_memory *shared = get_desktop_shared_memory();

    if (!pt || !shared) return FALSE;

    SHARED_READ_BEGIN( &shared->seq )
    {
        pt->x = shared->cursor.x;
        pt->y = shared->cursor.y;
        last_change = shared->cursor.last_change;
    }
    SHARED_READ_END( &shared->seq );

    /* query new position from graphics driver if we haven't updated recently */
    if (GetTickCount() - last_change > 100) ret = USER_Driver->pGetCursorPos( pt );
    if (ret && (dpi = get_thread_dpi()))
    {
        DPI_AWARENESS_CONTEXT context;
        context = SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE );
        *pt = map_dpi_point( *pt, get_monitor_dpi( MonitorFromPoint( *pt, MONITOR_DEFAULTTOPRIMARY )), dpi );
        SetThreadDpiAwarenessContext( context );
    }
    return ret;
}


/***********************************************************************
 *		GetCursorInfo (USER32.@)
 */
BOOL WINAPI GetCursorInfo( PCURSORINFO pci )
{
    volatile struct input_shared_memory *shared = get_foreground_shared_memory();
    BOOL ret;

    if (!pci) return FALSE;

    if (!shared) ret = FALSE;
    else SHARED_READ_BEGIN( &shared->seq )
    {
        pci->hCursor = wine_server_ptr_handle( shared->cursor );
        pci->flags = (shared->cursor_count >= 0) ? CURSOR_SHOWING : 0;
        ret = TRUE;
    }
    SHARED_READ_END( &shared->seq );
    GetCursorPos(&pci->ptScreenPos);
    return ret;
}


/***********************************************************************
 *		SetCursorPos (USER32.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH SetCursorPos( INT x, INT y )
{
    POINT pt = { x, y };
    BOOL ret;
    INT prev_x, prev_y, new_x, new_y;
    UINT dpi;

    if ((dpi = get_thread_dpi()))
        pt = map_dpi_point( pt, dpi, get_monitor_dpi( MonitorFromPoint( pt, MONITOR_DEFAULTTOPRIMARY )));

    SERVER_START_REQ( set_cursor )
    {
        req->flags = SET_CURSOR_POS;
        req->x     = pt.x;
        req->y     = pt.y;
        if ((ret = !wine_server_call( req )))
        {
            prev_x = reply->prev_x;
            prev_y = reply->prev_y;
            new_x  = reply->new_x;
            new_y  = reply->new_y;
        }
    }
    SERVER_END_REQ;
    if (ret && (prev_x != new_x || prev_y != new_y)) USER_Driver->pSetCursorPos( new_x, new_y );
    return ret;
}

/**********************************************************************
 *		SetCapture (USER32.@)
 */
HWND WINAPI DECLSPEC_HOTPATCH SetCapture( HWND hwnd )
{
    HWND previous = 0;

    set_capture_window( hwnd, 0, &previous );
    return previous;
}


/**********************************************************************
 *		ReleaseCapture (USER32.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH ReleaseCapture(void)
{
    BOOL ret = set_capture_window( 0, 0, NULL );

    /* Somebody may have missed some mouse movements */
    if (ret) mouse_event( MOUSEEVENTF_MOVE, 0, 0, 0, 0 );

    return ret;
}


/**********************************************************************
 *		GetCapture (USER32.@)
 */
HWND WINAPI GetCapture(void)
{
    volatile struct input_shared_memory *shared = get_input_shared_memory();
    HWND ret = 0;

    if (!shared) return 0;
    SHARED_READ_BEGIN( &shared->seq )
    {
        ret = wine_server_ptr_handle( shared->capture );
    }
    SHARED_READ_END( &shared->seq );
    return ret;
}


static void check_for_events( UINT flags )
{
    if (USER_Driver->pMsgWaitForMultipleObjectsEx( 0, NULL, 0, flags, 0 ) == WAIT_TIMEOUT)
        flush_window_surfaces( TRUE );
}

/**********************************************************************
 *		GetAsyncKeyState (USER32.@)
 *
 *	Determine if a key is or was pressed.  retval has high-order
 * bit set to 1 if currently pressed, low-order bit set to 1 if key has
 * been pressed.
 */
SHORT WINAPI DECLSPEC_HOTPATCH GetAsyncKeyState( INT key )
{
    volatile struct desktop_shared_memory *shared = get_desktop_shared_memory();
    BYTE state;

    if (key < 0 || key >= 256 || !shared) return 0;

    check_for_events( QS_INPUT );

    SHARED_READ_BEGIN( &shared->seq )
    {
        state = shared->keystate[key];
    }
    SHARED_READ_END( &shared->seq );

    return (state & 0x80) << 8;
}


/***********************************************************************
 *		GetQueueStatus (USER32.@)
 */
DWORD WINAPI GetQueueStatus( UINT flags )
{
    DWORD ret;

    if (flags & ~(QS_ALLINPUT | QS_ALLPOSTMESSAGE | QS_SMRESULT))
    {
        SetLastError( ERROR_INVALID_FLAGS );
        return 0;
    }

    check_for_events( flags );

    SERVER_START_REQ( get_queue_status )
    {
        req->clear_bits = flags;
        wine_server_call( req );
        ret = MAKELONG( reply->changed_bits & flags, reply->wake_bits & flags );
    }
    SERVER_END_REQ;
    return ret;
}


/***********************************************************************
 *		GetInputState   (USER32.@)
 */
BOOL WINAPI GetInputState(void)
{
    DWORD ret;

    check_for_events( QS_INPUT );

    SERVER_START_REQ( get_queue_status )
    {
        req->clear_bits = 0;
        wine_server_call( req );
        ret = reply->wake_bits & (QS_KEY | QS_MOUSEBUTTON);
    }
    SERVER_END_REQ;
    return ret;
}


/******************************************************************
 *              GetLastInputInfo (USER32.@)
 */
BOOL WINAPI GetLastInputInfo(PLASTINPUTINFO plii)
{
    BOOL ret;

    TRACE("%p\n", plii);

    if (plii->cbSize != sizeof (*plii) )
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    SERVER_START_REQ( get_last_input_time )
    {
        ret = !wine_server_call_err( req );
        if (ret)
            plii->dwTime = reply->time;
    }
    SERVER_END_REQ;
    return ret;
}


/**********************************************************************
 *		VkKeyScanA (USER32.@)
 *
 * VkKeyScan translates an ANSI character to a virtual-key and shift code
 * for the current keyboard.
 * high-order byte yields :
 *	0	Unshifted
 *	1	Shift
 *	2	Ctrl
 *	3-5	Shift-key combinations that are not used for characters
 *	6	Ctrl-Alt
 *	7	Ctrl-Alt-Shift
 *	I.e. :	Shift = 1, Ctrl = 2, Alt = 4.
 * FIXME : works ok except for dead chars :
 * VkKeyScan '^'(0x5e, 94) ... got keycode 00 ... returning 00
 * VkKeyScan '`'(0x60, 96) ... got keycode 00 ... returning 00
 */
SHORT WINAPI VkKeyScanA(CHAR cChar)
{
    WCHAR wChar;

    if (IsDBCSLeadByte(cChar)) return -1;

    MultiByteToWideChar(CP_ACP, 0, &cChar, 1, &wChar, 1);
    return VkKeyScanW(wChar);
}

/******************************************************************************
 *		VkKeyScanW (USER32.@)
 */
SHORT WINAPI VkKeyScanW(WCHAR cChar)
{
    return NtUserVkKeyScanEx( cChar, NtUserGetKeyboardLayout(0) );
}

/**********************************************************************
 *		VkKeyScanExA (USER32.@)
 */
WORD WINAPI VkKeyScanExA(CHAR cChar, HKL dwhkl)
{
    WCHAR wChar;

    if (IsDBCSLeadByte(cChar)) return -1;

    MultiByteToWideChar(CP_ACP, 0, &cChar, 1, &wChar, 1);
    return NtUserVkKeyScanEx( wChar, dwhkl );
}

/**********************************************************************
 *		OemKeyScan (USER32.@)
 */
DWORD WINAPI OemKeyScan( WORD oem )
{
    WCHAR wchr;
    DWORD vkey, scan;
    char oem_char = LOBYTE( oem );

    if (!OemToCharBuffW( &oem_char, &wchr, 1 ))
        return -1;

    vkey = VkKeyScanW( wchr );
    scan = MapVirtualKeyW( LOBYTE( vkey ), MAPVK_VK_TO_VSC );
    if (!scan) return -1;

    vkey &= 0xff00;
    vkey <<= 8;
    return vkey | scan;
}

/******************************************************************************
 *		GetKeyboardType (USER32.@)
 */
INT WINAPI GetKeyboardType(INT nTypeFlag)
{
    TRACE_(keyboard)("(%d)\n", nTypeFlag);
    if (LOWORD(NtUserGetKeyboardLayout(0)) == MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN))
    {
        /* scan code for `_', the key left of r-shift, in Japanese 106 keyboard */
        const UINT JP106_VSC_USCORE = 0x73;

        switch(nTypeFlag)
        {
        case 0:      /* Keyboard type */
            return 7;    /* Japanese keyboard */
        case 1:      /* Keyboard Subtype */
            /* Test keyboard mappings to detect Japanese keyboard */
            if (MapVirtualKeyW(VK_OEM_102, MAPVK_VK_TO_VSC) == JP106_VSC_USCORE
                && MapVirtualKeyW(JP106_VSC_USCORE, MAPVK_VSC_TO_VK) == VK_OEM_102)
                return 2;    /* Japanese 106 */
            else
                return 0;    /* AT-101 */
        case 2:      /* Number of F-keys */
            return 12;   /* It has 12 F-keys */
        }
    }
    else
    {
        switch(nTypeFlag)
        {
        case 0:      /* Keyboard type */
            return 4;    /* AT-101 */
        case 1:      /* Keyboard Subtype */
            return 0;    /* There are no defined subtypes */
        case 2:      /* Number of F-keys */
            return 12;   /* We're doing an 101 for now, so return 12 F-keys */
        }
    }
    WARN_(keyboard)("Unknown type\n");
    return 0;    /* The book says 0 here, so 0 */
}

/******************************************************************************
 *		MapVirtualKeyA (USER32.@)
 */
UINT WINAPI MapVirtualKeyA(UINT code, UINT maptype)
{
    return MapVirtualKeyExA( code, maptype, NtUserGetKeyboardLayout(0) );
}

/******************************************************************************
 *		MapVirtualKeyW (USER32.@)
 */
UINT WINAPI MapVirtualKeyW(UINT code, UINT maptype)
{
    return NtUserMapVirtualKeyEx( code, maptype, NtUserGetKeyboardLayout(0) );
}

/******************************************************************************
 *		MapVirtualKeyExA (USER32.@)
 */
UINT WINAPI MapVirtualKeyExA(UINT code, UINT maptype, HKL hkl)
{
    UINT ret;

    ret = NtUserMapVirtualKeyEx( code, maptype, hkl );
    if (maptype == MAPVK_VK_TO_CHAR)
    {
        BYTE ch = 0;
        WCHAR wch = ret;

        WideCharToMultiByte( CP_ACP, 0, &wch, 1, (LPSTR)&ch, 1, NULL, NULL );
        ret = ch;
    }
    return ret;
}

/****************************************************************************
 *		GetKBCodePage (USER32.@)
 */
UINT WINAPI GetKBCodePage(void)
{
    return GetOEMCP();
}

/****************************************************************************
 *		GetKeyboardLayoutNameA (USER32.@)
 */
BOOL WINAPI GetKeyboardLayoutNameA(LPSTR pszKLID)
{
    WCHAR buf[KL_NAMELENGTH];

    if (NtUserGetKeyboardLayoutName( buf ))
        return WideCharToMultiByte( CP_ACP, 0, buf, -1, pszKLID, KL_NAMELENGTH, NULL, NULL ) != 0;
    return FALSE;
}

/**********************************************************************
 *       GetKeyState    (USER32.@)
 *
 * An application calls the GetKeyState function in response to a
 * keyboard-input message.  This function retrieves the state of the key
 * at the time the input message was generated.
 */
SHORT WINAPI GetKeyState( INT vkey )
{
    volatile struct input_shared_memory *shared = get_input_shared_memory();
    SHORT retval = 0;
    BOOL skip = TRUE;

    if (!shared) skip = FALSE;
    else SHARED_READ_BEGIN( &shared->seq )
    {
        if (!shared->created) skip = FALSE; /* server needs to create the queue */
        else if (!shared->keystate_lock) skip = FALSE; /* server needs to call sync_input_keystate */
        else retval = (signed char)(shared->keystate[vkey & 0xff] & 0x81);
    }
    SHARED_READ_END( &shared->seq );

    if (!skip) retval = NtUserGetKeyState( vkey );
    TRACE("key (0x%x) -> %x\n", vkey, retval);
    return retval;
}

/**********************************************************************
 *       GetKeyboardState    (USER32.@)
 */
BOOL WINAPI GetKeyboardState( BYTE *state )
{
    volatile struct input_shared_memory *shared = get_input_shared_memory();
    BOOL skip = TRUE;
    int i;

    TRACE("(%p)\n", state);

    if (!shared) skip = FALSE;
    else SHARED_READ_BEGIN( &shared->seq )
    {
        if (!shared->created) skip = FALSE; /* server needs to create the queue */
        else memcpy( state, (const void *)shared->keystate, 256 );
    }
    SHARED_READ_END( &shared->seq );

    if (!skip) return NtUserGetKeyboardState( state );
    for (i = 0; i < 256; i++) state[i] &= 0x81;
    return TRUE;
}

/****************************************************************************
 *		GetKeyNameTextA (USER32.@)
 */
INT WINAPI GetKeyNameTextA(LONG lParam, LPSTR lpBuffer, INT nSize)
{
    WCHAR buf[256];
    INT ret;

    if (!nSize || !NtUserGetKeyNameText( lParam, buf, ARRAYSIZE(buf) ))
    {
        lpBuffer[0] = 0;
        return 0;
    }
    ret = WideCharToMultiByte(CP_ACP, 0, buf, -1, lpBuffer, nSize, NULL, NULL);
    if (!ret && nSize)
    {
        ret = nSize - 1;
        lpBuffer[ret] = 0;
    }
    else ret--;

    return ret;
}

/****************************************************************************
 *		ToUnicode (USER32.@)
 */
INT WINAPI ToUnicode( UINT virt, UINT scan, const BYTE *state, LPWSTR str, int size, UINT flags )
{
    return NtUserToUnicodeEx( virt, scan, state, str, size, flags, NtUserGetKeyboardLayout(0) );
}

/****************************************************************************
 *		ToAscii (USER32.@)
 */
INT WINAPI ToAscii( UINT virtKey, UINT scanCode, const BYTE *lpKeyState,
                    LPWORD lpChar, UINT flags )
{
    return ToAsciiEx( virtKey, scanCode, lpKeyState, lpChar, flags,
                      NtUserGetKeyboardLayout(0) );
}

/****************************************************************************
 *		ToAsciiEx (USER32.@)
 */
INT WINAPI ToAsciiEx( UINT virtKey, UINT scanCode, const BYTE *lpKeyState,
                      LPWORD lpChar, UINT flags, HKL dwhkl )
{
    WCHAR uni_chars[2];
    INT ret, n_ret;

    ret = NtUserToUnicodeEx( virtKey, scanCode, lpKeyState, uni_chars, 2, flags, dwhkl );
    if (ret < 0) n_ret = 1; /* FIXME: make ToUnicode return 2 for dead chars */
    else n_ret = ret;
    WideCharToMultiByte(CP_ACP, 0, uni_chars, n_ret, (LPSTR)lpChar, 2, NULL, NULL);
    return ret;
}

/**********************************************************************
 *		BlockInput (USER32.@)
 */
BOOL WINAPI BlockInput(BOOL fBlockIt)
{
    FIXME_(keyboard)("(%d): stub\n", fBlockIt);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);

    return FALSE;
}


/***********************************************************************
 *		RegisterHotKey (USER32.@)
 */
BOOL WINAPI RegisterHotKey(HWND hwnd,INT id,UINT modifiers,UINT vk)
{
    BOOL ret;
    int replaced=0;

    TRACE_(keyboard)("(%p,%d,0x%08x,%X)\n",hwnd,id,modifiers,vk);

    if ((hwnd == NULL || WIN_IsCurrentThread(hwnd)) &&
        !USER_Driver->pRegisterHotKey(hwnd, modifiers, vk))
        return FALSE;

    SERVER_START_REQ( register_hotkey )
    {
        req->window = wine_server_user_handle( hwnd );
        req->id = id;
        req->flags = modifiers;
        req->vkey = vk;
        if ((ret = !wine_server_call_err( req )))
        {
            replaced = reply->replaced;
            modifiers = reply->flags;
            vk = reply->vkey;
        }
    }
    SERVER_END_REQ;

    if (ret && replaced)
        USER_Driver->pUnregisterHotKey(hwnd, modifiers, vk);

    return ret;
}

/***********************************************************************
 *		LoadKeyboardLayoutW (USER32.@)
 */
HKL WINAPI LoadKeyboardLayoutW( const WCHAR *name, UINT flags )
{
    WCHAR layout_path[MAX_PATH], value[5];
    DWORD value_size, tmp;
    HKEY hkey;
    HKL layout;

    FIXME_(keyboard)( "name %s, flags %x, semi-stub!\n", debugstr_w( name ), flags );

    tmp = wcstoul( name, NULL, 16 );
    if (HIWORD( tmp )) layout = UlongToHandle( tmp );
    else layout = UlongToHandle( MAKELONG( LOWORD( tmp ), LOWORD( tmp ) ) );

    wcscpy( layout_path, L"System\\CurrentControlSet\\Control\\Keyboard Layouts\\" );
    wcscat( layout_path, name );

    if (!RegOpenKeyW( HKEY_LOCAL_MACHINE, layout_path, &hkey ))
    {
        value_size = sizeof(value);
        if (!RegGetValueW( hkey, NULL, L"Layout Id", RRF_RT_REG_SZ, NULL, (void *)&value, &value_size ))
            layout = UlongToHandle( MAKELONG( LOWORD( tmp ), 0xf000 | (wcstoul( value, NULL, 16 ) & 0xfff) ) );

        RegCloseKey( hkey );
    }

    if ((flags & KLF_ACTIVATE) && NtUserActivateKeyboardLayout( layout, 0 )) return layout;

    /* FIXME: semi-stub: returning default layout */
    return get_locale_kbd_layout();
}

/***********************************************************************
 *		LoadKeyboardLayoutA (USER32.@)
 */
HKL WINAPI LoadKeyboardLayoutA(LPCSTR pwszKLID, UINT Flags)
{
    HKL ret;
    UNICODE_STRING pwszKLIDW;

    if (pwszKLID) RtlCreateUnicodeStringFromAsciiz(&pwszKLIDW, pwszKLID);
    else pwszKLIDW.Buffer = NULL;

    ret = LoadKeyboardLayoutW(pwszKLIDW.Buffer, Flags);
    RtlFreeUnicodeString(&pwszKLIDW);
    return ret;
}


/***********************************************************************
 *		UnloadKeyboardLayout (USER32.@)
 */
BOOL WINAPI UnloadKeyboardLayout( HKL layout )
{
    FIXME_(keyboard)( "layout %p, stub!\n", layout );
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

typedef struct __TRACKINGLIST {
    TRACKMOUSEEVENT tme;
    POINT pos; /* center of hover rectangle */
} _TRACKINGLIST;

/* FIXME: move tracking stuff into a per thread data */
static _TRACKINGLIST tracking_info;
static UINT_PTR timer;

static void check_mouse_leave(HWND hwnd, int hittest)
{
    if (tracking_info.tme.hwndTrack != hwnd)
    {
        if (tracking_info.tme.dwFlags & TME_NONCLIENT)
            PostMessageW(tracking_info.tme.hwndTrack, WM_NCMOUSELEAVE, 0, 0);
        else
            PostMessageW(tracking_info.tme.hwndTrack, WM_MOUSELEAVE, 0, 0);

        /* remove the TME_LEAVE flag */
        tracking_info.tme.dwFlags &= ~TME_LEAVE;
    }
    else
    {
        if (hittest == HTCLIENT)
        {
            if (tracking_info.tme.dwFlags & TME_NONCLIENT)
            {
                PostMessageW(tracking_info.tme.hwndTrack, WM_NCMOUSELEAVE, 0, 0);
                /* remove the TME_LEAVE flag */
                tracking_info.tme.dwFlags &= ~TME_LEAVE;
            }
        }
        else
        {
            if (!(tracking_info.tme.dwFlags & TME_NONCLIENT))
            {
                PostMessageW(tracking_info.tme.hwndTrack, WM_MOUSELEAVE, 0, 0);
                /* remove the TME_LEAVE flag */
                tracking_info.tme.dwFlags &= ~TME_LEAVE;
            }
        }
    }
}

static void CALLBACK TrackMouseEventProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent,
                                         DWORD dwTime)
{
    POINT pos;
    INT hoverwidth = 0, hoverheight = 0, hittest;

    TRACE("hwnd %p, msg %04x, id %04lx, time %u\n", hwnd, uMsg, idEvent, dwTime);

    GetCursorPos(&pos);
    hwnd = WINPOS_WindowFromPoint(hwnd, pos, &hittest);

    TRACE("point %s hwnd %p hittest %d\n", wine_dbgstr_point(&pos), hwnd, hittest);

    SystemParametersInfoW(SPI_GETMOUSEHOVERWIDTH, 0, &hoverwidth, 0);
    SystemParametersInfoW(SPI_GETMOUSEHOVERHEIGHT, 0, &hoverheight, 0);

    TRACE("tracked pos %s, current pos %s, hover width %d, hover height %d\n",
           wine_dbgstr_point(&tracking_info.pos), wine_dbgstr_point(&pos),
           hoverwidth, hoverheight);

    /* see if this tracking event is looking for TME_LEAVE and that the */
    /* mouse has left the window */
    if (tracking_info.tme.dwFlags & TME_LEAVE)
    {
        check_mouse_leave(hwnd, hittest);
    }

    if (tracking_info.tme.hwndTrack != hwnd)
    {
        /* mouse is gone, stop tracking mouse hover */
        tracking_info.tme.dwFlags &= ~TME_HOVER;
    }

    /* see if we are tracking hovering for this hwnd */
    if (tracking_info.tme.dwFlags & TME_HOVER)
    {
        /* has the cursor moved outside the rectangle centered around pos? */
        if ((abs(pos.x - tracking_info.pos.x) > (hoverwidth / 2)) ||
            (abs(pos.y - tracking_info.pos.y) > (hoverheight / 2)))
        {
            /* record this new position as the current position */
            tracking_info.pos = pos;
        }
        else
        {
            if (hittest == HTCLIENT)
            {
                ScreenToClient(hwnd, &pos);
                TRACE("client cursor pos %s\n", wine_dbgstr_point(&pos));

                PostMessageW(tracking_info.tme.hwndTrack, WM_MOUSEHOVER,
                             get_key_state(), MAKELPARAM( pos.x, pos.y ));
            }
            else
            {
                if (tracking_info.tme.dwFlags & TME_NONCLIENT)
                    PostMessageW(tracking_info.tme.hwndTrack, WM_NCMOUSEHOVER,
                                 hittest, MAKELPARAM( pos.x, pos.y ));
            }

            /* stop tracking mouse hover */
            tracking_info.tme.dwFlags &= ~TME_HOVER;
        }
    }

    /* stop the timer if the tracking list is empty */
    if (!(tracking_info.tme.dwFlags & (TME_HOVER | TME_LEAVE)))
    {
        KillSystemTimer(tracking_info.tme.hwndTrack, timer);
        timer = 0;
        tracking_info.tme.hwndTrack = 0;
        tracking_info.tme.dwFlags = 0;
        tracking_info.tme.dwHoverTime = 0;
    }
}


/***********************************************************************
 * TrackMouseEvent [USER32]
 *
 * Requests notification of mouse events
 *
 * During mouse tracking WM_MOUSEHOVER or WM_MOUSELEAVE events are posted
 * to the hwnd specified in the ptme structure.  After the event message
 * is posted to the hwnd, the entry in the queue is removed.
 *
 * If the current hwnd isn't ptme->hwndTrack the TME_HOVER flag is completely
 * ignored. The TME_LEAVE flag results in a WM_MOUSELEAVE message being posted
 * immediately and the TME_LEAVE flag being ignored.
 *
 * PARAMS
 *     ptme [I,O] pointer to TRACKMOUSEEVENT information structure.
 *
 * RETURNS
 *     Success: non-zero
 *     Failure: zero
 *
 */

BOOL WINAPI
TrackMouseEvent (TRACKMOUSEEVENT *ptme)
{
    HWND hwnd;
    POINT pos;
    DWORD hover_time;
    INT hittest;

    TRACE("%x, %x, %p, %u\n", ptme->cbSize, ptme->dwFlags, ptme->hwndTrack, ptme->dwHoverTime);

    if (ptme->cbSize != sizeof(TRACKMOUSEEVENT)) {
        WARN("wrong TRACKMOUSEEVENT size from app\n");
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* fill the TRACKMOUSEEVENT struct with the current tracking for the given hwnd */
    if (ptme->dwFlags & TME_QUERY )
    {
        *ptme = tracking_info.tme;
        /* set cbSize in the case it's not initialized yet */
        ptme->cbSize = sizeof(TRACKMOUSEEVENT);

        return TRUE; /* return here, TME_QUERY is retrieving information */
    }

    if (!IsWindow(ptme->hwndTrack))
    {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    hover_time = (ptme->dwFlags & TME_HOVER) ? ptme->dwHoverTime : HOVER_DEFAULT;

    /* if HOVER_DEFAULT was specified replace this with the system's current value.
     * TME_LEAVE doesn't need to specify hover time so use default */
    if (hover_time == HOVER_DEFAULT || hover_time == 0)
        SystemParametersInfoW(SPI_GETMOUSEHOVERTIME, 0, &hover_time, 0);

    GetCursorPos(&pos);
    hwnd = WINPOS_WindowFromPoint(ptme->hwndTrack, pos, &hittest);
    TRACE("point %s hwnd %p hittest %d\n", wine_dbgstr_point(&pos), hwnd, hittest);

    if (ptme->dwFlags & ~(TME_CANCEL | TME_HOVER | TME_LEAVE | TME_NONCLIENT))
        FIXME("Unknown flag(s) %08x\n", ptme->dwFlags & ~(TME_CANCEL | TME_HOVER | TME_LEAVE | TME_NONCLIENT));

    if (ptme->dwFlags & TME_CANCEL)
    {
        if (tracking_info.tme.hwndTrack == ptme->hwndTrack)
        {
            tracking_info.tme.dwFlags &= ~(ptme->dwFlags & ~TME_CANCEL);

            /* if we aren't tracking on hover or leave remove this entry */
            if (!(tracking_info.tme.dwFlags & (TME_HOVER | TME_LEAVE)))
            {
                KillSystemTimer(tracking_info.tme.hwndTrack, timer);
                timer = 0;
                tracking_info.tme.hwndTrack = 0;
                tracking_info.tme.dwFlags = 0;
                tracking_info.tme.dwHoverTime = 0;
            }
        }
    } else {
        /* In our implementation it's possible that another window will receive a
         * WM_MOUSEMOVE and call TrackMouseEvent before TrackMouseEventProc is
         * called. In such a situation post the WM_MOUSELEAVE now */
        if (tracking_info.tme.dwFlags & TME_LEAVE && tracking_info.tme.hwndTrack != NULL)
            check_mouse_leave(hwnd, hittest);

        if (timer)
        {
            KillSystemTimer(tracking_info.tme.hwndTrack, timer);
            timer = 0;
            tracking_info.tme.hwndTrack = 0;
            tracking_info.tme.dwFlags = 0;
            tracking_info.tme.dwHoverTime = 0;
        }

        if (ptme->hwndTrack == hwnd)
        {
            /* Adding new mouse event to the tracking list */
            tracking_info.tme = *ptme;
            tracking_info.tme.dwHoverTime = hover_time;

            /* Initialize HoverInfo variables even if not hover tracking */
            tracking_info.pos = pos;

            timer = SetSystemTimer(tracking_info.tme.hwndTrack, (UINT_PTR)&tracking_info.tme, hover_time, TrackMouseEventProc);
        }
    }

    return TRUE;
}

BOOL enable_mouse_in_pointer = FALSE;

/***********************************************************************
 *		EnableMouseInPointer (USER32.@)
 */
BOOL WINAPI EnableMouseInPointer(BOOL enable)
{
    FIXME("(%#x) semi-stub\n", enable);
    enable_mouse_in_pointer = TRUE;
    return TRUE;
}

static DWORD CALLBACK devnotify_window_callback(HANDLE handle, DWORD flags, DEV_BROADCAST_HDR *header)
{
    DEV_BROADCAST_DEVICEINTERFACE_W *iface = (DEV_BROADCAST_DEVICEINTERFACE_W *)header;
    if (flags == DBT_DEVICEARRIVAL) rawinput_add_device(iface->dbcc_name);
    if (flags == DBT_DEVICEREMOVECOMPLETE) rawinput_remove_device(iface->dbcc_name);

    SendMessageTimeoutW(handle, WM_DEVICECHANGE, flags, (LPARAM)header, SMTO_ABORTIFHUNG, 2000, NULL);
    return 0;
}

static DWORD CALLBACK devnotify_service_callback(HANDLE handle, DWORD flags, DEV_BROADCAST_HDR *header)
{
    FIXME("Support for service handles is not yet implemented!\n");
    return 0;
}

struct device_notification_details
{
    DWORD (CALLBACK *cb)(HANDLE handle, DWORD flags, DEV_BROADCAST_HDR *header);
    HANDLE handle;
    union
    {
        DEV_BROADCAST_HDR header;
        DEV_BROADCAST_DEVICEINTERFACE_W iface;
    } filter;
};

extern HDEVNOTIFY WINAPI I_ScRegisterDeviceNotification( struct device_notification_details *details,
        void *filter, DWORD flags );
extern BOOL WINAPI I_ScUnregisterDeviceNotification( HDEVNOTIFY handle );

/***********************************************************************
 *		RegisterDeviceNotificationA (USER32.@)
 *
 * See RegisterDeviceNotificationW.
 */
HDEVNOTIFY WINAPI RegisterDeviceNotificationA( HANDLE handle, void *filter, DWORD flags )
{
    return RegisterDeviceNotificationW( handle, filter, flags );
}

/***********************************************************************
 *		RegisterDeviceNotificationW (USER32.@)
 */
HDEVNOTIFY WINAPI RegisterDeviceNotificationW( HANDLE handle, void *filter, DWORD flags )
{
    struct device_notification_details details;
    DEV_BROADCAST_HDR *header = filter;

    TRACE("handle %p, filter %p, flags %#x\n", handle, filter, flags);

    if (flags & ~(DEVICE_NOTIFY_SERVICE_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES))
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return NULL;
    }

    if (!(flags & DEVICE_NOTIFY_SERVICE_HANDLE) && !IsWindow( handle ))
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return NULL;
    }

    if (!header) details.filter.header.dbch_devicetype = 0;
    else if (header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
    {
        DEV_BROADCAST_DEVICEINTERFACE_W *iface = (DEV_BROADCAST_DEVICEINTERFACE_W *)header;
        details.filter.iface = *iface;

        if (flags & DEVICE_NOTIFY_ALL_INTERFACE_CLASSES)
            details.filter.iface.dbcc_size = offsetof( DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_classguid );
        else
            details.filter.iface.dbcc_size = offsetof( DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name );
    }
    else if (header->dbch_devicetype == DBT_DEVTYP_HANDLE)
    {
        FIXME( "DBT_DEVTYP_HANDLE filter type not implemented\n" );
        details.filter.header.dbch_devicetype = 0;
    }
    else
    {
        SetLastError( ERROR_INVALID_DATA );
        return NULL;
    }

    details.handle = handle;

    if (flags & DEVICE_NOTIFY_SERVICE_HANDLE)
        details.cb = devnotify_service_callback;
    else
        details.cb = devnotify_window_callback;

    return I_ScRegisterDeviceNotification( &details, filter, 0 );
}

/***********************************************************************
 *		UnregisterDeviceNotification (USER32.@)
 */
BOOL WINAPI UnregisterDeviceNotification( HDEVNOTIFY handle )
{
    TRACE("%p\n", handle);

    return I_ScUnregisterDeviceNotification( handle );
}

/*****************************************************************************
 * CloseTouchInputHandle (USER32.@)
 */
BOOL WINAPI CloseTouchInputHandle( HTOUCHINPUT handle )
{
    TRACE( "handle %p.\n", handle );
    return TRUE;
}

/*****************************************************************************
 * GetTouchInputInfo (USER32.@)
 */
BOOL WINAPI GetTouchInputInfo( HTOUCHINPUT handle, UINT count, TOUCHINPUT *ptr, int size )
{
    TRACE( "handle %p, count %u, ptr %p, size %u.\n", handle, count, ptr, size );
    *ptr = *(TOUCHINPUT *)handle;
    return TRUE;
}

/**********************************************************************
 * IsTouchWindow (USER32.@)
 */
BOOL WINAPI IsTouchWindow( HWND hwnd, ULONG *flags )
{
    DWORD win_flags = win_set_flags( hwnd, 0, 0 );
    TRACE( "hwnd %p, flags %p.\n", hwnd, flags );
    return (win_flags & WIN_IS_TOUCH) != 0;
}

/*****************************************************************************
 * RegisterTouchWindow (USER32.@)
 */
BOOL WINAPI RegisterTouchWindow( HWND hwnd, ULONG flags )
{
    DWORD win_flags = win_set_flags( hwnd, WIN_IS_TOUCH, 0 );
    TRACE( "hwnd %p, flags %#x.\n", hwnd, flags );
    return (win_flags & WIN_IS_TOUCH) == 0;
}

/*****************************************************************************
 * UnregisterTouchWindow (USER32.@)
 */
BOOL WINAPI UnregisterTouchWindow( HWND hwnd )
{
    DWORD win_flags = win_set_flags( hwnd, 0, WIN_IS_TOUCH );
    TRACE( "hwnd %p.\n", hwnd );
    return (win_flags & WIN_IS_TOUCH) != 0;
}

/*****************************************************************************
 * GetGestureInfo (USER32.@)
 */
BOOL WINAPI CloseGestureInfoHandle( HGESTUREINFO handle )
{
    FIXME( "handle %p stub!\n", handle );
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

/*****************************************************************************
 * GetGestureInfo (USER32.@)
 */
BOOL WINAPI GetGestureExtraArgs( HGESTUREINFO handle, UINT count, BYTE *args )
{
    FIXME( "handle %p, count %u, args %p stub!\n", handle, count, args );
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

/*****************************************************************************
 * GetGestureInfo (USER32.@)
 */
BOOL WINAPI GetGestureInfo( HGESTUREINFO handle, GESTUREINFO *ptr )
{
    FIXME( "handle %p, ptr %p stub!\n", handle, ptr );
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

/*****************************************************************************
 * GetGestureConfig (USER32.@)
 */
BOOL WINAPI GetGestureConfig( HWND hwnd, DWORD reserved, DWORD flags, UINT *count,
                              GESTURECONFIG *config, UINT size )
{
    FIXME( "handle %p, reserved %#x, flags %#x, count %p, config %p, size %u stub!\n",
           hwnd, reserved, flags, count, config, size );
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

/**********************************************************************
 * SetGestureConfig (USER32.@)
 */
BOOL WINAPI SetGestureConfig( HWND hwnd, DWORD reserved, UINT count,
                              GESTURECONFIG *config, UINT size )
{
    FIXME( "handle %p, reserved %#x, count %u, config %p, size %u stub!\n",
           hwnd, reserved, count, config, size );
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}
