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

// TODO optimize cairo redrawing, uses more CPU than GTK2 version (make clock huge and increase hz to test)

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

typedef struct {
    LayerElement element; // The enum value (index into g_svg_handles)
    const char* name;    // String name for logging/debugging
} SvgLayerInfo;

static RsvgHandle *g_svg_handles[CLOCK_ELEMENTS];

static guint clock_width = 400, clock_height = 400;
static int resized_width, resized_height;
static gchar *theme;
static int refresh_rate = 5;
static gboolean userthemes;
static gboolean dont_show_seconds;
static GtkWidget *g_window = NULL;
static GtkWidget *g_drawing_area = NULL;

static cairo_surface_t *bg_cache = NULL;
static int bg_cache_w = 0, bg_cache_h = 0;

static GTimer *g_clock_timer = NULL;
static GKeyFile *key_file;
static gchar *config_file;
static gchar *config_dir;
static char *themesystem = "/usr/share/clok4";

static RsvgHandle *load_svg(const char *filename, gboolean needed) {
    GError *err = NULL;
    char *full = g_strconcat(userthemes ? config_dir : themesystem, G_DIR_SEPARATOR_S, "themes", G_DIR_SEPARATOR_S,
                             theme, G_DIR_SEPARATOR_S, filename, NULL);
    RsvgHandle *h = rsvg_handle_new_from_file(full, &err);
    if (!h) {
        gchar errstring[4096];
        g_snprintf(errstring, sizeof(errstring), "[%s] Cannot load SVG from %s: %s", needed ? "ERROR" : "WARNING", full,
                   err ? err->message : "unknown error");
        g_warning(errstring);
        g_clear_error(&err);
        if (needed)
            exit(EXIT_FAILURE);
    }
    g_free(full);
    return h;
}

static gboolean tick(gpointer user_data) {
    // fprintf(stderr, "tick\n");
    gtk_widget_queue_draw(g_drawing_area);
    return TRUE;
}

static void load_transparent_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const gchar *css = "window, box, drawingarea { background-color: transparent; }";
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void load_clock_svgs(void) {
    g_svg_handles[CLOCK_DROP_SHADOW] = load_svg("clock-drop-shadow.svg", true);
    g_svg_handles[CLOCK_FACE] = load_svg("clock-face.svg", true);
    g_svg_handles[CLOCK_FACE_SHADOW] = load_svg("clock-face-shadow.svg", false);
    g_svg_handles[CLOCK_MARKS] = load_svg("clock-marks.svg", false);
    g_svg_handles[CLOCK_MINUTE_HAND] = load_svg("clock-minute-hand.svg", true);
    g_svg_handles[CLOCK_MINUTE_HAND_SHADOW] = load_svg("clock-minute-hand-shadow.svg", false);
    if (!dont_show_seconds) {
        g_svg_handles[CLOCK_SECOND_HAND] = load_svg("clock-second-hand.svg", false);
        g_svg_handles[CLOCK_SECOND_HAND_SHADOW] = load_svg("clock-second-hand-shadow.svg", false);
    }
    g_svg_handles[CLOCK_HOUR_HAND] = load_svg("clock-hour-hand.svg", true);
    g_svg_handles[CLOCK_HOUR_HAND_SHADOW] = load_svg("clock-hour-hand-shadow.svg", false);
    g_svg_handles[CLOCK_GLASS] = load_svg("clock-glass.svg", false);
    g_svg_handles[CLOCK_FRAME] = load_svg("clock-frame.svg", false);

    if (g_svg_handles[CLOCK_DROP_SHADOW]) {
        gdouble w, h;
        rsvg_handle_get_intrinsic_size_in_pixels(g_svg_handles[CLOCK_DROP_SHADOW], &w, &h);
        clock_width = (int)ceil(w);
        clock_height = (int)ceil(h);
    }
}

static void on_quit_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    g_application_quit(G_APPLICATION(user_data));
}

