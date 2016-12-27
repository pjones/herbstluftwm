
#include "decoration.h"
#include "client.h"
#include "globals.h"
#include "settings.h"
#include "ewmh.h"
#include "root.h"

#include <stdio.h>
#include <string.h>
#include <sstream>
#include <memory>

using namespace herbstluft;

std::map<Window,HSClient*> Decoration::decwin2client;

static int* g_pseudotile_center_threshold;
static int* g_update_dragged_clients;


static std::shared_ptr<Theme> g_theme;

const Theme& Theme::get() {
    return *g_theme;
}

Theme::Theme(std::string name) : DecTriple(name),
    dec {
        DecTriple("fullscreen"),
        DecTriple("tiling"),
        DecTriple("floating"),
        DecTriple("minimal")
    }
{
    for (int i = 0; i < (int)Type::Count; i++) {
        addStaticChild(&dec[i]);
    }
    // forward attribute changes only to tiling and floating
    active.makeProxyFor({
        &dec[(int)Type::Tiling].active,
        &dec[(int)Type::Floating].active,
    });
    normal.makeProxyFor({
        &dec[(int)Type::Tiling].normal,
        &dec[(int)Type::Floating].normal,
    });
    urgent.makeProxyFor({
        &dec[(int)Type::Tiling].urgent,
        &dec[(int)Type::Floating].urgent,
    });
}

DecorationScheme::DecorationScheme(std::string name)
    : Object(name),
    border_width("border_width", ACCEPT_ALL, 1),
    border_color("color", ACCEPT_ALL, Color::fromStr("black")),
    tight_decoration("tight_decoration", false),
    inner_color("inner_color", ACCEPT_ALL, Color::fromStr("black")),
    inner_width("inner_width", ACCEPT_ALL, 0),
    outer_color("outer_color", ACCEPT_ALL, Color::fromStr("black")),
    outer_width("outer_width", ACCEPT_ALL, 0),
    padding_top("padding_top", ACCEPT_ALL, 0),
    padding_right("padding_right", ACCEPT_ALL, 0),
    padding_bottom("padding_bottom", ACCEPT_ALL, 0),
    padding_left("padding_left", ACCEPT_ALL, 0),
    background_color("background_color", ACCEPT_ALL, Color::fromStr("black"))
{
    wireAttributes({
        &border_width,
        &border_color,
        &tight_decoration,
        &inner_color,
        &inner_width,
        &outer_color,
        &outer_width,
        &padding_top,
        &padding_right,
        &padding_bottom,
        &padding_left,
        &background_color,
    });
}

DecTriple::DecTriple(std::string name)
    : DecorationScheme(name),
      normal("normal"),
      active("active"),
      urgent("urgent")
{
    addStaticChild(&normal);
    addStaticChild(&active);
    addStaticChild(&urgent);
    makeProxyFor({
        &normal,
        &active,
        &urgent,
    });
}

void DecorationScheme::makeProxyFor(std::vector<DecorationScheme*> decs) {
    for (auto it : attributes()) {
        std::string attrib_name = it.first;
        auto this_attribute = it.second;
        // if an attribute of this DecorationScheme is changed, then
        ValueValidator vv = [decs, attrib_name, this_attribute] () {
            // for each decoration to forward the value to
            for (auto dec_it : decs) {
                auto target_attribute = dec_it->attribute(attrib_name);
                // consider only those having an attribute of the same name
                if (target_attribute) {
                    // if writing the attribute fails, abort forwarding.
                    // usually, all attributes should have the same type, so
                    // writing should fail for either all or none.
                    std::string error_msg = target_attribute->change(this_attribute->str());
                    if (error_msg != "") {
                        return error_msg;
                    }
                }
            }
            // if all writes succeeds, then this succeeds as well.
            return std::string("");
        };
        it.second->setOnChange(vv);
    }
}

void decorations_init() {
    g_pseudotile_center_threshold = &(settings_find("pseudotile_center_threshold")->value.i);
    g_update_dragged_clients = &(settings_find("update_dragged_clients")->value.i);
    g_theme = std::make_shared<Theme>("theme");
    Root::get()->addChild(g_theme);
}

void reset_helper(void* data, GString* output) {
    (void) data;
    g_string_append(output, "Writing this resets all attributes to a default value\n");
}

void decorations_destroy() {
}

