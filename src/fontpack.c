/* Copyright 2021 Jaakko Keränen <jaakko.keranen@iki.fi>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#include "fontpack.h"

#include <the_Foundation/archive.h>
#include <the_Foundation/array.h>
#include <the_Foundation/file.h>
#include <the_Foundation/path.h>
#include <the_Foundation/ptrarray.h>
#include <the_Foundation/string.h>
#include <the_Foundation/toml.h>

#include "embedded.h"

/* TODO: Clean up and/or reorder this file, it's a bit unorganized. */

float scale_FontSize(enum iFontSize size) {
    static const float sizes[max_FontSize] = {
        1.000, /* UI sizes */
        1.125,
        1.333,
        1.666,
        0.800,
        0.900,
        1.000, /* document sizes */
        1.200,
        1.333,
        1.666,
        2.000,
        0.568,
        0.710, /* calibration: fits the Lagrange title screen with Normal line width */
    };
    if (size < 0 || size >= max_FontSize) {
        return 1.0f;
    }
    return sizes[size];
}

iDeclareType(Fonts)    
    
struct Impl_Fonts {
    iString   userDir;
    iPtrArray packs;
    iPtrArray files;
    iPtrArray specOrder; /* specs sorted by priority */
};

static iFonts fonts_;

static void unloadFiles_Fonts_(iFonts *d) {
    /* TODO: Mark all files in font packs as not resident. */    
    iForEach(PtrArray, i, &d->files) {
        delete_FontFile(i.ptr);
    }
    clear_PtrArray(&d->files);
}

static iFontFile *findFile_Fonts_(iFonts *d, const iString *id) {
    iForEach(PtrArray, i, &d->files) {
        iFontFile *ff = i.ptr;
        if (equal_String(&ff->id, id)) {
            return ff;
        }
    }
    return NULL;
}

/*----------------------------------------------------------------------------------------------*/

iDefineTypeConstruction(FontFile)

void init_FontFile(iFontFile *d) {
    init_String(&d->id);
    d->style = regular_FontStyle;
    init_Block(&d->sourceData, 0);
    iZap(d->stbInfo);
#if defined (LAGRANGE_ENABLE_HARFBUZZ)
    d->hbBlob = NULL;
    d->hbFace = NULL;
    d->hbFont = NULL;
#endif
}

static void load_FontFile_(iFontFile *d, const iBlock *data) {
    set_Block(&d->sourceData, data);
    stbtt_InitFont(&d->stbInfo, constData_Block(&d->sourceData), 0);
    /* Basic metrics. */
    stbtt_GetFontVMetrics(&d->stbInfo, &d->ascent, &d->descent, NULL);
    stbtt_GetCodepointHMetrics(&d->stbInfo, 'M', &d->emAdvance, NULL);
#if defined(LAGRANGE_ENABLE_HARFBUZZ)
    /* HarfBuzz will read the font data. */
    d->hbBlob = hb_blob_create(constData_Block(&d->sourceData), size_Block(&d->sourceData),
                               HB_MEMORY_MODE_READONLY, NULL, NULL);
    d->hbFace = hb_face_create(d->hbBlob, 0);
    d->hbFont = hb_font_create(d->hbFace);
#endif    
}

static void unload_FontFile_(iFontFile *d) {
#if defined(LAGRANGE_ENABLE_HARFBUZZ)
    /* HarfBuzz objects. */
    hb_font_destroy(d->hbFont);
    hb_face_destroy(d->hbFace);
    hb_blob_destroy(d->hbBlob);
    d->hbFont = NULL;
    d->hbFace = NULL;
    d->hbBlob = NULL;
#endif    
    clear_Block(&d->sourceData);
    iZap(d->stbInfo);
}

void deinit_FontFile(iFontFile *d) {
    unload_FontFile_(d);
    deinit_Block(&d->sourceData);
    deinit_String(&d->id);
}

float scaleForPixelHeight_FontFile(const iFontFile *d, int pixelHeight) {
    return stbtt_ScaleForPixelHeight(&d->stbInfo, pixelHeight);
}

uint8_t *rasterizeGlyph_FontFile(const iFontFile *d, float xScale, float yScale, float xShift,
                                 uint32_t glyphIndex, int *w, int *h) {
    return stbtt_GetGlyphBitmapSubpixel(
        &d->stbInfo, xScale, yScale, xShift, 0.0f, glyphIndex, w, h, 0, 0);
}

