#include <iostream>
#define _USE_MATH_DEFINES
#include <cmath>

#include "GamesEngineeringBase.h" // Include the GamesEngineeringBase header
#include <algorithm>
#include <chrono>

#include <cmath>
#include "matrix.h"
#include "colour.h"
#include "mesh.h"
#include "zbuffer.h"
#include "renderer.h"
#include "RNG.h"
#include "light.h"
#include "triangle.h"

// [Optimization 7] Thread Pool Support
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <queue>
#include <atomic>

// [Optimization] Thread Local ID for lock-free resource access
static thread_local int t_threadId = 0;

// A simple, persistent thread pool to avoid thread creation/destruction overhead
class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false) {
        for (size_t i = 0; i < threads; ++i) {
            // [Optimization] Capture loop index to assign simplified thread ID
            workers.emplace_back([this, i] {
                t_threadId = static_cast<int>(i);
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this] { return stop || !tasks.empty(); });
                        if (stop && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();

                    {
                        std::unique_lock<std::mutex> lock(wait_mutex);
                        active_tasks--;
                    }
                    wait_condition.notify_all();
                }
                });
        }
    }

    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        {
            std::unique_lock<std::mutex> lock(wait_mutex);
            active_tasks++;
        }
        condition.notify_one();
    }

    void wait_all() {
        std::unique_lock<std::mutex> lock(wait_mutex);
        wait_condition.wait(lock, [this] { return active_tasks == 0; });
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread& worker : workers)
            worker.join();
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;

    std::mutex wait_mutex;
    std::condition_variable wait_condition;
    int active_tasks = 0;
};

// Singleton ThreadPool provider
ThreadPool& getThreadPool() {
    static unsigned int tObj = std::thread::hardware_concurrency();
    // Use hardware concurrency, fallback to 12 if detection fails
    static ThreadPool pool(tObj > 0 ? tObj : 12);
    return pool;
}

// [Optimization] Helper for Serial Vertex Transformation
inline void processVerticesSerial(const size_t start, const size_t end,
    const std::vector<Vertex>& srcVertices,
    std::vector<Vertex>& dstVertices,
    size_t dstOffset, // [New] Offset in the global destination buffer
    const matrix& pMat, const matrix& worldMat,
    float halfWidth, float halfHeight, float height) {

    for (size_t i = start; i < end; ++i) {
        // Map local index 'i' to global buffer index
        size_t globalIdx = dstOffset + i;
        dstVertices[globalIdx] = srcVertices[i];

        // Local -> Screen
        dstVertices[globalIdx].p = pMat * srcVertices[i].p;
        dstVertices[globalIdx].p.divideW();

        // Normal World Transform
        dstVertices[globalIdx].normal = worldMat * srcVertices[i].normal;
        dstVertices[globalIdx].normal.normalise();

        // Viewport Mapping
        dstVertices[globalIdx].p[0] = (dstVertices[globalIdx].p[0] + 1.f) * halfWidth;
        dstVertices[globalIdx].p[1] = (dstVertices[globalIdx].p[1] + 1.f) * halfHeight;
        dstVertices[globalIdx].p[1] = height - dstVertices[globalIdx].p[1];
    }
}

// Legacy helper for Single Threaded path
inline void rasterizeChunkSerial(Renderer& renderer, const Mesh* mesh,
    const std::vector<Vertex>& tVertices,
    size_t vertexOffset, // Offset into tVertices
    Light& L, int clipMinY, int clipMaxY) {

    float fClipMinY = static_cast<float>(clipMinY);
    float fClipMaxY = static_cast<float>(clipMaxY);

    for (const triIndices& ind : mesh->triangles) {
        const Vertex* v0 = &tVertices[vertexOffset + ind.v[0]];
        const Vertex* v1 = &tVertices[vertexOffset + ind.v[1]];
        const Vertex* v2 = &tVertices[vertexOffset + ind.v[2]];

        if (fabs(v0->p[2]) > 1.0f || fabs(v1->p[2]) > 1.0f || fabs(v2->p[2]) > 1.0f) continue;

        vec4 e1 = v1->p - v0->p;
        vec4 e2 = v2->p - v0->p;
        if ((e1[0] * e2[1] - e1[1] * e2[0]) < 0.f) continue;

        triangle tri(*v0, *v1, *v2);
        tri.draw(renderer, L, mesh->ka, mesh->kd, clipMinY, clipMaxY);
    }
}

