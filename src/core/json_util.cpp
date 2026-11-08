#include "json_util.h"

#include <cstdlib>
#include <cstring>

#include "rapidjson/error/en.h"

namespace vt {

const JVal* jget(const JVal& v, const char* key) {
    if (!v.IsObject()) return nullptr;
    auto it = v.FindMember(key);
    return it == v.MemberEnd() ? nullptr : &it->value;
}

const JVal* jget(const JVal* v, const char* key) { return v ? jget(*v, key) : nullptr; }

const JVal* jpath(const JVal& root, const char* path) {
    const JVal* cur = &root;
    const char* p = path;
    char part[96];
    while (cur && *p) {
        size_t n = 0;
        while (p[n] && p[n] != '.') n++;
        if (n >= sizeof(part)) return nullptr;
        memcpy(part, p, n);
        part[n] = 0;
        p += n;
        if (*p == '.') p++;
        if (cur->IsArray() && part[0] >= '0' && part[0] <= '9') {
            unsigned idx = (unsigned)atoi(part);
            cur = idx < cur->Size() ? &(*cur)[idx] : nullptr;
        } else {
            cur = jget(*cur, part);
        }
    }
    return cur;
}

const JVal* jpath(const JVal* v, const char* path) { return v ? jpath(*v, path) : nullptr; }

std::string jstr(const JVal* v) {
    if (!v) return std::string();
    if (v->IsString()) return std::string(v->GetString(), v->GetStringLength());
    if (v->IsInt64()) return std::to_string(v->GetInt64());
    if (v->IsUint64()) return std::to_string(v->GetUint64());
    if (v->IsDouble()) return std::to_string(v->GetDouble());
    if (v->IsBool()) return v->GetBool() ? "true" : "false";
    return std::string();
}

std::string jstr(const JVal& v, const char* path) { return jstr(jpath(v, path)); }

long long jint(const JVal* v, long long def) {
    if (!v) return def;
    if (v->IsInt64()) return v->GetInt64();
    if (v->IsUint64()) return (long long)v->GetUint64();
    if (v->IsDouble()) return (long long)v->GetDouble();
    if (v->IsString()) {
        const char* s = v->GetString();
        char* end = nullptr;
        long long r = strtoll(s, &end, 10);
        return end != s ? r : def;
    }
    return def;
}

long long jint(const JVal& v, const char* path, long long def) { return jint(jpath(v, path), def); }

bool jbool(const JVal& v, const char* path, bool def) {
    const JVal* x = jpath(v, path);
    if (!x) return def;
    if (x->IsBool()) return x->GetBool();
    if (x->IsString()) return strcmp(x->GetString(), "true") == 0;
    return def;
}

std::string jtext(const JVal* v) {
    if (!v) return std::string();
    if (v->IsString()) return jstr(v);
    if (const JVal* s = jget(*v, "simpleText")) return jstr(s);
    if (const JVal* c = jget(*v, "content")) {
        if (c->IsString()) return jstr(c);
    }
    if (const JVal* runs = jget(*v, "runs")) {
        std::string out;
        if (runs->IsArray())
            for (auto& r : runs->GetArray()) out += jstr(jget(r, "text"));
        return out;
    }
    return std::string();
}

static const JVal* jfind_rec(const JVal& v, const char* key, int depth) {
    if (depth <= 0) return nullptr;
    if (v.IsObject()) {
        for (auto& m : v.GetObject()) {
            if (strcmp(m.name.GetString(), key) == 0) return &m.value;
        }
        for (auto& m : v.GetObject()) {
            if (m.value.IsObject() || m.value.IsArray()) {
                if (const JVal* r = jfind_rec(m.value, key, depth - 1)) return r;
            }
        }
    } else if (v.IsArray()) {
        for (auto& e : v.GetArray()) {
            if (const JVal* r = jfind_rec(e, key, depth - 1)) return r;
        }
    }
    return nullptr;
}

const JVal* jfind(const JVal& v, const char* key, int maxDepth) { return jfind_rec(v, key, maxDepth); }

static void jwalk_rec(const JVal& v, const std::function<bool(const char*, const JVal&)>& fn, int depth) {
    if (depth > 120) return;
    if (v.IsObject()) {
        for (auto& m : v.GetObject()) {
            if (fn(m.name.GetString(), m.value) && (m.value.IsObject() || m.value.IsArray()))
                jwalk_rec(m.value, fn, depth + 1);
        }
    } else if (v.IsArray()) {
        for (auto& e : v.GetArray()) {
            if (fn(nullptr, e) && (e.IsObject() || e.IsArray())) jwalk_rec(e, fn, depth + 1);
        }
    }
}

void jwalk(const JVal& v, const std::function<bool(const char*, const JVal&)>& fn) { jwalk_rec(v, fn, 0); }

JDocPtr jparse(const std::string& text, std::string* err) {
    auto doc = std::make_shared<JDoc>();
    // Iterative parsing keeps stack use flat on the Vita's small thread stacks.
    doc->Parse<rapidjson::kParseIterativeFlag>(text.c_str(), text.size());
    if (doc->HasParseError()) {
        if (err)
            *err = std::string("JSON parse error: ") + rapidjson::GetParseError_En(doc->GetParseError()) +
                   " at " + std::to_string(doc->GetErrorOffset());
        return nullptr;
    }
    return doc;
}

std::string json_string(const std::string& s) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.String(s.c_str(), (rapidjson::SizeType)s.size());
    return std::string(sb.GetString(), sb.GetSize());
}

std::string jserialize(const JVal& v) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    v.Accept(w);
    return std::string(sb.GetString(), sb.GetSize());
}

}  // namespace vt