// from openbox/frame.c
static Visual* check_32bit_client(HSClient* c)
{
    XWindowAttributes wattrib;
    Status ret;

    ret = XGetWindowAttributes(g_display, c->window_, &wattrib);
    HSWeakAssert(ret != BadDrawable);
    HSWeakAssert(ret != BadWindow);

    if (wattrib.depth == 32)
        return wattrib.visual;
    return NULL;
}

Decoration::Decoration(HSClient* client)
    : client(client),
      decwin(0),
      last_scheme(NULL),
	  pixmap(0),
	  pixmap_height(0),
	  pixmap_width(0)
{
}

void Decoration::createWindow() {
    Decoration* dec = this;
    XSetWindowAttributes at;
    long mask = 0;
    // copy attributes from client and not from the root window
    Visual* visual = check_32bit_client(client);
    if (visual) {
        /* client has a 32-bit visual */
        mask = CWColormap | CWBackPixel | CWBorderPixel;
        /* create a colormap with the visual */
        dec->colormap = at.colormap =
            XCreateColormap(g_display, g_root, visual, AllocNone);
        at.background_pixel = BlackPixel(g_display, g_screen);
        at.border_pixel = BlackPixel(g_display, g_screen);
    } else {
        dec->colormap = 0;
    }
    dec->depth = visual
                 ? 32
                 : (DefaultDepth(g_display, DefaultScreen(g_display)));
    dec->decwin = XCreateWindow(g_display, g_root, 0,0, 30, 30, 0,
                        dec->depth,
                        InputOutput,
                        visual
                            ? visual
                            : DefaultVisual(g_display, DefaultScreen(g_display)),
                        mask, &at);
    mask = 0;
    if (visual) {
        /* client has a 32-bit visual */
        mask = CWColormap | CWBackPixel | CWBorderPixel;
        // TODO: why does DefaultColormap work in openbox but crashes hlwm here?
        // It somehow must be incompatible to the visual and thus causes the
        // BadMatch on XCreateWindow
        at.colormap = dec->colormap;
        at.background_pixel = BlackPixel(g_display, g_screen);
        at.border_pixel = BlackPixel(g_display, g_screen);
    }
    dec->bgwin = 0;
    dec->bgwin = XCreateWindow(g_display, dec->decwin, 0,0, 30, 30, 0,
                        dec->depth,
                        InputOutput,
                        CopyFromParent,
                        mask, &at);
    XMapWindow(g_display, dec->bgwin);
    // use a clients requested initial floating size as the initial size
    dec->last_rect_inner = true;
    dec->last_inner_rect = client->float_size_;
    dec->last_outer_rect = client->float_size_; // TODO: is this correct?
    dec->last_actual_rect = dec->last_inner_rect;
    dec->last_actual_rect.x -= dec->last_outer_rect.x;
    dec->last_actual_rect.y -= dec->last_outer_rect.y;
    decwin2client[decwin] = client;
    // set wm_class for window
    XClassHint *hint = XAllocClassHint();
    hint->res_name = (char*)HERBST_DECORATION_CLASS;
    hint->res_class = (char*)HERBST_DECORATION_CLASS;
    XSetClassHint(g_display, dec->decwin, hint);
    XFree(hint);
}

Decoration::~Decoration() {
    decwin2client.erase(decwin);
    if (colormap) {
        XFreeColormap(g_display, colormap);
    }
    if (pixmap) {
        XFreePixmap(g_display, pixmap);
    }
    if (bgwin) {
        XDestroyWindow(g_display, bgwin);
    }
    if (decwin) {
        XDestroyWindow(g_display, decwin);
    }
}

HSClient* Decoration::toClient(Window decoration_window)
{
    auto cl = decwin2client.find(decoration_window);
    if (cl == decwin2client.end()) {
        return NULL;
    } else {
        return cl->second;
    }
}

Rectangle DecorationScheme::outline_to_inner_rect(Rectangle rect) const {
    return {
        rect.x + *border_width + *padding_left,
        rect.y + *border_width + *padding_top,
        rect.width  - 2* *border_width - *padding_left - *padding_right,
        rect.height - 2* *border_width - *padding_top - *padding_bottom
    };
}

