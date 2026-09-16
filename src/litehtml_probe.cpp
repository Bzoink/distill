#include <litehtml/litehtml.h>

#include "image.h"
#include "render.h"

#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Probe final : litehtml::document_container {
    explicit Probe(DRenderContext *context = nullptr) : renderer(context) {}

    litehtml::uint_ptr create_font(const char *face_name, int size, int weight,
                                   litehtml::font_style italic, unsigned int decoration,
                                   litehtml::font_metrics *metrics) override {
        if (!renderer) return 0;
        DRenderFontMetrics output = {};
        DRenderFont *font = d_render_font_create(renderer, face_name, size, weight,
                                                  italic == litehtml::font_style_italic,
                                                  decoration, &output);
        if (metrics && font) {
            metrics->height = output.height;
            metrics->ascent = output.ascent;
            metrics->descent = output.descent;
            metrics->x_height = output.x_height;
            metrics->draw_spaces = output.draw_spaces;
        }
        return reinterpret_cast<litehtml::uint_ptr>(font);
    }

    void delete_font(litehtml::uint_ptr font) override {
        d_render_font_destroy(reinterpret_cast<DRenderFont *>(font));
    }

    int text_width(const char *text, litehtml::uint_ptr font) override {
        return d_render_text_width(reinterpret_cast<DRenderFont *>(font), text);
    }

    void draw_text(litehtml::uint_ptr, const char *text, litehtml::uint_ptr font,
                   litehtml::web_color color, const litehtml::position &position) override {
        if (!renderer) return;
        d_render_draw_text(renderer, text, reinterpret_cast<DRenderFont *>(font),
                           to_color(color), to_rect(position));
    }

    int pt_to_px(int pt) const override { return pt; }
    int get_default_font_size() const override { return 18; }
    const char *get_default_font_name() const override { return "sans"; }

    void draw_list_marker(litehtml::uint_ptr, const litehtml::list_marker &marker) override {
        if (!renderer) return;
        d_render_draw_marker(renderer, to_rect(marker.pos), to_color(marker.color),
                             marker.index > 0, marker.index);
    }

    /* The renderer never fetches. An image is drawn only from a file that is
       already on disk, which in this program means one the engine downloaded:
       keeping the network on one side of that line is what lets the reader
       stay lazy about media and what stops a page from making the paint layer
       issue requests of its own. Anything with a scheme is refused here. */
    static bool local_path(const char *source, std::string &out) {
        if (!source || !*source) return false;
        std::string value(source);
        if (value.compare(0, 7, "file://") == 0) value.erase(0, 7);
        else if (value.find("://") != std::string::npos) return false;
        else if (value.find(':') != std::string::npos) return false;
        if (value.empty()) return false;
        out = value;
        return true;
    }

    const DImage *image_for(const char *source) {
        std::string path;
        if (!local_path(source, path)) return nullptr;
        auto found = images.find(path);
        if (found != images.end())
            return found->second.pixels ? &found->second : nullptr;
        DImage decoded = {};
        /* A failed decode is cached as an empty entry so a broken file is
           opened once rather than on every layout pass and every repaint. */
        if (!d_image_load_file(path.c_str(), &decoded)) decoded = DImage{};
        auto inserted = images.emplace(path, decoded).first;
        return inserted->second.pixels ? &inserted->second : nullptr;
    }

    void load_image(const char *source, const char *, bool) override {
        image_for(source);
    }

    /* Fitting happens here rather than by styling the element. An <img> given
       an explicit CSS width and height in this litehtml build reserves the box
       but paints nothing into it, so the size litehtml is told is the size it
       must lay out and draw. A reader lays out at a fixed width, and an image
       wider than the surface is shown at the surface's width rather than
       overflowing it. */
    void get_image_size(const char *source, const char *, litehtml::size &size) override {
        const DImage *image = image_for(source);
        if (!image) { size.width = 0; size.height = 0; return; }
        size.width = image->width;
        size.height = image->height;
        if (width > 0 && size.width > width) {
            size.height = (int)((double)size.height * width / size.width);
            size.width = width;
            if (size.height < 1) size.height = 1;
        }
    }

    /* litehtml draws <img> through this too, with the source in `image`. */
    void draw_background(litehtml::uint_ptr, const std::vector<litehtml::background_paint> &backgrounds) override {
        if (!renderer || backgrounds.empty()) return;
        const litehtml::background_paint &background = backgrounds.back();
        if (background.color.alpha) {
            d_render_fill(renderer, to_rect(background.clip_box), to_color(background.color));
        }
        if (background.image.empty()) return;
        const DImage *image = image_for(background.image.c_str());
        if (!image) return;
        int width = background.image_size.width > 0 ? background.image_size.width : image->width;
        int height = background.image_size.height > 0 ? background.image_size.height : image->height;
        /* position_x and position_y are document coordinates here, not an
           offset from the origin box: litehtml has already resolved the
           background position. Adding them to origin_box draws the image at
           twice its own offset, which looks like a partly clipped image
           rather than like a wrong coordinate, so it is worth naming. */
        d_render_set_clip(renderer, to_rect(background.clip_box));
        d_render_draw_image(renderer,
                            {background.position_x, background.position_y, width, height},
                            image->pixels, image->width, image->height);
        d_render_del_clip(renderer);
    }

    void draw_borders(litehtml::uint_ptr, const litehtml::borders &borders,
                      const litehtml::position &position, bool) override {
        if (!renderer) return;
        if (borders.top.width && borders.top.style > litehtml::border_style_hidden)
            d_render_fill(renderer, {position.x, position.y, position.width, borders.top.width}, to_color(borders.top.color));
        if (borders.bottom.width && borders.bottom.style > litehtml::border_style_hidden)
            d_render_fill(renderer, {position.x, position.bottom() - borders.bottom.width, position.width, borders.bottom.width}, to_color(borders.bottom.color));
        if (borders.left.width && borders.left.style > litehtml::border_style_hidden)
            d_render_fill(renderer, {position.x, position.y, borders.left.width, position.height}, to_color(borders.left.color));
        if (borders.right.width && borders.right.style > litehtml::border_style_hidden)
            d_render_fill(renderer, {position.right() - borders.right.width, position.y, borders.right.width, position.height}, to_color(borders.right.color));
    }

    void set_caption(const char *) override {}
    void set_base_url(const char *) override {}
    void link(const std::shared_ptr<litehtml::document> &, const litehtml::element::ptr &) override {}
    void on_anchor_click(const char *, const litehtml::element::ptr &) override {}
    /* litehtml resolves the CSS cursor for whatever is under the pointer, so
       the hand over a link comes from the stylesheet rather than from us
       guessing which rectangles are links. */
    void set_cursor(const char *cursor) override { cursor_name = cursor ? cursor : ""; }
    std::string cursor_name;

    void transform_text(litehtml::string &text, litehtml::text_transform transform) override {
        for (char &character : text) {
            unsigned char value = static_cast<unsigned char>(character);
            if (transform == litehtml::text_transform_capitalize)
                character = static_cast<char>(std::toupper(value));
            else if (transform == litehtml::text_transform_uppercase)
                character = static_cast<char>(std::toupper(value));
            else if (transform == litehtml::text_transform_lowercase)
                character = static_cast<char>(std::tolower(value));
        }
    }

    void import_css(litehtml::string &, const litehtml::string &, litehtml::string &) override {}

    void set_clip(const litehtml::position &position, const litehtml::border_radiuses &) override {
        if (renderer) d_render_set_clip(renderer, to_rect(position));
    }

    void del_clip() override {
        if (renderer) d_render_del_clip(renderer);
    }

    void get_client_rect(litehtml::position &client) const override {
        client.width = width;
        client.height = height;
    }

    litehtml::element::ptr create_element(const char *, const litehtml::string_map &, const std::shared_ptr<litehtml::document> &) override { return nullptr; }
    void get_media_features(litehtml::media_features &) const override {}
    void get_language(litehtml::string &, litehtml::string &) const override {}

    static DRenderColor to_color(litehtml::web_color color) {
        return {color.red, color.green, color.blue, color.alpha};
    }

    static DRenderRect to_rect(const litehtml::position &position) {
        return {position.x, position.y, position.width, position.height};
    }

    ~Probe() {
        for (auto &entry : images) d_image_free(&entry.second);
    }

    DRenderContext *renderer;
    int width = 1280;
    int height = 1024;
    /* Decoded once per document, keyed by path: a template may show the same
       file more than once, and layout asks for the size before painting. */
    std::map<std::string, DImage> images;
};

}

