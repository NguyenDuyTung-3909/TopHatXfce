#include "popup.h"
 
#include "collector_cpu.h"
#include "collector_mem.h"
#include "sysinfo.h"
#include "utils.h"
 
#include <stdio.h>
#include <string.h>
 
#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
 
 
#define POPUP_WIDTH     290
#define GRAPH_HEIGHT    90
#define AXIS_WIDTH      40
#define REFRESH_MS      1000
 
 
typedef enum { POPUP_CPU, POPUP_MEM } popup_kind_t;
 
 
// A click on the panel takes the focus away from the popup (-> it closes) and
// then reaches the entry (-> it would toggle the popup open again). Remember
// when we closed so that the click can be ignored.
static gint64 last_close_us = 0;
static panel_entry_t* last_close_entry = NULL;
 
 
typedef struct
{
    popup_kind_t kind;
    panel_entry_t* entry;       // (non-owning ptr)
    GtkWidget* popover;         // actually an undecorated GtkWindow
 
    guint timer;
    proc_sampler_t sampler;
 
    // widgets updated on every refresh
    GtkWidget* l_main;          // CPU: utilization, MEM: RAM used
    GtkWidget* l_a;             // CPU: frequency,   MEM: RAM details
    GtkWidget* l_b;             // CPU: temperature, MEM: swap used
    GtkWidget* l_c;             //                   MEM: swap details
    GtkWidget* l_span;          // "N min ago"
    GtkWidget* l_uptime;
    GtkWidget* graph;
    GtkWidget* proc_name[ TOP_PROCS ];
    GtkWidget* proc_val[ TOP_PROCS ];
 
} popup_t;
 
 
//
// small widget helpers
//
 
static GtkWidget* make_label( const char* text, const char* css_class,
                              gfloat xalign )
{
    GtkWidget* l = gtk_label_new( text );
    gtk_label_set_xalign( GTK_LABEL( l ), xalign );
    gtk_label_set_ellipsize( GTK_LABEL( l ), PANGO_ELLIPSIZE_END );
    if ( css_class )
        gtk_style_context_add_class( gtk_widget_get_style_context( l ),
                                     css_class );
    gtk_widget_show( l );
    return l;
}
 
 
static GtkWidget* make_row( GtkWidget* box, const char* left_text,
                            const char* left_class, const char* right_class,
                            GtkWidget** left_out, GtkWidget** right_out )
{
    GtkWidget* row = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 6 );
    GtkWidget* l = make_label( left_text, left_class, 0 );
    GtkWidget* r = make_label( "", right_class, 1 );
    gtk_widget_set_hexpand( l, TRUE );
    gtk_box_pack_start( GTK_BOX( row ), l, TRUE, TRUE, 0 );
    gtk_box_pack_end( GTK_BOX( row ), r, FALSE, FALSE, 0 );
    gtk_widget_show( row );
    gtk_box_pack_start( GTK_BOX( box ), row, FALSE, FALSE, 0 );
    if ( left_out )
        *left_out = l;
    if ( right_out )
        *right_out = r;
    return row;
}
 
 
static void set_text( GtkWidget* label, const char* fmt, ... ) G_GNUC_PRINTF( 2, 3 );
 
static void set_text( GtkWidget* label, const char* fmt, ... )
{
    char buf[ 128 ];
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );
    gtk_label_set_text( GTK_LABEL( label ), buf );
}
 
 
//
// history graph
//
 
