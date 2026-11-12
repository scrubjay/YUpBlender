/* SPDX-FileCopyrightText: 2011-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "light.hh"

#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/usd/usdLux/tokens.h>

#include "BLI_math_constants.hh"
#include "BLI_utildefines.hh"

#include "DNA_ID.h"
#include "DNA_light_types.h"
#include "DNA_object_types.h"
#include "DNA_view3d_types.h"

#include "BKE_idprop.hh"

#include "populate_context.hh"
#include "util.hh"

namespace blender::io::hydra {

/* Hydra prim type for a Blender light's type/shape. */
static pxr::TfToken light_prim_type(const Light *light)
{
  switch (light->type) {
    case LA_AREA:
      switch (light->area_shape) {
        case LA_AREA_SQUARE:
        case LA_AREA_RECT:
          return pxr::HdPrimTypeTokens->rectLight;
        case LA_AREA_DISK:
        case LA_AREA_ELLIPSE:
          return pxr::HdPrimTypeTokens->diskLight;
        default:
          return pxr::HdPrimTypeTokens->rectLight;
      }
    case LA_LOCAL:
    case LA_SPOT:
      return pxr::HdPrimTypeTokens->sphereLight;
    case LA_SUN:
      return pxr::HdPrimTypeTokens->distantLight;
    default:
      BLI_assert_unreachable();
  }
  return pxr::TfToken();
}

/* Build the #HdLightSchema container with the light's parameters. */
static pxr::HdContainerDataSourceHandle build_light_params_data_source(const Object *object)
{
  const Light *light = id_cast<const Light *>(object->data);

  HdContainerBuilder b;

  switch (light->type) {
    case LA_AREA:
      switch (light->area_shape) {
        case LA_AREA_SQUARE:
          b.add(pxr::HdLightTokens->width, light->area_size);
          b.add(pxr::HdLightTokens->height, light->area_size);
          break;
        case LA_AREA_RECT:
          b.add(pxr::HdLightTokens->width, light->area_size);
          b.add(pxr::HdLightTokens->height, light->area_sizey);
          break;
        case LA_AREA_DISK:
          b.add(pxr::HdLightTokens->radius, light->area_size / 2.0f);
          break;
        case LA_AREA_ELLIPSE:
          /* An ellipse light deteriorates into a disk light. */
          b.add(pxr::HdLightTokens->radius, (light->area_size + light->area_sizey) / 4.0f);
          break;
      }
      break;
    case LA_LOCAL:
    case LA_SPOT:
      b.add(pxr::HdLightTokens->radius, light->radius);
      if (light->radius == 0.0f) {
        b.add(pxr::UsdLuxTokens->treatAsPoint, true);
      }
      if (light->type == LA_SPOT) {
        b.add(pxr::UsdLuxTokens->inputsShapingConeAngle, RAD2DEGF(light->spotsize * 0.5f));
        b.add(pxr::UsdLuxTokens->inputsShapingConeSoftness, light->spotblend);
      }
      break;
    case LA_SUN:
      b.add(pxr::HdLightTokens->angle, RAD2DEGF(light->sun_angle * 0.5f));
      break;
  }

  float intensity;
  if (light->type == LA_SUN) {
    /* Unclear why, but approximately matches Karma. */
    intensity = light->energy / 4.0f;
  }
  else {
    /* Convert from radiant flux to intensity. */
    intensity = light->energy / M_PI;
  }

  b.add(pxr::HdLightTokens->color, pxr::GfVec3f(light->r, light->g, light->b));
  b.add(pxr::HdLightTokens->enableColorTemperature, (light->mode & LA_USE_TEMPERATURE) != 0);
  b.add(pxr::HdLightTokens->colorTemperature, light->temperature);
  b.add(pxr::HdLightTokens->intensity, intensity);
  b.add(pxr::HdLightTokens->exposure, light->exposure);
  b.add(pxr::HdLightTokens->diffuse, light->diff_fac);
  b.add(pxr::HdLightTokens->specular, light->spec_fac);
  b.add(pxr::HdLightTokens->normalize, (light->mode & LA_UNNORMALIZED) == 0);

  if (light->id.properties) {
    for (const IDProperty *prop = light->id.properties->data.group.first();
         prop;
         prop = prop->next)
    {
      pxr::TfToken name(prop->name);
      switch (prop->type) {
        case IDP_INT:
          b.add(name, IDP_int_get(prop));
          break;
        case IDP_FLOAT:
          b.add(name, IDP_float_get(prop));
          break;
        case IDP_DOUBLE:
          b.add(name, float(IDP_double_get(prop)));
          break;
        case IDP_BOOLEAN:
          b.add(name, bool(IDP_bool_get(prop)));
          break;
        case IDP_STRING:
          b.add(name, pxr::TfToken(IDP_string_get(prop)));
          break;
        case IDP_ARRAY:
          if (prop->data.pointer) {
            if (prop->len == 2) {
              if (prop->subtype == IDP_FLOAT) {
                const float *f = static_cast<const float *>(prop->data.pointer);
                b.add(name, pxr::GfVec2f(f[0], f[1]));
              }
              else if (prop->subtype == IDP_DOUBLE) {
                const double *d = static_cast<const double *>(prop->data.pointer);
                b.add(name, pxr::GfVec2f(float(d[0]), float(d[1])));
              }
            }
            else if (prop->len == 3) {
              if (prop->subtype == IDP_FLOAT) {
                const float *f = static_cast<const float *>(prop->data.pointer);
                b.add(name, pxr::GfVec3f(f[0], f[1], f[2]));
              }
              else if (prop->subtype == IDP_DOUBLE) {
                const double *d = static_cast<const double *>(prop->data.pointer);
                b.add(name, pxr::GfVec3f(float(d[0]), float(d[1]), float(d[2])));
              }
            }
            else if (prop->len == 4) {
              if (prop->subtype == IDP_FLOAT) {
                const float *f = static_cast<const float *>(prop->data.pointer);
                b.add(name, pxr::GfVec4f(f[0], f[1], f[2], f[3]));
              }
              else if (prop->subtype == IDP_DOUBLE) {
                const double *d = static_cast<const double *>(prop->data.pointer);
                b.add(name, pxr::GfVec4f(float(d[0]), float(d[1]), float(d[2]), float(d[3])));
              }
            }
            else if (prop->len == 1) {
              if (prop->subtype == IDP_FLOAT) {
                b.add(name, static_cast<const float *>(prop->data.pointer)[0]);
              }
              else if (prop->subtype == IDP_DOUBLE) {
                b.add(name, float(static_cast<const double *>(prop->data.pointer)[0]));
              }
              else if (prop->subtype == IDP_INT) {
                b.add(name, static_cast<const int *>(prop->data.pointer)[0]);
              }
            }
          }
          break;
        default:
          break;
      }
    }
  }

  return b.build();
}