/* Draw all static layers (drop shadow, face, marks, face-shadow, glass, frame) */
static void draw_static_layers(cairo_t *cr, int width, int height) {
    // Define the drawing order for static layers
    static const SvgLayerInfo static_layers_info[] = {
        { CLOCK_DROP_SHADOW, "CLOCK_DROP_SHADOW" },
        { CLOCK_FACE,        "CLOCK_FACE"        },
        { CLOCK_MARKS,       "CLOCK_MARKS"       },
        // Note: Shadows/Hands are drawn in draw_clock_hands
        { CLOCK_FACE_SHADOW, "CLOCK_FACE_SHADOW" },
        { CLOCK_GLASS,       "CLOCK_GLASS"       },
        { CLOCK_FRAME,       "CLOCK_FRAME"       }
    };

    cairo_save(cr);
    double sx = (double)width / clock_width;
    double sy = (double)height / clock_height;
    cairo_scale(cr, sx, sy); // Keep the scaling

    /* Clear to transparent */
    cairo_set_source_rgba(cr, 1, 1, 1, 0);
    cairo_paint(cr);

    GError *render_error = NULL;
    gboolean success;
    RsvgRectangle viewport = { 0.0, 0.0, (double)clock_width, (double)clock_height };

    // Loop through the static layers
    for (size_t i = 0; i < G_N_ELEMENTS(static_layers_info); ++i) {
        const SvgLayerInfo* info = &static_layers_info[i];
        RsvgHandle* handle = g_svg_handles[info->element];

        if (handle) {
            success = rsvg_handle_render_document(handle, cr, &viewport, &render_error);
            if (!success) {
                g_warning("Failed to render %s: %s",
                          info->name, // Use the name from the struct
                          render_error ? render_error->message : "Unknown error (GError not set)");
                g_clear_error(&render_error); // Clear error immediately after logging
            }
        }
    }

    g_clear_error(&render_error); // Ensure cleared at the end too
    cairo_restore(cr);
}

static void ensure_bg_cache(cairo_t *cr, int width, int height) {
    if (bg_cache && width == bg_cache_w && height == bg_cache_h) {
        // Already valid; no need to rebuild
        return;
    }
    // Rebuild the cached background surface
    if (bg_cache) {
        cairo_surface_destroy(bg_cache);
    }

    bg_cache = cairo_surface_create_similar(cairo_get_target(cr), CAIRO_CONTENT_COLOR_ALPHA, width, height);
    bg_cache_w = width;
    bg_cache_h = height;

    // Render all static (background) stuff
    cairo_t *bg_cr = cairo_create(bg_cache);
    draw_static_layers(bg_cr, width, height);
    cairo_destroy(bg_cr);
}

