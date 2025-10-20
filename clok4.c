#include <time.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/time.h>
#include <gtk/gtk.h>
#include <librsvg/rsvg.h>

// GTK4 clock using Cairo, based on GTK2 cairo-clock by Mirco "MacSlow" Müller (2006)
// Copyright 2025 Sami Farin
//
// Themes are compatible, but:
//   INSTALL and theme.conf are ignored
//   configuration files are saved to ~/.config/clok4/clok4.conf
//   -u option specifies that ~/.config/clok4/themes is searched for themes instead of /usr/share/clok4

#define M_PI     3.14159265358979323846
#define APP_NAME "clok4"

typedef enum {
    CLOCK_DROP_SHADOW = 0,
    CLOCK_FACE,
    CLOCK_MARKS,
    CLOCK_HOUR_HAND_SHADOW,
    CLOCK_MINUTE_HAND_SHADOW,
    CLOCK_SECOND_HAND_SHADOW,
    CLOCK_HOUR_HAND,
    CLOCK_MINUTE_HAND,
    CLOCK_SECOND_HAND,
    CLOCK_FACE_SHADOW,
    CLOCK_GLASS,
    CLOCK_FRAME,
    CLOCK_ELEMENTS
} LayerElement;

static RsvgHandle *g_svg_handles[CLOCK_ELEMENTS];
static guint clock_width = 400, clock_height = 400;
static int resized_width, resized_height;
static gchar *theme;
static int refresh_rate = 5;
static gboolean userthemes;
static gboolean dont_show_seconds;
static GtkWidget *g_window = NULL;
static GtkWidget *g_clock_widget = NULL;
static GTimer *g_clock_timer = NULL;
static GKeyFile *key_file;
static gchar *config_file;
static gchar *config_dir;
static char *themesystem = "/usr/share/clok4";

// Cached background texture
static GdkTexture *bg_cache_texture = NULL;
static int bg_cache_w = 0, bg_cache_h = 0;

// Forward declarations
#define CLOCK_TYPE_WIDGET (clock_widget_get_type())
G_DECLARE_FINAL_TYPE(ClockWidget, clock_widget, CLOCK, WIDGET, GtkWidget)

struct _ClockWidget {
    GtkWidget parent_instance;
};

G_DEFINE_TYPE(ClockWidget, clock_widget, GTK_TYPE_WIDGET)

static RsvgHandle *load_svg(const char *filename, gboolean needed) {
    GError *err = NULL;
    char *full = g_strconcat(userthemes ? config_dir : themesystem, G_DIR_SEPARATOR_S, "themes", G_DIR_SEPARATOR_S,
                             theme, G_DIR_SEPARATOR_S, filename, NULL);
    RsvgHandle *h = rsvg_handle_new_from_file(full, &err);
    if (!h) {
        gchar errstring[4096];
        g_snprintf(errstring, sizeof(errstring), "[%s] Cannot load SVG from %s: %s", needed ? "ERROR" : "WARNING", full,
                   err ? err->message : "unknown error");
        g_warning("%s", errstring);
        g_clear_error(&err);
        if (needed)
            exit(EXIT_FAILURE);
    }
    g_free(full);
    return h;
}

static gboolean tick(gpointer user_data) {
    if (g_clock_widget)
        gtk_widget_queue_draw(g_clock_widget);
    return TRUE;
}

static void load_transparent_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const gchar *css = "window, box, widget { background-color: transparent; }";
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

// Create cached background texture (called once per size change)
static void ensure_bg_cache(int width, int height) {
    if (bg_cache_texture && width == bg_cache_w && height == bg_cache_h) {
        return;  // Already cached at this size
    }

    if (bg_cache_texture) {
        g_object_unref(bg_cache_texture);
        bg_cache_texture = NULL;
    }

    // Create a Cairo surface to render the background
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(surface);

    // Clear to transparent
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // Scale and render all static background layers
    cairo_save(cr);
    double sx = (double)width / clock_width;
    double sy = (double)height / clock_height;
    cairo_scale(cr, sx, sy);

    RsvgRectangle viewport = {0.0, 0.0, (double)clock_width, (double)clock_height};

    static const LayerElement bg_layers[] = {CLOCK_DROP_SHADOW, CLOCK_FACE,  CLOCK_MARKS,
                                             CLOCK_FACE_SHADOW, CLOCK_GLASS, CLOCK_FRAME};

    for (size_t i = 0; i < sizeof(bg_layers) / sizeof(bg_layers[0]); i++) {
        if (g_svg_handles[bg_layers[i]]) {
            rsvg_handle_render_document(g_svg_handles[bg_layers[i]], cr, &viewport, NULL);
        }
    }

    cairo_restore(cr);
    cairo_destroy(cr);

    // Convert Cairo surface to GdkTexture
    GBytes *bytes =
        g_bytes_new_with_free_func(cairo_image_surface_get_data(surface),
                                   cairo_image_surface_get_height(surface) * cairo_image_surface_get_stride(surface),
                                   (GDestroyNotify)cairo_surface_destroy, surface);

    bg_cache_texture =
        gdk_memory_texture_new(width, height, GDK_MEMORY_DEFAULT, bytes, cairo_image_surface_get_stride(surface));

    g_bytes_unref(bytes);

    bg_cache_w = width;
    bg_cache_h = height;
}

