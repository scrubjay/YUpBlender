/* SPDX-FileCopyrightText: 2011-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "material.hh"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usdImaging/usdImaging/materialParamUtils.h>
#include <pxr/usd/sdr/registry.h>

#ifdef WITH_MATERIALX
#  include <pxr/usd/usdMtlx/reader.h>
#endif

#include "DEG_depsgraph_query.hh"

#include "DNA_material_types.h"

#include "CLG_log.h"

#include "image.hh"
#include "scene_index.hh"

#include "intern/usd_exporter_context.hh"
#include "intern/usd_hook.hh"
#include "intern/usd_writer_material.hh"
#include "usd_private.hh"

#ifdef WITH_MATERIALX
#  include "shader/materialx/material.h"
#endif

#include <pxr/usd/sdf/assetPath.h>

namespace blender::io::hydra {

static pxr::HdContainerDataSourceHandle build_material_node_data_source(
    const pxr::HdMaterialNode2 &node, bool is_moonray)
{
  pxr::TfToken node_type_id = node.nodeTypeId;
  Map<pxr::TfToken, pxr::VtValue> parameters;
  Map<pxr::TfToken, std::vector<pxr::HdMaterialConnection2>> input_connections;

  for (const auto &param : node.parameters) {
    parameters.add(param.first, param.second);
  }
  for (const auto &input : node.inputConnections) {
    input_connections.add(input.first, input.second);
  }

  if (is_moonray) {
    if (node_type_id == pxr::TfToken("UsdUVTexture")) {
      const bool is_normal_map = parameters.contains(pxr::TfToken("bias")) ||
                                 parameters.contains(pxr::TfToken("scale"));
      if (is_normal_map) {
        node_type_id = pxr::TfToken("ImageNormalMap");
        if (parameters.contains(pxr::TfToken("bias"))) {
          parameters.pop(pxr::TfToken("bias"));
        }
        if (parameters.contains(pxr::TfToken("scale"))) {
          parameters.pop(pxr::TfToken("scale"));
        }
      }
      else {
        node_type_id = pxr::TfToken("ImageMap");
      }
      if (parameters.contains(pxr::TfToken("file"))) {
        pxr::VtValue file_val = parameters.pop(pxr::TfToken("file"));
        if (file_val.IsHolding<pxr::SdfAssetPath>()) {
          const auto &asset_path = file_val.Get<pxr::SdfAssetPath>();
          std::string path_str = asset_path.GetResolvedPath().empty() ?
                                     asset_path.GetAssetPath() :
                                     asset_path.GetResolvedPath();
          if (is_normal_map) {
            parameters.add(pxr::TfToken("tangent_space_normal_texture"), pxr::VtValue(path_str));
          }
          else {
            parameters.add(pxr::TfToken("texture"), pxr::VtValue(path_str));
          }
        }
        else {
          if (is_normal_map) {
            parameters.add(pxr::TfToken("tangent_space_normal_texture"), file_val);
          }
          else {
            parameters.add(pxr::TfToken("texture"), file_val);
          }
        }
      }
      if (input_connections.contains(pxr::TfToken("st"))) {
        input_connections.pop(pxr::TfToken("st"));
      }
      parameters.add(pxr::TfToken("texture_coordinates"), pxr::VtValue(0));
    }
    else if (node_type_id == pxr::TfToken("UsdPreviewSurface")) {
      node_type_id = pxr::TfToken("DwaBaseMaterial");
      parameters.add(pxr::TfToken("show_diffuse"), pxr::VtValue(true));
      parameters.add(pxr::TfToken("show_specular"), pxr::VtValue(true));
      if (parameters.contains(pxr::TfToken("diffuseColor"))) {
        parameters.add(pxr::TfToken("albedo"), parameters.pop(pxr::TfToken("diffuseColor")));
      }
      if (parameters.contains(pxr::TfToken("emissiveColor"))) {
        parameters.add(pxr::TfToken("emission"), parameters.pop(pxr::TfToken("emissiveColor")));
        parameters.add(pxr::TfToken("show_emission"), pxr::VtValue(true));
      }
      if (parameters.contains(pxr::TfToken("ior"))) {
        parameters.add(pxr::TfToken("refractive_index"), parameters.pop(pxr::TfToken("ior")));
      }
      if (input_connections.contains(pxr::TfToken("diffuseColor"))) {
        input_connections.add(pxr::TfToken("albedo"), input_connections.pop(pxr::TfToken("diffuseColor")));
      }
      if (input_connections.contains(pxr::TfToken("emissiveColor"))) {
        input_connections.add(pxr::TfToken("emission"), input_connections.pop(pxr::TfToken("emissiveColor")));
      }
      if (input_connections.contains(pxr::TfToken("normal"))) {
        input_connections.add(pxr::TfToken("input_normal"), input_connections.pop(pxr::TfToken("normal")));
      }
      if (input_connections.contains(pxr::TfToken("roughness"))) {
        input_connections.add(pxr::TfToken("roughness"), input_connections.pop(pxr::TfToken("roughness")));
      }
      if (input_connections.contains(pxr::TfToken("metallic"))) {
        input_connections.add(pxr::TfToken("metallic"), input_connections.pop(pxr::TfToken("metallic")));
      }
    }
  }

  /* Parameters. */
  HdContainerBuilder params;
  params.reserve(parameters.size());
  for (const auto &param : parameters.items()) {
    params.add(param.key,
               pxr::HdMaterialNodeParameterSchema::Builder()
                   .SetValue(pxr::HdRetainedSampledDataSource::New(param.value))
                   .Build());
  }

  /* Input connections. */
  HdContainerBuilder inputs;
  inputs.reserve(input_connections.size());
  for (const auto &input : input_connections.items()) {
    Vector<pxr::HdDataSourceBaseHandle> conn_values;
    conn_values.reserve(input.value.size());
    for (const pxr::HdMaterialConnection2 &conn : input.value) {
      pxr::TfToken out_name = conn.upstreamOutputName;
      if (is_moonray && (out_name == pxr::TfToken("rgb") || out_name == pxr::TfToken("color"))) {
        out_name = pxr::TfToken("color");
      }
      conn_values.append(
          pxr::HdMaterialConnectionSchema::Builder()
              .SetUpstreamNodePath(pxr::HdRetainedTypedSampledDataSource<pxr::TfToken>::New(
                  conn.upstreamNode.GetToken()))
              .SetUpstreamNodeOutputName(pxr::HdRetainedTypedSampledDataSource<pxr::TfToken>::New(
                  out_name))
              .Build());
    }
    inputs.add(input.key,
               pxr::HdRetainedSmallVectorDataSource::New(conn_values.size(), conn_values.data()));
  }

  return pxr::HdMaterialNodeSchema::Builder()
      .SetParameters(params.build())
      .SetInputConnections(inputs.build())
      .SetNodeIdentifier(pxr::HdRetainedTypedSampledDataSource<pxr::TfToken>::New(node_type_id))
      .Build();
}

