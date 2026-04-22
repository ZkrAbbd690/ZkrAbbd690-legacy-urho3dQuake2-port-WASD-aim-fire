
#include <cstring>
#include <stdio.h>
#include "Material.h"
#include "TBEModelLoad.h"
#include "TBESurface.h"
#include "TBEAliasModel.h"


extern refimport_t ri;

static byte	mod_novis[MAX_MAP_LEAFS/8];
#define	MAX_MOD_KNOWN 512
static model_t	mod_known[MAX_MOD_KNOWN];
static int mod_numknown;

model_t	*r_worldmodel;
model_t	*currentmodel;
model_t	*loadmodel;

static int		modfilelen;

// the inline * models from the current map are kept seperate
static model_t	mod_inline[MAX_MOD_KNOWN];

static int		registration_sequence;

void Mod_LoadAliasModel (model_t *mod, void *buffer);
void Mod_LoadSpriteModel (model_t *mod, void *buffer);
void Mod_LoadBrushModel (model_t *mod, void *buffer);
void Mod_Free (model_t *mod);


/*
===============
Mod_PointInLeaf
===============
*/
mleaf_t *Mod_PointInLeaf (vec3_t p, model_t *model)
{
    mnode_t		*node;
    float		d;
    cplane_t	*plane;

    if (!model || !model->nodes)
        ri.Sys_Error (ERR_DROP, "Mod_PointInLeaf: bad model");

    node = model->nodes;
    while (1)
    {
        if (node->contents != -1)
            return (mleaf_t *)node;
        plane = node->plane;
        d = DotProduct (p,plane->normal) - plane->dist;
        if (d > 0)
            node = node->children[0];
        else
            node = node->children[1];
    }

    return NULL;	// never reached
}


/*
===================
Mod_DecompressVis
===================
*/
byte *Mod_DecompressVis (byte *in, model_t *model)
{
    static byte	decompressed[MAX_MAP_LEAFS/8];
    int		c;
    byte	*out;
    int		row;

    row = (model->vis->numclusters+7)>>3;
    out = decompressed;

    if (!in)
    {	// no vis info, so make all visible
        while (row)
        {
            *out++ = 0xff;
            row--;
        }
        return decompressed;
    }

    do
    {
        if (*in)
        {
            *out++ = *in++;
            continue;
        }

        c = in[1];
        in += 2;
        while (c)
        {
            *out++ = 0;
            c--;
        }
    } while (out - decompressed < row);

    return decompressed;
}

/*
==============
Mod_ClusterPVS
==============
*/
byte *Mod_ClusterPVS (int cluster, model_t *model)
{
    if (cluster == -1 || !model->vis)
        return mod_novis;
    return Mod_DecompressVis ( (byte *)model->vis + model->vis->bitofs[cluster][DVIS_PVS],
        model);
}


//===============================================================================

/*
================
Mod_Modellist_f
================
*/
void Mod_Modellist_f (void)
{
    int		i;
    model_t	*mod;
    int		total;

    total = 0;
    ri.Con_Printf (PRINT_ALL,"Loaded models:\n");
    for (i=0, mod=mod_known ; i < mod_numknown ; i++, mod++)
    {
        if (!mod->name[0])
            continue;
        ri.Con_Printf (PRINT_ALL, "%8i : %s\n",mod->extradatasize, mod->name);
        total += mod->extradatasize;
    }
    ri.Con_Printf (PRINT_ALL, "Total resident: %i\n", total);
}

/*
===============
Mod_Init
===============
*/
void Mod_Init (void)
{
    memset (mod_novis, 0xff, sizeof(mod_novis));
}



/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
model_t *Mod_ForName (char *name, qboolean crash)
{
    model_t	*mod;
    unsigned *buf;
    int		i;

    if (!name[0])
        ri.Sys_Error (ERR_DROP, "Mod_ForName: NULL name");

    //
    // inline models are grabbed only from worldmodel
    //
    if (name[0] == '*')
    {
        i = atoi(name+1);
        if (i < 1 || !r_worldmodel || i >= r_worldmodel->numsubmodels)
            ri.Sys_Error (ERR_DROP, "bad inline model number");
        return &mod_inline[i];
    }

    //
    // search the currently loaded models
    //
    for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
    {
        if (!mod->name[0])
            continue;
        if (!strcmp (mod->name, name) )
            return mod;
    }

    //
    // find a free model slot spot
    //
    for (i=0 , mod=mod_known ; i<mod_numknown ; i++, mod++)
    {
        if (!mod->name[0])
            break;	// free spot
    }
    if (i == mod_numknown)
    {
        if (mod_numknown == MAX_MOD_KNOWN)
            ri.Sys_Error (ERR_DROP, "mod_numknown == MAX_MOD_KNOWN");
        mod_numknown++;
    }
    strcpy (mod->name, name);

    //
    // load the file
    //
    modfilelen = ri.FS_LoadFile (mod->name, (void**) &buf);
    if (!buf)
    {
        if (crash)
            ri.Sys_Error (ERR_DROP, "Mod_NumForName: %s not found", mod->name);
        memset (mod->name, 0, sizeof(mod->name));
        return NULL;
    }

    loadmodel = mod;

    //
    // fill it in
    //


    // call the apropriate loader

    switch (LittleLong(*(unsigned *)buf))
    {
    case IDALIASHEADER:
        loadmodel->extradata = Hunk_Begin (0x200000);
        Mod_LoadAliasModel (mod, buf);
        break;

    case IDSPRITEHEADER:
        loadmodel->extradata = Hunk_Begin (0x10000);
        Mod_LoadSpriteModel (mod, buf);
        break;

    case IDBSPHEADER:
        loadmodel->extradata = Hunk_Begin (0x1000000);
        Mod_LoadBrushModel (mod, buf);
        break;

    default:
        ri.Sys_Error (ERR_DROP,"Mod_NumForName: unknown fileid for %s", mod->name);
        break;
    }

    loadmodel->extradatasize = Hunk_End ();

    ri.FS_FreeFile (buf);

    return mod;
}

