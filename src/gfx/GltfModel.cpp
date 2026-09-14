#include "gfx/GltfModel.h"
#include "core/Log.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <stb_image.h>
#include <webp/decode.h>

#include <draco/compression/decode.h>
#include <draco/core/decoder_buffer.h>
#include <draco/mesh/mesh.h>

#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace space::gfx {

namespace {

struct LoadContext {
    const cgltf_data* data = nullptr;
    std::filesystem::path baseDir; // external image URIs are relative to the model file
    std::string name;              // for log messages
    ModelData* out = nullptr;
    // Distinct glTF images to decode, and the (source, sRGB) pairs that become ModelData::images entries.
    std::unordered_map<const cgltf_image*, int> sourceIndex;
    std::vector<const cgltf_image*> sources;
    std::map<std::pair<int, bool>, int> entryIndex;
    std::vector<std::pair<int, bool>> entries;
    size_t firstPrimitive = 0;
    bool warnedTexcoord = false, warnedBasisu = false;
};

// Registers the texture of a view and returns a provisional entry index (remapped once decoded), -1 = none.
int textureRef(LoadContext& ctx, const cgltf_texture_view& view, bool srgb) {
    const cgltf_texture* tex = view.texture;
    if (!tex) return -1;
    const int texcoord = view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord;
    if (texcoord != 0) {
        if (!ctx.warnedTexcoord)
            LOG_WARN("Model {}: a texture uses TEXCOORD_{}; only TEXCOORD_0 is supported, ignoring it", ctx.name, texcoord);
        ctx.warnedTexcoord = true;
        return -1;
    }
    const cgltf_image* img = tex->has_webp && tex->webp_image ? tex->webp_image : tex->image;
    if (!img) {
        if (tex->has_basisu && !ctx.warnedBasisu) LOG_WARN("Model {}: KHR_texture_basisu textures are not supported", ctx.name);
        ctx.warnedBasisu = ctx.warnedBasisu || tex->has_basisu;
        return -1;
    }
    auto [srcIt, newSource] = ctx.sourceIndex.try_emplace(img, (int)ctx.sources.size());
    if (newSource) ctx.sources.push_back(img);
    const std::pair<int, bool> key{srcIt->second, srgb};
    auto [entryIt, newEntry] = ctx.entryIndex.try_emplace(key, (int)ctx.entries.size());
    if (newEntry) ctx.entries.push_back(key);
    return entryIt->second;
}

void appendPrimitive(const cgltf_primitive& prim, const glm::mat4& world, LoadContext& ctx) {
    if (prim.type != cgltf_primitive_type_triangles) return;
    ModelData& out = *ctx.out;
    const cgltf_accessor* posAcc = nullptr;
    const cgltf_accessor* nrmAcc = nullptr;
    const cgltf_accessor* uvAcc = nullptr;
    for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
        const auto& attr = prim.attributes[a];
        if (attr.type == cgltf_attribute_type_position) posAcc = attr.data;
        else if (attr.type == cgltf_attribute_type_normal) nrmAcc = attr.data;
        else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uvAcc = attr.data;
    }
    if (!posAcc) return;

