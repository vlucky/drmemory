/* **********************************************************
 * Copyright (c) 2011-2013 Google, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* Front-end to drsyms for Windows */

#include "dr_api.h"
#include "drsyms.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Pull in BUFFER_SIZE_ELEMENTS, IF_WINDOWS, TESTALL, and other useful macros */
#include "utils.h"
#undef sscanf /* we can use sscanf */

#define MAX_FUNC_LEN 256

#define MAX_PATH_STR STRINGIFY(MAXIMUM_PATH)

#ifndef WINDOWS
# define _stricmp strcasecmp
#endif

/* forward decls */
static void lookup_address(const char *dllpath, size_t modoffs);
static void lookup_symbol(const char *dllpath, const char *sym);
static void enumerate_symbols(const char *dllpath, const char *match,
                              bool search, bool searchall);
static void enumerate_lines(const char *dllpath);

/* options */
#define USAGE_PRE "Usage:\n\
Look up addresses for one module:\n\
  %s -e <module> [-f] [-v] -a [<address relative to module base> ...]\n\
Look up addresses for multiple modules:\n\
  %s [-f] [-v] -q <pairs of [module_path;address relative to module base] on stdin>\n\
Look up exact symbols for one module:\n\
  %s -e <module> [-v] [--enum] -s [<symbol1> <symbol2> ...]\n"

#ifdef WINDOWS
# define USAGE_MID \
"Look up symbols matching wildcard patterns (glob-style: *,?) for one module:\n\
  %s -e <module> [-v] --search -s [<symbol1> <symbol2> ...]\n\
Look up private symbols matching wildcard patterns (glob-style: *,?) for one module:\n\
  %s -e <module> [-v] --searchall -s [<symbol1> <symbol2> ...]\n"
#else
# define USAGE_MID "%.0s%.0s"
#endif

#define USAGE_POST \
"List all symbols in a module:\n\
  %s -e <module> [-v] --list\n\
List all source lines in a module:\n\
  %s -e <module> [-v] --lines\n\
Optional parameters:\n\
  -f = show function name\n\
  -v = verbose\n\
  --enum = look up via external enum rather than drsyms-internal enum\n"

#define PRINT_USAGE(mypath) do {\
    printf(USAGE_PRE, mypath, mypath, mypath);\
    printf(USAGE_MID, mypath, mypath);\
    printf(USAGE_POST, mypath, mypath);\
} while (0)

static bool show_func;
static bool verbose;

