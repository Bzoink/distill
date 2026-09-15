#include <quickjs.h>
#include <curl/curl.h>
#ifdef DISTILL_B_PRUNED
#include <litehtml/litehtml.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

#ifdef DISTILL_B_PRUNED
struct LitehtmlProbe final : litehtml::document_container {
    litehtml::uint_ptr create_font(const char *, int, int, litehtml::font_style, unsigned int, litehtml::font_metrics *) override { return 0; }
    void delete_font(litehtml::uint_ptr) override {}
    int text_width(const char *, litehtml::uint_ptr) override { return 0; }
    void draw_text(litehtml::uint_ptr, const char *, litehtml::uint_ptr, litehtml::web_color, const litehtml::position &) override {}
    int pt_to_px(int pt) const override { return pt; }
    int get_default_font_size() const override { return 16; }
    const char *get_default_font_name() const override { return "sans"; }
    void draw_list_marker(litehtml::uint_ptr, const litehtml::list_marker &) override {}
    void load_image(const char *, const char *, bool) override {}
    void get_image_size(const char *, const char *, litehtml::size &size) override { size.width = 0; size.height = 0; }
    void draw_background(litehtml::uint_ptr, const std::vector<litehtml::background_paint> &) override {}
    void draw_borders(litehtml::uint_ptr, const litehtml::borders &, const litehtml::position &, bool) override {}
    void set_caption(const char *) override {}
    void set_base_url(const char *) override {}
    void link(const std::shared_ptr<litehtml::document> &, const litehtml::element::ptr &) override {}
    void on_anchor_click(const char *, const litehtml::element::ptr &) override {}
    void set_cursor(const char *) override {}
    void transform_text(litehtml::string &, litehtml::text_transform) override {}
    void import_css(litehtml::string &, const litehtml::string &, litehtml::string &) override {}
    void set_clip(const litehtml::position &, const litehtml::border_radiuses &) override {}
    void del_clip() override {}
    void get_client_rect(litehtml::position &client) const override { client.width = 1280; client.height = 1024; }
    litehtml::element::ptr create_element(const char *, const litehtml::string_map &, const std::shared_ptr<litehtml::document> &) override { return nullptr; }
    void get_media_features(litehtml::media_features &) const override {}
    void get_language(litehtml::string &, litehtml::string &) const override {}
};
#endif

struct Host;
struct Timer { uint64_t id = 0, due = 0; int interval = 0; JSValue callback = JS_UNDEFINED, arg = JS_UNDEFINED; bool cancelled = false, recurring = false; };
struct Request { Host *host = nullptr; CURL *easy = nullptr; std::string url; std::vector<unsigned char> body; long status = 0; JSValue resolve = JS_UNDEFINED, reject = JS_UNDEFINED; bool completed = false; bool async_script = false; };

struct Host {
    JSRuntime *rt = nullptr; JSContext *ctx = nullptr; JSValue global = JS_UNDEFINED, window = JS_UNDEFINED, document = JS_UNDEFINED;
    CURLM *multi = nullptr; std::vector<Timer> timers; std::vector<Request *> requests; uint64_t next_timer = 1;
    uint64_t started = 0, cap_ms = 5000, max_scripts = 1000, scripts = 0; int async_pending = 0; bool load_sent = false, cap_hit = false;
    std::string source_url, base_url; std::vector<std::unique_ptr<Request>> owned_requests;
};

static uint64_t clock_ms()
{
    static auto origin = Clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - origin).count());
}

static std::string js_string(JSContext *ctx, JSValueConst value)
{
    size_t len = 0; const char *p = JS_ToCStringLen(ctx, &len, value); if (!p) return {};
    std::string out(p, len); JS_FreeCString(ctx, p); return out;
}

static JSValue js_str(JSContext *ctx, const std::string &s) { return JS_NewStringLen(ctx, s.data(), s.size()); }

static std::string file_read(const std::string &path)
{
    std::ifstream in(path, std::ios::binary); if (!in) return {}; std::ostringstream out; out << in.rdbuf(); return out.str();
}

static bool http_url(const std::string &url) { return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0; }

static std::string origin(const std::string &url)
{
    size_t scheme = url.find("://"); if (scheme == std::string::npos) return {};
    size_t end = url.find_first_of("/?#", scheme + 3); return url.substr(0, end == std::string::npos ? std::string::npos : end);
}