// [Optimization] New Structure for Pre-Culled Triangles
// Compact (20 bytes) and linear memory friendly
struct TriRef {
    uint32_t v0_idx;
    uint32_t v1_idx;
    uint32_t v2_idx;
    float minY;
    float maxY;
    // We also need material properties since we mixed all meshes
    float ka;
    float kd;
};

// [Optimization] Global Render Context to avoid allocation churn
// Reuses memory for vertices and triangle bins across frames
struct RenderContext {
    std::vector<Vertex> tVertices;
    // [cullThreadId][TaskIndex] -> List of Triangles
    std::vector<std::vector<std::vector<TriRef>>> threadedBins;

    void reset(size_t numVerts, size_t numThreads, size_t numTasks) {
        // Resize vertex buffer to hold the WHOLE scene
        if (tVertices.size() < numVerts) {
            tVertices.resize(numVerts);
        }

        if (threadedBins.size() != numThreads) {
            threadedBins.resize(numThreads);
        }

        // Clear bins and ensure enough bins for all tasks
        for (auto& tBins : threadedBins) {
            if (tBins.size() != numTasks) tBins.resize(numTasks);
            for (auto& bin : tBins) {
                bin.clear();
            }
        }
    }
};

static RenderContext g_renderCtx;

// [Optimization] Binned Culling Logic
// Sorts triangles directly into the Rasterizer thread buckets during the cull pass
inline void cullTrianglesBinned(const size_t start, const size_t end,
    const Mesh* mesh, const std::vector<Vertex>& tVertices,
    std::vector<std::vector<TriRef>>& outBins,
    size_t vertexGlobalOffset, // Offset to apply to triangle indices
    int linesPerTask, int numTasks, int canvasHeight) {

    float fCanvasHeight = static_cast<float>(canvasHeight);
    float fLinesPerTask = static_cast<float>(linesPerTask);

    for (size_t i = start; i < end; ++i) {
        const triIndices& ind = mesh->triangles[i];

        // Access via GLOBAL indices
        uint32_t g_v0 = static_cast<uint32_t>(vertexGlobalOffset + ind.v[0]);
        uint32_t g_v1 = static_cast<uint32_t>(vertexGlobalOffset + ind.v[1]);
        uint32_t g_v2 = static_cast<uint32_t>(vertexGlobalOffset + ind.v[2]);

        const Vertex* v0 = &tVertices[g_v0];
        const Vertex* v1 = &tVertices[g_v1];
        const Vertex* v2 = &tVertices[g_v2];

        // 1. Z-Clipping 
        if (fabs(v0->p[2]) > 1.0f || fabs(v1->p[2]) > 1.0f || fabs(v2->p[2]) > 1.0f) continue;

        // 2. Backface Culling
        vec4 e1 = v1->p - v0->p;
        vec4 e2 = v2->p - v0->p;
        if ((e1[0] * e2[1] - e1[1] * e2[0]) < 0.f) continue;

        // 3. Y-Bounds & Binning
        float minY = v0->p[1];
        float maxY = minY;

        float y1 = v1->p[1];
        if (y1 < minY) minY = y1;
        else if (y1 > maxY) maxY = y1;

        float y2 = v2->p[1];
        if (y2 < minY) minY = y2;
        else if (y2 > maxY) maxY = y2;

        if (minY >= fCanvasHeight || maxY < 0.0f) continue;

        float cMinY = std::max(0.0f, minY);
        float cMaxY = std::min(fCanvasHeight - 0.01f, maxY);

        int bStart = static_cast<int>(cMinY / fLinesPerTask);
        int bEnd = static_cast<int>(cMaxY / fLinesPerTask);

        bStart = std::max(0, std::min(bStart, numTasks - 1));
        bEnd = std::max(0, std::min(bEnd, numTasks - 1));

        // Store with GLOBAL indices and Mesh properties
        TriRef tRef{ g_v0, g_v1, g_v2, minY, maxY, mesh->ka, mesh->kd };

        for (int b = bStart; b <= bEnd; ++b) {
            outBins[b].push_back(tRef);
        }
    }
}

