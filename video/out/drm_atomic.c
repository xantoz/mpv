/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <inttypes.h>

#include "common/common.h"
#include "common/msg.h"
#include "drm_atomic.h"

int drm_object_create_properties(struct mp_log *log, int fd,
                                 struct drm_object *object)
{
    object->props = drmModeObjectGetProperties(fd, object->id, object->type);
    if (object->props) {
        object->props_info = talloc_zero_size(NULL, object->props->count_props
                                              * sizeof(object->props_info));
        if (object->props_info) {
            for (int i = 0; i < object->props->count_props; i++)
                object->props_info[i] = drmModeGetProperty(fd, object->props->props[i]);
        } else {
            mp_err(log, "Out of memory\n");
            goto fail;
        }
    } else {
        mp_err(log, "Failed to retrieve properties for object id %d\n", object->id);
        goto fail;
    }

    return 0;

  fail:
    drm_object_free_properties(object);
    return -1;
}

void drm_object_free_properties(struct drm_object *object)
{
    if (object->props) {
        for (int i = 0; i < object->props->count_props; i++) {
            if (object->props_info[i]) {
                drmModeFreeProperty(object->props_info[i]);
                object->props_info[i] = NULL;
            }
        }

        talloc_free(object->props_info);
        object->props_info = NULL;

        drmModeFreeObjectProperties(object->props);
        object->props = NULL;
    }
}

int drm_object_get_property(struct drm_object *object, char *name, uint64_t *value)
{
   for (int i = 0; i < object->props->count_props; i++) {
       if (strcasecmp(name, object->props_info[i]->name) == 0) {
           *value = object->props->prop_values[i];
           return 0;
       }
   }

   return -EINVAL;
}

drmModePropertyBlobPtr drm_object_get_property_blob(struct drm_object *object, char *name)
{
   uint64_t blob_id;

   if (!drm_object_get_property(object, name, &blob_id)) {
       return drmModeGetPropertyBlob(object->fd, blob_id);
   }

   return NULL;
}

int drm_object_set_property(drmModeAtomicReq *request, struct drm_object *object,
                            char *name, uint64_t value)
{
   for (int i = 0; i < object->props->count_props; i++) {
       if (strcasecmp(name, object->props_info[i]->name) == 0) {
           return drmModeAtomicAddProperty(request, object->id,
                                           object->props_info[i]->prop_id, value);
       }
   }

   return -EINVAL;
}

struct drm_object * drm_object_create(struct mp_log *log, int fd,
                                      uint32_t object_id, uint32_t type)
{
    struct drm_object *obj = NULL;
    obj = talloc_zero(NULL, struct drm_object);
    obj->id = object_id;
    obj->type = type;
    obj->fd = fd;

    if (drm_object_create_properties(log, fd, obj)) {
        talloc_free(obj);
        return NULL;
    }

    return obj;
}

void drm_object_free(struct drm_object *object)
{
    if (object) {
        drm_object_free_properties(object);
        talloc_free(object);
    }
}

void drm_object_print_info(struct mp_log *log, struct drm_object *object)
{
    mp_err(log, "Object ID = %d (type = %x) has %d properties\n",
           object->id, object->type, object->props->count_props);

    for (int i = 0; i < object->props->count_props; i++)
        mp_err(log, "    Property '%s' = %lld\n", object->props_info[i]->name,
               (long long)object->props->prop_values[i]);
}