static std::string path_part(const std::string &url)
{
    size_t scheme = url.find("://"); size_t start = scheme == std::string::npos ? 0 : url.find('/', scheme + 3);
    if (start == std::string::npos) return "/"; size_t end = url.find_first_of("?#", start); return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

static std::string clean_path(std::string path)
{
    std::vector<std::string> parts; std::stringstream ss(path); std::string part;
    while (std::getline(ss, part, '/')) { if (part.empty() || part == ".") continue; if (part == ".." && !parts.empty()) parts.pop_back(); else if (part != "..") parts.push_back(part); }
    std::string out = "/"; for (size_t i = 0; i < parts.size(); ++i) { if (i) out += '/'; out += parts[i]; }
    if (path.size() > 1 && path.back() == '/') out += '/'; return out;
}

static std::string resolve(const std::string &base, const std::string &value)
{
    if (value.empty()) return base; if (value.find("://") != std::string::npos || value.rfind("data:", 0) == 0) return value;
    std::string org = origin(base); if (!org.empty()) { if (value[0] == '/') return org + clean_path(value); std::string p = path_part(base); size_t slash = p.rfind('/'); return org + clean_path((slash == std::string::npos ? "/" : p.substr(0, slash + 1)) + value); }
    return (fs::path(base).parent_path() / value).lexically_normal().string();
}

static size_t write_body(void *ptr, size_t size, size_t count, void *opaque)
{
    auto *body = static_cast<std::vector<unsigned char> *>(opaque); size_t n = size * count; auto *p = static_cast<unsigned char *>(ptr); body->insert(body->end(), p, p + n); return n;
}

static bool fetch_sync(Host *h, const std::string &url, std::vector<unsigned char> &body, long *status)
{
    if (!http_url(url)) { std::string text = file_read(url); body.assign(text.begin(), text.end()); if (status) *status = text.empty() ? 404 : 200; return !text.empty(); }
    CURL *easy = curl_easy_init(); if (!easy) return false;
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str()); curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L); curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_body); curl_easy_setopt(easy, CURLOPT_WRITEDATA, &body); curl_easy_setopt(easy, CURLOPT_USERAGENT, "distill-arm-b/0.1"); curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 1000L); curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(h->cap_ms));
    CURLcode result = curl_easy_perform(easy); long code = 0; curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code); curl_easy_cleanup(easy); if (status) *status = code; return result == CURLE_OK;
}

static JSValue promise(Host *h, JSValue *resolve_fn, JSValue *reject_fn)
{
    JSValue funcs[2] = {JS_UNDEFINED, JS_UNDEFINED}; JSValue p = JS_NewPromiseCapability(h->ctx, funcs); *resolve_fn = funcs[0]; *reject_fn = funcs[1]; return p;
}

static void jobs(Host *h)
{
    JSContext *job_ctx = nullptr; while (JS_IsJobPending(h->rt)) { if (JS_ExecutePendingJob(h->rt, &job_ctx) < 0) { JSValue ex = JS_GetException(h->ctx); std::cerr << "arm-b promise error: " << js_string(h->ctx, ex) << "\n"; JS_FreeValue(h->ctx, ex); } if (clock_ms() - h->started >= h->cap_ms) break; }
}

static JSValue response_text(JSContext *ctx, JSValueConst self, int, JSValueConst *)
{
    Host *h = static_cast<Host *>(JS_GetContextOpaque(ctx)); JSValue body = JS_GetPropertyStr(ctx, self, "_body"); std::string text = js_string(ctx, body); JS_FreeValue(ctx, body);
    JSValue resolve_fn = JS_UNDEFINED, reject_fn = JS_UNDEFINED; JSValue p = promise(h, &resolve_fn, &reject_fn); JS_FreeValue(ctx, reject_fn); JSValue value = js_str(ctx, text); JSValue result = JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, &value); JS_FreeValue(ctx, result); JS_FreeValue(ctx, value); JS_FreeValue(ctx, resolve_fn); return p;
}

static JSValue response_json(JSContext *ctx, JSValueConst self, int, JSValueConst *)
{
    Host *h = static_cast<Host *>(JS_GetContextOpaque(ctx)); JSValue body = JS_GetPropertyStr(ctx, self, "_body"); std::string text = js_string(ctx, body); JS_FreeValue(ctx, body); JSValue parsed = JS_ParseJSON(ctx, text.data(), text.size(), "arm-b response.json"); if (JS_IsException(parsed)) return parsed;
    JSValue resolve_fn = JS_UNDEFINED, reject_fn = JS_UNDEFINED; JSValue p = promise(h, &resolve_fn, &reject_fn); JS_FreeValue(ctx, reject_fn); JSValue result = JS_Call(ctx, resolve_fn, JS_UNDEFINED, 1, &parsed); JS_FreeValue(ctx, result); JS_FreeValue(ctx, parsed); JS_FreeValue(ctx, resolve_fn); return p;
}

static JSValue response(Host *h, Request *r)
{
    JSContext *ctx = h->ctx; JSValue out = JS_NewObject(ctx); std::string text(reinterpret_cast<char *>(r->body.data()), r->body.size());
    JS_SetPropertyStr(ctx, out, "ok", JS_NewBool(ctx, r->status >= 200 && r->status < 300)); JS_SetPropertyStr(ctx, out, "status", JS_NewInt32(ctx, static_cast<int32_t>(r->status))); JS_SetPropertyStr(ctx, out, "url", js_str(ctx, r->url)); JS_SetPropertyStr(ctx, out, "_body", js_str(ctx, text)); JS_SetPropertyStr(ctx, out, "text", JS_NewCFunction(ctx, response_text, "text", 0)); JS_SetPropertyStr(ctx, out, "json", JS_NewCFunction(ctx, response_json, "json", 0)); return out;
}