static void draw_clock_hands(cairo_t *cr, int width, int height) {
    // Define the drawing order for hands and their shadows
    // Shadows are drawn first, then the corresponding hands,
    // layer by layer (e.g., all shadows, then all hands is also possible)
    static const SvgLayerInfo hand_layers_info[] = {
        // Order matters for correct layering!
        { CLOCK_HOUR_HAND_SHADOW,   "CLOCK_HOUR_HAND_SHADOW"   },
        { CLOCK_MINUTE_HAND_SHADOW, "CLOCK_MINUTE_HAND_SHADOW" },
        { CLOCK_SECOND_HAND_SHADOW, "CLOCK_SECOND_HAND_SHADOW" },
        { CLOCK_HOUR_HAND,          "CLOCK_HOUR_HAND"          },
        { CLOCK_MINUTE_HAND,        "CLOCK_MINUTE_HAND"        },
        { CLOCK_SECOND_HAND,        "CLOCK_SECOND_HAND"        }
    };
    struct timespec ts;
    struct tm tm;
    time_t time_sec;

    clock_gettime(CLOCK_REALTIME, &ts);
    time_sec = ts.tv_sec;
    localtime_r(&time_sec, &tm);
    int hour = tm.tm_hour;
    int minute = tm.tm_min;
    double second = tm.tm_sec + ((double)ts.tv_nsec / 1e9);

    // Calculate angles in degrees
    double angle_hour_deg = (hour % 12) * 30.0 + (minute * 0.5) + (second * (0.5 / 60.0));
    double angle_minute_deg = minute * 6.0 + (second * 0.1);
    double angle_second_deg = second * 6.0;

    // Convert angles to radians once
    double angle_hour_rad = angle_hour_deg * (M_PI / 180.0);
    double angle_min_rad = angle_minute_deg * (M_PI / 180.0);
    double angle_sec_rad = angle_second_deg * (M_PI / 180.0);

    cairo_save(cr); // Save overall state before hands
    cairo_translate(cr, width / 2.0, height / 2.0);
    cairo_scale(cr, (double)width / clock_width, (double)height / clock_height); // Keep the scaling
    cairo_rotate(cr, -M_PI / 2.0); // Initial rotation for clock orientation

    GError *render_error = NULL;
    gboolean success;
    RsvgRectangle viewport = { 0.0, 0.0, (double)clock_width, (double)clock_height };

    // Loop through the hand/shadow layers
    for (size_t i = 0; i < G_N_ELEMENTS(hand_layers_info); ++i) {
        const SvgLayerInfo* info = &hand_layers_info[i];
        RsvgHandle* handle = g_svg_handles[info->element];

        if (handle) {
            double current_angle_rad = 0;
            gboolean is_shadow = FALSE;

            // Determine the correct angle and shadow status for this layer
            switch (info->element) {
                case CLOCK_HOUR_HAND_SHADOW:
                    current_angle_rad = angle_hour_rad;
                    is_shadow = TRUE;
                    break;
                case CLOCK_HOUR_HAND:
                    current_angle_rad = angle_hour_rad;
                    is_shadow = FALSE;
                    break;
                case CLOCK_MINUTE_HAND_SHADOW:
                    current_angle_rad = angle_min_rad;
                    is_shadow = TRUE;
                    break;
                case CLOCK_MINUTE_HAND:
                    current_angle_rad = angle_min_rad;
                    is_shadow = FALSE;
                    break;
                case CLOCK_SECOND_HAND_SHADOW:
                    current_angle_rad = angle_sec_rad;
                    is_shadow = TRUE;
                    break;
                case CLOCK_SECOND_HAND:
                    current_angle_rad = angle_sec_rad;
                    is_shadow = FALSE;
                    break;
                default: // Skip any unexpected elements
                    continue;
            }

            // Perform drawing operations for this layer
            cairo_save(cr); // Save state before drawing this specific layer

            if (is_shadow) {
                cairo_translate(cr, 1, 1); // Apply shadow offset
            }
            cairo_rotate(cr, current_angle_rad); // Apply rotation for this hand/shadow

            success = rsvg_handle_render_document(handle, cr, &viewport, &render_error);
            if (!success) {
                g_warning("Failed to render %s: %s",
                          info->name, // Use the name from the struct
                          render_error ? render_error->message : "Unknown error (GError not set)");
                g_clear_error(&render_error); // Clear error immediately
            }

            cairo_restore(cr); // Restore state after drawing this layer
        }
    }

    g_clear_error(&render_error); // Ensure cleared at the end too
    cairo_restore(cr); // Restore overall state from before hands
}

static void on_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    // Ensure background cache is up to date
    ensure_bg_cache(cr, width, height);

    // Paint the cached background
    cairo_set_source_surface(cr, bg_cache, 0, 0);
    cairo_paint(cr);

    // Then draw the moving hands
    draw_clock_hands(cr, width, height);
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
        {"noseconds", 'n', 0, G_OPTION_ARG_NONE, &dont_show_seconds, "Don’t show second hand", "NOSECONDS"},
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

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_window_set_child(GTK_WINDOW(g_window), box);

    GtkWidget *aspect_frame = gtk_aspect_frame_new(0.5, 0.5, 1.0, TRUE);
    gtk_widget_set_hexpand(aspect_frame, TRUE);
    gtk_widget_set_vexpand(aspect_frame, TRUE);
    gtk_box_append(GTK_BOX(box), aspect_frame);

    g_drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(g_drawing_area, TRUE);
    gtk_widget_set_vexpand(g_drawing_area, TRUE);
    gtk_aspect_frame_set_child(GTK_ASPECT_FRAME(aspect_frame), g_drawing_area);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_drawing_area), on_draw, NULL, NULL);

    load_clock_svgs();

    gtk_widget_set_visible(box, TRUE);

    g_clock_timer = g_timer_new();
    /* refresh at chosen hz */
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

    /* Cleanup cached surface */
    if (bg_cache) {
        cairo_surface_destroy(bg_cache);
        bg_cache = NULL;
    }

    /* Cleanup all RsvgHandles */
    for (int i = 0; i < CLOCK_ELEMENTS; i++) {
        if (g_svg_handles[i]) {
            g_object_unref(g_svg_handles[i]);
            g_svg_handles[i] = NULL;
        }
    }

    g_object_unref(app);
    return status;
}
