#include <litehtml/litehtml.h>

#include "render.h"

#include <cctype>
#include <cstring>
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

    void load_image(const char *, const char *, bool) override {}

    void get_image_size(const char *, const char *, litehtml::size &size) override {
        size.width = 0;
        size.height = 0;
    }

    void draw_background(litehtml::uint_ptr, const std::vector<litehtml::background_paint> &backgrounds) override {
        if (!renderer || backgrounds.empty()) return;
        const litehtml::background_paint &background = backgrounds.back();
        if (background.color.alpha) {
            d_render_fill(renderer, to_rect(background.clip_box), to_color(background.color));
        }
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
    void set_cursor(const char *) override {}

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

    DRenderContext *renderer;
    int width = 1280;
    int height = 1024;
};

}

extern "C" int distill_litehtml_probe(const char *html)
{
    Probe probe(nullptr);
    std::shared_ptr<litehtml::document> document = litehtml::document::createFromString(html ? html : "", &probe);
    return document ? 1 : 0;
}

/* How tall the document becomes at this width. A reader page is as long as
   its content, and rendering it into a fixed-height surface silently discards
   whatever falls past the bottom -- the page looks complete and simply stops. */
extern "C" int distill_litehtml_content_height(const char *html, int width,
                                               const char *font_dir)
{
    if (!html || width <= 0) return 0;
    /* Lay out against a nominal surface; only the resulting height is wanted. */
    DRenderContext *renderer = d_render_create(width, 16, font_dir);
    if (!renderer) return 0;
    Probe probe(renderer);
    probe.width = width;
    probe.height = 16;
    int measured = 0;
    {
        std::shared_ptr<litehtml::document> document =
            litehtml::document::createFromString(html, &probe);
        if (document) {
            document->render(width, litehtml::render_all);
            measured = document->height();
        }
    }
    d_render_destroy(renderer);
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
    DRenderContext *renderer = d_render_create(width, height, font_dir);
    if (!renderer) return 0;
    Probe probe(renderer);
    probe.width = width;
    probe.height = height;
    int result = 0;
    {
        std::shared_ptr<litehtml::document> document = litehtml::document::createFromString(html, &probe);
        if (!document) {
            d_render_destroy(renderer);
            return 0;
        }
        document->render(width, litehtml::render_all);
        litehtml::position clip(0, 0, width, height);
        document->draw(reinterpret_cast<litehtml::uint_ptr>(renderer), 0, 0, &clip);
        result = d_render_write_png(renderer, output_path);
    }
    d_render_destroy(renderer);
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
        copy_box_string(box.type, sizeof(box.type), element->get_attr("type", "text"));
        copy_box_string(box.name, sizeof(box.name), element->get_attr("name", ""));
        copy_box_string(box.value, sizeof(box.value), element->get_attr("value", ""));
        if (!box.value[0] && tag && !std::strcmp(tag, "button")) {
            litehtml::string text;
            element->get_text(text);
            copy_box_string(box.value, sizeof(box.value), text.c_str());
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