static pxr::HdContainerDataSourceHandle build_material_network_data_source(
    const pxr::HdMaterialNetworkMap &network_map, bool is_moonray)
{
  const pxr::HdMaterialNetwork2 net2 = pxr::HdConvertToHdMaterialNetwork2(network_map);

  std::set<pxr::SdfPath> terminal_nodes;
  for (const auto &entry : net2.terminals) {
    terminal_nodes.insert(entry.second.upstreamNode);
  }

  /* Nodes. Insert non-terminal nodes first, and terminal (root material) nodes last
   * so that render delegates like hdMoonray that take the last node in the network
   * receive the actual root material rather than an intermediate texture map. */
  HdContainerBuilder nodes;
  nodes.reserve(net2.nodes.size());
  for (const auto &entry : net2.nodes) {
    if (terminal_nodes.find(entry.first) == terminal_nodes.end()) {
      nodes.add(entry.first.GetToken(), build_material_node_data_source(entry.second, is_moonray));
    }
  }
  for (const auto &entry : net2.nodes) {
    if (terminal_nodes.find(entry.first) != terminal_nodes.end()) {
      nodes.add(entry.first.GetToken(), build_material_node_data_source(entry.second, is_moonray));
    }
  }

  /* Terminals. */
  HdContainerBuilder terminals;
  terminals.reserve(net2.terminals.size());
  for (const auto &entry : net2.terminals) {
    terminals.add(
        entry.first,
        pxr::HdMaterialConnectionSchema::Builder()
            .SetUpstreamNodePath(pxr::HdRetainedTypedSampledDataSource<pxr::TfToken>::New(
                entry.second.upstreamNode.GetToken()))
            .SetUpstreamNodeOutputName(pxr::HdRetainedTypedSampledDataSource<pxr::TfToken>::New(
                entry.second.upstreamOutputName))
            .Build());
  }

  return pxr::HdMaterialNetworkSchema::Builder()
      .SetNodes(nodes.build())
      .SetTerminals(terminals.build())
      .Build();
}