// [Optimization] Frame-Level Merging Render Pipeline
// Accepts the entire scene vector to minimize sync overhead
void renderScene(Renderer& renderer, const std::vector<Mesh*>& meshes, matrix& camera, Light& L) {
    if (meshes.empty()) return;

    // 1. Pre-pass: Calculate total resources needed
    size_t totalVertices = 0;
    size_t totalTriangles = 0;
    std::vector<size_t> meshVertOffsets;
    meshVertOffsets.reserve(meshes.size());

    for (const auto* m : meshes) {
        meshVertOffsets.push_back(totalVertices);
        totalVertices += m->vertices.size();
        totalTriangles += m->triangles.size();
    }

    float halfWidth = static_cast<float>(renderer.canvas.getWidth()) * 0.5f;
    float halfHeight = static_cast<float>(renderer.canvas.getHeight()) * 0.5f;
    float height = static_cast<float>(renderer.canvas.getHeight());
    int iCanvasHeight = renderer.canvas.getHeight();

    // Setup threading info
    unsigned int numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 12;

    // Define Task Granularity
    const int TASK_SIZE = 32;
    int numTasks = (iCanvasHeight + TASK_SIZE - 1) / TASK_SIZE;

    // Reset Context once per frame
    g_renderCtx.reset(totalVertices, numThreads, numTasks);
    // Reference to the persistent global vertex buffer
    std::vector<Vertex>& tVertices = g_renderCtx.tVertices;


    // Threshold check (Global scene complexity)
    bool useThreading = totalTriangles > 0;

    if (!useThreading) {
        // --- Serial Fallback ---
        matrix pMat = renderer.perspective * camera; // Partial vp matrix
        for (size_t i = 0; i < meshes.size(); ++i) {
            Mesh* m = meshes[i];
            matrix p = pMat * m->world; // Full MVP
            // Process to global buffer
            processVerticesSerial(0, m->vertices.size(), m->vertices, tVertices, meshVertOffsets[i], p, m->world, halfWidth, halfHeight, height);
            // Rasterize from global buffer
            rasterizeChunkSerial(renderer, m, tVertices, meshVertOffsets[i], L, 0, 2147483647);
        }
    }
    else {
        // --- Multithreaded Frame-Level Pipeline ---
        ThreadPool& pool = getThreadPool();

        matrix pMat = renderer.perspective * camera;

        // Phase 1: Global Vertex Transformation
        // Enqueue tasks for all meshes
        for (size_t i = 0; i < meshes.size(); ++i) {
            Mesh* m = meshes[i];
            size_t vCount = m->vertices.size();
            size_t vOffset = meshVertOffsets[i];

            // Heuristic: Split large meshes, keep small ones whole
            size_t batchSize = 10000;
            size_t numBatches = (vCount + batchSize - 1) / batchSize;

            for (size_t b = 0; b < numBatches; ++b) {
                size_t start = b * batchSize;
                size_t end = std::min(start + batchSize, vCount);

                // Copy necessary data to lambda (matrix by value)
                matrix p = pMat * m->world;
                matrix world = m->world;

                pool.enqueue([start, end, m, &tVertices, vOffset, p, world, height, halfWidth, halfHeight] {
                    // Write to global buffer at vOffset
                    processVerticesSerial(start, end, m->vertices, tVertices, vOffset, p, world, halfWidth, halfHeight, height);
                    });
            }
        }
        pool.wait_all(); // Barrier 1: Vertices Ready

        // Phase 2: Global Binning / Culling
        // Enqueue tasks for all meshes
        for (size_t i = 0; i < meshes.size(); ++i) {
            Mesh* m = meshes[i];
            size_t tCount = m->triangles.size();
            size_t vOffset = meshVertOffsets[i];

            size_t batchSize = 5000;
            size_t numBatches = (tCount + batchSize - 1) / batchSize;

            for (size_t b = 0; b < numBatches; ++b) {
                size_t start = b * batchSize;
                size_t end = std::min(start + batchSize, tCount);

                pool.enqueue([start, end, m, &tVertices, vOffset, TASK_SIZE, numTasks, iCanvasHeight] {
                    // Implicitly uses t_threadId to select the write buffer
                    cullTrianglesBinned(start, end, m, tVertices, g_renderCtx.threadedBins[t_threadId], vOffset, TASK_SIZE, numTasks, iCanvasHeight);
                    });
            }
        }
        pool.wait_all(); // Barrier 2: Bins Ready

        // Phase 3: Dynamic Rasterization (Task Stealing)
        std::atomic<int> nextTaskIdx(0);

        auto rasterWorker = [&renderer, &tVertices, &L, numThreads, numTasks, &nextTaskIdx, TASK_SIZE, iCanvasHeight]() {
            while (true) {
                int myTaskIdx = nextTaskIdx.fetch_add(1);
                if (myTaskIdx >= numTasks) break;

                int startY = myTaskIdx * TASK_SIZE;
                int endY = std::min(startY + TASK_SIZE, iCanvasHeight);

                // Collect from ALL threads for this task ID
                for (unsigned int c = 0; c < numThreads; ++c) {
                    const auto& tris = g_renderCtx.threadedBins[c][myTaskIdx];

                    for (const TriRef& tRef : tris) {
                        // All info is now in tRef (Global Indices + Material)
                        triangle tri(tVertices[tRef.v0_idx], tVertices[tRef.v1_idx], tVertices[tRef.v2_idx]);

                        // Use material from TriRef
                        tri.draw(renderer, L, tRef.ka, tRef.kd, startY, endY);
                    }
                }
            }
            };

        for (unsigned int t = 0; t < numThreads; ++t) {
            pool.enqueue(rasterWorker);
        }
        pool.wait_all(); // Barrier 3: Frame Done
    }

    if (useThreading) {
        static bool printed = false;
        if (!printed) {
            std::cout << "Threading enabled with " << numThreads << " threads" << std::endl;
            std::cout << "Total triangles: " << totalTriangles << std::endl;
            printed = true;
        }
    }
}

