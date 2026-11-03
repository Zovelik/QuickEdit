#include <core/Guard.h>

#include <cstdio>
#include <mutex>


namespace core
{

namespace {

std::mutex reporterMutex;
std::function<void(const std::string&, const std::string&)> errorReporter;

}


void setErrorReporter(std::function<void(const std::string& where, const std::string& what)> reporter)
{
    std::scoped_lock lock(reporterMutex);
    errorReporter = std::move(reporter);
}


void reportError(const std::string& where, const std::string& what)
{
    try {
        std::scoped_lock lock(reporterMutex);

        if (errorReporter) {
            errorReporter(where, what);
        }
        else {
            std::fprintf(stderr, "%s: %s\n", where.c_str(), what.c_str());
        }
    }
    catch (...) {
        // Reporting must not end the program either.
    }
}

}
