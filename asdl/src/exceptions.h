#ifndef ASDL_EXCEPTIONS_H
#define ASDL_EXCEPTIONS_H

#include <exception>
#include <string>

class ASDLException : public std::exception {
private:
    std::string message;
public:
    explicit ASDLException(const std::string& msg) : message(msg) {}
    const char* what() const noexcept override { return message.c_str(); }
};

class FileException : public ASDLException {
public:
    explicit FileException(const std::string& filename, const std::string& operation)
        : ASDLException("Failed to " + operation + " file: " + filename) {}
};

class ParseException : public ASDLException {
public:
    explicit ParseException(const std::string& msg) : ASDLException("Parse error: " + msg) {}
};

class TemplateException : public ASDLException {
public:
    explicit TemplateException(const std::string& msg) : ASDLException("Template error: " + msg) {}
};

#endif // ASDL_EXCEPTIONS_H
