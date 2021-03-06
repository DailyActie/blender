/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/sculpt_paint/paint_vertex_weight_utils.c
 *  \ingroup edsculpt
 *
 * Intended for use by `paint_vertex.c` & `paint_vertex_weight_ops.c`.
 */

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_string_utils.h"

#include "DNA_armature_types.h"
#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"
#include "DNA_brush_types.h"
#include "DNA_object_types.h"

#include "BKE_action.h"
#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_mesh.h"
#include "BKE_modifier.h"
#include "BKE_object_deform.h"
#include "BKE_report.h"

#include "WM_api.h"
#include "WM_types.h"

#include "paint_intern.h"  /* own include */

/* -------------------------------------------------------------------- */
/** \name Weight Paint Sanity Checks
 * \{ */

/* ensure we have data on wpaint start, add if needed */
bool ED_wpaint_ensure_data(
        bContext *C, struct ReportList *reports,
        enum eWPaintFlag flag, struct WPaintVGroupIndex *vgroup_index)
{
	Scene *scene = CTX_data_scene(C);
	Object *ob = CTX_data_active_object(C);
	Mesh *me = BKE_mesh_from_object(ob);

	if (vgroup_index) {
		vgroup_index->active = -1;
		vgroup_index->mirror = -1;
	}

	if (scene->obedit) {
		return false;
	}

	if (me == NULL || me->totpoly == 0) {
		return false;
	}

	/* if nothing was added yet, we make dverts and a vertex deform group */
	if (!me->dvert) {
		BKE_object_defgroup_data_create(&me->id);
		WM_event_add_notifier(C, NC_GEOM | ND_DATA, me);
	}

	/* this happens on a Bone select, when no vgroup existed yet */
	if (ob->actdef <= 0) {
		Object *modob;
		if ((modob = modifiers_isDeformedByArmature(ob))) {
			Bone *actbone = ((bArmature *)modob->data)->act_bone;
			if (actbone) {
				bPoseChannel *pchan = BKE_pose_channel_find_name(modob->pose, actbone->name);

				if (pchan) {
					bDeformGroup *dg = defgroup_find_name(ob, pchan->name);
					if (dg == NULL) {
						dg = BKE_object_defgroup_add_name(ob, pchan->name);  /* sets actdef */
					}
					else {
						int actdef = 1 + BLI_findindex(&ob->defbase, dg);
						BLI_assert(actdef >= 0);
						ob->actdef = actdef;
					}
				}
			}
		}
	}
	if (BLI_listbase_is_empty(&ob->defbase)) {
		BKE_object_defgroup_add(ob);
	}

	/* ensure we don't try paint onto an invalid group */
	if (ob->actdef <= 0) {
		BKE_report(reports, RPT_WARNING, "No active vertex group for painting, aborting");
		return false;
	}

	if (vgroup_index) {
		vgroup_index->active = ob->actdef - 1;
	}

	if (flag & WPAINT_ENSURE_MIRROR) {
		if (me->editflag & ME_EDIT_MIRROR_X) {
			int mirror = ED_wpaint_mirror_vgroup_ensure(ob, ob->actdef - 1);
			if (vgroup_index) {
				vgroup_index->mirror = mirror;
			}
		}
	}

	return true;
}
/** \} */

/* mirror_vgroup is set to -1 when invalid */
int ED_wpaint_mirror_vgroup_ensure(Object *ob, const int vgroup_active)
{
	bDeformGroup *defgroup = BLI_findlink(&ob->defbase, vgroup_active);

	if (defgroup) {
		int mirrdef;
		char name_flip[MAXBONENAME];

		BLI_string_flip_side_name(name_flip, defgroup->name, false, sizeof(name_flip));
		mirrdef = defgroup_name_index(ob, name_flip);
		if (mirrdef == -1) {
			if (BKE_defgroup_new(ob, name_flip)) {
				mirrdef = BLI_listbase_count(&ob->defbase) - 1;
			}
		}

		/* curdef should never be NULL unless this is
		 * a  lamp and BKE_object_defgroup_add_name fails */
		return mirrdef;
	}

	return -1;
}

/* -------------------------------------------------------------------- */
/** \name Weight Blending Modes
 * \{ */

BLI_INLINE float wval_blend(const float weight, const float paintval, const float alpha)
{
	const float talpha = min_ff(alpha, 1.0f);  /* blending with values over 1 doesn't make sense */
	return (paintval * talpha) + (weight * (1.0f - talpha));
}
BLI_INLINE float wval_add(const float weight, const float paintval, const float alpha)
{
	return weight + (paintval * alpha);
}
BLI_INLINE float wval_sub(const float weight, const float paintval, const float alpha)
{
	return weight - (paintval * alpha);
}
BLI_INLINE float wval_mul(const float weight, const float paintval, const float alpha)
{   /* first mul, then blend the fac */
	return ((1.0f - alpha) + (alpha * paintval)) * weight;
}
BLI_INLINE float wval_lighten(const float weight, const float paintval, const float alpha)
{
	return (weight < paintval) ? wval_blend(weight, paintval, alpha) : weight;
}
BLI_INLINE float wval_darken(const float weight, const float paintval, const float alpha)
{
	return (weight > paintval) ? wval_blend(weight, paintval, alpha) : weight;
}

/* vpaint has 'vpaint_blend_tool' */
/* result is not clamped from [0-1] */
float ED_wpaint_blend_tool(
        const int tool,
        /* dw->weight */
        const float weight,
        const float paintval, const float alpha)
{
	switch (tool) {
		case PAINT_BLEND_MIX:
		case PAINT_BLEND_AVERAGE:
		case PAINT_BLEND_SMEAR:
		case PAINT_BLEND_BLUR:     return wval_blend(weight, paintval, alpha);
		case PAINT_BLEND_ADD:      return wval_add(weight, paintval, alpha);
		case PAINT_BLEND_SUB:      return wval_sub(weight, paintval, alpha);
		case PAINT_BLEND_MUL:      return wval_mul(weight, paintval, alpha);
		case PAINT_BLEND_LIGHTEN:  return wval_lighten(weight, paintval, alpha);
		case PAINT_BLEND_DARKEN:   return wval_darken(weight, paintval, alpha);
		default:
			BLI_assert(0);
			return 0.0f;
	}
}

/** \} */