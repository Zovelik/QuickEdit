#include <app/FileDrag.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>

#include <string>


namespace {

std::vector<std::filesystem::path> filesOf(IDataObject* data)
{
    std::vector<std::filesystem::path> files;

    FORMATETC format = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium = {};

    if (!data || FAILED(data->GetData(&format, &medium))) {
        return files;
    }

    if (HDROP drop = (HDROP)GlobalLock(medium.hGlobal)) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);

        for (UINT i = 0; i < count; i++) {
            std::wstring name(DragQueryFileW(drop, i, nullptr, 0), L'\0');
            DragQueryFileW(drop, i, name.data(), (UINT)name.size() + 1);
            files.emplace_back(name);
        }

        GlobalUnlock(medium.hGlobal);
    }

    ReleaseStgMedium(&medium);

    return files;
}


class DropTarget : public IDropTarget
{
public:
    DropTarget(HWND hwnd, FileDrag& drag) : _hwnd(hwnd), _drag(drag) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&_references); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG references = InterlockedDecrement(&_references);

        if (references == 0) {
            delete this;
        }

        return (ULONG)references;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL point, DWORD* effect) override
    {
        _drag = FileDrag();
        _drag.files = filesOf(data);
        _drag.active = !_drag.files.empty();
        move(point);

        *effect = _drag.active ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override
    {
        move(point);

        *effect = _drag.active ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        _drag = FileDrag();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL point, DWORD* effect) override
    {
        _drag.files = filesOf(data);
        _drag.active = !_drag.files.empty();
        _drag.dropped = _drag.active;
        move(point);

        *effect = _drag.active ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

private:
    // Client pixels, the window coordinates of SDL on Windows.
    void move(POINTL point)
    {
        POINT client = { point.x, point.y };
        ScreenToClient(_hwnd, &client);

        _drag.x = (float)client.x;
        _drag.y = (float)client.y;
    }

    LONG _references = 1;
    HWND _hwnd;
    FileDrag& _drag;
};

} // namespace


bool registerFileDropTarget(SDL_Window* window, FileDrag& drag)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);

    if (!hwnd) {
        return false;
    }

    // Done by SDL already: only adds a reference.
    OleInitialize(nullptr);

    RevokeDragDrop(hwnd);

    DropTarget* target = new DropTarget(hwnd, drag);
    const bool registered = SUCCEEDED(RegisterDragDrop(hwnd, target));

    // Kept alive by OLE while registered.
    target->Release();

    return registered;
}

#else

bool registerFileDropTarget(SDL_Window*, FileDrag&)
{
    return false;
}

#endif