EmittedMaterial build_emitted_material(const PopulateContext &ctx, const Material *material)
{
  /* Create temporary in memory stage to avoid collisions with other materials. */
  pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory();

  const pxr::SdfPath material_library_path("/_materials");
  const pxr::SdfPath path = ctx.material_prim_id(material);
  const pxr::SdfPath usd_material_path = material_library_path.AppendChild(
      pxr::TfToken(path.GetElementString()));

  /* Create USD export content to reuse USD file export code. */
  io::usd::USDExportParams export_params;
  export_params.relative_paths = false;
  export_params.export_textures = false; /* Don't copy all textures, is slow. */
  export_params.generate_preview_surface = true;
  export_params.generate_materialx_network = false;
  export_params.evaluation_mode = DEG_get_mode(ctx.depsgraph);

  const pxr::UsdTimeCode time = pxr::UsdTimeCode::Default();
  auto get_time_code = [time]() { return time; };

  io::usd::USDExporterContext export_context{ctx.bmain,
                                             ctx.depsgraph,
                                             stage,
                                             material_library_path,
                                             get_time_code,
                                             export_params,
                                             io::usd::image_cache_file_path(),
                                             cache_or_get_image_file};
  /* Create USD material. */
  pxr::UsdShadeMaterial usd_material;
#ifdef WITH_MATERIALX
  if (ctx.use_materialx) {
    const std::string material_name = pxr::TfMakeValidIdentifier(material->id.name);
    nodes::materialx::ExportParams materialx_export_params{
        material_name, cache_or_get_image_file, "st", "UVMap"};
    MaterialX::DocumentPtr doc = nodes::materialx::export_to_materialx(
        ctx.depsgraph, material, materialx_export_params);
    pxr::UsdMtlxRead(doc, stage);

    if (pxr::UsdPrim materials = stage->GetPrimAtPath(pxr::SdfPath("/MaterialX/Materials"))) {
      pxr::UsdPrimSiblingRange children = materials.GetChildren();
      if (!children.empty()) {
        usd_material = pxr::UsdShadeMaterial(*children.begin());
      }
    }
    if (!usd_material) {
      CLOG_WARN(LOG_HYDRA_SCENE_INDEX, "MaterialX export failed for '%s'", material->id.name + 2);
    }
    else {
      /* Fall back to UsdPreviewSurface if MaterialX did not have a valid surface,
       * matching USD export behavior. */
      const pxr::UsdShadeOutput surface = usd_material.GetSurfaceOutput();
      if (!surface || !surface.HasConnectedSource()) {
        io::usd::create_usd_viewport_material(export_context, material, usd_material);
      }
    }
  }
  else
#endif
  {
    usd_material = io::usd::create_usd_material(
        export_context, usd_material_path, material, "st", nullptr);
  }

  if (usd_material) {
    io::usd::call_material_export_hooks(stage, material, usd_material, export_params, nullptr);
  }

  EmittedMaterial entry;
  entry.path = path;
  /* Double side is a geometry level parameter in USD. */
  entry.double_sided = (material->blend_flag & MA_BL_CULL_BACKFACE) == 0;

  if (!usd_material) {
    return entry;
  }

  /* Build the material network from the surface, volume and displacement terminals. */
  std::vector<pxr::TfToken> effective_render_contexts(ctx.material_render_contexts.begin(),
                                                      ctx.material_render_contexts.end());
  bool has_universal_context = false;
  for (const auto &c : effective_render_contexts) {
    if (c.IsEmpty()) {
      has_universal_context = true;
      break;
    }
  }
  if (!has_universal_context) {
    effective_render_contexts.push_back(pxr::TfToken());
  }

  const std::vector<pxr::TfToken> source_types(ctx.shader_source_types.begin(),
                                               ctx.shader_source_types.end());
  std::vector<pxr::TfToken> effective_source_types = source_types;
  bool has_universal_source = false;
  for (const auto &s : effective_source_types) {
    if (s.IsEmpty()) {
      has_universal_source = true;
      break;
    }
  }
  if (!has_universal_source) {
    effective_source_types.push_back(pxr::TfToken());
  }

  pxr::HdMaterialNetworkMap network_map;
  pxr::UsdShadeShader surface = usd_material.ComputeSurfaceSource(effective_render_contexts);
  if (!surface) {
    surface = usd_material.ComputeSurfaceSource();
  }
  if (surface) {
    pxr::TfToken surface_id;
    surface.GetIdAttr().Get(&surface_id);
    if (!surface_id.IsEmpty() &&
        pxr::SdrRegistry::GetInstance().GetShaderNodeByIdentifier(surface_id))
    {
      pxr::UsdImagingBuildHdMaterialNetworkFromTerminal(surface.GetPrim(),
                                                        pxr::HdMaterialTerminalTokens->surface,
                                                        effective_source_types,
                                                        effective_render_contexts,
                                                        &network_map,
                                                        time);
    }
  }

  pxr::UsdShadeShader volume = usd_material.ComputeVolumeSource(effective_render_contexts);
  if (!volume) {
    volume = usd_material.ComputeVolumeSource();
  }
  if (volume) {
    pxr::TfToken volume_id;
    volume.GetIdAttr().Get(&volume_id);
    if (!volume_id.IsEmpty() &&
        pxr::SdrRegistry::GetInstance().GetShaderNodeByIdentifier(volume_id))
    {
      pxr::UsdImagingBuildHdMaterialNetworkFromTerminal(volume.GetPrim(),
                                                        pxr::HdMaterialTerminalTokens->volume,
                                                        effective_source_types,
                                                        effective_render_contexts,
                                                        &network_map,
                                                        time);
    }
  }

  pxr::UsdShadeShader displacement = usd_material.ComputeDisplacementSource(effective_render_contexts);
  if (!displacement) {
    displacement = usd_material.ComputeDisplacementSource();
  }
  if (displacement) {
    pxr::TfToken disp_id;
    displacement.GetIdAttr().Get(&disp_id);
    if (!disp_id.IsEmpty() &&
        pxr::SdrRegistry::GetInstance().GetShaderNodeByIdentifier(disp_id))
    {
      pxr::UsdImagingBuildHdMaterialNetworkFromTerminal(displacement.GetPrim(),
                                                        pxr::HdMaterialTerminalTokens->displacement,
                                                        effective_source_types,
                                                        effective_render_contexts,
                                                        &network_map,
                                                        time);
    }
  }

  if (network_map.terminals.empty() && (surface || volume || displacement)) {
    /* If UsdImagingBuildHdMaterialNetworkFromTerminal was skipped or failed because SdrRegistry
     * does not have a parser plugin registered for custom shaders (e.g. MoonRay DwaBaseMaterial, DwaRefractiveMaterial, ImageMap),
     * build the HdMaterialNetwork directly by traversing UsdShadeShader prims in the stage. */
    pxr::HdMaterialNetwork base_net;
    std::vector<std::pair<pxr::SdfPath, pxr::HdMaterialNode>> all_nodes;

    for (const pxr::UsdPrim &child : stage->Traverse()) {
      if (child.IsA<pxr::UsdShadeShader>()) {
        pxr::UsdShadeShader shader(child);
        pxr::HdMaterialNode node;
        node.path = shader.GetPath();

        pxr::TfToken id;
        if (shader.GetIdAttr().Get(&id)) {
          node.identifier = id;
        }
        else {
          node.identifier = pxr::TfToken(child.GetName().GetString());
        }

        for (const pxr::UsdShadeInput &input : shader.GetInputs()) {
          pxr::UsdShadeConnectableAPI source;
          pxr::TfToken source_name;
          pxr::UsdShadeAttributeType source_type;
          if (pxr::UsdShadeConnectableAPI::GetConnectedSource(
                  input, &source, &source_name, &source_type)) {
            pxr::HdMaterialRelationship rel;
            rel.inputId = source.GetPath();
            rel.inputName = source_name;
            rel.outputId = shader.GetPath();
            rel.outputName = input.GetBaseName();
            base_net.relationships.push_back(rel);
          }
          else {
            pxr::VtValue val;
            if (input.Get(&val, time)) {
              node.parameters[input.GetBaseName()] = val;
            }
          }
        }

        all_nodes.push_back({node.path, node});
      }
    }

    auto add_terminal_network = [&](const pxr::UsdShadeShader &term_shader,
                                    const pxr::TfToken &term_token) {
      if (!term_shader) {
        return;
      }
      pxr::HdMaterialNetwork net = base_net;
      pxr::SdfPath term_path = term_shader.GetPath();
      pxr::HdMaterialNode term_node;
      bool found_term = false;

      for (const auto &pair : all_nodes) {
        if (pair.first == term_path) {
          term_node = pair.second;
          found_term = true;
        }
        else {
          net.nodes.push_back(pair.second);
        }
      }

      if (found_term) {
        net.nodes.push_back(term_node);
      }

      if (!net.nodes.empty()) {
        network_map.terminals.push_back(term_path);
        network_map.map[term_token] = net;
      }
    };

    add_terminal_network(surface, pxr::HdMaterialTerminalTokens->surface);
    add_terminal_network(displacement, pxr::HdMaterialTerminalTokens->displacement);
    add_terminal_network(volume, pxr::HdMaterialTerminalTokens->volume);
  }

  if (network_map.terminals.empty()) {
    return entry;
  }

  bool is_moonray = false;
  for (const pxr::TfToken &c : effective_render_contexts) {
    if (c == pxr::TfToken("moonray")) {
      is_moonray = true;
      break;
    }
  }

  /* Publish the network under every render context plus the universal fallback.
   * Blender does have solid viewport material parameters but Hydra is only used for
   * the rendered display mode. */
  pxr::HdContainerDataSourceHandle network_ds = build_material_network_data_source(
      network_map, is_moonray);
  HdContainerBuilder b;
  b.reserve(effective_render_contexts.size() + 1);
  for (const pxr::TfToken &c : effective_render_contexts) {
    if (!c.IsEmpty()) {
      b.add(c, network_ds);
    }
  }
  b.add(pxr::HdMaterialSchemaTokens->universalRenderContext, network_ds);
  entry.data_source = pxr::HdRetainedContainerDataSource::New(
      pxr::HdMaterialSchema::GetSchemaToken(), b.build());
  return entry;
}

}  // namespace blender::io::hydra