/*
===============================================================================

                    BRUSHMODEL LOADING

===============================================================================
*/

byte	*mod_base;


/*
=================
Mod_LoadLighting
=================
*/
void Mod_LoadLighting (lump_t *l)
{
    if (!l->filelen)
    {
        loadmodel->lightdata = NULL;
        return;
    }
    loadmodel->lightdata = (byte*) Hunk_Alloc ( l->filelen);
    memcpy (loadmodel->lightdata, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadVisibility
=================
*/
void Mod_LoadVisibility (lump_t *l)
{
    int		i;

    if (!l->filelen)
    {
        loadmodel->vis = NULL;
        return;
    }
    loadmodel->vis = (dvis_t*) Hunk_Alloc ( l->filelen);
    memcpy (loadmodel->vis, mod_base + l->fileofs, l->filelen);

    loadmodel->vis->numclusters = LittleLong (loadmodel->vis->numclusters);
    for (i=0 ; i<loadmodel->vis->numclusters ; i++)
    {
        loadmodel->vis->bitofs[i][0] = LittleLong (loadmodel->vis->bitofs[i][0]);
        loadmodel->vis->bitofs[i][1] = LittleLong (loadmodel->vis->bitofs[i][1]);
    }
}


/*
=================
Mod_LoadVertexes
=================
*/
void Mod_LoadVertexes (lump_t *l)
{
    dvertex_t	*in;
    mvertex_t	*out;
    int			i, count;

    in = (dvertex_t	*)((void *)(mod_base + l->fileofs));
    if (l->filelen % sizeof(*in))
        ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = (mvertex_t *) Hunk_Alloc ( count*sizeof(*out));

    loadmodel->vertexes = out;
    loadmodel->numvertexes = count;

    for ( i=0 ; i<count ; i++, in++, out++)
    {
        out->position[0] = LittleFloat (in->point[0]);
        out->position[1] = LittleFloat (in->point[1]);
        out->position[2] = LittleFloat (in->point[2]);
    }
}

/*
=================
RadiusFromBounds
=================
*/
float RadiusFromBounds (vec3_t mins, vec3_t maxs)
{
    int		i;
    vec3_t	corner;

    for (i=0 ; i<3 ; i++)
    {
        corner[i] = fabs(mins[i]) > fabs(maxs[i]) ? fabs(mins[i]) : fabs(maxs[i]);
    }

    return VectorLength (corner);
}


/*
=================
Mod_LoadSubmodels
=================
*/
void Mod_LoadSubmodels (lump_t *l)
{
    dmodel_t	*in;
    model_t	*out;
    int			i, j, count;

    in = (dmodel_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
        ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = (model_t*) Hunk_Alloc ( count * sizeof(model_t));
    
    loadmodel->submodels = out;
    loadmodel->numsubmodels = count;
    
    for ( i=0 ; i<count ; i++, in++, out++)
    {
        for (j=0 ; j<3 ; j++)
        {	// spread the mins / maxs by a pixel
            out->mins[j] = LittleFloat (in->mins[j]) - 1;
            out->maxs[j] = LittleFloat (in->maxs[j]) + 1;
            out->origin[j] = LittleFloat (in->origin[j]);
        }
        out->radius = RadiusFromBounds (out->mins, out->maxs);
        out->headnode = LittleLong (in->headnode);
        out->firstface = LittleLong (in->firstface);
        out->numfaces = LittleLong (in->numfaces);
    }
}

/*
=================
Mod_LoadEdges
=================
*/
void Mod_LoadEdges (lump_t *l)
{
    dedge_t *in;
    medge_t *out;
    int 	i, count;

    in = (dedge_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
        ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = (medge_t*) Hunk_Alloc ( (count + 1) * sizeof(*out));

    loadmodel->edges = out;
    loadmodel->numedges = count;

    for ( i=0 ; i<count ; i++, in++, out++)
    {
        out->v[0] = (unsigned short)LittleShort(in->v[0]);
        out->v[1] = (unsigned short)LittleShort(in->v[1]);
    }
}

/*
=================
Mod_LoadTexinfo
=================
*/
void Mod_LoadTexinfo (lump_t *l)
{
    texinfo_t *in;
    mtexinfo_t *out, *step;
    int 	i, j, count;
    char	name[MAX_QPATH];
    int		next;

    // CRITICAL DEBUG: Use stderr instead of ri.Con_Printf (not initialized yet at this stage)
    fprintf(stderr, "FATAL-DEBUG: Mod_LoadTexinfo ENTRY, l=%p\n", (void*)l);

    if (!l) {
        fprintf(stderr, "FATAL-DEBUG: l is NULL - aborting\n");
        return;
    }

    fprintf(stderr, "FATAL-DEBUG: l->fileofs=%d, l->filelen=%d\n", l->fileofs, l->filelen);
    fprintf(stderr, "FATAL-DEBUG: mod_base=%p\n", (void*)mod_base);

    if (!mod_base) {
        fprintf(stderr, "FATAL-DEBUG: mod_base is NULL - aborting\n");
        return;
    }

    count = l->filelen / sizeof(texinfo_t);
    fprintf(stderr, "FATAL-DEBUG: sizeof(texinfo_t)=%zu, count=%d\n", sizeof(texinfo_t), count);

    if (count < 0 || count > 10000) {
        fprintf(stderr, "FATAL-DEBUG: insane count=%d - aborting\n", count);
        return;
    }

    in = (texinfo_t *)((void *)(mod_base + l->fileofs));
    fprintf(stderr, "FATAL-DEBUG: in pointer=%p\n", (void*)in);

    if (!in) {
        fprintf(stderr, "FATAL-DEBUG: in pointer is NULL - aborting\n");
        return;
    }

    fprintf(stderr, "FATAL-DEBUG: About to Hunk_Alloc\n");
    if (l->filelen % sizeof(*in))
        ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
    count = l->filelen / sizeof(*in);

    // DEBUG: Pre-loop diagnostics
    ri.Con_Printf(PRINT_ALL, "DEBUG: Mod_LoadTexinfo started, count=%d\n", count);
    ri.Con_Printf(PRINT_ALL, "DEBUG: sizeof(texinfo_t) = %d bytes (should be 76 for 32-bit compatibility)\n", (int)sizeof(texinfo_t));
    ri.Con_Printf(PRINT_ALL, "DEBUG: l->filelen = %d\n", l->filelen);
    ri.Con_Printf(PRINT_ALL, "DEBUG: Calculated count = %d (should match)\n", count);
    if (mod_base && l->fileofs >= 0) {
        ri.Con_Printf(PRINT_ALL, "DEBUG: First 4 bytes of BSP data: %02x %02x %02x %02x (floating point vector data)\n",
            ((unsigned char*)mod_base)[l->fileofs],
            ((unsigned char*)mod_base)[l->fileofs + 1],
            ((unsigned char*)mod_base)[l->fileofs + 2],
            ((unsigned char*)mod_base)[l->fileofs + 3]);
    }

    size_t alloc_size = count * sizeof(*out);
    fprintf(stderr, "FATAL-DEBUG: alloc_size=%zu, sizeof(mtexinfo_t)=%zu\n", alloc_size, sizeof(mtexinfo_t));

    out = (mtexinfo_t *)Hunk_Alloc(alloc_size);
    fprintf(stderr, "FATAL-DEBUG: Hunk_Alloc returned %p\n", (void*)out);

    if (!out) {
        fprintf(stderr, "FATAL-DEBUG: Hunk_Alloc returned NULL - aborting\n");
        return;
    }

    fprintf(stderr, "FATAL-DEBUG: About to enter loop, step=out\n");
    step = out;
    fprintf(stderr, "FATAL-DEBUG: step=%p\n", (void*)step);

    loadmodel->texinfo = out;
    loadmodel->numtexinfo = count;

    fprintf(stderr, "FATAL-DEBUG: About to enter FOR LOOP, count=%d\n", count);

    for (i=0; i<count; i++, in++, out++)
    {
        fprintf(stderr, "FATAL-DEBUG: LOOP i=%d, in=%p, out=%p\n", i, (void*)in, (void*)out);

        if (!in) {
            fprintf(stderr, "FATAL-DEBUG: in is NULL at i=%d - aborting\n", i);
            return;
        }
        if (!out) {
            fprintf(stderr, "FATAL-DEBUG: out is NULL at i=%d - aborting\n", i);
            return;
        }

        // Copy vecs using memcpy (safe for unaligned data)
        fprintf(stderr, "FATAL-DEBUG: About to memcpy vecs from in=%p to out=%p\n", (void*)in->vecs, (void*)out->vecs);
        memcpy(out->vecs, in->vecs, sizeof(out->vecs));
        fprintf(stderr, "FATAL-DEBUG: memcpy vecs SUCCESS\n");

        // Copy flags
        fprintf(stderr, "FATAL-DEBUG: About to copy flags, in->flags=%d\n", in->flags);
        out->flags = in->flags;
        fprintf(stderr, "FATAL-DEBUG: flags copied, out->flags=%d\n", out->flags);

        // numframes - source is in->value (from BSP), dest is out->numframes
        fprintf(stderr, "FATAL-DEBUG: About to copy numframes from in->value=%d\n", in->value);
        out->numframes = in->value;  // value from BSP becomes numframes
        fprintf(stderr, "FATAL-DEBUG: numframes copied, out->numframes=%d\n", out->numframes);

        // next pointer - for animation chain
        fprintf(stderr, "FATAL-DEBUG: About to set next pointer\n");
        if (i < count - 1)
            out->next = step + 1;
        else
            out->next = NULL;
        fprintf(stderr, "FATAL-DEBUG: next set to %p\n", (void*)out->next);
        step++;

        // Process texture
        fprintf(stderr, "FATAL-DEBUG: About to access in->texture string\n");
        fprintf(stderr, "FATAL-DEBUG: in->texture at %p, first 4 chars: %.4s\n", (void*)in->texture, in->texture);

        char name[MAX_QPATH];
        Com_sprintf(name, sizeof(name), "textures/%s.wal", in->texture);
        fprintf(stderr, "FATAL-DEBUG: Com_sprintf result: %s\n", name);

        fprintf(stderr, "FATAL-DEBUG: About to call GL_FindImage\n");
        out->image = GL_FindImage(name, it_wall);
        fprintf(stderr, "FATAL-DEBUG: GL_FindImage returned %p\n", (void*)out->image);

        fprintf(stderr, "FATAL-DEBUG: LOOP i=%d COMPLETED SUCCESSFULLY\n", i);
    }

    fprintf(stderr, "FATAL-DEBUG: ALL LOOPS COMPLETED, processed %d texinfos\n", count);

    // count animation frames
    for (i=0 ; i<count ; i++)
    {
        out = &loadmodel->texinfo[i];
        out->numframes = 1;
        for (step = out->next ; step && step != out ; step=step->next)
            out->numframes++;
    }

    fprintf(stderr, "DEBUG-SEQ: Mod_LoadTexinfo loop done, about to return\n");
}

/*
================
CalcSurfaceExtents

Fills in s->texturemins[] and s->extents[]
================
*/
void CalcSurfaceExtents (msurface_t *s)
{
    float	mins[2], maxs[2], val;
    int		i,j, e;
    mvertex_t	*v;
    mtexinfo_t	*tex;
    int		bmins[2], bmaxs[2];

    mins[0] = mins[1] = 999999;
    maxs[0] = maxs[1] = -99999;

    tex = s->texinfo;

    for (i=0 ; i<s->numedges ; i++)
    {
        e = loadmodel->surfedges[s->firstedge+i];
        if (e >= 0)
            v = &loadmodel->vertexes[loadmodel->edges[e].v[0]];
        else
            v = &loadmodel->vertexes[loadmodel->edges[-e].v[1]];

        for (j=0 ; j<2 ; j++)
        {
            val = v->position[0] * tex->vecs[j][0] +
                v->position[1] * tex->vecs[j][1] +
                v->position[2] * tex->vecs[j][2] +
                tex->vecs[j][3];
            if (val < mins[j])
                mins[j] = val;
            if (val > maxs[j])
                maxs[j] = val;
        }
    }

    for (i=0 ; i<2 ; i++)
    {
        bmins[i] = floor(mins[i]/16);
        bmaxs[i] = ceil(maxs[i]/16);

        s->texturemins[i] = bmins[i] * 16;
        s->extents[i] = (bmaxs[i] - bmins[i]) * 16;

//		if ( !(tex->flags & TEX_SPECIAL) && s->extents[i] > 512 /* 256 */ )
//			ri.Sys_Error (ERR_DROP, "Bad surface extents");
    }
}


void GL_BuildPolygonFromSurface(msurface_t *fa);
void GL_CreateSurfaceLightmap (msurface_t *surf);
void GL_EndBuildingLightmaps (void);
void GL_BeginBuildingLightmaps (model_t *m);

/*
=================
Mod_LoadFaces
=================
*/
void Mod_LoadFaces (lump_t *l)
{
    dface_t		*in;
    msurface_t 	*out;
    int			i, count, surfnum;
    int			planenum, side;
    int			ti;

    in = (dface_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
        ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = (msurface_t*) Hunk_Alloc ( count*sizeof(*out));

    loadmodel->surfaces = out;
    loadmodel->numsurfaces = count;

    currentmodel = loadmodel;

    GL_BeginBuildingLightmaps (loadmodel);

    for ( surfnum=0 ; surfnum<count ; surfnum++, in++, out++)
    {
        out->firstedge = LittleLong(in->firstedge);
        out->numedges = LittleShort(in->numedges);
        out->flags = 0;
        out->polys = NULL;
        out->emitted = 0;
        out->material = NULL;
        out->lightmaptexturenum = -1;

        planenum = LittleShort(in->planenum);
        side = LittleShort(in->side);
        if (side)
            out->flags |= SURF_PLANEBACK;

        out->plane = loadmodel->planes + planenum;

        ti = LittleShort (in->texinfo);
        if (ti < 0 || ti >= loadmodel->numtexinfo)
            ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: bad texinfo number");
        out->texinfo = loadmodel->texinfo + ti;

        CalcSurfaceExtents (out);

    // lighting info

        for (i=0 ; i<MAXLIGHTMAPS ; i++)
            out->styles[i] = in->styles[i];
        i = LittleLong(in->lightofs);
        if (i == -1)
            out->samples = NULL;
        else
            out->samples = loadmodel->lightdata + i;

    // set the drawing flags

        if (out->texinfo->flags & SURF_WARP)
        {
            out->flags |= SURF_DRAWTURB;
            for (i=0 ; i<2 ; i++)
            {
                out->extents[i] = 16384;
                out->texturemins[i] = -8192;
            }

            GL_SubdivideSurface (out);	// cut up polygon for warps
        }

        // create lightmaps and polygons
        if ( !(out->texinfo->flags & (SURF_SKY|SURF_TRANS33|SURF_TRANS66|SURF_WARP) ) )
            GL_CreateSurfaceLightmap (out);

        if (! (out->texinfo->flags & SURF_WARP) )
            GL_BuildPolygonFromSurface(out);

    }

    GL_EndBuildingLightmaps ();
}


/*
=================
Mod_SetParent
=================
*/
void Mod_SetParent (mnode_t *node, mnode_t *parent)
{
    node->parent = parent;
    if (node->contents != -1)
        return;
    Mod_SetParent (node->children[0], node);
    Mod_SetParent (node->children[1], node);
}

/*
=================
Mod_LoadNodes
=================
*/
void Mod_LoadNodes (lump_t *l)
{
    int			i, j, count, p;
    dnode_t		*in;
    mnode_t 	*out;

    in = (dnode_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
        ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = (mnode_t*) Hunk_Alloc ( count*sizeof(*out));

    loadmodel->nodes = out;
    loadmodel->numnodes = count;

    for ( i=0 ; i<count ; i++, in++, out++)
    {
        for (j=0 ; j<3 ; j++)
        {
            out->minmaxs[j] = LittleShort (in->mins[j]);
            out->minmaxs[3+j] = LittleShort (in->maxs[j]);
        }

        p = LittleLong(in->planenum);
        out->plane = loadmodel->planes + p;

        out->firstsurface = LittleShort (in->firstface);
        out->numsurfaces = LittleShort (in->numfaces);
        out->contents = -1;	// differentiate from leafs

        for (j=0 ; j<2 ; j++)
        {
            p = LittleLong (in->children[j]);
            if (p >= 0)
                out->children[j] = loadmodel->nodes + p;
            else
                out->children[j] = (mnode_t *)(loadmodel->leafs + (-1 - p));
        }
    }

    Mod_SetParent (loadmodel->nodes, NULL);	// sets nodes and leafs
}

/*
=================
Mod_LoadLeafs
=================
*/
void Mod_LoadLeafs (lump_t *l)
{
    dleaf_t 	*in;
    mleaf_t 	*out;
    int			i, j, k, count, p;
//	glpoly_t	*poly;

    in = (dleaf_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
        ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = (mleaf_t*) Hunk_Alloc ( count*sizeof(*out));

    loadmodel->leafs = out;
    loadmodel->numleafs = count;

    for ( i=0 ; i<count ; i++, in++, out++)
    {
        for (j=0 ; j<3 ; j++)
        {
            out->minmaxs[j] = LittleShort (in->mins[j]);
            out->minmaxs[3+j] = LittleShort (in->maxs[j]);
        }

        p = LittleLong(in->contents);
        out->contents = p;

        out->cluster = LittleShort(in->cluster);
        out->area = LittleShort(in->area);

        out->firstmarksurface = loadmodel->marksurfaces +
            LittleShort(in->firstleafface);
        out->nummarksurfaces = LittleShort(in->numleaffaces);

        for (k=0 ; k < out->nummarksurfaces ; k++)
        {
            out->firstmarksurface[k]->area = out->area;
        }


        // gl underwater warp
#if 0
        if (out->contents & (CONTENTS_WATER|CONTENTS_SLIME|CONTENTS_LAVA|CONTENTS_THINWATER) )
        {
            for (j=0 ; j<out->nummarksurfaces ; j++)
            {
                out->firstmarksurface[j]->flags |= SURF_UNDERWATER;
                for (poly = out->firstmarksurface[j]->polys ; poly ; poly=poly->next)
                    poly->flags |= SURF_UNDERWATER;
            }
        }
#endif
    }
}

/*
=================
Mod_LoadMarksurfaces
=================
*/
void Mod_LoadMarksurfaces (lump_t *l)
{
    int		i, j, count;
    short		*in;
    msurface_t **out;

    in = (short *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
        ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = (msurface_t**) Hunk_Alloc ( count*sizeof(*out));

    loadmodel->marksurfaces = out;
    loadmodel->nummarksurfaces = count;

    for ( i=0 ; i<count ; i++)
    {
        j = LittleShort(in[i]);
        if (j < 0 ||  j >= loadmodel->numsurfaces)
            ri.Sys_Error (ERR_DROP, "Mod_ParseMarksurfaces: bad surface number");
        out[i] = loadmodel->surfaces + j;
    }
}

/*
=================
Mod_LoadSurfedges
=================
*/
void Mod_LoadSurfedges (lump_t *l)
{
    int		i, count;
    int		*in, *out;

    in = (int *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
        ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
    count = l->filelen / sizeof(*in);
    if (count < 1 || count >= MAX_MAP_SURFEDGES)
        ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: bad surfedges count in %s: %i",
        loadmodel->name, count);

    out = (int*) Hunk_Alloc ( count*sizeof(*out));

    loadmodel->surfedges = out;
    loadmodel->numsurfedges = count;

    for ( i=0 ; i<count ; i++)
        out[i] = LittleLong (in[i]);
}


/*
=================
Mod_LoadPlanes
=================
*/
void Mod_LoadPlanes (lump_t *l)
{
    int			i, j;
    cplane_t	*out;
    dplane_t 	*in;
    int			count;
    int			bits;

    in = (dplane_t *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
        ri.Sys_Error (ERR_DROP, "MOD_LoadBmodel: funny lump size in %s",loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = (cplane_t*) Hunk_Alloc ( count*2*sizeof(*out));

    loadmodel->planes = out;
    loadmodel->numplanes = count;

    for ( i=0 ; i<count ; i++, in++, out++)
    {
        bits = 0;
        for (j=0 ; j<3 ; j++)
        {
            out->normal[j] = LittleFloat (in->normal[j]);
            if (out->normal[j] < 0)
                bits |= 1<<j;
        }

        out->dist = LittleFloat (in->dist);
        out->type = LittleLong (in->type);
        out->signbits = bits;
    }
}

/*
=================
Mod_LoadBrushModel
=================
*/
void Mod_LoadBrushModel (model_t *mod, void *buffer)
{
    int			i;
    dheader_t	*header;
    model_t *bm;

    loadmodel->type = mod_brush;
    if (loadmodel != mod_known)
        ri.Sys_Error (ERR_DROP, "Loaded a brush model after the world");

    header = (dheader_t *)buffer;

    i = LittleLong (header->version);
    if (i != BSPVERSION)
        ri.Sys_Error (ERR_DROP, "Mod_LoadBrushModel: %s has wrong version number (%i should be %i)", mod->name, i, BSPVERSION);

// swap all the lumps
    mod_base = (byte *)header;

    for (i=0 ; i<sizeof(dheader_t)/4 ; i++)
        ((int *)header)[i] = LittleLong ( ((int *)header)[i]);

// load into heap

    fprintf(stderr, "DEBUG-SEQ: About to call Mod_LoadVertexes\n");
    Mod_LoadVertexes (&header->lumps[LUMP_VERTEXES]);
    fprintf(stderr, "DEBUG-SEQ: Mod_LoadVertexes returned OK\n");

    fprintf(stderr, "DEBUG-SEQ: About to call Mod_LoadEdges\n");
    Mod_LoadEdges (&header->lumps[LUMP_EDGES]);
    fprintf(stderr, "DEBUG-SEQ: Mod_LoadEdges returned OK\n");

    fprintf(stderr, "DEBUG-SEQ: About to call Mod_LoadSurfedges\n");
    Mod_LoadSurfedges (&header->lumps[LUMP_SURFEDGES]);
    fprintf(stderr, "DEBUG-SEQ: Mod_LoadSurfedges returned OK\n");

    fprintf(stderr, "DEBUG-SEQ: About to call Mod_LoadLighting\n");
    Mod_LoadLighting (&header->lumps[LUMP_LIGHTING]);
    fprintf(stderr, "DEBUG-SEQ: Mod_LoadLighting returned OK\n");

    fprintf(stderr, "DEBUG-SEQ: About to call Mod_LoadPlanes\n");
    Mod_LoadPlanes (&header->lumps[LUMP_PLANES]);
    fprintf(stderr, "DEBUG-SEQ: Mod_LoadPlanes returned OK\n");

    fprintf(stderr, "DEBUG-SEQ: About to call Mod_LoadTexinfo\n");
    Mod_LoadTexinfo (&header->lumps[LUMP_TEXINFO]);
    fprintf(stderr, "DEBUG-SEQ: Mod_LoadTexinfo returned OK\n");

    fprintf(stderr, "DEBUG-SEQ: About to call Mod_LoadFaces\n");
    Mod_LoadFaces (&header->lumps[LUMP_FACES]);
    fprintf(stderr, "DEBUG-SEQ: Mod_LoadFaces returned OK\n");

    fprintf(stderr, "DEBUG-SEQ: About to call Mod_LoadMarksurfaces\n");
    Mod_LoadMarksurfaces (&header->lumps[LUMP_LEAFFACES]);
    fprintf(stderr, "DEBUG-SEQ: Mod_LoadMarksurfaces returned OK\n");

    fprintf(stderr, "DEBUG-SEQ: About to call Mod_LoadVisibility\n");
    Mod_LoadVisibility (&header->lumps[LUMP_VISIBILITY]);
    fprintf(stderr, "DEBUG-SEQ: Mod_LoadVisibility returned OK\n");

    fprintf(stderr, "DEBUG-SEQ: About to call Mod_LoadLeafs\n");
    Mod_LoadLeafs (&header->lumps[LUMP_LEAFS]);
    fprintf(stderr, "DEBUG-SEQ: Mod_LoadLeafs returned OK\n");

    fprintf(stderr, "DEBUG-SEQ: About to call Mod_LoadNodes\n");
    Mod_LoadNodes (&header->lumps[LUMP_NODES]);
    fprintf(stderr, "DEBUG-SEQ: Mod_LoadNodes returned OK\n");

    fprintf(stderr, "DEBUG-SEQ: About to call Mod_LoadSubmodels\n");
    Mod_LoadSubmodels (&header->lumps[LUMP_MODELS]);
    fprintf(stderr, "DEBUG-SEQ: Mod_LoadSubmodels returned OK\n");
    mod->numframes = 2;		// regular and alternate animation
    
    //
    // set up the submodels
    //
    fprintf(stderr, "DEBUG-POST: numsubmodels=%d, sizeof(model_t)=%zu\n", mod->numsubmodels, sizeof(model_t));
    
    for (i=0 ; i<mod->numsubmodels ; i++)
    {
        model_t	*starmod;
    
        bm = &mod->submodels[i];
        starmod = &mod_inline[i];
    
        fprintf(stderr, "DEBUG-POST: submodel i=%d starmod=%p\n", i, (void*)starmod);
    
        // DO NOT do full struct copy (*starmod = *loadmodel) — the Urho3D port
        // extended model_t with C++ object pointers (Node*, Model*, etc.) and
        // a bitwise struct copy corrupts reference counts / causes double-free.
        // Copy only standard Quake 2 POD fields instead:
        // memset removed to avoid corrupting C++ objects

        strncpy(starmod->name, loadmodel->name, sizeof(starmod->name) - 1);
        starmod->name[sizeof(starmod->name) - 1] = '\0';
        starmod->registration_sequence = loadmodel->registration_sequence;
        starmod->type = loadmodel->type;
        starmod->numframes = loadmodel->numframes;
        starmod->flags = loadmodel->flags;

        starmod->numsubmodels = loadmodel->numsubmodels;
        starmod->submodels = loadmodel->submodels;
        starmod->numplanes = loadmodel->numplanes;
        starmod->planes = loadmodel->planes;
        starmod->numleafs = loadmodel->numleafs;
        starmod->leafs = loadmodel->leafs;
        starmod->numvertexes = loadmodel->numvertexes;
        starmod->vertexes = loadmodel->vertexes;
        starmod->numedges = loadmodel->numedges;
        starmod->edges = loadmodel->edges;
        starmod->numnodes = loadmodel->numnodes;
        starmod->nodes = loadmodel->nodes;
        starmod->numtexinfo = loadmodel->numtexinfo;
        starmod->texinfo = loadmodel->texinfo;
        starmod->numsurfaces = loadmodel->numsurfaces;
        starmod->surfaces = loadmodel->surfaces;
        starmod->numsurfedges = loadmodel->numsurfedges;
        starmod->surfedges = loadmodel->surfedges;
        starmod->nummarksurfaces = loadmodel->nummarksurfaces;
        starmod->marksurfaces = loadmodel->marksurfaces;
        starmod->vis = loadmodel->vis;
        starmod->lightdata = loadmodel->lightdata;

        // Intentionally NOT copied: material (Urho3D refcount ptr), skins, extradata
        // Submodels share BSP data but don't own alias/sprite resources

        // Initialize Urho3D pointers to NULL to prevent crashes when accessed
        memset(starmod->skins, 0, sizeof(starmod->skins));
        starmod->material = NULL;
        starmod->extradata = NULL;

        starmod->firstmodelsurface = bm->firstface;
        starmod->nummodelsurfaces = bm->numfaces;
        starmod->firstnode = bm->headnode;
        if (starmod->firstnode >= loadmodel->numnodes)
            ri.Sys_Error (ERR_DROP, "Inline model %i has bad firstnode", i);
    
        VectorCopy (bm->maxs, starmod->maxs);
        VectorCopy (bm->mins, starmod->mins);
        starmod->radius = bm->radius;
    
        if (i == 0)
        {
            // Same problem here: DO NOT copy full struct back.
            // Only copy the fields that submodel 0 overrides.
            loadmodel->firstmodelsurface = starmod->firstmodelsurface;
            loadmodel->nummodelsurfaces = starmod->nummodelsurfaces;
            loadmodel->firstnode = starmod->firstnode;
            VectorCopy(starmod->maxs, loadmodel->maxs);
            VectorCopy(starmod->mins, loadmodel->mins);
            loadmodel->radius = starmod->radius;
        }
    
        starmod->numleafs = bm->visleafs;
        fprintf(stderr, "DEBUG-POST: submodel i=%d done\n", i);
    }
    fprintf(stderr, "DEBUG-POST: submodel loop finished\n");
}

/*
==============================================================================

ALIAS MODELS

==============================================================================
*/


#define	MAX_LBM_HEIGHT		480

/*
=================
Mod_LoadAliasModel
=================
*/
void Mod_LoadAliasModel (model_t *mod, void *buffer)
{
    int					i, j;
    dmdl_t				*pinmodel, *pheader;
    dstvert_t			*pinst, *poutst;
    dtriangle_t			*pintri, *pouttri;
    daliasframe_t		*pinframe, *poutframe;
    int					*pincmd, *poutcmd;
    int					version;

    pinmodel = (dmdl_t *)buffer;

    version = LittleLong (pinmodel->version);
    if (version != ALIAS_VERSION)
        ri.Sys_Error (ERR_DROP, "%s has wrong version number (%i should be %i)",
                 mod->name, version, ALIAS_VERSION);

    pheader = (dmdl_t *) Hunk_Alloc (LittleLong(pinmodel->ofs_end));

    // byte swap the header fields and sanity check
    for (i=0 ; i<sizeof(dmdl_t)/4 ; i++)
        ((int *)pheader)[i] = LittleLong (((int *)buffer)[i]);

    if (pheader->skinheight > MAX_LBM_HEIGHT)
        ri.Sys_Error (ERR_DROP, "model %s has a skin taller than %d", mod->name,
                   MAX_LBM_HEIGHT);

    if (pheader->num_xyz <= 0)
        ri.Sys_Error (ERR_DROP, "model %s has no vertices", mod->name);

    if (pheader->num_xyz > MAX_VERTS)
        ri.Sys_Error (ERR_DROP, "model %s has too many vertices", mod->name);

    if (pheader->num_st <= 0)
        ri.Sys_Error (ERR_DROP, "model %s has no st vertices", mod->name);

    if (pheader->num_tris <= 0)
        ri.Sys_Error (ERR_DROP, "model %s has no triangles", mod->name);

    if (pheader->num_frames <= 0)
        ri.Sys_Error (ERR_DROP, "model %s has no frames", mod->name);

//
// load base s and t vertices (not used in gl version)
//
    pinst = (dstvert_t *) ((byte *)pinmodel + pheader->ofs_st);
    poutst = (dstvert_t *) ((byte *)pheader + pheader->ofs_st);

    for (i=0 ; i<pheader->num_st ; i++)
    {
        poutst[i].s = LittleShort (pinst[i].s);
        poutst[i].t = LittleShort (pinst[i].t);
    }

//
// load triangle lists
//
    pintri = (dtriangle_t *) ((byte *)pinmodel + pheader->ofs_tris);
    pouttri = (dtriangle_t *) ((byte *)pheader + pheader->ofs_tris);

    for (i=0 ; i<pheader->num_tris ; i++)
    {
        for (j=0 ; j<3 ; j++)
        {
            pouttri[i].index_xyz[j] = LittleShort (pintri[i].index_xyz[j]);
            pouttri[i].index_st[j] = LittleShort (pintri[i].index_st[j]);
        }
    }

//
// load the frames
//
    for (i=0 ; i<pheader->num_frames ; i++)
    {
        pinframe = (daliasframe_t *) ((byte *)pinmodel
            + pheader->ofs_frames + i * pheader->framesize);
        poutframe = (daliasframe_t *) ((byte *)pheader
            + pheader->ofs_frames + i * pheader->framesize);

        memcpy (poutframe->name, pinframe->name, sizeof(poutframe->name));
        for (j=0 ; j<3 ; j++)
        {
            poutframe->scale[j] = LittleFloat (pinframe->scale[j]);
            poutframe->translate[j] = LittleFloat (pinframe->translate[j]);
        }
        // verts are all 8 bit, so no swapping needed
        memcpy (poutframe->verts, pinframe->verts,
            pheader->num_xyz*sizeof(dtrivertx_t));

    }

    mod->type = mod_alias;

    //
    // load the glcmds
    //
    pincmd = (int *) ((byte *)pinmodel + pheader->ofs_glcmds);
    poutcmd = (int *) ((byte *)pheader + pheader->ofs_glcmds);
    for (i=0 ; i<pheader->num_glcmds ; i++)
        poutcmd[i] = LittleLong (pincmd[i]);


    // register all skins
    memcpy ((char *)pheader + pheader->ofs_skins, (char *)pinmodel + pheader->ofs_skins,
        pheader->num_skins*MAX_SKINNAME);
    for (i=0 ; i<pheader->num_skins ; i++)
    {
        mod->skins[i] = GL_FindImage ((char *)pheader + pheader->ofs_skins + i*MAX_SKINNAME, it_skin);
    }

    mod->mins[0] = -32;
    mod->mins[1] = -32;
    mod->mins[2] = -32;
    mod->maxs[0] = 32;
    mod->maxs[1] = 32;
    mod->maxs[2] = 32;
}

/*
==============================================================================

SPRITE MODELS

==============================================================================
*/

/*
=================
Mod_LoadSpriteModel
=================
*/
void Mod_LoadSpriteModel (model_t *mod, void *buffer)
{
    dsprite_t	*sprin, *sprout;
    int			i;

    sprin = (dsprite_t *)buffer;
    sprout = (dsprite_t*) Hunk_Alloc (modfilelen);

    sprout->ident = LittleLong (sprin->ident);
    sprout->version = LittleLong (sprin->version);
    sprout->numframes = LittleLong (sprin->numframes);

    if (sprout->version != SPRITE_VERSION)
        ri.Sys_Error (ERR_DROP, "%s has wrong version number (%i should be %i)",
                 mod->name, sprout->version, SPRITE_VERSION);

    if (sprout->numframes > MAX_MD2SKINS)
        ri.Sys_Error (ERR_DROP, "%s has too many frames (%i > %i)",
                 mod->name, sprout->numframes, MAX_MD2SKINS);

    // byte swap everything
    for (i=0 ; i<sprout->numframes ; i++)
    {
        sprout->frames[i].width = LittleLong (sprin->frames[i].width);
        sprout->frames[i].height = LittleLong (sprin->frames[i].height);
        sprout->frames[i].origin_x = LittleLong (sprin->frames[i].origin_x);
        sprout->frames[i].origin_y = LittleLong (sprin->frames[i].origin_y);
        memcpy (sprout->frames[i].name, sprin->frames[i].name, MAX_SKINNAME);
        mod->skins[i] = GL_FindImage (sprout->frames[i].name,
            it_sprite);
    }

    mod->type = mod_sprite;
}

//=============================================================================

/*
@@@@@@@@@@@@@@@@@@@@@
R_BeginRegistration

Specifies the model that will be used as the world
@@@@@@@@@@@@@@@@@@@@@
*/
void R_BeginRegistration (char *model)
{
    char	fullname[MAX_QPATH];
    cvar_t	*flushmap;

    registration_sequence++;
    //r_oldviewcluster = -1;		// force markleafs

    Com_sprintf (fullname, sizeof(fullname), "maps/%s.bsp", model);

    // explicitly free the old map if different
    // this guarantees that mod_known[0] is the world map
    flushmap = ri.Cvar_Get ("flushmap", "0", 0);
    if ( strcmp(mod_known[0].name, fullname) || flushmap->value)
        Mod_Free (&mod_known[0]);

    r_worldmodel = Mod_ForName(fullname, qtrue);

    //r_viewcluster = -1;
}


/*
@@@@@@@@@@@@@@@@@@@@@
R_RegisterModel

@@@@@@@@@@@@@@@@@@@@@
*/
struct model_s *R_RegisterModel (char *name)
{
    model_t	*mod;
    int		i;
    dsprite_t	*sprout;
    dmdl_t		*pheader;

    mod = Mod_ForName (name, qfalse);
    if (mod)
    {
        mod->registration_sequence = registration_sequence;

        // register any images used by the models
        if (mod->type == mod_sprite)
        {
            sprout = (dsprite_t *)mod->extradata;
            for (i=0 ; i<sprout->numframes ; i++)
                mod->skins[i] = GL_FindImage (sprout->frames[i].name, it_sprite);
        }
        else if (mod->type == mod_alias)
        {
            pheader = (dmdl_t *)mod->extradata;
            for (i=0 ; i<pheader->num_skins ; i++)
                mod->skins[i] = GL_FindImage ((char *)pheader + pheader->ofs_skins + i*MAX_SKINNAME, it_skin);

            mod->numframes = pheader->num_frames;

            GetAliasModel(mod);
        }
        else if (mod->type == mod_brush)
        {
            for (i=0 ; i<mod->numtexinfo ; i++)
                mod->texinfo[i].image->registration_sequence = registration_sequence;
        }
    }
    return mod;
}

/*
@@@@@@@@@@@@@@@@@@@@@
R_EndRegistration

@@@@@@@@@@@@@@@@@@@@@
*/
void R_EndRegistration (void)
{
    int		i;
    model_t	*mod;

    for (i=0, mod=mod_known ; i<mod_numknown ; i++, mod++)
    {
        if (!mod->name[0])
            continue;
        if (mod->registration_sequence != registration_sequence)
        {	// don't need this model
            Mod_Free (mod);
        }
    }

    GL_FreeUnusedImages ();

    void R_InitMapModel();
    R_InitMapModel();

}


//=============================================================================


/*
================
Mod_Free
================
*/
void Mod_Free (model_t *mod)
{
    Hunk_Free (mod->extradata);
    memset (mod, 0, sizeof(*mod));
}

/*
================
Mod_FreeAll
================
*/
void Mod_FreeAll (void)
{
    int i;
    model_t *mod;

    for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++)
    {
        if (!mod->name[0])
            continue;
        Mod_Free(mod);
    }
    mod_numknown = 0;
    r_worldmodel = NULL;
    currentmodel = NULL;
    loadmodel = NULL;
}
