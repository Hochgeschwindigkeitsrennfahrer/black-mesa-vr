#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct VrHandMatrix4
{
    // Column-major matrix. Element(row, column) = m[column * 4 + row].
    std::array<float, 16> m{};
};

struct VrHandMatrixRows3x4
{
    // Three affine rows consumed by the D3D9 vertex shader.
    std::array<float, 12> v{};
};

struct VrHandFingerCurlOverride
{
    bool enabled = false;
    std::array<float, 5> minCurl{};
    std::array<float, 5> maxCurl{};
};

struct VrHandVertex
{
    float position[3]{};
    float normal[3]{};
    float uv[2]{};
    float weights[4]{};
    std::uint8_t joints[4]{};
};

struct VrHandMeshAsset
{
    std::vector<VrHandVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<std::string> jointNames;
    std::vector<int> jointParents;
    std::vector<VrHandMatrix4> bindMatrices;
    std::vector<VrHandMatrix4> inverseBindMatrices;
    // BMVR (Black Mesa hands only): each joint's source bone frame in glove
    // space, unrolled (bindMatrices rebase the finger frames for the curl
    // hinges). Joint-local translations are in glove units. Empty otherwise.
    std::vector<VrHandMatrix4> sourceBoneBind;
    std::vector<std::uint8_t> baseColorTextureBytes;
    std::uint32_t fallbackColorArgb = 0xFFFFFFFFu;
    std::string sourcePath;

    bool IsValid() const
    {
        return !vertices.empty() &&
            !indices.empty() &&
            !jointNames.empty() &&
            jointParents.size() == jointNames.size() &&
            bindMatrices.size() == jointNames.size() &&
            inverseBindMatrices.size() == jointNames.size();
    }
};