struct DLiteHtmlDocument {
    DRenderContext *renderer = nullptr;
    std::unique_ptr<Probe> probe;
    std::shared_ptr<litehtml::document> document;
    int width = 0;
    int surface_height = 0;
    int content_height = 0;
};

extern "C" int distill_litehtml_probe(const char *html)
{
    Probe probe(nullptr);
    std::shared_ptr<litehtml::document> document = litehtml::document::createFromString(html ? html : "", &probe);
    return document ? 1 : 0;
}

extern "C" DLiteHtmlDocument *distill_litehtml_document_create(const char *html,
                                                                  int width,
                                                                  int surface_height,
                                                                  const char *font_dir)
{
    if (!html || width <= 0 || surface_height <= 0) return nullptr;
    std::unique_ptr<DLiteHtmlDocument> result(new DLiteHtmlDocument);
    result->renderer = d_render_create(width, surface_height, font_dir);
    if (!result->renderer) return nullptr;
    result->width = width;
    result->surface_height = surface_height;
    result->probe = std::make_unique<Probe>(result->renderer);
    result->probe->width = width;
    result->probe->height = surface_height;
    result->document = litehtml::document::createFromString(html, result->probe.get());
    if (!result->document) {
        result->probe.reset();
        d_render_destroy(result->renderer);
        result->renderer = nullptr;
        return nullptr;
    }
    result->document->render(width, litehtml::render_all);
    result->content_height = result->document->height();
    return result.release();
}

