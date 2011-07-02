/*
 * $Id$
 *
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
 * The Original Code is Copyright (C) 2011 Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Blender Foundation,
 *                 Sergey Sharybin
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/tracking.c
 *  \ingroup bke
 */

#include <stddef.h>

#include "MEM_guardedalloc.h"

#include "DNA_movieclip_types.h"
#include "DNA_object_types.h"	/* SELECT */

#include "BLI_utildefines.h"
#include "BLI_math.h"
#include "BLI_listbase.h"
#include "BLI_ghash.h"

#include "BKE_tracking.h"
#include "BKE_movieclip.h"

#include "IMB_imbuf_types.h"
#include "IMB_imbuf.h"

#ifdef WITH_LIBMV
#include "libmv-capi.h"
#endif

/*********************** common functions *************************/

void BKE_tracking_clamp_track(MovieTrackingTrack *track, int event)
{
	int a;

	/* sort */
	for(a= 0; a<2; a++) {
		if(track->pat_min[a]>track->pat_max[a])
			SWAP(float, track->pat_min[a], track->pat_max[a]);

		if(track->search_min[a]>track->search_max[a])
			SWAP(float, track->search_min[a], track->search_max[a]);
	}

	if(event==CLAMP_PAT_DIM) {
		for(a= 0; a<2; a++) {
			/* pattern shouldn't be resized bigger than search */
			track->pat_min[a]= MAX2(track->pat_min[a], track->search_min[a]);
			track->pat_max[a]= MIN2(track->pat_max[a], track->search_max[a]);
		}
	}
	else if(event==CLAMP_PAT_POS) {
		float dim[2];
		sub_v2_v2v2(dim, track->pat_max, track->pat_min);

		for(a= 0; a<2; a++) {
			/* pattern shouldn't be moved outside of search */
			if(track->pat_min[a] < track->search_min[a]) {
				track->pat_min[a]= track->search_min[a];
				track->pat_max[a]= track->pat_min[a]+dim[a];
			}
			if(track->pat_max[a] > track->search_max[a]) {
				track->pat_max[a]= track->search_max[a];
				track->pat_min[a]= track->pat_max[a]-dim[a];
			}
		}
	}
	else if(event==CLAMP_SEARCH_DIM) {
		for(a= 0; a<2; a++) {
			/* search shouldn't be resized smaller than pattern */
			track->search_min[a]= MIN2(track->pat_min[a], track->search_min[a]);
			track->search_max[a]= MAX2(track->pat_max[a], track->search_max[a]);
		}
	}
	else if(event==CLAMP_SEARCH_POS) {
		float dim[2];
		sub_v2_v2v2(dim, track->search_max, track->search_min);

		for(a= 0; a<2; a++) {
			/* search shouldn't be moved inside pattern */
			if(track->search_min[a] > track->pat_min[a]) {
				track->search_min[a]= track->pat_min[a];
				track->search_max[a]= track->search_min[a]+dim[a];
			}
			if(track->search_max[a] < track->pat_max[a]) {
				track->search_max[a]= track->pat_max[a];
				track->search_min[a]= track->search_max[a]-dim[a];
			}
		}
	}

	/* marker's center should be inside pattern */
	if(event==CLAMP_PAT_DIM || event==CLAMP_PAT_POS) {
		float dim[2];
		sub_v2_v2v2(dim, track->pat_max, track->pat_min);

		for(a= 0; a<2; a++) {
			if(track->pat_min[a] > 0.0f) {
				track->pat_min[a]= 0.0f;
				track->pat_max[a]= dim[a];
			}
			if(track->pat_max[a] < 0.0f) {
				track->pat_max[a]= 0.0f;
				track->pat_min[a]= -dim[a];
			}
		}
	}
}

void BKE_tracking_track_flag(MovieTrackingTrack *track, int area, int flag, int clear)
{
	if(area==TRACK_AREA_NONE)
		return;

	if(clear) {
		if(area&TRACK_AREA_POINT)	track->flag&= ~flag;
		if(area&TRACK_AREA_PAT)		track->pat_flag&= ~flag;
		if(area&TRACK_AREA_SEARCH)	track->search_flag&= ~flag;
	} else {
		if(area&TRACK_AREA_POINT)	track->flag|= flag;
		if(area&TRACK_AREA_PAT)		track->pat_flag|= flag;
		if(area&TRACK_AREA_SEARCH)	track->search_flag|= flag;
	}
}

