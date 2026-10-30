#include "stream_invite.h"
#include "stream.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>

#include <cctype>
#include <cstring>
#include <vector>

namespace openshao::stream {

namespace {

const char kBase64UrlChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string Base64UrlEncode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t val = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
        out.push_back(kBase64UrlChars[(val >> 18) & 0x3F]);
        out.push_back(kBase64UrlChars[(val >> 12) & 0x3F]);
        out.push_back(kBase64UrlChars[(val >> 6) & 0x3F]);
        out.push_back(kBase64UrlChars[val & 0x3F]);
        i += 3;
    }
    if (i + 1 == len) {
        uint32_t val = uint32_t(data[i]) << 16;
        out.push_back(kBase64UrlChars[(val >> 18) & 0x3F]);
        out.push_back(kBase64UrlChars[(val >> 12) & 0x3F]);
    } else if (i + 2 == len) {
        uint32_t val = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(kBase64UrlChars[(val >> 18) & 0x3F]);
        out.push_back(kBase64UrlChars[(val >> 12) & 0x3F]);
        out.push_back(kBase64UrlChars[(val >> 6) & 0x3F]);
    }
    return out;
}

int Base64UrlCharValue(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

bool Base64UrlDecode(std::string_view in, std::vector<uint8_t>& out) {
    if (in.size() % 4 == 1) return false;
    out.clear();
    out.reserve((in.size() * 3) / 4);

    size_t i = 0;
    while (i + 4 <= in.size()) {
        int v0 = Base64UrlCharValue(in[i]);
        int v1 = Base64UrlCharValue(in[i + 1]);
        int v2 = Base64UrlCharValue(in[i + 2]);
        int v3 = Base64UrlCharValue(in[i + 3]);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) return false;
        uint32_t val = (uint32_t(v0) << 18) | (uint32_t(v1) << 12) | (uint32_t(v2) << 6) | uint32_t(v3);
        out.push_back(uint8_t((val >> 16) & 0xFF));
        out.push_back(uint8_t((val >> 8) & 0xFF));
        out.push_back(uint8_t(val & 0xFF));
        i += 4;
    }
    size_t rem = in.size() - i;
    if (rem == 2) {
        int v0 = Base64UrlCharValue(in[i]);
        int v1 = Base64UrlCharValue(in[i + 1]);
        if (v0 < 0 || v1 < 0) return false;
        uint32_t val = (uint32_t(v0) << 18) | (uint32_t(v1) << 12);
        out.push_back(uint8_t((val >> 16) & 0xFF));
    } else if (rem == 3) {
        int v0 = Base64UrlCharValue(in[i]);
        int v1 = Base64UrlCharValue(in[i + 1]);
        int v2 = Base64UrlCharValue(in[i + 2]);
        if (v0 < 0 || v1 < 0 || v2 < 0) return false;
        uint32_t val = (uint32_t(v0) << 18) | (uint32_t(v1) << 12) | (uint32_t(v2) << 6);
        out.push_back(uint8_t((val >> 16) & 0xFF));
        out.push_back(uint8_t((val >> 8) & 0xFF));
    } else if (rem != 0) {
        return false;
    }
    return true;
}

bool ShouldKeepLine(std::string_view line) {
    if (line.starts_with("a=ice-ufrag")) return true;
    if (line.starts_with("a=ice-pwd")) return true;
    if (line.starts_with("a=candidate")) return true;
    if (line.starts_with("a=end-of-candidates")) return true;
    return false;
}

std::string FilterDescription(std::string_view desc) {
    std::string out;
    size_t pos = 0;
    while (pos < desc.size()) {
        size_t next_nl = desc.find('\n', pos);
        std::string_view raw_line = (next_nl == std::string_view::npos)
            ? desc.substr(pos)
            : desc.substr(pos, next_nl - pos);
        pos = (next_nl == std::string_view::npos) ? desc.size() : next_nl + 1;

        std::string_view line = raw_line;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty()) continue;

        if (ShouldKeepLine(line)) {
            out.append(line);
            out.append("\r\n");
        }
    }
    return out;
}