extern "C" void distill_litehtml_document_destroy(DLiteHtmlDocument *document)
{
    if (!document) return;
    document->document.reset();
    document->probe.reset();
    d_render_destroy(document->renderer);
    delete document;
}

/* What is under a point, asked of the same laid-out document that was
   painted. Collected boxes cannot answer this: an inline element has no single
   placement -- its text lives in one box per line -- so a link inside a
   sentence, or a headline link, has no rectangle to test against. litehtml
   knows, because it did the layout. */
extern "C" int distill_litehtml_document_link_at(DLiteHtmlDocument *document,
                                                 int x, int y,
                                                 char *href_out, size_t href_capacity)
{
    if (href_out && href_capacity) href_out[0] = '\0';
    if (!document || !document->document || !document->probe) return 0;
    litehtml::position::vector redraw;
    document->probe->cursor_name.clear();
    document->document->on_mouse_over(x, y, x, y, redraw);
    litehtml::element::const_ptr over = document->document->get_over_element();
    for (litehtml::element::const_ptr element = over; element; element = element->parent()) {
        const char *tag = element->get_tagName();
        if (!tag || std::strcmp(tag, "a")) continue;
        const char *href = element->get_attr("href", "");
        if (!href || !*href) continue;
        if (href_out && href_capacity) {
            std::strncpy(href_out, href, href_capacity - 1);
            href_out[href_capacity - 1] = '\0';
        }
        return 1;
    }
    return 0;
}

static litehtml::element::const_ptr choice_input_in_label(const litehtml::element::const_ptr &element)
{
    if (!element) return nullptr;
    const char *tag = element->get_tagName();
    const char *type = element->get_attr("type", "");
    if (tag && !std::strcmp(tag, "input") &&
        (!std::strcmp(type, "checkbox") || !std::strcmp(type, "radio"))) return element;
    for (const auto &child : element->children()) {
        litehtml::element::const_ptr choice = choice_input_in_label(child);
        if (choice) return choice;
    }
    return nullptr;
}

extern "C" int distill_litehtml_document_choice_id_at(DLiteHtmlDocument *document,
                                                        int x, int y,
                                                        char *id_out, size_t id_capacity)
{
    if (id_out && id_capacity) id_out[0] = '\0';
    if (!document || !document->document || !document->probe) return 0;
    litehtml::position::vector redraw;
    document->document->on_mouse_over(x, y, x, y, redraw);
    litehtml::element::const_ptr over = document->document->get_over_element();
    for (litehtml::element::const_ptr element = over; element; element = element->parent()) {
        const char *tag = element->get_tagName();
        if (!tag || std::strcmp(tag, "label")) continue;
        litehtml::element::const_ptr input = choice_input_in_label(element);
        const char *id = input ? input->get_attr("id", "") : "";
        if (!id || !*id) return 0;
        if (id_out && id_capacity) {
            std::strncpy(id_out, id, id_capacity - 1);
            id_out[id_capacity - 1] = '\0';
        }
        return 1;
    }
    return 0;
}