// Custom widget snapshot function - optimized version
static void clock_widget_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    int width = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);

    if (width <= 0 || height <= 0)
        return;

    // Load SVGs once
    static gboolean svgs_loaded = FALSE;
    if (!svgs_loaded) {
        g_svg_handles[CLOCK_DROP_SHADOW] = load_svg("clock-drop-shadow.svg", TRUE);
        g_svg_handles[CLOCK_FACE] = load_svg("clock-face.svg", TRUE);
        g_svg_handles[CLOCK_FACE_SHADOW] = load_svg("clock-face-shadow.svg", FALSE);
        g_svg_handles[CLOCK_MARKS] = load_svg("clock-marks.svg", FALSE);
        g_svg_handles[CLOCK_MINUTE_HAND] = load_svg("clock-minute-hand.svg", TRUE);
        g_svg_handles[CLOCK_MINUTE_HAND_SHADOW] = load_svg("clock-minute-hand-shadow.svg", FALSE);
        g_svg_handles[CLOCK_HOUR_HAND] = load_svg("clock-hour-hand.svg", TRUE);
        g_svg_handles[CLOCK_HOUR_HAND_SHADOW] = load_svg("clock-hour-hand-shadow.svg", FALSE);
        g_svg_handles[CLOCK_GLASS] = load_svg("clock-glass.svg", FALSE);
        g_svg_handles[CLOCK_FRAME] = load_svg("clock-frame.svg", FALSE);

        if (!dont_show_seconds) {
            g_svg_handles[CLOCK_SECOND_HAND] = load_svg("clock-second-hand.svg", FALSE);
            g_svg_handles[CLOCK_SECOND_HAND_SHADOW] = load_svg("clock-second-hand-shadow.svg", FALSE);
        }

        // Get intrinsic size from drop shadow
        if (g_svg_handles[CLOCK_DROP_SHADOW]) {
            gdouble w, h;
            rsvg_handle_get_intrinsic_size_in_pixels(g_svg_handles[CLOCK_DROP_SHADOW], &w, &h);
            clock_width = (int)ceil(w);
            clock_height = (int)ceil(h);
        }

        svgs_loaded = TRUE;
    }

    // Ensure background cache is ready
    ensure_bg_cache(width, height);

    // Draw cached background texture (fast!)
    if (bg_cache_texture) {
        graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
        gtk_snapshot_append_texture(snapshot, bg_cache_texture, &bounds);
    }

    // Get current time
    struct timespec ts;
    struct tm tm;
    time_t time_sec;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_sec = ts.tv_sec;
    localtime_r(&time_sec, &tm);

    int hour = tm.tm_hour;
    int minute = tm.tm_min;
    double second = tm.tm_sec + ((double)ts.tv_nsec / 1e9);

    // Calculate angles in radians
    double angle_hour = (hour % 12) * 30.0 + (minute * 0.5) + (second * (0.5 / 60.0));
    double angle_minute = minute * 6.0 + (second * 0.1);
    double angle_second = second * 6.0;

    double angle_hour_rad = angle_hour * (M_PI / 180.0);
    double angle_min_rad = angle_minute * (M_PI / 180.0);
    double angle_sec_rad = angle_second * (M_PI / 180.0);

    // Draw hands using Cairo (only 6 small SVGs per frame)
    graphene_rect_t hand_bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
    cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &hand_bounds);

    cairo_save(cr);
    double sx = (double)width / clock_width;
    double sy = (double)height / clock_height;
    cairo_translate(cr, width / 2.0, height / 2.0);
    cairo_scale(cr, sx, sy);
    cairo_rotate(cr, -M_PI / 2.0);

    RsvgRectangle viewport = {0.0, 0.0, (double)clock_width, (double)clock_height};

    // Hour hand shadow
    if (g_svg_handles[CLOCK_HOUR_HAND_SHADOW]) {
        cairo_save(cr);
        cairo_translate(cr, 1, 1);
        cairo_rotate(cr, angle_hour_rad);
        rsvg_handle_render_document(g_svg_handles[CLOCK_HOUR_HAND_SHADOW], cr, &viewport, NULL);
        cairo_restore(cr);
    }

    // Minute hand shadow
    if (g_svg_handles[CLOCK_MINUTE_HAND_SHADOW]) {
        cairo_save(cr);
        cairo_translate(cr, 1, 1);
        cairo_rotate(cr, angle_min_rad);
        rsvg_handle_render_document(g_svg_handles[CLOCK_MINUTE_HAND_SHADOW], cr, &viewport, NULL);
        cairo_restore(cr);
    }

    // Second hand shadow
    if (g_svg_handles[CLOCK_SECOND_HAND_SHADOW]) {
        cairo_save(cr);
        cairo_translate(cr, 1, 1);
        cairo_rotate(cr, angle_sec_rad);
        rsvg_handle_render_document(g_svg_handles[CLOCK_SECOND_HAND_SHADOW], cr, &viewport, NULL);
        cairo_restore(cr);
    }

    // Hour hand
    if (g_svg_handles[CLOCK_HOUR_HAND]) {
        cairo_save(cr);
        cairo_rotate(cr, angle_hour_rad);
        rsvg_handle_render_document(g_svg_handles[CLOCK_HOUR_HAND], cr, &viewport, NULL);
        cairo_restore(cr);
    }

    // Minute hand
    if (g_svg_handles[CLOCK_MINUTE_HAND]) {
        cairo_save(cr);
        cairo_rotate(cr, angle_min_rad);
        rsvg_handle_render_document(g_svg_handles[CLOCK_MINUTE_HAND], cr, &viewport, NULL);
        cairo_restore(cr);
    }

    // Second hand
    if (g_svg_handles[CLOCK_SECOND_HAND]) {
        cairo_save(cr);
        cairo_rotate(cr, angle_sec_rad);
        rsvg_handle_render_document(g_svg_handles[CLOCK_SECOND_HAND], cr, &viewport, NULL);
        cairo_restore(cr);
    }

    cairo_restore(cr);
    cairo_destroy(cr);
}