void BKE_tracking_insert_marker(MovieTrackingTrack *track, MovieTrackingMarker *marker)
{
	MovieTrackingMarker *old_marker= BKE_tracking_get_marker(track, marker->framenr);

	if(old_marker && old_marker->framenr==marker->framenr) {
		*old_marker= *marker;
	} else {
		int a= track->markersnr;

		while(a--) {
			if(track->markers[a].framenr<marker->framenr)
				break;
		}

		track->markersnr++;

		if(track->markers) track->markers= MEM_reallocN(track->markers, sizeof(MovieTrackingMarker)*track->markersnr);
		else track->markers= MEM_callocN(sizeof(MovieTrackingMarker), "MovieTracking markers");

		memmove(track->markers+a+2, track->markers+a+1, (track->markersnr-a-2)*sizeof(MovieTrackingMarker));
		track->markers[a+1]= *marker;

		track->last_marker= a+1;
	}
}

void BKE_tracking_delete_marker(MovieTrackingTrack *track, int framenr)
{
	int a= 1;

	while(a<track->markersnr) {
		if(track->markers[a].framenr==framenr) {
			if(track->markersnr>1) {
				memmove(track->markers+a, track->markers+a+1, (track->markersnr-a-1)*sizeof(MovieTrackingMarker));
				track->markersnr--;
				track->markers= MEM_reallocN(track->markers, sizeof(MovieTrackingMarker)*track->markersnr);
			} else {
				MEM_freeN(track->markers);
				track->markers= NULL;
				track->markersnr= 0;
			}

			break;
		}

		a++;
	}}

MovieTrackingMarker *BKE_tracking_get_marker(MovieTrackingTrack *track, int framenr)
{
	int a= track->markersnr-1;

	if(!track->markersnr)
		return NULL;

	/* approximate pre-first framenr marker with first marker */
	if(framenr<track->markers[0].framenr)
		return &track->markers[0];

	if(track->last_marker<track->markersnr)
		a= track->last_marker;

	if(track->markers[a].framenr<=framenr) {
		while(a<track->markersnr && track->markers[a].framenr<=framenr) {
			if(track->markers[a].framenr==framenr) {
				track->last_marker= a;
				return &track->markers[a];
			}
			a++;
		}

		/* if there's no marker for exact position, use nearest marker from left side */
		return &track->markers[a-1];
	} else {
		while(a>=0 && track->markers[a].framenr>=framenr) {
			if(track->markers[a].framenr==framenr) {
				track->last_marker= a;
				return &track->markers[a];
			}

			a--;
		}

		/* if there's no marker for exact position, use nearest marker from left side */
		return &track->markers[a];
	}

	return NULL;
}

MovieTrackingMarker *BKE_tracking_ensure_marker(MovieTrackingTrack *track, int framenr)
{
	MovieTrackingMarker *marker= BKE_tracking_get_marker(track, framenr);

	if(marker && marker->framenr!=framenr) {
		MovieTrackingMarker marker_new;

		marker_new= *marker;
		marker_new.framenr= framenr;

		BKE_tracking_insert_marker(track, &marker_new);
		marker= BKE_tracking_get_marker(track, framenr);
	}

	return marker;
}

MovieTrackingMarker *BKE_tracking_exact_marker(MovieTrackingTrack *track, int framenr)
{
	MovieTrackingMarker *marker= BKE_tracking_get_marker(track, framenr);

	if(marker && marker->framenr!=framenr)
		return NULL;

	return marker;
}

int BKE_tracking_has_marker(MovieTrackingTrack *track, int framenr)
{
	return BKE_tracking_get_marker(track, framenr) != 0;
}

void BKE_tracking_free_track(MovieTrackingTrack *track)
{
	if(track->markers) MEM_freeN(track->markers);
}

MovieTrackingTrack *BKE_tracking_copy_track(MovieTrackingTrack *track)
{
	MovieTrackingTrack *new_track= MEM_dupallocN(track);

	new_track->next= new_track->prev= NULL;

	if(new_track->markers)
		new_track->markers= MEM_dupallocN(new_track->markers);

	return new_track;
}

