#pragma once

#ifndef __SYSINFO_H__
#define __SYSINFO_H__

#include <glib.h>


#define  TOP_PROCS  6


typedef struct
{
    char name[ 32 ];
    double value;           // CPU: percent of the whole machine; MEM: bytes
} top_proc_t;


typedef struct
{
    guint64 total, avail, swap_total, swap_free;    // all in kB
} meminfo_t;


// Return 0 on success, -1 if the information is not available.
int sysinfo_cpu_model( char* out, size_t size );
int sysinfo_cpu_freq_mhz( double* mhz );        // average over all cores
int sysinfo_cpu_temp_c( double* celsius );
int sysinfo_meminfo( meminfo_t* mi );
int sysinfo_uptime( double* seconds );

// Formats a size given in kB as "12 GB", "956 MB", "8.6 GB", ...
void sysinfo_format_kb( guint64 kb, char* out, size_t size );


// Per-process sampler. CPU usage needs two samples to be computed (the delta
// between them), so keep the sampler alive between calls.
typedef struct
{
    GHashTable* prev_ticks;     // pid -> guint64*
    guint64 prev_total;
} proc_sampler_t;

void proc_sampler_init( proc_sampler_t* s );
void proc_sampler_free( proc_sampler_t* s );

// Both fill `out` with up to TOP_PROCS items sorted descending and return
// how many were written.
int proc_sampler_top_cpu( proc_sampler_t* s, top_proc_t* out );
int proc_sampler_top_mem( top_proc_t* out );


#endif // __SYSINFO_H__