static void clock_widget_measure(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum,
                                 int *natural, int *minimum_baseline, int *natural_baseline) {
    *minimum = 100;
    *natural = 400;
}

static void clock_widget_class_init(ClockWidgetClass *klass) {
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->snapshot = clock_widget_snapshot;
    widget_class->measure = clock_widget_measure;
}

static void clock_widget_init(ClockWidget *self) {
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
}

static GtkWidget *clock_widget_new(void) {
    return g_object_new(CLOCK_TYPE_WIDGET, NULL);
}

static void on_quit_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    g_application_quit(G_APPLICATION(user_data));
}

static void save_key_file(GKeyFile *kf) {
    GError *error = NULL;
    g_key_file_set_integer(kf, "Settings", "width", resized_width);
    g_key_file_set_integer(kf, "Settings", "height", resized_height);
    g_key_file_set_string(kf, "Settings", "theme", theme);
    g_key_file_set_integer(kf, "Settings", "hz", refresh_rate);
    if (!g_key_file_save_to_file(kf, config_file, &error)) {
        g_printerr("Failed to save configuration: %s\n", error->message);
        g_clear_error(&error);
    }
}

static int process_config(int argc, char **argv) {
    GOptionContext *context;
    GError *error = NULL;
    gchar *newtheme;
    GOptionEntry entries[] = {
        {"width", 'w', 0, G_OPTION_ARG_INT, &clock_width, "Width of the window", "WIDTH"},
        {"height", 'h', 0, G_OPTION_ARG_INT, &clock_height, "Height of the window", "HEIGHT"},
        {"theme", 't', 0, G_OPTION_ARG_STRING, &theme, "Theme name", "THEME"},
        {"userthemes", 'u', 0, G_OPTION_ARG_NONE, &userthemes, "Use user theme", "USERTHEMES"},
        {"hz", 'z', 0, G_OPTION_ARG_INT, &refresh_rate, "Refresh rate (hz)", "HZ"},
        {"noseconds", 'n', 0, G_OPTION_ARG_NONE, &dont_show_seconds, "Don't show second hand", "NOSECONDS"},
        {NULL}
    };

    const gchar *user_config_dir = g_get_user_config_dir();
    if (!user_config_dir) {
        g_printerr("Failed to get user config directory\n");
        return 1;
    }

    config_dir = g_build_path(G_DIR_SEPARATOR_S, user_config_dir, APP_NAME, NULL);
    config_file = g_build_path(G_DIR_SEPARATOR_S, config_dir, APP_NAME ".conf", NULL);

    if (!g_file_test(config_dir, G_FILE_TEST_IS_DIR)) {
        if (-1 == g_mkdir_with_parents(config_dir, 0700)) {
            g_printerr("Failed to create directory: %s\n", config_dir);
            return 1;
        }
    }

    key_file = g_key_file_new();
    gboolean config_loaded = g_key_file_load_from_file(key_file, config_file, G_KEY_FILE_NONE, &error);
    if (!config_loaded && !g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        g_printerr("Failed to load configuration: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }
    g_clear_error(&error);

    clock_width = g_key_file_get_integer(key_file, "Settings", "width", NULL);
    if (!clock_width)
        clock_width = 400;
    clock_height = g_key_file_get_integer(key_file, "Settings", "height", NULL);
    if (!clock_height)
        clock_height = 400;
    newtheme = g_key_file_get_string(key_file, "Settings", "theme", NULL);
    if (theme)
        g_free(theme);
    theme = newtheme ? newtheme : g_strdup("default");
    refresh_rate = g_key_file_get_integer(key_file, "Settings", "hz", NULL);
    if (!refresh_rate)
        refresh_rate = 10;

    context = g_option_context_new("- Save configuration for " APP_NAME);
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Option parsing failed: %s\n", error->message);
        g_clear_error(&error);
        g_option_context_free(context);
        return 1;
    }
    g_option_context_free(context);

    return 0;
}

static void on_app_activate_cb(GtkApplication *app, gpointer user_data) {
    load_transparent_css();

    g_window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g_window), APP_NAME);
    gtk_window_set_decorated(GTK_WINDOW(g_window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(g_window), clock_width, clock_height);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(g_window), box);

    GtkWidget *aspect_frame = gtk_aspect_frame_new(0.5, 0.5, 1.0, TRUE);
    gtk_widget_set_hexpand(aspect_frame, TRUE);
    gtk_widget_set_vexpand(aspect_frame, TRUE);
    gtk_box_append(GTK_BOX(box), aspect_frame);

    g_clock_widget = clock_widget_new();
    gtk_aspect_frame_set_child(GTK_ASPECT_FRAME(aspect_frame), g_clock_widget);

    gtk_widget_set_visible(box, TRUE);

    g_clock_timer = g_timer_new();
    g_timeout_add(1000 / refresh_rate, tick, NULL);

    gtk_window_present(GTK_WINDOW(g_window));
}

