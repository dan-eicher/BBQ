#include "FrameProcessor.h"

namespace bbqgen {

FrameProcessor::FrameProcessor(const std::string& frame_path, std::ostream& out)
    : out_(out) {
    file_stream_.open(frame_path);
    if (file_stream_.is_open()) {
        frame_ = &file_stream_;
        ok_ = true;
    }
}

FrameProcessor::FrameProcessor(std::istream& frame_stream, std::ostream& out)
    : frame_(&frame_stream), out_(out), ok_(true) {}

bool FrameProcessor::CopyFramePart(const std::string& marker) {
    return AdvanceTo(marker, true);
}

bool FrameProcessor::SkipFramePart(const std::string& marker) {
    return AdvanceTo(marker, false);
}

void FrameProcessor::Emit(const std::string& code) {
    out_ << code;
}

void FrameProcessor::EmitLine(const std::string& code) {
    out_ << code << "\n";
}

void FrameProcessor::EmitLine() {
    out_ << "\n";
}

bool FrameProcessor::AdvanceTo(const std::string& marker, bool copy) {
    if (!ok_ || !frame_) return false;

    std::string search = "-->" + marker;
    std::string line;
    while (std::getline(*frame_, line)) {
        // Check if this line contains the marker
        auto pos = line.find(search);
        if (pos != std::string::npos) {
            // Output everything before the marker on this line
            if (copy && pos > 0) {
                out_ << line.substr(0, pos);
            }
            return true;
        }
        if (copy) {
            out_ << line << "\n";
        }
    }
    return false; // marker not found
}

} // namespace bbqgen