void measureGlyph_FontFile(const iFontFile *d, uint32_t glyphIndex,
                           float xScale, float yScale, float xShift,
                           int *x0, int *y0, int *x1, int *y1) {
    stbtt_GetGlyphBitmapBoxSubpixel(
        &d->stbInfo, glyphIndex, xScale, yScale, xShift, 0.0f, x0, y0, x1, y1);
}

/*----------------------------------------------------------------------------------------------*/


iDefineTypeConstruction(FontSpec)
    
void init_FontSpec(iFontSpec *d) {
    init_String(&d->id);
    init_String(&d->name);
    d->flags      = 0;
    d->priority   = 0;
    for (int i = 0; i < 2; ++i) {
        d->heightScale[i]     = 1.0f;
        d->glyphScale[i]      = 1.0f;
        d->vertOffsetScale[i] = 1.0f;
    }
    iZap(d->styles);
}

void deinit_FontSpec(iFontSpec *d) {
    deinit_String(&d->name);
    deinit_String(&d->id);
}

/*----------------------------------------------------------------------------------------------*/

iDeclareType(FontPack)
iDeclareTypeConstruction(FontPack)

struct Impl_FontPack {
    const iArchive *archive; /* opened ZIP archive */
    iArray          fonts;   /* array of FontSpecs */
    iString *       loadPath;
    iFontSpec *     loadSpec;
};

void init_FontPack(iFontPack *d) {
    d->archive = NULL;
    init_Array(&d->fonts, sizeof(iFontSpec));
    d->loadSpec = NULL;
    d->loadPath = NULL;
}

void deinit_FontPack(iFontPack *d) {
    iForEach(Array, i, &d->fonts) {
        deinit_FontSpec(i.value);
    }
    deinit_Array(&d->fonts);
}

iDefineTypeConstruction(FontPack)

void handleIniTable_FontPack_(void *context, const iString *table, iBool isStart) {
    iFontPack *d = context;
    if (isStart) {
        iAssert(!d->loadSpec);
        d->loadSpec = new_FontSpec();
        set_String(&d->loadSpec->id, table);
    }
    else {
        /* Set fallback font files. */ {
            const iFontFile **styles = d->loadSpec->styles;
            if (!styles[regular_FontStyle]) {
                fprintf(stderr, "[FontPack] \"%s\" missing a regular style font file\n",
                        cstr_String(table));
                delete_FontSpec(d->loadSpec);
                d->loadSpec = NULL;
                return;
            }
            if (!styles[semiBold_FontStyle]) {
                styles[semiBold_FontStyle] = styles[bold_FontStyle];
            }
            for (size_t s = 0; s < max_FontStyle; s++) {
                if (!styles[s]) {
                    styles[s] = styles[regular_FontStyle];
                }
            }
        }
        pushBack_Array(&d->fonts, d->loadSpec);
        d->loadSpec = NULL;
    }   
}

static iBlock *readFile_FontPack_(const iFontPack *d, const iString *path) {
    iBlock *data = NULL;
    if (d->archive) {
        /* Loading from a ZIP archive. */
        data = copy_Block(data_Archive(d->archive, path));
    }
    else if (d->loadPath) {
        /* Loading from a regular file. */
        iFile *srcFile = new_File(collect_String(concat_Path(d->loadPath, path)));
        if (open_File(srcFile, readOnly_FileMode)) {
            data = readAll_File(srcFile);
        }
        iRelease(srcFile);
        return data;
    }
    return data;
}