// --- Scene Functions ---

void sceneTest() {
    Renderer renderer;
    Light L{ vec4(0.f, 1.f, 1.f, 0.f), colour(1.0f, 1.0f, 1.0f), colour(0.2f, 0.2f, 0.2f) };
    matrix camera = matrix::makeIdentity();

    bool running = true;
    std::vector<Mesh*> scene;

    Mesh mesh = Mesh::makeSphere(1.0f, 10, 20);
    scene.push_back(&mesh);

    float x = 0.0f, y = 0.0f, z = -4.0f;
    mesh.world = matrix::makeTranslation(x, y, z);

    while (running) {
        renderer.canvas.checkInput();
        renderer.clear();

        mesh.world = matrix::makeTranslation(x, y, z);

        if (renderer.canvas.keyPressed(VK_ESCAPE)) break;
        if (renderer.canvas.keyPressed('A')) x += -0.1f;
        if (renderer.canvas.keyPressed('D')) x += 0.1f;
        if (renderer.canvas.keyPressed('W')) y += 0.1f;
        if (renderer.canvas.keyPressed('S')) y += -0.1f;
        if (renderer.canvas.keyPressed('Q')) z += 0.1f;
        if (renderer.canvas.keyPressed('E')) z += -0.1f;

        // Render entire scene at once
        renderScene(renderer, scene, camera, L);

        renderer.present();
    }
}

matrix makeRandomRotation() {
    RandomNumberGenerator& rng = RandomNumberGenerator::getInstance();
    unsigned int r = rng.getRandomInt(0, 3);
    switch (r) {
    case 0: return matrix::makeRotateX(rng.getRandomFloat(0.f, 2.0f * M_PI));
    case 1: return matrix::makeRotateY(rng.getRandomFloat(0.f, 2.0f * M_PI));
    case 2: return matrix::makeRotateZ(rng.getRandomFloat(0.f, 2.0f * M_PI));
    default: return matrix::makeIdentity();
    }
}

