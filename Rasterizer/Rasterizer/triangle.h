#pragma once

#include "mesh.h"
#include "colour.h"
#include "renderer.h"
#include "light.h"
#include <iostream>
#include <algorithm>
#include <cmath>

// Simple support class for a 2D vector
class vec2D {
public:
    float x, y;

    // Default constructor initializes both components to 0
    vec2D() { x = y = 0.f; };

    // Constructor initializes components with given values
    vec2D(float _x, float _y) : x(_x), y(_y) {}

    // Constructor initializes components from a vec4
    vec2D(vec4 v) {
        x = v[0];
        y = v[1];
    }

    // Display the vector components
    void display() { std::cout << x << '\t' << y << std::endl; }

    // Overloaded subtraction operator for vector subtraction
    vec2D operator- (vec2D& v) {
        vec2D q;
        q.x = x - v.x;
        q.y = y - v.y;
        return q;
    }
};

// Class representing a triangle for rendering purposes
class triangle {
    Vertex v[3];       // Vertices of the triangle
    float area;        // Area of the triangle
    colour col[3];     // Colors for each vertex of the triangle

public:
    // Constructor initializes the triangle with three vertices
    // Input Variables:
    // - v1, v2, v3: Vertices defining the triangle
    triangle(const Vertex& v1, const Vertex& v2, const Vertex& v3) {
        v[0] = v1;
        v[1] = v2;
        v[2] = v3;

        // Calculate the 2D area of the triangle
        vec2D e1 = vec2D(v[1].p - v[0].p);
        vec2D e2 = vec2D(v[2].p - v[0].p);
        area = std::fabs(e1.x * e2.y - e1.y * e2.x);
    }

    // [Optimization 1] 使用 inline 减少调用开销，使用 const& 避免对象拷贝
    inline float getC(const vec2D& v1, const vec2D& v2, const vec2D& p) {
        // 展开计算，避免构造临时的 vec2D e 和 q 对象
        // 原逻辑: (p.y - v1.y) * (v2.x - v1.x) - (p.x - v1.x) * (v2.y - v1.y);
        return (p.y - v1.y) * (v2.x - v1.x) - (p.x - v1.x) * (v2.y - v1.y);
    }

   
    // Template function to interpolate values using barycentric coordinates
    // Input Variables:
    // - alpha, beta, gamma: Barycentric coordinates
    // - a1, a2, a3: Values to interpolate
    // Returns the interpolated value
    template <typename T>
    T interpolate(float alpha, float beta, float gamma, T a1, T a2, T a3) {
        return (a1 * alpha) + (a2 * beta) + (a3 * gamma);
    }

   
    // [Optimization 6]基于[Optimization 2]和[Optimization 5]更改 增量式重心坐标计算
    // [Optimization 7] Multi-threading support: added clipMinY and clipMaxY
    void draw(Renderer& renderer, Light& L, float ka, float kd, int clipMinY = 0, int clipMaxY = 2147483647) {
        vec2D minV, maxV;

        // 获取屏幕空间的包围盒
        getBoundsWindow(renderer.canvas, minV, maxV);

        // [Multi-threading] Apply vertical clipping for threaded bands
        // Only render lines startY to endY within this thread's band
        int startY = std::max((int)minV.y, clipMinY);
        int endY = std::min((int)ceil(maxV.y), clipMaxY);

        // If the triangle is completely outside the band, skip
        if (startY >= endY) return;

        // 面积太小不渲染
        if (area < 1.f) return;

        // 预计算：将除法转换为乘法
        float invArea = 1.0f / area;

        // 预提取顶点坐标到局部变量
        vec2D p0(v[0].p), p1(v[1].p), p2(v[2].p);

        // 光源方向归一化提前到循环外
        vec4 lightDir = L.omega_i;
        lightDir.normalise();

        // [Optimization 6] 预计算边缘函数的增量
        float dAlpha_dx = -(p2.y - p1.y) * invArea;
        float dAlpha_dy = (p2.x - p1.x) * invArea;
        
        float dBeta_dx = -(p0.y - p2.y) * invArea;
        float dBeta_dy = (p0.x - p2.x) * invArea;
        
        float dGamma_dx = -(p1.y - p0.y) * invArea;
        float dGamma_dy = (p1.x - p0.x) * invArea;

        // 计算起始点 (minV.x, minV.y) 的初始值
        // Note: minV.y is the original top, but we start loop at startY (clipped)
        int startX = (int)(minV.x);
        int originalStartY = (int)(minV.y);
        vec2D startP((float)startX, (float)originalStartY);
        
        float rowAlpha = getC(p1, p2, startP) * invArea;
        float rowBeta = getC(p2, p0, startP) * invArea;
        float rowGamma = getC(p0, p1, startP) * invArea;

        // [Multi-threading] Advance barycentric rows to the clipped start Y
        int dy = startY - originalStartY;
        if (dy > 0) {
            rowAlpha += dAlpha_dy * (float)dy;
            rowBeta  += dBeta_dy  * (float)dy;
            rowGamma += dGamma_dy * (float)dy;
        }

        // 遍历包围盒 (clipped range)
        for (int y = startY; y < endY; y++) {
            float alpha = rowAlpha;
            float beta = rowBeta;
            float gamma = rowGamma;

            for (int x = startX; x < (int)ceil(maxV.x); x++) {
                // 检查是否在三角形内
                if (alpha >= 0.f && beta >= 0.f && gamma >= 0.f) {
                    // 像素在三角形内
                    float depth = interpolate(beta, gamma, alpha, v[0].p[2], v[1].p[2], v[2].p[2]);

                    if (depth > 0.001f && renderer.zbuffer(x, y) > depth) {
                        renderer.zbuffer(x, y) = depth;

                        vec4 normal = interpolate(beta, gamma, alpha, v[0].normal, v[1].normal, v[2].normal);
                        normal.normalise();

                        float dot = std::max(vec4::dot(lightDir, normal), 0.0f);

                        colour c = interpolate(beta, gamma, alpha, v[0].rgb, v[1].rgb, v[2].rgb);

                        colour a = (c * kd) * (L.L * dot) + (L.ambient * ka);

                        unsigned char r, g, b;
                        a.toRGB(r, g, b);
                        renderer.canvas.draw(x, y, r, g, b);
                    }
                }

                // 水平方向增量更新
                alpha += dAlpha_dx;
                beta += dBeta_dx;
                gamma += dGamma_dx;
            }

            // 垂直方向增量更新 (移到下一行)
            rowAlpha += dAlpha_dy;
            rowBeta += dBeta_dy;
            rowGamma += dGamma_dy;
        }
    }


