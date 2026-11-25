#define _POSIX_C_SOURCE 200809L

#include "sysinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>         // sysconf


//
// helpers
//

// Reads a whole (small) text file. Returns NULL on failure, free with g_free.
static char* slurp( const char* path )
{
    char* content = NULL;
    if ( !g_file_get_contents( path, &content, NULL, NULL ) )
        return NULL;
    return content;
}


static int read_long( const char* path, long* out )
{
    char* s = slurp( path );
    if ( !s )
        return -1;
    *out = strtol( s, NULL, 10 );
    g_free( s );
    return 0;
}


//
// CPU
//

int sysinfo_cpu_model( char* out, size_t size )
{
    char* s = slurp( "/proc/cpuinfo" );
    if ( !s )
        return -1;

    int res = -1;
    char* p = strstr( s, "model name" );
    if ( p && ( p = strchr( p, ':' ) ) )
    {
        ++p;
        while ( *p == ' ' || *p == '\t' )
            ++p;
        char* nl = strchr( p, '\n' );
        if ( nl )
            *nl = '\0';
        g_strlcpy( out, p, size );
        res = 0;
    }

    g_free( s );
    return res;
}


int sysinfo_cpu_freq_mhz( double* mhz )
{
    double sum = 0;
    int n = 0;

    for ( int i = 0; i < 1024; ++i )
    {
        char path[ 96 ];
        snprintf( path, sizeof( path ),
                  "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i );

        long khz;
        if ( read_long( path, &khz ) != 0 )
        {
            // cpus are numbered consecutively; offline cpus lack cpufreq
            // so only stop at the first gap after at least one hit
            if ( n > 0 || i > 256 )
                break;
            continue;
        }
        sum += khz / 1000.0;
        ++n;
    }

    if ( n > 0 )
    {
        *mhz = sum / n;
        return 0;
    }

    // fallback: "cpu MHz" lines in /proc/cpuinfo
    char* s = slurp( "/proc/cpuinfo" );
    if ( !s )
        return -1;

    char* p = s;
    while ( ( p = strstr( p, "cpu MHz" ) ) )
    {
        p = strchr( p, ':' );
        if ( !p )
            break;
        sum += strtod( ++p, NULL );
        ++n;
    }
    g_free( s );

    if ( n == 0 )
        return -1;

    *mhz = sum / n;
    return 0;
}


static int name_is_cpu_sensor( const char* name )
{
    static const char* names[] = { "k10temp", "coretemp", "zenpower",
                                   "cpu_thermal", "cpu-thermal", "x86_pkg_temp",
                                   NULL };
    for ( int i = 0; names[ i ]; ++i )
        if ( strcmp( name, names[ i ] ) == 0 )
            return 1;
    return 0;
}


int sysinfo_cpu_temp_c( double* celsius )
{
    // hwmon: k10temp (AMD), coretemp (Intel), ...
    for ( int i = 0; i < 64; ++i )
    {
        char path[ 96 ];
        snprintf( path, sizeof( path ), "/sys/class/hwmon/hwmon%d/name", i );

        char* name = slurp( path );
        if ( !name )
            continue;
        g_strstrip( name );
        int ok = name_is_cpu_sensor( name );
        g_free( name );

        if ( !ok )
            continue;

        // temp1 is Tctl/Tdie (AMD) or the package temperature (Intel)
        snprintf( path, sizeof( path ), "/sys/class/hwmon/hwmon%d/temp1_input",
                  i );
        long milli;
        if ( read_long( path, &milli ) == 0 )
        {
            *celsius = milli / 1000.0;
            return 0;
        }
    }

    // fallback: thermal zones
    for ( int i = 0; i < 64; ++i )
    {
        char path[ 96 ];
        snprintf( path, sizeof( path ), "/sys/class/thermal/thermal_zone%d/type",
                  i );

        char* type = slurp( path );
        if ( !type )
            continue;
        g_strstrip( type );
        int ok = name_is_cpu_sensor( type );
        g_free( type );

        if ( !ok )
            continue;

        snprintf( path, sizeof( path ), "/sys/class/thermal/thermal_zone%d/temp",
                  i );
        long milli;
        if ( read_long( path, &milli ) == 0 )
        {
            *celsius = milli / 1000.0;
            return 0;
        }
    }

    return -1;
}


//
// memory, uptime
//

static guint64 meminfo_field( const char* text, const char* key )
{
    const char* p = strstr( text, key );
    if ( !p )
        return 0;
    return g_ascii_strtoull( p + strlen( key ), NULL, 10 );
}


int sysinfo_meminfo( meminfo_t* mi )
{
    char* s = slurp( "/proc/meminfo" );
    if ( !s )
        return -1;

    // keys include the leading newline so that "SwapFree:" can't match
    // inside e.g. "ZSwapFree:"-like lines; the first line needs a prefix
    char* text = g_strconcat( "\n", s, NULL );
    g_free( s );

    mi->total      = meminfo_field( text, "\nMemTotal:" );
    mi->avail      = meminfo_field( text, "\nMemAvailable:" );
    mi->swap_total = meminfo_field( text, "\nSwapTotal:" );
    mi->swap_free  = meminfo_field( text, "\nSwapFree:" );

    g_free( text );
    return mi->total > 0 ? 0 : -1;
}


int sysinfo_uptime( double* seconds )
{
    char* s = slurp( "/proc/uptime" );
    if ( !s )
        return -1;
    *seconds = strtod( s, NULL );
    g_free( s );
    return 0;
}


void sysinfo_format_kb( guint64 kb, char* out, size_t size )
{
    double mb = kb / 1024.0;
    double gb = mb / 1024.0;

    if ( gb >= 10 )
        snprintf( out, size, "%.0f GB", gb );
    else if ( gb >= 1 )
        snprintf( out, size, "%.1f GB", gb );
    else
        snprintf( out, size, "%.0f MB", mb );
}


//
// processes
//

// Parses /proc/<pid>/stat. The command name is in parentheses and may itself
// contain spaces or parentheses, so look for the *last* ')'.
static int parse_stat( const char* stat, char* comm, size_t comm_size,
                       guint64* ticks )
{
    const char* open = strchr( stat, '(' );
    const char* close = strrchr( stat, ')' );
    if ( !open || !close || close < open )
        return -1;

    size_t len = (size_t)( close - open - 1 );
    if ( len >= comm_size )
        len = comm_size - 1;
    memcpy( comm, open + 1, len );
    comm[ len ] = '\0';

    // after ") " come: state ppid pgrp session tty tpgid flags minflt cminflt
    //                  majflt cmajflt utime stime ...
    char** f = g_strsplit( close + 1, " ", 0 );
    int ok = -1;
    int n = 0;
    while ( f[ n ] )
        ++n;

    // f[0] is "" (space after ')'), f[1] = state (field 3 of the file), so
    // field N of the file is f[N - 2]: utime (14) = f[12], stime (15) = f[13]
    if ( n > 13 )
    {
        *ticks = g_ascii_strtoull( f[ 12 ], NULL, 10 )
               + g_ascii_strtoull( f[ 13 ], NULL, 10 );
        ok = 0;
    }
    g_strfreev( f );
    return ok;
}


static guint64 total_cpu_ticks( void )
{
    char* s = slurp( "/proc/stat" );
    if ( !s )
        return 0;

    guint64 total = 0;
    if ( strncmp( s, "cpu ", 4 ) == 0 )
    {
        // user nice system idle iowait irq softirq steal
        // (guest and guest_nice are already included in user and nice)
        char* p = s + 4;
        for ( int i = 0; i < 8; ++i )
        {
            char* end;
            total += g_ascii_strtoull( p, &end, 10 );
            if ( end == p )
                break;
            p = end;
        }
    }
    g_free( s );
    return total;
}


static int cmp_desc( const void* a, const void* b )
{
    double x = ( (const top_proc_t*) a )->value;
    double y = ( (const top_proc_t*) b )->value;
    return ( x < y ) - ( x > y );
}


// Keeps `out` as a sorted-descending list of the best TOP_PROCS items.
static void top_insert( top_proc_t* out, int* count, const top_proc_t* item )
{
    if ( *count < TOP_PROCS )
    {
        out[ ( *count )++ ] = *item;
    }
    else if ( item->value > out[ TOP_PROCS - 1 ].value )
    {
        out[ TOP_PROCS - 1 ] = *item;
    }
    else
        return;

    qsort( out, *count, sizeof( *out ), cmp_desc );
}


void proc_sampler_init( proc_sampler_t* s )
{
    s->prev_ticks = g_hash_table_new_full( g_direct_hash, g_direct_equal,
                                           NULL, g_free );
    s->prev_total = 0;
}


void proc_sampler_free( proc_sampler_t* s )
{
    if ( s->prev_ticks )
        g_hash_table_destroy( s->prev_ticks );
    s->prev_ticks = NULL;
}


int proc_sampler_top_cpu( proc_sampler_t* s, top_proc_t* out )
{
    int count = 0;

    GDir* dir = g_dir_open( "/proc", 0, NULL );
    if ( !dir )
        return 0;

    guint64 total = total_cpu_ticks();
    guint64 dtotal = total > s->prev_total ? total - s->prev_total : 0;

    GHashTable* now = g_hash_table_new_full( g_direct_hash, g_direct_equal,
                                             NULL, g_free );

    const char* entry;
    while ( ( entry = g_dir_read_name( dir ) ) )
    {
        if ( !g_ascii_isdigit( entry[ 0 ] ) )
            continue;

        char path[ 64 ];
        snprintf( path, sizeof( path ), "/proc/%s/stat", entry );
        char* stat = slurp( path );
        if ( !stat )
            continue;

        top_proc_t item = { { 0 }, 0 };
        guint64 ticks = 0;
        int ok = parse_stat( stat, item.name, sizeof( item.name ), &ticks );
        g_free( stat );
        if ( ok != 0 )
            continue;

        gpointer key = GINT_TO_POINTER( atoi( entry ) );

        guint64* store = g_new( guint64, 1 );
        *store = ticks;
        g_hash_table_insert( now, key, store );

        guint64* old = g_hash_table_lookup( s->prev_ticks, key );
        if ( old && dtotal > 0 && ticks >= *old )
        {
            item.value = (double)( ticks - *old ) / (double) dtotal * 100.0;
            top_insert( out, &count, &item );
        }
    }
    g_dir_close( dir );

    // replace the previous snapshot
    g_hash_table_destroy( s->prev_ticks );
    s->prev_ticks = now;
    s->prev_total = total;

    return count;
}


int proc_sampler_top_mem( top_proc_t* out )
{
    int count = 0;

    GDir* dir = g_dir_open( "/proc", 0, NULL );
    if ( !dir )
        return 0;

    long page = sysconf( _SC_PAGESIZE );
    if ( page <= 0 )
        page = 4096;

    const char* entry;
    while ( ( entry = g_dir_read_name( dir ) ) )
    {
        if ( !g_ascii_isdigit( entry[ 0 ] ) )
            continue;

        char path[ 64 ];
        snprintf( path, sizeof( path ), "/proc/%s/statm", entry );
        char* statm = slurp( path );
        if ( !statm )
            continue;

        // size resident shared text lib data dt   (in pages)
        char* p = strchr( statm, ' ' );
        guint64 rss_pages = p ? g_ascii_strtoull( p + 1, NULL, 10 ) : 0;
        g_free( statm );

        if ( rss_pages == 0 )
            continue;       // kernel threads

        snprintf( path, sizeof( path ), "/proc/%s/stat", entry );
        char* stat = slurp( path );
        if ( !stat )
            continue;

        top_proc_t item = { { 0 }, 0 };
        guint64 ticks;
        int ok = parse_stat( stat, item.name, sizeof( item.name ), &ticks );
        g_free( stat );
        if ( ok != 0 )
            continue;

        item.value = (double) rss_pages * (double) page;
        top_insert( out, &count, &item );
    }
    g_dir_close( dir );

    return count;
}