void scene1() {
    Renderer renderer;
    matrix camera;
    Light L{ vec4(0.f, 1.f, 1.f, 0.f), colour(1.0f, 1.0f, 1.0f), colour(0.2f, 0.2f, 0.2f) };

    bool running = true;
    std::vector<Mesh*> scene;

    for (unsigned int i = 0; i < 20; i++) {
        Mesh* m = new Mesh();
        *m = Mesh::makeCube(1.f);
        m->world = matrix::makeTranslation(-2.0f, 0.0f, (-3 * static_cast<float>(i))) * makeRandomRotation();
        scene.push_back(m);
        m = new Mesh();
        *m = Mesh::makeCube(1.f);
        m->world = matrix::makeTranslation(2.0f, 0.0f, (-3 * static_cast<float>(i))) * makeRandomRotation();
        scene.push_back(m);
    }

    float zoffset = 8.0f;
    float step = -0.1f;

    auto start = std::chrono::high_resolution_clock::now();
    std::chrono::time_point<std::chrono::high_resolution_clock> end;
    int cycle = 0;

    while (running) {
        renderer.canvas.checkInput();
        renderer.clear();

        camera = matrix::makeTranslation(0, 0, -zoffset);

        scene[0]->world = scene[0]->world * matrix::makeRotateXYZ(0.1f, 0.1f, 0.0f);
        scene[1]->world = scene[1]->world * matrix::makeRotateXYZ(0.0f, 0.1f, 0.2f);

        if (renderer.canvas.keyPressed(VK_ESCAPE)) break;

        zoffset += step;
        if (zoffset < -60.f || zoffset > 8.f) {
            step *= -1.f;
            if (++cycle % 2 == 0) {
                end = std::chrono::high_resolution_clock::now();
                std::cout << cycle / 2 << " :" << std::chrono::duration<double, std::milli>(end - start).count() << "ms\n";
                start = std::chrono::high_resolution_clock::now();
            }
        }

        renderScene(renderer, scene, camera, L);
        renderer.present();
    }

    for (auto& m : scene) delete m;
}

void scene2() {
    Renderer renderer;
    matrix camera = matrix::makeIdentity();
    Light L{ vec4(0.f, 1.f, 1.f, 0.f), colour(1.0f, 1.0f, 1.0f), colour(0.2f, 0.2f, 0.2f) };

    std::vector<Mesh*> scene;
    struct rRot { float x; float y; float z; };
    std::vector<rRot> rotations;

    RandomNumberGenerator& rng = RandomNumberGenerator::getInstance();

    for (unsigned int y = 0; y < 6; y++) {
        for (unsigned int x = 0; x < 8; x++) {
            Mesh* m = new Mesh();
            *m = Mesh::makeCube(1.f);
            scene.push_back(m);
            m->world = matrix::makeTranslation(-7.0f + (static_cast<float>(x) * 2.f), 5.0f - (static_cast<float>(y) * 2.f), -8.f);
            rRot r{ rng.getRandomFloat(-.1f, .1f), rng.getRandomFloat(-.1f, .1f), rng.getRandomFloat(-.1f, .1f) };
            rotations.push_back(r);
        }
    }

    Mesh* sphere = new Mesh();
    *sphere = Mesh::makeSphere(1.0f, 10, 20);
    scene.push_back(sphere);
    float sphereOffset = -6.f;
    float sphereStep = 0.1f;
    sphere->world = matrix::makeTranslation(sphereOffset, 0.f, -6.f);

    auto start = std::chrono::high_resolution_clock::now();
    std::chrono::time_point<std::chrono::high_resolution_clock> end;
    int cycle = 0;

    bool running = true;
    while (running) {
        renderer.canvas.checkInput();
        renderer.clear();

        for (unsigned int i = 0; i < rotations.size(); i++)
            scene[i]->world = scene[i]->world * matrix::makeRotateXYZ(rotations[i].x, rotations[i].y, rotations[i].z);

        sphereOffset += sphereStep;
        sphere->world = matrix::makeTranslation(sphereOffset, 0.f, -6.f);
        if (sphereOffset > 6.0f || sphereOffset < -6.0f) {
            sphereStep *= -1.f;
            if (++cycle % 2 == 0) {
                end = std::chrono::high_resolution_clock::now();
                std::cout << cycle / 2 << " :" << std::chrono::duration<double, std::milli>(end - start).count() << "ms\n";
                start = std::chrono::high_resolution_clock::now();
            }
        }

        if (renderer.canvas.keyPressed(VK_ESCAPE)) break;

        renderScene(renderer, scene, camera, L);
        renderer.present();
    }

    for (auto& m : scene) delete m;
}

