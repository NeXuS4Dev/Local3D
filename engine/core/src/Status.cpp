#include "local3d/core/Status.hpp"

#include "local3d/core/Enum.hpp"

#include <string>

namespace l3d {

std::string_view StatusCodeName(StatusCode code) noexcept {
    static constexpr std::pair<StatusCode, std::string_view> table[] = {
        {StatusCode::Ok, "Ok"},
        {StatusCode::InvalidArgument, "InvalidArgument"},
        {StatusCode::InvalidState, "InvalidState"},
        {StatusCode::NotFound, "NotFound"},
        {StatusCode::AlreadyExists, "AlreadyExists"},
        {StatusCode::OutOfRange, "OutOfRange"},
        {StatusCode::OutOfMemory, "OutOfMemory"},
        {StatusCode::IoError, "IoError"},
        {StatusCode::ParseError, "ParseError"},
        {StatusCode::Unsupported, "Unsupported"},
        {StatusCode::Timeout, "Timeout"},
        {StatusCode::NotInitialized, "NotInitialized"},
        {StatusCode::DeviceLost, "DeviceLost"},
        {StatusCode::Cancelled, "Cancelled"},
        {StatusCode::Internal, "Internal"},
    };
    return EnumName(code, table, "UnknownStatus");
}

std::string Status::ToString() const {
    std::string out(StatusCodeName(code_));
    if (messageLength_ > 0) {
        out.append(": ");
        out.append(Message());
    }
    return out;
}

} // namespace l3d