void BKE_tracking_clear_path(MovieTrackingTrack *track, int ref_frame, int action)
{
	int a;

	if(action==TRACK_CLEAR_REMAINED) {
		a= 1;
		while(a<track->markersnr) {
			if(track->markers[a].framenr>ref_frame) {
				track->markersnr= a;
				track->markers= MEM_reallocN(track->markers, sizeof(MovieTrackingMarker)*track->markersnr);

				break;
			}

			a++;
		}
	} else if(action==TRACK_CLEAR_UPTO) {
		a= track->markersnr-1;
		while(a>=0) {
			if(track->markers[a].framenr<=ref_frame) {
				memmove(track->markers, track->markers+a, (track->markersnr-a)*sizeof(MovieTrackingMarker));

				track->markersnr= track->markersnr-a;
				track->markers= MEM_reallocN(track->markers, sizeof(MovieTrackingMarker)*track->markersnr);

				break;
			}

			a--;
		}
	} else if(action==TRACK_CLEAR_ALL) {
		MovieTrackingMarker *marker, marker_new;

		marker= BKE_tracking_get_marker(track, ref_frame);
		if(marker)
			marker_new= *marker;

		MEM_freeN(track->markers);
		track->markers= NULL;
		track->markersnr= 0;

		if(marker)
			BKE_tracking_insert_marker(track, &marker_new);
	}
}

void BKE_tracking_free(MovieTracking *tracking)
{
	MovieTrackingTrack *track;

	for(track= tracking->tracks.first; track; track= track->next) {
		BKE_tracking_free_track(track);
	}

	BLI_freelistN(&tracking->tracks);
	BLI_freelistN(&tracking->bundles);
}

/*********************** tracking *************************/

typedef struct MovieTrackingContext {
	MovieClipUser user;
	MovieClip *clip;

#ifdef WITH_LIBMV
	libmv_regionTrackerHandle region_tracker;
#endif
	ListBase tracks;
	GHash *hash;
	MovieTrackingSettings settings;

	int backwards;
	int sync_frame;
} MovieTrackingContext;

MovieTrackingContext *BKE_tracking_context_new(MovieClip *clip, MovieClipUser *user, int backwards)
{
	MovieTrackingContext *context= MEM_callocN(sizeof(MovieTrackingContext), "trackingContext");
	MovieTracking *tracking= &clip->tracking;
	MovieTrackingSettings *settings= &tracking->settings;
	MovieTrackingTrack *track;

#ifdef WITH_LIBMV
	context->region_tracker= libmv_regionTrackerNew(100, 3, 0.2);
#endif

	context->settings= *settings;
	context->backwards= backwards;
	context->hash= BLI_ghash_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, "tracking trackHash");

	track= tracking->tracks.first;
	while(track) {
		if(TRACK_SELECTED(track)) {
			MovieTrackingTrack *new_track= BKE_tracking_copy_track(track);

			BLI_addtail(&context->tracks, new_track);
			BLI_ghash_insert(context->hash, new_track, track);
		}

		track= track->next;
	}

	context->clip= clip;
	context->user= *user;

	return context;
}

void BKE_tracking_context_free(MovieTrackingContext *context)
{
	MovieTrackingTrack *track;

#if WITH_LIBMV
	libmv_regionTrackerDestroy(context->region_tracker);
#endif

	track= context->tracks.first;
	while(track) {
		BKE_tracking_free_track(track);
		track= track->next;
	}
	BLI_freelistN(&context->tracks);

	BLI_ghash_free(context->hash, NULL, NULL);

	MEM_freeN(context);
}

static void disable_imbuf_channels(ImBuf *ibuf, MovieTrackingTrack *track)
{
	int x, y;

	if((track->flag&(TRACK_DISABLE_RED|TRACK_DISABLE_GREEN|TRACK_DISABLE_BLUE))==0)
		return;

	for(y= 0; y<ibuf->y; y++) {
		for (x= 0; x<ibuf->x; x++) {
			int pixel= ibuf->x*y + x;
			char *rrgb= (char*)ibuf->rect + pixel*4;

			if(track->flag&TRACK_DISABLE_RED)	rrgb[0]= 0;
			if(track->flag&TRACK_DISABLE_GREEN)	rrgb[1]= 0;
			if(track->flag&TRACK_DISABLE_BLUE)	rrgb[2]= 0;
		}
	}
}

