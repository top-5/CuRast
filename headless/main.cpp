// Linux/headless integration for the CuRast visibility core. AGPL-3.0, as src/.
#include <cuda.h>
#include <cuda_runtime.h>
#include <nvrtc.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
// HostDeviceInterface uses the global min/max names provided by CUDA in NVRTC.
using std::min;
using std::max;
#include "kernels/HostDeviceInterface.h"
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/math.hpp>
#include "json/json.hpp"

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr uint64_t clearPixel = 0xfffffff000000000ull; // resolve.cu:kernel_clearFramebuffer
constexpr uint64_t idMask = (uint64_t(1) << 33) - 1;

void check(CUresult result) {
    if (result != CUDA_SUCCESS) {
        const char* text = nullptr;
        cuGetErrorString(result, &text);
        throw std::runtime_error(text ? text : "CUDA driver failure");
    }
}
void check(cudaError_t result) {
    if (result != cudaSuccess) throw std::runtime_error(cudaGetErrorString(result));
}
void check(nvrtcResult result) {
    if (result != NVRTC_SUCCESS) throw std::runtime_error(nvrtcGetErrorString(result));
}
std::string readText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
struct Options {
    fs::path input, output, report, dump;
    int resolution = 512, repeat = 31, device = 0;
    bool selfTest = false, help = false;
};
Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") { options.help = true; continue; }
        if (arg == "--self-test") { options.selfTest = true; continue; }
        if (++i >= argc) throw std::runtime_error("Missing value for " + arg);
        const std::string value = argv[i];
        if (arg == "--input") options.input = value;
        else if (arg == "--output") options.output = value;
        else if (arg == "--report") options.report = value;
        else if (arg == "--dump") options.dump = value;
        else if (arg == "--resolution") options.resolution = std::stoi(value);
        else if (arg == "--repeat") options.repeat = std::stoi(value);
        else if (arg == "--device") options.device = std::stoi(value);
        else throw std::runtime_error("Unknown argument " + arg);
    }
    if (options.help) return options;
    if (options.input.empty() == !options.selfTest)
        throw std::runtime_error("Choose exactly one of --input static.glb or --self-test");
    if (options.resolution < 64 || options.resolution > 4096 || options.repeat < 1 || options.repeat > 1000)
        throw std::runtime_error("Resolution must be64..4096 and repeat1..1000");
    if (options.selfTest && options.resolution != 512)
        throw std::runtime_error("The three-stage fixture requires resolution512");
    for (const auto& output : {options.output, options.report, options.dump}) {
        if (!output.empty() && !options.input.empty() && fs::absolute(output).lexically_normal() == fs::absolute(options.input).lexically_normal())
            throw std::runtime_error("Output must not overwrite input");
    }
    return options;
}
struct HostMesh {
    std::vector<fastgltf::math::fvec3> positions;
    std::vector<uint32_t> indices;
    glm::mat4 world{1.f};
};
struct Scene {
    std::vector<HostMesh> meshes;
    glm::vec3 minimum{std::numeric_limits<float>::infinity()};
    glm::vec3 maximum{-std::numeric_limits<float>::infinity()};
    bool fixture = false;
};
Scene fixture() {
    Scene scene;
    scene.fixture = true;
    HostMesh mesh;
    const float f = 1.f / std::tan(glm::radians(45.f) / 2.f);
    for (const auto& triangle : std::array<std::array<glm::vec2, 3>, 3>{{
        {{{30,30},{36,30},{30,36}}},
        {{{60,60},{100,60},{60,100}}},
        {{{140,140},{450,140},{140,450}}}
    }}) {
        for (const auto& point : triangle) {
            const glm::vec2 ndc = point / 512.f * 2.f - 1.f;
            mesh.positions.push_back({ndc.x * 2.f / f, ndc.y * 2.f / f, -2.f});
            mesh.indices.push_back(uint32_t(mesh.indices.size()));
        }
    }
    scene.meshes.push_back(std::move(mesh));
    return scene;
}
Scene loadScene(const fs::path& path) {
    auto file = fastgltf::MappedGltfFile::FromPath(path);
    if (!file) throw std::runtime_error("Cannot map input GLB");
    fastgltf::Parser parser;
    auto loaded = parser.loadGltfBinary(file.get(), path.parent_path());
    if (!loaded) throw std::runtime_error(std::string(fastgltf::getErrorMessage(loaded.error())));
    auto& asset = loaded.get();
    if (!asset.skins.empty() || !asset.animations.empty())
        throw std::runtime_error("Visibility runner accepts static meshes; skinning/animation is not implemented");
    if (asset.scenes.empty()) throw std::runtime_error("GLB has no scene");
    Scene scene;
    fastgltf::iterateSceneNodes(asset, asset.defaultScene.value_or(0), fastgltf::math::fmat4x4(),
        [&](fastgltf::Node& node, const fastgltf::math::fmat4x4& transform) {
            if (!node.meshIndex) return;
            glm::mat4 world;
            static_assert(sizeof(world) == sizeof(transform));
            std::memcpy(&world, &transform, sizeof(world));
            for (const auto& primitive : asset.meshes[*node.meshIndex].primitives) {
                if (primitive.type != fastgltf::PrimitiveType::Triangles || !primitive.targets.empty())
                    throw std::runtime_error("Only static triangle primitives without morph targets are supported");
                if (primitive.materialIndex && asset.materials[*primitive.materialIndex].alphaMode != fastgltf::AlphaMode::Opaque)
                    throw std::runtime_error("MASK/BLEND materials require the viewer's separate material pipeline");
                const auto position = primitive.findAttribute("POSITION");
                if (position == primitive.attributes.end()) throw std::runtime_error("POSITION is required");
                const auto& accessor = asset.accessors[position->accessorIndex];
                if (accessor.type != fastgltf::AccessorType::Vec3 || !accessor.min || !accessor.max)
                    throw std::runtime_error("POSITION must supply VEC3 min/max bounds");
                HostMesh mesh;
                mesh.world = world;
                mesh.positions.resize(accessor.count);
                fastgltf::copyFromAccessor<fastgltf::math::fvec3>(asset, accessor, mesh.positions.data());
                if (primitive.indicesAccessor) {
                    const auto& indices = asset.accessors[*primitive.indicesAccessor];
                    mesh.indices.resize(indices.count);
                    fastgltf::copyFromAccessor<uint32_t>(asset, indices, mesh.indices.data());
                } else {
                    mesh.indices.resize(accessor.count);
                    std::iota(mesh.indices.begin(), mesh.indices.end(), 0u);
                }
                if (mesh.indices.empty() || mesh.indices.size() % 3)
                    throw std::runtime_error("Invalid triangle index count");
                if (*std::max_element(mesh.indices.begin(), mesh.indices.end()) >= mesh.positions.size())
                    throw std::runtime_error("Triangle index outside POSITION");
                glm::vec3 lo, hi;
                for (int axis = 0; axis < 3; ++axis) {
                    lo[axis] = float(accessor.min->get<double>(axis));
                    hi[axis] = float(accessor.max->get<double>(axis));
                }
                // Eight metadata-bound corners per primitive; vertex transforms remain on GPU.
                for (int corner = 0; corner < 8; ++corner) {
                    const glm::vec3 p = glm::vec3(world * glm::vec4(
                        corner & 1 ? hi.x : lo.x, corner & 2 ? hi.y : lo.y, corner & 4 ? hi.z : lo.z, 1.f));
                    scene.minimum = glm::min(scene.minimum, p);
                    scene.maximum = glm::max(scene.maximum, p);
                }
                scene.meshes.push_back(std::move(mesh));
            }
        });
    if (scene.meshes.empty()) throw std::runtime_error("No renderable triangles");
    return scene;
}
struct Allocations {
    std::vector<void*> pointers;
    ~Allocations() { for (auto p : pointers) cudaFree(p); }
    template<class T> T* allocate(size_t count, const T* source = nullptr) {
        T* result = nullptr;
        check(cudaMalloc(&result, count * sizeof(T)));
        pointers.push_back(result);
        if (source) check(cudaMemcpy(result, source, count * sizeof(T), cudaMemcpyHostToDevice));
        else check(cudaMemset(result, 0, count * sizeof(T)));
        return result;
    }
};
struct Module {
    CUmodule value = nullptr;
    double compileSeconds = 0;
    Module(const cudaDeviceProp& device) {
        const std::string source = readText(fs::path(CURAST_SOURCE_DIR) / "src/kernels/triangles_visbuffer.cu");
        nvrtcProgram program;
        check(nvrtcCreateProgram(&program, source.c_str(), "triangles_visbuffer.cu", 0, nullptr, nullptr));
        const auto include = fs::path(CURAST_CUDA_INCLUDE);
        const auto root = fs::path(CURAST_SOURCE_DIR);
        std::vector<std::string> options = {"--gpu-architecture=compute_" + std::to_string(device.major) + std::to_string(device.minor),
            "--std=c++20", "-default-device", "--device-c", "--use_fast_math", "--disable-warnings",
            "-DCURAST_HEADLESS_QUEUE_GUARDS"};
        for (const auto& p : {include / "cccl/cuda/std", include / "cccl", include, root, root / "libs", root / "src", root / "src/kernels"})
            options.push_back("-I" + p.string());
        std::vector<const char*> arguments;
        for (const auto& option : options) arguments.push_back(option.c_str());
        auto start = Clock::now();
        const auto status = nvrtcCompileProgram(program, int(arguments.size()), arguments.data());
        compileSeconds = std::chrono::duration<double>(Clock::now() - start).count();
        size_t length = 0;
        if (status != NVRTC_SUCCESS) {
            nvrtcGetProgramLogSize(program, &length);
            std::string log(length, '\0');
            nvrtcGetProgramLog(program, log.data());
            nvrtcDestroyProgram(&program);
            throw std::runtime_error("NVRTC compilation failed:\n" + log);
        }
        check(nvrtcGetPTXSize(program, &length));
        std::string ptx(length, '\0');
        check(nvrtcGetPTX(program, ptx.data()));
        check(nvrtcDestroyProgram(&program));
        check(cuModuleLoadData(&value, ptx.data()));
    }
    ~Module() { if (value) cuModuleUnload(value); }
};
Json render(const Scene& scene, const Options& options) {
    check(cudaSetDevice(options.device));
    cudaDeviceProp device;
    check(cudaGetDeviceProperties(&device, options.device));
    Module module(device);
    CUfunction functions[3];
    const char* names[] = {"kernel_stage1_drawSmallTriangles_indexbuffer_uncompressed",
        "kernel_stage2_drawMediumTriangles_indexbuffer_uncompressed", "kernel_stage3_drawHugeTriangles_indexbuffer_uncompressed"};
    int active = std::numeric_limits<int>::max();
    for (int i = 0; i < 3; ++i) {
        check(cuModuleGetFunction(&functions[i], module.value, names[i]));
        int blocks = 0;
        check(cuOccupancyMaxActiveBlocksPerMultiprocessor(&blocks, functions[i], 256, 0));
        active = std::min(active, blocks);
    }
    if (!device.cooperativeLaunch || active < 1) throw std::runtime_error("Cooperative launch unsupported");
    const int blocks = device.multiProcessorCount * std::min(active, 2);
    Allocations memory;
    std::vector<CMesh> meshes;
    std::vector<glm::mat4> transforms;
    glm::mat4 view(1.f);
    if (!scene.fixture) {
        const auto center = (scene.minimum + scene.maximum) * .5f;
        const auto extent = scene.maximum - scene.minimum;
        const float distance = std::max({extent.x, extent.y, extent.z, .01f}) * 1.5f;
        view = glm::lookAt(center + glm::vec3(0, 0, distance), center, glm::vec3(0, 1, 0));
    }
    uint64_t triangles = 0, vertices = 0;
    for (const auto& input : scene.meshes) {
        CMesh mesh{};
        static_assert(sizeof(glm::vec3) == sizeof(fastgltf::math::fvec3));
        mesh.positions = reinterpret_cast<glm::vec3*>(memory.allocate(input.positions.size(), input.positions.data()));
        mesh.indices = memory.allocate(input.indices.size(), input.indices.data());
        mesh.numTriangles = uint32_t(input.indices.size() / 3);
        mesh.numVertices = uint32_t(input.positions.size());
        mesh.cummulativeTriangleCount = triangles;
        mesh.world = input.world;
        mesh.instances.offset = int(meshes.size());
        mesh.instances.count = 1;
        mesh.isLoaded = true;
        meshes.push_back(mesh);
        transforms.push_back(view * input.world);
        triangles += mesh.numTriangles;
        vertices += mesh.numVertices;
    }
    if (triangles >= MAX_NONTRIVIAL_TRIANGLES || triangles >= (uint64_t(1) << 33))
        throw std::runtime_error("Input exceeds bounded headless queue capacity");
    const uint64_t frameTiles = uint64_t((options.resolution + TILE_SIZE - 1) / TILE_SIZE);
    if (triangles * frameTiles * frameTiles >= std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Worst-case tile count exceeds the kernel's uint32 counter");
    RasterArgs args{};
    args.meshes = memory.allocate(meshes.size(), meshes.data());
    args.instances = args.meshes;
    args.numMeshes = args.numInstances = uint32_t(meshes.size());
    args.transforms = memory.allocate(transforms.size(), transforms.data());
    args.numProcessedBatches = memory.allocate<uint32_t>(1);
    args.numProcessedBatches_nontrivial = memory.allocate<uint32_t>(1);
    args.hugeTriangles = memory.allocate<HugeTriangle>(MAX_HUGE_TRIANGLES);
    args.hugeTrianglesCounter = memory.allocate<uint32_t>(1);
    args.numProcessedHugeTriangles = memory.allocate<uint32_t>(1);
    args.nontrivialTrianglesCounter = memory.allocate<uint32_t>(1);
    args.nontrivialTrianglesList = memory.allocate<uint64_t>(MAX_NONTRIVIAL_TRIANGLES);
    args.target.width = args.target.height = options.resolution;
    args.target.view = view;
    args.target.proj = glm::perspective(glm::radians(45.f), 1.f, .01f, 100.f);
    const auto count = size_t(options.resolution) * options.resolution;
    args.target.framebuffer = memory.allocate<uint64_t>(count);
    args.state = memory.allocate<DeviceState>(1);
    std::vector<uint64_t> pixels(count, clearPixel);
    cudaEvent_t start, end;
    check(cudaEventCreate(&start)); check(cudaEventCreate(&end));
    std::vector<float> timings;
    void* parameters[] = {&args};
    for (int repeat = -2; repeat < options.repeat; ++repeat) {
        check(cudaMemcpy(args.target.framebuffer, pixels.data(), count * 8, cudaMemcpyHostToDevice));
        check(cudaEventRecord(start));
        for (auto function : functions)
            check(cuLaunchCooperativeKernel(function, blocks, 1, 1, 256, 1, 1, 0, nullptr, parameters));
        check(cudaEventRecord(end)); check(cudaEventSynchronize(end));
        float elapsed = 0;
        check(cudaEventElapsedTime(&elapsed, start, end));
        if (repeat >= 0) timings.push_back(elapsed);
    }
    check(cudaEventDestroy(start)); check(cudaEventDestroy(end));
    check(cudaMemcpy(pixels.data(), args.target.framebuffer, count * 8, cudaMemcpyDeviceToHost));
    const auto covered = std::count_if(pixels.begin(), pixels.end(), [](auto p) { return p != clearPixel; });
    if (!covered) throw std::runtime_error("Visibility correctness check failed: empty framebuffer");
    uint32_t medium = 0, huge = 0;
    check(cudaMemcpy(&medium, args.nontrivialTrianglesCounter, 4, cudaMemcpyDeviceToHost));
    check(cudaMemcpy(&huge, args.hugeTrianglesCounter, 4, cudaMemcpyDeviceToHost));
    if (medium > MAX_NONTRIVIAL_TRIANGLES || huge > MAX_HUGE_TRIANGLES)
        throw std::runtime_error("Visibility queue exhausted; incomplete output discarded");
    bool correct = true;
    if (scene.fixture) {
        correct = medium >= 2 && huge > 0;
        for (const auto& sample : std::array<std::array<int,3>,3>{{{{31,31,0}},{{70,70,1}},{{200,200,2}}}}) {
            const auto pixel = pixels[size_t(sample[1]) * options.resolution + sample[0]];
            correct &= pixel != clearPixel && (pixel & idMask) == uint64_t(sample[2]);
        }
        correct &= pixels[size_t(500) * options.resolution + 500] == clearPixel;
        if (!correct) throw std::runtime_error("Three-stage fixture visibility/ID check failed");
    }
    if (!options.dump.empty()) {
        std::ofstream file(options.dump, std::ios::binary);
        file.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(count * 8));
        if (!file) throw std::runtime_error("Could not write visibility dump");
    }
    if (!options.output.empty()) {
        std::ofstream file(options.output, std::ios::binary);
        file << "P6\n" << options.resolution << ' ' << options.resolution << "\n255\n";
        for (int y = options.resolution - 1; y >= 0; --y) for (int x = 0; x < options.resolution; ++x) {
            const auto pixel = pixels[size_t(y) * options.resolution + x];
            const uint32_t color = uint32_t((pixel & idMask) * 2654435761u);
            for (int channel = 0; channel < 3; ++channel)
                file.put(pixel == clearPixel ? 0 : char(64 + ((color >> (channel * 8)) & 191)));
        }
        if (!file) throw std::runtime_error("Could not write PPM");
    }
    std::sort(timings.begin(), timings.end());
    int driver = 0, runtime = 0, major = 0, minor = 0;
    check(cudaDriverGetVersion(&driver)); check(cudaRuntimeGetVersion(&runtime)); check(nvrtcVersion(&major, &minor));
    return Json{{"schema", "curast.headless.v1"}, {"status", "ok"}, {"gpu", device.name},
        {"build_revision", CURAST_BUILD_REV},
        {"compute_capability", std::to_string(device.major) + "." + std::to_string(device.minor)},
        {"driver_api", driver}, {"cuda_runtime", runtime}, {"nvrtc", std::to_string(major) + "." + std::to_string(minor)},
        {"input", options.selfTest ? "three-stage-synthetic" : options.input.filename().string()},
        {"primitives", meshes.size()}, {"vertices", vertices}, {"triangles", triangles}, {"resolution", options.resolution},
        {"repeat", options.repeat}, {"warmup", 2}, {"gpu_median_ms", timings[timings.size()/2]},
        {"gpu_min_ms", timings.front()}, {"gpu_max_ms", timings.back()}, {"nvrtc_compile_seconds", module.compileSeconds},
        {"covered_pixels", covered}, {"medium_queue", medium}, {"huge_tiles", huge},
        {"self_test_passed", scene.fixture ? Json(correct) : Json(nullptr)},
        {"timing_scope", "three visibility stages only; excludes clear, parse, uploads, compile and shading"},
        {"material_policy", "opaque backface-culled visibility; no textures, material doubleSided or alpha evaluation"}};
}
int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        if (options.help) {
            std::cout << "curast-headless (--input static.glb | --self-test) [--resolution 512] [--repeat 31]\n"
                         "  [--output visibility.ppm] [--dump packed-u64.bin] [--report result.json] [--device 0]\n"
                         "Separate flags and values with spaces. GPU architecture is selected at runtime.\n";
            return 0;
        }
        const auto scene = options.selfTest ? fixture() : loadScene(options.input);
        auto report = render(scene, options);
        std::cout << report.dump(2) << '\n';
        if (!options.report.empty()) {
            std::ofstream file(options.report);
            file << report.dump(2) << '\n';
            if (!file) throw std::runtime_error("Could not write report");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "curast-headless: " << error.what() << '\n';
        return 1;
    }
}