struct drm_atomic_context *drm_atomic_create_context(struct mp_log *log, int fd, int crtc_id,
                                                     int connector_id,
                                                     int osd_plane_idx, int video_plane_idx)
{
    drmModePlaneRes *plane_res = NULL;
    drmModeRes *res = NULL;
    struct drm_object *plane = NULL;
    struct drm_atomic_context *ctx;
    int crtc_index = -1;
    int layercount = -1;
    int primary_id = 0;
    int overlay_id = 0;

    uint64_t value;

    res = drmModeGetResources(fd);
    if (!res) {
        mp_err(log, "Cannot retrieve DRM resources: %s\n", mp_strerror(errno));
        goto fail;
    }

    plane_res = drmModeGetPlaneResources(fd);
    if (!plane_res) {
        mp_err(log, "Cannot retrieve plane ressources: %s\n", mp_strerror(errno));
        goto fail;
    }

    ctx = talloc_zero(NULL, struct drm_atomic_context);
    if (!ctx) {
        mp_err(log, "Out of memory\n");
        goto fail;
    }

    ctx->fd = fd;
    ctx->crtc = drm_object_create(log, ctx->fd, crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!ctx->crtc) {
        mp_err(log, "Failed to create CRTC object\n");
        goto fail;
    }

    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id) {
            crtc_index = i;
            break;
        }
    }

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *connector = drmModeGetConnector(fd, res->connectors[i]);
        if (connector) {
            if (connector->connector_id == connector_id)
                ctx->connector =  drm_object_create(log, ctx->fd, connector->connector_id,
                                                    DRM_MODE_OBJECT_CONNECTOR);

            drmModeFreeConnector(connector);
            if (ctx->connector)
                break;
        }
    }

    for (unsigned int j = 0; j < plane_res->count_planes; j++) {

        drmModePlane *drmplane = drmModeGetPlane(ctx->fd, plane_res->planes[j]);
        const uint32_t possible_crtcs = drmplane->possible_crtcs;
        const uint32_t plane_id = drmplane->plane_id;
        drmModeFreePlane(drmplane);
        drmplane = NULL;

        if (possible_crtcs & (1 << crtc_index)) {
            plane = drm_object_create(log, ctx->fd, plane_id,
                                      DRM_MODE_OBJECT_PLANE);

            if (!plane) {
                mp_err(log, "Failed to create Plane object from plane ID %d\n",
                       plane_id);
                goto fail;
            }

            if (drm_object_get_property(plane, "TYPE", &value) == -EINVAL) {
                mp_err(log, "Unable to retrieve type property from plane %d\n", j);
                goto fail;
            }

            if (value != DRM_PLANE_TYPE_CURSOR) { // Skip cursor planes
                layercount++;

                if ((!primary_id) && (value == DRM_PLANE_TYPE_PRIMARY))
                    primary_id = plane_id;

                if ((!overlay_id) && (value == DRM_PLANE_TYPE_OVERLAY))
                    overlay_id = plane_id;

                if (layercount == osd_plane_idx) {
                    ctx->osd_plane = plane;
                    continue;
                }

                if (layercount == video_plane_idx) {
                    ctx->video_plane = plane;
                    continue;
                }
            }

            drm_object_free(plane);
            plane = NULL;
        }
    }

    // OSD plane was specified as either of the special options: any primary plane or any overlay plane
    if (!ctx->osd_plane) {
        const int osd_plane_id = (osd_plane_idx == DRM_OPTS_OVERLAY_PLANE) ? overlay_id : primary_id;
        const char *plane_type = (osd_plane_idx == DRM_OPTS_OVERLAY_PLANE) ? "overlay" : "primary";
        if (osd_plane_id) {
            mp_verbose(log, "Using %s plane %d for OSD\n", plane_type, osd_plane_id);
            ctx->osd_plane = drm_object_create(log, ctx->fd, osd_plane_id, DRM_MODE_OBJECT_PLANE);
        } else {
            mp_err(log, "Failed to find OSD plane with idx=%d\n", osd_plane_idx);
            goto fail;
        }
    } else {
        mp_verbose(log, "Found OSD plane with ID %d\n", ctx->osd_plane->id);
    }

    // video plane was specified as either of the special options: any primary plane or any overlay plane
    if (!ctx->video_plane) {
        const int video_plane_id = (video_plane_idx == DRM_OPTS_PRIMARY_PLANE) ? primary_id : overlay_id;
        const char *plane_type = (video_plane_idx == DRM_OPTS_PRIMARY_PLANE) ? "primary" : "overlay";

        if (video_plane_id) {
            mp_verbose(log, "Using %s plane %d for video\n", plane_type, video_plane_id);
            ctx->video_plane = drm_object_create(log, ctx->fd, video_plane_id, DRM_MODE_OBJECT_PLANE);
        } else {
            mp_err(log, "Failed to find video plane with idx=%d\n", video_plane_idx);
            goto fail;
        }
    } else {
        mp_verbose(log, "Found video plane with ID %d\n", ctx->video_plane->id);
    }

    mp_verbose(log, "Found Video plane with ID %d, OSD with ID %d\n",
               ctx->video_plane->id, ctx->osd_plane->id);

    drmModeFreePlaneResources(plane_res);
    drmModeFreeResources(res);
    return ctx;


fail:
    if (res)
        drmModeFreeResources(res);
    if (plane_res)
        drmModeFreePlaneResources(plane_res);
    if (plane)
        drm_object_free(plane);
    return NULL;
}

void drm_atomic_destroy_context(struct drm_atomic_context *ctx)
{
    drm_object_free(ctx->crtc);
    drm_object_free(ctx->connector);
    drm_object_free(ctx->osd_plane);
    drm_object_free(ctx->video_plane);
    talloc_free(ctx);
}