static gboolean graph_draw( GtkWidget* widget, cairo_t* cr, gpointer ptr )
{
    popup_t* p = ptr;
    const section_t* sec = p->entry->section;
    const data_t* hist = &p->entry->hist;
 
    int w = gtk_widget_get_allocated_width( widget );
    int h = gtk_widget_get_allocated_height( widget );
    double gw = w - AXIS_WIDTH;
 
    // plot area
    cairo_set_source_rgb( cr, 0.08, 0.08, 0.08 );
    cairo_rectangle( cr, 0, 0, gw, h );
    cairo_fill( cr );
 
    // grid
    cairo_set_source_rgba( cr, 1, 1, 1, 0.12 );
    cairo_set_line_width( cr, 1 );
    for ( int i = 1; i < 4; ++i )
    {
        double y = (int)( h * i / 4.0 ) + 0.5;
        cairo_move_to( cr, 0, y );
        cairo_line_to( cr, gw, y );
    }
    cairo_stroke( cr );
 
    // bars, newest on the right
    size_t cap = data_capacity( hist );
    size_t count = data_count( hist );
    if ( cap > 0 )
    {
        double bar_w = gw / (double) cap;
        cairo_set_source_rgb( cr, sec->graph.rgb_on[ 0 ],
                                  sec->graph.rgb_on[ 1 ],
                                  sec->graph.rgb_on[ 2 ] );
 
        for ( size_t i = 0; i < count; ++i )
        {
            double v = data_at( hist, i );
            v = v < 0 ? 0 : v > 100 ? 100 : v;
            double bh = v / 100.0 * h;
            double x = gw - ( count - i ) * bar_w;
            cairo_rectangle( cr, x, h - bh, bar_w > 1.5 ? bar_w - 0.5 : bar_w,
                                 bh );
        }
        cairo_fill( cr );
    }
 
    // axis labels
    GtkStyleContext* ctx = gtk_widget_get_style_context( widget );
    GdkRGBA fg;
    gtk_style_context_get_color( ctx, gtk_style_context_get_state( ctx ), &fg );
    gdk_cairo_set_source_rgba( cr, &fg );
 
    PangoLayout* lay = gtk_widget_create_pango_layout( widget, NULL );
    static const char* marks[] = { "100%", "50%", "0%" };
    for ( int i = 0; i < 3; ++i )
    {
        pango_layout_set_text( lay, marks[ i ], -1 );
        int tw, th;
        pango_layout_get_pixel_size( lay, &tw, &th );
        double y = h * i / 2.0 - th / 2.0;
        y = y < 0 ? 0 : y > h - th ? h - th : y;
        cairo_move_to( cr, gw + 6, y );
        pango_cairo_show_layout( cr, lay );
    }
    g_object_unref( lay );
 
    return FALSE;
}
 
 
//
// content refresh
//
 
