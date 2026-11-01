module;

#include <json/json.h>

#include <optional>
#include <sstream>
#include <string>
#include <string_view>

export module JsonHelpers;

export namespace webrtc_poc {

inline std::string JsonDump(const Json::Value& value) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    auto s = Json::writeString(w, value);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

inline std::optional<Json::Value> JsonParse(std::string_view text) {
    Json::CharReaderBuilder r;
    r["collectComments"] = false;
    std::string errs;
    Json::Value v;
    std::istringstream ss{std::string(text)};
    if (!Json::parseFromStream(r, ss, &v, &errs)) return std::nullopt;
    return v;
}

inline std::string JsonStr(const Json::Value& v, const char* key) {
    if (v.isObject() && v.isMember(key) && v[key].isString()) return v[key].asString();
    return {};
}

inline int JsonInt(const Json::Value& v, const char* key, int def = 0) {
    if (v.isObject() && v.isMember(key) && v[key].isNumeric()) return v[key].asInt();
    return def;
}

inline std::string Truncate(std::string_view text, size_t len = 120) {
    if (text.size() <= len) return std::string(text);
    return std::string(text.substr(0, len)) + "...";
}

}  // namespace webrtc_poc
