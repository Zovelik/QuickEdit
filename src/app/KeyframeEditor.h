#pragma once

#include <core/Project.h>

#include <imgui.h>

#include <set>
#include <string>
#include <vector>


// The curves of the animated properties of a clip (transform, effects,
// generator), in a panel beside the timeline: the properties on the left,
// the curve of the chosen one on the right. Keyframes are added with a
// double click, moved by dragging (in time and value), removed with Delete,
// and their easing is set from their context menu.
class KeyframeEditor
{
public:
    struct Result
    {
        std::string edit;           // Label of a finished edit, if any
        bool seeked = false;        // The playhead was moved
    };

    Result draw(core::Project& project, const std::vector<core::Id>& selection, core::Time& playhead);

private:
    struct Property
    {
        std::string group;
        std::string name;
        core::AnimatedValue* value = nullptr;
        float minimum = -1e6f;
        float maximum = 1e6f;
        float displayScale = 1.f;   // Shown values: value * displayScale
    };

    static std::vector<Property> propertiesOf(core::Project& project, core::Clip& clip);

    void drawCurve(const core::Clip& clip, Property& property, const core::Sequence& sequence, core::Time& playhead, Result& result);

    core::Id _clip = core::InvalidId;
    std::string _property;          // group + name of the chosen one

    // Time shown, in the clip; 0, 0 for the whole clip.
    core::Time _viewStart = 0;
    core::Time _viewEnd = 0;

    std::set<int> _selectedKeys;

    // Dragging keys: the one grabbed, the range of values while dragging.
    int _draggedKey = -1;
    bool _dragMoved = false;
    float _dragMinimum = 0.f;
    float _dragMaximum = 1.f;
    ImVec2 _dragStart;
    std::vector<core::Keyframe> _dragStartKeys;
};