extern "C" int distill_litehtml_document_content_height(const DLiteHtmlDocument *document)
{
    return document ? document->content_height : 0;
}

extern "C" int distill_litehtml_document_render_region(DLiteHtmlDocument *document,
                                                          int scroll_y)
{
    if (!document || !document->document || !document->renderer) return 0;
    if (scroll_y < 0) scroll_y = 0;
    d_render_clear(document->renderer);
    litehtml::position clip(0, 0, document->width, document->surface_height);
    document->document->draw(reinterpret_cast<litehtml::uint_ptr>(document->renderer),
                             0, -scroll_y, &clip);
    return 1;
}

extern "C" void *distill_litehtml_document_surface(DLiteHtmlDocument *document)
{
    return document ? d_render_surface(document->renderer) : nullptr;
}

/* How tall the document becomes at this width. A reader page is as long as
   its content, and rendering it into a fixed-height surface silently discards
   whatever falls past the bottom -- the page looks complete and simply stops. */
extern "C" int distill_litehtml_content_height(const char *html, int width,
                                               const char *font_dir)
{
    DLiteHtmlDocument *document = distill_litehtml_document_create(html, width, 1, font_dir);
    if (!document) return 0;
    int measured = distill_litehtml_document_content_height(document);
    distill_litehtml_document_destroy(document);
    return measured;
}

extern "C" int distill_litehtml_render(const char *html, const char *output_path,
                                         int width, int height, const char *font_dir)
{
    if (!html || !output_path || width <= 0) return 0;
    if (height <= 0) {
        /* Fit the content, but stay bounded: the surface is width * height * 4
           bytes and a pathological page must not be allowed to ask for all of
           memory. A page longer than the cap is truncated, as before. */
        height = distill_litehtml_content_height(html, width, font_dir);
        if (height < 1) height = 1;
        if (height > DISTILL_MAX_RENDER_HEIGHT) height = DISTILL_MAX_RENDER_HEIGHT;
    }
    DLiteHtmlDocument *document = distill_litehtml_document_create(html, width, height, font_dir);
    if (!document) return 0;
    int result = distill_litehtml_document_render_region(document, 0) &&
                 d_render_write_png(document->renderer, output_path);
    distill_litehtml_document_destroy(document);
    return result;
}

extern "C" int distill_litehtml_render_region(const char *html, const char *output_path,
                                                int width, int viewport, int scroll_y,
                                                const char *font_dir)
{
    if (!html || !output_path || width <= 0 || viewport <= 0) return 0;
    DLiteHtmlDocument *document = distill_litehtml_document_create(html, width, viewport, font_dir);
    if (!document) return 0;
    int result = distill_litehtml_document_render_region(document, scroll_y) &&
                 d_render_write_png(document->renderer, output_path);
    distill_litehtml_document_destroy(document);
    return result;
}

namespace {

static void copy_box_string(char *destination, size_t capacity, const char *value)
{
    if (!destination || capacity == 0) return;
    const char *source = value ? value : "";
    std::strncpy(destination, source, capacity - 1);
    destination[capacity - 1] = '\0';
}

static bool is_form_control(const char *tag)
{
    return tag && (!std::strcmp(tag, "input") || !std::strcmp(tag, "button") ||
                   !std::strcmp(tag, "select") || !std::strcmp(tag, "textarea"));
}

/* DFormBox crosses the C++ layout/GTK boundary, so select choices travel in a
   compact string rather than leaking litehtml nodes into C. Values and labels
   come from our own reader template; replace the two separators if a page
   happens to include them in an option. */
static void append_option_piece(char *destination, size_t capacity, size_t *used,
                                const char *text)
{
    if (!destination || !capacity || !used) return;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        if (*used + 1 >= capacity) return;
        destination[(*used)++] = (*p == 0x1e || *p == 0x1f) ? ' ' : (char)*p;
    }
    destination[*used] = '\0';
}