void handleIniKeyValue_FontPack_(void *context, const iString *table, const iString *key,
                                 const iTomlValue *value) {
    iFontPack *d = context;
    if (!d->loadSpec) return;
    iUnused(table);
    if (!cmp_String(key, "name") && value->type == string_TomlType) {
        set_String(&d->loadSpec->name, value->value.string);        
    }
    else if (!cmp_String(key, "priority") && value->type == int64_TomlType) {
        d->loadSpec->priority = (int) value->value.int64;        
    }
    else if (!cmp_String(key, "height")) {
        d->loadSpec->heightScale[0] = d->loadSpec->heightScale[1] = (float) number_TomlValue(value);
    }
    else if (!cmp_String(key, "glyphscale")) {
        d->loadSpec->glyphScale[0] = d->loadSpec->glyphScale[1] = (float) number_TomlValue(value);
    }
    else if (!cmp_String(key, "voffset")) {
        d->loadSpec->vertOffsetScale[0] = d->loadSpec->vertOffsetScale[1] =
            (float) number_TomlValue(value);
    }
    else if (startsWith_String(key, "ui.") || startsWith_String(key, "doc.")) {
        const int scope = startsWith_String(key, "ui.") ? 0 : 1;        
        if (endsWith_String(key, ".height")) {
            d->loadSpec->heightScale[scope] = (float) number_TomlValue(value);
        }
        if (endsWith_String(key, ".glyphscale")) {
            d->loadSpec->glyphScale[scope] = (float) number_TomlValue(value);
        }
        else if (endsWith_String(key, ".voffset")) {
            d->loadSpec->vertOffsetScale[scope] = (float) number_TomlValue(value);
        }
    }
    else if (!cmp_String(key, "override") && value->type == boolean_TomlType) {
        iChangeFlags(d->loadSpec->flags, override_FontSpecFlag, value->value.boolean);
    }
    else if (!cmp_String(key, "monospace") && value->type == boolean_TomlType) {
        iChangeFlags(d->loadSpec->flags, monospace_FontSpecFlag, value->value.boolean);
    }
    else if (!cmp_String(key, "auxiliary") && value->type == boolean_TomlType) {
        iChangeFlags(d->loadSpec->flags, auxiliary_FontSpecFlag, value->value.boolean);
    }
    else if (!cmp_String(key, "arabic") && value->type == boolean_TomlType) {
        iChangeFlags(d->loadSpec->flags, arabic_FontSpecFlag, value->value.boolean);
    }
    else if (!cmp_String(key, "tweaks")) {
        iChangeFlags(d->loadSpec->flags, fixNunitoKerning_FontSpecFlag,
                     ((int) number_TomlValue(value)) & 1);
    }
    else if (value->type == string_TomlType) {
        const char *styles[max_FontStyle] = { "regular", "italic", "light", "semibold", "bold" };
        iForIndices(i, styles) {
            if (!cmp_String(key, styles[i]) && !d->loadSpec->styles[i]) {
                iFontFile *ff = NULL;
                iString *fontFileId = concat_Path(d->loadPath, value->value.string);
                if (!(ff = findFile_Fonts_(&fonts_, fontFileId))) {
                    iBlock *data = readFile_FontPack_(d, value->value.string);
                    if (data) {
                        ff = new_FontFile();
                        set_String(&ff->id, fontFileId);
                        load_FontFile_(ff, data);
                        pushBack_PtrArray(&fonts_.files, ff); /* centralized ownership */
                        delete_Block(data);
//                        printf("[FontPack] loaded file: %s\n", cstr_String(fontFileId));
                    }
                }
                d->loadSpec->styles[i] = ff;
                delete_String(fontFileId);
                break;
            }
        }
    }
}

static iBool load_FontPack_(iFontPack *d, const iString *ini) {
    iBeginCollect();
    iBool ok = iFalse;
//    iFile *f = iClob(new_File(iniPath));
    //if (open_File(f, text_FileMode | readOnly_FileMode)) {
//        d->loadPath = collect_String(newRange_String(dirName_Path(iniPath)));
//    iString *src = collect_String(readString_File(f));
    iTomlParser *toml = collect_TomlParser(new_TomlParser());
    setHandlers_TomlParser(toml, handleIniTable_FontPack_, handleIniKeyValue_FontPack_, d);
    if (parse_TomlParser(toml, ini)) {
        ok = iTrue;
    }
    iAssert(d->loadSpec == NULL);
//        d->loadPath = NULL;
//    }
    iEndCollect();
    return ok;
}

