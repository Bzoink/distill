#include <litehtml/litehtml.h>

#include <memory>
#include <vector>

namespace {

struct Probe final : litehtml::document_container {
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

}

extern "C" int distill_litehtml_probe(const char *html)
{
    Probe probe;
    std::shared_ptr<litehtml::document> document = litehtml::document::createFromString(html ? html : "", &probe);
    return document ? 1 : 0;
}