pxr::HdContainerDataSourceHandle build_light_prim_data_source(
    const pxr::HdContainerDataSourceHandle &light_params,
    const pxr::GfMatrix4d &transform,
    const bool visible)
{
  pxr::HdContainerDataSourceHandle xform =
      pxr::HdXformSchema::Builder()
          .SetMatrix(pxr::HdRetainedTypedSampledDataSource<pxr::GfMatrix4d>::New(transform))
          .SetResetXformStack(pxr::HdRetainedTypedSampledDataSource<bool>::New(false))
          .Build();
  pxr::HdContainerDataSourceHandle visibility =
      pxr::HdVisibilitySchema::Builder()
          .SetVisibility(pxr::HdRetainedTypedSampledDataSource<bool>::New(visible))
          .Build();

  HdContainerBuilder b;
  b.add(pxr::HdLightSchema::GetSchemaToken(), light_params);
  b.add(pxr::HdXformSchema::GetSchemaToken(), xform);
  b.add(pxr::HdVisibilitySchema::GetSchemaToken(), visibility);
  return b.build();
}

void emit_light_object(PopulateContext &ctx, const Object *object, EmittedObject &emitted)
{
  if (ctx.view3d && !V3D_USES_SCENE_LIGHTS(ctx.view3d)) {
    /* Lookdev mode where the user disabled scene lights. */
    return;
  }
  const pxr::SdfPath path = ctx.object_prim_id(object);
  const pxr::TfToken type = light_prim_type(id_cast<const Light *>(object->data));
  pxr::HdContainerDataSourceHandle params = build_light_params_data_source(object);
  pxr::HdContainerDataSourceHandle prim_ds = build_light_prim_data_source(
      params, gf_matrix_from_transform(object->object_to_world().ptr()), true);
  ctx.emit_object_prim(emitted, path, type, prim_ds);
}

void emit_light_dupli(PopulateContext &ctx, const Object *source, const float dupli_mat[4][4])
{
  if (ctx.view3d && !V3D_USES_SCENE_LIGHTS(ctx.view3d)) {
    return;
  }
  const int idx = ctx.nonmesh_instance_count.lookup_default(source, 0);
  ctx.nonmesh_instance_count.add_overwrite(source, idx + 1);
  const pxr::SdfPath path = ctx.instance_clone_prim_id(source, idx);
  const pxr::TfToken type = light_prim_type(id_cast<const Light *>(source->data));
  pxr::HdContainerDataSourceHandle params = build_light_params_data_source(source);
  pxr::HdContainerDataSourceHandle prim_ds = build_light_prim_data_source(
      params, gf_matrix_from_transform(dupli_mat), true);
  ctx.emit_prim(path, type, prim_ds);
}

}  // namespace blender::io::hydra
