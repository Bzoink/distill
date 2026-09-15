#include <quickjs.h>

#include <lexbor/dom/interfaces/document.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/html/interfaces/element.h>
#include <lexbor/html/serialize.h>

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Engine;
struct NodeRef { Engine *engine; lxb_dom_node_t *node; };
struct StorageRef { Engine *engine; bool session; };
struct ObserverRef { Engine *engine; JSValue callback; bool intersection; bool observed; };
struct XhrRef { Engine *engine; };

static JSClassID node_class;
static JSClassID dataset_class;
static JSClassID storage_class;
static JSClassID observer_class;
static JSClassID xhr_class;
static JSClassExoticMethods node_exotic;

static std::string lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static std::string js_string(JSContext *ctx, JSValueConst value)
{
    size_t len = 0;
    const char *p = JS_ToCStringLen(ctx, &len, value);
    if (!p) return {};
    std::string out(p, len);
    JS_FreeCString(ctx, p);
    return out;
}

static JSValue js_str(JSContext *ctx, const std::string &s)
{
    return JS_NewStringLen(ctx, s.data(), s.size());
}

static std::string path_read(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

static uint64_t now_ms()
{
    static const auto origin = Clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - origin).count());
}

static std::string percent_decode_path(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char *end = nullptr;
            long v = std::strtol(s.substr(i + 1, 2).c_str(), &end, 16);
            if (end && *end == '\0') { out.push_back(static_cast<char>(v)); i += 2; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
}

static bool is_http_url(const std::string &url)
{
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

static std::string url_path(const std::string &url)
{
    size_t scheme = url.find("://");
    size_t start = scheme == std::string::npos ? 0 : url.find('/', scheme + 3);
    if (start == std::string::npos) return "/";
    size_t end = url.find_first_of("?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

static std::string url_origin(const std::string &url)
{
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    size_t end = url.find_first_of("/?#", scheme + 3);
    return url.substr(0, end == std::string::npos ? std::string::npos : end);
}

static std::string normalize_path(std::string path)
{
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == ".." && !parts.empty()) parts.pop_back();
        else if (part != "..") parts.push_back(part);
    }
    std::string out = "/";
    for (size_t i = 0; i < parts.size(); ++i) { if (i) out += '/'; out += parts[i]; }
    if (path.size() > 1 && path.back() == '/') out += '/';
    return out;
}

static std::string resolve_url(const std::string &base, const std::string &candidate)
{
    if (candidate.empty()) return base;
    if (candidate.find("://") != std::string::npos || candidate.rfind("data:", 0) == 0 || candidate.rfind("file:", 0) == 0)
        return candidate;
    if (candidate.rfind("//", 0) == 0) {
        size_t scheme = base.find(':');
        return (scheme == std::string::npos ? "http:" : base.substr(0, scheme + 1)) + candidate;
    }
    std::string origin = url_origin(base);
    if (!origin.empty()) {
        if (candidate[0] == '/') return origin + normalize_path(candidate);
        std::string p = url_path(base);
        size_t slash = p.rfind('/');
        return origin + normalize_path((slash == std::string::npos ? "/" : p.substr(0, slash + 1)) + candidate);
    }
    fs::path b = base;
    return (b.parent_path() / candidate).lexically_normal().string();
}

static std::string file_from_url(const std::string &url)
{
    if (url.rfind("file://", 0) == 0) return percent_decode_path(url.substr(7));
    return url;
}

static bool attr_equals(lxb_dom_element_t *el, const char *name, const std::string &value)
{
    size_t len = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, reinterpret_cast<const lxb_char_t *>(name), std::strlen(name), &len);
    return v && std::string(reinterpret_cast<const char *>(v), len) == value;
}

static std::string attr_value(lxb_dom_element_t *el, const std::string &name)
{
    size_t len = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, reinterpret_cast<const lxb_char_t *>(name.data()), name.size(), &len);
    return v ? std::string(reinterpret_cast<const char *>(v), len) : std::string();
}

static bool is_element(lxb_dom_node_t *node)
{
    return node && node->type == LXB_DOM_NODE_TYPE_ELEMENT;
}

static std::string node_tag(lxb_dom_node_t *node)
{
    if (!is_element(node)) return {};
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    const lxb_char_t *p = lxb_dom_element_local_name(el, nullptr);
    if (!p) return {};
    return lower(reinterpret_cast<const char *>(p));
}

static std::vector<lxb_dom_node_t *> child_elements(lxb_dom_node_t *node)
{
    std::vector<lxb_dom_node_t *> out;
    for (lxb_dom_node_t *p = node ? node->first_child : nullptr; p; p = p->next) if (is_element(p)) out.push_back(p);
    return out;
}

static bool class_has(lxb_dom_element_t *el, const std::string &name)
{
    std::stringstream ss(attr_value(el, "class"));
    std::string item;
    while (ss >> item) if (item == name) return true;
    return false;
}

static bool selector_match(lxb_dom_node_t *node, std::string selector)
{
    if (!is_element(node)) return false;
    selector = trim(selector);
    if (selector.empty()) return false;
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    std::string tag, id, cls;
    size_t hash = selector.find('#');
    size_t dot = selector.find('.');
    size_t attr = selector.find('[');
    size_t end_tag = std::min({hash == std::string::npos ? selector.size() : hash,
                               dot == std::string::npos ? selector.size() : dot,
                               attr == std::string::npos ? selector.size() : attr});
    tag = lower(selector.substr(0, end_tag));
    if (!tag.empty() && tag != "*" && tag != node_tag(node)) return false;
    if (hash != std::string::npos) {
        size_t end = dot != std::string::npos && dot > hash ? dot : (attr != std::string::npos && attr > hash ? attr : selector.size());
        id = selector.substr(hash + 1, end - hash - 1);
        if (attr_value(el, "id") != id) return false;
    }
    if (dot != std::string::npos) {
        size_t end = attr != std::string::npos && attr > dot ? attr : selector.size();
        cls = selector.substr(dot + 1, end - dot - 1);
        if (!class_has(el, cls)) return false;
    }
    if (attr != std::string::npos) {
        size_t close = selector.find(']', attr);
        std::string name = selector.substr(attr + 1, close == std::string::npos ? std::string::npos : close - attr - 1);
        size_t eq = name.find('=');
        if (eq != std::string::npos) {
            std::string val = name.substr(eq + 1);
            if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\''))) val = val.substr(1, val.size() - 2);
            if (attr_value(el, trim(name.substr(0, eq))) != val) return false;
        } else if (attr_value(el, trim(name)).empty()) return false;
    }
    return true;
}

static void walk(lxb_dom_node_t *node, const std::function<void(lxb_dom_node_t *)> &fn)
{
    if (!node) return;
    fn(node);
    for (lxb_dom_node_t *p = node->first_child; p; p = p->next) walk(p, fn);
}

struct Timer {
    uint64_t id = 0;
    uint64_t due = 0;
    int interval = 0;
    JSValue callback = JS_UNDEFINED;
    JSValue arg = JS_UNDEFINED;
    bool cancelled = false;
    bool recurring = false;
};

struct Listener { bool window; std::string type; JSValue callback = JS_UNDEFINED; };

struct Request {
    enum Kind { Fetch, Xhr, AsyncScript } kind;
    Engine *engine = nullptr;
    CURL *easy = nullptr;
    std::string url;
    std::vector<unsigned char> body;
    long status = 0;
    JSValue resolve = JS_UNDEFINED;
    JSValue reject = JS_UNDEFINED;
    JSValue xhr = JS_UNDEFINED;
    bool completed = false;
};

struct Engine {
    JSRuntime *rt = nullptr;
    JSContext *ctx = nullptr;
    lxb_html_document_t *document = nullptr;
    CURLM *multi = nullptr;
    std::string source_url;
    std::string base_url;
    uint64_t started = 0;
    uint64_t cap_ms = 5000;
    uint64_t memory_limit = 128 * 1024 * 1024;
    uint64_t max_scripts = 1000;
    uint64_t scripts_run = 0;
    uint64_t next_timer = 1;
    bool interrupted = false;
    bool cap_hit = false;
    std::string ready_state = "loading";
    bool load_dispatched = false;
    int async_pending = 0;
    std::vector<Timer> timers;
    std::vector<Listener> listeners;
    std::vector<std::unique_ptr<Request>> requests;
    std::vector<ObserverRef *> observers;
    std::unordered_map<lxb_dom_node_t *, NodeRef *> refs;
    std::unordered_map<lxb_dom_node_t *, JSValue> wrappers;
    std::map<std::string, std::string> local_storage;
    std::map<std::string, std::string> session_storage;
    std::string cookie;
    std::ofstream telemetry_file;
    std::map<std::string, uint64_t> telemetry_counts;
    JSValue global = JS_UNDEFINED;
    JSValue document_value = JS_UNDEFINED;

    void telemetry(const std::string &event, const std::string &detail = {})
    {
        telemetry_counts[event]++;
        if (telemetry_file) telemetry_file << "{\"event\":\"" << event << "\",\"detail\":\"" << detail << "\"}\n";
        std::cerr << "telemetry " << event << (detail.empty() ? "" : " ") << detail << "\n";
    }

    bool expired() const { return now_ms() - started >= cap_ms; }
    void note_error(const char *where)
    {
        if (!JS_HasException(ctx)) return;
        JSValue ex = JS_GetException(ctx);
        std::string msg = js_string(ctx, ex);
        JSValue stack = JS_GetPropertyStr(ctx, ex, "stack");
        std::string stack_text = js_string(ctx, stack);
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, ex);
        std::cerr << "script error [" << where << "]: " << msg << (stack_text.empty() ? "" : "\n" + stack_text) << "\n";
    }
};

static Engine *engine_from_runtime(JSRuntime *rt) { return static_cast<Engine *>(JS_GetRuntimeOpaque(rt)); }
static Engine *engine_from_context(JSContext *ctx) { return static_cast<Engine *>(JS_GetContextOpaque(ctx)); }
static NodeRef *node_ref(JSContext *ctx, JSValueConst value)
{
    return static_cast<NodeRef *>(JS_GetOpaque2(ctx, value, node_class));
}
static lxb_dom_node_t *node_value(JSContext *ctx, JSValueConst value)
{
    NodeRef *ref = node_ref(ctx, value);
    return ref ? ref->node : nullptr;
}
static StorageRef *storage_ref(JSContext *ctx, JSValueConst value)
{
    return static_cast<StorageRef *>(JS_GetOpaque2(ctx, value, storage_class));
}
static ObserverRef *observer_ref(JSContext *ctx, JSValueConst value)
{
    return static_cast<ObserverRef *>(JS_GetOpaque2(ctx, value, observer_class));
}
static XhrRef *xhr_ref(JSContext *ctx, JSValueConst value)
{
    return static_cast<XhrRef *>(JS_GetOpaque2(ctx, value, xhr_class));
}