#if 0
iBool loadIniFile_FontPack(iFontPack *d, const iString *iniPath) {
    iBeginCollect();
    iBool ok = iFalse;
    iFile *f = iClob(new_File(iniPath));
    if (open_File(f, text_FileMode | readOnly_FileMode)) {
        d->loadPath = collect_String(newRange_String(dirName_Path(iniPath)));
        iString *src = collect_String(readString_File(f));
        
        iTomlParser *ini = collect_TomlParser(new_TomlParser());
        setHandlers_TomlParser(ini, handleIniTable_FontPack_, handleIniKeyValue_FontPack_, d);
        if (!parse_TomlParser(ini, src)) {
            fprintf(stderr, "[FontPack] error parsing %s\n", cstr_String(iniPath));
        }
        iAssert(d->loadSpec == NULL);
        d->loadPath = NULL;
        ok = iTrue;
    }
    iEndCollect();
    return ok;
}
#endif

iBool loadArchive_FontPack(iFontPack *d, const iArchive *zip) {
    d->archive = zip;
    iBool ok = iFalse;
    const iBlock *iniData = dataCStr_Archive(zip, "fontpack.ini");
    if (iniData) {
        iString ini;
        initBlock_String(&ini, iniData);
        if (load_FontPack_(d, &ini)) {
            ok = iTrue;
        }
        deinit_String(&ini);
    }
    return ok;
}

/*----------------------------------------------------------------------------------------------*/

static void unloadFonts_Fonts_(iFonts *d) {
    iForEach(PtrArray, i, &d->packs) {
        iFontPack *pack = i.ptr;
        delete_FontPack(pack);
    }
    clear_PtrArray(&d->packs);
}

static int cmpPriority_FontSpecPtr_(const void *a, const void *b) {
    const iFontSpec **p1 = (const iFontSpec **) a, **p2 = (const iFontSpec **) b;
    return -iCmp((*p1)->priority, (*p2)->priority); /* highest priority first */
}

static int cmpName_FontSpecPtr_(const void *a, const void *b) {
    const iFontSpec **p1 = (const iFontSpec **) a, **p2 = (const iFontSpec **) b;
    return cmpStringCase_String(&(*p1)->name, &(*p2)->name);
}

static void sortSpecs_Fonts_(iFonts *d) {
    clear_PtrArray(&d->specOrder);
    iConstForEach(PtrArray, p, &d->packs) {
        const iFontPack *pack = p.ptr;
        iConstForEach(Array, i, &pack->fonts) {
            pushBack_PtrArray(&d->specOrder, i.value);
        }
    }
    sort_Array(&d->specOrder, cmpPriority_FontSpecPtr_);
}

void init_Fonts(const char *userDir) {
    iFonts *d = &fonts_;
    initCStr_String(&d->userDir, userDir);
    init_PtrArray(&d->packs);
    init_PtrArray(&d->files);
    init_PtrArray(&d->specOrder);
    /* Load the required fonts. */ {
        iFontPack *pack = new_FontPack();
        iArchive *defaultPack = new_Archive();
        openData_Archive(defaultPack, &fontpackDefault_Embedded);
        loadArchive_FontPack(pack, defaultPack);
        iRelease(defaultPack);
        pushBack_PtrArray(&d->packs, pack);
    }
    /* TODO: find and load .fontpack files in known locations */
    sortSpecs_Fonts_(d);
}

void deinit_Fonts(void) {
    iFonts *d = &fonts_;
    unloadFonts_Fonts_(d);
    unloadFiles_Fonts_(d);
    deinit_PtrArray(&d->specOrder);
    deinit_PtrArray(&d->packs);
    deinit_PtrArray(&d->files);
    deinit_String(&d->userDir);
}

const iFontSpec *findSpec_Fonts(const char *fontId) {
    iFonts *d = &fonts_;
    iConstForEach(PtrArray, i, &d->specOrder) {
        const iFontSpec *spec = i.ptr;
        if (!cmp_String(&spec->id, fontId)) {
            return spec;
        }
    }
    return NULL;
}

const iPtrArray *listSpecs_Fonts(iBool (*filterFunc)(const iFontSpec *)) {
    iFonts *d = &fonts_;
    iPtrArray *list = collectNew_PtrArray();
    iConstForEach(PtrArray, i, &d->specOrder) {
        if (filterFunc == NULL || filterFunc(i.ptr)) {
            pushBack_PtrArray(list, i.ptr);
        }
    }
    sort_Array(list, cmpName_FontSpecPtr_);
    return list;
}

const iPtrArray *listSpecsByPriority_Fonts(void) {
    return &fonts_.specOrder;
}