Rectangle DecorationScheme::inner_rect_to_outline(Rectangle rect) const {
	unsigned long bw = *border_width;
    return {
        rect.x - bw - *padding_left,
        rect.y - bw - *padding_top,
        rect.width  + 2* bw + *padding_left + *padding_right,
        rect.height + 2* bw + *padding_top + *padding_bottom
    };
}

void Decoration::resize_inner(Rectangle inner, const DecorationScheme& scheme) {
    resize_outline(scheme.inner_rect_to_outline(inner), scheme);
    client->dec.last_rect_inner = true;
}

Rectangle Decoration::inner_to_outer(Rectangle rect) {
    return last_scheme->inner_rect_to_outline(rect);
}

void Decoration::resize_outline(Rectangle outline, const DecorationScheme& scheme)
{
    auto inner = scheme.outline_to_inner_rect(outline);
    // get relative coordinates
    Window decwin = client->dec.decwin;
    Window win = client->window_;

    auto tile = inner;
    client->applysizehints(&inner.width, &inner.height);
    if (!false) { // formely: if (!tight_decoration)
        // center the window in the outline tile
        // but only if it's relative coordinates would not be too close to the
        // upper left tile border
        int threshold = *g_pseudotile_center_threshold;
        int dx = tile.width/2 - inner.width/2;
        int dy = tile.height/2 - inner.height/2;
        inner.x = tile.x + ((dx < threshold) ? 0 : dx);
        inner.y = tile.y + ((dy < threshold) ? 0 : dy);
    }

    //if (RECTANGLE_EQUALS(client->last_size, rect)
    //    && client->last_border_width == border_width) {
    //    return;
    //}

    if (false) { // formely: if (tight_decoration)
        outline = scheme.inner_rect_to_outline(inner);
    }
    client->dec.last_inner_rect = inner;
    inner.x -= outline.x;
    inner.y -= outline.y;
    XWindowChanges changes;
    changes.x = inner.x;
    changes.y = inner.y;
    changes.width = inner.width;
    changes.height = inner.height;
    changes.border_width = 0;

    int mask = CWX | CWY | CWWidth | CWHeight | CWBorderWidth;
    //if (*g_window_border_inner_width > 0
    //    && *g_window_border_inner_width < *g_window_border_width) {
    //    unsigned long current_border_color = get_window_border_color(client);
    //    HSDebug("client_resize %s\n",
    //            current_border_color == g_window_border_active_color
    //            ? "ACTIVE" : "NORMAL");
    //    set_window_double_border(g_display, win, *g_window_border_inner_width,
    //                             g_window_border_inner_color,
    //                             current_border_color);
    //}
    // send new size to client
    // update structs
    bool size_changed = outline.width != client->dec.last_outer_rect.width
                     || outline.height != client->dec.last_outer_rect.height;
    client->dec.last_outer_rect = outline;
    client->dec.last_rect_inner = false;
    client->last_size_ = inner;
    client->dec.last_scheme = &scheme;
    // redraw
    // TODO: reduce flickering
    if (!client->dragged_ || *g_update_dragged_clients) {
        client->dec.last_actual_rect.x = changes.x;
        client->dec.last_actual_rect.y = changes.y;
        client->dec.last_actual_rect.width = changes.width;
        client->dec.last_actual_rect.height = changes.height;
    }
    redrawPixmap();
    XSetWindowBackgroundPixmap(g_display, decwin, client->dec.pixmap);
    if (!size_changed) {
        // if size changes, then the window is cleared automatically
        XClearWindow(g_display, decwin);
    }
    if (!client->dragged_ || *g_update_dragged_clients) {
        XConfigureWindow(g_display, win, mask, &changes);
        XMoveResizeWindow(g_display, client->dec.bgwin,
                          changes.x, changes.y,
                          changes.width, changes.height);
    }
    XMoveResizeWindow(g_display, decwin,
                      outline.x, outline.y, outline.width, outline.height);
    updateFrameExtends();
    if (!client->dragged_ || *g_update_dragged_clients) {
        client->send_configure();
    }
    XSync(g_display, False);
}

void Decoration::updateFrameExtends() {
    int left = last_inner_rect.x - last_outer_rect.x;
    int top  = last_inner_rect.y - last_outer_rect.y;
    int right = last_outer_rect.width - last_inner_rect.width - left;
    int bottom = last_outer_rect.height - last_inner_rect.height - top;
    ewmh_update_frame_extents(client->window_, left,right, top,bottom);
}