    const uint32_t baseVertex = (uint32_t)out.vertices.size();
    const glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(world)));
    // A mirroring transform (negative determinant, e.g. the HD ISS root scale of -0.0026) flips the winding.
    const bool mirrored = glm::determinant(glm::mat3(world)) < 0.f;
    ModelPrimitive mp;
    mp.firstIndex = (uint32_t)out.indices.size();

    if (prim.has_draco_mesh_compression) {
        // KHR_draco_mesh_compression: the accessors are placeholders; decode the Draco blob instead.
        const auto& dc = prim.draco_mesh_compression;
        const auto* bv = dc.buffer_view;
        const uint8_t* bytes = static_cast<const uint8_t*>(bv->buffer->data) + bv->offset;
        draco::DecoderBuffer dbuf;
        dbuf.Init(reinterpret_cast<const char*>(bytes), bv->size);
        draco::Decoder decoder;
        auto result = decoder.DecodeMeshFromBuffer(&dbuf);
        if (!result.ok()) return;
        std::unique_ptr<draco::Mesh> mesh = std::move(result).value();

        // Draco attribute ids arrive through cgltf as fake accessor pointers; recover the integer.
        auto dracoId = [&](cgltf_attribute_type type) -> int {
            for (cgltf_size a = 0; a < dc.attributes_count; ++a)
                if (dc.attributes[a].type == type &&
                    (type != cgltf_attribute_type_texcoord || dc.attributes[a].index == 0))
                    return (int)(dc.attributes[a].data - ctx.data->accessors);
            return -1;
        };
        const int pid = dracoId(cgltf_attribute_type_position);
        const int nid = dracoId(cgltf_attribute_type_normal);
        const int tid = dracoId(cgltf_attribute_type_texcoord);
        const draco::PointAttribute* pa = pid >= 0 ? mesh->GetAttributeByUniqueId(pid) : nullptr;
        const draco::PointAttribute* na = nid >= 0 ? mesh->GetAttributeByUniqueId(nid) : nullptr;
        const draco::PointAttribute* ta = tid >= 0 ? mesh->GetAttributeByUniqueId(tid) : nullptr;
        if (!pa) return;
        const uint32_t count = mesh->num_points();
        for (uint32_t i = 0; i < count; ++i) {
            ModelVertex v{};
            float p[3] = {0, 0, 0}, n[3] = {0, 1, 0}, t[2] = {0, 0};
            pa->ConvertValue<float, 3>(pa->mapped_index(draco::PointIndex(i)), p);
            if (na) na->ConvertValue<float, 3>(na->mapped_index(draco::PointIndex(i)), n);
            if (ta) ta->ConvertValue<float, 2>(ta->mapped_index(draco::PointIndex(i)), t);
            v.position = glm::vec3(world * glm::vec4(p[0], p[1], p[2], 1.f));
            v.normal = glm::normalize(normalMat * glm::vec3(n[0], n[1], n[2]));
            v.uv = glm::vec2(t[0], t[1]);
            out.vertices.push_back(v);
        }
        for (draco::FaceIndex f(0); f < mesh->num_faces(); ++f) {
            const auto& face = mesh->face(f);
            out.indices.push_back(baseVertex + face[0].value());
            out.indices.push_back(baseVertex + face[mirrored ? 2 : 1].value());
            out.indices.push_back(baseVertex + face[mirrored ? 1 : 2].value());
        }
        mp.indexCount = (uint32_t)mesh->num_faces() * 3;
    } else {
        for (cgltf_size i = 0; i < posAcc->count; ++i) {
            ModelVertex v{};
            float p[3] = {0, 0, 0}, n[3] = {0, 1, 0}, t[2] = {0, 0};
            cgltf_accessor_read_float(posAcc, i, p, 3);
            if (nrmAcc) cgltf_accessor_read_float(nrmAcc, i, n, 3);
            if (uvAcc) cgltf_accessor_read_float(uvAcc, i, t, 2);
            v.position = glm::vec3(world * glm::vec4(p[0], p[1], p[2], 1.f));
            v.normal = glm::normalize(normalMat * glm::vec3(n[0], n[1], n[2]));
            v.uv = glm::vec2(t[0], t[1]);
            out.vertices.push_back(v);
        }
        if (prim.indices) {
            for (cgltf_size i = 0; i < prim.indices->count; ++i)
                out.indices.push_back(baseVertex + (uint32_t)cgltf_accessor_read_index(prim.indices, i));
            mp.indexCount = (uint32_t)prim.indices->count;
        } else {
            for (cgltf_size i = 0; i < posAcc->count; ++i) out.indices.push_back(baseVertex + (uint32_t)i);
            mp.indexCount = (uint32_t)posAcc->count;
        }
        if (mirrored)
            for (size_t i = mp.firstIndex; i + 2 < out.indices.size(); i += 3) std::swap(out.indices[i + 1], out.indices[i + 2]);
    }

    if (prim.material) {
        const cgltf_material* m = prim.material;
        if (m->has_pbr_metallic_roughness) {
            const auto& pbr = m->pbr_metallic_roughness;
            mp.baseColor = glm::make_vec4(pbr.base_color_factor);
            mp.metallic = pbr.metallic_factor;
            mp.roughness = pbr.roughness_factor;
            mp.imageIndex = textureRef(ctx, pbr.base_color_texture, true);
            if (mp.imageIndex >= 0 && pbr.base_color_texture.has_transform) {
                const auto& tt = pbr.base_color_texture.transform;
                mp.uvTransform = glm::vec4(tt.offset[0], tt.offset[1], tt.scale[0], tt.scale[1]);
            }
            mp.mrImageIndex = textureRef(ctx, pbr.metallic_roughness_texture, false);
        } else if (m->has_pbr_specular_glossiness) {
            mp.baseColor = glm::make_vec4(m->pbr_specular_glossiness.diffuse_factor);
        }
        mp.normalImageIndex = textureRef(ctx, m->normal_texture, false);
        if (mp.normalImageIndex >= 0) mp.normalScale = m->normal_texture.scale;
        mp.occlusionImageIndex = textureRef(ctx, m->occlusion_texture, false);
        if (mp.occlusionImageIndex >= 0) mp.occlusionStrength = m->occlusion_texture.scale;
        mp.emissiveImageIndex = textureRef(ctx, m->emissive_texture, true);
        mp.emissiveFactor = glm::make_vec3(m->emissive_factor);
        if (m->has_emissive_strength) mp.emissiveFactor *= m->emissive_strength.emissive_strength;
    }
    out.primitives.push_back(mp);
}