static JSValue make_node(Engine *e, lxb_dom_node_t *node);
static void notify_mutation(Engine *e, lxb_dom_node_t *target);
static JSValue plain_object(Engine *e);
static JSValue make_promise(Engine *e, JSValue *resolve, JSValue *reject);
static bool eval_script(Engine *e, const std::string &source, const std::string &filename, int flags = JS_EVAL_TYPE_GLOBAL);
static bool fetch_sync(Engine *e, const std::string &url, std::vector<unsigned char> &body, long *status);

static JSValue node_unimplemented_get(JSContext *ctx, JSValueConst obj, JSAtom atom, JSValueConst)
{
    NodeRef *ref = static_cast<NodeRef *>(JS_GetOpaque(obj, node_class));
    const char *name = JS_AtomToCString(ctx, atom);
    if (ref) ref->engine->telemetry("unimplemented.dom", name ? name : "symbol");
    if (name) JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue unimplemented_callback(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    Engine *e = engine_from_context(ctx);
    e->telemetry("unimplemented.dom", argc ? js_string(ctx, argv[0]) : "unknown");
    return JS_UNDEFINED;
}

static int node_unimplemented_set(JSContext *ctx, JSValueConst obj, JSAtom atom, JSValueConst, JSValueConst, int)
{
    NodeRef *ref = static_cast<NodeRef *>(JS_GetOpaque(obj, node_class));
    const char *name = JS_AtomToCString(ctx, atom);
    if (ref) ref->engine->telemetry("unimplemented.dom", name ? name : "symbol");
    if (name) JS_FreeCString(ctx, name);
    return 1;
}

static JSValue make_array(Engine *e, const std::vector<JSValue> &values)
{
    JSValue array = JS_NewArray(e->ctx);
    for (uint32_t i = 0; i < values.size(); ++i) JS_SetPropertyUint32(e->ctx, array, i, JS_DupValue(e->ctx, values[i]));
    return array;
}

static std::string node_text(lxb_dom_node_t *node)
{
    size_t len = 0;
    lxb_char_t *value = lxb_dom_node_text_content(node, &len);
    return value ? std::string(reinterpret_cast<char *>(value), len) : std::string();
}

static void set_attribute(lxb_dom_element_t *element, const std::string &name, const std::string &value)
{
    lxb_dom_element_set_attribute(element, reinterpret_cast<const lxb_char_t *>(name.data()), name.size(),
                                   reinterpret_cast<const lxb_char_t *>(value.data()), value.size());
}

static JSValue get_node_property(JSContext *ctx, JSValueConst this_val, const char *name);
static int set_node_property(JSContext *ctx, JSValueConst this_val, const char *name, JSValueConst value);

static void node_finalizer(JSRuntime *, JSValueConst value)
{
    NodeRef *ref = static_cast<NodeRef *>(JS_GetOpaque(value, node_class));
    delete ref;
}

static void storage_finalizer(JSRuntime *, JSValueConst value)
{
    delete static_cast<StorageRef *>(JS_GetOpaque(value, storage_class));
}

static void observer_finalizer(JSRuntime *rt, JSValueConst value)
{
    ObserverRef *ref = static_cast<ObserverRef *>(JS_GetOpaque(value, observer_class));
    if (ref) {
        Engine *e = engine_from_runtime(rt);
        if (e && e->ctx) JS_FreeValue(e->ctx, ref->callback);
        if (e) e->observers.erase(std::remove(e->observers.begin(), e->observers.end(), ref), e->observers.end());
        delete ref;
    }
}

static void xhr_finalizer(JSRuntime *, JSValueConst value)
{
    delete static_cast<XhrRef *>(JS_GetOpaque(value, xhr_class));
}

static JSValue node_get_text(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *node = node_value(ctx, this_val);
    return node ? js_str(ctx, node_text(node)) : JS_UNDEFINED;
}

static int node_set_text(JSContext *ctx, JSValueConst this_val, JSValueConst value)
{
    NodeRef *ref = node_ref(ctx, this_val);
    if (!ref || !ref->node) return -1;
    std::string text = js_string(ctx, value);
    lxb_dom_node_t *node = ref->node;
    for (lxb_dom_node_t *child = node->first_child; child;) {
        lxb_dom_node_t *next = child->next;
        lxb_dom_node_remove_wo_events(child);
        child = next;
    }
    lxb_dom_node_text_content_set(node, reinterpret_cast<const lxb_char_t *>(text.data()), text.size());
    notify_mutation(ref->engine, node);
    return 0;
}

static std::string serialize_children(lxb_dom_node_t *node)
{
    lexbor_str_t str = {0};
    for (lxb_dom_node_t *child = node ? node->first_child : nullptr; child; child = child->next)
        lxb_html_serialize_tree_str(child, &str);
    return str.data ? std::string(reinterpret_cast<char *>(str.data), str.length) : std::string();
}

static JSValue node_get_inner_html(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *node = node_value(ctx, this_val);
    return node ? js_str(ctx, serialize_children(node)) : JS_UNDEFINED;
}

static int node_set_inner_html(JSContext *ctx, JSValueConst this_val, JSValueConst value)
{
    NodeRef *ref = node_ref(ctx, this_val);
    if (!ref || !ref->node || !is_element(ref->node)) return -1;
    std::string html = js_string(ctx, value);
    lxb_dom_node_t *node = ref->node;
    for (lxb_dom_node_t *child = node->first_child; child;) {
        lxb_dom_node_t *next = child->next;
        lxb_dom_node_remove_wo_events(child);
        child = next;
    }
    lxb_dom_node_t *fragment = lxb_html_document_parse_fragment(ref->engine->document,
        lxb_dom_interface_element(node), reinterpret_cast<const lxb_char_t *>(html.data()), html.size());
    if (fragment) {
        for (lxb_dom_node_t *child = fragment->first_child; child;) {
            lxb_dom_node_t *next = child->next;
            lxb_dom_node_append_child(node, child);
            child = next;
        }
    }
    notify_mutation(ref->engine, node);
    return 0;
}

static JSValue node_get_first_element_child(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *node = node_value(ctx, this_val);
    if (!node) return JS_NULL;
    for (lxb_dom_node_t *child = node->first_child; child; child = child->next) if (is_element(child)) return make_node(engine_from_context(ctx), child);
    return JS_NULL;
}

static JSValue node_get_parent(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *node = node_value(ctx, this_val);
    return node && node->parent ? make_node(engine_from_context(ctx), node->parent) : JS_NULL;
}

static JSValue node_get_children(JSContext *ctx, JSValueConst this_val)
{
    Engine *e = engine_from_context(ctx);
    std::vector<JSValue> values;
    for (lxb_dom_node_t *child = node_value(ctx, this_val) ? node_value(ctx, this_val)->first_child : nullptr; child; child = child->next)
        if (is_element(child)) values.push_back(make_node(e, child));
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < values.size(); ++i) JS_SetPropertyUint32(ctx, array, i, values[i]);
    JS_SetPropertyStr(ctx, array, "length", JS_NewInt32(ctx, static_cast<int32_t>(values.size())));
    return array;
}

static JSValue node_get_attr(JSContext *ctx, JSValueConst this_val, const char *name)
{
    lxb_dom_node_t *node = node_value(ctx, this_val);
    if (!is_element(node)) return JS_UNDEFINED;
    std::string value = attr_value(lxb_dom_interface_element(node), name);
    return js_str(ctx, value);
}

static JSValue node_get_name(JSContext *ctx, JSValueConst this_val, bool upper)
{
    lxb_dom_node_t *node = node_value(ctx, this_val);
    if (!node) return JS_UNDEFINED;
    if (!is_element(node)) {
        if (node->type == LXB_DOM_NODE_TYPE_DOCUMENT) return js_str(ctx, "#document");
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) return js_str(ctx, "#text");
        return js_str(ctx, "#document-fragment");
    }
    std::string name = node_tag(node);
    if (upper) for (char &c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return js_str(ctx, name);
}

static JSValue classlist_method(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int magic, JSValueConst *data)
{
    if (argc < 1) return JS_UNDEFINED;
    lxb_dom_node_t *node = node_value(ctx, data[0]);
    if (!node || !is_element(node)) return JS_UNDEFINED;
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    std::string item = js_string(ctx, argv[0]);
    std::vector<std::string> classes;
    std::stringstream ss(attr_value(el, "class"));
    std::string token;
    while (ss >> token) classes.push_back(token);
    auto it = std::find(classes.begin(), classes.end(), item);
    if (magic == 0) { if (it == classes.end()) classes.push_back(item); }
    else if (magic == 1) { if (it != classes.end()) classes.erase(it); }
    else if (magic == 2) return JS_NewBool(ctx, it != classes.end());
    std::string joined;
    for (const auto &c : classes) { if (!joined.empty()) joined += ' '; joined += c; }
    set_attribute(el, "class", joined);
    notify_mutation(engine_from_context(ctx), node);
    return JS_UNDEFINED;
}

static JSValue node_get_class_list(JSContext *ctx, JSValueConst this_val)
{
    JSValue object = JS_NewObject(ctx);
    JSValue data[1] = { JS_DupValue(ctx, this_val) };
    JSValue add = JS_NewCFunctionData(ctx, classlist_method, 1, 0, 1, data);
    JSValue remove = JS_NewCFunctionData(ctx, classlist_method, 1, 1, 1, data);
    JSValue contains = JS_NewCFunctionData(ctx, classlist_method, 1, 2, 1, data);
    JS_SetPropertyStr(ctx, object, "add", add);
    JS_SetPropertyStr(ctx, object, "remove", remove);
    JS_SetPropertyStr(ctx, object, "contains", contains);
    JS_FreeValue(ctx, data[0]);
    return object;
}

static std::string camel_to_data(const std::string &key)
{
    std::string out = "data-";
    for (char c : key) { if (std::isupper(static_cast<unsigned char>(c))) out += '-'; out += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return out;
}

static JSValue dataset_get_property(JSContext *ctx, JSValueConst obj, JSAtom atom, JSValueConst)
{
    NodeRef *ref = static_cast<NodeRef *>(JS_GetOpaque(JS_GetPropertyStr(ctx, obj, "__node"), node_class));
    JSValue key_value = JS_AtomToString(ctx, atom);
    std::string key = js_string(ctx, key_value);
    JS_FreeValue(ctx, key_value);
    if (!ref || !ref->node || !is_element(ref->node)) return JS_UNDEFINED;
    std::string value = attr_value(lxb_dom_interface_element(ref->node), camel_to_data(key));
    return value.empty() ? JS_UNDEFINED : js_str(ctx, value);
}

static int dataset_set_property(JSContext *ctx, JSValueConst obj, JSAtom atom, JSValueConst value, JSValueConst, int)
{
    JSValue node_value_obj = JS_GetPropertyStr(ctx, obj, "__node");
    NodeRef *ref = static_cast<NodeRef *>(JS_GetOpaque(node_value_obj, node_class));
    JS_FreeValue(ctx, node_value_obj);
    JSValue key_value = JS_AtomToString(ctx, atom);
    std::string key = js_string(ctx, key_value);
    JS_FreeValue(ctx, key_value);
    if (!ref || !ref->node || !is_element(ref->node)) return 0;
    std::string value_string = js_string(ctx, value);
    set_attribute(lxb_dom_interface_element(ref->node), camel_to_data(key), value_string);
    notify_mutation(ref->engine, ref->node);
    return 1;
}

static JSValue node_get_dataset(JSContext *ctx, JSValueConst this_val)
{
    NodeRef *ref = node_ref(ctx, this_val);
    if (!ref) return JS_UNDEFINED;
    JSValue object = JS_NewObjectClass(ctx, dataset_class);
    JS_SetOpaque(object, ref->engine);
    JSValue node = JS_DupValue(ctx, this_val);
    JS_DefinePropertyValueStr(ctx, object, "__node", node, JS_PROP_CONFIGURABLE);
    return object;
}

static void find_selector(lxb_dom_node_t *root, const std::string &selector, std::vector<lxb_dom_node_t *> &out)
{
    walk(root, [&](lxb_dom_node_t *node) { if (selector_match(node, selector)) out.push_back(node); });
}

static JSValue node_query_selector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;
    Engine *e = engine_from_context(ctx);
    std::vector<lxb_dom_node_t *> found;
    find_selector(node_value(ctx, this_val), js_string(ctx, argv[0]), found);
    return found.empty() ? JS_NULL : make_node(e, found.front());
}