static void fill_procs( popup_t* p, const top_proc_t* procs, int count )
{
    for ( int i = 0; i < TOP_PROCS; ++i )
    {
        if ( i >= count )
        {
            gtk_label_set_text( GTK_LABEL( p->proc_name[ i ] ), "" );
            gtk_label_set_text( GTK_LABEL( p->proc_val[ i ] ), "" );
            continue;
        }
 
        gtk_label_set_text( GTK_LABEL( p->proc_name[ i ] ), procs[ i ].name );
 
        if ( p->kind == POPUP_CPU )
        {
            if ( procs[ i ].value < 1 )
                gtk_label_set_text( GTK_LABEL( p->proc_val[ i ] ), "< 1%" );
            else
                set_text( p->proc_val[ i ], "%.0f%%", procs[ i ].value );
        }
        else
        {
            char buf[ 32 ];
            sysinfo_format_kb( (guint64)( procs[ i ].value / 1024.0 ), buf,
                               sizeof( buf ) );
            gtk_label_set_text( GTK_LABEL( p->proc_val[ i ] ), buf );
        }
    }
}
 
 
static void fill_uptime( popup_t* p )
{
    double up;
    if ( sysinfo_uptime( &up ) != 0 )
        return;
 
    long s = (long) up;
    long d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
 
    if ( d > 0 )
        set_text( p->l_uptime, "%ld d %ld h %ld min", d, h, m );
    else if ( h > 0 )
        set_text( p->l_uptime, "%ld h %ld min", h, m );
    else
        set_text( p->l_uptime, "%ld minutes", m );
}
 
 
static void fill_span( popup_t* p )
{
    double secs = data_count( &p->entry->hist )
                * p->entry->section->interval / 1000.0;
    if ( secs < 60 )
        set_text( p->l_span, "%.0f s ago", secs );
    else
        set_text( p->l_span, "%.0f mins ago", secs / 60.0 );
}
 
 
static double latest( const data_t* hist )
{
    size_t n = data_count( hist );
    if ( n == 0 )
        return 0;
    double v = data_at( hist, n - 1 );
    return v < 0 ? 0 : v;
}
 
 
static void refresh_cpu( popup_t* p )
{
    set_text( p->l_main, "%.0f%%", latest( &p->entry->hist ) );
 
    double mhz, temp;
    if ( sysinfo_cpu_freq_mhz( &mhz ) == 0 )
        set_text( p->l_a, "%.1f GHz", mhz / 1000.0 );
    else
        gtk_label_set_text( GTK_LABEL( p->l_a ), "n/a" );
 
    if ( sysinfo_cpu_temp_c( &temp ) == 0 )
        set_text( p->l_b, "%.0f °C", temp );
    else
        gtk_label_set_text( GTK_LABEL( p->l_b ), "n/a" );
 
    top_proc_t procs[ TOP_PROCS ];
    int n = proc_sampler_top_cpu( &p->sampler, procs );
    fill_procs( p, procs, n );
}
 
 
static void refresh_mem( popup_t* p )
{
    meminfo_t mi;
    if ( sysinfo_meminfo( &mi ) == 0 )
    {
        char avail[ 32 ], total[ 32 ];
 
        double used = ( mi.total - mi.avail ) * 100.0 / mi.total;
        set_text( p->l_main, "%.0f%%", used );
        sysinfo_format_kb( mi.avail, avail, sizeof( avail ) );
        sysinfo_format_kb( mi.total, total, sizeof( total ) );
        set_text( p->l_a, "%s available of %s", avail, total );
 
        if ( mi.swap_total > 0 )
        {
            double sw = ( mi.swap_total - mi.swap_free ) * 100.0
                      / mi.swap_total;
            set_text( p->l_b, "%.0f%%", sw );
            sysinfo_format_kb( mi.swap_free, avail, sizeof( avail ) );
            sysinfo_format_kb( mi.swap_total, total, sizeof( total ) );
            set_text( p->l_c, "%s available of %s", avail, total );
        }
        else
        {
            gtk_label_set_text( GTK_LABEL( p->l_b ), "n/a" );
            gtk_label_set_text( GTK_LABEL( p->l_c ), "no swap" );
        }
    }
 
    top_proc_t procs[ TOP_PROCS ];
    int n = proc_sampler_top_mem( procs );
    fill_procs( p, procs, n );
}
 
 
static gboolean refresh( gpointer ptr )
{
    popup_t* p = ptr;
 
    if ( p->kind == POPUP_CPU )
        refresh_cpu( p );
    else
        refresh_mem( p );
 
    fill_span( p );
    gtk_widget_queue_draw( p->graph );
    return G_SOURCE_CONTINUE;
}
 
 
//
// actions
//
 
static void on_monitor_clicked( GtkButton* b, popup_t* p )
{
    (void) b;
 
    static const char* cmds[] = { "xfce4-taskmanager", "gnome-system-monitor",
                                  "ksysguard", NULL };
    for ( int i = 0; cmds[ i ]; ++i )
    {
        char* path = g_find_program_in_path( cmds[ i ] );
        if ( !path )
            continue;
        g_free( path );
        g_spawn_command_line_async( cmds[ i ], NULL );
        break;
    }
 
    gtk_widget_destroy( p->popover );
}
 
 
static void on_settings_clicked( GtkButton* b, popup_t* p )
{
    (void) b;
 
    GtkWidget* plugin = gtk_widget_get_ancestor( p->entry->box,
                                                 XFCE_TYPE_PANEL_PLUGIN );
    gtk_widget_destroy( p->popover );
 
    if ( plugin )
        g_signal_emit_by_name( plugin, "configure-plugin" );
}
 
 
static GtkWidget* make_icon_button( const char* icon, GCallback cb, popup_t* p )
{
    GtkWidget* b = gtk_button_new_from_icon_name( icon, GTK_ICON_SIZE_BUTTON );
    gtk_style_context_add_class( gtk_widget_get_style_context( b ),
                                 "circular" );
    g_signal_connect( b, "clicked", cb, p );
    gtk_widget_show_all( b );
    return b;
}
 
 
//
// lifetime
//
 
