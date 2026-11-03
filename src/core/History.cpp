#include <core/History.h>

#include <limits>


namespace core
{

namespace {

const std::string EmptyLabel;

// No saved state matches this index.
constexpr size_t NoSavedState = std::numeric_limits<size_t>::max();

} // namespace


void History::reset(const Project& project)
{
    // Snapshots keep absolute paths: they never leave the application.
    _states.clear();
    _states.push_back({ project.toJson({}), {} });
    _current = 0;
    _savedState = 0;
    _revision++;
}


void History::markUnsaved()
{
    _savedState = NoSavedState;
}


void History::commit(const Project& project, const std::string& label)
{
    // A new edit drops the redo branch.
    _states.resize(_current + 1);

    if (_savedState != NoSavedState && _savedState > _current) {
        _savedState = NoSavedState;
    }

    _states.push_back({ project.toJson({}), label });
    _current++;
    _revision++;

    if (_states.size() > _maxSteps) {
        _states.erase(_states.begin());
        _current--;

        if (_savedState != NoSavedState) {
            _savedState = (_savedState == 0) ? NoSavedState : _savedState - 1;
        }
    }
}


const std::string& History::undoLabel() const
{
    return canUndo() ? _states[_current].label : EmptyLabel;
}


const std::string& History::redoLabel() const
{
    return canRedo() ? _states[_current + 1].label : EmptyLabel;
}


void History::undo(Project& project)
{
    if (!canUndo()) {
        return;
    }

    _current--;
    _revision++;
    project.fromJson(_states[_current].snapshot, {});
}


void History::redo(Project& project)
{
    if (!canRedo()) {
        return;
    }

    _current++;
    _revision++;
    project.fromJson(_states[_current].snapshot, {});
}

}
