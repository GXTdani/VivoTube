// Thin helpers over RapidJSON for digging through InnerTube responses, which are deep,
// change shape often, and mix "simpleText", "runs" and view-model "content" strings.
#pragma once

#define RAPIDJSON_HAS_STDSTRING 1
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <functional>
#include <memory>
#include <string>

namespace vt {

using JVal = rapidjson::Value;
using JDoc = rapidjson::Document;
using JDocPtr = std::shared_ptr<JDoc>;

// Member lookup; nullptr when v is not an object or key is missing.
const JVal* jget(const JVal& v, const char* key);
const JVal* jget(const JVal* v, const char* key);
// Dotted path lookup; numeric components index arrays ("a.0.b").
const JVal* jpath(const JVal& v, const char* path);
const JVal* jpath(const JVal* v, const char* path);

// String value (numbers and bools are stringified); "" otherwise.
std::string jstr(const JVal* v);
std::string jstr(const JVal& v, const char* path);
long long jint(const JVal* v, long long def = 0);  // accepts numeric strings too
long long jint(const JVal& v, const char* path, long long def = 0);
bool jbool(const JVal& v, const char* path, bool def = false);

// Text from {simpleText}, {runs:[{text}]}, {content}, or a plain string.
std::string jtext(const JVal* v);

// Depth-first search for the first value stored under key (objects only).
const JVal* jfind(const JVal& v, const char* key, int maxDepth = 48);

// Pre-order walk. fn(key, value) is called for every object member (key = member name)
// and array element (key = nullptr). Return true to descend into value.
void jwalk(const JVal& v, const std::function<bool(const char* key, const JVal& val)>& fn);

// Parse helper; returns nullptr on error and fills *err.
JDocPtr jparse(const std::string& text, std::string* err);

std::string jserialize(const JVal& v);
// A JSON string literal (quoted and escaped) for building request bodies.
std::string json_string(const std::string& s);

}  // namespace vt