void scene3() {
    Renderer renderer;
    matrix camera = matrix::makeIdentity();
    Light L{ vec4(0.f, 1.f, 1.f, 0.f), colour(1.0f, 1.0f, 1.0f), colour(0.2f, 0.2f, 0.2f) };

    std::vector<Mesh*> scene;
    RandomNumberGenerator& rng = RandomNumberGenerator::getInstance();

    // 8x8 grid = 64 spheres
    // every sphere: 50x100 = 5000 quads = 10000 triangles
    // total: 640,000 triangles
    const int gridSize = 8;
    const int latDivisions = 50;
    const int lonDivisions = 100;

    struct SphereData {
        float rotSpeedX, rotSpeedY, rotSpeedZ;
        float baseX, baseY;
        float phase;
    };
    std::vector<SphereData> sphereData;

    for (int row = 0; row < gridSize; ++row) {
        for (int col = 0; col < gridSize; ++col) {
            Mesh* sphere = new Mesh();
            *sphere = Mesh::makeSphere(0.6f, latDivisions, lonDivisions);
            float x = -7.0f + col * 2.0f;
            float y = 7.0f - row * 2.0f;
            sphere->world = matrix::makeTranslation(x, y, -10.0f);
            scene.push_back(sphere);

            SphereData data;
            data.rotSpeedX = rng.getRandomFloat(-0.1f, 0.1f);
            data.rotSpeedY = rng.getRandomFloat(-0.1f, 0.1f);
            data.rotSpeedZ = rng.getRandomFloat(-0.1f, 0.1f);
            data.baseX = x;
            data.baseY = y;
            data.phase = rng.getRandomFloat(0.f, 2.0f * static_cast<float>(M_PI));
            sphereData.push_back(data);
        }
    }
	//Add multiple layers of overlapping large cubes to create extreme overdraw
    for (int layer = 0; layer < 5; ++layer) {
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                Mesh* cube = new Mesh();
                *cube = Mesh::makeCube(1.8f);
                float x = -3.0f + col * 2.0f;
                float y = 3.0f - row * 2.0f;
                float z = -8.0f - layer * 1.5f;
                cube->world = matrix::makeTranslation(x, y, z) * makeRandomRotation();
                scene.push_back(cube);
            }
        }
    }
    size_t numCubes = 5 * 4 * 4;

    float time = 0.0f;
    float zOffset = 0.0f;
    float zStep = 0.03f;

    auto start = std::chrono::high_resolution_clock::now();
    std::chrono::time_point<std::chrono::high_resolution_clock> end;
    int cycle = 0;

    bool running = true;
    while (running) {
        renderer.canvas.checkInput();
        renderer.clear();

        time += 0.016f;

        for (size_t i = 0; i < gridSize * gridSize; ++i) {
            float x = sphereData[i].baseX;
            float waveY = sphereData[i].baseY + std::sin(time * 2.0f + sphereData[i].phase) * 0.5f;
            float z = -10.0f + zOffset;

            scene[i]->world = matrix::makeTranslation(x, waveY, z)
                * matrix::makeRotateX(time * sphereData[i].rotSpeedX * 15.f)
                * matrix::makeRotateY(time * sphereData[i].rotSpeedY * 15.f)
                * matrix::makeRotateZ(time * sphereData[i].rotSpeedZ * 15.f);
        }

        size_t cubeStartIdx = gridSize * gridSize;
        for (size_t i = 0; i < numCubes; ++i) {
            scene[cubeStartIdx + i]->world = scene[cubeStartIdx + i]->world
                * matrix::makeRotateXYZ(0.015f, 0.02f, 0.01f);
        }

        if (renderer.canvas.keyPressed(VK_ESCAPE)) break;

        zOffset += zStep;
        if (zOffset > 6.0f || zOffset < -6.0f) {
            zStep *= -1.f;
            if (++cycle % 2 == 0) {
                end = std::chrono::high_resolution_clock::now();
                std::cout << cycle / 2 << " :" << std::chrono::duration<double, std::milli>(end - start).count() << "ms\n";
                start = std::chrono::high_resolution_clock::now();
            }
        }

        renderScene(renderer, scene, camera, L);
        renderer.present();
    }

    for (auto& m : scene) delete m;
}

int main() {
    //scene1();
    //scene2();
    scene3();
    return 0;
}