static void append_option_separator(char *destination, size_t capacity, size_t *used,
                                    char separator)
{
    if (!destination || !capacity || !used || *used + 1 >= capacity) return;
    destination[(*used)++] = separator;
    destination[*used] = '\0';
}

static void collect_select_options(const litehtml::element::ptr &element, DFormBox *box,
                                   size_t *used)
{
    if (!element || !box || !used) return;
    const char *tag = element->get_tagName();
    if (tag && !std::strcmp(tag, "option")) {
        const char *raw_value = element->get_attr("value", "");
        litehtml::string label;
        element->get_text(label);
        const char *value = raw_value && *raw_value ? raw_value : label.c_str();
        bool selected = !std::strcmp(element->get_attr("data-distill-selected", ""), "1");
        if (selected || !*box->value) {
            copy_box_string(box->value, sizeof(box->value), value);
        }
        append_option_piece(box->options, sizeof(box->options), used, value);
        append_option_separator(box->options, sizeof(box->options), used, '\x1f');
        append_option_piece(box->options, sizeof(box->options), used, label.c_str());
        append_option_separator(box->options, sizeof(box->options), used, '\x1f');
        append_option_piece(box->options, sizeof(box->options), used, selected ? "1" : "0");
        append_option_separator(box->options, sizeof(box->options), used, '\x1e');
    }
    for (const auto &child : element->children())
        collect_select_options(child, box, used);
}

static void collect_form_boxes(const litehtml::element::ptr &element,
                               bool in_form, unsigned int form_index,
                               unsigned int &next_form_index, const char *action,
                               const char *method, std::vector<DFormBox> &output)
{
    if (!element) return;
    const char *tag = element->get_tagName();
    bool child_in_form = in_form;
    unsigned int child_form_index = form_index;
    std::string child_action = action ? action : "";
    std::string child_method = method ? method : "get";
    if (tag && !std::strcmp(tag, "form")) {
        child_in_form = true;
        child_form_index = next_form_index++;
        child_action = element->get_attr("action", "");
        child_method = element->get_attr("method", "get");
    }
    if (is_form_control(tag) && child_in_form) {
        litehtml::position position = element->get_placement();
        DFormBox box = {};
        box.x = position.x;
        box.y = position.y;
        box.width = position.width;
        box.height = position.height;
        box.form_index = child_form_index;
        copy_box_string(box.tag, sizeof(box.tag), tag);
        const char *distill_type = element->get_attr("data-distill-type", "");
        copy_box_string(box.type, sizeof(box.type),
                        distill_type && *distill_type ? distill_type : element->get_attr("type", "text"));
        copy_box_string(box.id, sizeof(box.id), element->get_attr("id", ""));
        copy_box_string(box.name, sizeof(box.name), element->get_attr("name", ""));
        copy_box_string(box.value, sizeof(box.value), element->get_attr("value", ""));
        copy_box_string(box.placeholder, sizeof(box.placeholder),
                        element->get_attr("placeholder", ""));
        box.checked = !std::strcmp(element->get_attr("data-distill-checked", ""), "1");
        box.disabled = !std::strcmp(element->get_attr("data-distill-disabled", ""), "1");
        box.required = !std::strcmp(element->get_attr("data-distill-required", ""), "1");
        if (tag && !std::strcmp(tag, "textarea")) {
            litehtml::string text;
            element->get_text(text);
            copy_box_string(box.value, sizeof(box.value), text.c_str());
        }
        if (tag && !std::strcmp(tag, "select")) {
            size_t used = 0;
            collect_select_options(element, &box, &used);
        }
        if (tag && !std::strcmp(tag, "button")) {
            litehtml::string text;
            element->get_text(text);
            copy_box_string(box.caption, sizeof(box.caption), text.c_str());
        }
        copy_box_string(box.action, sizeof(box.action), child_action.c_str());
        copy_box_string(box.method, sizeof(box.method), child_method.c_str());
        output.push_back(box);
    }
    for (const auto &child : element->children())
        collect_form_boxes(child, child_in_form, child_form_index, next_form_index,
                           child_action.c_str(), child_method.c_str(), output);
}

