#pragma once

#include <exception>
#include <functional>
#include <string>


namespace core
{
    // Where the errors no one handled go: the log, and the user told (set
    // by the application). Called on the thread of the error.
    void setErrorReporter(std::function<void(const std::string& where, const std::string& what)> reporter);
    void reportError(const std::string& where, const std::string& what);

    // Runs a function and reports an exception thrown out of it instead of
    // ending the program, which an exception leaving a thread does. Returns
    // false after an exception.
    template<typename Function>
    bool guarded(const char* where, Function&& function) noexcept
    {
        try {
            function();
            return true;
        }
        catch (const std::exception& e) {
            reportError(where, e.what());
        }
        catch (...) {
            reportError(where, "unknown error");
        }

        return false;
    }
}