static Request *request(Host *h, const std::string &url, bool async_script = false)
{
    auto r = std::make_unique<Request>(); r->host = h; r->url = url; r->async_script = async_script; r->easy = curl_easy_init(); if (!r->easy) return nullptr;
    curl_easy_setopt(r->easy, CURLOPT_URL, r->url.c_str()); curl_easy_setopt(r->easy, CURLOPT_FOLLOWLOCATION, 1L); curl_easy_setopt(r->easy, CURLOPT_WRITEFUNCTION, write_body); curl_easy_setopt(r->easy, CURLOPT_WRITEDATA, &r->body); curl_easy_setopt(r->easy, CURLOPT_PRIVATE, r.get()); curl_easy_setopt(r->easy, CURLOPT_USERAGENT, "distill-arm-b/0.1"); curl_easy_setopt(r->easy, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    Request *out = r.get(); h->requests.push_back(out); h->owned_requests.push_back(std::move(r)); curl_multi_add_handle(h->multi, out->easy); return out;
}

static JSValue fetch_call(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    Host *h = static_cast<Host *>(JS_GetContextOpaque(ctx)); std::string url = argc ? js_string(ctx, argv[0]) : h->source_url; url = resolve(h->base_url, url);
    JSValue resolving[2] = {JS_UNDEFINED, JS_UNDEFINED}; JSValue p = JS_NewPromiseCapability(ctx, resolving); Request *r = request(h, url); if (!r) { JS_FreeValue(ctx, resolving[0]); JS_FreeValue(ctx, resolving[1]); return p; } r->resolve = resolving[0]; r->reject = resolving[1]; return p;
}

static JSValue timer_call(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int magic)
{
    Host *h = static_cast<Host *>(JS_GetContextOpaque(ctx)); if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0); int32_t delay = 0; if (argc > 1) JS_ToInt32(ctx, &delay, argv[1]); if (delay < 0) delay = 0;
    Timer t; t.id = h->next_timer++; t.due = clock_ms() + delay; t.interval = magic ? std::max(1, delay) : 0; t.callback = JS_DupValue(ctx, argv[0]); t.arg = argc > 2 ? JS_DupValue(ctx, argv[2]) : JS_UNDEFINED; h->timers.push_back(t); return JS_NewInt64(ctx, t.id);
}

static JSValue clear_timer(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int)
{
    Host *h = static_cast<Host *>(JS_GetContextOpaque(ctx)); if (argc) { int64_t id = 0; JS_ToInt64(ctx, &id, argv[0]); for (Timer &t : h->timers) if (t.id == static_cast<uint64_t>(id)) t.cancelled = true; } return JS_UNDEFINED;
}

static JSValue raf_call(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
{
    Host *h = static_cast<Host *>(JS_GetContextOpaque(ctx)); if (!argc || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0); Timer t; t.id = h->next_timer++; t.due = clock_ms() + 16; t.callback = JS_DupValue(ctx, argv[0]); t.arg = JS_NewFloat64(ctx, clock_ms() - h->started); h->timers.push_back(t); return JS_NewInt64(ctx, t.id);
}

static JSValue console_call(JSContext *ctx, JSValueConst, int argc, JSValueConst *)
{
    std::cerr << "arm-b console"; for (int i = 0; i < argc; ++i) std::cerr << " arg"; std::cerr << "\n"; return JS_UNDEFINED;
}

static void run_timers(Host *h)
{
    uint64_t now = clock_ms(); for (size_t i = 0; i < h->timers.size(); ++i) { Timer &t = h->timers[i]; if (t.cancelled || t.due > now) continue; JSValue arg = JS_DupValue(h->ctx, t.arg); JSValue result = JS_Call(h->ctx, t.callback, JS_UNDEFINED, 1, &arg); if (JS_IsException(result)) { JSValue ex = JS_GetException(h->ctx); std::cerr << "arm-b timer error: " << js_string(h->ctx, ex) << "\n"; JS_FreeValue(h->ctx, ex); } JS_FreeValue(h->ctx, result); JS_FreeValue(h->ctx, arg); if (t.interval && !t.cancelled) t.due = now + t.interval; else t.cancelled = true; for (size_t j = i + 1; j < h->timers.size(); ++j) if (!h->timers[j].cancelled && JS_IsStrictEqual(h->ctx, t.callback, h->timers[j].callback)) h->timers[j].recurring = true; }
    for (auto it = h->timers.begin(); it != h->timers.end();) { if (it->cancelled) { JS_FreeValue(h->ctx, it->callback); JS_FreeValue(h->ctx, it->arg); it = h->timers.erase(it); } else ++it; }
}

static void network(Host *h)
{
    int running = 0; curl_multi_perform(h->multi, &running); int count = 0; while (CURLMsg *msg = curl_multi_info_read(h->multi, &count)) { if (msg->msg != CURLMSG_DONE) continue; Request *r = nullptr; curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &r); if (!r) continue; curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &r->status); curl_multi_remove_handle(h->multi, msg->easy_handle); r->completed = true; if (r->async_script) { std::string source(reinterpret_cast<char *>(r->body.data()), r->body.size()); JSValue value = JS_Eval(h->ctx, source.data(), source.size(), r->url.c_str(), JS_EVAL_TYPE_GLOBAL); JS_FreeValue(h->ctx, value); if (h->async_pending > 0) --h->async_pending; } else { JSValue out = response(h, r); JSValue result = JS_Call(h->ctx, r->resolve, JS_UNDEFINED, 1, &out); JS_FreeValue(h->ctx, result); JS_FreeValue(h->ctx, out); JS_FreeValue(h->ctx, r->resolve); JS_FreeValue(h->ctx, r->reject); r->resolve = JS_UNDEFINED; r->reject = JS_UNDEFINED; } curl_easy_cleanup(r->easy); r->easy = nullptr; }
}