static void collect_link_boxes(const litehtml::element::ptr &element,
                               std::vector<DLinkBox> &output)
{
    if (!element) return;
    const char *tag = element->get_tagName();
    if (tag && !std::strcmp(tag, "a")) {
        const char *href = element->get_attr("href", "");
        litehtml::position position = element->get_placement();
        if (href && *href && position.width > 0 && position.height > 0) {
            DLinkBox box = {};
            box.x = position.x;
            box.y = position.y;
            box.width = position.width;
            box.height = position.height;
            copy_box_string(box.href, sizeof(box.href), href);
            output.push_back(box);
        }
    }
    for (const auto &child : element->children()) collect_link_boxes(child, output);
}

static size_t copy_form_boxes(const DLiteHtmlDocument *document, DFormBox *boxes,
                              size_t box_capacity)
{
    if (!document || !document->document) return 0;
    std::vector<DFormBox> collected;
    unsigned int next_form_index = 0;
    collect_form_boxes(document->document->root(), false, 0, next_form_index,
                       "", "get", collected);
    size_t count = collected.size();
    if (boxes && box_capacity) {
        size_t copy_count = count < box_capacity ? count : box_capacity;
        std::memcpy(boxes, collected.data(), copy_count * sizeof(*boxes));
    }
    return count;
}

static size_t copy_link_boxes(const DLiteHtmlDocument *document, DLinkBox *boxes,
                              size_t box_capacity)
{
    if (!document || !document->document) return 0;
    std::vector<DLinkBox> collected;
    collect_link_boxes(document->document->root(), collected);
    size_t count = collected.size();
    if (boxes && box_capacity) {
        size_t copy_count = count < box_capacity ? count : box_capacity;
        std::memcpy(boxes, collected.data(), copy_count * sizeof(*boxes));
    }
    return count;
}

}

extern "C" size_t distill_litehtml_document_form_boxes(const DLiteHtmlDocument *document,
                                                          DFormBox *boxes,
                                                          size_t box_capacity)
{
    return copy_form_boxes(document, boxes, box_capacity);
}

extern "C" size_t distill_litehtml_document_link_boxes(const DLiteHtmlDocument *document,
                                                          DLinkBox *boxes,
                                                          size_t box_capacity)
{
    return copy_link_boxes(document, boxes, box_capacity);
}

extern "C" size_t distill_litehtml_form_boxes(const char *html, int width, int height,
                                               const char *font_dir, DFormBox *boxes,
                                               size_t box_capacity)
{
    if (!html || width <= 0 || height <= 0) return 0;
    DRenderContext *renderer = d_render_create(width, height, font_dir);
    if (!renderer) return 0;
    Probe probe(renderer);
    probe.width = width;
    probe.height = height;
    std::shared_ptr<litehtml::document> document = litehtml::document::createFromString(html, &probe);
    if (!document) {
        d_render_destroy(renderer);
        return 0;
    }
    document->render(width, litehtml::render_all);
    std::vector<DFormBox> collected;
    unsigned int next_form_index = 0;
    collect_form_boxes(document->root(), false, 0, next_form_index, "", "get", collected);
    size_t count = collected.size();
    if (boxes && box_capacity) {
        size_t copy_count = count < box_capacity ? count : box_capacity;
        std::memcpy(boxes, collected.data(), copy_count * sizeof(*boxes));
    }
    document.reset();
    d_render_destroy(renderer);
    return count;
}

extern "C" size_t distill_litehtml_link_boxes(const char *html, int width, int height,
                                               const char *font_dir, DLinkBox *boxes,
                                               size_t box_capacity)
{
    if (!html || width <= 0 || height <= 0) return 0;
    DRenderContext *renderer = d_render_create(width, height, font_dir);
    if (!renderer) return 0;
    Probe probe(renderer);
    probe.width = width;
    probe.height = height;
    std::shared_ptr<litehtml::document> document = litehtml::document::createFromString(html, &probe);
    if (!document) {
        d_render_destroy(renderer);
        return 0;
    }
    document->render(width, litehtml::render_all);
    std::vector<DLinkBox> collected;
    collect_link_boxes(document->root(), collected);
    size_t count = collected.size();
    if (boxes && box_capacity) {
        size_t copy_count = count < box_capacity ? count : box_capacity;
        std::memcpy(boxes, collected.data(), copy_count * sizeof(*boxes));
    }
    document.reset();
    d_render_destroy(renderer);
    return count;
}
