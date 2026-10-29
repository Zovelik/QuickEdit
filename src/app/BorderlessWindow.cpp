#include <app/BorderlessWindow.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>


namespace {

constexpr UINT_PTR SubclassId = 1;


// True when an auto-hide taskbar sits on the given edge of the monitor.
bool hasAutoHideTaskbar(UINT edge, const RECT& monitor)
{
    APPBARDATA data = {};
    data.cbSize = sizeof(data);
    data.uEdge = edge;
    data.rc = monitor;

    return SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &data) != 0;
}


void constrainToWorkArea(HWND hwnd, MINMAXINFO* minMaxInfo)
{
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);

    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
        return;
    }

    const RECT& monitor = monitorInfo.rcMonitor;
    RECT work = monitorInfo.rcWork;

    // An auto-hide taskbar does not reduce the work area, but it cannot be
    // revealed over a window covering the whole monitor: leave it one pixel.
    if (EqualRect(&work, &monitor)) {
        if      (hasAutoHideTaskbar(ABE_BOTTOM, monitor)) { work.bottom -= 1; }
        else if (hasAutoHideTaskbar(ABE_TOP,    monitor)) { work.top    += 1; }
        else if (hasAutoHideTaskbar(ABE_LEFT,   monitor)) { work.left   += 1; }
        else if (hasAutoHideTaskbar(ABE_RIGHT,  monitor)) { work.right  -= 1; }
    }

    // Maximized position is relative to the monitor.
    minMaxInfo->ptMaxPosition.x = work.left - monitor.left;
    minMaxInfo->ptMaxPosition.y = work.top  - monitor.top;
    minMaxInfo->ptMaxSize.x = work.right  - work.left;
    minMaxInfo->ptMaxSize.y = work.bottom - work.top;
}


LRESULT CALLBACK subclassProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR /*id*/,
    DWORD_PTR /*refData*/)
{
    switch (msg) {
    case WM_GETMINMAXINFO: {
        // Let SDL fill in the min / max tracking sizes, then override the
        // maximized rectangle.
        const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
        constrainToWorkArea(hwnd, reinterpret_cast<MINMAXINFO*>(lParam));
        return result;
    }

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, subclassProc, SubclassId);
        break;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

} // namespace


void fixBorderlessMaximize(SDL_Window* window)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr
    );

    if (hwnd) {
        SetWindowSubclass(hwnd, subclassProc, SubclassId, 0);
    }
}

#else

void fixBorderlessMaximize(SDL_Window* /*window*/)
{
}

#endif // _WIN32