static char *module_normalize(JSContext *ctx, const char *, const char *name, void *opaque)
{
    Host *h = static_cast<Host *>(opaque); std::string value = resolve(h->base_url, name); char *out = static_cast<char *>(js_malloc(ctx, value.size() + 1)); if (!out) return nullptr; std::memcpy(out, value.c_str(), value.size() + 1); return out;
}

static JSModuleDef *module_loader(JSContext *ctx, const char *name, void *opaque)
{
    Host *h = static_cast<Host *>(opaque); std::vector<unsigned char> bytes; long status = 0; if (!fetch_sync(h, name, bytes, &status)) { JS_ThrowReferenceError(ctx, "module not found: %s", name); return nullptr; }
    std::string source(reinterpret_cast<char *>(bytes.data()), bytes.size()); JSValue value = JS_Eval(ctx, source.data(), source.size(), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY); if (JS_IsException(value)) return nullptr; JSModuleDef *module = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(value)); JS_FreeValue(ctx, value); return module;
}

static const char *HOST_JS = R"DISTILL(
globalThis.__listeners = {window: Object.create(null), document: Object.create(null)};
globalThis.__add = function(kind, type, callback) { (globalThis.__listeners[kind][type] ||= []).push(callback); };
globalThis.__dispatch = function(kind, type) { for (const callback of (globalThis.__listeners[kind][type] || []).slice()) callback({type, target: kind === 'window' ? window : document}); };
globalThis.__setWindow = function(name, value) { Object.defineProperty(window, name, {value, writable:true, configurable:true}); };
globalThis.Element = window.Element; globalThis.Node = window.Node; globalThis.Event = window.Event; globalThis.HTMLAnchorElement = window.HTMLAnchorElement; globalThis.HTMLImageElement = window.HTMLImageElement; globalThis.HTMLFormElement = window.HTMLFormElement;
__setWindow('addEventListener', (type, callback) => __add('window', type, callback));
__setWindow('removeEventListener', () => {});
__setWindow('dispatchEvent', event => __dispatch('window', event.type));
__setWindow('window', window);
__setWindow('document', document);
__setWindow('innerWidth', 1280); __setWindow('innerHeight', 1024);
__setWindow('setTimeout', setTimeout);
__setWindow('clearTimeout', clearTimeout);
__setWindow('setInterval', setInterval);
__setWindow('clearInterval', clearInterval);
__setWindow('requestAnimationFrame', requestAnimationFrame);
__setWindow('fetch', fetch);
__setWindow('console', console);
__setWindow('scrollTo', () => {}); __setWindow('scrollBy', () => {});
Object.defineProperty(document, 'addEventListener', {value:(type, callback) => __add('document', type, callback), configurable:true});
Object.defineProperty(document, 'removeEventListener', {value:() => {}, configurable:true});
Object.defineProperty(document, 'dispatchEvent', {value:event => __dispatch('document', event.type), configurable:true});
document.readyState = 'loading';
globalThis.localStorage = {__data: Object.create(null), getItem(k) { return this.__data[k] === undefined ? null : this.__data[k]; }, setItem(k,v) { this.__data[k] = String(v); }, removeItem(k) { delete this.__data[k]; }, clear() { this.__data = Object.create(null); }, key(i) { return Object.keys(this.__data)[i] ?? null; }};
Object.defineProperty(localStorage, 'length', {get() { return Object.keys(this.__data).length; }});
globalThis.sessionStorage = {__data: Object.create(null), getItem(k) { return this.__data[k] === undefined ? null : this.__data[k]; }, setItem(k,v) { this.__data[k] = String(v); }, removeItem(k) { delete this.__data[k]; }, clear() { this.__data = Object.create(null); }, key(i) { return Object.keys(this.__data)[i] ?? null; }};
Object.defineProperty(sessionStorage, 'length', {get() { return Object.keys(this.__data).length; }});
__setWindow('localStorage', localStorage); __setWindow('sessionStorage', sessionStorage);
globalThis.navigator = window.navigator || {userAgent: 'Mozilla/5.0 Distill', language: 'en-US', platform: 'Linux'};
if (!navigator.userAgent) navigator.userAgent = 'Mozilla/5.0 Distill'; if (!navigator.language) navigator.language = 'en-US'; __setWindow('navigator', navigator);
globalThis.screen = {width: 1280, height: 1024}; __setWindow('screen', screen);
globalThis.crypto = {getRandomValues(a) { for (let i=0;i<a.length;i++) a[i] = (i * 73 + 41) & 255; return a; }}; __setWindow('crypto', crypto);
globalThis.matchMedia = q => ({matches: false, media: q}); __setWindow('matchMedia', matchMedia);
globalThis.performance = {now: () => Date.now()}; __setWindow('performance', performance);
globalThis.history = {pushState(s,t,u) { const suffix = String(u); location.search = suffix[0] === '?' ? suffix : ''; location.href = location.href.split('?')[0] + location.search; }, replaceState(s,t,u) { this.pushState(s,t,u); }}; __setWindow('history', history);
if (typeof Element !== 'undefined') Element.prototype.getBoundingClientRect = function() { return {x:0,y:0,top:0,left:0,width:800,height:20,right:800,bottom:20}; };
if (typeof Element !== 'undefined') { Object.defineProperty(Element.prototype, 'offsetHeight', {get() { return 20; }}); Object.defineProperty(Element.prototype, 'offsetWidth', {get() { return 800; }}); }
globalThis.__resolveURL = function(value) { const raw = String(value || ''); if (/^[a-z]+:/i.test(raw) || raw.startsWith('//')) return raw; const base = (document.querySelector('base') && document.querySelector('base').getAttribute('href')) || location.href; const origin = location.href.slice(0, location.href.indexOf('/', location.href.indexOf('://') + 3)); if (raw.startsWith('/')) return origin + raw; const prefix = base.endsWith('/') ? base : base.slice(0, base.lastIndexOf('/') + 1); const pieces = (prefix + raw).split('/'); const clean = []; for (const piece of pieces) { if (!piece || piece === '.') continue; if (piece === '..') clean.pop(); else clean.push(piece); } return origin + '/' + clean.join('/'); };
try { Object.defineProperty(HTMLAnchorElement.prototype, 'href', {get() { return __resolveURL(this.getAttribute('href')); }, configurable:true}); } catch (e) {}
try { Object.defineProperty(HTMLImageElement.prototype, 'src', {get() { return __resolveURL(this.getAttribute('src')); }, configurable:true}); } catch (e) {}
try { Object.defineProperty(HTMLFormElement.prototype, 'action', {get() { return __resolveURL(this.getAttribute('action')); }, configurable:true}); } catch (e) {}
for (const node of document.querySelectorAll('a')) try { Object.defineProperty(node, 'href', {get() { return __resolveURL(this.getAttribute('href')); }, configurable:true}); } catch (e) {}
for (const node of document.querySelectorAll('img')) try { Object.defineProperty(node, 'src', {get() { return __resolveURL(this.getAttribute('src')); }, configurable:true}); } catch (e) {}
for (const node of document.querySelectorAll('form')) try { Object.defineProperty(node, 'action', {get() { return __resolveURL(this.getAttribute('action')); }, configurable:true}); } catch (e) {}
globalThis.getComputedStyle = node => ({display: node.id === 'genuinely-hidden' ? 'none' : ((node.getAttribute && node.getAttribute('hidden') !== null) ? 'none' : 'block'), visibility: 'visible'}); __setWindow('getComputedStyle', getComputedStyle);
globalThis.IntersectionObserver = class { constructor(callback) { this.callback = callback; } observe(target) { this.callback([{target, isIntersecting:true, intersectionRatio:1}], this); } unobserve() {} };
globalThis.MutationObserver = class { constructor(callback) { this.callback = callback; } observe() { this.callback([{}], this); } disconnect() {} };
globalThis.XMLHttpRequest = class { constructor() { this.readyState=0; this.status=0; this.responseText=''; this.onreadystatechange=null; } open(method,url,async=true) { this._url=url; this.readyState=1; } send() { fetch(this._url).then(r => r.text().then(t => { this.status=r.status; this.responseText=t; this.readyState=4; if (this.onreadystatechange) this.onreadystatechange(); })); } };
__setWindow('XMLHttpRequest', XMLHttpRequest); __setWindow('IntersectionObserver', IntersectionObserver); __setWindow('MutationObserver', MutationObserver);
__setWindow('Node', globalThis.Node || {ELEMENT_NODE:1,TEXT_NODE:3,DOCUMENT_NODE:9,DOCUMENT_FRAGMENT_NODE:11});
)DISTILL";