int
main(int argc, char *argv[])
{
    char *dll = NULL;
    IF_WINDOWS(char dll_local[MAXIMUM_PATH];)
    int i;
    /* module + address per line */
    char line[MAXIMUM_PATH*2];
    size_t modoffs;

    /* options that can be local vars */
    bool addr2sym = false;
    bool addr2sym_multi = false;
    bool sym2addr = false;
    bool enumerate = false;
    bool enumerate_all = false;
    bool search = false;
    bool searchall = false;
    bool enum_lines = false;

    for (i = 1; i < argc; i++) {
        if (_stricmp(argv[i], "-e") == 0) {
            IF_WINDOWS(int res;)
            if (i+1 >= argc) {
                PRINT_USAGE(argv[0]);
                return 1;
            }
            i++;
#ifdef WINDOWS
            /* Handle relative paths */
            res = GetFullPathName(argv[i], BUFFER_SIZE_ELEMENTS(dll_local),
                                  dll_local, NULL);
            NULL_TERMINATE_BUFFER(dll_local);
            dll = dll_local;
#else
            dll = argv[i];
#endif
            if (
#ifdef WINDOWS
                res <= 0 || res >= BUFFER_SIZE_ELEMENTS(dll_local) - 1/*null*/ ||
                _access(dll, 4/*read*/) == -1
#else
                !dr_file_exists(dll)
#endif
                ) {
                printf("ERROR: invalid path %s\n", argv[i]);
                return 1;
            }
        } else if (_stricmp(argv[i], "-f") == 0) {
            show_func = true;
        } else if (_stricmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (_stricmp(argv[i], "-a") == 0 ||
                   _stricmp(argv[i], "-s") == 0) {
            if (i+1 >= argc) {
                PRINT_USAGE(argv[0]);
                return 1;
            }
            if (_stricmp(argv[i], "-a") == 0)
                addr2sym = true;
            else
                sym2addr = true;
            i++;
            /* rest of args read below */
            break;
        } else if (_stricmp(argv[i], "--lines") == 0) {
            enum_lines = true;
        } else if (_stricmp(argv[i], "-q") == 0) {
            addr2sym_multi = true;
        } else if (_stricmp(argv[i], "--enum") == 0) {
            enumerate = true;
        } else if (_stricmp(argv[i], "--list") == 0) {
            enumerate_all = true;
        } else if (_stricmp(argv[i], "--search") == 0) {
            search = true;
        } else if (_stricmp(argv[i], "--searchall") == 0) {
            search = true;
            searchall = true;
        } else {
            PRINT_USAGE(argv[0]);
            return 1;
        }
    }
    if ((!addr2sym_multi && dll == NULL) ||
        (addr2sym_multi && dll != NULL) ||
        (!sym2addr && !addr2sym && !addr2sym_multi && !enumerate_all && !enum_lines)) {
        PRINT_USAGE(argv[0]);
        return 1;
    }

    dr_standalone_init();

    if (drsym_init(IF_WINDOWS_ELSE(NULL, 0)) != DRSYM_SUCCESS) {
        printf("ERROR: unable to initialize symbol library\n");
        return 1;
    }

    if (!addr2sym_multi) {
        if (enum_lines)
            enumerate_lines(dll);
        else if (enumerate_all)
            enumerate_symbols(dll, NULL, search, searchall);
        else {
            /* kind of a hack: assumes i hasn't changed and that -s/-a is last option */
            for (; i < argc; i++) {
                if (addr2sym) {
                    if (sscanf(argv[i], "%x", (uint *)&modoffs) == 1)
                        lookup_address(dll, modoffs);
                    else
                        printf("ERROR: unknown input %s\n", argv[i]);
                } else if (enumerate || search)
                    enumerate_symbols(dll, argv[i], search, searchall);
                else
                    lookup_symbol(dll, argv[i]);
            }
        }
    } else {
        while (!feof(stdin)) {
            char modpath[MAXIMUM_PATH];
            if (fgets(line, sizeof(line), stdin) == NULL ||
                /* when postprocess.pl closes the pipe, fgets is not
                 * returning, so using an alternative eof code
                 */
                strcmp(line, ";exit\n") == 0)
                break;
            /* Ensure we support spaces in paths by using ; to split.
             * Since ; separates PATH, no Windows dll will have ; in its name.
             */
            if (sscanf(line, "%"MAX_PATH_STR"[^;];%x", (char *)&modpath,
                       (uint *)&modoffs) == 2) {
                lookup_address(modpath, modoffs);
                fflush(stdout); /* ensure flush in case piped */
            } else if (verbose)
                printf("Error: unknown input %s\n", line);
        }
    }

    if (drsym_exit() != DRSYM_SUCCESS)
        printf("WARNING: error cleaning up symbol library\n");

    return 0;
}

static void
print_debug_kind(drsym_debug_kind_t kind)
{
    printf("<debug info: type=%s, %s symbols, %s line numbers>\n",
           TEST(DRSYM_ELF_SYMTAB, kind) ? "ELF symtab" :
           (TEST(DRSYM_PECOFF_SYMTAB, kind) ? "PECOFF symtab" :
            (TEST(DRSYM_PDB, kind) ? "PDB" : "no symbols")),
           TEST(DRSYM_SYMBOLS, kind) ? "has" : "NO",
           TEST(DRSYM_LINE_NUMS, kind) ? "has" : "NO");
}

static void
get_and_print_debug_kind(const char *dllpath)
{
    drsym_debug_kind_t kind;
    drsym_error_t symres = drsym_get_module_debug_kind(dllpath, &kind);
    if (symres == DRSYM_SUCCESS)
        print_debug_kind(kind);
}

static void
lookup_address(const char *dllpath, size_t modoffs)
{
    drsym_error_t symres;
    drsym_info_t sym;
    char name[MAX_FUNC_LEN];
    char file[MAXIMUM_PATH];
    sym.struct_size = sizeof(sym);
    sym.name = name;
    sym.name_size = MAX_FUNC_LEN;
    sym.file = file;
    sym.file_size = MAXIMUM_PATH;
    symres = drsym_lookup_address(dllpath, modoffs, &sym, DRSYM_DEMANGLE);
    if (symres == DRSYM_SUCCESS || symres == DRSYM_ERROR_LINE_NOT_AVAILABLE) {
        if (verbose)
            print_debug_kind(sym.debug_kind);
        if (sym.name_available_size >= sym.name_size)
            printf("WARNING: function name longer than max: %s\n", sym.name);
        if (show_func)
            printf("%s+0x%x\n", sym.name, (uint)(modoffs - sym.start_offs));

        if (symres == DRSYM_ERROR_LINE_NOT_AVAILABLE) {
            printf("??:0\n");
        } else {
            printf("%s:%"INT64_FORMAT"u+0x%x\n", sym.file, sym.line,
                   (uint)sym.line_offs);
        }
    } else {
        if (verbose)
            printf("drsym_lookup_address error %d\n", symres);
        else if (show_func)
            printf("?\n");
    }
}

static void
lookup_symbol(const char *dllpath, const char *sym)
{
    size_t modoffs;
    drsym_error_t symres;
    if (verbose)
        get_and_print_debug_kind(dllpath);
    symres = drsym_lookup_symbol(dllpath, sym, &modoffs, DRSYM_DEMANGLE);
    if (symres == DRSYM_SUCCESS || symres == DRSYM_ERROR_LINE_NOT_AVAILABLE) {
        printf("+0x%x\n", (uint)modoffs);
    } else {
        if (verbose)
            printf("drsym error %d looking up \"%s\" in \"%s\"\n", symres, sym, dllpath);
        else
            printf("??\n");
    }
}

static bool
search_cb(drsym_info_t *info, drsym_error_t status, void *data)
{
    const char *match = (const char *) data;
    if (match == NULL || strcmp(info->name, match) == 0)
        printf("%s +0x%x-0x%x\n", info->name, (uint)info->start_offs,
               (uint)info->end_offs);
    return true; /* keep iterating */
}

static void
enumerate_symbols(const char *dllpath, const char *match, bool search, bool searchall)
{
    drsym_error_t symres;
    if (verbose)
        get_and_print_debug_kind(dllpath);
#ifdef WINDOWS
    if (search)
        symres = drsym_search_symbols_ex(dllpath, match, searchall, search_cb,
                                         sizeof(drsym_info_t), NULL);
    else {
#endif
        symres = drsym_enumerate_symbols_ex(dllpath, search_cb, sizeof(drsym_info_t),
                                            (void *)match, DRSYM_DEMANGLE);
#ifdef WINDOWS
    }
#endif
    if (symres != DRSYM_SUCCESS && verbose)
        printf("search/enum error %d\n", symres);
}

static bool
enum_line_cb(drsym_line_info_t *info, void *data)
{
    printf("cu=\"%s\", file=\"%s\" line=" INT64_FORMAT_STRING ", addr="PIFX"\n",
           (info->cu_name == NULL) ? "<null>" : info->cu_name,
           (info->file == NULL) ? "<null>" : info->file,
           info->line, info->line_addr);
    return true;
}

static void
enumerate_lines(const char *dllpath)
{
    drsym_error_t symres;
    if (verbose)
        get_and_print_debug_kind(dllpath);
    symres = drsym_enumerate_lines(dllpath, enum_line_cb, (void *) dllpath);
    if (symres != DRSYM_SUCCESS && verbose)
        printf("line enum error %d\n", symres);
}