void walkNode(const cgltf_node* node, const glm::mat4& parent, LoadContext& ctx) {
    float local[16];
    cgltf_node_transform_local(node, local);
    const glm::mat4 world = parent * glm::make_mat4(local);
    if (node->mesh)
        for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) appendPrimitive(node->mesh->primitives[p], world, ctx);
    for (cgltf_size c = 0; c < node->children_count; ++c) walkNode(node->children[c], world, ctx);
}

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

bool decodeBase64(std::string_view s, std::vector<uint8_t>& out) {
    out.reserve(s.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        const int v = base64Value(c);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xFF));
        }
    }
    return true;
}

int hexValue(char h) {
    if (h >= '0' && h <= '9') return h - '0';
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
    return -1;
}

// glTF URIs are percent-encoded; the decoded bytes are UTF-8.
std::string decodePercent(std::string_view uri) {
    std::string s;
    s.reserve(uri.size());
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size() && hexValue(uri[i + 1]) >= 0 && hexValue(uri[i + 2]) >= 0) {
            s.push_back((char)(hexValue(uri[i + 1]) * 16 + hexValue(uri[i + 2])));
            i += 2;
        } else {
            s.push_back(uri[i]);
        }
    }
    return s;
}

// Thread-safe: reads the encoded bytes (buffer view, data: URI or external file) and decodes them to RGBA8.
DecodedImage decodeSource(const cgltf_image* img, const std::filesystem::path& baseDir, const std::string& name) {
    DecodedImage di;
    const bool dataUri = img->uri && std::strncmp(img->uri, "data:", 5) == 0;
    const std::string label = img->name ? img->name : (img->uri && !dataUri ? img->uri : "(embedded)");
    std::vector<uint8_t> owned;
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    if (img->buffer_view) {
        const auto* bv = img->buffer_view;
        if (!bv->buffer->data) return di;
        bytes = static_cast<const uint8_t*>(bv->buffer->data) + bv->offset;
        size = bv->size;
    } else if (dataUri) {
        const std::string_view uri(img->uri);
        const size_t comma = uri.find(',');
        if (comma == std::string_view::npos || uri.substr(0, comma).find(";base64") == std::string_view::npos ||
            !decodeBase64(uri.substr(comma + 1), owned)) {
            LOG_WARN("Model {}: image {} has an unsupported data URI", name, label);
            return di;
        }
    } else if (img->uri) {
        const std::string rel = decodePercent(img->uri);
        const std::filesystem::path file = baseDir / std::filesystem::path(std::u8string(rel.begin(), rel.end()));
        std::ifstream f(file, std::ios::binary | std::ios::ate);
        if (!f) {
            LOG_WARN("Model {}: image {} not found", name, file.string());
            return di;
        }
        owned.resize((size_t)f.tellg());
        f.seekg(0);
        f.read(reinterpret_cast<char*>(owned.data()), (std::streamsize)owned.size());
        if (!f) owned.clear();
    }
    if (!bytes) {
        bytes = owned.data();
        size = owned.size();
    }
    if (!bytes || size == 0) {
        LOG_WARN("Model {}: image {} has no data", name, label);
        return di;
    }

    int w = 0, h = 0, comp = 0;
    const bool isWebp = size > 12 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "WEBP", 4) == 0;
    if (isWebp) {
        if (uint8_t* px = WebPDecodeRGBA(bytes, size, &w, &h)) {
            di.width = w;
            di.height = h;
            di.rgba.assign(px, px + (size_t)w * h * 4);
            WebPFree(px);
        }
    } else if (size <= (size_t)INT_MAX) {
        if (stbi_uc* px = stbi_load_from_memory(bytes, (int)size, &w, &h, &comp, 4)) {
            di.width = w;
            di.height = h;
            di.rgba.assign(px, px + (size_t)w * h * 4);
            stbi_image_free(px);
        }
    }
    if (!di.ok()) LOG_WARN("Model {}: image {} failed to decode", name, label);
    return di;
}

