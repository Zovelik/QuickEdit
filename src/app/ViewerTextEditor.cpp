#include <app/ViewerTextEditor.h>

#include <core/TextEdit.h>

#include <cmath>


using namespace core;


namespace {

bool sameSpans(const std::vector<TextSpan>& a, const std::vector<TextSpan>& b)
{
    if (a.size() != b.size()) {
        return false;
    }

    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].text != b[i].text || !(a[i].style == b[i].style)) {
            return false;
        }
    }

    return true;
}


void appendUtf8(std::string& out, uint32_t codepoint)
{
    if (codepoint < 0x80) {
        out += (char)codepoint;
    }
    else if (codepoint < 0x800) {
        out += (char)(0xC0 | (codepoint >> 6));
        out += (char)(0x80 | (codepoint & 0x3F));
    }
    else if (codepoint < 0x10000) {
        out += (char)(0xE0 | (codepoint >> 12));
        out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out += (char)(0x80 | (codepoint & 0x3F));
    }
    else {
        out += (char)(0xF0 | (codepoint >> 18));
        out += (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out += (char)(0x80 | (codepoint & 0x3F));
    }
}


// Next style of the family for bold / italic toggles.
std::string toggledStyle(const std::string& style, bool bold)
{
    const bool isBold = style.find("Bold") != std::string::npos;
    const bool isItalic = style.find("Italic") != std::string::npos;

    const bool wantBold = bold ? !isBold : isBold;
    const bool wantItalic = bold ? isItalic : !isItalic;

    if (wantBold && wantItalic) return "Bold Italic";
    if (wantBold) return "Bold";
    if (wantItalic) return "Italic";
    return "Regular";
}

} // namespace


ImVec2 ViewerTextEditor::Mapping::toScreen(float lx, float ly) const
{
    const float sx = lx * scale;
    const float sy = ly * scale;
    const float px = x + sx * cosine - sy * sine;
    const float py = y + sx * sine + sy * cosine;

    return ImVec2(frameMin.x + px / toSequence, frameMin.y + py / toSequence);
}


ImVec2 ViewerTextEditor::Mapping::toLayer(const ImVec2& screen) const
{
    const float px = (screen.x - frameMin.x) * toSequence - x;
    const float py = (screen.y - frameMin.y) * toSequence - y;

    // Inverse rotation, then scale.
    const float rx = px * cosine + py * sine;
    const float ry = -px * sine + py * cosine;
    const float s = (scale != 0.f) ? scale : 1.f;

    return ImVec2(rx / s, ry / s);
}


void ViewerTextEditor::begin(const Clip& clip)
{
    _clip = clip.id;
    _snapshot = clip.text.spans;
    _anchor = 0;
    _caret = textLength(clip.text);
    _dragging = false;
    _hasPreferredX = false;
    _blinkStart = ImGui::GetTime();
}


bool ViewerTextEditor::end(Project& project)
{
    const Clip* clip = project.findClip(_clip);
    const bool changed = clip && !sameSpans(clip->text.spans, _snapshot);

    _clip = InvalidId;
    _snapshot.clear();
    _dragging = false;

    return changed;
}


void ViewerTextEditor::markCommitted(const Project& project)
{
    if (const Clip* clip = project.findClip(_clip)) {
        _snapshot = clip->text.spans;
    }
}


void ViewerTextEditor::moveCaret(size_t position, bool extend)
{
    _caret = position;

    if (!extend) {
        _anchor = position;
    }

    _blinkStart = ImGui::GetTime();
}


size_t ViewerTextEditor::positionAt(const TextLayout& layout, const ImVec2& point) const
{
    if (layout.characters.empty()) {
        return 0;
    }

    // The line under the point, or the nearest one.
    int line = 0;
    float bestDistance = 1e30f;

    for (const CharacterBox& box : layout.characters) {
        const float distance = (point.y < box.top) ? box.top - point.y : (point.y > box.bottom ? point.y - box.bottom : 0.f);

        if (distance < bestDistance) {
            bestDistance = distance;
            line = box.line;
        }
    }

    // The nearest character boundary on that line.
    size_t best = 0;
    bestDistance = 1e30f;

    for (size_t i = 0; i < layout.characters.size(); i++) {
        const CharacterBox& box = layout.characters[i];

        if (box.line != line) {
            continue;
        }

        if (std::abs(point.x - box.x0) < bestDistance) {
            bestDistance = std::abs(point.x - box.x0);
            best = i;
        }

        // A line break or a wrapped space has no width: the position after
        // it is on the next line.
        if (box.x1 > box.x0 && std::abs(point.x - box.x1) < bestDistance) {
            bestDistance = std::abs(point.x - box.x1);
            best = i + 1;
        }
    }

    return best;
}