static void on_popover_destroy( GtkWidget* w, popup_t* p )
{
    (void) w;
 
    if ( p->timer )
        g_source_remove( p->timer );
 
    if ( p->entry->popover == p->popover )
        p->entry->popover = NULL;
 
    last_close_us = g_get_monotonic_time();
    last_close_entry = p->entry;
 
    proc_sampler_free( &p->sampler );
    g_free( p );
}
 
 
static gboolean on_focus_out( GtkWidget* w, GdkEvent* ev, gpointer unused )
{
    (void) ev;
    (void) unused;
    gtk_widget_destroy( w );
    return FALSE;
}
 
 
static gboolean on_key_press( GtkWidget* w, GdkEventKey* ev, gpointer unused )
{
    (void) unused;
    if ( ev->keyval == GDK_KEY_Escape )
    {
        gtk_widget_destroy( w );
        return TRUE;
    }
    return FALSE;
}
 
 
static void apply_css( void )
{
    static gboolean done = FALSE;
    if ( done )
        return;
    done = TRUE;
    static const char *css =
            ".graphinator-popup {"
            "   background-color: @theme_bg_color;"
            "   color: @theme_fg_color;"
            "   border: 1px solid alpha(@theme_fg_color, 0.15);"
            "   border-radius: 16px;"
            "}"

            ".graphinator-popup label {"
            "   color: @theme_fg_color;"
            "}"

            ".graphinator-title {"
            "   font-weight: bold;"
            "}"

            ".graphinator-bold {"
            "   font-weight: bold;"
            "}"

            ".graphinator-big {"
            "   font-size: larger;"
            "}"

            ".graphinator-dim {"
            "   opacity: 0.65;"
            "   font-size: smaller;"
            "}";
    ;
    GtkCssProvider* prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data( prov, css, -1, NULL );
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER( prov ),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION );
    g_object_unref( prov );
}
 
 
// Put the window next to the plugin, on the side away from the panel edge.
// (xfce_panel_plugin_position_widget() crashes with an unrealized window and
// a no-window attach widget, so the position is computed here instead.)
static void place_window( popup_t* p )
{
    GtkWidget* anchor = p->entry->box;
    GdkWindow* gw = gtk_widget_get_window( anchor );
    if ( !gw )
        return;
 
    gint ox = 0, oy = 0;
    gdk_window_get_origin( gw, &ox, &oy );
 
    GtkAllocation a;
    gtk_widget_get_allocation( anchor, &a );
    ox += a.x;
    oy += a.y;
 
    GtkRequisition req;
    gtk_widget_get_preferred_size( p->popover, &req, NULL );
 
    // which edge of the screen is the panel on?
    XfceScreenPosition pos = XFCE_SCREEN_POSITION_NONE;
    GtkWidget* plugin = gtk_widget_get_ancestor( anchor,
                                                 XFCE_TYPE_PANEL_PLUGIN );
    if ( plugin )
        pos = xfce_panel_plugin_get_screen_position(
                  XFCE_PANEL_PLUGIN( plugin ) );
 
    gint x = ox, y = oy + a.height;             // default: below (top panel)
 
    if ( xfce_screen_position_is_bottom( pos ) )
        y = oy - req.height;                    // above
    else if ( xfce_screen_position_is_left( pos ) )
        { x = ox + a.width; y = oy; }           // to the right
    else if ( xfce_screen_position_is_right( pos ) )
        { x = ox - req.width; y = oy; }         // to the left
 
    // keep it on screen
    GdkDisplay* disp = gdk_window_get_display( gw );
    GdkMonitor* mon = gdk_display_get_monitor_at_point( disp, ox, oy );
    if ( mon )
    {
        GdkRectangle wa;
        gdk_monitor_get_workarea( mon, &wa );
        x = CLAMP( x, wa.x, wa.x + wa.width  - req.width );
        y = CLAMP( y, wa.y, wa.y + wa.height - req.height );
    }
 
    gtk_window_move( GTK_WINDOW( p->popover ), x, y );
}
 
 
static popup_kind_t kind_of( const panel_entry_t* entry )
{
    return collector_equals( &entry->section->collector, &mem_collector )
         ? POPUP_MEM
         : POPUP_CPU;
}
 
 
void popup_toggle( panel_entry_t* entry )
{
    // already open -> close it
    if ( entry->popover )
    {
        gtk_widget_destroy( entry->popover );
        return;
    }
 
    if ( last_close_entry == entry
      && g_get_monotonic_time() - last_close_us < 250 * 1000 )
        return;
 
    popup_t* p = g_new0( popup_t, 1 );
    p->entry = entry;
    p->kind = kind_of( entry );
    proc_sampler_init( &p->sampler );
 
    apply_css();
 
    p->popover = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    entry->popover = p->popover;

    GtkWindow *win = GTK_WINDOW(p->popover);

    // Enable transparent window corners
    gtk_widget_set_app_paintable(p->popover, TRUE);

    GdkScreen *screen = gtk_widget_get_screen(p->popover);

    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);

    if (visual != NULL)
        gtk_widget_set_visual(p->popover, visual);

    gtk_window_set_decorated(win, FALSE);
    gtk_window_set_resizable( win, FALSE );
    gtk_window_set_skip_taskbar_hint( win, TRUE );
    gtk_window_set_skip_pager_hint( win, TRUE );
    gtk_window_set_keep_above( win, TRUE );
    gtk_window_stick( win );
    gtk_window_set_type_hint( win, GDK_WINDOW_TYPE_HINT_UTILITY );
 
    GtkWidget *frame = gtk_event_box_new();
    gtk_style_context_add_class(
        gtk_widget_get_style_context(frame),
        "graphinator-popup"
    );

    gtk_event_box_set_visible_window(
        GTK_EVENT_BOX(frame), TRUE
    );

    gtk_widget_show(frame);

    gtk_container_add(
        GTK_CONTAINER(win), frame
    );
 
    GtkWidget* box = gtk_box_new( GTK_ORIENTATION_VERTICAL, 6 );
    gtk_container_set_border_width( GTK_CONTAINER( box ), 14 );
    gtk_widget_set_size_request( box, POPUP_WIDTH, -1 );
    gtk_container_add( GTK_CONTAINER( frame ), box );
 
    //
    // header
    //
    GtkWidget* title = make_label( p->kind == POPUP_CPU ? "Processor usage"
                                                        : "Memory usage",
                                   "graphinator-title", 0.5 );
    gtk_box_pack_start( GTK_BOX( box ), title, FALSE, FALSE, 4 );
 
    if ( p->kind == POPUP_CPU )
    {
        make_row( box, "Processor utilization:",
                  "graphinator-big", "graphinator-bold", NULL, &p->l_main );
 
        char model[ 128 ] = "";
        sysinfo_cpu_model( model, sizeof( model ) );
        GtkWidget* m = make_label( model, NULL, 0 );
        gtk_widget_set_margin_top( m, 6 );
        gtk_box_pack_start( GTK_BOX( box ), m, FALSE, FALSE, 0 );
 
        make_row( box, "Frequency:",   NULL, NULL, NULL, &p->l_a );
        make_row( box, "Temperature:", NULL, NULL, NULL, &p->l_b );
    }
    else
    {
        make_row( box, "RAM used:", "graphinator-big", "graphinator-bold",
                  NULL, &p->l_main );
        GtkWidget* a = make_label( "", "graphinator-dim", 1 );
        gtk_box_pack_start( GTK_BOX( box ), a, FALSE, FALSE, 0 );
        p->l_a = a;
 
        make_row( box, "Swap used:", "graphinator-big", "graphinator-bold",
                  NULL, &p->l_b );
        GtkWidget* c = make_label( "", "graphinator-dim", 1 );
        gtk_box_pack_start( GTK_BOX( box ), c, FALSE, FALSE, 0 );
        p->l_c = c;
    }
 
    //
    // history graph
    //
    p->graph = gtk_drawing_area_new();
    gtk_widget_set_size_request( p->graph, -1, GRAPH_HEIGHT );
    gtk_widget_set_margin_top( p->graph, 8 );
    g_signal_connect( p->graph, "draw", G_CALLBACK( graph_draw ), p );
    gtk_widget_show( p->graph );
    gtk_box_pack_start( GTK_BOX( box ), p->graph, FALSE, FALSE, 0 );
 
    GtkWidget* span_row = make_row( box, "", "graphinator-dim",
                                    "graphinator-dim", &p->l_span, NULL );
    // "now" sits under the right edge of the plot area, left of the axis
    GList* kids = gtk_container_get_children( GTK_CONTAINER( span_row ) );
    gtk_label_set_text( GTK_LABEL( g_list_last( kids )->data ), "now" );
    gtk_widget_set_margin_end( GTK_WIDGET( g_list_last( kids )->data ),
                               AXIS_WIDTH );
    g_list_free( kids );
 
    //
    // top processes
    //
    GtkWidget* t = make_label( "Top processes", "graphinator-title", 0.5 );
    gtk_widget_set_margin_top( t, 8 );
    gtk_box_pack_start( GTK_BOX( box ), t, FALSE, FALSE, 4 );
 
    for ( int i = 0; i < TOP_PROCS; ++i )
        make_row( box, "", NULL, NULL, &p->proc_name[ i ], &p->proc_val[ i ] );
 
    //
    // uptime (CPU only)
    //
    if ( p->kind == POPUP_CPU )
    {
        GtkWidget* u = make_label( "System uptime", "graphinator-title", 0.5 );
        gtk_widget_set_margin_top( u, 8 );
        gtk_box_pack_start( GTK_BOX( box ), u, FALSE, FALSE, 4 );
 
        p->l_uptime = make_label( "", NULL, 0.5 );
        gtk_box_pack_start( GTK_BOX( box ), p->l_uptime, FALSE, FALSE, 0 );
        fill_uptime( p );
    }
 
    //
    // buttons
    //
    GtkWidget* bb = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 16 );
    gtk_widget_set_halign( bb, GTK_ALIGN_CENTER );
    gtk_widget_set_margin_top( bb, 10 );
    gtk_box_pack_start( GTK_BOX( bb ),
        make_icon_button( "utilities-system-monitor-symbolic",
                          G_CALLBACK( on_monitor_clicked ), p ),
        FALSE, FALSE, 0 );
    gtk_box_pack_start( GTK_BOX( bb ),
        make_icon_button( "emblem-system-symbolic",
                          G_CALLBACK( on_settings_clicked ), p ),
        FALSE, FALSE, 0 );
    gtk_widget_show( bb );
    gtk_box_pack_start( GTK_BOX( box ), bb, FALSE, FALSE, 0 );
 
    // the first CPU sample only primes the sampler, so take it right away;
    // the real numbers show up after the first timer tick
    refresh( p );
    p->timer = g_timeout_add( REFRESH_MS, refresh, p );
 
    g_signal_connect( p->popover, "focus-out-event",
                      G_CALLBACK( on_focus_out ), NULL );
    g_signal_connect( p->popover, "key-press-event",
                      G_CALLBACK( on_key_press ), NULL );
    g_signal_connect( p->popover, "destroy", G_CALLBACK( on_popover_destroy ),
                                             p );
 
    gtk_widget_show( box );
    place_window( p );
    gtk_window_present( win );
}
 