static JSValue node_query_selector_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NewArray(ctx);
    Engine *e = engine_from_context(ctx);
    std::vector<lxb_dom_node_t *> found;
    find_selector(node_value(ctx, this_val), js_string(ctx, argv[0]), found);
    JSValue out = JS_NewArray(ctx);
    for (uint32_t i = 0; i < found.size(); ++i) JS_SetPropertyUint32(ctx, out, i, make_node(e, found[i]));
    return out;
}

static JSValue node_append_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "appendChild requires a node");
    lxb_dom_node_t *parent = node_value(ctx, this_val);
    lxb_dom_node_t *child = node_value(ctx, argv[0]);
    if (!parent || !child) return JS_ThrowTypeError(ctx, "not a DOM node");
    lxb_dom_node_append_child(parent, child);
    notify_mutation(engine_from_context(ctx), parent);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue node_remove_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "removeChild requires a node");
    lxb_dom_node_t *parent = node_value(ctx, this_val);
    lxb_dom_node_t *child = node_value(ctx, argv[0]);
    if (!parent || !child) return JS_ThrowTypeError(ctx, "not a DOM node");
    lxb_dom_node_remove_child(parent, child);
    notify_mutation(engine_from_context(ctx), parent);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue node_get_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;
    NodeRef *ref = node_ref(ctx, this_val);
    if (!ref || !is_element(ref->node)) return JS_NULL;
    std::string name = js_string(ctx, argv[0]);
    std::string value = attr_value(lxb_dom_interface_element(ref->node), name);
    return lxb_dom_element_has_attribute(lxb_dom_interface_element(ref->node), reinterpret_cast<const lxb_char_t *>(name.data()), name.size())
        ? js_str(ctx, value) : JS_NULL;
}

static JSValue node_set_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    NodeRef *ref = node_ref(ctx, this_val);
    if (!ref || !is_element(ref->node)) return JS_UNDEFINED;
    set_attribute(lxb_dom_interface_element(ref->node), js_string(ctx, argv[0]), js_string(ctx, argv[1]));
    notify_mutation(ref->engine, ref->node);
    return JS_UNDEFINED;
}

static JSValue node_remove_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    NodeRef *ref = node_ref(ctx, this_val);
    if (ref && is_element(ref->node)) {
        std::string name = js_string(ctx, argv[0]);
        lxb_dom_element_remove_attribute(lxb_dom_interface_element(ref->node), reinterpret_cast<const lxb_char_t *>(name.data()), name.size());
        notify_mutation(ref->engine, ref->node);
    }
    return JS_UNDEFINED;
}

static JSValue document_create_element(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "createElement requires a name");
    NodeRef *ref = node_ref(ctx, this_val);
    if (!ref) return JS_EXCEPTION;
    std::string tag = lower(js_string(ctx, argv[0]));
    lxb_dom_element_t *el = lxb_dom_document_create_element(&ref->engine->document->dom_document,
        reinterpret_cast<const lxb_char_t *>(tag.data()), tag.size(), nullptr);
    return make_node(ref->engine, lxb_dom_interface_node(el));
}

static JSValue document_create_fragment(JSContext *ctx, JSValueConst this_val, int, JSValueConst *)
{
    NodeRef *ref = node_ref(ctx, this_val);
    if (!ref) return JS_EXCEPTION;
    return make_node(ref->engine, lxb_dom_interface_node(lxb_dom_document_create_document_fragment(&ref->engine->document->dom_document)));
}

static JSValue document_get_element_by_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;
    std::string id = js_string(ctx, argv[0]);
    std::vector<lxb_dom_node_t *> found;
    find_selector(node_value(ctx, this_val), "#" + id, found);
    return found.empty() ? JS_NULL : make_node(engine_from_context(ctx), found.front());
}

static JSValue get_node_href(JSContext *ctx, JSValueConst this_val, const char *attribute)
{
    NodeRef *ref = node_ref(ctx, this_val);
    if (!ref || !is_element(ref->node)) return JS_UNDEFINED;
    return js_str(ctx, resolve_url(ref->engine->base_url, attr_value(lxb_dom_interface_element(ref->node), attribute)));
}

static JSValue node_get_rect(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
    engine_from_context(ctx)->telemetry("geometry.getBoundingClientRect");
    JSValue object = JS_NewObject(ctx);
    const char *names[] = {"x", "y", "top", "left", "width", "height", "right", "bottom"};
    double vals[] = {0, 0, 0, 0, 800, 20, 800, 20};
    for (int i = 0; i < 8; ++i) JS_SetPropertyStr(ctx, object, names[i], JS_NewFloat64(ctx, vals[i]));
    return object;
}

static JSValue node_method(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    switch (magic) {
    case 0: return node_query_selector(ctx, this_val, argc, argv);
    case 1: return node_query_selector_all(ctx, this_val, argc, argv);
    case 2: return node_append_child(ctx, this_val, argc, argv);
    case 3: return node_remove_child(ctx, this_val, argc, argv);
    case 4: return document_create_element(ctx, this_val, argc, argv);
    case 5: return document_create_fragment(ctx, this_val, argc, argv);
    case 6: return document_get_element_by_id(ctx, this_val, argc, argv);
    case 7: return node_get_rect(ctx, this_val, argc, argv);
    }
    return JS_UNDEFINED;
}

static JSValue node_getter(JSContext *ctx, JSValueConst this_val, int magic)
{
    switch (magic) {
    case 0: return node_get_text(ctx, this_val);
    case 1: return node_get_inner_html(ctx, this_val);
    case 2: return node_get_first_element_child(ctx, this_val);
    case 3: return node_get_parent(ctx, this_val);
    case 4: return node_get_children(ctx, this_val);
    case 5: return node_get_name(ctx, this_val, false);
    case 6: return node_get_name(ctx, this_val, true);
    case 7: return node_get_attr(ctx, this_val, "id");
    case 8: return node_get_attr(ctx, this_val, "class");
    case 9: return node_get_class_list(ctx, this_val);
    case 10: return node_get_dataset(ctx, this_val);
    case 11: return get_node_href(ctx, this_val, "href");
    case 12: return get_node_href(ctx, this_val, "src");
    case 13: return get_node_href(ctx, this_val, "action");
    case 14: engine_from_context(ctx)->telemetry("geometry.offsetHeight"); return JS_NewInt32(ctx, 20);
    case 15: engine_from_context(ctx)->telemetry("geometry.offsetWidth"); return JS_NewInt32(ctx, 800);
    case 16: return JS_NewInt32(ctx, 0);
    case 17: { NodeRef *ref = node_ref(ctx, this_val); return ref && ref->node && ref->node->type == LXB_DOM_NODE_TYPE_DOCUMENT ? js_str(ctx, ref->engine->ready_state) : JS_UNDEFINED; }
    case 18: { NodeRef *ref = node_ref(ctx, this_val); return ref ? js_str(ctx, ref->engine->cookie) : JS_UNDEFINED; }
    }
    return JS_UNDEFINED;
}

static JSValue node_getter_magic(JSContext *ctx, JSValueConst this_val, int magic)
{
    return node_getter(ctx, this_val, magic);
}

