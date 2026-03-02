#pragma once

#include <string>
#include <fstream>
#include <sstream>

namespace bbqgen {

class FrameProcessor {
public:
    // Construct from a frame file path and output stream
    FrameProcessor(const std::string& frame_path, std::ostream& out);

    // Construct from frame content string (for testing)
    FrameProcessor(std::istream& frame_stream, std::ostream& out);

    // Copy frame content to output until marker is found (exclusive).
    // Returns false if marker not found.
    bool CopyFramePart(const std::string& marker);

    // Skip frame content until marker is found.
    bool SkipFramePart(const std::string& marker);

    // Write directly to output.
    void Emit(const std::string& code);
    void EmitLine(const std::string& code);
    void EmitLine();

    // Check if frame was loaded successfully
    bool ok() const { return ok_; }

private:
    bool AdvanceTo(const std::string& marker, bool copy);
    std::istream* frame_;
    std::ifstream file_stream_;
    std::ostream& out_;
    bool ok_ = false;
};

} // namespace bbqgen