static ImBuf *acquire_area_imbuf(ImBuf *ibuf, MovieTrackingTrack *track, MovieTrackingMarker *marker,
			float min[2], float max[2], int margin, float pos[2], int origin[2])
{
	ImBuf *tmpibuf;
	int x, y;
	int x1, y1, x2, y2, w, h;

	x= marker->pos[0]*ibuf->x;
	y= marker->pos[1]*ibuf->y;
	x1= x-(int)(-min[0]*ibuf->x);
	y1= y-(int)(-min[1]*ibuf->y);
	x2= x+(int)(max[0]*ibuf->x);
	y2= y+(int)(max[1]*ibuf->y);

	/* dimensions should be odd */
	w= (x2-x1)|1;
	h= (y2-y1)|1;

	/* happens due to rounding issues */
	if(x1+w<=x) x1++;
	if(y1+h<=y) y1++;

	tmpibuf= IMB_allocImBuf(w+margin*2, h+margin*2, 32, IB_rect);
	IMB_rectcpy(tmpibuf, ibuf, 0, 0, x1-margin, y1-margin, w+margin*2, h+margin*2);

	if(pos != NULL) {
		pos[0]= x-x1+(marker->pos[0]*ibuf->x-x)+margin;
		pos[1]= y-y1+(marker->pos[1]*ibuf->y-y)+margin;
	}

	if(origin != NULL) {
		origin[0]= x1-margin;
		origin[1]= y1-margin;
	}

	disable_imbuf_channels(tmpibuf, track);

	return tmpibuf;
}

ImBuf *BKE_tracking_acquire_pattern_imbuf(ImBuf *ibuf, MovieTrackingTrack *track, MovieTrackingMarker *marker,
			int margin, float pos[2], int origin[2])
{
	return acquire_area_imbuf(ibuf, track, marker, track->pat_min, track->pat_max, margin, pos, origin);
}

ImBuf *BKE_tracking_acquire_search_imbuf(ImBuf *ibuf, MovieTrackingTrack *track, MovieTrackingMarker *marker,
			int margin, float pos[2], int origin[2])
{
	return acquire_area_imbuf(ibuf, track, marker, track->search_min, track->search_max, margin, pos, origin);
}

#ifdef WITH_LIBMV
static float *acquire_search_floatbuf(ImBuf *ibuf, MovieTrackingTrack *track, MovieTrackingMarker *marker,
			int *width_r, int *height_r, float pos[2], int origin[2])
{
	ImBuf *tmpibuf;
	float *pixels, *fp;
	int x, y, width, height;

	width= (track->search_max[0]-track->search_min[0])*ibuf->x;
	height= (track->search_max[1]-track->search_min[1])*ibuf->y;

	tmpibuf= BKE_tracking_acquire_search_imbuf(ibuf, track, marker, 0, pos, origin);
	disable_imbuf_channels(tmpibuf, track);

	*width_r= width;
	*height_r= height;

	fp= pixels= MEM_callocN(width*height*sizeof(float), "tracking floatBuf");
	for(y= 0; y<(int)height; y++) {
		for (x= 0; x<(int)width; x++) {
			int pixel= tmpibuf->x*y + x;
			char *rrgb= (char*)tmpibuf->rect + pixel*4;

			*fp= (0.2126*rrgb[0] + 0.7152*rrgb[1] + 0.0722*rrgb[2])/255;
			fp++;
		}
	}

	IMB_freeImBuf(tmpibuf);

	return pixels;
}
#endif