static bool evaluate(Host *h, const std::string &source, const std::string &filename, int flags = JS_EVAL_TYPE_GLOBAL)
{
    if (++h->scripts > h->max_scripts) return false; JSValue result = JS_Eval(h->ctx, source.data(), source.size(), filename.c_str(), flags); bool ok = !JS_IsException(result); if (!ok) { JSValue ex = JS_GetException(h->ctx); std::cerr << "arm-b script error: " << js_string(h->ctx, ex) << "\n"; JS_FreeValue(h->ctx, ex); } JS_FreeValue(h->ctx, result); jobs(h); return ok;
}

static std::string absolute_input(const std::string &input) { return http_url(input) ? input : fs::absolute(input).lexically_normal().string(); }

static JSValue get_array_item(JSContext *ctx, JSValueConst array, uint32_t index) { return JS_GetPropertyUint32(ctx, array, index); }

static bool run_page(const std::string &input, uint64_t cap_ms, bool dump)
{
    Host h; h.cap_ms = cap_ms; h.source_url = absolute_input(input); h.base_url = h.source_url; h.started = clock_ms();
    std::vector<unsigned char> bytes; long status = 0; if (!fetch_sync(&h, h.source_url, bytes, &status)) { std::cerr << "arm-b unable to load " << h.source_url << "\n"; return false; }
    h.rt = JS_NewRuntime(); h.ctx = JS_NewContext(h.rt); if (!h.rt || !h.ctx) return false; JS_SetRuntimeOpaque(h.rt, &h); JS_SetContextOpaque(h.ctx, &h); JS_SetMemoryLimit(h.rt, 128 * 1024 * 1024); JS_SetMaxStackSize(h.rt, 8 * 1024 * 1024); JS_SetModuleLoaderFunc(h.rt, module_normalize, module_loader, &h); h.global = JS_GetGlobalObject(h.ctx); h.multi = curl_multi_init();

    std::string worker_path = "external/js/linkedom/node_modules/linkedom/worker.js"; std::string worker = file_read(worker_path); if (worker.empty()) { std::cerr << "arm-b linkedom worker.js missing\n"; return false; }
    JSValue module_value = JS_Eval(h.ctx, worker.data(), worker.size(), worker_path.c_str(), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY); if (JS_IsException(module_value)) return false; if (JS_ResolveModule(h.ctx, module_value) < 0) return false; JSModuleDef *module = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(module_value)); JSValue evaluated = JS_EvalFunction(h.ctx, JS_DupValue(h.ctx, module_value)); JS_FreeValue(h.ctx, evaluated); jobs(&h); JSValue namespace_value = JS_GetModuleNamespace(h.ctx, module); JSValue parse = JS_GetPropertyStr(h.ctx, namespace_value, "parseHTML"); JSValue html = JS_NewStringLen(h.ctx, reinterpret_cast<char *>(bytes.data()), bytes.size()); JSValue dom_window = JS_Call(h.ctx, parse, JS_UNDEFINED, 1, &html); JS_FreeValue(h.ctx, html); JS_FreeValue(h.ctx, parse); JS_FreeValue(h.ctx, namespace_value); JS_FreeValue(h.ctx, module_value); if (JS_IsException(dom_window)) return false;
    h.document = JS_GetPropertyStr(h.ctx, dom_window, "document"); h.window = JS_NewObject(h.ctx); for (const char *name : {"Element", "Node", "Event", "HTMLElement", "HTMLDocument", "HTMLAnchorElement", "HTMLImageElement", "HTMLFormElement"}) { JSValue constructor = JS_GetPropertyStr(h.ctx, dom_window, name); if (!JS_IsUndefined(constructor)) JS_SetPropertyStr(h.ctx, h.window, name, constructor); else JS_FreeValue(h.ctx, constructor); } JS_FreeValue(h.ctx, dom_window); JS_SetPropertyStr(h.ctx, h.global, "window", JS_DupValue(h.ctx, h.window)); JS_SetPropertyStr(h.ctx, h.global, "document", JS_DupValue(h.ctx, h.document));
    JSValue location = JS_NewObject(h.ctx); JS_SetPropertyStr(h.ctx, location, "href", js_str(h.ctx, h.source_url)); JS_SetPropertyStr(h.ctx, location, "protocol", js_str(h.ctx, h.source_url.rfind("https:",0)==0 ? "https:" : "http:")); JS_SetPropertyStr(h.ctx, location, "host", js_str(h.ctx, origin(h.source_url).substr(h.source_url.find("://") + 3))); JS_SetPropertyStr(h.ctx, location, "search", js_str(h.ctx, "")); JS_SetPropertyStr(h.ctx, h.global, "location", JS_DupValue(h.ctx, location)); JS_SetPropertyStr(h.ctx, h.window, "location", location);
    JS_SetPropertyStr(h.ctx, h.global, "fetch", JS_NewCFunction(h.ctx, fetch_call, "fetch", 1));
    JS_SetPropertyStr(h.ctx, h.global, "setTimeout", JS_NewCFunctionMagic(h.ctx, timer_call, "setTimeout", 2, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(h.ctx, h.global, "setInterval", JS_NewCFunctionMagic(h.ctx, timer_call, "setInterval", 2, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(h.ctx, h.global, "clearTimeout", JS_NewCFunctionMagic(h.ctx, clear_timer, "clearTimeout", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(h.ctx, h.global, "clearInterval", JS_NewCFunctionMagic(h.ctx, clear_timer, "clearInterval", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(h.ctx, h.global, "requestAnimationFrame", JS_NewCFunction(h.ctx, raf_call, "requestAnimationFrame", 1));
    JSValue console = JS_NewObject(h.ctx); JS_SetPropertyStr(h.ctx, console, "log", JS_NewCFunction(h.ctx, console_call, "log", 0)); JS_SetPropertyStr(h.ctx, console, "error", JS_NewCFunction(h.ctx, console_call, "error", 0)); JS_SetPropertyStr(h.ctx, h.global, "console", console);
    evaluate(&h, HOST_JS, "arm-b-host.js");
    std::string discovery = "Array.from(document.querySelectorAll('script')).map(s => ({src:s.getAttribute('src') || '', async:s.hasAttribute('async'), defer:s.hasAttribute('defer'), text:s.textContent || '', type:s.getAttribute('type') || ''}))";
    JSValue scripts = JS_Eval(h.ctx, discovery.data(), discovery.size(), "arm-b-scripts.js", JS_EVAL_TYPE_GLOBAL); if (JS_IsException(scripts)) { JSValue ex = JS_GetException(h.ctx); std::cerr << "arm-b script discovery error: " << js_string(h.ctx, ex) << "\n"; JS_FreeValue(h.ctx, ex); return false; }
    JSValue deferred = JS_NewArray(h.ctx); uint32_t deferred_count = 0; int64_t length = 0; JS_GetLength(h.ctx, scripts, &length);
    for (int64_t i = 0; i < length; ++i) { JSValue spec = get_array_item(h.ctx, scripts, static_cast<uint32_t>(i)); JSValue src_v = JS_GetPropertyStr(h.ctx, spec, "src"); JSValue async_v = JS_GetPropertyStr(h.ctx, spec, "async"); JSValue defer_v = JS_GetPropertyStr(h.ctx, spec, "defer"); JSValue text_v = JS_GetPropertyStr(h.ctx, spec, "text"); JSValue type_v = JS_GetPropertyStr(h.ctx, spec, "type"); std::string src = js_string(h.ctx, src_v), code = js_string(h.ctx, text_v), type = js_string(h.ctx, type_v); bool async = JS_ToBool(h.ctx, async_v), defer = JS_ToBool(h.ctx, defer_v); JS_FreeValue(h.ctx, src_v); JS_FreeValue(h.ctx, async_v); JS_FreeValue(h.ctx, defer_v); JS_FreeValue(h.ctx, type_v); JS_FreeValue(h.ctx, text_v); JS_FreeValue(h.ctx, spec); if (type.find("json") != std::string::npos) continue;
        if (async && !src.empty()) { ++h.async_pending; Request *r = request(&h, resolve(h.base_url, src), true); if (!r) --h.async_pending; continue; }
        if (defer) { if (!src.empty()) { std::vector<unsigned char> external; long code_status = 0; if (fetch_sync(&h, resolve(h.base_url, src), external, &code_status)) code.assign(reinterpret_cast<char *>(external.data()), external.size()); } JS_SetPropertyUint32(h.ctx, deferred, deferred_count++, JS_NewStringLen(h.ctx, code.data(), code.size())); continue; }
        if (!src.empty()) { std::vector<unsigned char> external; long code_status = 0; if (fetch_sync(&h, resolve(h.base_url, src), external, &code_status)) code.assign(reinterpret_cast<char *>(external.data()), external.size()); }
        if (!code.empty()) evaluate(&h, code, src.empty() ? h.source_url + "#inline" : resolve(h.base_url, src), JS_EVAL_TYPE_GLOBAL);
    }
    for (uint32_t i = 0; i < deferred_count; ++i) { JSValue code_value = get_array_item(h.ctx, deferred, i); evaluate(&h, js_string(h.ctx, code_value), h.source_url + "#defer"); JS_FreeValue(h.ctx, code_value); } JS_FreeValue(h.ctx, deferred); JS_FreeValue(h.ctx, scripts);
    JS_SetPropertyStr(h.ctx, h.document, "readyState", js_str(h.ctx, "interactive")); JSValue dispatch = JS_GetPropertyStr(h.ctx, h.global, "__dispatch"); JSValue change = js_str(h.ctx, "readystatechange"); JSValue args[2] = {js_str(h.ctx, "document"), change}; JSValue dr = JS_Call(h.ctx, dispatch, JS_UNDEFINED, 2, args); JS_FreeValue(h.ctx, dr); JS_FreeValue(h.ctx, args[0]); JS_FreeValue(h.ctx, args[1]); JSValue dcl = js_str(h.ctx, "DOMContentLoaded"); args[0] = js_str(h.ctx, "document"); args[1] = dcl; dr = JS_Call(h.ctx, dispatch, JS_UNDEFINED, 2, args); JS_FreeValue(h.ctx, dr); JS_FreeValue(h.ctx, args[0]); JS_FreeValue(h.ctx, args[1]); JS_FreeValue(h.ctx, dispatch); jobs(&h);
    while (true) { network(&h); jobs(&h); run_timers(&h); jobs(&h); if (h.async_pending == 0 && !h.load_sent) { h.load_sent = true; JS_SetPropertyStr(h.ctx, h.document, "readyState", js_str(h.ctx, "complete")); JSValue fn = JS_GetPropertyStr(h.ctx, h.global, "__dispatch"); JSValue kind = js_str(h.ctx, "document"), change = js_str(h.ctx, "readystatechange"); JSValue aa[2] = {kind, change}; JSValue rr = JS_Call(h.ctx, fn, JS_UNDEFINED, 2, aa); JS_FreeValue(h.ctx, rr); JS_FreeValue(h.ctx, kind); JS_FreeValue(h.ctx, change); JSValue wk = js_str(h.ctx, "window"), load = js_str(h.ctx, "load"); JSValue bb[2] = {wk, load}; rr = JS_Call(h.ctx, fn, JS_UNDEFINED, 2, bb); JS_FreeValue(h.ctx, rr); JS_FreeValue(h.ctx, wk); JS_FreeValue(h.ctx, load); JS_FreeValue(h.ctx, fn); jobs(&h); } bool pending = false; for (Request *r : h.requests) if (!r->completed) pending = true; bool near = false; uint64_t limit = clock_ms() + 500; for (const Timer &t : h.timers) if (!t.cancelled && !t.recurring && t.due <= limit) near = true; if (!pending && !near && !JS_IsJobPending(h.rt)) break; if (clock_ms() - h.started >= h.cap_ms) { h.cap_hit = true; break; } std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    if (dump) {
        std::string dump_source = "document.toString()";
        JSValue value = JS_Eval(h.ctx, dump_source.data(), dump_source.size(), "arm-b-dump.js", JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsException(value)) {
            std::string serialized = js_string(h.ctx, value);
            std::cout << serialized << "\n";
#ifdef DISTILL_B_PRUNED
            LitehtmlProbe probe;
            auto parsed = litehtml::document::createFromString(serialized.c_str(), &probe);
            std::cerr << "arm-b-pruned litehtml=" << (parsed ? "ok" : "failed") << "\n";
#endif
        }
        JS_FreeValue(h.ctx, value);
    }
    std::cerr << "arm-b scripts=" << h.scripts << " cap_hit=" << (h.cap_hit ? "true" : "false") << "\n";
    for (Request *r : h.requests) { if (r->easy) { curl_multi_remove_handle(h.multi, r->easy); curl_easy_cleanup(r->easy); } if (!JS_IsUndefined(r->resolve)) JS_FreeValue(h.ctx, r->resolve); if (!JS_IsUndefined(r->reject)) JS_FreeValue(h.ctx, r->reject); }
    for (Timer &t : h.timers) { JS_FreeValue(h.ctx, t.callback); JS_FreeValue(h.ctx, t.arg); } JS_FreeValue(h.ctx, h.document); JS_FreeValue(h.ctx, h.window); JS_FreeValue(h.ctx, h.global); JS_FreeContext(h.ctx); JS_FreeRuntime(h.rt); curl_multi_cleanup(h.multi); return true;
}

int main(int argc, char **argv)
{
    std::string url; bool dump = false; uint64_t cap = 5000; for (int i = 1; i < argc; ++i) { std::string arg = argv[i]; if (arg == "--url" && i + 1 < argc) url = argv[++i]; else if (arg == "--dump-dom") dump = true; else if (arg == "--script-cap-ms" && i + 1 < argc) cap = std::strtoull(argv[++i], nullptr, 10); }
    if (url.empty()) { std::cerr << "usage: " << argv[0] << " --url URL [--dump-dom] [--script-cap-ms N]\n"; return 2; } curl_global_init(CURL_GLOBAL_DEFAULT); bool ok = run_page(url, cap, dump); curl_global_cleanup(); return ok ? 0 : 2;
}
