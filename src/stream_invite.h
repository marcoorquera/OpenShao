#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace openshao::stream {

inline constexpr uint8_t kCodeVersion = 1;

enum class CodeKind : uint8_t { Invite = 'I', Reply = 'R' };

struct ConnectionCode {
    CodeKind kind = CodeKind::Invite;
    uint32_t session = 0;     // random per invite; a reply repeats its invite's session
    std::string description;  // libjuice local description (SDP lines)
};

// "OS" + kind letter + kCodeVersion digit + "-" + base64url (no padding) of the deflated body.
std::string EncodeCode(const ConnectionCode& code);
// Ignores whitespace anywhere (chat apps wrap long text). Fails with a user-facing English message:
// not an OpenShao code, wrong kind, made by another version, damaged.
bool DecodeCode(std::string_view text, CodeKind expected, ConnectionCode& code, std::string& error);

} // namespace openshao::stream