    // Compute the 2D bounds of the triangle
    // Output Variables:
    // - minV, maxV: Minimum and maximum bounds in 2D space
    void getBounds(vec2D& minV, vec2D& maxV) {
        minV = vec2D(v[0].p);
        maxV = vec2D(v[0].p);
        for (unsigned int i = 1; i < 3; i++) {
            minV.x = std::min(minV.x, v[i].p[0]);
            minV.y = std::min(minV.y, v[i].p[1]);
            maxV.x = std::max(maxV.x, v[i].p[0]);
            maxV.y = std::max(maxV.y, v[i].p[1]);
        }
    }

    // Compute the 2D bounds of the triangle, clipped to the canvas
    // Input Variables:
    // - canvas: Reference to the rendering canvas
    // Output Variables:
    // - minV, maxV: Clipped minimum and maximum bounds
    void getBoundsWindow(GamesEngineeringBase::Window& canvas, vec2D& minV, vec2D& maxV) {
        getBounds(minV, maxV);
        minV.x = std::max(minV.x, static_cast<float>(0));
        minV.y = std::max(minV.y, static_cast<float>(0));
        maxV.x = std::min(maxV.x, static_cast<float>(canvas.getWidth()));
        maxV.y = std::min(maxV.y, static_cast<float>(canvas.getHeight()));
    }

    // Debugging utility to display the triangle bounds on the canvas
    // Input Variables:
    // - canvas: Reference to the rendering canvas
    void drawBounds(GamesEngineeringBase::Window& canvas) {
        vec2D minV, maxV;
        getBounds(minV, maxV);

        for (int y = (int)minV.y; y < (int)maxV.y; y++) {
            for (int x = (int)minV.x; x < (int)maxV.x; x++) {
                canvas.draw(x, y, 255, 0, 0);
            }
        }
    }

    // Debugging utility to display the coordinates of the triangle vertices
    void display() {
        for (unsigned int i = 0; i < 3; i++) {
            v[i].p.display();
        }
        std::cout << std::endl;
    }
};
