#pragma once

#include <core/Json.h>
#include <core/Project.h>

#include <string>
#include <vector>


namespace core
{
    // Undo / redo by snapshots of the whole project. Snapshots share their
    // unchanged arrays and objects (see Json), and a project is small next
    // to its media, so this stays cheap while keeping every edit trivially
    // undoable. To be replaced by commands if projects grow large.
    class History
    {
    public:
        explicit History(size_t maxSteps = 200) : _maxSteps(maxSteps) {}

        // Forgets the history and records the given state as the current one.
        void reset(const Project& project);

        // Records the state of the project after an edit described by label.
        void commit(const Project& project, const std::string& label);

        bool canUndo() const { return _current > 0; }
        bool canRedo() const { return _current + 1 < _states.size(); }

        // Label of the edit undone / redone by the next call.
        const std::string& undoLabel() const;
        const std::string& redoLabel() const;

        // Restores the project to the previous / next state.
        void undo(Project& project);
        void redo(Project& project);

        // True when the current state differs from the last saved one.
        bool modified() const { return _current != _savedState; }
        void markSaved() { _savedState = _current; }

    private:
        struct State
        {
            Json snapshot;
            std::string label;
        };

        std::vector<State> _states;
        size_t _current = 0;
        size_t _savedState = 0;
        size_t _maxSteps;
    };
}