static JSValue node_setter_magic(JSContext *ctx, JSValueConst this_val, JSValueConst value, int magic)
{
    int result = 0;
    switch (magic) {
    case 0: result = node_set_text(ctx, this_val, value); break;
    case 1: result = node_set_inner_html(ctx, this_val, value); break;
    case 2: result = set_node_property(ctx, this_val, "id", value); break;
    case 3: result = set_node_property(ctx, this_val, "class", value); break;
    case 4: { NodeRef *ref = node_ref(ctx, this_val); if (ref) ref->engine->cookie = js_string(ctx, value); break; }
    }
    return result < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue make_node(Engine *e, lxb_dom_node_t *node)
{
    if (!node) return JS_NULL;
    auto found = e->wrappers.find(node);
    if (found != e->wrappers.end()) return JS_DupValue(e->ctx, found->second);
    JSValue object = JS_NewObjectClass(e->ctx, node_class);
    NodeRef *ref = new NodeRef{e, node};
    JS_SetOpaque(object, ref);
    e->refs[node] = ref;
    e->wrappers.emplace(node, JS_DupValue(e->ctx, object));
    return object;
}

static JSValue get_node_property(JSContext *ctx, JSValueConst this_val, const char *name)
{
    if (std::strcmp(name, "textContent") == 0) return node_get_text(ctx, this_val);
    if (std::strcmp(name, "innerHTML") == 0) return node_get_inner_html(ctx, this_val);
    if (std::strcmp(name, "firstElementChild") == 0) return node_get_first_element_child(ctx, this_val);
    if (std::strcmp(name, "parentNode") == 0) return node_get_parent(ctx, this_val);
    if (std::strcmp(name, "children") == 0) return node_get_children(ctx, this_val);
    if (std::strcmp(name, "nodeName") == 0) return node_get_name(ctx, this_val, false);
    if (std::strcmp(name, "tagName") == 0) return node_get_name(ctx, this_val, true);
    if (std::strcmp(name, "id") == 0) return node_get_attr(ctx, this_val, "id");
    if (std::strcmp(name, "className") == 0) return node_get_attr(ctx, this_val, "class");
    if (std::strcmp(name, "classList") == 0) return node_get_class_list(ctx, this_val);
    if (std::strcmp(name, "dataset") == 0) return node_get_dataset(ctx, this_val);
    if (std::strcmp(name, "href") == 0) return get_node_href(ctx, this_val, "href");
    if (std::strcmp(name, "src") == 0) return get_node_href(ctx, this_val, "src");
    if (std::strcmp(name, "action") == 0) return get_node_href(ctx, this_val, "action");
    if (std::strcmp(name, "offsetHeight") == 0) return JS_NewInt32(ctx, 20);
    if (std::strcmp(name, "offsetWidth") == 0) return JS_NewInt32(ctx, 800);
    if (std::strcmp(name, "readyState") == 0) return js_str(ctx, "loading");
    return JS_UNDEFINED;
}

static int set_node_property(JSContext *ctx, JSValueConst this_val, const char *name, JSValueConst value)
{
    NodeRef *ref = node_ref(ctx, this_val);
    if (!ref || !is_element(ref->node)) return 0;
    if (std::strcmp(name, "textContent") == 0) return node_set_text(ctx, this_val, value);
    if (std::strcmp(name, "innerHTML") == 0) return node_set_inner_html(ctx, this_val, value);
    if (std::strcmp(name, "id") == 0 || std::strcmp(name, "class") == 0) {
        set_attribute(lxb_dom_interface_element(ref->node), name, js_string(ctx, value));
        notify_mutation(ref->engine, ref->node);
    }
    return 0;
}

static JSValue node_getter_c(JSContext *ctx, JSValueConst this_val, int, JSValueConst *) { return get_node_property(ctx, this_val, ""); }

static JSValue node_add_event_listener(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
    Engine *e = engine_from_context(ctx);
    e->listeners.push_back({node_value(ctx, this_val) && node_value(ctx, this_val)->type == LXB_DOM_NODE_TYPE_DOCUMENT ? false : true,
                            js_string(ctx, argv[0]), JS_DupValue(ctx, argv[1])});
    return JS_UNDEFINED;
}

static JSValue node_remove_event_listener(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    Engine *e = engine_from_context(ctx);
    bool window = !(node_value(ctx, this_val) && node_value(ctx, this_val)->type == LXB_DOM_NODE_TYPE_DOCUMENT);
    std::string type = js_string(ctx, argv[0]);
    for (auto it = e->listeners.begin(); it != e->listeners.end(); ++it) {
        if (it->window == window && it->type == type && JS_IsStrictEqual(ctx, it->callback, argv[1])) {
            JS_FreeValue(ctx, it->callback); e->listeners.erase(it); break;
        }
    }
    return JS_UNDEFINED;
}

static std::map<std::string, std::string> &storage_map(StorageRef *ref)
{
    return ref->session ? ref->engine->session_storage : ref->engine->local_storage;
}

static JSValue storage_get_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    StorageRef *ref = storage_ref(ctx, this_val);
    if (!ref || argc < 1) return JS_NULL;
    std::string key = js_string(ctx, argv[0]);
    auto &items = storage_map(ref);
    auto it = items.find(key);
    return it == items.end() ? JS_NULL : js_str(ctx, it->second);
}

static JSValue storage_set_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    StorageRef *ref = storage_ref(ctx, this_val);
    if (!ref || argc < 2) return JS_UNDEFINED;
    storage_map(ref)[js_string(ctx, argv[0])] = js_string(ctx, argv[1]);
    return JS_UNDEFINED;
}

static JSValue storage_remove_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    StorageRef *ref = storage_ref(ctx, this_val);
    if (ref && argc >= 1) storage_map(ref).erase(js_string(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue storage_clear(JSContext *ctx, JSValueConst this_val, int, JSValueConst *)
{
    StorageRef *ref = storage_ref(ctx, this_val);
    if (ref) storage_map(ref).clear();
    return JS_UNDEFINED;
}

static JSValue storage_key(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    StorageRef *ref = storage_ref(ctx, this_val);
    if (!ref || argc < 1) return JS_NULL;
    int32_t index = 0;
    JS_ToInt32(ctx, &index, argv[0]);
    auto &items = storage_map(ref);
    if (index < 0 || static_cast<size_t>(index) >= items.size()) return JS_NULL;
    auto it = items.begin();
    std::advance(it, index);
    return js_str(ctx, it->first);
}

static JSValue storage_length(JSContext *ctx, JSValueConst this_val)
{
    StorageRef *ref = storage_ref(ctx, this_val);
    return JS_NewInt32(ctx, ref ? static_cast<int32_t>(storage_map(ref).size()) : 0);
}

static JSValue make_storage(Engine *e, bool session)
{
    JSValue object = JS_NewObjectClass(e->ctx, storage_class);
    JS_SetOpaque(object, new StorageRef{e, session});
    return object;
}

static JSValue observer_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "observer callback required");
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue object = JS_NewObjectProtoClass(ctx, proto, observer_class);
    JS_FreeValue(ctx, proto);
    ObserverRef *ref = new ObserverRef{engine_from_context(ctx), JS_DupValue(ctx, argv[0]), false, false};
    ref->intersection = false;
    JS_SetOpaque(object, ref);
    ref->engine->observers.push_back(ref);
    return object;
}

static JSValue make_observer_object(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, bool intersection)
{
    JSValue object = observer_constructor(ctx, new_target, argc, argv);
    if (JS_IsException(object)) return object;
    ObserverRef *ref = observer_ref(ctx, object);
    if (ref) ref->intersection = intersection;
    return object;
}

static JSValue mutation_observer_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    return make_observer_object(ctx, new_target, argc, argv, false);
}

static JSValue intersection_observer_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    return make_observer_object(ctx, new_target, argc, argv, true);
}

static JSValue observer_observe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ObserverRef *ref = observer_ref(ctx, this_val);
    if (!ref || argc < 1) return JS_UNDEFINED;
    ref->observed = true;
    if (ref->intersection) {
        ref->engine->telemetry("geometry.IntersectionObserver");
        JSValue entry = plain_object(ref->engine);
        JS_SetPropertyStr(ctx, entry, "target", JS_DupValue(ctx, argv[0]));
        JS_SetPropertyStr(ctx, entry, "isIntersecting", JS_NewBool(ctx, true));
        JS_SetPropertyStr(ctx, entry, "intersectionRatio", JS_NewFloat64(ctx, 1));
        JSValue entries = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, entries, 0, entry);
        JSValue result = JS_Call(ctx, ref->callback, JS_UNDEFINED, 1, &entries);
        if (JS_IsException(result)) ref->engine->note_error("IntersectionObserver");
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, entries);
    }
    return JS_UNDEFINED;
}

static JSValue observer_unobserve(JSContext *ctx, JSValueConst this_val, int, JSValueConst *)
{
    ObserverRef *ref = observer_ref(ctx, this_val);
    if (ref) ref->observed = false;
    return JS_UNDEFINED;
}

static JSValue make_observer_constructor(Engine *e, bool intersection)
{
    JSValue ctor = JS_NewCFunction2(e->ctx, intersection ? intersection_observer_constructor : mutation_observer_constructor,
                                   intersection ? "IntersectionObserver" : "MutationObserver", 1, JS_CFUNC_constructor, 0);
    JSValue proto = JS_GetClassProto(e->ctx, observer_class);
    JS_SetConstructor(e->ctx, ctor, proto);
    JS_SetPropertyStr(e->ctx, e->global, intersection ? "IntersectionObserver" : "MutationObserver", ctor);
    JS_FreeValue(e->ctx, proto);
    return JS_UNDEFINED;
}

static JSValue make_timer_callback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    Engine *e = engine_from_context(ctx);
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    int32_t delay = 0;
    if (argc > 1) JS_ToInt32(ctx, &delay, argv[1]);
    if (delay < 0) delay = 0;
    Timer timer;
    timer.id = e->next_timer++;
    timer.due = now_ms() + static_cast<uint64_t>(delay);
    timer.interval = magic == 1 ? std::max(1, delay) : 0;
    timer.callback = JS_DupValue(ctx, argv[0]);
    timer.arg = argc > 2 ? JS_DupValue(ctx, argv[2]) : JS_UNDEFINED;
    e->timers.push_back(timer);
    return JS_NewInt64(ctx, static_cast<int64_t>(timer.id));
}

static JSValue clear_timer_callback(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int)
{
    Engine *e = engine_from_context(ctx);
    if (argc < 1) return JS_UNDEFINED;
    int64_t id = 0;
    JS_ToInt64(ctx, &id, argv[0]);
    for (Timer &timer : e->timers) if (timer.id == static_cast<uint64_t>(id)) timer.cancelled = true;
    return JS_UNDEFINED;
}

static JSValue request_animation_frame(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    Engine *e = engine_from_context(ctx);
    Timer timer;
    timer.id = e->next_timer++;
    timer.due = now_ms() + 16;
    timer.callback = JS_DupValue(ctx, argv[0]);
    timer.arg = JS_NewFloat64(ctx, static_cast<double>(now_ms() - e->started));
    e->timers.push_back(timer);
    return JS_NewInt64(ctx, static_cast<int64_t>(timer.id));
}

static JSValue console_callback(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int magic)
{
    std::cerr << (magic ? "console.error" : "console.log");
    for (int i = 0; i < argc; ++i) std::cerr << (i ? " " : " ") << js_string(ctx, argv[i]);
    std::cerr << "\n";
    return JS_UNDEFINED;
}

static JSValue performance_now(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
    Engine *e = engine_from_context(ctx);
    return JS_NewFloat64(ctx, static_cast<double>(now_ms() - e->started));
}

static JSValue crypto_random(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "typed array required");
    size_t length = 0;
    uint8_t *data = JS_GetUint8Array(ctx, &length, argv[0]);
    if (!data) return JS_ThrowTypeError(ctx, "typed array required");
    std::random_device random;
    for (size_t i = 0; i < length; ++i) data[i] = static_cast<uint8_t>(random());
    return JS_DupValue(ctx, argv[0]);
}

static JSValue match_media(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "matches", JS_NewBool(ctx, false));
    JS_SetPropertyStr(ctx, object, "media", argc ? JS_DupValue(ctx, argv[0]) : js_str(ctx, ""));
    return object;
}

static JSValue history_push_state(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    Engine *e = engine_from_context(ctx);
    if (argc < 3) return JS_UNDEFINED;
    std::string url = resolve_url(e->source_url, js_string(ctx, argv[2]));
    e->source_url = url;
    JSValue location = JS_GetPropertyStr(ctx, e->global, "location");
    size_t q = url.find('?');
    size_t h = url.find('#');
    std::string query = q == std::string::npos ? "" : url.substr(q, h == std::string::npos ? std::string::npos : h - q);
    JS_SetPropertyStr(ctx, location, "search", js_str(ctx, query));
    JS_SetPropertyStr(ctx, location, "href", js_str(ctx, url));
    JS_FreeValue(ctx, location);
    return JS_UNDEFINED;
}

static JSValue make_promise(Engine *e, JSValue *resolve, JSValue *reject)
{
    JSValue funcs[2] = {JS_UNDEFINED, JS_UNDEFINED};
    JSValue promise = JS_NewPromiseCapability(e->ctx, funcs);
    *resolve = funcs[0];
    *reject = funcs[1];
    return promise;
}