// Decodes every distinct source image once, in parallel, then builds ModelData::images from the (source, sRGB)
// entries and patches the primitives' provisional entry indices into final image indices (-1 if decoding failed).
void resolveImages(LoadContext& ctx) {
    const size_t count = ctx.sources.size();
    std::vector<DecodedImage> decoded(count);
    if (count > 0) {
        // A bounded number of workers keeps peak memory in check (the HD ISS has 100+ textures).
        const size_t workers = std::min<size_t>(count, std::max(1u, std::thread::hardware_concurrency()));
        std::atomic<size_t> next{0};
        std::vector<std::future<void>> jobs;
        jobs.reserve(workers);
        for (size_t w = 0; w < workers; ++w)
            jobs.push_back(std::async(std::launch::async, [&] {
                for (size_t i = next++; i < count; i = next++) decoded[i] = decodeSource(ctx.sources[i], ctx.baseDir, ctx.name);
            }));
        for (auto& j : jobs) j.get();
    }

    ModelData& out = *ctx.out;
    std::vector<int> usesLeft(count, 0);
    for (const auto& e : ctx.entries) ++usesLeft[(size_t)e.first];
    std::vector<int> remap(ctx.entries.size(), -1);
    for (size_t e = 0; e < ctx.entries.size(); ++e) {
        const auto [src, srgb] = ctx.entries[e];
        DecodedImage& di = decoded[(size_t)src];
        const bool last = --usesLeft[(size_t)src] == 0;
        if (!di.ok()) continue;
        // Copy only when one image is used both as colour and as data; the last use takes the pixels.
        DecodedImage img = last ? std::move(di) : di;
        img.srgb = srgb;
        out.images.push_back(std::move(img));
        remap[e] = (int)out.images.size() - 1;
    }
    for (size_t p = ctx.firstPrimitive; p < out.primitives.size(); ++p) {
        ModelPrimitive& mp = out.primitives[p];
        for (int* idx : {&mp.imageIndex, &mp.normalImageIndex, &mp.mrImageIndex, &mp.occlusionImageIndex, &mp.emissiveImageIndex})
            if (*idx >= 0) *idx = remap[(size_t)*idx];
    }
}

size_t distinctImages(const std::vector<ModelPrimitive>& prims, int ModelPrimitive::*field) {
    std::vector<int> seen;
    for (const auto& mp : prims)
        if (mp.*field >= 0 && std::find(seen.begin(), seen.end(), mp.*field) == seen.end()) seen.push_back(mp.*field);
    return seen.size();
}

} // namespace

bool loadModel(const std::filesystem::path& path, ModelData& out) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    const std::string p = path.string();
    const std::string name = path.filename().string();
    if (cgltf_parse_file(&options, p.c_str(), &data) != cgltf_result_success) {
        LOG_WARN("Model {}: parse failed", p);
        return false;
    }
    // Resolves the GLB BIN chunk, external .bin files (relative to the .gltf) and data: URIs.
    if (cgltf_load_buffers(&options, data, p.c_str()) != cgltf_result_success) {
        LOG_WARN("Model {}: buffers failed", p);
        cgltf_free(data);
        return false;
    }
    static constexpr const char* kSupported[] = {"KHR_draco_mesh_compression", "EXT_texture_webp", "KHR_texture_transform",
                                                 "KHR_materials_emissive_strength", "KHR_mesh_quantization"};
    for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
        const char* ext = data->extensions_required[i];
        if (std::none_of(std::begin(kSupported), std::end(kSupported), [&](const char* s) { return std::strcmp(s, ext) == 0; }))
            LOG_WARN("Model {}: required extension {} is not supported", name, ext);
    }

    LoadContext ctx;
    ctx.data = data;
    ctx.baseDir = path.parent_path();
    ctx.name = name;
    ctx.out = &out;
    ctx.firstPrimitive = out.primitives.size();
    const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
    if (scene) {
        for (cgltf_size n = 0; n < scene->nodes_count; ++n) walkNode(scene->nodes[n], glm::mat4(1.f), ctx);
    } else {
        for (cgltf_size n = 0; n < data->nodes_count; ++n)
            if (!data->nodes[n].parent) walkNode(&data->nodes[n], glm::mat4(1.f), ctx);
    }
    resolveImages(ctx); // embedded images live in the cgltf buffers, so decode before cgltf_free
    cgltf_free(data);

    if (out.vertices.empty()) {
        LOG_WARN("Model {}: no triangles", p);
        return false;
    }
    out.boundsMin = out.boundsMax = out.vertices[0].position;
    for (const auto& v : out.vertices) {
        out.boundsMin = glm::min(out.boundsMin, v.position);
        out.boundsMax = glm::max(out.boundsMax, v.position);
    }
    LOG_INFO("Model {}: {} verts, {} tris, {} prims, {} textures ({} normal, {} MR, {} AO maps), extent {:.2f}", name,
             out.vertices.size(), out.indices.size() / 3, out.primitives.size(), out.images.size(),
             distinctImages(out.primitives, &ModelPrimitive::normalImageIndex),
             distinctImages(out.primitives, &ModelPrimitive::mrImageIndex),
             distinctImages(out.primitives, &ModelPrimitive::occlusionImageIndex), out.extent());
    return true;
}

} // namespace space::gfx
