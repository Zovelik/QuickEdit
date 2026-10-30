#pragma once

#include <render/FontLibrary.h>
#include <render/TextLayout.h>

#include <core/Project.h>

#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>


// Edits a text clip directly in the viewer, as in a slide editor: caret,
// selection with the mouse and the keyboard, copy and paste, bold and
// italic. The text is laid out as the compositor draws it, so the caret and
// the selection fall on the letters.
class ViewerTextEditor
{
public:
    // Where the text is drawn: layer pixels to sequence pixels (as the
    // compositor places it), and sequence pixels to the screen.
    struct Mapping
    {
        float scale = 1.f;
        float cosine = 1.f;
        float sine = 0.f;
        float x = 0.f;              // Center of the text, sequence pixels
        float y = 0.f;
        ImVec2 frameMin;            // Screen position of the frame
        float toSequence = 1.f;     // Sequence pixels per screen pixel

        ImVec2 toScreen(float lx, float ly) const;
        ImVec2 toLayer(const ImVec2& screen) const;
    };

    bool active() const { return _clip != core::InvalidId; }
    core::Id clip() const { return _clip; }

    // Starts editing, with the whole text selected.
    void begin(const core::Clip& clip);

    // Stops editing. Returns true when the text changed since begin(), for
    // the undo history.
    bool end(core::Project& project);

    // Selection [start, end) in characters; start == end for a caret.
    // (Parentheses: windows.h may define min and max.)
    size_t selectionStart() const { return (std::min)(_caret, _anchor); }
    size_t selectionEnd() const { return (std::max)(_caret, _anchor); }

    // Handles the keyboard and the mouse (hovered: the mouse is over the
    // viewer frame), and draws the caret and the selection. Returns false
    // when a click outside the text ended the editing (the caller then
    // calls end()).
    bool update(core::Project& project, FontLibrary& fonts, const Mapping& mapping, bool hovered);

    // The text changed outside the editor (inspector): the snapshot of
    // begin() is refreshed by the caller committing it.
    void markCommitted(const core::Project& project);

private:
    size_t positionAt(const TextLayout& layout, const ImVec2& point) const;
    void moveCaret(size_t position, bool extend);

    core::Id _clip = core::InvalidId;
    std::vector<core::TextSpan> _snapshot;  // Text when editing started

    size_t _caret = 0;
    size_t _anchor = 0;
    bool _dragging = false;
    double _blinkStart = 0.0;

    // Remembered horizontal position for Up / Down.
    float _preferredX = 0.f;
    bool _hasPreferredX = false;
};