static void run_jobs(Engine *e)
{
    JSContext *job_ctx = nullptr;
    while (JS_IsJobPending(e->rt)) {
        int result = JS_ExecutePendingJob(e->rt, &job_ctx);
        if (result < 0) e->note_error("microtask");
        if (e->expired()) break;
    }
}

static void notify_mutation(Engine *e, lxb_dom_node_t *)
{
    for (ObserverRef *observer : e->observers) {
        if (!observer || !observer->observed || observer->intersection) continue;
        JSValue records = JS_NewArray(e->ctx);
        JSValue record = plain_object(e);
        JS_SetPropertyUint32(e->ctx, records, 0, record);
        JSValue result = JS_Call(e->ctx, observer->callback, JS_UNDEFINED, 1, &records);
        if (JS_IsException(result)) e->note_error("MutationObserver");
        JS_FreeValue(e->ctx, result);
        JS_FreeValue(e->ctx, records);
    }
}

static void process_timers(Engine *e)
{
    uint64_t current = now_ms();
    for (size_t i = 0; i < e->timers.size(); ++i) {
        Timer &timer = e->timers[i];
        if (timer.cancelled || timer.due > current) continue;
        JSValue callback = JS_DupValue(e->ctx, timer.callback);
        JSValue arg = JS_DupValue(e->ctx, timer.arg);
        JSValue result = JS_Call(e->ctx, callback, JS_UNDEFINED, 1, &arg);
        if (JS_IsException(result)) e->note_error("timer");
        JS_FreeValue(e->ctx, result);
        JS_FreeValue(e->ctx, callback);
        JS_FreeValue(e->ctx, arg);
        if (timer.interval > 0 && !timer.cancelled) timer.due = current + static_cast<uint64_t>(timer.interval);
        else timer.cancelled = true;
        JSValue fired_callback = timer.callback;
        for (size_t j = i + 1; j < e->timers.size(); ++j) {
            if (!e->timers[j].cancelled && JS_IsStrictEqual(e->ctx, fired_callback, e->timers[j].callback)) e->timers[j].recurring = true;
        }
    }
    for (auto it = e->timers.begin(); it != e->timers.end();) {
        if (it->cancelled) {
            JS_FreeValue(e->ctx, it->callback);
            JS_FreeValue(e->ctx, it->arg);
            it = e->timers.erase(it);
        } else ++it;
    }
}

static JSValue get_computed_style(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    Engine *e = engine_from_context(ctx);
    JSValue style = JS_NewObject(ctx);
    std::string display = "block";
    std::string visibility = "visible";
    if (argc >= 1) {
        lxb_dom_node_t *node = node_value(ctx, argv[0]);
        if (is_element(node)) {
            lxb_dom_element_t *el = lxb_dom_interface_element(node);
            if (lxb_dom_element_has_attribute(el, reinterpret_cast<const lxb_char_t *>("hidden"), 6)) display = "none";
            std::string inline_style = lower(attr_value(el, "style"));
            if (inline_style.find("display:none") != std::string::npos || inline_style.find("display: none") != std::string::npos) display = "none";
            if (inline_style.find("visibility:hidden") != std::string::npos || inline_style.find("visibility: hidden") != std::string::npos) visibility = "hidden";
            std::vector<lxb_dom_node_t *> styles;
            find_selector(lxb_dom_interface_node(e->document), "style", styles);
            for (lxb_dom_node_t *style_node : styles) {
                std::string css = lower(node_text(style_node));
                std::string id = attr_value(el, "id");
                size_t id_pos = id.empty() ? std::string::npos : css.find("#" + lower(id));
                if (id_pos != std::string::npos) {
                    size_t open = css.find('{', id_pos), close = css.find('}', open);
                    std::string rules = css.substr(open, close == std::string::npos ? std::string::npos : close - open);
                    if (rules.find("display:none") != std::string::npos || rules.find("display: none") != std::string::npos) display = "none";
                }
            }
        }
    }
    JS_SetPropertyStr(ctx, style, "display", js_str(ctx, display));
    JS_SetPropertyStr(ctx, style, "visibility", js_str(ctx, visibility));
    return style;
}

static void dispatch_event(Engine *e, const std::string &type, bool window)
{
    JSValue target = window ? JS_DupValue(e->ctx, e->global) : JS_DupValue(e->ctx, e->document_value);
    std::vector<JSValue> callbacks;
    for (const Listener &listener : e->listeners) if (listener.window == window && listener.type == type) callbacks.push_back(JS_DupValue(e->ctx, listener.callback));
    for (JSValue callback : callbacks) {
        JSValue event = plain_object(e);
        JS_SetPropertyStr(e->ctx, event, "type", js_str(e->ctx, type));
        JS_SetPropertyStr(e->ctx, event, "target", JS_DupValue(e->ctx, target));
        JSValue result = JS_Call(e->ctx, callback, target, 1, &event);
        if (JS_IsException(result)) e->note_error("event listener");
        JS_FreeValue(e->ctx, result);
        JS_FreeValue(e->ctx, event);
        JS_FreeValue(e->ctx, callback);
    }
    JS_FreeValue(e->ctx, target);
}

static void set_ready_state(Engine *e, const std::string &state)
{
    e->ready_state = state;
    JS_SetPropertyStr(e->ctx, e->document_value, "readyState", js_str(e->ctx, state));
    dispatch_event(e, "readystatechange", false);
}

static void maybe_dispatch_load(Engine *e)
{
    if (e->load_dispatched || e->async_pending != 0) return;
    e->load_dispatched = true;
    set_ready_state(e, "complete");
    dispatch_event(e, "load", true);
}

static std::string module_resolve(Engine *e, const std::string &base, const std::string &name)
{
    return resolve_url(base.empty() ? e->base_url : base, name);
}

static char *module_normalize(JSContext *ctx, const char *base_name, const char *module_name, void *opaque)
{
    Engine *e = static_cast<Engine *>(opaque);
    std::string resolved = module_resolve(e, base_name ? base_name : e->base_url, module_name);
    char *out = static_cast<char *>(js_malloc(ctx, resolved.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, resolved.c_str(), resolved.size() + 1);
    return out;
}

static JSModuleDef *module_loader(JSContext *ctx, const char *module_name, void *opaque)
{
    Engine *e = static_cast<Engine *>(opaque);
    std::vector<unsigned char> bytes;
    long status = 0;
    if (!fetch_sync(e, module_name, bytes, &status)) {
        JS_ThrowReferenceError(ctx, "unable to load module %s", module_name);
        return nullptr;
    }
    std::string source(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    JSValue module = JS_Eval(ctx, source.data(), source.size(), module_name,
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(module)) return nullptr;
    JSModuleDef *definition = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(module));
    JS_FreeValue(ctx, module);
    return definition;
}

static bool eval_script(Engine *e, const std::string &source, const std::string &filename, int flags)
{
    if (++e->scripts_run > e->max_scripts) {
        e->telemetry("script-limit", filename);
        return false;
    }
    JSValue value = JS_Eval(e->ctx, source.data(), source.size(), filename.c_str(), flags);
    bool ok = !JS_IsException(value);
    if (!ok) e->note_error(filename.c_str());
    JS_FreeValue(e->ctx, value);
    run_jobs(e);
    return ok;
}

static size_t curl_write(void *ptr, size_t size, size_t count, void *opaque)
{
    auto *body = static_cast<std::vector<unsigned char> *>(opaque);
    size_t total = size * count;
    const auto *bytes = static_cast<const unsigned char *>(ptr);
    body->insert(body->end(), bytes, bytes + total);
    return total;
}

static bool fetch_sync(Engine *e, const std::string &url, std::vector<unsigned char> &body, long *status)
{
    if (!is_http_url(url)) {
        std::string data = path_read(file_from_url(url));
        body.assign(data.begin(), data.end());
        if (status) *status = data.empty() ? 404 : 200;
        return !data.empty();
    }
    CURL *easy = curl_easy_init();
    if (!easy) return false;
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "distill-phase0/0.1");
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(std::max<uint64_t>(1000, e->cap_ms)));
    CURLcode result = curl_easy_perform(easy);
    long code = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(easy);
    if (status) *status = code;
    return result == CURLE_OK;
}

static JSValue response_text(JSContext *ctx, JSValueConst this_val, int, JSValueConst *)
{
    JSValue body_value = JS_GetPropertyStr(ctx, this_val, "_body");
    std::string body = js_string(ctx, body_value);
    JS_FreeValue(ctx, body_value);
    JSValue resolve = JS_UNDEFINED, reject = JS_UNDEFINED;
    JSValue promise = make_promise(engine_from_context(ctx), &resolve, &reject);
    JS_FreeValue(ctx, reject);
    JSValue body_arg = js_str(ctx, body);
    JSValue result = JS_Call(ctx, resolve, JS_UNDEFINED, 1, &body_arg);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, body_arg);
    JS_FreeValue(ctx, resolve);
    return promise;
}

static JSValue response_json(JSContext *ctx, JSValueConst this_val, int, JSValueConst *)
{
    Engine *e = engine_from_context(ctx);
    JSValue body_value = JS_GetPropertyStr(ctx, this_val, "_body");
    std::string body = js_string(ctx, body_value);
    JS_FreeValue(ctx, body_value);
    JSValue parsed = JS_ParseJSON(ctx, body.data(), body.size(), "response.json");
    if (JS_IsException(parsed)) return parsed;
    JSValue resolve = JS_UNDEFINED, reject = JS_UNDEFINED;
    JSValue promise = make_promise(e, &resolve, &reject);
    JS_FreeValue(ctx, reject);
    JSValue result = JS_Call(ctx, resolve, JS_UNDEFINED, 1, &parsed);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, parsed);
    JS_FreeValue(ctx, resolve);
    return promise;
}

static JSValue response_object(Engine *e, Request *request)
{
    JSContext *ctx = e->ctx;
    JSValue response = JS_NewObject(ctx);
    std::string body(reinterpret_cast<const char *>(request->body.data()), request->body.size());
    JS_SetPropertyStr(ctx, response, "ok", JS_NewBool(ctx, request->status >= 200 && request->status < 300));
    JS_SetPropertyStr(ctx, response, "status", JS_NewInt32(ctx, static_cast<int32_t>(request->status)));
    JS_SetPropertyStr(ctx, response, "url", js_str(ctx, request->url));
    JS_SetPropertyStr(ctx, response, "_body", js_str(ctx, body));
    JS_SetPropertyStr(ctx, response, "text", JS_NewCFunction(ctx, response_text, "text", 0));
    JS_SetPropertyStr(ctx, response, "json", JS_NewCFunction(ctx, response_json, "json", 0));
    return response;
}