void Decoration::change_scheme(const DecorationScheme& scheme) {
    if (client->dec.last_inner_rect.width < 0) {
        // TODO: do something useful here
        return;
    }
    if (client->dec.last_rect_inner) {
        resize_inner(last_inner_rect, scheme);
    } else {
        resize_outline(last_outer_rect, scheme);
    }
}

unsigned int Decoration::get_client_color(Color color) {
    XColor xcol = color.toXColor();
    if (colormap) {
        /* get pixel value back appropriate for client */
        XAllocColor(g_display, colormap, &xcol);
        return xcol.pixel;
    } else {
        /* get pixel value back appropriate for main color map*/
        XAllocColor(g_display, DefaultColormap(g_display, g_screen), &xcol);
        return xcol.pixel;
    }
}

// draw a decoration to the client->dec.pixmap
void Decoration::redrawPixmap() {
    const DecorationScheme& s = *last_scheme;
    auto dec = this;
    Window win = client->dec.decwin;
    auto outer = client->dec.last_outer_rect;
    unsigned int depth = client->dec.depth;
    // TODO: maybe do something like pixmap recreate threshhold?
    bool recreate_pixmap = (dec->pixmap == 0) || (dec->pixmap_width != outer.width)
                                              || (dec->pixmap_height != outer.height);
    if (recreate_pixmap) {
        if (dec->pixmap) {
            XFreePixmap(g_display, dec->pixmap);
        }
        dec->pixmap = XCreatePixmap(g_display, win, outer.width, outer.height, depth);
    }
    Pixmap pix = dec->pixmap;
    GC gc = XCreateGC(g_display, pix, 0, NULL);

    // draw background
    XSetForeground(g_display, gc, get_client_color(s.border_color()));
    XFillRectangle(g_display, pix, gc, 0, 0, outer.width, outer.height);

    // Draw inner border
    int iw = s.inner_width();
    auto inner = client->dec.last_inner_rect;
    inner.x -= client->dec.last_outer_rect.x;
    inner.y -= client->dec.last_outer_rect.y;
    if (iw > 0) {
        /* fill rectangles because drawing does not work */
        XRectangle rects[] = {
            { inner.x - iw, inner.y - iw, inner.width + 2*iw, iw }, /* top */
            { inner.x - iw, inner.y, iw, inner.height },  /* left */
            { inner.x + inner.width, inner.y, iw, inner.height }, /* right */
            { inner.x - iw, inner.y + inner.height, inner.width + 2*iw, iw }, /* bottom */
        };
        XSetForeground(g_display, gc, get_client_color(s.inner_color()));
        XFillRectangles(g_display, pix, gc, rects, LENGTH(rects));
    }

    // Draw outer border
    unsigned long ow = s.outer_width;
    outer.x -= client->dec.last_outer_rect.x;
    outer.y -= client->dec.last_outer_rect.y;
    if (ow > 0) {
        ow = std::min((unsigned int)ow, (outer.height+1) / 2);
        XRectangle rects[] = {
            { 0, 0, outer.width, ow }, /* top */
            { 0, ow, ow, outer.height - 2*ow }, /* left */
            { outer.width-ow, ow, ow, outer.height - 2*ow }, /* right */
            { 0, outer.height - ow, outer.width, ow }, /* bottom */
        };
        XSetForeground(g_display, gc, get_client_color(s.outer_color));
        XFillRectangles(g_display, pix, gc, rects, LENGTH(rects));
    }
    // fill inner rect that is not covered by the client
    XSetForeground(g_display, gc, get_client_color(s.background_color));
    if (dec->last_actual_rect.width < inner.width) {
        XFillRectangle(g_display, pix, gc,
                       dec->last_actual_rect.x + dec->last_actual_rect.width,
                       dec->last_actual_rect.y,
                       inner.width - dec->last_actual_rect.width,
                       dec->last_actual_rect.height);
    }
    if (dec->last_actual_rect.height < inner.height) {
        XFillRectangle(g_display, pix, gc,
                       dec->last_actual_rect.x,
                       dec->last_actual_rect.y + dec->last_actual_rect.height,
                       inner.width,
                       inner.height - dec->last_actual_rect.height);
    }
    // clean up
    XFreeGC(g_display, gc);
}