int main(int argc, char **argv) {
    tzset();

    GtkApplication *app = gtk_application_new(APP_NAME ".CairoClock", G_APPLICATION_DEFAULT_FLAGS);
    GSimpleAction *quit_action = g_simple_action_new("quit", NULL);
    g_signal_connect(quit_action, "activate", G_CALLBACK(on_quit_action), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit_action));

    if (process_config(argc, argv) != 0) {
        exit(EXIT_FAILURE);
    }

    const char *quit_accel[2] = {"<Control>q", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accel);

    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate_cb), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    if (g_window) {
        gtk_window_get_default_size(GTK_WINDOW(g_window), &resized_width, &resized_height);
    } else {
        resized_width = clock_width;
        resized_height = clock_height;
    }

    if (g_clock_timer) {
        g_timer_destroy(g_clock_timer);
        g_clock_timer = NULL;
    }

    save_key_file(key_file);
    g_key_file_free(key_file);

    g_free(config_dir);
    g_free(config_file);

    // Cleanup cached texture
    if (bg_cache_texture) {
        g_object_unref(bg_cache_texture);
        bg_cache_texture = NULL;
    }

    // Cleanup all RsvgHandles
    for (int i = 0; i < CLOCK_ELEMENTS; i++) {
        if (g_svg_handles[i]) {
            g_object_unref(g_svg_handles[i]);
            g_svg_handles[i] = NULL;
        }
    }

    g_object_unref(app);
    return status;
}