static Request *start_request(Engine *e, Request::Kind kind, const std::string &url)
{
    auto request = std::make_unique<Request>();
    request->engine = e;
    request->kind = kind;
    request->url = url;
    request->easy = curl_easy_init();
    if (!request->easy) return nullptr;
    curl_easy_setopt(request->easy, CURLOPT_URL, request->url.c_str());
    curl_easy_setopt(request->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(request->easy, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(request->easy, CURLOPT_WRITEDATA, &request->body);
    curl_easy_setopt(request->easy, CURLOPT_PRIVATE, request.get());
    curl_easy_setopt(request->easy, CURLOPT_USERAGENT, "distill-phase0/0.1");
    curl_easy_setopt(request->easy, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    Request *out = request.get();
    e->requests.push_back(std::move(request));
    if (is_http_url(url)) curl_multi_add_handle(e->multi, out->easy);
    else {
        std::string data = path_read(file_from_url(url));
        out->body.assign(data.begin(), data.end());
        out->status = data.empty() ? 404 : 200;
        out->easy = nullptr;
        out->completed = false;
    }
    return out;
}

static JSValue fetch_callback(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    Engine *e = engine_from_context(ctx);
    std::string url = argc ? js_string(ctx, argv[0]) : e->source_url;
    if (argc && JS_IsObject(argv[0])) {
        JSValue requested = JS_GetPropertyStr(ctx, argv[0], "url");
        if (!JS_IsUndefined(requested)) url = js_string(ctx, requested);
        JS_FreeValue(ctx, requested);
    }
    url = resolve_url(e->base_url, url);
    JSValue resolving[2] = {JS_UNDEFINED, JS_UNDEFINED};
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    Request *request = start_request(e, Request::Fetch, url);
    if (!request) {
        JSValue error = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, error, "message", js_str(ctx, "unable to start request"));
        JSValue result = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &error);
        JS_FreeValue(ctx, result); JS_FreeValue(ctx, error);
        JS_FreeValue(ctx, resolving[0]); JS_FreeValue(ctx, resolving[1]);
        return promise;
    }
    request->resolve = resolving[0];
    request->reject = resolving[1];
    return promise;
}

static JSValue xhr_constructor(JSContext *ctx, JSValueConst new_target, int, JSValueConst *)
{
    Engine *e = engine_from_context(ctx);
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue object = JS_NewObjectProtoClass(ctx, proto, xhr_class);
    JS_FreeValue(ctx, proto);
    JS_SetOpaque(object, new XhrRef{e});
    JS_SetPropertyStr(ctx, object, "readyState", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, object, "status", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, object, "responseText", js_str(ctx, ""));
    JS_SetPropertyStr(ctx, object, "onreadystatechange", JS_UNDEFINED);
    return object;
}

static JSValue xhr_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    XhrRef *ref = xhr_ref(ctx, this_val);
    if (!ref || argc < 2) return JS_UNDEFINED;
    std::string method = js_string(ctx, argv[0]);
    std::string url = resolve_url(ref->engine->base_url, js_string(ctx, argv[1]));
    JS_SetPropertyStr(ctx, this_val, "_method", js_str(ctx, method));
    JS_SetPropertyStr(ctx, this_val, "_url", js_str(ctx, url));
    JS_SetPropertyStr(ctx, this_val, "_async", JS_NewBool(ctx, argc < 3 || JS_ToBool(ctx, argv[2])));
    JS_SetPropertyStr(ctx, this_val, "readyState", JS_NewInt32(ctx, 1));
    return JS_UNDEFINED;
}

static JSValue xhr_send(JSContext *ctx, JSValueConst this_val, int, JSValueConst *)
{
    XhrRef *ref = xhr_ref(ctx, this_val);
    if (!ref) return JS_UNDEFINED;
    JSValue url_value = JS_GetPropertyStr(ctx, this_val, "_url");
    std::string url = js_string(ctx, url_value);
    JS_FreeValue(ctx, url_value);
    Request *request = start_request(ref->engine, Request::Xhr, url);
    if (request) request->xhr = JS_DupValue(ctx, this_val);
    return JS_UNDEFINED;
}

static JSValue xhr_set_request_header(JSContext *, JSValueConst, int, JSValueConst *) { return JS_UNDEFINED; }

static void finish_request(Engine *e, Request *request)
{
    if (request->kind == Request::Fetch) {
        JSValue response = response_object(e, request);
        JSValue result = JS_Call(e->ctx, request->resolve, JS_UNDEFINED, 1, &response);
        if (JS_IsException(result)) e->note_error("fetch resolve");
        JS_FreeValue(e->ctx, result);
        JS_FreeValue(e->ctx, response);
        JS_FreeValue(e->ctx, request->resolve);
        JS_FreeValue(e->ctx, request->reject);
        request->resolve = JS_UNDEFINED;
        request->reject = JS_UNDEFINED;
    } else if (request->kind == Request::Xhr) {
        JS_SetPropertyStr(e->ctx, request->xhr, "readyState", JS_NewInt32(e->ctx, 4));
        JS_SetPropertyStr(e->ctx, request->xhr, "status", JS_NewInt32(e->ctx, static_cast<int32_t>(request->status)));
        JS_SetPropertyStr(e->ctx, request->xhr, "responseText", js_str(e->ctx, std::string(reinterpret_cast<const char *>(request->body.data()), request->body.size())));
        JSValue callback = JS_GetPropertyStr(e->ctx, request->xhr, "onreadystatechange");
        if (JS_IsFunction(e->ctx, callback)) {
            JSValue result = JS_Call(e->ctx, callback, request->xhr, 0, nullptr);
            if (JS_IsException(result)) e->note_error("xhr callback");
            JS_FreeValue(e->ctx, result);
        }
        JS_FreeValue(e->ctx, callback);
        JS_FreeValue(e->ctx, request->xhr);
        request->xhr = JS_UNDEFINED;
    } else {
        std::string body(reinterpret_cast<const char *>(request->body.data()), request->body.size());
        eval_script(e, body, request->url, JS_EVAL_TYPE_GLOBAL);
        if (e->async_pending > 0) --e->async_pending;
    }
    if (request->easy) curl_easy_cleanup(request->easy);
    request->easy = nullptr;
}

static void process_network(Engine *e)
{
    for (auto &local : e->requests) {
        if (!local->easy && !local->completed) {
            local->completed = true;
            finish_request(e, local.get());
        }
    }
    if (!e->multi) return;
    int running = 0;
    curl_multi_perform(e->multi, &running);
    int messages = 0;
    while (CURLMsg *message = curl_multi_info_read(e->multi, &messages)) {
        if (message->msg != CURLMSG_DONE) continue;
        Request *request = nullptr;
        curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &request);
        if (!request) continue;
        curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &request->status);
        curl_multi_remove_handle(e->multi, message->easy_handle);
        request->completed = true;
        finish_request(e, request);
    }
}

static JSValue global_add_event_listener(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc >= 2 && JS_IsFunction(ctx, argv[1])) {
        Engine *e = engine_from_context(ctx);
        e->listeners.push_back({true, js_string(ctx, argv[0]), JS_DupValue(ctx, argv[1])});
    }
    return JS_UNDEFINED;
}

static JSValue global_remove_event_listener(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    Engine *e = engine_from_context(ctx);
    std::string type = js_string(ctx, argv[0]);
    for (auto it = e->listeners.begin(); it != e->listeners.end(); ++it) {
        if (it->window && it->type == type && JS_IsStrictEqual(ctx, it->callback, argv[1])) {
            JS_FreeValue(ctx, it->callback); e->listeners.erase(it); break;
        }
    }
    return JS_UNDEFINED;
}

static JSValue scroll_noop(JSContext *ctx, JSValueConst, int, JSValueConst *) { engine_from_context(ctx)->telemetry("geometry.scroll"); return JS_UNDEFINED; }

static JSValue make_location(Engine *e)
{
    JSContext *ctx = e->ctx;
    JSValue location = JS_NewObject(ctx);
    std::string url = e->source_url;
    std::string origin = url_origin(url);
    std::string path = url_path(url);
    size_t q = url.find('?');
    size_t h = url.find('#');
    std::string search = q == std::string::npos ? "" : url.substr(q, h == std::string::npos ? std::string::npos : h - q);
    std::string hash = h == std::string::npos ? "" : url.substr(h);
    std::string protocol;
    size_t scheme = url.find(':');
    if (scheme != std::string::npos) protocol = url.substr(0, scheme + 1);
    std::string host = origin;
    if (!protocol.empty() && host.rfind(protocol, 0) == 0) host = host.substr(protocol.size());
    JS_SetPropertyStr(ctx, location, "href", js_str(ctx, url));
    JS_SetPropertyStr(ctx, location, "protocol", js_str(ctx, protocol));
    JS_SetPropertyStr(ctx, location, "host", js_str(ctx, host));
    JS_SetPropertyStr(ctx, location, "hostname", js_str(ctx, host));
    JS_SetPropertyStr(ctx, location, "pathname", js_str(ctx, path));
    JS_SetPropertyStr(ctx, location, "search", js_str(ctx, search));
    JS_SetPropertyStr(ctx, location, "hash", js_str(ctx, hash));
    return location;
}

