#include "pch.h"
#include "logger.h"
#include <cstdio>

namespace RE3HT {

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::SetREFunctions(REFLogFn info, REFLogFn warn, REFLogFn error) {
    m_logInfo = info;
    m_logWarn = warn;
    m_logError = error;
    m_initialized = true;
}

void Logger::FormatAndLog(REFLogFn logFn, const char* fmt, va_list args) {
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    logFn("[RE3HT] %s", buffer);
}

void Logger::Info(const char* fmt, ...) {
    if (!m_initialized || !m_logInfo) return;
    va_list args;
    va_start(args, fmt);
    FormatAndLog(m_logInfo, fmt, args);
    va_end(args);
}

void Logger::Warning(const char* fmt, ...) {
    if (!m_initialized || !m_logWarn) return;
    va_list args;
    va_start(args, fmt);
    FormatAndLog(m_logWarn, fmt, args);
    va_end(args);
}

void Logger::Error(const char* fmt, ...) {
    if (!m_initialized || !m_logError) return;
    va_list args;
    va_start(args, fmt);
    FormatAndLog(m_logError, fmt, args);
    va_end(args);
}

} // namespace RE3HT