bool ViewerTextEditor::update(Project& project, FontLibrary& fonts, const Mapping& mapping, bool hovered)
{
    Clip* clip = project.findClip(_clip);

    if (!clip || clip->kind != ClipKind::Text) {
        return false;
    }

    TextContent& text = clip->text;
    const TextLayout layout = layoutText(fonts, text);
    const size_t length = textLength(text);

    _caret = std::min(_caret, length);
    _anchor = std::min(_anchor, length);

    ImGuiIO& io = ImGui::GetIO();

    // ------------------------------------------------------------------------
    // Mouse
    // ------------------------------------------------------------------------

    const ImVec2 mouse = mapping.toLayer(io.MousePos);
    const float margin = 0.25f * FontLibrary::BaseSize;
    const bool overText = mouse.x >= -0.5f * layout.width - margin && mouse.x <= 0.5f * layout.width + margin &&
                          mouse.y >= -0.5f * layout.height - margin && mouse.y <= 0.5f * layout.height + margin;

    if (hovered && overText) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (!overText) {
            return false;
        }

        const size_t position = positionAt(layout, mouse);

        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            _anchor = wordStart(text, position);
            _caret = wordEnd(text, position);
        }
        else {
            moveCaret(position, io.KeyShift);
            _dragging = true;
        }

        _hasPreferredX = false;
    }

    if (_dragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            moveCaret(positionAt(layout, mouse), true);
        }
        else {
            _dragging = false;
        }
    }

    // ------------------------------------------------------------------------
    // Keyboard, unless an ImGui widget (inspector field) has it
    // ------------------------------------------------------------------------

    const bool keyboard = !io.WantTextInput && !ImGui::IsAnyItemActive();
    bool edited = false;

    auto replaceSelection = [&](const std::string& utf8) {
        const size_t from = selectionStart();
        eraseText(text, from, selectionEnd());
        moveCaret(insertText(text, from, utf8), false);
        edited = true;
    };

    auto caretX = [&](size_t position) {
        if (layout.characters.empty()) return 0.f;
        return (position < layout.characters.size()) ? layout.characters[position].x0 : layout.characters.back().x1;
    };

    auto caretLine = [&](size_t position) {
        if (layout.characters.empty()) return 0;
        return (position < layout.characters.size()) ? layout.characters[position].line : layout.characters.back().line;
    };

    auto lineMiddle = [&](int line) {
        for (const CharacterBox& box : layout.characters) {
            if (box.line == line) return 0.5f * (box.top + box.bottom);
        }
        return 0.f;
    };

    if (keyboard && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        return false;
    }

    if (keyboard) {
        ImGui::SetNextFrameWantCaptureKeyboard(true);

        const bool ctrl = io.KeyCtrl;
        const bool shift = io.KeyShift;

        // Typed characters
        std::string typed;

        for (const ImWchar character : io.InputQueueCharacters) {
            if (character >= 0x20 && character != 0x7F) {
                appendUtf8(typed, character);
            }
        }

        if (!typed.empty() && !ctrl) {
            replaceSelection(typed);
            _hasPreferredX = false;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            replaceSelection("\n");
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            if (selectionStart() != selectionEnd()) {
                replaceSelection("");
            }
            else if (_caret > 0) {
                const size_t from = ctrl ? wordStart(text, _caret - 1) : _caret - 1;
                eraseText(text, from, _caret);
                moveCaret(from, false);
                edited = true;
            }
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (selectionStart() != selectionEnd()) {
                replaceSelection("");
            }
            else if (_caret < length) {
                const size_t to = ctrl ? wordEnd(text, _caret + 1) : _caret + 1;
                eraseText(text, _caret, to);
                edited = true;
            }
        }

        // Caret moves
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            const size_t target = (!shift && selectionStart() != selectionEnd()) ? selectionStart()
                : (_caret == 0 ? 0 : (ctrl ? wordStart(text, _caret - 1) : _caret - 1));
            moveCaret(target, shift);
            _hasPreferredX = false;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            const size_t target = (!shift && selectionStart() != selectionEnd()) ? selectionEnd()
                : (_caret >= length ? length : (ctrl ? wordEnd(text, _caret + 1) : _caret + 1));
            moveCaret(target, shift);
            _hasPreferredX = false;
        }

        for (const ImGuiKey key : { ImGuiKey_UpArrow, ImGuiKey_DownArrow }) {
            if (!ImGui::IsKeyPressed(key)) {
                continue;
            }

            const int line = caretLine(_caret) + ((key == ImGuiKey_UpArrow) ? -1 : 1);

            if (!_hasPreferredX) {
                _preferredX = caretX(_caret);
                _hasPreferredX = true;
            }

            if (line < 0) {
                moveCaret(0, shift);
            }
            else if (line >= layout.lineCount) {
                moveCaret(length, shift);
            }
            else {
                moveCaret(positionAt(layout, ImVec2(_preferredX, lineMiddle(line))), shift);
            }
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Home) || ImGui::IsKeyPressed(ImGuiKey_End)) {
            // Start or end of the line of the caret.
            const int line = caretLine(_caret);
            const bool home = ImGui::IsKeyPressed(ImGuiKey_Home);
            size_t target = home ? length : 0;

            for (size_t i = 0; i < layout.characters.size(); i++) {
                if (layout.characters[i].line != line) {
                    continue;
                }

                if (home) {
                    target = std::min(target, i);
                }
                else {
                    // Before the line break that ends the line, if any.
                    const bool lineBreak = layout.characters[i].x1 == layout.characters[i].x0;
                    target = std::max(target, lineBreak ? i : i + 1);
                }
            }

            moveCaret(ctrl ? (home ? 0 : length) : target, shift);
            _hasPreferredX = false;
        }

        // Shortcuts
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            _anchor = 0;
            _caret = length;
        }

        if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_C) || ImGui::IsKeyPressed(ImGuiKey_X)) && selectionStart() != selectionEnd()) {
            ImGui::SetClipboardText(textRange(text, selectionStart(), selectionEnd()).c_str());

            if (ImGui::IsKeyPressed(ImGuiKey_X)) {
                replaceSelection("");
            }
        }

        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
            if (const char* clipboard = ImGui::GetClipboardText()) {
                replaceSelection(clipboard);
            }
        }

        // Bold / italic: the variants of the font of the selection.
        for (const bool bold : { true, false }) {
            if (!ctrl || !ImGui::IsKeyPressed(bold ? ImGuiKey_B : ImGuiKey_I)) {
                continue;
            }

            const TextStyle before = styleAt(text, selectionStart() + 1);
            const std::string variant = fonts.variant(before.font, toggledStyle(fonts.styleOf(before.font), bold));

            if (!variant.empty()) {
                TextStyle after = before;
                after.font = variant;
                applyStyleChange(text, selectionStart(), selectionEnd(), before, after);
                edited = true;
            }
            else if (bold && fonts.styleOf(before.font).find("Bold") == std::string::npos) {
                // No bold variant (the default font): thicker letters.
                TextStyle after = before;
                after.bold = !before.bold;
                applyStyleChange(text, selectionStart(), selectionEnd(), before, after);
                edited = true;
            }
        }
    }

    if (edited) {
        _blinkStart = ImGui::GetTime();
    }

    // ------------------------------------------------------------------------
    // Selection and caret, over the frame
    // ------------------------------------------------------------------------

    // Layout of the edited text, which may have changed above.
    const TextLayout shown = edited ? layoutText(fonts, text) : layout;
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Text box
    const float hw = 0.5f * shown.width + 0.1f * FontLibrary::BaseSize;
    const float hh = 0.5f * shown.height + 0.1f * FontLibrary::BaseSize;
    const ImVec2 box[4] = { mapping.toScreen(-hw, -hh), mapping.toScreen(hw, -hh), mapping.toScreen(hw, hh), mapping.toScreen(-hw, hh) };
    drawList->AddPolyline(box, 4, IM_COL32(255, 255, 255, 120), ImDrawFlags_Closed, 1.f);

    for (size_t i = selectionStart(); i < std::min(selectionEnd(), shown.characters.size()); i++) {
        const CharacterBox& c = shown.characters[i];
        const float x1 = std::max(c.x1, c.x0 + 0.15f * FontLibrary::BaseSize);   // Line breaks show a little

        drawList->AddQuadFilled(
            mapping.toScreen(c.x0, c.top), mapping.toScreen(x1, c.top),
            mapping.toScreen(x1, c.bottom), mapping.toScreen(c.x0, c.bottom),
            IM_COL32(80, 150, 255, 110));
    }

    // Caret, blinking.
    if (std::fmod(ImGui::GetTime() - _blinkStart, 1.0) < 0.6) {
        float x = 0.f;
        float top = -0.5f * FontLibrary::BaseSize;
        float bottom = 0.5f * FontLibrary::BaseSize;
        const std::string plain = text.plainText();

        if (!shown.characters.empty()) {
            if (_caret < shown.characters.size()) {
                const CharacterBox& c = shown.characters[_caret];
                x = c.x0;
                top = c.top;
                bottom = c.bottom;
            }
            else {
                const CharacterBox& c = shown.characters.back();
                x = c.x1;
                top = c.top;
                bottom = c.bottom;

                // After a final line break: the start of the empty line.
                if (!plain.empty() && plain.back() == '\n') {
                    const float height = c.bottom - c.top;
                    top = c.bottom;
                    bottom = c.bottom + height;
                    x = (text.align == TextAlign::Center) ? 0.f : (text.align == TextAlign::Right ? 0.5f * shown.width : -0.5f * shown.width);
                }
            }
        }

        drawList->AddLine(mapping.toScreen(x, top), mapping.toScreen(x, bottom), IM_COL32(255, 255, 255, 255), 2.f);
    }

    return true;
}