static JSValue make_navigator(JSContext *ctx)
{
    JSValue navigator = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, navigator, "userAgent", js_str(ctx, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0 Safari/537.36"));
    JS_SetPropertyStr(ctx, navigator, "language", js_str(ctx, "en-US"));
    JS_SetPropertyStr(ctx, navigator, "platform", js_str(ctx, "Linux x86_64"));
    JS_SetPropertyStr(ctx, navigator, "onLine", JS_NewBool(ctx, true));
    return navigator;
}

static JSValue xhr_make_constructor(Engine *e)
{
    JSValue ctor = JS_NewCFunction2(e->ctx, xhr_constructor, "XMLHttpRequest", 0, JS_CFUNC_constructor, 0);
    JSValue proto = JS_NewObject(e->ctx);
    JS_SetPropertyFunctionList(e->ctx, proto, (const JSCFunctionListEntry[]){
        JS_CFUNC_DEF("open", 2, xhr_open),
        JS_CFUNC_DEF("send", 0, xhr_send),
        JS_CFUNC_DEF("setRequestHeader", 2, xhr_set_request_header),
    }, 3);
    JS_SetConstructor(e->ctx, ctor, proto);
    JS_SetPropertyStr(e->ctx, e->global, "XMLHttpRequest", ctor);
    JS_FreeValue(e->ctx, proto);
    return JS_UNDEFINED;
}

static void install_classes(Engine *e)
{
    JS_NewClassID(e->rt, &node_class);
    JS_NewClassID(e->rt, &dataset_class);
    JS_NewClassID(e->rt, &storage_class);
    JS_NewClassID(e->rt, &observer_class);
    JS_NewClassID(e->rt, &xhr_class);
    static JSClassExoticMethods dataset_exotic = {};
    dataset_exotic.get_property = dataset_get_property;
    dataset_exotic.set_property = dataset_set_property;
    node_exotic = {};
    node_exotic.get_property = node_unimplemented_get;
    node_exotic.set_property = node_unimplemented_set;
    JSClassDef node_def = {"DistillNode", node_finalizer, nullptr, nullptr, nullptr};
    JSClassDef dataset_def = {"DOMStringMap", nullptr, nullptr, nullptr, &dataset_exotic};
    JSClassDef storage_def = {"Storage", storage_finalizer, nullptr, nullptr, nullptr};
    JSClassDef observer_def = {"Observer", observer_finalizer, nullptr, nullptr, nullptr};
    JSClassDef xhr_def = {"XMLHttpRequest", xhr_finalizer, nullptr, nullptr, nullptr};
    JS_NewClass(e->rt, node_class, &node_def);
    JS_NewClass(e->rt, dataset_class, &dataset_def);
    JS_NewClass(e->rt, storage_class, &storage_def);
    JS_NewClass(e->rt, observer_class, &observer_def);
    JS_NewClass(e->rt, xhr_class, &xhr_def);

    JSValue proto = JS_NewObject(e->ctx);
    static const JSCFunctionListEntry node_proto[] = {
        JS_CGETSET_MAGIC_DEF("textContent", node_getter_magic, node_setter_magic, 0),
        JS_CGETSET_MAGIC_DEF("innerHTML", node_getter_magic, node_setter_magic, 1),
        JS_CGETSET_MAGIC_DEF("firstElementChild", node_getter_magic, nullptr, 2),
        JS_CGETSET_MAGIC_DEF("parentNode", node_getter_magic, nullptr, 3),
        JS_CGETSET_MAGIC_DEF("children", node_getter_magic, nullptr, 4),
        JS_CGETSET_MAGIC_DEF("nodeName", node_getter_magic, nullptr, 5),
        JS_CGETSET_MAGIC_DEF("tagName", node_getter_magic, nullptr, 6),
        JS_CGETSET_MAGIC_DEF("id", node_getter_magic, node_setter_magic, 2),
        JS_CGETSET_MAGIC_DEF("className", node_getter_magic, node_setter_magic, 3),
        JS_CGETSET_MAGIC_DEF("classList", node_getter_magic, nullptr, 9),
        JS_CGETSET_MAGIC_DEF("dataset", node_getter_magic, nullptr, 10),
        JS_CGETSET_MAGIC_DEF("href", node_getter_magic, nullptr, 11),
        JS_CGETSET_MAGIC_DEF("src", node_getter_magic, nullptr, 12),
        JS_CGETSET_MAGIC_DEF("action", node_getter_magic, nullptr, 13),
        JS_CGETSET_MAGIC_DEF("offsetHeight", node_getter_magic, nullptr, 14),
        JS_CGETSET_MAGIC_DEF("offsetWidth", node_getter_magic, nullptr, 15),
        JS_CGETSET_MAGIC_DEF("readyState", node_getter_magic, nullptr, 17),
        JS_CGETSET_MAGIC_DEF("cookie", node_getter_magic, node_setter_magic, 18),
        JS_CFUNC_MAGIC_DEF("querySelector", 1, node_method, 0),
        JS_CFUNC_MAGIC_DEF("querySelectorAll", 1, node_method, 1),
        JS_CFUNC_MAGIC_DEF("appendChild", 1, node_method, 2),
        JS_CFUNC_MAGIC_DEF("removeChild", 1, node_method, 3),
        JS_CFUNC_MAGIC_DEF("createElement", 1, node_method, 4),
        JS_CFUNC_MAGIC_DEF("createDocumentFragment", 0, node_method, 5),
        JS_CFUNC_MAGIC_DEF("getElementById", 1, node_method, 6),
        JS_CFUNC_DEF("getAttribute", 1, node_get_attribute),
        JS_CFUNC_DEF("setAttribute", 2, node_set_attribute),
        JS_CFUNC_DEF("removeAttribute", 1, node_remove_attribute),
        JS_CFUNC_DEF("addEventListener", 2, node_add_event_listener),
        JS_CFUNC_DEF("removeEventListener", 2, node_remove_event_listener),
        JS_CFUNC_MAGIC_DEF("getBoundingClientRect", 0, node_method, 7),
    };
    JS_SetPropertyFunctionList(e->ctx, proto, node_proto, sizeof(node_proto) / sizeof(node_proto[0]));
    JS_SetClassProto(e->ctx, node_class, proto);

    JSValue storage_proto = JS_NewObject(e->ctx);
    static const JSCFunctionListEntry storage_methods[] = {
        JS_CFUNC_DEF("getItem", 1, storage_get_item),
        JS_CFUNC_DEF("setItem", 2, storage_set_item),
        JS_CFUNC_DEF("removeItem", 1, storage_remove_item),
        JS_CFUNC_DEF("clear", 0, storage_clear),
        JS_CFUNC_DEF("key", 1, storage_key),
        JS_CGETSET_DEF("length", storage_length, nullptr),
    };
    JS_SetPropertyFunctionList(e->ctx, storage_proto, storage_methods, sizeof(storage_methods) / sizeof(storage_methods[0]));
    JS_SetClassProto(e->ctx, storage_class, storage_proto);

    JSValue observer_proto = JS_NewObject(e->ctx);
    static const JSCFunctionListEntry observer_methods[] = {
        JS_CFUNC_DEF("observe", 1, observer_observe),
        JS_CFUNC_DEF("unobserve", 1, observer_unobserve),
    };
    JS_SetPropertyFunctionList(e->ctx, observer_proto, observer_methods, 2);
    JS_SetClassProto(e->ctx, observer_class, observer_proto);

    JSValue xhr_proto = JS_NewObject(e->ctx);
    static const JSCFunctionListEntry xhr_methods[] = {
        JS_CFUNC_DEF("open", 2, xhr_open),
        JS_CFUNC_DEF("send", 0, xhr_send),
        JS_CFUNC_DEF("setRequestHeader", 2, xhr_set_request_header),
    };
    JS_SetPropertyFunctionList(e->ctx, xhr_proto, xhr_methods, 3);
    JS_SetClassProto(e->ctx, xhr_class, xhr_proto);
}

static JSValue plain_object(Engine *e)
{
    return JS_NewObject(e->ctx);
}

static void install_globals(Engine *e)
{
    JSContext *ctx = e->ctx;
    JSValue global = e->global;
    JSValue window = JS_DupValue(ctx, global);
    JS_SetPropertyStr(ctx, global, "window", window);
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "innerWidth", JS_NewInt32(ctx, 1280));
    JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewInt32(ctx, 1024));
    JS_SetPropertyStr(ctx, global, "document", JS_DupValue(ctx, e->document_value));

    JSValue body = make_node(e, lxb_dom_interface_node(e->document->body));
    JSValue head = make_node(e, lxb_dom_interface_node(e->document->head));
    JSValue document_element = make_node(e, lxb_dom_interface_node(e->document->dom_document.element));
    JS_SetPropertyStr(ctx, e->document_value, "body", body);
    JS_SetPropertyStr(ctx, e->document_value, "head", head);
    JS_SetPropertyStr(ctx, e->document_value, "documentElement", document_element);

    JS_SetPropertyStr(ctx, global, "navigator", make_navigator(ctx));
    JSValue screen = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, screen, "width", JS_NewInt32(ctx, 1280));
    JS_SetPropertyStr(ctx, screen, "height", JS_NewInt32(ctx, 1024));
    JS_SetPropertyStr(ctx, global, "screen", screen);
    JS_SetPropertyStr(ctx, global, "location", make_location(e));

    JSValue performance = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, performance, "now", JS_NewCFunction(ctx, performance_now, "now", 0));
    JS_SetPropertyStr(ctx, global, "performance", performance);
    JSValue crypto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, crypto, "getRandomValues", JS_NewCFunction(ctx, crypto_random, "getRandomValues", 1));
    JS_SetPropertyStr(ctx, global, "crypto", crypto);
    JSValue history = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, history, "pushState", JS_NewCFunction(ctx, history_push_state, "pushState", 3));
    JS_SetPropertyStr(ctx, history, "replaceState", JS_NewCFunction(ctx, history_push_state, "replaceState", 3));
    JS_SetPropertyStr(ctx, global, "history", history);

    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunctionMagic(ctx, console_callback, "log", 0, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunctionMagic(ctx, console_callback, "error", 0, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "console", console);

    JS_SetPropertyStr(ctx, global, "fetch", JS_NewCFunction(ctx, fetch_callback, "fetch", 1));
    JS_SetPropertyStr(ctx, global, "getComputedStyle", JS_NewCFunction(ctx, get_computed_style, "getComputedStyle", 1));
    JS_SetPropertyStr(ctx, global, "matchMedia", JS_NewCFunction(ctx, match_media, "matchMedia", 1));
    JS_SetPropertyStr(ctx, global, "scrollTo", JS_NewCFunction(ctx, scroll_noop, "scrollTo", 2));
    JS_SetPropertyStr(ctx, global, "scrollBy", JS_NewCFunction(ctx, scroll_noop, "scrollBy", 2));
    JS_SetPropertyStr(ctx, global, "addEventListener", JS_NewCFunction(ctx, global_add_event_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, global, "removeEventListener", JS_NewCFunction(ctx, global_remove_event_listener, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, global, "setTimeout", JS_NewCFunctionMagic(ctx, make_timer_callback, "setTimeout", 2, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "setInterval", JS_NewCFunctionMagic(ctx, make_timer_callback, "setInterval", 2, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "clearTimeout", JS_NewCFunctionMagic(ctx, clear_timer_callback, "clearTimeout", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "clearInterval", JS_NewCFunctionMagic(ctx, clear_timer_callback, "clearInterval", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "requestAnimationFrame", JS_NewCFunction(ctx, request_animation_frame, "requestAnimationFrame", 1));

    JSValue node_constants = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, node_constants, "ELEMENT_NODE", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, node_constants, "TEXT_NODE", JS_NewInt32(ctx, 3));
    JS_SetPropertyStr(ctx, node_constants, "DOCUMENT_NODE", JS_NewInt32(ctx, 9));
    JS_SetPropertyStr(ctx, node_constants, "DOCUMENT_FRAGMENT_NODE", JS_NewInt32(ctx, 11));
    JS_SetPropertyStr(ctx, global, "Node", node_constants);

    JS_SetPropertyStr(ctx, global, "localStorage", make_storage(e, false));
    JS_SetPropertyStr(ctx, global, "sessionStorage", make_storage(e, true));
    make_observer_constructor(e, false);
    make_observer_constructor(e, true);

    JSValue xhr_ctor = JS_NewCFunction2(ctx, xhr_constructor, "XMLHttpRequest", 0, JS_CFUNC_constructor, 0);
    JSValue xhr_proto = JS_GetClassProto(ctx, xhr_class);
    JS_SetConstructor(ctx, xhr_ctor, xhr_proto);
    JS_SetPropertyStr(ctx, global, "XMLHttpRequest", xhr_ctor);
    JS_FreeValue(ctx, xhr_proto);

    JS_SetPropertyStr(ctx, global, "__distill_unimplemented", JS_NewCFunction(ctx, unimplemented_callback, "__distill_unimplemented", 1));
    JS_SetPropertyStr(ctx, global, "__distill_node_proto", JS_GetClassProto(ctx, node_class));
    const char *telemetry_proxy =
        "Object.setPrototypeOf(__distill_node_proto, new Proxy(Object.prototype, {"
        "get(target, property, receiver) { __distill_unimplemented(String(property)); "
        "return Reflect.get(target, property, receiver); }}));";
    JSValue telemetry_result = JS_Eval(ctx, telemetry_proxy, std::strlen(telemetry_proxy), "distill-telemetry.js", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, telemetry_result);
}

struct ScriptSpec { lxb_dom_node_t *node; std::string src; bool async; bool defer; bool module; };

static std::vector<ScriptSpec> page_scripts(Engine *e)
{
    std::vector<lxb_dom_node_t *> nodes;
    find_selector(lxb_dom_interface_node(e->document), "script", nodes);
    std::vector<ScriptSpec> scripts;
    for (lxb_dom_node_t *node : nodes) {
        lxb_dom_element_t *el = lxb_dom_interface_element(node);
        std::string type = lower(attr_value(el, "type"));
        if (type.find("json") != std::string::npos) continue;
        scripts.push_back({node, attr_value(el, "src"), lxb_dom_element_has_attribute(el, reinterpret_cast<const lxb_char_t *>("async"), 5),
                           lxb_dom_element_has_attribute(el, reinterpret_cast<const lxb_char_t *>("defer"), 5), type == "module" || type == "text/javascript;module"});
    }
    return scripts;
}

static std::string script_text(lxb_dom_node_t *node)
{
    return node_text(node);
}

static void execute_page_scripts(Engine *e)
{
    std::vector<ScriptSpec> deferred;
    for (const ScriptSpec &script : page_scripts(e)) {
        lxb_dom_element_t *el = lxb_dom_interface_element(script.node);
        std::string code;
        std::string filename = e->source_url + "#inline";
        if (!script.src.empty()) {
            std::string url = resolve_url(e->base_url, script.src);
            if (script.async) {
                ++e->async_pending;
                Request *request = start_request(e, Request::AsyncScript, url);
                if (!request) --e->async_pending;
                continue;
            }
            if (script.defer) { deferred.push_back(script); continue; }
            std::vector<unsigned char> bytes;
            long status = 0;
            if (fetch_sync(e, url, bytes, &status)) code.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            filename = url;
        } else code = script_text(script.node);
        if (!code.empty()) eval_script(e, code, filename, script.module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
    }
    for (const ScriptSpec &script : deferred) {
        lxb_dom_element_t *el = lxb_dom_interface_element(script.node);
        std::string code;
        std::string filename = e->source_url + "#defer";
        if (!script.src.empty()) {
            std::string url = resolve_url(e->base_url, script.src);
            std::vector<unsigned char> bytes;
            long status = 0;
            if (fetch_sync(e, url, bytes, &status)) code.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            filename = url;
        } else code = script_text(script.node);
        if (!code.empty()) eval_script(e, code, filename, script.module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
    }
    set_ready_state(e, "interactive");
    dispatch_event(e, "DOMContentLoaded", false);
}

static int interrupt_handler(JSRuntime *rt, void *opaque)
{
    Engine *e = static_cast<Engine *>(opaque);
    if (e->expired()) {
        e->interrupted = true;
        e->cap_hit = true;
        e->telemetry("script-cap");
        return 1;
    }
    return 0;
}

static bool request_in_flight(Engine *e)
{
    for (const auto &request : e->requests) if (!request->completed) return true;
    return false;
}

static bool timer_near(Engine *e)
{
    uint64_t limit = now_ms() + 500;
    for (const Timer &timer : e->timers) if (!timer.cancelled && !timer.recurring && timer.due <= limit) return true;
    return false;
}

static void stop_at_cap(Engine *e)
{
    if (!e->expired()) return;
    e->cap_hit = true;
    e->telemetry("settle-cap");
    for (auto &request : e->requests) {
        if (!request->completed && request->easy) {
            curl_multi_remove_handle(e->multi, request->easy);
            request->completed = true;
            curl_easy_cleanup(request->easy);
            request->easy = nullptr;
            if (request->kind == Request::Fetch && !JS_IsUndefined(request->reject)) {
                JSValue error = JS_NewError(e->ctx);
                JS_SetPropertyStr(e->ctx, error, "message", js_str(e->ctx, "request cancelled at settle cap"));
                JSValue result = JS_Call(e->ctx, request->reject, JS_UNDEFINED, 1, &error);
                JS_FreeValue(e->ctx, result); JS_FreeValue(e->ctx, error);
                JS_FreeValue(e->ctx, request->resolve);
                JS_FreeValue(e->ctx, request->reject);
                request->resolve = JS_UNDEFINED;
                request->reject = JS_UNDEFINED;
            }
            if (request->kind == Request::Xhr && !JS_IsUndefined(request->xhr)) {
                JS_FreeValue(e->ctx, request->xhr);
                request->xhr = JS_UNDEFINED;
            }
            if (request->kind == Request::AsyncScript && e->async_pending > 0) --e->async_pending;
        }
    }
}

static void settle(Engine *e)
{
    while (true) {
        process_network(e);
        run_jobs(e);
        process_timers(e);
        run_jobs(e);
        maybe_dispatch_load(e);
        if (e->expired()) { stop_at_cap(e); break; }
        if (!request_in_flight(e) && !JS_IsJobPending(e->rt) && !timer_near(e)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

static std::string make_absolute_input(const std::string &input)
{
    if (is_http_url(input) || input.rfind("file:", 0) == 0) return input;
    return fs::absolute(input).lexically_normal().string();
}

static void free_engine_values(Engine *e)
{
    for (Listener &listener : e->listeners) JS_FreeValue(e->ctx, listener.callback);
    e->listeners.clear();
    for (Timer &timer : e->timers) { JS_FreeValue(e->ctx, timer.callback); JS_FreeValue(e->ctx, timer.arg); }
    e->timers.clear();
    for (auto &request : e->requests) {
        if (request->easy) {
            curl_multi_remove_handle(e->multi, request->easy);
            curl_easy_cleanup(request->easy);
            request->easy = nullptr;
        }
        if (!JS_IsUndefined(request->resolve)) JS_FreeValue(e->ctx, request->resolve);
        if (!JS_IsUndefined(request->reject)) JS_FreeValue(e->ctx, request->reject);
        if (!JS_IsUndefined(request->xhr)) JS_FreeValue(e->ctx, request->xhr);
    }
    e->requests.clear();
    for (auto &item : e->wrappers) JS_FreeValue(e->ctx, item.second);
    e->wrappers.clear();
    e->refs.clear();
}

static int run_page(const std::string &input, uint64_t cap_ms, uint64_t memory_mb, uint64_t max_scripts,
                    const std::string &telemetry_path, bool dump_dom)
{
    Engine e;
    e.cap_ms = cap_ms;
    e.memory_limit = memory_mb * 1024 * 1024;
    e.max_scripts = max_scripts;
    e.source_url = make_absolute_input(input);
    e.base_url = e.source_url;
    e.started = now_ms();
    if (!telemetry_path.empty()) e.telemetry_file.open(telemetry_path, std::ios::out | std::ios::trunc);

    std::vector<unsigned char> html_bytes;
    long status = 0;
    if (!fetch_sync(&e, e.source_url, html_bytes, &status)) {
        std::cerr << "unable to load " << e.source_url << " (status " << status << ")\n";
        return 2;
    }
    e.document = lxb_html_document_create();
    if (!e.document || lxb_html_document_parse(e.document, html_bytes.data(), html_bytes.size()) != LXB_STATUS_OK) {
        std::cerr << "unable to parse " << e.source_url << "\n";
        if (e.document) lxb_html_document_destroy(e.document);
        return 2;
    }
    std::vector<lxb_dom_node_t *> base_nodes;
    find_selector(lxb_dom_interface_node(e.document), "base", base_nodes);
    if (!base_nodes.empty()) e.base_url = resolve_url(e.source_url, attr_value(lxb_dom_interface_element(base_nodes.front()), "href"));

    e.rt = JS_NewRuntime();
    if (!e.rt) { lxb_html_document_destroy(e.document); return 2; }
    JS_SetRuntimeOpaque(e.rt, &e);
    JS_SetMemoryLimit(e.rt, e.memory_limit);
    JS_SetMaxStackSize(e.rt, 8 * 1024 * 1024);
    JS_SetInterruptHandler(e.rt, interrupt_handler, &e);
    JS_SetModuleLoaderFunc(e.rt, module_normalize, module_loader, &e);
    e.ctx = JS_NewContext(e.rt);
    if (!e.ctx) { JS_FreeRuntime(e.rt); lxb_html_document_destroy(e.document); return 2; }
    JS_SetContextOpaque(e.ctx, &e);
    e.global = JS_GetGlobalObject(e.ctx);
    install_classes(&e);
    e.document_value = make_node(&e, lxb_dom_interface_node(e.document));
    install_globals(&e);
    e.multi = curl_multi_init();
    execute_page_scripts(&e);
    settle(&e);

    if (dump_dom) {
        lexbor_str_t serialized = {0};
        lxb_html_serialize_tree_str(lxb_dom_interface_node(e.document), &serialized);
        if (serialized.data) std::cout.write(reinterpret_cast<const char *>(serialized.data), static_cast<std::streamsize>(serialized.length));
        std::cout << '\n';
    }
    std::cerr << "phase0 scripts=" << e.scripts_run << " cap_hit=" << (e.cap_hit ? "true" : "false") << "\n";
    free_engine_values(&e);
    JS_FreeValue(e.ctx, e.document_value);
    JS_FreeValue(e.ctx, e.global);
    JS_FreeContext(e.ctx);
    JS_FreeRuntime(e.rt);
    if (e.multi) curl_multi_cleanup(e.multi);
    lxb_html_document_destroy(e.document);
    return 0;
}

static void usage(const char *program)
{
    std::cerr << "usage: " << program << " --url URL [--dump-dom] [--script-cap-ms N] [--memory-limit-mb N] [--max-scripts N] [--telemetry FILE]\n";
}

int main(int argc, char **argv)
{
    std::string url;
    std::string telemetry_path;
    uint64_t cap_ms = 5000, memory_mb = 128, max_scripts = 1000;
    bool dump_dom = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next_value = [&](uint64_t &value) { if (i + 1 < argc) value = std::strtoull(argv[++i], nullptr, 10); };
        if (arg == "--url" && i + 1 < argc) url = argv[++i];
        else if (arg == "--dump-dom") dump_dom = true;
        else if (arg == "--script-cap-ms") next_value(cap_ms);
        else if (arg == "--memory-limit-mb") next_value(memory_mb);
        else if (arg == "--max-scripts") next_value(max_scripts);
        else if (arg == "--telemetry" && i + 1 < argc) telemetry_path = argv[++i];
        else if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
    }
    if (url.empty()) { usage(argv[0]); return 2; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int result = run_page(url, cap_ms, memory_mb, max_scripts, telemetry_path, dump_dom);
    curl_global_cleanup();
    return result;
}