void BKE_tracking_sync(MovieTrackingContext *context)
{
	MovieTrackingTrack *track;
	ListBase tracks= {NULL, NULL};
	ListBase *old_tracks= &context->clip->tracking.tracks;
	int sel_type, newframe;
	void *sel;

	BKE_movieclip_last_selection(context->clip, &sel_type, &sel);

	/* duplicate currently tracking tracks to list of displaying tracks */
	track= context->tracks.first;
	while(track) {
		int replace_sel= 0;
		MovieTrackingTrack *new_track, *old;

		/* find original of tracking track in list of previously displayed tracks */
		old= BLI_ghash_lookup(context->hash, track);
		if(old) {
			MovieTrackingTrack *cur= old_tracks->first;

			while(cur) {
				if(cur==old)
					break;

				cur= cur->next;
			}

			/* original track was found, re-use flags and remove this track */
			if(cur) {
				if(sel_type==MCLIP_SEL_TRACK && sel==cur)
					replace_sel= 1;

				track->flag= cur->flag | (track->flag&TRACK_PROCESSED);
				track->pat_flag= cur->pat_flag;
				track->search_flag= cur->search_flag;

				BKE_tracking_free_track(cur);
				BLI_freelinkN(old_tracks, cur);
			}
		}

		new_track= BKE_tracking_copy_track(track);

		BLI_ghash_remove(context->hash, track, NULL, NULL); /* XXX: are we actually need this */
		BLI_ghash_insert(context->hash, track, new_track);

		if(replace_sel)		/* update current selection in clip */
			BKE_movieclip_set_selection(context->clip, MCLIP_SEL_TRACK, new_track);

		BLI_addtail(&tracks, new_track);

		track= track->next;
	}

	/* move tracks, which could be added by user during tracking */
	track= old_tracks->first;
	while(track) {
		MovieTrackingTrack *next= track->next;

		track->next= track->prev= NULL;
		BLI_addtail(&tracks, track);

		track= next;
	}

	context->clip->tracking.tracks= tracks;

	if(context->backwards) newframe= context->user.framenr+1;
	else newframe= context->user.framenr-1;

	context->sync_frame= newframe;
}

void BKE_tracking_sync_user(MovieClipUser *user, MovieTrackingContext *context)
{
	user->framenr= context->sync_frame;
}

int BKE_tracking_next(MovieTrackingContext *context)
{
	ImBuf *ibuf, *ibuf_new;
	MovieTrackingTrack *track;
	int curfra= context->user.framenr;
	int ok= 0;

	/* nothing to track, avoid unneeded frames reading to save time and memory */
	if(!context->tracks.first)
		return 0;

	ibuf= BKE_movieclip_acquire_ibuf(context->clip, &context->user);
	if(!ibuf) return 0;

	if(context->backwards) context->user.framenr--;
	else context->user.framenr++;

	ibuf_new= BKE_movieclip_acquire_ibuf(context->clip, &context->user);
	if(!ibuf_new) {
		IMB_freeImBuf(ibuf);
		return 0;
	}

	track= context->tracks.first;
	while(track) {
		MovieTrackingMarker *marker= BKE_tracking_get_marker(track, curfra);

		if(marker && (marker->flag&MARKER_DISABLED)==0 && marker->framenr==curfra) {
#ifdef WITH_LIBMV
			int width, height, origin[2];
			float pos[2];
			float *patch= acquire_search_floatbuf(ibuf, track, marker, &width, &height, pos, origin);
			float *patch_new= acquire_search_floatbuf(ibuf_new, track, marker, &width, &height, pos, origin);
			double x1= pos[0], y1= pos[1];
			double x2= x1, y2= y1;
			int wndx, wndy;

			wndx= (int)((track->pat_max[0]-track->pat_min[0])*ibuf->x)/2;
			wndy= (int)((track->pat_max[1]-track->pat_min[1])*ibuf->y)/2;

			if(libmv_regionTrackerTrack(context->region_tracker, patch, patch_new,
						width, height, MAX2(wndx, wndy),
						x1, y1, &x2, &y2)) {
				MovieTrackingMarker marker_new;

				memset(&marker_new, 0, sizeof(marker_new));
				marker_new.pos[0]= (origin[0]+x2)/ibuf_new->x;
				marker_new.pos[1]= (origin[1]+y2)/ibuf_new->y;

				if(context->backwards) marker_new.framenr= curfra-1;
				else marker_new.framenr= curfra+1;

				track->flag|= TRACK_PROCESSED;

				BKE_tracking_insert_marker(track, &marker_new);

				ok= 1;
			}

			MEM_freeN(patch);
			MEM_freeN(patch_new);
#endif
		} else
			track->flag|= TRACK_PROCESSED;

		track= track->next;
	}

	IMB_freeImBuf(ibuf);
	IMB_freeImBuf(ibuf_new);

	return ok;
}

void BKE_track_unique_name(MovieTracking *tracking, MovieTrackingTrack *track)
{
	BLI_uniquename(&tracking->tracks, track, "Track", '.', offsetof(MovieTrackingTrack, name), sizeof(track->name));
}

MovieTrackingTrack *BKE_find_track_by_name(MovieTracking *tracking, const char *name)
{
	MovieTrackingTrack *track= tracking->tracks.first;

	while(track) {
		if(!strcmp(track->name, name))
			return track;

		track= track->next;
	}

	return NULL;
}