bool ValidDescriptionBytes(std::string_view desc) {
    if (desc.empty()) return false;
    for (char c : desc) {
        if (!((c >= 32 && c <= 126) || c == '\r' || c == '\n')) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string EncodeCode(const ConnectionCode& code) {
    std::string filtered = FilterDescription(code.description);
    if (!ValidDescriptionBytes(filtered)) {
        return {};
    }

    size_t body_size = 6 + filtered.size();
    if (body_size > 4096) {
        return {};
    }

    std::vector<uint8_t> body(body_size);
    uint16_t ver = static_cast<uint16_t>(stream::kVersion);
    body[0] = uint8_t(ver & 0xFF);
    body[1] = uint8_t((ver >> 8) & 0xFF);
    body[2] = uint8_t(code.session & 0xFF);
    body[3] = uint8_t((code.session >> 8) & 0xFF);
    body[4] = uint8_t((code.session >> 16) & 0xFF);
    body[5] = uint8_t((code.session >> 24) & 0xFF);
    std::memcpy(body.data() + 6, filtered.data(), filtered.size());

    mz_ulong dest_len = mz_compressBound(static_cast<mz_ulong>(body.size()));
    std::vector<uint8_t> compressed(dest_len);
    int res = mz_compress2(compressed.data(), &dest_len, body.data(), static_cast<mz_ulong>(body.size()), 9);
    if (res != MZ_OK) {
        return {};
    }
    compressed.resize(dest_len);

    std::string out = "OS";
    out.push_back(static_cast<char>(code.kind));
    out.push_back(static_cast<char>('0' + kCodeVersion));
    out.push_back('-');
    out.append(Base64UrlEncode(compressed.data(), compressed.size()));
    return out;
}

bool DecodeCode(std::string_view text, CodeKind expected, ConnectionCode& code, std::string& error) {
    std::string s;
    s.reserve(text.size());
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            s.push_back(c);
        }
    }

    if (s.size() < 5 || s[0] != 'O' || s[1] != 'S') {
        error = "Not an OpenShao connection code.";
        return false;
    }

    char kind_char = s[2];
    if (kind_char != 'I' && kind_char != 'R') {
        error = "Not an OpenShao connection code.";
        return false;
    }

    if (kind_char != static_cast<char>(expected)) {
        if (kind_char == 'R' && expected == CodeKind::Invite) {
            error = "This is a reply code. Paste it on the host.";
            return false;
        }
        if (kind_char == 'I' && expected == CodeKind::Reply) {
            error = "This is an invite code. Paste it in Join.";
            return false;
        }
        error = "Not an OpenShao connection code.";
        return false;
    }

    char ver_digit = s[3];
    if (ver_digit != static_cast<char>('0' + kCodeVersion)) {
        error = "This code comes from a different OpenShao version. Both players need the same version.";
        return false;
    }

    if (s[4] != '-') {
        error = "Not an OpenShao connection code.";
        return false;
    }

    std::string_view b64 = std::string_view(s).substr(5);
    // Base64 of the largest accepted body; anything longer cannot be a valid code.
    if (b64.empty() || b64.size() > 4096 * 4 / 3 + 4) {
        error = "The connection code is damaged or incomplete.";
        return false;
    }

    std::vector<uint8_t> compressed;
    if (!Base64UrlDecode(b64, compressed)) {
        error = "The connection code is damaged or incomplete.";
        return false;
    }

    uint8_t body[4096];
    mz_ulong uncomp_len = sizeof(body);
    int res = mz_uncompress(body, &uncomp_len, compressed.data(), static_cast<mz_ulong>(compressed.size()));
    if (res != MZ_OK || uncomp_len < 6 || uncomp_len > 4096) {
        error = "The connection code is damaged or incomplete.";
        return false;
    }

    uint16_t body_ver = static_cast<uint16_t>(body[0]) | (static_cast<uint16_t>(body[1]) << 8);
    if (body_ver != static_cast<uint16_t>(stream::kVersion)) {
        error = "This code comes from a different OpenShao version. Both players need the same version.";
        return false;
    }

    uint32_t session = static_cast<uint32_t>(body[2]) |
                       (static_cast<uint32_t>(body[3]) << 8) |
                       (static_cast<uint32_t>(body[4]) << 16) |
                       (static_cast<uint32_t>(body[5]) << 24);

    size_t desc_len = uncomp_len - 6;
    if (desc_len == 0) {
        error = "The connection code is damaged or incomplete.";
        return false;
    }

    std::string_view desc_view(reinterpret_cast<const char*>(body + 6), desc_len);
    if (!ValidDescriptionBytes(desc_view)) {
        error = "The connection code is damaged or incomplete.";
        return false;
    }

    code.kind = expected;
    code.session = session;
    code.description.assign(desc_view);
    return true;
}

} // namespace openshao::stream
